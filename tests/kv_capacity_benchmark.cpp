// Storage-only benchmark. Run through tests/run_kv_capacity.py, which builds
// the fault-enabled test binary and supplies a fresh directory per workload.
#include "../src/storage/kv_store.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace std::chrono;
using Clock = steady_clock;
using Time = Clock::time_point;
using fortis::storage::KvMutation;
using fortis::storage::KvStore;
constexpr size_t kKeys = 4096;
constexpr size_t kValueSize = 256;
constexpr size_t kMaxBatch = 64;
using Key = std::array<char, 24>;
using Value = std::array<char, kValueSize>;

struct Settings {
  size_t reads, writes, seconds, workers, queue_capacity, batch_size, hot_percent;
  bool compact;
  bool group_commit;
  bool parallel_point_reads;
  bool commit_slots;
};

fortis::storage::KvDurability Durability(bool commit_slots) {
  return commit_slots ? fortis::storage::KvDurability::CommitSlots : fortis::storage::KvDurability::RollbackMarker;
}

struct Job {
  uint64_t sequence;
  Time arrival;
  bool write;
};

struct Samples {
  std::vector<uint64_t> reads, writes, read_queue, write_queue;
  size_t rejected_reads = 0;
  size_t rejected_writes = 0;
  size_t errors = 0;
};

uint64_t Us(Clock::duration duration) {
  return static_cast<uint64_t>(duration_cast<microseconds>(duration).count());
}

Key MakeKey(size_t index) {
  Key key{};
  std::snprintf(key.data(), key.size(), "bench/%06zu", index);
  return key;
}

Value MakeValue(uint64_t sequence) {
  Value value;
  value.fill(static_cast<char>(sequence));
  return value;
}

size_t Hash(uint64_t index) {
  // Fixed workload distribution. Mix the bits before choosing hot keys.
  index = (index ^ (index >> 30)) * 0xbf58476d1ce4e5b9ULL;
  index = (index ^ (index >> 27)) * 0x94d049bb133111ebULL;
  return static_cast<size_t>(index ^ (index >> 31));
}

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void Seed(KvStore& store) {
  auto value = MakeValue(0);
  // Overwrite the seed keys so the concurrent compactor has dead records.
  for (size_t pass = 0; pass < 2; ++pass) {
    for (size_t base = 0; base < kKeys; base += kMaxBatch) {
      std::array<Key, kMaxBatch> keys;
      std::array<KvMutation, kMaxBatch> mutations;
      for (size_t i = 0; i < mutations.size(); ++i) {
        keys[i] = MakeKey(base + i);
        mutations[i] = {keys[i].data(), value.data(), kValueSize, 0, false};
      }
      Require(store.Write(mutations.data(), mutations.size()), "seed write failed");
    }
  }
}

class Workload {
 public:
  Workload(KvStore& store, const Settings& settings) : store_(store), settings_(settings) {}

