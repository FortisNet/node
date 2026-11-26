#include "../src/storage/kv_runtime.h"

#include <assert.h>
#include <atomic>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

int main() {
  char parent[] = "/tmp/fortis-runtime-concurrent-XXXXXX";
  assert(mkdtemp(parent));
  char directory[256];
  snprintf(directory, sizeof(directory), "%s/retry", parent);

  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    std::atomic<bool> failed{false};
    std::thread setters[8];
    for (std::thread& setter : setters)
      setter = std::thread([&] { fortis::storage::RuntimeKvSetDirectory(directory); });
    for (std::thread& setter : setters) setter.join();

    std::thread failed_openers[8];
    for (std::thread& opener : failed_openers) {
      opener = std::thread([&] {
        if (fortis::storage::RuntimeKvStore() != nullptr) failed.store(true);
      });
    }
    for (std::thread& opener : failed_openers) opener.join();

    if (mkdir(directory, 0700) != 0) _exit(2);
    std::thread retry_setters[8];
    for (std::thread& setter : retry_setters)
      setter = std::thread([&] { fortis::storage::RuntimeKvSetDirectory(directory); });
    for (std::thread& setter : retry_setters) setter.join();

    std::atomic<fortis::storage::KvStore*> shared_store{nullptr};
    std::thread openers[16];
    for (std::thread& opener : openers) {
      opener = std::thread([&] {
        fortis::storage::KvStore* result = fortis::storage::RuntimeKvStore();
        if (!result) {
          failed.store(true);
          return;
        }
        fortis::storage::KvStore* expected = nullptr;
        if (!shared_store.compare_exchange_strong(expected, result) && expected != result)
          failed.store(true);
      });
    }
    for (std::thread& opener : openers) opener.join();

    constexpr unsigned kWorkers = 16;
    constexpr unsigned kIncrements = 200;
    std::atomic<uint64_t> max_value{0};
    std::thread counters[kWorkers];
    for (std::thread& counter : counters) {
      counter = std::thread([&] {
        for (unsigned i = 0; i < kIncrements; ++i) {
          uint64_t value = 0;
          if (!fortis::storage::RuntimeKvIncrementWindow("concurrent:quota", 6000000,
                                                60000, &value)) {
            failed.store(true);
            return;
          }
          uint64_t seen = max_value.load();
          while (seen < value && !max_value.compare_exchange_weak(seen, value)) {}
        }
      });
    }
    for (std::thread& counter : counters) counter.join();
    _exit(failed.load() || max_value.load() != kWorkers * kIncrements ? 1 : 0);
  }

  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  char path[320];
  snprintf(path, sizeof(path), "%s/fortis.kv", directory);
  assert(unlink(path) == 0);
  snprintf(path, sizeof(path), "%s/fortis.kv.lock", directory);
  assert(unlink(path) == 0);
  assert(rmdir(directory) == 0);
  assert(rmdir(parent) == 0);
  puts("runtime KV concurrent lazy-open retry and quota-counter test passed");
}
