#include "../src/storage/kv_store.h"

#include <assert.h>
#include <atomic>
#include <chrono>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <thread>
#include <unistd.h>

struct VisitState {
  size_t count = 0;
  char last[128]{};
  bool valid = true;
};

static bool visit(const char* key, const void*, size_t size, VisitState* state) {
  if (size != sizeof(uint64_t) || (state->count && strcmp(state->last, key) >= 0)) {
    state->valid = false;
    return false;
  }
  snprintf(state->last, sizeof(state->last), "%s", key);
  ++state->count;
  return true;
}

static void write_value(fortis::storage::KvStore& store, const char* key, uint64_t value,
                        int64_t expiry = 0) {
  const fortis::storage::KvMutation mutation{key, &value, sizeof(value), expiry, false};
  assert(store.Write(&mutation, 1));
}

int main() {
  char directory[] = "/tmp/fortis-kv-concurrent-XXXXXX";
  assert(mkdtemp(directory));
  fortis::storage::KvStore store;
  assert(store.Open(directory));

  for (unsigned i = 0; i < 16; ++i) {
    char key[32];
    snprintf(key, sizeof(key), "stress/shared/%02u", i);
    write_value(store, key, i);
  }

  // Hold the lock in a scan and verify that the diagnostics remain readable
  // while a point read waits for it. Release the scan before joining the reader.
  std::atomic<bool> scanning{false};
  std::atomic<bool> release_scan{false};
  std::atomic<bool>* signals[] = {&scanning, &release_scan};
  std::thread scanner([&] {
    assert(store.Scan("stress/", nullptr, 1, 0, [context = signals](const char*, const void*, size_t) {
      auto** state = static_cast<std::atomic<bool>**>(context);
      state[0]->store(true);
      while (!state[1]->load()) std::this_thread::yield();
      return true;
    }));
  });
  while (!scanning.load()) std::this_thread::yield();
  std::thread blocked_reader([&] {
    uint64_t value = 0;
    size_t size = 0;
    assert(store.Get("stress/shared/00", 0, &value, sizeof(value), &size));
  });
  fortis::storage::KvActivityStats sample;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  do {
    store.ActivityStats(sample);
    if (sample.lock_waiters) break;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  const uint32_t waiting = sample.lock_waiters;
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  release_scan.store(true);
  scanner.join();
  blocked_reader.join();
  store.ActivityStats(sample);
  assert(waiting == 1 && sample.lock_waiters == 0 && sample.peak_lock_waiters == 1);
  uint64_t blocked = 0;
  for (size_t i = 1; i < fortis::storage::KvActivityStats::kBuckets; ++i) blocked += sample.lock_wait[i];
  assert(blocked == 1);

  std::atomic<bool> failed{false};
  std::thread workers[6];
  for (unsigned writer = 0; writer < 2; ++writer) {
    workers[writer] = std::thread([&, writer] {
      for (unsigned i = 0; i < 300; ++i) {
        char key[48];
        snprintf(key, sizeof(key), "stress/writer/%u/%04u", writer, i);
        const uint64_t value = (static_cast<uint64_t>(writer) << 32) | i;
        const fortis::storage::KvMutation mutation{key, &value, sizeof(value), 0, false};
        if (!store.Write(&mutation, 1)) failed.store(true);
        snprintf(key, sizeof(key), "stress/shared/%02u", i % 16);
        const fortis::storage::KvMutation update{key, &value, sizeof(value), 0, false};
        if (!store.Write(&update, 1)) failed.store(true);
      }
    });
  }

  for (unsigned reader = 0; reader < 3; ++reader) {
    workers[reader + 2] = std::thread([&, reader] {
      (void)reader;
      for (unsigned i = 0; i < 300; ++i) {
        char key[32];
        snprintf(key, sizeof(key), "stress/shared/%02u", i % 16);
        uint64_t value = 0;
        size_t size = 0;
        if (!store.Get(key, 0, &value, sizeof(value), &size) ||
            size != sizeof(value)) failed.store(true);
        if (i % 10 == 0) {
          VisitState state;
          if (!store.Scan("stress/", nullptr, 24, 0, [&, visitor_state = &state](const char* key, const void* value, size_t size) { return (visit)(key, value, size, visitor_state); }) || !state.valid)
            failed.store(true);
          fortis::storage::KvPageResult page;
          state = {};
          if (!store.Page("stress/", nullptr, 16, 0, false, [&, visitor_state = &state](const char* key, const void* value, size_t size) { return (visit)(key, value, size, visitor_state); }, &page) ||
              !state.valid || page.scanned != 16)
            failed.store(true);
        }
        (void)store.Size();
      }
    });
  }

  workers[5] = std::thread([&] {
    for (unsigned i = 0; i < 160; ++i) {
      char key[32];
      snprintf(key, sizeof(key), "stress/expiry/%03u", i);
      write_value(store, key, i, 100);
      (void)store.Sweep(100, 16);
      if (i % 16 == 0 && !store.Compact()) failed.store(true);
    }
  });

  for (std::thread& worker : workers) worker.join();
  assert(!failed.load());
  assert(store.Compact());
  store.ActivityStats(sample);
  const auto phase_count = [&](fortis::storage::KvActivityStats::Phase phase) {
    uint64_t count = 0;
    for (const auto bucket : sample.phases[static_cast<size_t>(phase)]) count += bucket;
    return count;
  };
  using Phase = fortis::storage::KvActivityStats::Phase;
  assert(phase_count(Phase::SweepHold) == 160);
  assert(phase_count(Phase::CompactionHold) > 0);
  assert(phase_count(Phase::FileRead) > 0 && phase_count(Phase::FileSync) > 0);
  for (unsigned writer = 0; writer < 2; ++writer) {
    char key[48];
    snprintf(key, sizeof(key), "stress/writer/%u/%04u", writer, 299u);
    uint64_t value = 0;
    size_t size = 0;
    assert(store.Get(key, 0, &value, sizeof(value), &size));
    assert(size == sizeof(value));
  }
  assert(store.Size() == 616);
  store.Close();
  assert(store.Open(directory));
  uint64_t value = 0;
  size_t size = 0;
  assert(store.Get("stress/shared/03", 0, &value, sizeof(value), &size));
  assert(size == sizeof(value));
  store.Close();

  char path[256];
  snprintf(path, sizeof(path), "%s/fortis.kv", directory);
  assert(unlink(path) == 0);
  snprintf(path, sizeof(path), "%s/fortis.kv.lock", directory);
  assert(unlink(path) == 0);
  assert(rmdir(directory) == 0);
  puts("native KV concurrent reader/writer, sweep, compact, and reopen test passed");
}