  Samples Run(uint64_t* duration_us, size_t* peak_queue) {
    std::vector<Samples> worker_stats(settings_.workers);
    std::vector<std::thread> workers;
    workers.reserve(worker_stats.size());
    for (auto& stats : worker_stats) workers.emplace_back(&Workload::Worker, this, &stats);

    const Time start = Clock::now();
    Samples totals;
    const size_t seconds = std::max(settings_.seconds, size_t{1});
    const size_t jobs_per_second = settings_.reads + settings_.writes;
    const size_t total_jobs = jobs_per_second * seconds;
    size_t next = 0;
    Time last_due = start;
    for (size_t tick = 0; next < total_jobs; ++tick) {
      const size_t due = settings_.seconds ? std::min(total_jobs, jobs_per_second * (tick + 1) / 1000)
                                           : total_jobs;
      const Time wake_at = start + milliseconds(tick + 1);
      if (settings_.seconds) std::this_thread::sleep_until(wake_at);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (; next < due; ++next) {
          const auto scheduled = settings_.seconds
              ? start + microseconds(next * 1000000ULL / jobs_per_second) : start;
          // Space each operation type uniformly through the arrival stream.
          const bool write = (next + 1) * settings_.writes / jobs_per_second !=
                             next * settings_.writes / jobs_per_second;
          if (reads_.size() + writes_.size() == settings_.queue_capacity) {
            if (write) ++totals.rejected_writes;
            else ++totals.rejected_reads;
            continue;
          }
          (write ? writes_ : reads_).push_back({next, scheduled, write});
          *peak_queue = std::max(*peak_queue, reads_.size() + writes_.size());
          last_due = scheduled;
        }
      }
      ready_.notify_all();
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      arrivals_done_ = true;
    }
    ready_.notify_all();
    for (auto& worker : workers) worker.join();
    *duration_us = Us(std::max(Clock::now(), last_due) - start);
    for (const Samples& worker : worker_stats) {
      totals.reads.insert(totals.reads.end(), worker.reads.begin(), worker.reads.end());
      totals.writes.insert(totals.writes.end(), worker.writes.begin(), worker.writes.end());
      totals.read_queue.insert(totals.read_queue.end(), worker.read_queue.begin(), worker.read_queue.end());
      totals.write_queue.insert(totals.write_queue.end(), worker.write_queue.begin(), worker.write_queue.end());
      totals.errors += worker.errors;
    }
    return totals;
  }

 private:
  Key KeyFor(const Job& job) const {
    const size_t mixed = Hash(job.sequence + 1);
    const size_t index = mixed % 100 < settings_.hot_percent ? (mixed / 100) % 8
                                                            : 8 + (mixed / 100) % (kKeys - 8);
    return MakeKey(index);
  }

  void Worker(Samples* stats) {
    std::array<Job, kMaxBatch> jobs;
    for (;;) {
      size_t count = 0;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [&] { return arrivals_done_ || !reads_.empty() || !writes_.empty(); });
        if (reads_.empty() && writes_.empty()) return;
        if (writes_.empty() || (!reads_.empty() && reads_.front().sequence < writes_.front().sequence)) {
          jobs[count++] = reads_.front();
          reads_.pop_front();
        } else {
          while (count < settings_.batch_size && !writes_.empty()) {
            jobs[count++] = writes_.front();
            writes_.pop_front();
          }
        }
      }
      const Time started = Clock::now();
      const bool write = jobs[0].write;
      bool success;
      if (write) {
        std::array<Key, kMaxBatch> keys;
        std::array<Value, kMaxBatch> values;
        std::array<KvMutation, kMaxBatch> mutations;
        for (size_t i = 0; i < count; ++i) {
          keys[i] = KeyFor(jobs[i]);
          values[i] = MakeValue(jobs[i].sequence + 1);
          mutations[i] = {keys[i].data(), values[i].data(), kValueSize, 0, false};
        }
        success = store_.Write(mutations.data(), count);
      } else {
        const Key key = KeyFor(jobs[0]);
        Value value;
        size_t size = 0;
        success = store_.Get(key.data(), 0, value.data(), value.size(), &size) &&
                  size == value.size() &&
                  std::all_of(value.begin(), value.end(), [&](char byte) { return byte == value[0]; });
      }
      const Time completed = Clock::now();
      for (size_t i = 0; i < count; ++i) {
        if (!success) {
          ++stats->errors;
          continue;
        }
        (write ? stats->writes : stats->reads).push_back(Us(completed - jobs[i].arrival));
        (write ? stats->write_queue : stats->read_queue).push_back(Us(started - jobs[i].arrival));
      }
    }
  }

  KvStore& store_;
  const Settings& settings_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Job> reads_, writes_;
  bool arrivals_done_ = false;
};

void PrintPercentiles(const char* name, std::vector<uint64_t>& values) {
  if (values.empty()) {
    std::printf("\"%s\":null", name);
    return;
  }
  std::sort(values.begin(), values.end());
  const auto pct = [&](size_t percent) {
    return static_cast<unsigned long long>(values[(values.size() * percent + 99) / 100 - 1]);
  };
  std::printf("\"%s\":{\"p50\":%llu,\"p95\":%llu,\"p99\":%llu,\"max\":%llu}",
              name, pct(50), pct(95), pct(99), pct(100));
}

