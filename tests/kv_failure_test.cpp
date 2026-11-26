// Logging is a no-throw effect even while failed-append recovery is being marked.
#include "log.h"
#include <new>
#include <stdexcept>
#include "../src/storage/kv_store.h"
#include "../src/storage/kv_expiry_sweeper.h"

#include <assert.h>
#include <chrono>
#include <initializer_list>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace fortis::storage;

static void expect_value(fortis::storage::KvStore& store, const char* key, const char* expected) {
  char value[64]{};
  size_t size = 0;
  assert(store.Get(key, 0, value, sizeof(value), &size));
  assert(size == strlen(expected) && memcmp(value, expected, size) == 0);
}

static void test_scan_allocation(fortis::storage::KvStore& store) {
  const uint32_t large_size = 1024 * 1024;
  auto* large = static_cast<char*>(malloc(large_size));
  assert(large);
  memset(large, 'x', large_size);
  const fortis::storage::KvMutation rows[] = {
      {"scan/index/1", "article", 7, 0, false},
      {"scan/index/2", nullptr, 0, 0, false},
      {"scan/index/3", large, large_size, 1, false},
      {"scan/large", large, large_size, 0, false},
  };
  assert(store.Write(rows, sizeof(rows) / sizeof(rows[0])));

  const auto visit_index = [](const char* key, const void* value, size_t size, void* context) {
    auto* count = static_cast<unsigned*>(context);
    ++*count;
    if (strcmp(key, "scan/index/1") == 0) {
      assert(size == 7 && memcmp(value, "article", size) == 0);
    } else {
      assert(strcmp(key, "scan/index/2") == 0 && size == 0);
    }
    return true;
  };
  const auto visit_large = [](const char* key, const void* value, size_t size, void* context) {
    assert(strcmp(key, "scan/large") == 0 && size == large_size);
    assert(memcmp(value, context, size) == 0);
    return true;
  };

  fortis::storage::KvPageResult page;
  assert(setenv("FORTIS_KV_TEST_FAIL", "scan_allocate", 1) == 0);
  unsigned count = 0;
  assert(store.Scan("scan/index/", nullptr, 4, 1, [&, visitor_state = &count](const char* key, const void* value, size_t size) { return (visit_index)(key, value, size, visitor_state); }, false));
  assert(count == 2);
  count = 0;
  assert(store.Page("scan/index/", nullptr, 4, 1, true, [&, visitor_state = &count](const char* key, const void* value, size_t size) { return (visit_index)(key, value, size, visitor_state); }, &page));
  assert(count == 2 && page.scanned == 3 && page.done);
  assert(!store.Page("scan/large", nullptr, 1, 1, false, [&, visitor_state = large](const char* key, const void* value, size_t size) { return (visit_large)(key, value, size, visitor_state); }, &page));
  assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
  assert(store.Page("scan/large", nullptr, 1, 1, false, [&, visitor_state = large](const char* key, const void* value, size_t size) { return (visit_large)(key, value, size, visitor_state); }, &page));
  free(large);
}

