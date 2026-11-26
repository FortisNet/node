#include <sstream>
#include "test_support.h"
// Incremental KV compaction: stepped runs against a model, fault injection at
// every commit/swap boundary, low disk, scheduler cadence, cancellation,
// concurrent foreground traffic, and SIGKILL at copy/fsync/rename/remap and
// restart points with acknowledged-write checks after every restart.
// Linked with the FORTIS_KV_TEST_FAILURES store object.
#include "../src/storage/kv_store.h"

#include <assert.h>
#include <atomic>
#include <map>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using fortis::storage::KvCompactionPhase;
using fortis::storage::KvCompactionPolicy;
using fortis::storage::KvCompactionStats;
using fortis::storage::KvMutation;

// --commit-slots runs every scenario against KvDurability::CommitSlots.
fortis::storage::KvDurability durability = fortis::storage::KvDurability::RollbackMarker;
uint64_t LogBase() { return durability == fortis::storage::KvDurability::CommitSlots ? 8192 : 0; }

class KvStore : public fortis::storage::KvStore {
 public:
  explicit KvStore(bool group_commit = false, bool parallel_point_reads = false)
      : fortis::storage::KvStore(group_commit, parallel_point_reads, durability) {}
};

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
      abort();                                                                  \
    }                                                                           \
  } while (0)

uint64_t Fnv(const char* text) {
  uint64_t hash = 1469598103934665603ull;
  for (; *text; ++text) { hash ^= static_cast<uint8_t>(*text); hash *= 1099511628211ull; }
  return hash;
}

// A self-describing value: version, size, payload derived from key+version.
std::string MakeValue(const std::string& key, uint32_t version, uint32_t size) {
  if (size < 8) size = 8;
  std::string value(size, '\0');
  memcpy(&value[0], &version, 4);
  memcpy(&value[4], &size, 4);
  uint64_t state = Fnv(key.c_str()) ^ (static_cast<uint64_t>(version) * 0x9e3779b97f4a7c15ull);
  for (uint32_t i = 8; i < size; ++i) {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    value[i] = static_cast<char>(state);
  }
  return value;
}

// Returns the version a stored value claims, or -1 if it is not a valid
// value for `key` at any version.
int64_t ValueVersion(const std::string& key, const char* data, size_t size) {
  if (size < 8) return -1;
  uint32_t version = 0, claimed = 0;
  memcpy(&version, data, 4);
  memcpy(&claimed, data + 4, 4);
  if (claimed != size) return -1;
  const std::string expected = MakeValue(key, version, claimed);
  return memcmp(expected.data(), data, size) == 0 ? version : -1;
}

struct Entry {
  uint32_t version = 0;
  uint32_t size = 0;
  int64_t expiry = 0;
};

// The acknowledged state: present keys only.
using Model = std::map<std::string, Entry>;

struct Rng {
  uint64_t state;
  explicit Rng(uint64_t seed) : state(seed ? seed : 1) {}
  uint32_t Next() {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    return static_cast<uint32_t>(state >> 11);
  }
  uint32_t Below(uint32_t n) { return Next() % n; }
};

struct Batch {
  std::vector<std::string> keys;
  std::vector<std::string> values;
  std::vector<KvMutation> mutations;
  std::vector<Entry> entries;
  void Put(const std::string& key, uint32_t version, uint32_t size, int64_t expiry) {
    keys.push_back(key);
    values.push_back(MakeValue(key, version, size));
    entries.push_back({version, static_cast<uint32_t>(values.back().size()), expiry});
    mutations.push_back({nullptr, nullptr, 0, expiry, false});
  }
  void Erase(const std::string& key) {
    keys.push_back(key);
    values.emplace_back();
    entries.push_back({0, 0, -1});
    mutations.push_back({nullptr, nullptr, 0, 0, true});
  }
  bool Commit(KvStore& store, Model* model) {
    for (size_t i = 0; i < mutations.size(); ++i) {
      mutations[i].key = keys[i].c_str();
      if (!mutations[i].erase) {
        mutations[i].value = values[i].data();
        mutations[i].value_size = static_cast<uint32_t>(values[i].size());
      }
    }
    if (!store.Write(mutations.data(), mutations.size())) return false;
    if (model) {
      for (size_t i = 0; i < keys.size(); ++i) {
        if (mutations[i].erase) model->erase(keys[i]);
        else (*model)[keys[i]] = entries[i];
      }
    }
    return true;
  }
};

struct ScanCount { size_t count = 0; };
bool CountVisit(const char*, const void*, size_t, void* context) {
  ++static_cast<ScanCount*>(context)->count;
  return true;
}