size_t Number(const char* input, size_t low, size_t high) {
  char* end = nullptr;
  const auto parsed = std::strtoull(input, &end, 10);
  Require(end != input && !*end && parsed >= low && parsed <= high, "invalid workload argument");
  return static_cast<size_t>(parsed);
}

void Verify(const char* path, bool acknowledged, bool commit_slots) {
  const Time start = Clock::now();
  KvStore store(false, false, Durability(commit_slots));
  Require(store.Open(path), "KV replay failed");
  const uint64_t replay_us = Us(Clock::now() - start);
  char value[5];
  size_t size = 0;
  Require(store.Get("anchor", 0, value, sizeof(value), &size) &&
          size == sizeof(value) && std::memcmp(value, "prior", size) == 0,
          "acknowledged anchor lost");
  const auto expects = acknowledged ? fortis::storage::KvReadStatus::Found : fortis::storage::KvReadStatus::Missing;
  for (const char* key : {"crash/1", "crash/2"}) {
    Require(store.GetStatus(key, 0, value, sizeof(value), &size) == expects,
            "crash frame was not replayed atomically");
    if (acknowledged)
      Require(size == sizeof(value) && std::memcmp(value, "value", size) == 0, "replayed value invalid");
  }
  std::printf("%llu", static_cast<unsigned long long>(replay_us));
}

void Crash(const char* path, bool commit_slots) {
  for (const char* point : {"write_after_header", "after_returned_ack"}) {
    const bool acknowledged = std::strcmp(point, "after_returned_ack") == 0;
    if (!acknowledged) {
      KvStore store(false, false, Durability(commit_slots));
      Require(store.Open(path), "crash seed open failed");
      const KvMutation marker{"anchor", "prior", 5, 0, false};
      Require(store.Write(&marker, 1), "crash seed write failed");
    }
    const pid_t child = fork();
    Require(child >= 0, "crash child could not start");
    if (child == 0) {
      KvStore store(false, false, Durability(commit_slots));
      const KvMutation mutations[] = {{"crash/1", "value", 5, 0, false},
                                      {"crash/2", "value", 5, 0, false}};
      if (!store.Open(path)) _exit(2);
      if (!acknowledged && setenv("FORTIS_KV_TEST_KILL", point, 1) != 0) _exit(2);
      if (!store.Write(mutations, 2)) _exit(3);
      if (acknowledged) raise(SIGKILL);
      _exit(4);
    }
    int status;
    Require(waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
            WTERMSIG(status) == SIGKILL, "crash injection did not fire");
    std::printf("%s\"%s_replay_us\":", acknowledged ? "," : "{", point);
    Verify(path, acknowledged, commit_slots);
  }
  std::puts("}");
}

