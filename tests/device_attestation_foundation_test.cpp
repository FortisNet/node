#include "test_support.h"
// KvStore::CompareAndWrite, the one-use
// development challenge and DeviceAuthStore::EnrollAttested.
//
// Covers outcomes and argument bounds, the exact expiry instant, simultaneous
// consumers, injected I/O failures, SIGKILL crash images, compaction runs
// around conditional frames, and attested records across Authenticate,
// Rotate, Revoke, restart and replay. Linked with the FORTIS_KV_TEST_FAILURES
// store object. The synthetic DeviceAttestationIssuer below exists only in
// this test binary; no production translation unit defines one.
#include "../src/auth/device_auth.h"
#include "../src/auth/device_challenge.h"
#include "../src/storage/kv_store.h"

#include <atomic>
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

using namespace fortis::auth;

namespace fortis::auth {
// Test-only stand-in for the Apple/Google verifiers (the production App
// Attest issuer lives in apple_app_attest.cpp, which this binary does not
// link).
class DeviceAttestationIssuer {
 public:
  static VerifiedDeviceAttestation Make(DeviceAttestation provider, DevicePlatform platform,
                                        const char* challenge, const char* app_version,
                                        size_t app_version_length, int64_t verified_at_ms,
                                        bool apple_evidence = true) {
    VerifiedDeviceAttestation proof;
    proof.provider_ = provider;
    proof.platform_ = platform;
    snprintf(proof.challenge_, sizeof(proof.challenge_), "%s", challenge);
    if (app_version) {
      memcpy(proof.app_version_, app_version, app_version_length);
      proof.app_version_length_ = static_cast<uint16_t>(app_version_length);
      proof.has_app_version_ = true;
    }
    proof.verified_at_ms_ = verified_at_ms;
    // An App Attest verdict carries its key association; the
    // stand-in derives a distinct key id from the challenge.
    if (provider == DeviceAttestation::AppleAppAttest && apple_evidence) {
      static const uint8_t kReceipt[] = {0x30, 0x80, 0x06, 0x09};
      for (size_t i = 0; i < 32; ++i) {
        uint32_t hash = 2166136261u ^ static_cast<uint32_t>(i);
        for (const char* p = challenge; *p; ++p)
          hash = (hash ^ static_cast<uint8_t>(*p)) * 16777619u;
        proof.apple_key_id_[i] = static_cast<uint8_t>(hash >> 24);
      }
      proof.apple_public_key_[0] = 0x04;
      proof.apple_environment_ = AppleAttestEnvironment::Production;
      proof.apple_validation_category_ = 4;
      memcpy(proof.apple_bundle_version_, "1", 2);
      proof.apple_bundle_version_length_ = 1;
      proof.apple_receipt_ = kReceipt;
      proof.apple_receipt_length_ = sizeof(kReceipt);
    }
    // A Play Integrity verdict carries its versionCode and
    // device label; `apple_evidence` false omits them too.
    if (provider == DeviceAttestation::PlayIntegrity && apple_evidence) {
      proof.play_version_code_ = 42;
      proof.play_device_label_ = 1;
    }
    return proof;
  }
};
}  // namespace fortis::auth

namespace {

using fortis::auth::AppleAttestEnvironment;
using fortis::auth::DeviceAttestation;
using fortis::auth::DeviceAttestationIssuer;
using fortis::auth::DeviceAuthResult;
using fortis::auth::DeviceAuthStore;
using fortis::auth::DeviceCredentials;
using fortis::auth::DeviceEnrollResult;
using fortis::auth::DevicePlatform;
using fortis::auth::DeviceQuotaTier;
using fortis::auth::DeviceRecord;
using fortis::storage::KvCompactionPolicy;
using fortis::storage::KvCompareStatus;
using fortis::storage::KvExpectation;
using fortis::storage::KvMutation;
using fortis::storage::KvReadStatus;
using fortis::storage::KvStore;
using fortis::auth::VerifiedDeviceAttestation;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
      abort();                                                                  \
    }                                                                           \
  } while (0)

constexpr int64_t kNow = 1700000000000LL;
constexpr int64_t kTtl = 86400000;
const char kMarker = '1';

std::string MakeDirectory(const char* name) {
  char path[256];
  snprintf(path, sizeof(path), "/tmp/fortis-attest-%s-XXXXXX", name);
  CHECK(mkdtemp(path));
  return path;
}

void RemoveDirectory(const std::string& directory) {
  for (const char* file : {"/fortis.kv", "/fortis.kv.lock", "/fortis.kv.compact"})
    unlink((directory + file).c_str());
  CHECK(rmdir(directory.c_str()) == 0);
}

off_t FileSize(const std::string& directory) {
  struct stat info {};
  CHECK(stat((directory + "/fortis.kv").c_str(), &info) == 0);
  return info.st_size;
}

bool Live(KvStore& store, const char* key, int64_t now) {
  char value[64];
  size_t size = 0;
  return store.GetStatus(key, now, value, sizeof(value), &size) == KvReadStatus::Found;
}

// A 43-character base64url challenge distinct per `seed`.
std::string Challenge(unsigned seed) {
  char text[44];
  snprintf(text, sizeof(text), "C%042u", seed);
  return text;
}

std::string ChallengeKey(const std::string& challenge) {
  return "attest:challenge:" + challenge;
}

void SetFail(const char* stage) {
  if (stage) CHECK(setenv("FORTIS_KV_TEST_FAIL", stage, 1) == 0);
  else CHECK(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
}

bool CountVisit(const char*, const void*, size_t, void* context) {
  ++*static_cast<size_t*>(context);
  return true;
}

size_t CountPrefix(KvStore& store, const char* prefix, int64_t now) {
  size_t count = 0;
  CHECK(store.Scan(prefix, nullptr, SIZE_MAX, now, [&, visitor_state = &count](const char* key, const void* value, size_t size) { return (CountVisit)(key, value, size, visitor_state); }));
  return count;
}

KvCompareStatus Consume(KvStore& store, const char* key, int64_t now, const char* winner) {
  const KvExpectation expected{key, &kMarker, 1};
  const KvMutation writes[] = {{key, nullptr, 0, 0, true}, {winner, "w", 1, 0, false}};
  return store.CompareAndWrite(expected, now, writes, 2);
}

// Minimal thread barrier (macOS has no pthread_barrier_t).
class Gate {
 public:
  explicit Gate(unsigned parties) : parties_(parties) {}
  void Arrive() {
    const unsigned generation = generation_.load();
    if (arrived_.fetch_add(1) + 1 == parties_) {
      arrived_.store(0);
      generation_.fetch_add(1);
    } else {
      while (generation_.load() == generation) std::this_thread::yield();
    }
  }

 private:
  const unsigned parties_;
  std::atomic<unsigned> arrived_{0};
  std::atomic<unsigned> generation_{0};
};

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
  policy.slice_bytes = 4096;
  policy.swap_tail_bytes = 64;
  policy.sync_every_bytes = 1000;
  policy.tick_max_slices = 1;
  policy.tick_budget_ms = 60000;
  return policy;
}