static void test_recovery(fortis::storage::KvStore& store, const char* directory) {
  const fortis::storage::KvMutation invalid{"invalid", nullptr, 1, 0, false};
  assert(!store.Write(&invalid, 1));
  expect_value(store, "ack", "first");

  const fortis::storage::KvMutation pending{"recovery", "durable", 7, 0, false};
  assert(setenv("FORTIS_KV_TEST_FAIL", "write_allocate", 1) == 0);
  assert(!store.Write(&pending, 1));
  expect_value(store, "ack", "first");

  assert(setenv("FORTIS_KV_TEST_FAIL", "write_after_header", 1) == 0);
  assert(!store.Write(&pending, 1));
  char value[16]{};
  size_t size = 0;
  assert(store.GetStatus("ack", 0, value, sizeof(value), &size) ==
         fortis::storage::KvReadStatus::IoError);
  assert(!store.Write(&pending, 1));
  fortis::storage::KvStore competitor;
  assert(!competitor.Open(directory));

  assert(setenv("FORTIS_KV_TEST_FAIL", "recovery_replay", 1) == 0);
  assert(!store.RecoverIfNeeded(0));
  assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
  assert(!store.RecoverIfNeeded(999));
  assert(store.RecoverIfNeeded(2000));
  expect_value(store, "ack", "first");
  assert(!store.Get("recovery", 0, value, sizeof(value), &size));

  assert(setenv("FORTIS_KV_TEST_FAIL", "write_after_fsync", 1) == 0);
  assert(!store.Write(&pending, 1));
  assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
  assert(store.RecoverIfNeeded(3000));
  expect_value(store, "recovery", "durable");
  expect_value(store, "ack", "first");

  const fortis::storage::KvMutation failed_sync{"failed-sync", "discard", 7, 0, false};
  assert(setenv("FORTIS_KV_TEST_FAIL", "file_fsync", 1) == 0);
  assert(!store.Write(&failed_sync, 1));
  assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
  assert(setenv("FORTIS_KV_TEST_FAIL", "recovery_directory_fsync", 1) == 0);
  assert(!store.RecoverIfNeeded(3500));
  assert(store.GetStatus("ack", 0, value, sizeof(value), &size) ==
         fortis::storage::KvReadStatus::IoError);
  assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
  assert(store.RecoverIfNeeded(5500));
  assert(store.GetStatus(failed_sync.key, 0, value, sizeof(value), &size) ==
         fortis::storage::KvReadStatus::Missing);
  expect_value(store, "recovery", "durable");
  expect_value(store, "ack", "first");

  assert(setenv("FORTIS_KV_TEST_FAIL", "file_fsync", 1) == 0);
  assert(!store.Write(&failed_sync, 1));
  assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
  store.Close();
  assert(store.Open(directory));
  assert(store.GetStatus(failed_sync.key, 0, value, sizeof(value), &size) ==
         fortis::storage::KvReadStatus::Missing);
  expect_value(store, "ack", "first");

  char log[256];
  snprintf(log, sizeof(log), "%s/fortis.kv", directory);
  struct stat before {}, after {};
  assert(stat(log, &before) == 0);
  assert(setenv("FORTIS_KV_TEST_FAIL", "write_body_no_space", 1) == 0);
  assert(!store.Write(&failed_sync, 1));
  assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
  assert(store.RecoverIfNeeded(6000));
  assert(stat(log, &after) == 0 && before.st_ino == after.st_ino &&
         before.st_size == after.st_size);
  expect_value(store, "ack", "first");

  const fortis::storage::KvMutation second{"second-recovery", "complete", 8, 0, false};
  assert(setenv("FORTIS_KV_TEST_FAIL", "write_before_fsync", 1) == 0);
  assert(!store.Write(&second, 1));
  assert(setenv("FORTIS_KV_TEST_FAIL", "recovery_replay", 1) == 0);
  assert(!store.RecoverIfNeeded(6500));
  assert(!store.RecoverIfNeeded(8500));
  assert(!store.RecoverIfNeeded(11500));
  assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
  assert(!store.RecoverIfNeeded(20000));
  assert(!competitor.Open(directory));
  // Reopen resumes recovery from the durable rollback marker.
  assert(store.Open(directory));
  assert(store.GetStatus(second.key, 0, value, sizeof(value), &size) ==
         fortis::storage::KvReadStatus::Missing);
}