// Every model key reads back exactly; expired ones are invisible; a full scan
// sees no key the model does not have.
void Verify(KvStore& store, const Model& model, int64_t now, const char* prefix = "") {
  std::vector<char> buffer(1 << 20);
  size_t visible = 0;
  for (const auto& item : model) {
    size_t size = 0;
    const auto status = store.GetStatus(item.first.c_str(), now, buffer.data(),
                                        buffer.size(), &size);
    if (item.second.expiry && item.second.expiry <= now) {
      CHECK(status == fortis::storage::KvReadStatus::Missing);
      continue;
    }
    ++visible;
    if (status != fortis::storage::KvReadStatus::Found) {
      fprintf(stderr, "missing %s\n", item.first.c_str());
      CHECK(false);
    }
    CHECK(size == item.second.size);
    CHECK(ValueVersion(item.first, buffer.data(), size) == item.second.version);
  }
  ScanCount counted;
  CHECK(store.Scan(prefix, nullptr, SIZE_MAX, now, [&, visitor_state = &counted](const char* key, const void* value, size_t size) { return (CountVisit)(key, value, size, visitor_state); }));
  CHECK(counted.count == visible);
}

std::string MakeDirectory(const char* tag) {
  char path[] = "/tmp/fortis-kv-compaction-XXXXXX";
  CHECK(mkdtemp(path));
  (void)tag;
  return path;
}

void RemoveDirectory(const std::string& directory) {
  for (const char* name : {"fortis.kv", "fortis.kv.lock", "fortis.kv.compact"}) {
    const std::string path = directory + "/" + name;
    unlink(path.c_str());
  }
  CHECK(rmdir(directory.c_str()) == 0);
}

bool TemporaryExists(const std::string& directory) {
  return access((directory + "/fortis.kv.compact").c_str(), F_OK) == 0;
}

KvCompactionPolicy TinyPolicy() {
  KvCompactionPolicy policy;
  policy.min_file_bytes = 0;
  policy.dead_percent = 0;
  policy.startup_delay_ms = 0;
  policy.min_interval_ms = 0;
  policy.retry_backoff_ms = 0;
  policy.max_backoff_ms = 0;
  policy.reserve_bytes = 0;
  policy.slice_nodes = 7;
  policy.slice_records = 3;
  policy.slice_bytes = 4096;  // Copy slices stay tiny via slice_records.
  policy.swap_tail_bytes = 64;
  policy.sync_every_bytes = 1000;
  policy.tick_max_slices = 1;
  policy.tick_budget_ms = 60000;
  return policy;
}

KvCompactionStats Stats(KvStore& store) {
  KvCompactionStats stats;
  store.CompactionStats(&stats);
  return stats;
}

void Seed(KvStore& store, Model* model, Rng& rng, unsigned count, int64_t expiry_base) {
  for (unsigned i = 0; i < count; i += 4) {
    Batch batch;
    for (unsigned j = i; j < i + 4 && j < count; ++j) {
      char key[48];
      snprintf(key, sizeof(key), "seed/%05u", j);
      const int64_t expiry = j % 5 == 0 ? expiry_base + static_cast<int64_t>(j % 40) : 0;
      batch.Put(key, 1, 8 + rng.Below(200), expiry);
    }
    CHECK(batch.Commit(store, model));
  }
  // Overwrites and tombstones so there is garbage to reclaim.
  for (unsigned i = 0; i < count; i += 3) {
    Batch batch;
    char key[48];
    snprintf(key, sizeof(key), "seed/%05u", i);
    if (i % 2) batch.Erase(key);
    else batch.Put(key, 2, 8 + rng.Below(200), 0);
    CHECK(batch.Commit(store, model));
  }
}

// One random multi-record frame: puts, overwrites, erases and expiring puts.
void RandomFrame(KvStore& store, Model* model, Rng& rng, int64_t now, uint32_t* version) {
  Batch batch;
  const unsigned records = 1 + rng.Below(4);
  std::map<std::string, bool> used;
  for (unsigned i = 0; i < records; ++i) {
    char key[48];
    if (rng.Below(2)) snprintf(key, sizeof(key), "seed/%05u", rng.Below(400));
    else snprintf(key, sizeof(key), "live/%04u", rng.Below(300));
    if (used[key]) continue;
    used[key] = true;
    const uint32_t action = rng.Below(10);
    if (action < 2) batch.Erase(key);
    else batch.Put(key, ++*version, 8 + rng.Below(300),
                   action < 4 ? now + 1 + rng.Below(60) : 0);
  }
  if (!batch.mutations.empty()) CHECK(batch.Commit(store, model));
}