// ---- KvStore::CompareAndWrite ---------------------------------------------

void TestCompareOutcomes() {
  const std::string directory = MakeDirectory("outcomes");
  KvStore store;
  CHECK(store.Open(directory.c_str()));
  const KvMutation seed[] = {{"cond/a", "1", 1, 0, false},
                             {"cond/b", "value-b", 7, 0, false},
                             {"cond/empty", nullptr, 0, 0, false}};
  CHECK(store.Write(seed, 3));
  std::vector<char> large(KvExpectation::kMaxValueBytes + 1, 'x');
  const KvMutation big{"cond/large", large.data(), static_cast<uint32_t>(large.size()), 0, false};
  CHECK(store.Write(&big, 1));
  const off_t before = FileSize(directory);

  // Failed conditions append nothing.
  CHECK(Consume(store, "cond/missing", kNow, "out/x") == KvCompareStatus::MissingOrExpired);
  const KvExpectation wrong_bytes{"cond/b", "value-c", 7};
  const KvMutation write_x{"out/x", "x", 1, 0, false};
  CHECK(store.CompareAndWrite(wrong_bytes, kNow, &write_x, 1) == KvCompareStatus::ValueMismatch);
  const KvExpectation wrong_size{"cond/b", "value-", 6};
  CHECK(store.CompareAndWrite(wrong_size, kNow, &write_x, 1) == KvCompareStatus::ValueMismatch);
  const KvExpectation not_empty{"cond/a", nullptr, 0};
  CHECK(store.CompareAndWrite(not_empty, kNow, &write_x, 1) == KvCompareStatus::ValueMismatch);
  // A live value longer than any admissible expectation is a mismatch.
  const KvExpectation max_size{"cond/large", large.data(), KvExpectation::kMaxValueBytes};
  CHECK(store.CompareAndWrite(max_size, kNow, &write_x, 1) == KvCompareStatus::ValueMismatch);
  CHECK(FileSize(directory) == before && !Live(store, "out/x", kNow));

  // Invalid arguments are refused before any read and leave the store open.
  const KvExpectation too_big{"cond/large", large.data(),
                              static_cast<uint32_t>(large.size())};
  CHECK(store.CompareAndWrite(too_big, kNow, &write_x, 1) == KvCompareStatus::InvalidArgument);
  const KvExpectation no_key{nullptr, &kMarker, 1};
  CHECK(store.CompareAndWrite(no_key, kNow, &write_x, 1) == KvCompareStatus::InvalidArgument);
  const KvExpectation empty_key{"", &kMarker, 1};
  CHECK(store.CompareAndWrite(empty_key, kNow, &write_x, 1) == KvCompareStatus::InvalidArgument);
  const KvExpectation null_value{"cond/a", nullptr, 1};
  CHECK(store.CompareAndWrite(null_value, kNow, &write_x, 1) == KvCompareStatus::InvalidArgument);
  const KvExpectation good{"cond/a", &kMarker, 1};
  const KvMutation bad_erase{"out/x", "x", 1, 0, true};
  CHECK(store.CompareAndWrite(good, kNow, &bad_erase, 1) == KvCompareStatus::InvalidArgument);
  CHECK(store.CompareAndWrite(good, kNow, nullptr, 1) == KvCompareStatus::InvalidArgument);
  CHECK(store.CompareAndWrite(good, kNow, &write_x, 0) == KvCompareStatus::InvalidArgument);
  const KvMutation negative{"out/x", "x", 1, -1, false};
  CHECK(store.CompareAndWrite(good, kNow, &negative, 1) == KvCompareStatus::InvalidArgument);
  std::string long_key(1025, 'k');
  const KvMutation long_mutation{long_key.c_str(), "x", 1, 0, false};
  CHECK(store.CompareAndWrite(good, kNow, &long_mutation, 1) == KvCompareStatus::InvalidArgument);
  CHECK(FileSize(directory) == before && Live(store, "cond/a", kNow));

  // Matches commit one frame: exact bytes, and the empty value.
  CHECK(Consume(store, "cond/a", kNow, "out/a") == KvCompareStatus::Committed);
  CHECK(!Live(store, "cond/a", kNow) && Live(store, "out/a", kNow));
  CHECK(Consume(store, "cond/a", kNow, "out/a2") == KvCompareStatus::MissingOrExpired);
  const KvExpectation empty_value{"cond/empty", nullptr, 0};
  const KvMutation write_e{"out/e", "e", 1, 0, false};
  CHECK(store.CompareAndWrite(empty_value, kNow, &write_e, 1) == KvCompareStatus::Committed);
  // The condition key need not be mutated; it stays and still matches.
  const KvExpectation keep_b{"cond/b", "value-b", 7};
  CHECK(store.CompareAndWrite(keep_b, kNow, &write_e, 1) == KvCompareStatus::Committed);
  CHECK(store.CompareAndWrite(keep_b, kNow, &write_e, 1) == KvCompareStatus::Committed);
  store.Close();
  CHECK(store.CompareAndWrite(keep_b, kNow, &write_e, 1) == KvCompareStatus::IoError);
  CHECK(store.Open(directory.c_str()));
  CHECK(!Live(store, "cond/a", kNow) && Live(store, "out/a", kNow) && Live(store, "out/e", kNow));
  CHECK(!Live(store, "out/a2", kNow) && !Live(store, "out/x", kNow));
  store.Close();
  RemoveDirectory(directory);
  puts("compare-and-write outcomes and bounds: ok");
}

void TestExpiryBoundary() {
  const std::string directory = MakeDirectory("expiry");
  KvStore store;
  CHECK(store.Open(directory.c_str()));
  const int64_t expiry = kNow + 300000;
  const KvMutation seed[] = {{"exp/before", "1", 1, expiry, false},
                             {"exp/at", "1", 1, expiry, false},
                             {"exp/after", "1", 1, expiry, false}};
  CHECK(store.Write(seed, 3));
  // Live strictly before the expiry instant.
  CHECK(Consume(store, "exp/before", expiry - 1, "won/before") == KvCompareStatus::Committed);
  CHECK(Consume(store, "exp/at", expiry, "won/at") == KvCompareStatus::MissingOrExpired);
  CHECK(Consume(store, "exp/after", expiry + 1, "won/after") == KvCompareStatus::MissingOrExpired);
  // The caller's clock decides: the same record is still live earlier.
  CHECK(Consume(store, "exp/at", expiry - 1, "won/at") == KvCompareStatus::Committed);
  // Expired records survive restart as expired, never as live.
  store.Close();
  CHECK(store.Open(directory.c_str()));
  CHECK(Consume(store, "exp/after", expiry, "won/after") == KvCompareStatus::MissingOrExpired);
  CHECK(Consume(store, "exp/after", expiry - 1, "won/after") == KvCompareStatus::Committed);
  CHECK(Consume(store, "exp/before", expiry - 1, "won/again") == KvCompareStatus::MissingOrExpired);
  store.Close();
  RemoveDirectory(directory);
  puts("compare-and-write expiry boundary: ok");
}