static void test_automatic_recovery(fortis::storage::KvStore& store) {
  const fortis::storage::KvMutation pending{"automatic-recovery", "durable", 7, 0, false};
  assert(setenv("FORTIS_KV_TEST_FAIL", "write_after_fsync", 1) == 0);
  assert(!store.Write(&pending, 1));
  assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
  KvExpirySweeper sweeper;
  // Recovery has a shorter cadence than ordinary expiry maintenance.
  assert(sweeper.Start(&store, 30000));
  const auto timeout = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  char value[16]{};
  size_t size = 0;
  while (!store.Get(pending.key, 0, value, sizeof(value), &size) &&
         std::chrono::steady_clock::now() < timeout)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  sweeper.Stop();
  assert(sweeper.ticks() == 0);
  expect_value(store, pending.key, "durable");
}

static void test_append_crash_boundaries(const char* directory) {
  for (const char* point : {"append_before_marker_directory_sync",
                            "append_failed_before_recovery",
                            "append_synced_before_marker_clear"}) {
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
      fortis::storage::KvStore store;
      assert(store.Open(directory));
      assert(setenv("FORTIS_KV_TEST_KILL", point, 1) == 0);
      if (strcmp(point, "append_failed_before_recovery") == 0)
        assert(setenv("FORTIS_KV_TEST_FAIL", "file_fsync", 1) == 0);
      const fortis::storage::KvMutation pending{"crashed", "discard", 7, 0, false};
      store.Write(&pending, 1);
      _exit(1);
    }

    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    fortis::storage::KvStore recovered;
    assert(recovered.Open(directory));
    expect_value(recovered, "ack", "first");
    char value[8]{}; size_t size = 0;
    assert(recovered.GetStatus("crashed", 0, value, sizeof(value), &size) ==
           fortis::storage::KvReadStatus::Missing);
    const fortis::storage::KvMutation next{"post-crash", "kept", 4, 0, false};
    assert(recovered.Write(&next, 1));
    recovered.Close();
    assert(recovered.Open(directory));
    expect_value(recovered, "post-crash", "kept");
  }
}

static void test_throwing_logger_recovery(fortis::storage::KvStore& store, const char* directory) {
  struct Sink final : fortis::base::LogSink {
    int kind = 0;
    int calls = 0;
    bool Write(const fortis::base::LogRecord&) override {
      ++calls;
      if (kind == 0) throw std::bad_alloc();
      if (kind == 1) throw std::runtime_error("fintel_dev_private");
      throw 7;
    }
  } sink;
  for (int kind = 0; kind < 3; ++kind) {
    sink.kind = kind;
    auto* previous = fortis::base::SetLogSink(&sink);
    const fortis::storage::KvMutation refused{"logger-uncertain", "partial", 7, 0, false};
    assert(setenv("FORTIS_KV_TEST_FAIL", "write_after_header", 1) == 0);
    assert(!store.Write(&refused, 1));
    assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
    char value[16];
    size_t size = 0;
    assert(store.GetStatus("ack", 0, value, sizeof(value), &size) == fortis::storage::KvReadStatus::IoError);
    assert(sink.calls > kind);
    fortis::base::SetLogSink(previous);
    store.Close();
    assert(store.Open(directory));
    expect_value(store, "ack", "first");
    assert(store.GetStatus(refused.key, 0, value, sizeof(value), &size) == fortis::storage::KvReadStatus::Missing);
    const fortis::storage::KvMutation accepted{"logger-next", "kept", 4, 0, false};
    assert(store.Write(&accepted, 1));
    expect_value(store, "logger-next", "kept");
  }
}