void TestSteppedRuns() {
  const std::string directory = MakeDirectory("stepped");
  Model model;
  Rng rng(7);
  uint32_t version = 10;
  int64_t now = 500;
  KvCompactionPolicy policy = TinyPolicy();
  {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    Seed(store, &model, rng, 400, 560);
    bool saw[4] = {false, false, false, false};
    for (unsigned run = 0; run < 6; ++run) {
      const uint64_t before = Stats(store).runs_completed;
      bool running = store.MaintainCompaction(now, now, policy);
      CHECK(running);
      unsigned steps = 0;
      while (running) {
        saw[static_cast<int>(Stats(store).phase)] = true;
        // Foreground frames between every bounded step.
        for (unsigned i = 0; i < 1 + rng.Below(3); ++i) RandomFrame(store, &model, rng, now, &version);
        if (rng.Below(8) == 0) store.Sweep(now, 4);
        now += 1;
        if (steps % 10 == 0) Verify(store, model, now);
        running = store.MaintainCompaction(now, now, policy);
        ++steps;
      }
      const KvCompactionStats stats = Stats(store);
      CHECK(stats.runs_completed == before + 1);
      CHECK(stats.phase == KvCompactionPhase::Idle);
      CHECK(!TemporaryExists(directory));
      Verify(store, model, now);
      // A quiet run leaves exactly live records plus frame headers.
      store.Close();
      CHECK(store.Open(directory.c_str()));
      Verify(store, model, now);
    }
    CHECK(saw[1] && saw[2] && saw[3]);
    const KvCompactionStats stats = Stats(store);
    CHECK(stats.expired_dropped > 0);
    CHECK(stats.bytes_reclaimed == 0 || stats.bytes_copied > 0);
    // With no foreground traffic the result is compact: live bytes plus at
    // most one 16-byte frame overhead per three-record slice.
    const uint64_t live = stats.live_bytes;
    while (store.MaintainCompaction(now, now, policy)) {}
    const KvCompactionStats quiet = Stats(store);
    CHECK(quiet.file_bytes <= LogBase() + live + 16 * (store.Size() / 3 + 2));
    printf("stepped: runs=%llu copied=%llu tail=%llu reclaimed=%llu expired=%llu "
           "pauses=%llu max_pause_us=%llu\n",
           static_cast<unsigned long long>(quiet.runs_completed),
           static_cast<unsigned long long>(quiet.bytes_copied),
           static_cast<unsigned long long>(quiet.tail_bytes_copied),
           static_cast<unsigned long long>(quiet.bytes_reclaimed),
           static_cast<unsigned long long>(quiet.expired_dropped),
           static_cast<unsigned long long>(quiet.pauses),
           static_cast<unsigned long long>(quiet.pause_us_max));
  }
  // Directed ordering: a key copied then erased, one copied then rewritten
  // with expiry, and one erased then re-added all keep their final state.
  {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    Verify(store, model, now);
    Batch first;
    first.Put("order/a", 1, 32, 0);
    first.Put("order/b", 1, 32, 0);
    first.Put("order/c", 1, 32, 0);
    CHECK(first.Commit(store, &model));
    KvCompactionPolicy whole = TinyPolicy();
    whole.slice_nodes = 100000;
    whole.slice_records = 100000;
    whole.slice_bytes = 1 << 20;
    CHECK(store.MaintainCompaction(now, now, whole));  // Copy: all staged.
    CHECK(Stats(store).phase == KvCompactionPhase::CatchUp);
    Batch tail;
    tail.Erase("order/a");
    tail.Put("order/b", 2, 40, now + 1000);
    tail.Erase("order/c");
    CHECK(tail.Commit(store, &model));
    Batch again;
    again.Put("order/c", 3, 24, 0);
    CHECK(again.Commit(store, &model));
    while (store.MaintainCompaction(now, now, whole)) {
      Verify(store, model, now);
    }
    Verify(store, model, now);
    store.Close();
    CHECK(store.Open(directory.c_str()));
    Verify(store, model, now);
    char value[64]; size_t size = 0;
    CHECK(!store.Get("order/a", now, value, sizeof(value), &size));
    CHECK(!store.Get("order/b", now + 1000, value, sizeof(value), &size));
  }
  RemoveDirectory(directory);
  puts("stepped compaction against model: ok");
}