void TestSimultaneousConsumers() {
  const std::string directory = MakeDirectory("race");
  KvStore store;
  CHECK(store.Open(directory.c_str()));
  constexpr unsigned kThreads = 8, kRounds = 300;
  for (unsigned round = 0; round < kRounds; ++round) {
    char key[48];
    snprintf(key, sizeof(key), "race/%04u", round);
    const KvMutation seed{key, "1", 1, kNow + 300000, false};
    CHECK(store.Write(&seed, 1));
  }
  Gate gate(kThreads);
  std::atomic<unsigned> committed{0}, missing{0}, other{0};
  std::vector<unsigned> winners(kRounds, 0);
  std::vector<std::thread> threads;
  for (unsigned thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&, thread] {
      for (unsigned round = 0; round < kRounds; ++round) {
        char key[48], winner[48];
        snprintf(key, sizeof(key), "race/%04u", round);
        snprintf(winner, sizeof(winner), "winner/%04u/%u", round, thread);
        gate.Arrive();
        switch (Consume(store, key, kNow, winner)) {
          case KvCompareStatus::Committed: ++committed; winners[round] = thread + 1; break;
          case KvCompareStatus::MissingOrExpired: ++missing; break;
          default: ++other; break;
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  CHECK(committed == kRounds && missing == kRounds * (kThreads - 1) && other == 0);
  CHECK(CountPrefix(store, "winner/", kNow) == kRounds);
  CHECK(CountPrefix(store, "race/", kNow) == 0);
  store.Close();
  CHECK(store.Open(directory.c_str()));
  CHECK(CountPrefix(store, "winner/", kNow) == kRounds && CountPrefix(store, "race/", kNow) == 0);
  for (unsigned round = 0; round < kRounds; ++round) {
    char winner[48];
    snprintf(winner, sizeof(winner), "winner/%04u/%u", round, winners[round] - 1);
    CHECK(winners[round] && Live(store, winner, kNow));
  }
  store.Close();
  RemoveDirectory(directory);
  printf("simultaneous consumers: %u rounds x %u threads, one winner each: ok\n",
         kRounds, kThreads);
}

void TestInjectedFailures() {
  const std::string directory = MakeDirectory("faults");
  {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    const KvMutation seed[] = {{"f/1", "1", 1, 0, false}, {"f/2", "1", 1, 0, false},
                               {"f/3", "1", 1, 0, false}, {"f/4", "1", 1, 0, false}};
    CHECK(store.Write(seed, 4));
    // A fault after the match but before the append: nothing written, open.
    const off_t before = FileSize(directory);
    SetFail("compare_matched");
    CHECK(Consume(store, "f/1", kNow, "won/1") == KvCompareStatus::IoError);
    SetFail(nullptr);
    CHECK(FileSize(directory) == before && Live(store, "f/1", kNow));
    // Torn append: header only. The store closes; replay drops the frame.
    SetFail("write_after_header");
    CHECK(Consume(store, "f/1", kNow, "won/1") == KvCompareStatus::IoError);
    SetFail(nullptr);
    char value[4]; size_t size = 0;
    CHECK(store.GetStatus("f/1", kNow, value, sizeof(value), &size) == KvReadStatus::IoError);
    CHECK(Consume(store, "f/1", kNow, "won/1") == KvCompareStatus::IoError);
  }
  {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    CHECK(Live(store, "f/1", kNow) && !Live(store, "won/1", kNow));
    CHECK(Consume(store, "f/1", kNow, "won/1") == KvCompareStatus::Committed);
    // The rollback marker keeps the challenge available after reopen.
    SetFail("write_before_fsync");
    CHECK(Consume(store, "f/2", kNow, "won/2") == KvCompareStatus::IoError);
    SetFail(nullptr);
  }
  {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    CHECK(Live(store, "f/2", kNow) && !Live(store, "won/2", kNow));
    CHECK(Consume(store, "f/2", kNow, "won/2b") == KvCompareStatus::Committed);
    // Durable but unacknowledged.
    SetFail("write_after_fsync");
    CHECK(Consume(store, "f/3", kNow, "won/3") == KvCompareStatus::IoError);
    SetFail(nullptr);
  }
  {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    CHECK(!Live(store, "f/3", kNow) && Live(store, "won/3", kNow));
    // A swap whose directory fsync failed refuses conditional frames too,
    // without appending or closing, until the fsync succeeds.
    SetFail("compact_after_rename");
    CHECK(store.Compact());
    SetFail("directory_sync_retry");
    const off_t before = FileSize(directory);
    CHECK(Consume(store, "f/4", kNow, "won/4") == KvCompareStatus::IoError);
    SetFail(nullptr);
    CHECK(FileSize(directory) == before && Live(store, "f/4", kNow));
    CHECK(Consume(store, "f/4", kNow, "won/4") == KvCompareStatus::Committed);
  }
  {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    for (const char* key : {"f/1", "f/2", "f/3", "f/4"}) CHECK(!Live(store, key, kNow));
    for (const char* key : {"won/1", "won/2b", "won/3", "won/4"}) CHECK(Live(store, key, kNow));
    CHECK(!Live(store, "won/2", kNow));
  }
  RemoveDirectory(directory);
  puts("compare-and-write injected I/O failures: ok");
}

// Child process: one conditional consume, SIGKILLed at `point`.
void KillChild(const std::string& directory, const char* point) {
  alarm(30);
  CHECK(setenv("FORTIS_KV_TEST_KILL", point, 1) == 0);
  CHECK(setenv("FORTIS_KV_TEST_KILL_ON_CALL", "1", 1) == 0);
  KvStore store;
  if (!store.Open(directory.c_str())) _exit(4);
  Consume(store, "crash/challenge", kNow, "crash/winner");
  _exit(7);  // The kill point was not reached.
}

void TestCrashImages() {
  struct Case { const char* point; bool committed; };
  // The rollback marker excludes frames that have not reached the commit barrier.
  const Case cases[] = {{"compare_matched", false}, {"write_after_header", false},
                        {"write_before_fsync", false}, {"write_acknowledged", true}};
  for (const Case& item : cases) {
    const std::string directory = MakeDirectory("crash");
    {
      KvStore store;
      CHECK(store.Open(directory.c_str()));
      const KvMutation seed{"crash/challenge", "1", 1, kNow + 300000, false};
      CHECK(store.Write(&seed, 1));
    }
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) KillChild(directory, item.point);
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    const bool consumed = !Live(store, "crash/challenge", kNow);
    const bool won = Live(store, "crash/winner", kNow);
    CHECK(consumed == won && consumed == item.committed);
    // Recovery: exactly one consume ever commits.
    CHECK(Consume(store, "crash/challenge", kNow, "crash/retry") ==
          (item.committed ? KvCompareStatus::MissingOrExpired : KvCompareStatus::Committed));
    CHECK(Consume(store, "crash/challenge", kNow, "crash/third") ==
          KvCompareStatus::MissingOrExpired);
    store.Close();
    CHECK(store.Open(directory.c_str()));
    CHECK(Live(store, "crash/winner", kNow) != Live(store, "crash/retry", kNow));
    CHECK(!Live(store, "crash/third", kNow));
    store.Close();
    RemoveDirectory(directory);
    printf("crash image at %-20s: %s\n", item.point, item.committed ? "committed" : "not committed");
  }
}

void TestCompaction() {
  const std::string directory = MakeDirectory("compact");
  const KvCompactionPolicy policy = TinyPolicy();
  constexpr unsigned kChallenges = 24;
  {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    // Garbage to reclaim plus live challenges, some short-lived.
    for (unsigned i = 0; i < 200; ++i) {
      char key[48];
      snprintf(key, sizeof(key), "noise/%04u", i % 50);
      std::string value(40 + i % 30, static_cast<char>('a' + i % 26));
      const KvMutation write{key, value.data(), static_cast<uint32_t>(value.size()), 0, false};
      CHECK(store.Write(&write, 1));
    }
    for (unsigned i = 0; i < kChallenges; ++i) {
      char key[48];
      snprintf(key, sizeof(key), "cc/%02u", i);
      const int64_t expiry = i >= 20 ? kNow + 10 : kNow + 300000;
      const KvMutation write{key, "1", 1, expiry, false};
      CHECK(store.Write(&write, 1));
    }
    // Consume while a stepped run is in progress (copy, catch-up, remap).
    bool running = store.MaintainCompaction(kNow, 1, policy);
    CHECK(running);
    unsigned consumed = 0;
    int64_t tick = 1;
    while (running) {
      if (consumed < 10) {
        char key[48], winner[48];
        snprintf(key, sizeof(key), "cc/%02u", consumed);
        snprintf(winner, sizeof(winner), "cw/%02u", consumed);
        CHECK(Consume(store, key, kNow, winner) == KvCompareStatus::Committed);
        CHECK(Consume(store, key, kNow, "cw/loser") == KvCompareStatus::MissingOrExpired);
        ++consumed;
      }
      running = store.MaintainCompaction(kNow, ++tick, policy);
    }
    CHECK(consumed >= 3);  // At least some consumes landed mid-run.
    fortis::storage::KvCompactionStats stats;
    store.CompactionStats(&stats);
    CHECK(stats.runs_completed == 1);
    for (unsigned i = consumed; i < 15; ++i) {
      char key[48], winner[48];
      snprintf(key, sizeof(key), "cc/%02u", i);
      snprintf(winner, sizeof(winner), "cw/%02u", i);
      CHECK(Consume(store, key, kNow, winner) == KvCompareStatus::Committed);
    }
    // An expiring run drops records expired at its clock; they stay missing.
    const int64_t later = kNow + 20;
    while (store.MaintainCompaction(later, tick += 1000, policy)) {}
    CHECK(Consume(store, "cc/20", later, "cw/20") == KvCompareStatus::MissingOrExpired);
    CHECK(Consume(store, "cc/20", kNow, "cw/20") == KvCompareStatus::MissingOrExpired);
    CHECK(store.Compact());
    CHECK(Consume(store, "cc/15", kNow, "cw/15") == KvCompareStatus::Committed);
  }
  {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    for (unsigned i = 0; i < kChallenges; ++i) {
      char key[48], winner[48];
      snprintf(key, sizeof(key), "cc/%02u", i);
      snprintf(winner, sizeof(winner), "cw/%02u", i);
      const bool used = i <= 15;
      CHECK(Live(store, key, kNow) == (!used && i < 20));
      CHECK(Live(store, winner, kNow) == used);
    }
    CHECK(!Live(store, "cw/loser", kNow));
    CHECK(Consume(store, "cc/16", kNow, "cw/16") == KvCompareStatus::Committed);
  }
  RemoveDirectory(directory);
  puts("compare-and-write across compaction runs and replay: ok");
}

// ---- Development challenge -------------------------------------------------

void TestDevelopmentChallenge() {
  const std::string directory = MakeDirectory("dev");
  {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    DeviceAuthStore auth(&store);
    DeviceCredentials first{}, other{};
    const std::string c1 = Challenge(1), c2 = Challenge(2), c3 = Challenge(3);
    CHECK(fortis::auth::PersistDeviceChallenge(&store, c1.c_str(), kNow));
    CHECK(fortis::auth::PersistDeviceChallenge(&store, c2.c_str(), kNow));
    CHECK(fortis::auth::PersistDeviceChallenge(&store, c3.c_str(), kNow));
    // Last live instant, then one use only.
    CHECK(auth.EnrollDevelopmentWithChallenge(c1.data(), c1.size(), DevicePlatform::Android,
          "1.0.0", 5, kNow + 299999, kTtl, &first) == DeviceEnrollResult::Enrolled);
    CHECK(auth.EnrollDevelopmentWithChallenge(c1.data(), c1.size(), DevicePlatform::Android,
          "1.0.0", 5, kNow + 299999, kTtl, &other) == DeviceEnrollResult::ChallengeUnavailable);
    // Exact expiry instant: no device is created and the record is untouched.
    const size_t devices = CountPrefix(store, "device/id/", kNow);
    CHECK(auth.EnrollDevelopmentWithChallenge(c2.data(), c2.size(), DevicePlatform::Ios,
          nullptr, 0, kNow + 300000, kTtl, &other) == DeviceEnrollResult::ChallengeUnavailable);
    CHECK(CountPrefix(store, "device/id/", kNow) == devices);
    // A key not holding the minted marker is not a challenge.
    const std::string forged = Challenge(4), forged_key = ChallengeKey(forged);
    const KvMutation forge{forged_key.c_str(), "2", 1, kNow + 300000, false};
    CHECK(store.Write(&forge, 1));
    CHECK(auth.EnrollDevelopmentWithChallenge(forged.data(), forged.size(), DevicePlatform::Ios,
          nullptr, 0, kNow, kTtl, &other) == DeviceEnrollResult::ChallengeMismatch);
    CHECK(auth.EnrollDevelopmentWithChallenge(Challenge(9).data(), 43, DevicePlatform::Ios,
          nullptr, 0, kNow, kTtl, &other) == DeviceEnrollResult::ChallengeUnavailable);
    // Rejected input reads and writes nothing.
    CHECK(auth.EnrollDevelopmentWithChallenge("a\0b", 3, DevicePlatform::Ios, nullptr, 0,
          kNow, kTtl, &other) == DeviceEnrollResult::Rejected);
    CHECK(auth.EnrollDevelopmentWithChallenge(nullptr, 0, DevicePlatform::Ios, nullptr, 0,
          kNow, kTtl, &other) == DeviceEnrollResult::Rejected);
    CHECK(auth.EnrollDevelopmentWithChallenge(c3.data(), c3.size(), DevicePlatform::Ios,
          nullptr, 0, kNow, 299999, &other) == DeviceEnrollResult::Rejected);
    std::string long_challenge(1100, 'L');
    CHECK(auth.EnrollDevelopmentWithChallenge(long_challenge.data(), long_challenge.size(),
          DevicePlatform::Ios, nullptr, 0, kNow, kTtl, &other) == DeviceEnrollResult::Rejected);
    CHECK(Live(store, ChallengeKey(c3).c_str(), kNow));
    DeviceRecord row{};
    CHECK(auth.Authenticate(first.token, kNow + 299999, false, &row) == DeviceAuthResult::Valid);
    CHECK(row.attestation == DeviceAttestation::Development &&
          row.quota_tier == DeviceQuotaTier::Development && row.platform == DevicePlatform::Android);
    CHECK(auth.Authenticate(first.token, kNow + 299999, true, &row) ==
          DeviceAuthResult::AttestationRequired);
    // A failed frame leaves the challenge usable after restart.
    SetFail("write_after_header");
    CHECK(auth.EnrollDevelopmentWithChallenge(c3.data(), c3.size(), DevicePlatform::Ios,
          nullptr, 0, kNow, kTtl, &other) == DeviceEnrollResult::StorageError);
    SetFail(nullptr);
  }
  {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    DeviceAuthStore auth(&store);
    DeviceCredentials third{};
    const std::string c1 = Challenge(1), c3 = Challenge(3);
    CHECK(CountPrefix(store, "device/id/", kNow) == 1);
    CHECK(auth.EnrollDevelopmentWithChallenge(c1.data(), c1.size(), DevicePlatform::Android,
          nullptr, 0, kNow + 1000, kTtl, &third) == DeviceEnrollResult::ChallengeUnavailable);
    CHECK(auth.EnrollDevelopmentWithChallenge(c3.data(), c3.size(), DevicePlatform::Linux,
          nullptr, 0, kNow + 1000, kTtl, &third) == DeviceEnrollResult::Enrolled);
    CHECK(CountPrefix(store, "device/id/", kNow) == 2);
  }
  RemoveDirectory(directory);
  puts("development challenge: one use, exact expiry, mismatch, failure: ok");
}

void TestDevelopmentRace() {
  const std::string directory = MakeDirectory("devrace");
  KvStore store;
  CHECK(store.Open(directory.c_str()));
  constexpr unsigned kThreads = 8, kRounds = 60;
  for (unsigned round = 0; round < kRounds; ++round)
    CHECK(fortis::auth::PersistDeviceChallenge(&store, Challenge(100 + round).c_str(), kNow));
  Gate gate(kThreads);
  std::atomic<unsigned> enrolled{0}, unavailable{0}, other{0};
  std::vector<std::thread> threads;
  std::vector<DeviceCredentials> winners(kRounds);
  for (unsigned thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&] {
      DeviceAuthStore auth(&store);
      for (unsigned round = 0; round < kRounds; ++round) {
        const std::string challenge = Challenge(100 + round);
        DeviceCredentials credentials{};
        gate.Arrive();
        switch (auth.EnrollDevelopmentWithChallenge(challenge.data(), challenge.size(),
                DevicePlatform::Ios, "9.9.9", 5, kNow + 1, kTtl, &credentials)) {
          case DeviceEnrollResult::Enrolled: ++enrolled; winners[round] = credentials; break;
          case DeviceEnrollResult::ChallengeUnavailable: ++unavailable; break;
          default: ++other; break;
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  CHECK(enrolled == kRounds && unavailable == kRounds * (kThreads - 1) && other == 0);
  CHECK(CountPrefix(store, "device/id/", kNow) == kRounds);
  CHECK(CountPrefix(store, "device/hash/", kNow) == kRounds);
  DeviceAuthStore auth(&store);
  for (const DeviceCredentials& winner : winners) {
    DeviceRecord row{};
    CHECK(auth.Authenticate(winner.token, kNow + 2, false, &row) == DeviceAuthResult::Valid);
  }
  store.Close();
  RemoveDirectory(directory);
  printf("development enrollment race: %u rounds x %u threads, one device each: ok\n",
         kRounds, kThreads);
}

// Hard restart between mint and use, and while enrolling.
void TestDevelopmentHardRestart() {
  const std::string directory = MakeDirectory("devkill");
  const std::string challenge = Challenge(7);
  pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    KvStore store;
    if (!store.Open(directory.c_str()) ||
        !fortis::auth::PersistDeviceChallenge(&store, challenge.c_str(), kNow)) _exit(4);
    raise(SIGKILL);
    _exit(7);
  }
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
  // SIGKILL as the enrollment frame is acknowledged: the device exists, the
  // challenge is gone, and the credentials were never returned.
  child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    alarm(30);
    CHECK(setenv("FORTIS_KV_TEST_KILL", "write_acknowledged", 1) == 0);
    KvStore store;
    if (!store.Open(directory.c_str())) _exit(4);
    DeviceAuthStore auth(&store);
    DeviceCredentials credentials{};
    auth.EnrollDevelopmentWithChallenge(challenge.data(), challenge.size(), DevicePlatform::Ios,
                                        nullptr, 0, kNow + 1, kTtl, &credentials);
    _exit(7);
  }
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  KvStore store;
  CHECK(store.Open(directory.c_str()));
  DeviceAuthStore auth(&store);
  DeviceCredentials credentials{};
  CHECK(CountPrefix(store, "device/id/", kNow) == 1);
  CHECK(auth.EnrollDevelopmentWithChallenge(challenge.data(), challenge.size(),
        DevicePlatform::Ios, nullptr, 0, kNow + 2, kTtl, &credentials) ==
        DeviceEnrollResult::ChallengeUnavailable);
  store.Close();
  RemoveDirectory(directory);
  puts("development challenge hard restart: ok");
}

// ---- Attested records ------------------------------------------------------

VerifiedDeviceAttestation Apple(const std::string& challenge, const char* version, int64_t at) {
  return DeviceAttestationIssuer::Make(DeviceAttestation::AppleAppAttest, DevicePlatform::Ios,
                                       challenge.c_str(), version, version ? strlen(version) : 0, at);
}
VerifiedDeviceAttestation Play(const std::string& challenge, const char* version, int64_t at) {
  return DeviceAttestationIssuer::Make(DeviceAttestation::PlayIntegrity, DevicePlatform::Android,
                                       challenge.c_str(), version, version ? strlen(version) : 0, at);
}

// FDV2 layout: platform at 94, attestation at 95, tier (low bits) at 96.
void ExpectRawRecord(KvStore& store, const char* id, DevicePlatform platform,
                     DeviceAttestation attestation, DeviceQuotaTier tier, bool revoked) {
  char key[48];
  snprintf(key, sizeof(key), "device/id/%s", id);
  uint8_t data[512];
  size_t size = 0;
  CHECK(store.GetStatus(key, kNow, data, sizeof(data), &size) == KvReadStatus::Found);
  CHECK(size >= 138 && memcmp(data, "FDV2", 4) == 0);
  CHECK(data[94] == static_cast<uint8_t>(platform));
  CHECK(data[95] == static_cast<uint8_t>(attestation));
  CHECK((data[96] & 0x7f) == static_cast<uint8_t>(tier));
  uint64_t revoked_at = 0;
  for (unsigned i = 0; i < 8; ++i) revoked_at |= static_cast<uint64_t>(data[106 + i]) << (8 * i);
  CHECK((revoked_at != 0) == revoked);
}

void TestAttestedBoundaries() {
  const std::string directory = MakeDirectory("attest-bounds");
  KvStore store;
  CHECK(store.Open(directory.c_str()));
  DeviceAuthStore auth(&store);
  const std::string challenge = Challenge(500);
  CHECK(fortis::auth::PersistDeviceChallenge(&store, challenge.c_str(), kNow));
  const int64_t now = kNow + 1000;
  DeviceCredentials out{};
  struct Bad {
    const char* name;
    VerifiedDeviceAttestation proof;
    DevicePlatform platform;
    const char* version;
    DeviceQuotaTier tier;
    int64_t ttl;
  };
  const char* v = "2.0.0";
  std::string long_version(201, 'v');
  const std::string short_challenge = challenge.substr(0, 42);
  std::string bad_alphabet = challenge;
  bad_alphabet[5] = '+';
  const Bad cases[] = {
      {"development provider", DeviceAttestationIssuer::Make(DeviceAttestation::Development,
           DevicePlatform::Ios, challenge.c_str(), v, 5, now), DevicePlatform::Ios, v,
       DeviceQuotaTier::Attested, kTtl},
      {"apple on android", DeviceAttestationIssuer::Make(DeviceAttestation::AppleAppAttest,
           DevicePlatform::Android, challenge.c_str(), v, 5, now), DevicePlatform::Android, v,
       DeviceQuotaTier::Attested, kTtl},
      {"play on ios", DeviceAttestationIssuer::Make(DeviceAttestation::PlayIntegrity,
           DevicePlatform::Ios, challenge.c_str(), v, 5, now), DevicePlatform::Ios, v,
       DeviceQuotaTier::Attested, kTtl},
      {"apple on macos", DeviceAttestationIssuer::Make(DeviceAttestation::AppleAppAttest,
           DevicePlatform::Macos, challenge.c_str(), v, 5, now), DevicePlatform::Macos, v,
       DeviceQuotaTier::Attested, kTtl},
      {"request platform differs", Apple(challenge, v, now), DevicePlatform::Android, v,
       DeviceQuotaTier::Attested, kTtl},
      {"apple without key association", DeviceAttestationIssuer::Make(
           DeviceAttestation::AppleAppAttest, DevicePlatform::Ios, challenge.c_str(), v, 5, now,
           false), DevicePlatform::Ios, v, DeviceQuotaTier::Attested, kTtl},
      {"play without verdict evidence", DeviceAttestationIssuer::Make(
           DeviceAttestation::PlayIntegrity, DevicePlatform::Android, challenge.c_str(), v, 5,
           now, false), DevicePlatform::Android, v, DeviceQuotaTier::Attested, kTtl},
      {"version bytes differ", Apple(challenge, v, now), DevicePlatform::Ios, "2.0.1",
       DeviceQuotaTier::Attested, kTtl},
      {"version absent in request", Apple(challenge, v, now), DevicePlatform::Ios, nullptr,
       DeviceQuotaTier::Attested, kTtl},
      {"version absent in proof", Apple(challenge, nullptr, now), DevicePlatform::Ios, v,
       DeviceQuotaTier::Attested, kTtl},
      {"version too long", Apple(challenge, long_version.c_str(), now), DevicePlatform::Ios,
       long_version.c_str(), DeviceQuotaTier::Attested, kTtl},
      {"development tier", Apple(challenge, v, now), DevicePlatform::Ios, v,
       DeviceQuotaTier::Development, kTtl},
      {"premium tier", Apple(challenge, v, now), DevicePlatform::Ios, v,
       DeviceQuotaTier::Premium, kTtl},
      {"tier zero", Apple(challenge, v, now), DevicePlatform::Ios, v,
       static_cast<DeviceQuotaTier>(0), kTtl},  // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange): deliberately tests rejection of an unknown byte-valued tier.
      {"tier five", Apple(challenge, v, now), DevicePlatform::Ios, v,
       static_cast<DeviceQuotaTier>(5), kTtl},  // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange): deliberately tests rejection of an unknown byte-valued tier.
      {"ttl too short", Apple(challenge, v, now), DevicePlatform::Ios, v,
       DeviceQuotaTier::Attested, 299999},
      {"ttl too long", Apple(challenge, v, now), DevicePlatform::Ios, v,
       DeviceQuotaTier::Attested, 2592000001LL},
      {"verified in the future", Apple(challenge, v, now + 1), DevicePlatform::Ios, v,
       DeviceQuotaTier::Attested, kTtl},
      {"verified too long ago", Apple(challenge, v, now - 300001), DevicePlatform::Ios, v,
       DeviceQuotaTier::Attested, kTtl},
      {"never verified", Apple(challenge, v, 0), DevicePlatform::Ios, v,
       DeviceQuotaTier::Attested, kTtl},
      {"short challenge", Apple(short_challenge, v, now), DevicePlatform::Ios, v,
       DeviceQuotaTier::Attested, kTtl},
      {"non-base64url challenge", Apple(bad_alphabet, v, now), DevicePlatform::Ios, v,
       DeviceQuotaTier::Attested, kTtl},
  };
  const off_t before = FileSize(directory);
  for (const Bad& item : cases) {
    const size_t length = item.version ? strlen(item.version) : 0;
    if (auth.EnrollAttested(item.proof, item.platform, item.version, length, item.tier, now,
                            item.ttl, &out) != DeviceEnrollResult::Rejected) {
      fprintf(stderr, "attested case not rejected: %s\n", item.name);
      CHECK(false);
    }
  }
  // Rejections consume nothing and write nothing.
  CHECK(FileSize(directory) == before && Live(store, ChallengeKey(challenge).c_str(), now));
  CHECK(CountPrefix(store, "device/", now) == 0);
  // The limits themselves are accepted: oldest admissible proof, minimum TTL.
  std::string max_version(200, 'v');
  CHECK(auth.EnrollAttested(Apple(challenge, max_version.c_str(), now - 300000),
        DevicePlatform::Ios, max_version.c_str(), 200, DeviceQuotaTier::Standard, now, 300000,
        &out) == DeviceEnrollResult::Enrolled);
  DeviceRecord row{};
  CHECK(auth.Authenticate(out.token, now, true, &row) == DeviceAuthResult::Valid);
  CHECK(row.app_version_length == 200 && row.quota_tier == DeviceQuotaTier::Standard);
  CHECK(auth.Authenticate(out.token, now + 300000, true, &row) ==
        DeviceAuthResult::SessionExpired);
  // Exact challenge expiry for an otherwise valid proof.
  const std::string late = Challenge(501);
  CHECK(fortis::auth::PersistDeviceChallenge(&store, late.c_str(), kNow));
  CHECK(auth.EnrollAttested(Play(late, nullptr, kNow + 299000), DevicePlatform::Android,
        nullptr, 0, DeviceQuotaTier::Attested, kNow + 300000, kTtl, &out) ==
        DeviceEnrollResult::ChallengeUnavailable);
  CHECK(auth.EnrollAttested(Play(late, nullptr, kNow + 299000), DevicePlatform::Android,
        nullptr, 0, DeviceQuotaTier::Attested, kNow + 299999, kTtl, &out) ==
        DeviceEnrollResult::Enrolled);
  // A proof for a challenge the server never minted.
  CHECK(auth.EnrollAttested(Play(Challenge(502), nullptr, now), DevicePlatform::Android,
        nullptr, 0, DeviceQuotaTier::Attested, now, kTtl, &out) ==
        DeviceEnrollResult::ChallengeUnavailable);
  store.Close();
  RemoveDirectory(directory);
  printf("attested enrollment boundaries: %zu rejections consume nothing: ok\n",
         sizeof(cases) / sizeof(cases[0]));
}

void TestAttestedLifecycle() {
  const std::string directory = MakeDirectory("attest-life");
  const std::string apple_challenge = Challenge(600), play_challenge = Challenge(601);
  DeviceCredentials apple{}, play{}, development{}, rotated{};
  {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    DeviceAuthStore auth(&store);
    CHECK(fortis::auth::PersistDeviceChallenge(&store, apple_challenge.c_str(), kNow));
    CHECK(fortis::auth::PersistDeviceChallenge(&store, play_challenge.c_str(), kNow));
    const VerifiedDeviceAttestation apple_proof = Apple(apple_challenge, "3.0.0", kNow + 10);
    CHECK(auth.EnrollAttested(apple_proof, DevicePlatform::Ios, "3.0.0", 5,
          DeviceQuotaTier::Attested, kNow + 20, kTtl, &apple) == DeviceEnrollResult::Enrolled);
    // Replaying the same verified proof cannot enroll a second device.
    DeviceCredentials replay{};
    CHECK(auth.EnrollAttested(apple_proof, DevicePlatform::Ios, "3.0.0", 5,
          DeviceQuotaTier::Attested, kNow + 21, kTtl, &replay) ==
          DeviceEnrollResult::ChallengeUnavailable);
    CHECK(auth.EnrollAttested(Play(play_challenge, nullptr, kNow + 10), DevicePlatform::Android,
          nullptr, 0, DeviceQuotaTier::Standard, kNow + 20, kTtl, &play) ==
          DeviceEnrollResult::Enrolled);
    CHECK(auth.EnrollDevelopment(DevicePlatform::Ios, nullptr, 0, kNow, kTtl, &development));
    DeviceRecord row{};
    CHECK(auth.Authenticate(apple.token, kNow + 30, true, &row) == DeviceAuthResult::Valid);
    CHECK(row.attestation == DeviceAttestation::AppleAppAttest &&
          row.platform == DevicePlatform::Ios && row.quota_tier == DeviceQuotaTier::Attested);
    CHECK(row.has_app_version && strcmp(row.app_version, "3.0.0") == 0);
    CHECK(fortis::auth::DeviceQuotaPerMinute(row.quota_tier) == 600);
    CHECK(auth.Authenticate(play.token, kNow + 30, true, &row) == DeviceAuthResult::Valid);
    CHECK(row.attestation == DeviceAttestation::PlayIntegrity &&
          row.platform == DevicePlatform::Android && row.quota_tier == DeviceQuotaTier::Standard);
    CHECK(!row.has_app_version && fortis::auth::DeviceQuotaPerMinute(row.quota_tier) == 120);
    CHECK(auth.Authenticate(development.token, kNow + 30, true, &row) ==
          DeviceAuthResult::AttestationRequired);
    CHECK(auth.Authenticate(development.token, kNow + 30, false, &row) ==
          DeviceAuthResult::Valid);
    ExpectRawRecord(store, apple.id, DevicePlatform::Ios, DeviceAttestation::AppleAppAttest,
                    DeviceQuotaTier::Attested, false);
    // Rotation keeps the verified attestation, platform, version and tier.
    CHECK(auth.Authenticate(apple.token, kNow + 40, true, &row) == DeviceAuthResult::Valid);
    CHECK(auth.Rotate(row, kNow + 50, kTtl, &rotated) == fortis::auth::DeviceRotateResult::Rotated);
    CHECK(strcmp(rotated.id, apple.id) == 0);
    CHECK(auth.Authenticate(apple.token, kNow + 60, true, &row) == DeviceAuthResult::Unauthorized);
    CHECK(auth.Authenticate(rotated.token, kNow + 60, true, &row) == DeviceAuthResult::Valid);
    CHECK(row.attestation == DeviceAttestation::AppleAppAttest &&
          row.quota_tier == DeviceQuotaTier::Attested && strcmp(row.app_version, "3.0.0") == 0);
    CHECK(auth.Rotate(row, kNow + 70, kTtl, &rotated) == fortis::auth::DeviceRotateResult::Rotated);
  }
  // Restart: the attested records, their tiers and the consumed challenges
  // all come back from replay.
  {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    DeviceAuthStore auth(&store);
    DeviceRecord row{};
    CHECK(auth.Authenticate(rotated.token, kNow + 80, true, &row) == DeviceAuthResult::Valid);
    CHECK(row.attestation == DeviceAttestation::AppleAppAttest &&
          row.platform == DevicePlatform::Ios && row.quota_tier == DeviceQuotaTier::Attested);
    CHECK(auth.Authenticate(play.token, kNow + 80, true, &row) == DeviceAuthResult::Valid);
    CHECK(row.quota_tier == DeviceQuotaTier::Standard);
    CHECK(auth.Authenticate(development.token, kNow + 80, true, &row) ==
          DeviceAuthResult::AttestationRequired);
    DeviceCredentials replay{};
    CHECK(auth.EnrollAttested(Apple(apple_challenge, "3.0.0", kNow + 70), DevicePlatform::Ios,
          "3.0.0", 5, DeviceQuotaTier::Attested, kNow + 80, kTtl, &replay) ==
          DeviceEnrollResult::ChallengeUnavailable);
    // Compaction rewrites the records without changing them.
    CHECK(store.Compact());
    store.Close();
    CHECK(store.Open(directory.c_str()));
    CHECK(auth.Authenticate(rotated.token, kNow + 90, true, &row) == DeviceAuthResult::Valid);
    CHECK(row.quota_tier == DeviceQuotaTier::Attested);
    // Revocation is enforced and keeps the attestation fields on record.
    CHECK(auth.Revoke(play.id, kNow + 100));
    CHECK(auth.Authenticate(play.token, kNow + 101, true, &row) ==
          DeviceAuthResult::SessionExpired);
    ExpectRawRecord(store, play.id, DevicePlatform::Android, DeviceAttestation::PlayIntegrity,
                    DeviceQuotaTier::Standard, true);
    // Token expiry is enforced at its exact instant.
    CHECK(auth.Authenticate(rotated.token, rotated.expires_at_ms - 1, true, &row) ==
          DeviceAuthResult::Valid);
    CHECK(auth.Authenticate(rotated.token, rotated.expires_at_ms, true, &row) ==
          DeviceAuthResult::SessionExpired);
    // A record whose provider does not match its platform never passes
    // required mode, whatever wrote it.
    char key[48];
    snprintf(key, sizeof(key), "device/id/%s", rotated.id);
    uint8_t data[512]; size_t size = 0;
    CHECK(store.GetStatus(key, kNow, data, sizeof(data), &size) == KvReadStatus::Found);
    data[94] = static_cast<uint8_t>(DevicePlatform::Android);
    const KvMutation patch{key, data, static_cast<uint32_t>(size), 0, false};
    CHECK(store.Write(&patch, 1));
    CHECK(auth.Authenticate(rotated.token, kNow + 110, true, &row) ==
          DeviceAuthResult::AttestationRequired);
    CHECK(auth.Authenticate(rotated.token, kNow + 110, false, &row) == DeviceAuthResult::Valid);
  }
  RemoveDirectory(directory);
  puts("attested records across authenticate, rotate, revoke, restart and replay: ok");
}

void TestAttestedRaceAndCrash() {
  const std::string directory = MakeDirectory("attest-race");
  constexpr unsigned kThreads = 8, kRounds = 40;
  {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    for (unsigned round = 0; round < kRounds; ++round)
      CHECK(fortis::auth::PersistDeviceChallenge(&store, Challenge(700 + round).c_str(), kNow));
    Gate gate(kThreads);
    std::atomic<unsigned> enrolled{0}, unavailable{0}, other{0};
    std::vector<std::thread> threads;
    for (unsigned thread = 0; thread < kThreads; ++thread) {
      threads.emplace_back([&] {
        DeviceAuthStore auth(&store);
        for (unsigned round = 0; round < kRounds; ++round) {
          const VerifiedDeviceAttestation proof = Play(Challenge(700 + round), "5.0", kNow + 5);
          DeviceCredentials credentials{};
          gate.Arrive();
          switch (auth.EnrollAttested(proof, DevicePlatform::Android, "5.0", 3,
                  DeviceQuotaTier::Attested, kNow + 10, kTtl, &credentials)) {
            case DeviceEnrollResult::Enrolled: ++enrolled; break;
            case DeviceEnrollResult::ChallengeUnavailable: ++unavailable; break;
            default: ++other; break;
          }
        }
      });
    }
    for (auto& thread : threads) thread.join();
    CHECK(enrolled == kRounds && unavailable == kRounds * (kThreads - 1) && other == 0);
    CHECK(CountPrefix(store, "device/id/", kNow) == kRounds);
  }
  // SIGKILL mid-frame: the challenge and the attested record stand or fall
  // together.
  const char* points[] = {"write_after_header", "write_acknowledged"};
  for (unsigned index = 0; index < 2; ++index) {
    const char* point = points[index];
    const std::string challenge = Challenge(800 + index);
    size_t devices_before = 0;
    {
      KvStore store;
      CHECK(store.Open(directory.c_str()));
      CHECK(fortis::auth::PersistDeviceChallenge(&store, challenge.c_str(), kNow));
      devices_before = CountPrefix(store, "device/id/", kNow);
    }
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
      alarm(30);
      CHECK(setenv("FORTIS_KV_TEST_KILL", point, 1) == 0);
      KvStore store;
      if (!store.Open(directory.c_str())) _exit(4);
      DeviceAuthStore auth(&store);
      DeviceCredentials credentials{};
      auth.EnrollAttested(Apple(challenge, nullptr, kNow + 5), DevicePlatform::Ios, nullptr, 0,
                          DeviceQuotaTier::Attested, kNow + 10, kTtl, &credentials);
      _exit(7);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    const bool committed = strcmp(point, "write_acknowledged") == 0;
    const size_t devices = CountPrefix(store, "device/id/", kNow);
    CHECK(Live(store, ChallengeKey(challenge).c_str(), kNow + 10) == !committed);
    CHECK(devices == devices_before + (committed ? 1 : 0));
    DeviceAuthStore auth(&store);
    DeviceCredentials credentials{};
    const DeviceEnrollResult retry = auth.EnrollAttested(Apple(challenge, nullptr, kNow + 5),
        DevicePlatform::Ios, nullptr, 0, DeviceQuotaTier::Attested, kNow + 10, kTtl, &credentials);
    CHECK(retry == (committed ? DeviceEnrollResult::ChallengeUnavailable
                              : DeviceEnrollResult::Enrolled));
    CHECK(CountPrefix(store, "device/id/", kNow) == devices + (committed ? 0 : 1));
    printf("attested crash image at %-18s: %s\n", point, committed ? "committed" : "not committed");
  }
  RemoveDirectory(directory);
  printf("attested enrollment race: %u rounds x %u threads, one device each: ok\n",
         kRounds, kThreads);
}

}  // namespace

int main() try {
  TestCompareOutcomes();
  TestExpiryBoundary();
  TestSimultaneousConsumers();
  TestInjectedFailures();
  TestCrashImages();
  TestCompaction();
  TestDevelopmentChallenge();
  TestDevelopmentRace();
  TestDevelopmentHardRestart();
  TestAttestedBoundaries();
  TestAttestedLifecycle();
  TestAttestedRaceAndCrash();
  puts("device attestation step A foundation test passed");
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