int main() {
  char directory[] = "/tmp/fortis-kv-failure-XXXXXX";
  assert(mkdtemp(directory));
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    const fortis::storage::KvMutation acknowledged{"ack", "first", 5, 0, false};
    assert(store.Write(&acknowledged, 1));

    const fortis::storage::KvMutation uncertain{"uncertain", "possible", 8, 0, false};
    assert(setenv("FORTIS_KV_TEST_FAIL", "write_after_header", 1) == 0);
    assert(!store.Write(&uncertain, 1));
    assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
  }
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    expect_value(store, "ack", "first");
    const fortis::storage::KvMutation second{"ack2", "second", 6, 0, false};
    assert(store.Write(&second, 1));
    const fortis::storage::KvMutation uncertain{"uncertain2", "durable", 7, 0, false};
    assert(setenv("FORTIS_KV_TEST_FAIL", "write_after_fsync", 1) == 0);
    assert(!store.Write(&uncertain, 1));
    assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
  }
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    const fortis::storage::KvMutation uncertain{"uncertain3", "buffered", 8, 0, false};
    assert(setenv("FORTIS_KV_TEST_FAIL", "write_before_fsync", 1) == 0);
    assert(!store.Write(&uncertain, 1));
    assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
  }

  // Every pre-rename compaction fault leaves the live log authoritative and
  // the same store instance writable; the temporary file is removed.
  char temporary[256];
  snprintf(temporary, sizeof(temporary), "%s/fortis.kv.compact", directory);
  const char* compact_stages[] = {
      "compact_copy_write", "compact_before_fsync",
      "compact_after_file_fsync",
  };
  unsigned round = 0;
  for (const char* stage : compact_stages) {
    {
      fortis::storage::KvStore store;
      assert(store.Open(directory));
      expect_value(store, "ack", "first");
      assert(setenv("FORTIS_KV_TEST_FAIL", stage, 1) == 0);
      assert(!store.Compact());
      assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
      assert(access(temporary, F_OK) != 0);
      char key[32];
      snprintf(key, sizeof(key), "after-%u", round++);
      const fortis::storage::KvMutation next{key, "kept", 4, 0, false};
      assert(store.Write(&next, 1));
      expect_value(store, key, "kept");
      fortis::storage::KvCompactionStats stats;
      store.CompactionStats(&stats);
      assert(stats.runs_failed == 1 && stats.phase == fortis::storage::KvCompactionPhase::Idle);
    }
    {
      fortis::storage::KvStore recovered;
      assert(recovered.Open(directory));
      expect_value(recovered, "ack", "first");
      expect_value(recovered, "ack2", "second");
      char key[32];
      snprintf(key, sizeof(key), "after-%u", round - 1);
      expect_value(recovered, key, "kept");
    }
  }
  // A failed directory fsync after the rename commits the swap, but writes
  // are refused until a directory fsync succeeds.
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    assert(setenv("FORTIS_KV_TEST_FAIL", "compact_after_rename", 1) == 0);
    assert(store.Compact());
    assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
    fortis::storage::KvCompactionStats stats;
    store.CompactionStats(&stats);
    assert(stats.directory_sync_pending && stats.directory_sync_failures == 1);
    const fortis::storage::KvMutation refused{"refused", "no", 2, 0, false};
    assert(setenv("FORTIS_KV_TEST_FAIL", "directory_sync_retry", 1) == 0);
    assert(!store.Write(&refused, 1));
    assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
    expect_value(store, "ack", "first");
    const fortis::storage::KvMutation accepted{"accepted", "yes", 3, 0, false};
    assert(store.Write(&accepted, 1));
    store.CompactionStats(&stats);
    assert(!stats.directory_sync_pending);
  }
  {
    fortis::storage::KvStore recovered;
    assert(recovered.Open(directory));
    expect_value(recovered, "ack", "first");
    expect_value(recovered, "accepted", "yes");
    char value[8]; size_t size = 0;
    assert(!recovered.Get("refused", 0, value, sizeof(value), &size));
    test_scan_allocation(recovered);
    test_recovery(recovered, directory);
    test_automatic_recovery(recovered);
    test_throwing_logger_recovery(recovered, directory);
  }

  test_append_crash_boundaries(directory);

  char path[256];
  snprintf(path, sizeof(path), "%s/fortis.kv", directory);
  assert(unlink(path) == 0);
  snprintf(path, sizeof(path), "%s/fortis.kv.lock", directory);
  assert(unlink(path) == 0);
  assert(rmdir(directory) == 0);
  puts("native KV deterministic write, scan and compaction failure test passed");
}