void Benchmark(const char* path, const Settings& settings) {
  KvStore store(settings.group_commit, settings.parallel_point_reads,
                Durability(settings.commit_slots));
  Require(store.Open(path), "benchmark store could not open");
  Require(store.Size() == 0, "benchmark needs an empty store");
  Seed(store);
  const uint64_t seed_frames = store.WriteFrames();
  std::atomic<bool> stop{false};
  std::thread maintenance;
  if (settings.compact) {
    maintenance = std::thread([&] {
      fortis::storage::KvCompactionPolicy policy;
      policy.min_file_bytes = 1024 * 1024;
      policy.dead_percent = 35;
      policy.startup_delay_ms = 0;
      policy.min_interval_ms = 0;
      const Time maintenance_start = Clock::now();
      while (!stop.load()) {
        const auto elapsed = duration_cast<milliseconds>(Clock::now() - maintenance_start).count();
        store.MaintainCompaction(0, elapsed, policy, [&, visitor_state = &stop]() { return ([](void* context) { return static_cast<std::atomic<bool>*>(context)->load(); })(visitor_state); });
        std::this_thread::sleep_for(milliseconds(1));
      }
    });
  }
  uint64_t elapsed = 0;
  size_t peak_queue = 0;
  const std::clock_t cpu_start = std::clock();
  Samples samples = Workload(store, settings).Run(&elapsed, &peak_queue);
  stop = true;
  if (maintenance.joinable()) maintenance.join();
  const std::clock_t cpu_end = std::clock();
  fortis::storage::KvCompactionStats compaction{};
  store.CompactionStats(&compaction);
  const size_t windows = std::max(settings.seconds, size_t{1});
  std::printf("{\"submitted_reads\":%zu,\"submitted_writes\":%zu,\"read_ok\":%zu,\"write_ok\":%zu,"
              "\"read_rejected\":%zu,\"write_rejected\":%zu,\"errors\":%zu,\"elapsed_us\":%llu,"
              "\"peak_queue\":%zu,\"write_frames\":%llu,\"compactions_started\":%llu,"
              "\"compactions_completed\":%llu,\"compaction_pause_max_us\":%llu,",
              settings.reads * windows, settings.writes * windows, samples.reads.size(), samples.writes.size(),
              samples.rejected_reads, samples.rejected_writes, samples.errors,
              static_cast<unsigned long long>(elapsed), peak_queue,
              static_cast<unsigned long long>(store.WriteFrames() - seed_frames),
              static_cast<unsigned long long>(compaction.runs_started),
              static_cast<unsigned long long>(compaction.runs_completed),
              static_cast<unsigned long long>(compaction.pause_us_max));
  PrintPercentiles("read_us", samples.reads);
  std::printf(",");
  PrintPercentiles("write_us", samples.writes);
  std::printf(",");
  PrintPercentiles("read_queue_us", samples.read_queue);
  std::printf(",");
  PrintPercentiles("write_queue_us", samples.write_queue);
  std::printf(",\"workers\":%zu,\"queue_capacity\":%zu,\"batch_size\":%zu,\"hot_percent\":%zu,"
              "\"parallel_point_reads\":%s,\"commit_slots\":%s", settings.workers,
              settings.queue_capacity, settings.batch_size, settings.hot_percent,
              settings.parallel_point_reads ? "true" : "false",
              settings.commit_slots ? "true" : "false");
  if (cpu_start != static_cast<std::clock_t>(-1) && cpu_end >= cpu_start)
    std::printf(",\"process_cpu_us\":%.0f",
                (cpu_end - cpu_start) * 1000000.0 / CLOCKS_PER_SEC);
  std::puts("}");
  Require(samples.errors == 0, "one or more operations failed after admission");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    Require(argc == 3 || (argc >= 10 && argc <= 13), "usage: kv_capacity_benchmark empty_directory crash|crash-commit-slots | workload read write seconds workers queue batch hot-percent compact [group-commit [parallel-point-reads [commit-slots]]]");
    if (argc == 3 && std::strcmp(argv[2], "crash") == 0) Crash(argv[1], false);
    else if (argc == 3 && std::strcmp(argv[2], "crash-commit-slots") == 0) Crash(argv[1], true);
    else {
      Require(argc >= 10 && argc <= 13, "workload requires eight numeric settings and up to three flags");
      const Settings settings{Number(argv[2], 0, 1000000), Number(argv[3], 0, 1000000),
                              Number(argv[4], 0, 3600), Number(argv[5], 1, 1024),
                              Number(argv[6], 1, 1000000), Number(argv[7], 1, kMaxBatch),
                              Number(argv[8], 0, 100), Number(argv[9], 0, 1) != 0,
                              argc >= 11 && Number(argv[10], 0, 1) != 0,
                              argc >= 12 && Number(argv[11], 0, 1) != 0,
                              argc == 13 && Number(argv[12], 0, 1) != 0};
      const size_t offered = settings.reads + settings.writes;
      Require(offered && offered * std::max(settings.seconds, size_t{1}) <= 20000000,
              "workload must offer 1 to 20 million operations");
      Benchmark(argv[1], settings);
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
  return 0;
}