// Fault at each boundary: the run fails, the temporary file is gone, the
// same instance keeps serving the authoritative log, and a retry succeeds.
void TestFaults() {
  const char* stages[] = {
      "compact_copy_write", "compact_copy_fsync", "compact_catchup_write",
      "compact_swap_tail", "compact_before_fsync", "compact_after_file_fsync",
  };
  for (const char* stage : stages) {
    const std::string directory = MakeDirectory(stage);
    Model model;
    Rng rng(Fnv(stage));
    uint32_t version = 10;
    int64_t now = 100;
    KvCompactionPolicy policy = TinyPolicy();
    policy.sync_every_bytes = 200;
    {
      KvStore store;
      CHECK(store.Open(directory.c_str()));
      Seed(store, &model, rng, 200, 1000000);
      CHECK(setenv("FORTIS_KV_TEST_FAIL", stage, 1) == 0);
      bool running = store.MaintainCompaction(now, now, policy);
      unsigned steps = 0;
      while (running && steps < 100000) {
        // Enough tail traffic that catch-up and the swap tail both run.
        for (unsigned i = 0; i < 2; ++i) RandomFrame(store, &model, rng, now + 100000, &version);
        // The swap-tail stage needs the locked swap to copy a real tail.
        if (strcmp(stage, "compact_swap_tail") == 0 &&
            Stats(store).phase == KvCompactionPhase::CatchUp)
          policy.swap_tail_bytes = 1 << 20;
        running = store.MaintainCompaction(now, now, policy);
        ++steps;
      }
      CHECK(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
      KvCompactionStats stats = Stats(store);
      if (stats.runs_failed != 1) {
        fprintf(stderr, "stage %s did not fail (completed=%llu)\n", stage,
                static_cast<unsigned long long>(stats.runs_completed));
        CHECK(false);
      }
      CHECK(stats.runs_completed == 0 && stats.phase == KvCompactionPhase::Idle);
      CHECK(!TemporaryExists(directory));
      RandomFrame(store, &model, rng, now + 100000, &version);
      Verify(store, model, now);
      policy.swap_tail_bytes = 64;
      while (store.MaintainCompaction(now, now, policy))
        RandomFrame(store, &model, rng, now + 100000, &version);
      stats = Stats(store);
      CHECK(stats.runs_completed == 1);
      Verify(store, model, now);
    }
    {
      KvStore store;
      CHECK(store.Open(directory.c_str()));
      Verify(store, model, now);
    }
    RemoveDirectory(directory);
  }
  puts("fault injection at copy/fsync/catch-up/swap/rename boundaries: ok");
}

// Foreground appends that outrun catch-up abandon the run after the round
// limit instead of growing the temporary file without bound.
void TestStarvation() {
  const std::string directory = MakeDirectory("starved");
  Model model;
  Rng rng(17);
  uint32_t version = 10;
  KvStore store;
  CHECK(store.Open(directory.c_str()));
  Seed(store, &model, rng, 100, 0);
  KvCompactionPolicy policy = TinyPolicy();
  policy.slice_bytes = 64;
  policy.max_catch_up_rounds = 20;
  bool running = store.MaintainCompaction(0, 0, policy);
  while (running) {
    for (unsigned i = 0; i < 3; ++i) RandomFrame(store, &model, rng, 1000000, &version);
    running = store.MaintainCompaction(0, 0, policy);
  }
  const KvCompactionStats stats = Stats(store);
  CHECK(stats.runs_failed == 1 && stats.last_error == fortis::storage::KvCompactionError::Starved);
  CHECK(!TemporaryExists(directory));
  Verify(store, model, 0);
  store.Close();
  RemoveDirectory(directory);
  puts("catch-up starvation abandons the run: ok");
}

void TestLowDiskAndBackoff() {
  const std::string directory = MakeDirectory("disk");
  Model model;
  Rng rng(11);
  KvStore store;
  CHECK(store.Open(directory.c_str()));
  Seed(store, &model, rng, 200, 0);
  KvCompactionPolicy policy = TinyPolicy();
  policy.retry_backoff_ms = 100;
  policy.max_backoff_ms = 350;
  policy.reserve_bytes = 1000;
  CHECK(setenv("FORTIS_KV_TEST_FREE_BYTES", "0", 1) == 0);
  CHECK(!store.MaintainCompaction(0, 0, policy));
  KvCompactionStats stats = Stats(store);
  CHECK(stats.skipped_low_disk == 1 && stats.blocked_low_disk && stats.runs_started == 0);
  CHECK(!TemporaryExists(directory));
  CHECK(!store.MaintainCompaction(0, 99, policy));   // Backing off.
  CHECK(Stats(store).skipped_low_disk == 1);
  CHECK(!store.MaintainCompaction(0, 100, policy));  // Retry: still full.
  CHECK(Stats(store).skipped_low_disk == 2);
  CHECK(!store.MaintainCompaction(0, 299, policy));  // Backoff doubled to 200.
  CHECK(Stats(store).skipped_low_disk == 2);
  CHECK(!store.MaintainCompaction(0, 300, policy));
  CHECK(!store.MaintainCompaction(0, 700, policy));  // Capped at 350.
  CHECK(Stats(store).skipped_low_disk == 4);
  // Room to start; the volume then fills below the reserve mid-run.
  CHECK(setenv("FORTIS_KV_TEST_FREE_BYTES", "100000000", 1) == 0);
  CHECK(store.MaintainCompaction(0, 1050, policy));
  CHECK(Stats(store).phase == KvCompactionPhase::Copy && TemporaryExists(directory));
  CHECK(setenv("FORTIS_KV_TEST_FREE_BYTES", "999", 1) == 0);
  CHECK(!store.MaintainCompaction(0, 1051, policy));
  stats = Stats(store);
  CHECK(stats.runs_failed == 1 && stats.last_error == fortis::storage::KvCompactionError::LowDisk);
  CHECK(!TemporaryExists(directory));
  // Foreground writes continue on the authoritative log.
  uint32_t version = 50;
  RandomFrame(store, &model, rng, 0, &version);
  Verify(store, model, 0);
  CHECK(unsetenv("FORTIS_KV_TEST_FREE_BYTES") == 0);
  while (store.MaintainCompaction(0, 5000, policy)) {}
  CHECK(Stats(store).runs_completed == 1);
  Verify(store, model, 0);
  store.Close();
  RemoveDirectory(directory);
  puts("low-disk skip, mid-run abort and capped backoff: ok");
}

void TestScheduleAndCancellation() {
  const std::string directory = MakeDirectory("schedule");
  Model model;
  Rng rng(13);
  KvStore store;
  CHECK(store.Open(directory.c_str()));
  KvCompactionPolicy policy = TinyPolicy();
  policy.min_file_bytes = 4096;
  policy.dead_percent = 50;
  policy.dead_bytes = UINT64_MAX;
  policy.startup_delay_ms = 1000;
  policy.min_interval_ms = 5000;
  policy.tick_max_slices = 1000000;
  auto overwrite_all = [&](uint32_t* version, unsigned rounds) {
    for (unsigned round = 0; round < rounds; ++round) {
      for (unsigned i = 0; i < 100; ++i) {
        Batch batch;
        char key[32];
        snprintf(key, sizeof(key), "unique/%04u", i);
        batch.Put(key, ++*version, 100, 0);
        CHECK(batch.Commit(store, &model));
      }
    }
  };
  uint32_t version = 0;
  overwrite_all(&version, 1);                          // Unique keys: no garbage.
  CHECK(!store.MaintainCompaction(0, 0, policy));      // Anchors the startup delay.
  CHECK(!store.MaintainCompaction(0, 2000, policy));   // Not due: no garbage.
  CHECK(Stats(store).runs_started == 0);
  // A second instance has its own anchor: due, but inside the startup delay.
  overwrite_all(&version, 3);
  {
    KvStore fresh;
    store.Close();
    CHECK(fresh.Open(directory.c_str()));
    CHECK(!fresh.MaintainCompaction(0, 50000, policy));  // Anchor.
    CHECK(!fresh.MaintainCompaction(0, 50999, policy));  // Still starting up.
    CHECK(Stats(fresh).runs_started == 0);
    fresh.Close();
  }
  CHECK(store.Open(directory.c_str()));
  CHECK(!store.MaintainCompaction(0, 3000, policy));   // Due: one call finishes it.
  KvCompactionStats stats = Stats(store);
  CHECK(stats.runs_completed == 1);
  // Due again soon after, but not before min_interval from the last start.
  overwrite_all(&version, 3);
  CHECK(!store.MaintainCompaction(0, 7999, policy));
  CHECK(Stats(store).runs_started == 1);
  // Cancellation: the call returns with the run in progress; later calls
  // resume it from its cursor.
  static int budget = 0;
  budget = 3;
  auto cancelled = [](void*) -> bool { return budget-- <= 0; };
  policy.tick_max_slices = 1000000;
  CHECK(store.MaintainCompaction(0, 8000, policy, [&, visitor_state = nullptr]() { return (cancelled)(visitor_state); }));
  CHECK(Stats(store).phase != KvCompactionPhase::Idle);
  Verify(store, model, 0);
  while (store.MaintainCompaction(0, 8001, policy)) {}
  CHECK(Stats(store).runs_completed == 2);
  Verify(store, model, 0);
  // Close abandons a run between slices and deletes its temporary file.
  overwrite_all(&version, 3);
  policy.tick_max_slices = 2;
  CHECK(store.MaintainCompaction(0, 20000, policy));
  CHECK(TemporaryExists(directory));
  store.Close();
  CHECK(!TemporaryExists(directory));
  CHECK(store.Open(directory.c_str()));
  Verify(store, model, 0);
  store.Close();
  RemoveDirectory(directory);
  puts("startup delay, thresholds, interval, cancellation and close: ok");
}

// Two writers, readers, a pager, a sweeper and the maintenance loop all run
// together; readers only ever see self-consistent values with versions that
// never go backwards.
void TestConcurrent(bool parallel_point_reads = false) {
  const std::string directory = MakeDirectory("concurrent");
  KvStore store(false, parallel_point_reads);
  CHECK(store.Open(directory.c_str()));
  constexpr unsigned kWriters = 2, kKeys = 200, kRounds = 3000;
  std::vector<std::vector<uint32_t>> latest(kWriters, std::vector<uint32_t>(kKeys, 0));
  std::atomic<bool> failed{false}, done{false};
  std::vector<std::thread> threads;
  for (unsigned writer = 0; writer < kWriters; ++writer) {
    threads.emplace_back([&, writer] {
      Rng rng(100 + writer);
      // Keep writing until the maintenance loop has swapped several times.
      for (uint32_t round = 1;
           round <= kRounds || (Stats(store).runs_completed < 3 && round < 400000); ++round) {
        const unsigned slot = rng.Below(kKeys);
        char row[48], index[48];
        snprintf(row, sizeof(row), "c/%u/row/%04u", writer, slot);
        snprintf(index, sizeof(index), "c/%u/idx/%04u", writer, slot);
        Batch batch;
        if (rng.Below(10) == 0) { batch.Erase(row); batch.Erase(index); }
        else { batch.Put(row, round, 16 + rng.Below(600), 0); batch.Put(index, round, 12, 0); }
        if (!batch.Commit(store, nullptr)) failed = true;
        latest[writer][slot] = batch.mutations[0].erase ? 0 : round;
        // Keep foreground traffic active while letting maintenance converge
        // on filesystems where the append rate can outpace the catch-up copy.
        if (round % 16 == 0) usleep(1000);
      }
    });
  }
  for (unsigned reader = 0; reader < 2; ++reader) {
    threads.emplace_back([&, reader] {
      Rng rng(200 + reader);
      std::vector<char> buffer(4096);
      while (!done) {
        const unsigned writer = rng.Below(kWriters), slot = rng.Below(kKeys);
        char row[48], index[48];
        snprintf(row, sizeof(row), "c/%u/row/%04u", writer, slot);
        snprintf(index, sizeof(index), "c/%u/idx/%04u", writer, slot);
        size_t size = 0;
        const auto status = store.GetStatus(row, 0, buffer.data(), buffer.size(), &size);
        if (status == fortis::storage::KvReadStatus::Found) {
          if (ValueVersion(row, buffer.data(), size) < 0) failed = true;
        } else if (status != fortis::storage::KvReadStatus::Missing) {
          failed = true;
        }
        if (store.GetStatus(index, 0, buffer.data(), buffer.size(), &size) ==
                fortis::storage::KvReadStatus::Found &&
            ValueVersion(index, buffer.data(), size) < 0) failed = true;
      }
    });
  }
  threads.emplace_back([&] {
    while (!done) {
      fortis::storage::KvPageResult page;
      ScanCount counted;
      if (!store.Page("c/", nullptr, 64, 0, false, [&, visitor_state = &counted](const char* key, const void* value, size_t size) { return (CountVisit)(key, value, size, visitor_state); }, &page)) failed = true;
      store.Sweep(0, 16);
    }
  });
  std::atomic<unsigned> runs{0};
  std::thread maintenance([&] {
    KvCompactionPolicy policy = TinyPolicy();
    policy.slice_nodes = 64;
    policy.slice_records = 32;
    policy.slice_bytes = 64 * 1024;
    policy.swap_tail_bytes = 2048;
    policy.tick_max_slices = 16;
    int64_t tick = 0;
    while (!done) {
      if (!store.MaintainCompaction(0, ++tick, policy)) ++runs;
    }
  });
  for (unsigned writer = 0; writer < kWriters; ++writer) threads[writer].join();
  done = true;
  for (size_t i = kWriters; i < threads.size(); ++i) threads[i].join();
  maintenance.join();
  const KvCompactionStats stats = Stats(store);
  if (stats.runs_completed < 2 || stats.runs_failed) {
    fprintf(stderr, "concurrent: completed=%llu failed=%llu error=%d\n",
            static_cast<unsigned long long>(stats.runs_completed),
            static_cast<unsigned long long>(stats.runs_failed),
            static_cast<int>(stats.last_error));
    CHECK(false);
  }
  CHECK(!failed);
  auto check_final = [&](KvStore& view) {
    std::vector<char> buffer(4096);
    for (unsigned writer = 0; writer < kWriters; ++writer) {
      for (unsigned slot = 0; slot < kKeys; ++slot) {
        char row[48];
        snprintf(row, sizeof(row), "c/%u/row/%04u", writer, slot);
        size_t size = 0;
        const bool found = view.Get(row, 0, buffer.data(), buffer.size(), &size);
        if (!latest[writer][slot]) CHECK(!found);
        else CHECK(found && ValueVersion(row, buffer.data(), size) == latest[writer][slot]);
      }
    }
  };
  check_final(store);
  store.Close();
  CHECK(store.Open(directory.c_str()));
  check_final(store);
  printf("concurrent: runs=%llu pauses=%llu max_pause_us=%llu swap_pause_us=%llu "
         "lock_hold_max_us=%llu\n",
         static_cast<unsigned long long>(stats.runs_completed),
         static_cast<unsigned long long>(stats.pauses),
         static_cast<unsigned long long>(stats.pause_us_max),
         static_cast<unsigned long long>(stats.swap_pause_us_max),
         static_cast<unsigned long long>(stats.lock_hold_us_max));
  store.Close();
  RemoveDirectory(directory);
  puts("concurrent foreground traffic during repeated compaction: ok");
}

// ---- Hard kill ------------------------------------------------------------

struct KeyLog {
  // Op sequence numbers: acknowledged and the (at most one) in flight.
  int64_t acked = -1;      // -1: never acknowledged (state = seed state).
  int64_t attempted = -1;
  bool acked_erase = false;
  bool attempted_erase = false;
};

void ChildMain(const std::string& directory, int report, const char* point, unsigned call) {
  // Later kill points need repeated swaps while the writer syncs its marker.
  alarm(90);
  setenv("FORTIS_KV_TEST_KILL", point, 1);
  char call_text[16];
  snprintf(call_text, sizeof(call_text), "%u", call);
  setenv("FORTIS_KV_TEST_KILL_ON_CALL", call_text, 1);
  KvStore store;
  if (!store.Open(directory.c_str())) _exit(4);
  std::atomic<bool> stop{false};
  std::thread writer([&] {
    Rng rng(call * 7919u + Fnv(point));
    for (uint32_t op = 1; !stop; ++op) {
      const unsigned slot = rng.Below(97);
      char row[48], index[48], event[48], line[96];
      snprintf(row, sizeof(row), "w/row/%03u", slot);
      snprintf(index, sizeof(index), "w/idx/%03u", slot);
      snprintf(event, sizeof(event), "w/event/%08u", op);
      const bool erase = rng.Below(8) == 0;
      Batch batch;
      if (erase) { batch.Erase(row); batch.Erase(index); }
      else {
        batch.Put(row, op, 16 + rng.Below(400), 0);
        batch.Put(index, op, 12, 0);
        batch.Put(event, op, 24, 1000 + op);  // Expiring related record.
      }
      const int length = snprintf(line, sizeof(line), "A %u %u %d\n", slot, op, erase ? 1 : 0);
      if (write(report, line, length) != length) _exit(5);
      if (!batch.Commit(store, nullptr)) _exit(6);
      line[0] = 'K';
      if (write(report, line, length) != length) _exit(5);
      // Give the compactor a chance to advance between durable writes.
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });
  KvCompactionPolicy policy = TinyPolicy();
  policy.slice_nodes = 16;
  policy.slice_records = 8;
  policy.slice_bytes = 2048;
  policy.swap_tail_bytes = 1024;
  // Exercise the explicit catch-up barrier even when periodic sync timing
  // would otherwise leave the temporary log already synced.
  policy.sync_every_bytes = strcmp(point, "catch_up_fsync") == 0 ? UINT64_MAX : 4096;
  int64_t tick = 0;
  for (;;) {
    ++tick;
    store.MaintainCompaction(1000 + tick, tick, policy);
  }
}

void HardKillOnce(const char* point, unsigned call) {
  const std::string directory = MakeDirectory(point);
  Model seed;
  {
    Rng rng(3);
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    Seed(store, &seed, rng, 300, 1000000000);
  }
  int pipe_fds[2];
  CHECK(pipe(pipe_fds) == 0);
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    close(pipe_fds[0]);
    ChildMain(directory, pipe_fds[1], point, call);
    _exit(7);
  }
  close(pipe_fds[1]);
  std::string log;
  char buffer[4096];
  for (;;) {
    const ssize_t n = read(pipe_fds[0], buffer, sizeof(buffer));
    if (n <= 0) break;
    log.append(buffer, static_cast<size_t>(n));
  }
  close(pipe_fds[0]);
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child);
  if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
    fprintf(stderr, "kill point %s#%u not reached (status=%d)\n", point, call, status);
    CHECK(false);
  }
  std::map<unsigned, KeyLog> keys;
  std::map<uint32_t, unsigned> op_slot;
  size_t acked_ops = 0;
  for (size_t start = 0; start < log.size();) {
    const size_t end = log.find('\n', start);
    if (end == std::string::npos) break;  // A torn final line is impossible (<PIPE_BUF).
    char kind = 0; unsigned slot = 0, op = 0; int erase = 0;
    std::istringstream fields(log.substr(start, log.find('\n', start) - start));
    CHECK(static_cast<bool>(fields >> kind >> slot >> op >> erase));
    KeyLog& entry = keys[slot];
    if (kind == 'A') { entry.attempted = op; entry.attempted_erase = erase; }
    else { entry.acked = op; entry.acked_erase = erase; ++acked_ops; }
    op_slot[op] = slot;
    start = end + 1;
  }
  KvStore store;
  CHECK(store.Open(directory.c_str()));
  CHECK(!TemporaryExists(directory));
  // Seed records the writer never touches are intact.
  Verify(store, seed, 0, "seed/");
  std::vector<char> value(4096);
  for (const auto& item : keys) {
    char row[48], index[48];
    snprintf(row, sizeof(row), "w/row/%03u", item.first);
    snprintf(index, sizeof(index), "w/idx/%03u", item.first);
    size_t row_size = 0, index_size = 0;
    const bool row_found = store.Get(row, 0, value.data(), value.size(), &row_size);
    const int64_t row_version = row_found ? ValueVersion(row, value.data(), row_size) : 0;
    const bool index_found = store.Get(index, 0, value.data(), value.size(), &index_size);
    const int64_t index_version = index_found ? ValueVersion(index, value.data(), index_size) : 0;
    // Related records of one frame are all-or-nothing.
    CHECK(row_found == index_found && row_version == index_version);
    if (row_found) CHECK(row_version > 0);
    const KeyLog& entry = item.second;
    // The recovered state is the last acknowledged op's, or the in-flight
    // one's; never an older or fabricated state.
    const bool matches_acked = entry.acked < 0
        ? !row_found
        : (entry.acked_erase ? !row_found : row_found && row_version == entry.acked);
    const bool matches_attempt = entry.attempted > entry.acked &&
        (entry.attempted_erase ? !row_found : row_found && row_version == entry.attempted);
    if (!matches_acked && !matches_attempt) {
      fprintf(stderr, "%s#%u slot %u: found=%d version=%lld acked=%lld attempted=%lld\n",
              point, call, item.first, row_found, static_cast<long long>(row_version),
              static_cast<long long>(entry.acked), static_cast<long long>(entry.attempted));
      CHECK(false);
    }
  }
  // Events: any recovered event belongs to an attempted put whose row frame
  // is consistent; acknowledged unexpired events are present.
  struct EventScan { std::map<uint32_t, unsigned>* ops; bool ok = true; size_t count = 0; };
  EventScan scan{&op_slot};
  CHECK(store.Scan("w/event/", nullptr, SIZE_MAX, 0, [context = &scan](const char* key, const void* data, size_t size) {
    auto* state = static_cast<EventScan*>(context);
    const uint32_t op = static_cast<uint32_t>(strtoul(key + 8, nullptr, 10));
    if (!state->ops->count(op) || ValueVersion(key, static_cast<const char*>(data), size) !=
        static_cast<int64_t>(op)) state->ok = false;
    ++state->count;
    return true;
  }));
  CHECK(scan.ok);
  // Retry safely: the recovered log compacts and reopens cleanly.
  CHECK(store.Compact());
  store.Close();
  CHECK(store.Open(directory.c_str()));
  Verify(store, seed, 0, "seed/");
  store.Close();
  printf("hard kill %-22s #%u: acked_ops=%zu keys=%zu events=%zu\n", point, call,
         acked_ops, keys.size(), scan.count);
  RemoveDirectory(directory);
}

