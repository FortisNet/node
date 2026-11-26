// The scenarios exercise device operations through a fixture boundary so a
// future backend can run the same contract against its own adapter.
#include "../src/auth/device_auth.h"
#include "../src/storage/kv_store.h"

#include <assert.h>
#include <atomic>
#include <initializer_list>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace fortis::auth;
using namespace fortis::storage;

namespace {
using namespace fortis;
constexpr int64_t kNow = 1700000000000LL;
constexpr int64_t kTtl = 600000;
bool group_commit = false;
bool parallel_point_reads = false;
KvDurability durability = KvDurability::RollbackMarker;

class LegacyDeviceFixture {
 public:
  explicit LegacyDeviceFixture(const char* path)
      : path_(path), kv_(group_commit, parallel_point_reads, durability), auth_(&kv_) {
    assert(kv_.Open(path_));
  }

  void Challenge(const char* challenge, int64_t expiry) {
    char key[128];
    assert(DeviceChallengeKey(challenge, strlen(challenge), key, sizeof(key)));
    const KvMutation marker{key, "1", 1, expiry, false};
    assert(kv_.Write(&marker, 1));
  }

  DeviceEnrollResult Enroll(const char* challenge, int64_t time, DeviceCredentials& output) {
    return auth_.EnrollDevelopmentWithChallenge(challenge, strlen(challenge), DevicePlatform::Ios,
                                                "contract", 8, time, kTtl, &output);
  }

  DeviceAuthResult Authorize(const char* bearer, int64_t time, DeviceRecord& row) {
    return auth_.AuthenticateSnapshot(bearer, time, false, &row);
  }

  DeviceRotateResult Rotate(const DeviceRecord& row, int64_t time, DeviceCredentials& output) {
    return auth_.Rotate(row, time, kTtl, &output);
  }

  bool Revoke(const char* id, int64_t time) { return auth_.Revoke(id, time); }

  void Reopen() {
    kv_.Close();
    assert(kv_.Open(path_));
  }

