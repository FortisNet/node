#include "../src/storage/kv_runtime.h"

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main() {
  char directory[] = "/tmp/fortis-quota-XXXXXX";
  assert(mkdtemp(directory));
  assert(setenv("FORTIS_KV_DIR", directory, 1) == 0);

  uint64_t value = 0;
  assert(fortis::storage::RuntimeKvIncrementWindow("q:ip:127.0.0.1:100", 6000000, 60000, &value));
  assert(value == 1);
  assert(fortis::storage::RuntimeKvIncrementWindow("q:ip:127.0.0.1:100", 6000001, 60000, &value));
  assert(value == 2);
  assert(fortis::storage::RuntimeKvIncrementByWindow("q:dev:device:100", 5, 6000000, 60000, &value));
  assert(value == 5);
  assert(fortis::storage::RuntimeKvIncrementWindow("q:ip:127.0.0.1:101", 6060000, 60000, &value));
  assert(value == 1);
  assert(fortis::storage::RuntimeKvIncrementWindow("q:ip:127.0.0.1:100", 6120000, 60000, &value));
  assert(value == 1);

  // Exercise 1,000 device subjects plus 1,000 IP subjects. Natural hash
  // collisions must keep distinct full keys and counters.
  char device_key[64], ip_key[64];
  for (unsigned i = 0; i < 1000; ++i) {
    snprintf(device_key, sizeof(device_key), "q:dev:device-%04u:100", i);
    snprintf(ip_key, sizeof(ip_key), "q:ip:192.0.2.%u:100", i);
    assert(fortis::storage::RuntimeKvIncrementWindow(device_key, 7000000, 60000, &value) && value == 1);
    assert(fortis::storage::RuntimeKvIncrementWindow(ip_key, 7000000, 60000, &value) && value == 1);
  }
  for (unsigned i = 0; i < 1000; ++i) {
    snprintf(device_key, sizeof(device_key), "q:dev:device-%04u:100", i);
    snprintf(ip_key, sizeof(ip_key), "q:ip:192.0.2.%u:100", i);
    assert(fortis::storage::RuntimeKvIncrementWindow(device_key, 7000001, 60000, &value) && value == 2);
    assert(fortis::storage::RuntimeKvIncrementWindow(ip_key, 7000001, 60000, &value) && value == 2);
  }

  // Fill until the bounded probe budget is exhausted. Reject the new subject
  // while preserving existing device and IP counters.
  char fill_key[64]; bool overflowed = false;
  for (unsigned i = 0; i < 20000; ++i) {
    snprintf(fill_key, sizeof(fill_key), "q:fill:%05u", i);
    assert(fortis::storage::RuntimeKvIncrementWindow(fill_key, 7000000, 60000, &value));
    if (value == UINT64_MAX) {
      overflowed = true;
      break;
    }
  }
  assert(overflowed);
  snprintf(device_key, sizeof(device_key), "q:dev:device-%04u:100", 0U);
  assert(fortis::storage::RuntimeKvIncrementWindow(device_key, 7000002, 60000, &value) && value == 3);

  // A minute change clears the open-addressed table, so tombstones cannot
  // accumulate into a full-table scan over many windows.
  char epoch_key[64];
  for (unsigned epoch = 0; epoch < 20; ++epoch) {
    const int64_t minute = 200 + epoch;
    const int64_t now = minute * 60000;
    for (unsigned subject = 0; subject < 1000; ++subject) {
      snprintf(epoch_key, sizeof(epoch_key), "q:dev:e%02u-device-%04u:%lld",
          epoch, subject, static_cast<long long>(minute));
      assert(fortis::storage::RuntimeKvIncrementWindow(epoch_key, now, 60000, &value) && value == 1);
    }
  }

  // Characterize the existing quota policy at the replica rollout target.
  // Overflow refuses new subjects without evicting an admitted device.
  constexpr unsigned kSubjects = 100000;
  constexpr int64_t kLoadTime = 90000000;
  unsigned admitted = 0;
  for (unsigned subject = 0; subject < kSubjects; ++subject) {
    snprintf(device_key, sizeof(device_key), "q:dev:load-%06u:1500", subject);
    assert(fortis::storage::RuntimeKvIncrementWindow(device_key, kLoadTime, 60000, &value));
    assert(value == 1 || value == UINT64_MAX);
    admitted += value == 1;
  }
  assert(admitted > 0 && admitted <= 16384 && admitted < kSubjects);
  assert(fortis::storage::RuntimeKvIncrementWindow("q:dev:load-000000:1500", kLoadTime, 60000, &value));
  assert(value == 2);
  printf("single quota window: %u admitted, %u refused of %u subjects\n",
         admitted, kSubjects - admitted, kSubjects);

  DIR* contents = opendir(directory);
  assert(contents);
  struct dirent* entry = nullptr;
  while ((entry = readdir(contents)) != nullptr)
    assert(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0);
  closedir(contents);
  assert(rmdir(directory) == 0);
  puts("bounded in-memory quota window test passed");
}
