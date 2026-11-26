#include "../src/storage/kv_store.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

struct Seen { size_t count; char last[64]; };
static bool visit(const char* key, const void*, size_t, Seen* seen) {
  if (seen->count) assert(strcmp(seen->last, key) < 0);
  snprintf(seen->last, sizeof(seen->last), "%s", key);
  ++seen->count;
  return true;
}
static bool visit_reverse(const char* key, const void*, size_t, Seen* seen) {
  if (seen->count) assert(strcmp(seen->last, key) > 0);
  snprintf(seen->last, sizeof(seen->last), "%s", key);
  ++seen->count;
  return true;
}

int main() {
  // The native key bound is unchanged: 1024 was already the
  // durable limit. Reject an oversized batch atomically, keep the boundary
  // record byte-for-byte, and prove it survives compaction and reopening.
  char bounds_dir[] = "/tmp/fortis-kv-bounds-XXXXXX";
  assert(mkdtemp(bounds_dir));
  char boundary[1025], oversized[1026];
  memset(boundary, 'k', 1024);
  boundary[1024] = '\0';
  memcpy(oversized, boundary, 1024);
  oversized[1024] = 'x';
  oversized[1025] = '\0';
  for (int reopen = 0; reopen < 2; ++reopen) {
    fortis::storage::KvStore store;
    assert(store.Open(bounds_dir));
    if (!reopen) {
      const fortis::storage::KvMutation row{boundary, "legacy-json", 11, 0, false};
      assert(store.Write(&row, 1));
    }
    const fortis::storage::KvMutation rejected[] = {
        {boundary, "changed", 7, 0, false}, {oversized, "invalid", 7, 0, false}};
    assert(!store.Write(rejected, 2));
    char value[32]{};
    size_t size = 0;
    assert(store.Get(boundary, 0, value, sizeof(value), &size));
    assert(size == 11 && memcmp(value, "legacy-json", 11) == 0 && store.Size() == 1);
    assert(store.Compact());
  }
  char bounds_path[256];
  snprintf(bounds_path, sizeof(bounds_path), "%s/fortis.kv", bounds_dir);
  assert(unlink(bounds_path) == 0);
  snprintf(bounds_path, sizeof(bounds_path), "%s/fortis.kv.lock", bounds_dir);
  assert(unlink(bounds_path) == 0);
  assert(rmdir(bounds_dir) == 0);

  // Separate instances and processes cannot open the same directory for
  // recovery or writes while its current owner is alive.
  char ownership_dir[] = "/tmp/fortis-kv-owner-XXXXXX";
  assert(mkdtemp(ownership_dir));
  {
    fortis::storage::KvStore owner, contender;
    assert(owner.Open(ownership_dir));
    assert(!contender.Open(ownership_dir));
  }
  int ready[2];
  assert(pipe(ready) == 0);
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    close(ready[0]);
    fortis::storage::KvStore child_store;
    const char owned = child_store.Open(ownership_dir) ? 'Y' : 'N';
    if (write(ready[1], &owned, 1) != 1) _exit(2);
    if (owned == 'Y') for (;;) pause();
    _exit(1);
  }
  close(ready[1]);
  char owned = 0;
  assert(read(ready[0], &owned, 1) == 1 && owned == 'Y');
  close(ready[0]);
  {
    fortis::storage::KvStore contender;
    assert(!contender.Open(ownership_dir));
  }
  assert(kill(child, SIGKILL) == 0);
  int child_status = 0;
  assert(waitpid(child, &child_status, 0) == child && WIFSIGNALED(child_status));
  {
    fortis::storage::KvStore after_crash;
    assert(after_crash.Open(ownership_dir));
  }
  char lock_path[256], owner_data[256];
  snprintf(lock_path, sizeof(lock_path), "%s/fortis.kv.lock", ownership_dir);
  snprintf(owner_data, sizeof(owner_data), "%s/fortis.kv", ownership_dir);
  assert(unlink(owner_data) == 0);
  assert(unlink(lock_path) == 0);
  assert(rmdir(ownership_dir) == 0);

  char directory[] = "/tmp/fortis-kv-XXXXXX";
  assert(mkdtemp(directory));
  char path[256];
  snprintf(path, sizeof(path), "%s/fortis.kv", directory);
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    const fortis::storage::KvMutation initial[] = {
        {"wallet/1", "first", 5, 0, false},
        {"wallet/2", "second", 6, 200, false},
    };
    assert(store.Write(initial, 2));
    fortis::storage::KvDashboardStats dashboard;
    assert(store.TryDashboardStats(&dashboard) && dashboard.indexed_keys == 2);
    assert(dashboard.compaction.file_bytes > dashboard.compaction.live_bytes &&
           dashboard.compaction.live_bytes > 0 && !dashboard.recovery_pending);
    // A dashboard read must skip a held store lock, even during a scan.
    assert(store.Scan("wallet/", nullptr, 1, 100, [ctx = &store](const char*, const void*, size_t) {
      fortis::storage::KvDashboardStats busy;
      assert(!static_cast<fortis::storage::KvStore*>(ctx)->TryDashboardStats(&busy));
      return true;
    }));
    char value[32]; size_t length = 0;
    assert(store.Get("wallet/1", 100, value, sizeof(value), &length));
    assert(length == 5 && memcmp(value, "first", 5) == 0);
    assert(store.GetStatus("wallet/1", 100, value, 1, &length) ==
           fortis::storage::KvReadStatus::BufferTooSmall);
    assert(store.GetStatus("missing", 100, value, sizeof(value), &length) ==
           fortis::storage::KvReadStatus::Missing);
    assert(store.Get("wallet/2", 100, value, sizeof(value), &length));
    assert(!store.Get("wallet/2", 200, value, sizeof(value), &length));
    Seen seen{};
    assert(store.Scan("wallet/", nullptr, 10, 100, [&, visitor_state = &seen](const char* key, const void* value, size_t size) { return (visit)(key, value, size, visitor_state); }) && seen.count == 2);
    seen = {};
    assert(store.Scan("wallet/", "wallet/1", 10, 100, [&, visitor_state = &seen](const char* key, const void* value, size_t size) { return (visit)(key, value, size, visitor_state); }) && seen.count == 1);
    seen = {};
    assert(store.Scan("wallet/", nullptr, 10, 100, [&, visitor_state = &seen](const char* key, const void* value, size_t size) { return (visit_reverse)(key, value, size, visitor_state); }, true));
    assert(seen.count == 2 && strcmp(seen.last, "wallet/1") == 0);
    fortis::storage::KvPageResult page;
    unsigned records = 0;
    const bool records_ok = store.Page("wallet/", nullptr, 10, 100, false, nullptr, &page, [context = &records](const char* key, const void*, size_t, int64_t expiry) {
          assert(expiry == (strcmp(key, "wallet/2") == 0 ? 200 : 0));
          ++*static_cast<unsigned*>(context);
          return true;
        });
    assert(records_ok);
    assert(records == 2);
    seen = {};
    assert(store.Page("wallet/", nullptr, 1, 200, false, [&, visitor_state = &seen](const char* key, const void* value, size_t size) { return (visit)(key, value, size, visitor_state); }, &page));
    assert(seen.count == 1 && page.scanned == 1 && !page.done &&
           strcmp(page.next_cursor, "wallet/1") == 0);
    seen = {};
    assert(store.Page("wallet/", page.next_cursor, 1, 200, false, [&, visitor_state = &seen](const char* key, const void* value, size_t size) { return (visit)(key, value, size, visitor_state); }, &page));
    assert(seen.count == 0 && page.scanned == 1 && !page.done &&
           strcmp(page.next_cursor, "wallet/2") == 0);
    seen = {};
    assert(store.Page("wallet/", page.next_cursor, 1, 200, false, [&, visitor_state = &seen](const char* key, const void* value, size_t size) { return (visit)(key, value, size, visitor_state); }, &page));
    assert(seen.count == 0 && page.scanned == 0 && page.done);
    seen = {};
    assert(store.Page("wallet/", nullptr, 2, 100, true, [&, visitor_state = &seen](const char* key, const void* value, size_t size) { return (visit_reverse)(key, value, size, visitor_state); }, &page));
    assert(seen.count == 2 && page.scanned == 2 && !page.done &&
           strcmp(page.next_cursor, "wallet/1") == 0);
    // A page boundary may be longer than a stored key. A reverse scan must
    // still include the key that equals its first 1024 bytes.
    char long_key[1025];
    memcpy(long_key, "wallet/", 7);
    memset(long_key + 7, 'a', 1017);
    long_key[1024] = '\0';
    const fortis::storage::KvMutation long_row = {long_key, "x", 1, 0, false};
    assert(store.Write(&long_row, 1));
    char long_boundary[1026];
    memcpy(long_boundary, long_key, 1024);
    long_boundary[1024] = 'z';
    long_boundary[1025] = '\0';
    seen = {};
    assert(store.Page("wallet/", long_boundary, 4, 100, true, [&, visitor_state = &seen](const char* key, const void* value, size_t size) { return (visit_reverse)(key, value, size, visitor_state); }, &page));
    assert(seen.count == 3 && strcmp(seen.last, "wallet/1") == 0);
    seen = {};
    assert(store.Page("wallet/", long_boundary, 4, 100, false, [&, visitor_state = &seen](const char* key, const void* value, size_t size) { return (visit)(key, value, size, visitor_state); }, &page));
    assert(seen.count == 0);
    const fortis::storage::KvMutation remove_long_row = {long_key, nullptr, 0, 0, true};
    assert(store.Write(&remove_long_row, 1));
    const fortis::storage::KvMutation changes[] = {
        {"wallet/1", "replaced", 8, 0, false},
        {"wallet/2", nullptr, 0, 0, true},
    };
    assert(store.Write(changes, 2));
    assert(store.Size() == 1);
    assert(store.Compact());
  }
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    char value[32]; size_t length = 0;
    assert(store.Get("wallet/1", 300, value, sizeof(value), &length));
    assert(length == 8 && memcmp(value, "replaced", 8) == 0);
    assert(!store.Get("wallet/2", 300, value, sizeof(value), &length));
    const fortis::storage::KvMutation expiry{"expiring", "gone", 4, 50, false};
    assert(store.Write(&expiry, 1));
    assert(store.Sweep(50, 1) == 1);
    assert(store.Size() == 1);
  }
  // A crash during frame append leaves a partial tail. Recovery discards it.
  {
    const int fd = open(path, O_WRONLY | O_APPEND);
    assert(fd >= 0);
    assert(write(fd, "FKV1\x20\x00", 6) == 6);
    assert(close(fd) == 0);
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    assert(store.Size() == 1);
    const fortis::storage::KvMutation next{"after-recovery", "ok", 2, 0, false};
    assert(store.Write(&next, 1));
  }
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    char value[32]; size_t length = 0;
    assert(store.Get("after-recovery", 0, value, sizeof(value), &length));
    assert(length == 2 && memcmp(value, "ok", 2) == 0);
    char keys[600][32];
    fortis::storage::KvMutation insertions[600];
    for (unsigned i = 0; i < 600; ++i) {
      const unsigned number = (i * 317) % 600;
      snprintf(keys[i], sizeof(keys[i]), "bulk/%04u", number);
      insertions[i] = {keys[i], "x", 1, 0, false};
    }
    assert(store.Write(insertions, 600));
    fortis::storage::KvMutation removals[200];
    for (unsigned i = 0; i < 200; ++i)
      removals[i] = {keys[i * 3], nullptr, 0, 0, true};
    assert(store.Write(removals, 200));
    Seen seen{};
    assert(store.Scan("bulk/", nullptr, 1000, 0, [&, visitor_state = &seen](const char* key, const void* value, size_t size) { return (visit)(key, value, size, visitor_state); }));
    assert(seen.count == 400);
    assert(store.Compact());
  }
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    Seen seen{};
    assert(store.Scan("bulk/", nullptr, 1000, 0, [&, visitor_state = &seen](const char* key, const void* value, size_t size) { return (visit)(key, value, size, visitor_state); }));
    assert(seen.count == 400);
  }
  // A short suffix that cannot be the start of an FKV1 header is corruption.
  {
    const int fd = open(path, O_RDWR | O_APPEND);
    assert(fd >= 0);
    const off_t valid_end = lseek(fd, 0, SEEK_END);
    assert(valid_end >= 0);
    assert(write(fd, "BAD", 3) == 3);
    assert(close(fd) == 0);
    fortis::storage::KvStore store;
    assert(!store.Open(directory));
    const int repair_fd = open(path, O_RDWR);
    assert(repair_fd >= 0);
    assert(ftruncate(repair_fd, valid_end) == 0);
    assert(close(repair_fd) == 0);
  }
  // Complete, corrupted frames fail closed.
  {
    const int fd = open(path, O_RDWR);
    assert(fd >= 0);
    char byte = 0;
    assert(pread(fd, &byte, 1, 12) == 1);
    byte ^= 1;
    assert(pwrite(fd, &byte, 1, 12) == 1);
    assert(close(fd) == 0);
    fortis::storage::KvStore store;
    assert(!store.Open(directory));
  }
  assert(unlink(path) == 0);
  snprintf(path, sizeof(path), "%s/fortis.kv.lock", directory);
  assert(unlink(path) == 0);
  assert(rmdir(directory) == 0);
  puts("native KV transaction/recovery test passed");
}