// Restart points: a stale temporary file and a replay killed mid-way.
void HardKillRestart(const char* point, unsigned call) {
  const std::string directory = MakeDirectory(point);
  Model seed;
  {
    Rng rng(5);
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    Seed(store, &seed, rng, 300, 0);
  }
  const std::string stale = directory + "/fortis.kv.compact";
  FILE* handle = fopen(stale.c_str(), "w");
  CHECK(handle && fputs("FKV1 partial temporary", handle) >= 0 && fclose(handle) == 0);
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    alarm(30);
    setenv("FORTIS_KV_TEST_KILL", point, 1);
    char call_text[16];
    snprintf(call_text, sizeof(call_text), "%u", call);
    setenv("FORTIS_KV_TEST_KILL_ON_CALL", call_text, 1);
    KvStore store;
    store.Open(directory.c_str());
    _exit(7);
  }
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  KvStore store;
  CHECK(store.Open(directory.c_str()));
  CHECK(!TemporaryExists(directory));
  Verify(store, seed, 0);
  store.Close();
  printf("hard kill %-22s #%u: restart recovered %zu seed keys\n", point, call, seed.size());
  RemoveDirectory(directory);
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc > 1 && strcmp(argv[1], "--commit-slots") == 0) {
    durability = fortis::storage::KvDurability::CommitSlots;
    --argc;
    ++argv;
    puts("durability: commit slots");
  }
  if (argc == 2 && strcmp(argv[1], "--parallel-point-reads") == 0) {
    TestConcurrent(true);
    return 0;
  }
  const bool kill_only = argc > 1 && strcmp(argv[1], "--kill-only") == 0;
  if (!kill_only) {
    TestSteppedRuns();
    TestFaults();
    TestStarvation();
    TestLowDiskAndBackoff();
    TestScheduleAndCancellation();
    TestConcurrent();
  }
  const char* points[] = {
      "copy_slice", "catch_up_slice", "catch_up_fsync", "swap_tail_copied", "swap_file_synced",
      "swap_renamed", "swap_directory_synced", "remap_slice", "write_acknowledged",
  };
  for (const char* point : points)
    for (unsigned call : {1u, 5u, 25u}) HardKillOnce(point, call);
  // Append stages between the frame flush and the slot flush, with
  // compaction running beside the writer.
  if (durability == fortis::storage::KvDurability::CommitSlots)
    for (const char* point : {"write_before_fsync", "commit_frame_synced", "commit_slot_written"})
      for (unsigned call : {1u, 5u, 25u}) HardKillOnce(point, call);
  HardKillRestart("open_after_stale_unlink", 1);
  HardKillRestart("replay_frame", 1);
  HardKillRestart("replay_frame", 40);
  puts("native KV incremental compaction test passed");
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
