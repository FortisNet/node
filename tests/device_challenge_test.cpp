#include "../src/auth/device_challenge.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main() {
  char directory[] = "/tmp/fortis-challenge-XXXXXX";
  assert(mkdtemp(directory));
  char challenge[44];
  memset(challenge, 'A', 43); challenge[43] = '\0';
  constexpr int64_t now = 1700000000000LL;
  {
    fortis::storage::KvStore kv;
    assert(kv.Open(directory));
    assert(fortis::auth::PersistDeviceChallenge(&kv, challenge, now));
    assert(!fortis::auth::PersistDeviceChallenge(&kv, "bad", now));
  }
  {
    fortis::storage::KvStore kv;
    assert(kv.Open(directory));
    char value[2]{}; size_t size = 0;
    const char key[] = "attest:challenge:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    assert(kv.GetStatus(key, now + 299999, value, sizeof(value), &size) ==
           fortis::storage::KvReadStatus::Found);
    assert(size == 1 && value[0] == '1');
    assert(kv.GetStatus(key, now + 300000, value, sizeof(value), &size) ==
           fortis::storage::KvReadStatus::Missing);
  }
  char path[256];
  snprintf(path, sizeof(path), "%s/fortis.kv", directory);
  assert(unlink(path) == 0);
  snprintf(path, sizeof(path), "%s/fortis.kv.lock", directory);
  assert(unlink(path) == 0 && rmdir(directory) == 0);
  puts("device challenge persistence and expiry test passed");
}