 private:
  const char* path_;
  KvStore kv_;
  DeviceAuthStore auth_;
};

template <typename Fixture>
void RunContract(Fixture& store) {
  store.Challenge("expiry-boundary", kNow);
  DeviceCredentials rejected{};
  assert(store.Enroll("expiry-boundary", kNow, rejected) == DeviceEnrollResult::ChallengeUnavailable);

  store.Challenge("one-use", kNow + 60);
  DeviceEnrollResult enroll[2]{};
  DeviceCredentials enrolled[2]{};
  std::atomic<bool> go{false};
  auto enroll_once = [&](unsigned i) {
    while (!go.load()) std::this_thread::yield();
    enroll[i] = store.Enroll("one-use", kNow + 1, enrolled[i]);
  };
  std::thread first(enroll_once, 0), second(enroll_once, 1);
  go = true;
  first.join();
  second.join();
  const unsigned winner = enroll[0] == DeviceEnrollResult::Enrolled ? 0 : 1;
  assert(enroll[winner] == DeviceEnrollResult::Enrolled);
  assert(enroll[1 - winner] == DeviceEnrollResult::ChallengeUnavailable);

  // Losing a successful reply does not make a consumed challenge usable again.
  store.Reopen();
  assert(store.Enroll("one-use", kNow + 2, rejected) == DeviceEnrollResult::ChallengeUnavailable);
  DeviceRecord original{};
  assert(store.Authorize(enrolled[winner].token, kNow + 2, original) == DeviceAuthResult::Valid);

  DeviceRotateResult rotation[2]{};
  DeviceCredentials rotated[2]{};
  go = false;
  auto rotate_once = [&](unsigned i) {
    while (!go.load()) std::this_thread::yield();
    rotation[i] = store.Rotate(original, kNow + 3, rotated[i]);
  };
  std::thread left(rotate_once, 0), right(rotate_once, 1);
  go = true;
  left.join();
  right.join();
  const unsigned current = rotation[0] == DeviceRotateResult::Rotated ? 0 : 1;
  assert(rotation[current] == DeviceRotateResult::Rotated);
  assert(rotation[1 - current] == DeviceRotateResult::SessionExpired);
  store.Reopen();
  assert(store.Rotate(original, kNow + 4, rejected) == DeviceRotateResult::SessionExpired);
  assert(store.Authorize(enrolled[winner].token, kNow + 4, original) != DeviceAuthResult::Valid);

  DeviceRecord active{};
  assert(store.Authorize(rotated[current].token, kNow + 4, active) == DeviceAuthResult::Valid);
  assert(store.Authorize(rotated[current].token, rotated[current].expires_at_ms, active) ==
         DeviceAuthResult::SessionExpired);
  assert(store.Authorize(rotated[current].token, kNow + 5, active) == DeviceAuthResult::Valid);

  DeviceCredentials racing{};
  DeviceRotateResult race{};
  go = false;
  std::thread rotator([&] {
    while (!go.load()) std::this_thread::yield();
    race = store.Rotate(active, kNow + 6, racing);
  });
  go = true;
  assert(store.Revoke(active.id, kNow + 6));
  rotator.join();
  assert(race == DeviceRotateResult::Rotated || race == DeviceRotateResult::SessionExpired);
  assert(store.Authorize(rotated[current].token, kNow + 7, active) != DeviceAuthResult::Valid);
  if (race == DeviceRotateResult::Rotated)
    assert(store.Authorize(racing.token, kNow + 7, active) != DeviceAuthResult::Valid);
  store.Reopen();
  assert(store.Authorize(rotated[current].token, kNow + 8, active) != DeviceAuthResult::Valid);
  if (race == DeviceRotateResult::Rotated)
    assert(store.Authorize(racing.token, kNow + 8, active) != DeviceAuthResult::Valid);
}

void CrashContract(const char* directory, const char* stage, bool expected_commit) {
  {
    LegacyDeviceFixture store(directory);
    store.Challenge(stage, kNow + 60);
  }
  const pid_t child = fork();
  assert(child >= 0);
  if (!child) {
    LegacyDeviceFixture store(directory);
    assert(setenv("FORTIS_KV_TEST_KILL", stage, 1) == 0);
    DeviceCredentials credentials{};
    (void)store.Enroll(stage, kNow, credentials);
    _exit(99);
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  LegacyDeviceFixture store(directory);
  DeviceCredentials credentials{};
  const auto retry = store.Enroll(stage, kNow + 1, credentials);
  assert(retry == (expected_commit ? DeviceEnrollResult::ChallengeUnavailable :
                                    DeviceEnrollResult::Enrolled));
}

void ConditionalAbsenceContract(const char* directory) {
  KvStore store(group_commit, parallel_point_reads, durability);
  assert(store.Open(directory));
  const KvMutation initial[] = {{"contract/expected", "a", 1, 0, false},
                                {"contract/guard", "g", 1, kNow + 1, false}};
  assert(store.Write(initial, 2));
  const KvExpectation expected{"contract/expected", "a", 1};
  const char* absent[] = {"contract/guard"};
  const KvMutation updates[] = {{"contract/expected", "b", 1, 0, false},
                                {"contract/created", "v", 1, 0, false}};
  assert(store.CompareAndWrite(expected, absent, 1, kNow, updates, 2) ==
         KvCompareStatus::AbsentKeyPresent);
  char value = 0;
  size_t length = 0;
  assert(store.GetStatus("contract/created", kNow, &value, 1, &length) == KvReadStatus::Missing);
  assert(store.CompareAndWrite(expected, absent, 1, kNow + 1, updates, 2) ==
         KvCompareStatus::Committed);
  assert(store.CompareAndWrite(expected, absent, 1, kNow + 1, updates, 2) ==
         KvCompareStatus::ValueMismatch);
  store.Close();
  assert(store.Open(directory));
  assert(store.Get("contract/expected", kNow + 1, &value, 1, &length) && value == 'b');
  assert(store.Get("contract/created", kNow + 1, &value, 1, &length) && value == 'v');
}
void MultiRequestCrashContract(const char* directory, bool torn) {
  const char* first = torn ? "group/torn-a" : "group/acked-a";
  const char* second = torn ? "group/torn-b" : "group/acked-b";
  const pid_t child = fork();
  assert(child >= 0);
  if (!child) {
    KvStore store(true, parallel_point_reads, durability);
    assert(store.Open(directory));
    assert(setenv("FORTIS_KV_TEST_GROUP_FILL", "1", 1) == 0);
    if (torn) assert(setenv("FORTIS_KV_TEST_KILL", "write_after_header", 1) == 0);
    std::atomic<unsigned> ready{0};
    auto write = [&](const char* key) {
      const KvMutation mutation{key, "v", 1, 0, false};
      ++ready;
      while (ready < 2) std::this_thread::yield();
      assert(store.Write(&mutation, 1));
    };
    std::thread a(write, first), b(write, second);
    a.join();
    b.join();
    assert(store.WriteFrames() == 1);
    if (!torn) raise(SIGKILL);
    _exit(99);
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  KvStore store(false, parallel_point_reads, durability);
  assert(store.Open(directory));
  for (const char* key : {first, second}) {
    char value = 0;
    size_t length = 0;
    assert(store.GetStatus(key, kNow, &value, 1, &length) ==
           (torn ? KvReadStatus::Missing : KvReadStatus::Found));
    if (!torn) assert(length == 1 && value == 'v');
  }
}

void GroupConditionContract(const char* directory) {
  KvStore store(true, parallel_point_reads, durability);
  assert(store.Open(directory));
  const KvMutation seed{"group/once", "a", 1, 0, false};
  assert(store.Write(&seed, 1));
  const auto before = store.WriteFrames();
  assert(setenv("FORTIS_KV_TEST_GROUP_FILL", "1", 1) == 0);
  std::atomic<unsigned> ready{0};
  KvCompareStatus results[2]{};
  auto submit = [&](unsigned n) {
    const KvExpectation once{"group/once", "a", 1};
    const char* guards[] = {"group/consumed"};
    const KvMutation changes[] = {{"group/once", nullptr, 0, 0, true},
                                  {"group/consumed", "v", 1, 0, false}};
    ++ready;
    while (ready < 2) std::this_thread::yield();
    results[n] = store.CompareAndWrite(once, guards, 1, kNow, changes, 2);
  };
  std::thread first(submit, 0), second(submit, 1);
  first.join();
  second.join();
  assert(unsetenv("FORTIS_KV_TEST_GROUP_FILL") == 0);
  const unsigned winner = results[0] == KvCompareStatus::Committed ? 0 : 1;
  assert(results[winner] == KvCompareStatus::Committed);
  assert(results[1 - winner] == KvCompareStatus::MissingOrExpired);
  assert(store.WriteFrames() == before + 1);
}

void AmbiguousGroupContract(const char* directory) {
  KvStore store(true, parallel_point_reads, durability);
  assert(store.Open(directory));
  assert(setenv("FORTIS_KV_TEST_GROUP_FILL", "1", 1) == 0);
  assert(setenv("FORTIS_KV_TEST_FAIL", "write_after_fsync", 1) == 0);
  std::atomic<unsigned> ready{0};
  auto submit = [&](const char* key) {
    const KvMutation mutation{key, "v", 1, 0, false};
    ++ready;
    while (ready < 2) std::this_thread::yield();
    assert(!store.Write(&mutation, 1));
  };
  std::thread a(submit, "group/ambiguous-a"), b(submit, "group/ambiguous-b");
  a.join();
  b.join();
  assert(unsetenv("FORTIS_KV_TEST_GROUP_FILL") == 0);
  assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
  assert(store.WriteFrames() == 0 && store.CacheEpoch() == 0);
  char value = 0;
  size_t size = 0;
  assert(store.GetStatus("group/ambiguous-a", kNow, &value, 1, &size) == KvReadStatus::IoError);
  assert(store.RecoverIfNeeded(0));
  for (const char* key : {"group/ambiguous-a", "group/ambiguous-b"})
    assert(store.Get(key, kNow, &value, 1, &size) && size == 1 && value == 'v');
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--group-commit") == 0) group_commit = true;
    else if (strcmp(argv[i], "--parallel-point-reads") == 0) parallel_point_reads = true;
    else if (strcmp(argv[i], "--commit-slots") == 0) durability = KvDurability::CommitSlots;
    else assert(false);
  }
  char directory[] = "/tmp/fortis-device-contract-XXXXXX";
  assert(mkdtemp(directory));
  {
    LegacyDeviceFixture store(directory);
    RunContract(store);
  }
  CrashContract(directory, "write_after_header", false);
  CrashContract(directory, "write_acknowledged", true);
  ConditionalAbsenceContract(directory);
  if (group_commit) {
    GroupConditionContract(directory);
    MultiRequestCrashContract(directory, true);
    MultiRequestCrashContract(directory, false);
    AmbiguousGroupContract(directory);
  }
  char path[160];
  for (const char* file : {"fortis.kv", "fortis.kv.lock"}) {
    snprintf(path, sizeof(path), "%s/%s", directory, file);
    assert(unlink(path) == 0);
  }
  assert(rmdir(directory) == 0);
  puts("device transaction contract: expiry, replay, rotation, revocation and crash recovery passed");
}
