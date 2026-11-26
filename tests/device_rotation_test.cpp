#include "test_support.h"
// Atomic device bearer rotation (DeviceAuthStore::Rotate) and the
// conditional Revoke/TouchPresence/Authenticate paths that race with it.
//
// Linked with the FORTIS_DEVICE_ROTATION_TEST_ONLY device_auth object (the
// snapshot hook runs a competing write between a method's row read and its
// conditional write; the forced token makes collisions and crash images
// deterministic) and the FORTIS_KV_TEST_FAILURES store object. The
// synthetic DeviceAttestationIssuer below exists only in this binary.
//
// The interleaving, stale-snapshot and gated-race cases fail against an
// unconditional kv_->Write mutant of Rotate or Revoke.
#include "../src/auth/device_auth.h"
#include "../src/storage/kv_store.h"

#include <atomic>
#include <functional>
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
// Test-only stand-in for the Apple verifier: an App Attest verdict whose key
// id derives from `key_seed`.
class DeviceAttestationIssuer {
 public:
  static VerifiedDeviceAttestation Apple(const char* challenge, const char* app_version,
                                         int64_t verified_at_ms, unsigned key_seed) {
    static const uint8_t kReceipt[] = {0x30, 0x80, 0x06, 0x09, 0x2a};
    VerifiedDeviceAttestation proof;
    proof.provider_ = DeviceAttestation::AppleAppAttest;
    proof.platform_ = DevicePlatform::Ios;
    snprintf(proof.challenge_, sizeof(proof.challenge_), "%s", challenge);
    const size_t length = strlen(app_version);
    memcpy(proof.app_version_, app_version, length);
    proof.app_version_length_ = static_cast<uint16_t>(length);
    proof.has_app_version_ = true;
    proof.verified_at_ms_ = verified_at_ms;
    for (size_t i = 0; i < 32; ++i)
      proof.apple_key_id_[i] = static_cast<uint8_t>((key_seed * 131u + i * 7u) & 0xff);
    proof.apple_public_key_[0] = 0x04;
    for (size_t i = 1; i < 65; ++i) proof.apple_public_key_[i] = static_cast<uint8_t>(i);
    proof.apple_environment_ = AppleAttestEnvironment::Production;
    proof.apple_validation_category_ = 4;
    memcpy(proof.apple_bundle_version_, "42", 3);
    proof.apple_bundle_version_length_ = 2;
    proof.apple_receipt_ = kReceipt;
    proof.apple_receipt_length_ = sizeof(kReceipt);
    return proof;
  }
};
}  // namespace fortis::auth

namespace {

using fortis::auth::DeviceAttestation;
using fortis::auth::DeviceAttestationIssuer;
using fortis::auth::DeviceAuthResult;
using fortis::auth::DeviceAuthStore;
using fortis::auth::DeviceCredentials;
using fortis::auth::DeviceEnrollResult;
using fortis::auth::DevicePlatform;
using fortis::auth::DeviceQuotaTier;
using fortis::auth::DeviceRecord;
using fortis::auth::DeviceRotateResult;
using fortis::storage::KvMutation;
using fortis::storage::KvReadStatus;
using fortis::storage::KvStore;
bool group_commit = false;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
      abort();                                                                  \
    }                                                                           \
  } while (0)

constexpr int64_t kNow = 1700000000000LL;
constexpr int64_t kTtl = 86400000;
constexpr int64_t kRetainMs = 7LL * 86400000;

std::string MakeDirectory(const char* name) {
  char path[256];
  snprintf(path, sizeof(path), "/tmp/fortis-rotate-%s-XXXXXX", name);
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

void SetFail(const char* stage) {
  if (stage) CHECK(setenv("FORTIS_KV_TEST_FAIL", stage, 1) == 0);
  else CHECK(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
}

bool Live(KvStore& store, const std::string& key, int64_t now) {
  char value[4096];
  size_t size = 0;
  return store.GetStatus(key.c_str(), now, value, sizeof(value), &size) == KvReadStatus::Found;
}

std::string IdKey(const char* id) { return std::string("device/id/") + id; }
std::string HashKey(const char* token) {
  char hash[65];
  CHECK(fortis::auth::HashDeviceToken(token, hash));
  return std::string("device/hash/") + hash;
}

std::string RawRow(KvStore& store, const char* id, int64_t now) {
  char data[512];
  size_t size = 0;
  CHECK(store.GetStatus(IdKey(id).c_str(), now, data, sizeof(data), &size) == KvReadStatus::Found);
  return std::string(data, size);
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

// Hash keys that resolve to `id`.
struct HashCount { const char* id; size_t count; };
bool CountDeviceHashes(const char*, const void* value, size_t size, HashCount* target) {
  if (size == 26 && memcmp(value, target->id, 26) == 0) ++target->count;
  return true;
}
size_t HashesFor(KvStore& store, const char* id, int64_t now) {
  HashCount count{id, 0};
  CHECK(store.Scan("device/hash/", nullptr, SIZE_MAX, now, [&, visitor_state = &count](const char* key, const void* value, size_t size) { return (CountDeviceHashes)(key, value, size, visitor_state); }));
  return count.count;
}

// Field by field: struct assignment need not clear padding.
bool Untouched(const DeviceCredentials& output) {
  const DeviceCredentials zero{};
  return memcmp(output.id, zero.id, sizeof(zero.id)) == 0 &&
         memcmp(output.token, zero.token, sizeof(zero.token)) == 0 &&
         output.expires_at_ms == 0;
}

// A distinct, well-formed device token.
std::string ForcedToken(unsigned seed) {
  char token[55];
  snprintf(token, sizeof(token), "fintel_dev_T%042u", seed);
  CHECK(fortis::auth::IsDeviceToken(token));
  return token;
}

DeviceCredentials Enroll(DeviceAuthStore& auth, int64_t now, int64_t ttl = kTtl,
                         DevicePlatform platform = DevicePlatform::Android) {
  DeviceCredentials credentials{};
  CHECK(auth.EnrollDevelopment(platform, "7.1.0", 5, now, ttl, &credentials));
  return credentials;
}

DeviceRecord Row(DeviceAuthStore& auth, const char* token, int64_t now,
                 bool required = false) {
  DeviceRecord row{};
  CHECK(auth.Authenticate(token, now, required, &row) == DeviceAuthResult::Valid);
  return row;
}

DeviceAuthResult AuthResult(DeviceAuthStore& auth, const char* token, int64_t now,
                            bool required = false) {
  DeviceRecord row{};
  return auth.Authenticate(token, now, required, &row);
}

// The snapshot hook: `action` runs once per intercepted call on the calling
// thread, never re-entered from inside itself (the competing write it makes
// passes through the hook too).
std::function<void(const char* operation, const char* id)> g_action;
void Trampoline(const char* operation, const char* id, void*) {
  static thread_local bool inside = false;
  if (inside || !g_action) return;
  inside = true;
  g_action(operation, id);
  inside = false;
}
void Intercept(std::function<void(const char*, const char*)> action) {
  g_action = std::move(action);
  fortis::auth::SetDeviceRowSnapshotHook(g_action ? Trampoline : nullptr, nullptr);
}

// Reusable spin barrier (macOS has no pthread_barrier_t).
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

// ---- Arguments, preservation, expiry, revocation, stale snapshots ---------

void TestArguments() {
  const std::string directory = MakeDirectory("args");
  KvStore store(group_commit);
  CHECK(store.Open(directory.c_str()));
  DeviceAuthStore auth(&store);
  const DeviceCredentials device = Enroll(auth, kNow);
  DeviceRecord row = Row(auth, device.token, kNow);
  const off_t before = FileSize(directory);
  DeviceCredentials out{};
  struct Bad { const char* name; int64_t now; int64_t ttl; };
  const Bad bad[] = {{"ttl below 5 min", kNow, 299999}, {"ttl above 30 days", kNow, 2592000001LL},
                     {"zero ttl", kNow, 0}, {"negative ttl", kNow, -kTtl},
                     {"zero now", 0, kTtl}, {"negative now", -1, kTtl},
                     {"expiry overflow", INT64_MAX - 299999, 300000}};
  for (const Bad& item : bad) {
    if (auth.Rotate(row, item.now, item.ttl, &out) != DeviceRotateResult::Rejected) {
      fprintf(stderr, "rotation not rejected: %s\n", item.name);
      CHECK(false);
    }
  }
  CHECK(auth.Rotate(row, kNow, kTtl, nullptr) == DeviceRotateResult::Rejected);
  DeviceAuthStore detached(nullptr);
  CHECK(detached.Rotate(row, kNow, kTtl, &out) == DeviceRotateResult::Rejected);
  DeviceRecord bad_id = row;
  bad_id.id[3] = 'I';  // Not Crockford base32.
  CHECK(auth.Rotate(bad_id, kNow, kTtl, &out) == DeviceRotateResult::Rejected);
  DeviceRecord short_id = row;
  short_id.id[25] = '\0';
  CHECK(auth.Rotate(short_id, kNow, kTtl, &out) == DeviceRotateResult::Rejected);
  DeviceRecord upper_hash = row;
  upper_hash.token_hash[0] = 'A';
  CHECK(auth.Rotate(upper_hash, kNow, kTtl, &out) == DeviceRotateResult::Rejected);
  DeviceRecord short_hash = row;
  short_hash.token_hash[63] = '\0';
  CHECK(auth.Rotate(short_hash, kNow, kTtl, &out) == DeviceRotateResult::Rejected);
  CHECK(FileSize(directory) == before && Untouched(out));
  CHECK(AuthResult(auth, device.token, kNow) == DeviceAuthResult::Valid);
  // The TTL bounds themselves are accepted.
  CHECK(auth.Rotate(row, kNow + 1, 300000, &out) == DeviceRotateResult::Rotated);
  CHECK(out.expires_at_ms == kNow + 1 + 300000);
  row = Row(auth, out.token, kNow + 2);
  CHECK(auth.Rotate(row, kNow + 3, 2592000000LL, &out) == DeviceRotateResult::Rotated);
  CHECK(out.expires_at_ms == kNow + 3 + 2592000000LL);
  store.Close();
  RemoveDirectory(directory);
  puts("rotation arguments: rejections read and write nothing; TTL bounds accepted: ok");
}

void TestPreservation() {
  const std::string directory = MakeDirectory("keep");
  KvStore store(group_commit);
  CHECK(store.Open(directory.c_str()));
  DeviceAuthStore auth(&store);
  const DeviceCredentials device = Enroll(auth, kNow);
  CHECK(auth.TouchPresence(device.id, kNow + 400000));
  const DeviceRecord before = Row(auth, device.token, kNow + 500000);
  CHECK(before.last_seen_at_ms == kNow + 400000);
  const int64_t at = kNow + 600000;
  DeviceCredentials out{};
  CHECK(auth.Rotate(before, at, kTtl, &out) == DeviceRotateResult::Rotated);
  CHECK(strcmp(out.id, device.id) == 0 && fortis::auth::IsDeviceToken(out.token));
  CHECK(strcmp(out.token, device.token) != 0 && out.expires_at_ms == at + kTtl);
  const DeviceRecord after = Row(auth, out.token, at);
  char hash[65];
  CHECK(fortis::auth::HashDeviceToken(out.token, hash) && strcmp(after.token_hash, hash) == 0);
  CHECK(strcmp(after.id, device.id) == 0);
  CHECK(after.platform == DevicePlatform::Android &&
        after.attestation == DeviceAttestation::Development &&
        after.quota_tier == DeviceQuotaTier::Development);
  CHECK(after.has_app_version && after.app_version_length == 5 &&
        memcmp(after.app_version, "7.1.0", 5) == 0);
  CHECK(after.created_at_ms == kNow && after.last_seen_at_ms == kNow + 400000);
  CHECK(after.revoked_at_ms == 0 && after.updated_at_ms == at &&
        after.token_expires_at_ms == at + kTtl);
  CHECK(AuthResult(auth, device.token, at) == DeviceAuthResult::Unauthorized);
  CHECK(!Live(store, HashKey(device.token), at));
  // The row and the new hash key are retained until seven days after the
  // new token expiry; the new hash key names the device.
  const int64_t retained = at + kTtl + kRetainMs;
  CHECK(Live(store, IdKey(device.id), retained - 1) && !Live(store, IdKey(device.id), retained));
  CHECK(Live(store, HashKey(out.token), retained - 1) && !Live(store, HashKey(out.token), retained));
  char mapped[27]{};
  size_t size = 0;
  CHECK(store.GetStatus(HashKey(out.token).c_str(), at, mapped, 26, &size) == KvReadStatus::Found);
  CHECK(size == 26 && strcmp(mapped, device.id) == 0);
  CHECK(HashesFor(store, device.id, at) == 1);
  // Rotation does not extend an expiry: the new token lasts ttl from the
  // rotation, even when the old one had longer left.
  const DeviceCredentials long_lived = Enroll(auth, kNow, 2592000000LL);
  CHECK(auth.Rotate(Row(auth, long_lived.token, at), at, 300000, &out) ==
        DeviceRotateResult::Rotated);
  CHECK(AuthResult(auth, out.token, at + 299999) == DeviceAuthResult::Valid);
  CHECK(AuthResult(auth, out.token, at + 300000) == DeviceAuthResult::SessionExpired);
  store.Close();
  RemoveDirectory(directory);
  puts("rotation preserves id, platform, attestation, tier, version, timestamps; TTL from now: ok");
}

void TestExpiry() {
  const std::string directory = MakeDirectory("expiry");
  KvStore store(group_commit);
  CHECK(store.Open(directory.c_str()));
  DeviceAuthStore auth(&store);
  // The snapshot's own expiry, at its exact instant.
  const DeviceCredentials device = Enroll(auth, kNow, 300000);
  const DeviceRecord row = Row(auth, device.token, kNow);
  const int64_t expires = kNow + 300000;
  off_t before = FileSize(directory);
  DeviceCredentials out{};
  CHECK(auth.Rotate(row, expires, kTtl, &out) == DeviceRotateResult::SessionExpired);
  CHECK(auth.Rotate(row, expires + 1, kTtl, &out) == DeviceRotateResult::SessionExpired);
  CHECK(FileSize(directory) == before && Untouched(out));
  CHECK(auth.Rotate(row, expires - 1, kTtl, &out) == DeviceRotateResult::Rotated);
  // A stale snapshot that claims a later expiry than the live row: the live
  // row decides, at its exact instant.
  const DeviceCredentials second = Enroll(auth, kNow, 300000);
  DeviceRecord forged = Row(auth, second.token, kNow);
  forged.token_expires_at_ms += kTtl;
  before = FileSize(directory);
  out = DeviceCredentials{};
  CHECK(auth.Rotate(forged, expires, kTtl, &out) == DeviceRotateResult::SessionExpired);
  CHECK(FileSize(directory) == before && Untouched(out));
  CHECK(AuthResult(auth, second.token, expires - 1) == DeviceAuthResult::Valid);
  CHECK(auth.Rotate(forged, expires - 1, kTtl, &out) == DeviceRotateResult::Rotated);
  CHECK(AuthResult(auth, out.token, expires) == DeviceAuthResult::Valid);
  store.Close();
  RemoveDirectory(directory);
  puts("rotation expiry: refused at the snapshot's and the live row's exact expiry: ok");
}

void TestRevocationAndStaleSnapshots() {
  const std::string directory = MakeDirectory("stale");
  KvStore store(group_commit);
  CHECK(store.Open(directory.c_str()));
  DeviceAuthStore auth(&store);
  // A consumed bearer: the second rotation from one snapshot writes nothing.
  const DeviceCredentials device = Enroll(auth, kNow);
  const DeviceRecord row = Row(auth, device.token, kNow);
  DeviceCredentials first{}, second{};
  CHECK(auth.Rotate(row, kNow + 1, kTtl, &first) == DeviceRotateResult::Rotated);
  const off_t before = FileSize(directory);
  CHECK(auth.Rotate(row, kNow + 2, kTtl, &second) == DeviceRotateResult::SessionExpired);
  CHECK(FileSize(directory) == before && Untouched(second));
  CHECK(AuthResult(auth, first.token, kNow + 3) == DeviceAuthResult::Valid);
  CHECK(AuthResult(auth, device.token, kNow + 3) == DeviceAuthResult::Unauthorized);
  CHECK(HashesFor(store, device.id, kNow + 3) == 1);
  // Revoked after the snapshot: refused, and the revocation stands.
  const DeviceCredentials revoked = Enroll(auth, kNow);
  const DeviceRecord snapshot = Row(auth, revoked.token, kNow);
  CHECK(auth.Revoke(revoked.id, kNow + 10));
  const off_t after_revoke = FileSize(directory);
  DeviceCredentials out{};
  CHECK(auth.Rotate(snapshot, kNow + 11, kTtl, &out) == DeviceRotateResult::SessionExpired);
  CHECK(FileSize(directory) == after_revoke && Untouched(out));
  CHECK(AuthResult(auth, revoked.token, kNow + 12) == DeviceAuthResult::SessionExpired);
  CHECK(HashesFor(store, revoked.id, kNow + 12) == 1);
  // A revoked snapshot is refused before anything is read.
  DeviceRecord revoked_row = snapshot;
  revoked_row.revoked_at_ms = kNow + 10;
  CHECK(auth.Rotate(revoked_row, kNow + 11, kTtl, &out) == DeviceRotateResult::SessionExpired);
  store.Close();
  RemoveDirectory(directory);
  puts("stale snapshots: consumed and revoked bearers rotate to nothing: ok");
}

// ---- Deterministic interleavings through the snapshot hook ----------------

void TestInterleavings() {
  const std::string directory = MakeDirectory("interleave");
  KvStore store(group_commit);
  CHECK(store.Open(directory.c_str()));
  DeviceAuthStore auth(&store);
  const int64_t at = kNow + 1000;
  {
    // Revocation between a valid store read and publication must prevent a
    // stale positive entry. The in-flight request may finish on its old read.
    const DeviceCredentials device = Enroll(auth, kNow);
    unsigned revoked = 0;
    Intercept([&](const char* operation, const char* id) {
      if (strcmp(operation, "snapshot_auth_verified") != 0) return;
      CHECK(auth.Revoke(id, at));
      ++revoked;
    });
    DeviceRecord cached{};
    CHECK(auth.AuthenticateSnapshot(device.token, at, false, &cached) ==
          DeviceAuthResult::Valid);
    Intercept(nullptr);
    CHECK(revoked == 1);
    CHECK(auth.AuthenticateSnapshot(device.token, at + 1, false, &cached) ==
          DeviceAuthResult::SessionExpired);
  }
  {
    // Another rotation of the same bearer commits after this one's snapshot.
    const DeviceCredentials device = Enroll(auth, kNow);
    const DeviceRecord row = Row(auth, device.token, kNow);
    DeviceCredentials inner{}, outer{};
    off_t after_inner = 0;
    Intercept([&](const char* operation, const char*) {
      if (strcmp(operation, "rotate") != 0) return;
      CHECK(auth.Rotate(row, at, kTtl, &inner) == DeviceRotateResult::Rotated);
      after_inner = FileSize(directory);
    });
    CHECK(auth.Rotate(row, at, kTtl, &outer) == DeviceRotateResult::SessionExpired);
    Intercept(nullptr);
    CHECK(after_inner > 0 && FileSize(directory) == after_inner && Untouched(outer));
    CHECK(AuthResult(auth, inner.token, at) == DeviceAuthResult::Valid);
    CHECK(AuthResult(auth, device.token, at) == DeviceAuthResult::Unauthorized);
    CHECK(HashesFor(store, device.id, at) == 1);
  }
  {
    // A revoke commits after the snapshot: the rotation must not undo it.
    const DeviceCredentials device = Enroll(auth, kNow);
    const DeviceRecord row = Row(auth, device.token, kNow);
    off_t after_revoke = 0;
    Intercept([&](const char* operation, const char* id) {
      if (strcmp(operation, "rotate") != 0) return;
      CHECK(auth.Revoke(id, at));
      after_revoke = FileSize(directory);
    });
    DeviceCredentials out{};
    CHECK(auth.Rotate(row, at, kTtl, &out) == DeviceRotateResult::SessionExpired);
    Intercept(nullptr);
    CHECK(after_revoke > 0 && FileSize(directory) == after_revoke && Untouched(out));
    CHECK(AuthResult(auth, device.token, at + 1) == DeviceAuthResult::SessionExpired);
    CHECK(HashesFor(store, device.id, at + 1) == 1);
  }
  {
    // The row is deleted after the snapshot.
    const DeviceCredentials device = Enroll(auth, kNow);
    const DeviceRecord row = Row(auth, device.token, kNow);
    Intercept([&](const char* operation, const char* id) {
      if (strcmp(operation, "rotate") != 0) return;
      const std::string key = IdKey(id);
      const KvMutation erase{key.c_str(), nullptr, 0, 0, true};
      CHECK(store.Write(&erase, 1));
    });
    const off_t before = FileSize(directory);
    DeviceCredentials out{};
    CHECK(auth.Rotate(row, at, kTtl, &out) == DeviceRotateResult::SessionExpired);
    Intercept(nullptr);
    CHECK(FileSize(directory) > before && Untouched(out));
    CHECK(Live(store, HashKey(device.token), at) && !Live(store, IdKey(device.id), at));
  }
  {
    // An unrelated row change (presence) is retried, and preserved.
    const DeviceCredentials device = Enroll(auth, kNow);
    const DeviceRecord row = Row(auth, device.token, kNow);
    unsigned calls = 0;
    Intercept([&](const char* operation, const char* id) {
      if (strcmp(operation, "rotate") != 0 || calls++) return;
      CHECK(auth.TouchPresence(id, kNow + 300000));
    });
    DeviceCredentials out{};
    CHECK(auth.Rotate(row, at + 300000, kTtl, &out) == DeviceRotateResult::Rotated);
    Intercept(nullptr);
    CHECK(calls == 2);
    const DeviceRecord after = Row(auth, out.token, at + 300000);
    CHECK(after.last_seen_at_ms == kNow + 300000);
    CHECK(AuthResult(auth, device.token, at + 300000) == DeviceAuthResult::Unauthorized);
  }
  {
    // A row that keeps changing: bounded retries, then StorageError with
    // nothing written by the rotation and the old bearer still current.
    const DeviceCredentials device = Enroll(auth, kNow);
    const DeviceRecord row = Row(auth, device.token, kNow);
    unsigned calls = 0;
    off_t after_touch = 0;
    Intercept([&](const char* operation, const char* id) {
      if (strcmp(operation, "rotate") != 0) return;
      ++calls;
      CHECK(auth.TouchPresence(id, kNow + 300000LL * calls));
      after_touch = FileSize(directory);
    });
    DeviceCredentials out{};
    CHECK(auth.Rotate(row, at, kTtl, &out) == DeviceRotateResult::StorageError);
    Intercept(nullptr);
    CHECK(calls == 8 && FileSize(directory) == after_touch && Untouched(out));
    CHECK(AuthResult(auth, device.token, at) == DeviceAuthResult::Valid);
    CHECK(HashesFor(store, device.id, at) == 1);
  }
  {
    // Revoke whose snapshot predates a committed rotation: it retries and
    // revokes the rotated row, so the new bearer is SESSION_EXPIRED rather
    // than a row/hash mismatch (StorageError).
    const DeviceCredentials device = Enroll(auth, kNow);
    const DeviceRecord row = Row(auth, device.token, kNow);
    DeviceCredentials inner{};
    unsigned calls = 0;
    Intercept([&](const char* operation, const char*) {
      if (strcmp(operation, "revoke") != 0 || calls++) return;
      CHECK(auth.Rotate(row, at, kTtl, &inner) == DeviceRotateResult::Rotated);
    });
    CHECK(auth.Revoke(device.id, at + 1));
    Intercept(nullptr);
    CHECK(calls == 2);
    CHECK(AuthResult(auth, inner.token, at + 2) == DeviceAuthResult::SessionExpired);
    CHECK(AuthResult(auth, device.token, at + 2) == DeviceAuthResult::Unauthorized);
  }
  {
    // A presence touch whose snapshot predates a committed rotation.
    const DeviceCredentials device = Enroll(auth, kNow);
    const DeviceRecord row = Row(auth, device.token, kNow);
    DeviceCredentials inner{};
    unsigned calls = 0;
    Intercept([&](const char* operation, const char*) {
      if (strcmp(operation, "touch") != 0 || calls++) return;
      CHECK(auth.Rotate(row, at, kTtl, &inner) == DeviceRotateResult::Rotated);
    });
    CHECK(auth.TouchPresence(device.id, kNow + 300000));
    Intercept(nullptr);
    CHECK(calls == 2);
    const DeviceRecord after = Row(auth, inner.token, kNow + 300000);
    CHECK(after.last_seen_at_ms == kNow + 300000);
  }
  {
    // A rotation commits between Authenticate's hash lookup and row read:
    // the consumed bearer is SESSION_EXPIRED, not a storage error.
    const DeviceCredentials device = Enroll(auth, kNow);
    const DeviceRecord row = Row(auth, device.token, kNow);
    DeviceCredentials inner{};
    unsigned calls = 0;
    Intercept([&](const char* operation, const char*) {
      if (strcmp(operation, "authenticate") != 0 || calls++) return;
      CHECK(auth.Rotate(row, at, kTtl, &inner) == DeviceRotateResult::Rotated);
    });
    CHECK(AuthResult(auth, device.token, at) == DeviceAuthResult::SessionExpired);
    Intercept(nullptr);
    CHECK(AuthResult(auth, device.token, at) == DeviceAuthResult::Unauthorized);
    CHECK(AuthResult(auth, inner.token, at) == DeviceAuthResult::Valid);
    // A hash key that disagrees with a live row is still a storage error.
    const std::string stray = ForcedToken(9);
    const std::string stray_key = HashKey(stray.c_str());
    const KvMutation map{stray_key.c_str(), device.id, 26, 0, false};
    CHECK(store.Write(&map, 1));
    CHECK(AuthResult(auth, stray.c_str(), at) == DeviceAuthResult::StorageError);
  }
  store.Close();
  RemoveDirectory(directory);
  puts("interleavings: rotation/revoke/delete/presence between snapshot and commit: ok");
}

// ---- Collisions, malformed records, I/O failures, crash images ------------

void TestCollisions() {
  const std::string directory = MakeDirectory("collide");
  KvStore store(group_commit);
  CHECK(store.Open(directory.c_str()));
  DeviceAuthStore auth(&store);
  const DeviceCredentials a = Enroll(auth, kNow), b = Enroll(auth, kNow);
  const DeviceRecord row = Row(auth, a.token, kNow);
  const off_t before = FileSize(directory);
  DeviceCredentials out{};
  fortis::auth::SetDeviceRotationTokenForTest(b.token);
  CHECK(auth.Rotate(row, kNow + 1, kTtl, &out) == DeviceRotateResult::StorageError);
  fortis::auth::SetDeviceRotationTokenForTest(a.token);
  CHECK(auth.Rotate(row, kNow + 1, kTtl, &out) == DeviceRotateResult::StorageError);
  fortis::auth::SetDeviceRotationTokenForTest(nullptr);
  CHECK(FileSize(directory) == before && Untouched(out));
  CHECK(strcmp(Row(auth, a.token, kNow + 2).id, a.id) == 0);
  CHECK(strcmp(Row(auth, b.token, kNow + 2).id, b.id) == 0);
  CHECK(auth.Rotate(row, kNow + 3, kTtl, &out) == DeviceRotateResult::Rotated);
  store.Close();
  RemoveDirectory(directory);
  puts("collisions: a minted hash that exists commits nothing: ok");
}

void TestMalformedRecords() {
  const std::string directory = MakeDirectory("records");
  KvStore store(group_commit);
  CHECK(store.Open(directory.c_str()));
  DeviceAuthStore auth(&store);
  const DeviceCredentials other = Enroll(auth, kNow);
  const std::string other_row = RawRow(store, other.id, kNow);
  struct Case { const char* name; DeviceRotateResult expected; };
  const Case cases[] = {{"missing", DeviceRotateResult::SessionExpired},
                        {"kv-expired", DeviceRotateResult::SessionExpired},
                        {"short", DeviceRotateResult::StorageError},
                        {"one byte short", DeviceRotateResult::StorageError},
                        {"one byte long", DeviceRotateResult::StorageError},
                        {"oversized", DeviceRotateResult::StorageError},
                        {"beyond expectation bound", DeviceRotateResult::StorageError},
                        {"bad magic", DeviceRotateResult::StorageError},
                        {"foreign id", DeviceRotateResult::StorageError},
                        {"zero expiry", DeviceRotateResult::StorageError}};
  for (const Case& item : cases) {
    const DeviceCredentials device = Enroll(auth, kNow);
    const DeviceRecord row = Row(auth, device.token, kNow);
    const std::string key = IdKey(device.id);
    std::string raw = RawRow(store, device.id, kNow);
    KvMutation patch{key.c_str(), nullptr, 0, 0, false};
    std::string value;
    if (!strcmp(item.name, "missing")) patch.erase = true;
    else if (!strcmp(item.name, "kv-expired")) { value = raw; patch.expires_at_ms = kNow + 5; }
    else if (!strcmp(item.name, "short")) value = raw.substr(0, 10);
    else if (!strcmp(item.name, "one byte short")) value = raw.substr(0, raw.size() - 1);
    else if (!strcmp(item.name, "one byte long")) value = raw + "x";
    else if (!strcmp(item.name, "oversized")) value = raw + std::string(400, 'x');
    else if (!strcmp(item.name, "beyond expectation bound")) value = std::string(5000, 'x');
    else if (!strcmp(item.name, "bad magic")) { value = raw; value[3] = '9'; }
    else if (!strcmp(item.name, "foreign id")) value = other_row;
    else { value = raw; for (unsigned i = 0; i < 8; ++i) value[98 + i] = 0; }
    patch.value = value.data();
    patch.value_size = static_cast<uint32_t>(value.size());
    CHECK(store.Write(&patch, 1));
    const off_t before = FileSize(directory);
    DeviceCredentials out{};
    const DeviceRotateResult result = auth.Rotate(row, kNow + 10, kTtl, &out);
    if (result != item.expected) {
      fprintf(stderr, "record case %s: unexpected result %d\n", item.name, static_cast<int>(result));
      CHECK(false);
    }
    CHECK(FileSize(directory) == before && Untouched(out));
    CHECK(Live(store, HashKey(device.token), kNow + 10));
  }
  store.Close();
  RemoveDirectory(directory);
  printf("malformed records: %zu missing/short/oversized/foreign cases write nothing: ok\n",
         sizeof(cases) / sizeof(cases[0]));
}

void TestInjectedFailures() {
  const std::string directory = MakeDirectory("faults");
  const std::string forced = ForcedToken(1), forced2 = ForcedToken(2);
  DeviceCredentials device{};
  {
    KvStore store(group_commit);
    CHECK(store.Open(directory.c_str()));
    DeviceAuthStore auth(&store);
    device = Enroll(auth, kNow);
    const DeviceRecord row = Row(auth, device.token, kNow);
    fortis::auth::SetDeviceRotationTokenForTest(forced.c_str());
    // Failure after the condition matched, before the append: nothing.
    const off_t before = FileSize(directory);
    DeviceCredentials out{};
    SetFail("compare_matched");
    CHECK(auth.Rotate(row, kNow + 1, kTtl, &out) == DeviceRotateResult::StorageError);
    SetFail(nullptr);
    CHECK(FileSize(directory) == before && Untouched(out));
    CHECK(AuthResult(auth, device.token, kNow + 1) == DeviceAuthResult::Valid);
    CHECK(AuthResult(auth, forced.c_str(), kNow + 1) == DeviceAuthResult::Unauthorized);
    // An unrelated cached bearer must also fail closed while the torn
    // append leaves the whole store unavailable.
    const DeviceCredentials other = Enroll(auth, kNow);
    DeviceRecord cached{};
    CHECK(auth.AuthenticateSnapshot(other.token, kNow + 1, false, &cached) ==
          DeviceAuthResult::Valid);
    SetFail("write_after_header");
    CHECK(auth.Rotate(row, kNow + 1, kTtl, &out) == DeviceRotateResult::StorageError);
    SetFail(nullptr);
    CHECK(Untouched(out));
    CHECK(AuthResult(auth, device.token, kNow + 1) == DeviceAuthResult::StorageError);
    CHECK(auth.AuthenticateSnapshot(other.token, kNow + 1, false, &cached) ==
          DeviceAuthResult::StorageError);
    CHECK(auth.Rotate(row, kNow + 1, kTtl, &out) == DeviceRotateResult::StorageError);
  }
  {
    KvStore store(group_commit);
    CHECK(store.Open(directory.c_str()));
    DeviceAuthStore auth(&store);
    CHECK(AuthResult(auth, device.token, kNow + 2) == DeviceAuthResult::Valid);
    CHECK(AuthResult(auth, forced.c_str(), kNow + 2) == DeviceAuthResult::Unauthorized);
    // The rollback marker keeps the old bearer after reopen.
    DeviceCredentials out{};
    SetFail("write_before_fsync");
    CHECK(auth.Rotate(Row(auth, device.token, kNow + 2), kNow + 3, kTtl, &out) ==
          DeviceRotateResult::StorageError);
    SetFail(nullptr);
    CHECK(Untouched(out));
  }
  {
    KvStore store(group_commit);
    CHECK(store.Open(directory.c_str()));
    DeviceAuthStore auth(&store);
    CHECK(AuthResult(auth, forced.c_str(), kNow + 4) == DeviceAuthResult::Unauthorized);
    const DeviceRecord row = Row(auth, device.token, kNow + 4);
    CHECK(HashesFor(store, device.id, kNow + 4) == 1);
    // Durable but unacknowledged.
    fortis::auth::SetDeviceRotationTokenForTest(forced2.c_str());
    DeviceCredentials out{};
    SetFail("write_after_fsync");
    CHECK(auth.Rotate(row, kNow + 5, kTtl, &out) == DeviceRotateResult::StorageError);
    SetFail(nullptr);
    CHECK(Untouched(out));
  }
  {
    KvStore store(group_commit);
    CHECK(store.Open(directory.c_str()));
    DeviceAuthStore auth(&store);
    CHECK(AuthResult(auth, forced.c_str(), kNow + 6) == DeviceAuthResult::Unauthorized);
    const DeviceRecord row = Row(auth, forced2.c_str(), kNow + 6);
    // A failed snapshot read.
    store.Close();
    DeviceCredentials out{};
    CHECK(auth.Rotate(row, kNow + 7, kTtl, &out) == DeviceRotateResult::StorageError);
    CHECK(Untouched(out));
  }
  fortis::auth::SetDeviceRotationTokenForTest(nullptr);
  RemoveDirectory(directory);
  puts("injected I/O failures: no bearer revealed; replay decides torn and unacked frames: ok");
}

void TestCrashImages() {
  struct Case { const char* point; bool committed; };
  const Case cases[] = {{"compare_matched", false}, {"write_after_header", false},
                        {"write_before_fsync", false}, {"write_acknowledged", true}};
  unsigned seed = 100;
  for (const Case& item : cases) {
    const std::string directory = MakeDirectory("crash");
    const std::string forced = ForcedToken(seed++);
    DeviceCredentials device{};
    {
      KvStore store(group_commit);
      CHECK(store.Open(directory.c_str()));
      DeviceAuthStore auth(&store);
      device = Enroll(auth, kNow);
    }
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
      alarm(30);
      CHECK(setenv("FORTIS_KV_TEST_KILL", item.point, 1) == 0);
      CHECK(setenv("FORTIS_KV_TEST_KILL_ON_CALL", "1", 1) == 0);
      KvStore store(group_commit);
      if (!store.Open(directory.c_str())) _exit(4);
      DeviceAuthStore auth(&store);
      DeviceRecord row{};
      if (auth.Authenticate(device.token, kNow + 1, false, &row) != DeviceAuthResult::Valid)
        _exit(5);
      fortis::auth::SetDeviceRotationTokenForTest(forced.c_str());
      DeviceCredentials out{};
      auth.Rotate(row, kNow + 2, kTtl, &out);
      _exit(7);  // The kill point was not reached.
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    KvStore store(group_commit);
    CHECK(store.Open(directory.c_str()));
    DeviceAuthStore auth(&store);
    const DeviceAuthResult old_result = AuthResult(auth, device.token, kNow + 3);
    const DeviceAuthResult new_result = AuthResult(auth, forced.c_str(), kNow + 3);
    // Exactly one bearer is current after replay, never both or neither.
    CHECK((old_result == DeviceAuthResult::Valid) != item.committed);
    CHECK((new_result == DeviceAuthResult::Valid) == item.committed);
    CHECK((item.committed ? old_result : new_result) == DeviceAuthResult::Unauthorized);
    CHECK(HashesFor(store, device.id, kNow + 3) == 1);
    const DeviceRecord row = Row(auth, item.committed ? forced.c_str() : device.token, kNow + 3);
    DeviceCredentials out{};
    CHECK(auth.Rotate(row, kNow + 4, kTtl, &out) == DeviceRotateResult::Rotated);
    store.Close();
    RemoveDirectory(directory);
    printf("rotation crash image at %-20s: %s\n", item.point,
           item.committed ? "committed" : "not committed");
  }
}

// ---- Races, restart, compaction, App Attest association -------------------

// One round: `kRotators` threads rotate the same snapshot. With `gated`, the
// snapshot hook holds every rotator until all have read the row, so every
// one of them reaches the conditional write against the same bytes.
struct RaceTally { unsigned rotated = 0, expired = 0, other = 0, auth_other = 0; };

void RunSameBearerRace(KvStore& store, const char* label, unsigned rounds, bool gated,
                       bool compact_concurrently, const char* initial_token,
                       std::string* final_token, bool required = false) {
  constexpr unsigned kRotators = 8, kAuthenticators = 2;
  DeviceAuthStore auth(&store);
  Gate start(kRotators + kAuthenticators + 1), finish(kRotators + kAuthenticators + 1);
  Gate snapshot_gate(kRotators);
  if (gated) {
    Intercept([&](const char* operation, const char*) {
      if (strcmp(operation, "rotate") == 0) snapshot_gate.Arrive();
    });
  }
  std::string current = initial_token;
  DeviceRecord row{};
  int64_t now = kNow;
  DeviceRotateResult results[kRotators];
  DeviceCredentials outputs[kRotators];
  std::atomic<bool> stop{false};
  RaceTally tally;
  std::atomic<unsigned> auth_other{0};
  std::vector<std::thread> threads;
  for (unsigned index = 0; index < kRotators; ++index) {
    threads.emplace_back([&, index] {
      DeviceAuthStore local(&store);
      for (unsigned round = 0; round < rounds; ++round) {
        start.Arrive();
        outputs[index] = DeviceCredentials{};
        results[index] = local.Rotate(row, now, kTtl, &outputs[index]);
        finish.Arrive();
      }
    });
  }
  for (unsigned index = 0; index < kAuthenticators; ++index) {
    threads.emplace_back([&] {
      DeviceAuthStore local(&store);
      for (unsigned round = 0; round < rounds; ++round) {
        start.Arrive();
        const std::string& token = current;
        for (unsigned i = 0; i < 16; ++i) {
          DeviceRecord seen{};
          const DeviceAuthResult result = local.Authenticate(token.c_str(), now, required, &seen);
          if (result != DeviceAuthResult::Valid && result != DeviceAuthResult::Unauthorized &&
              result != DeviceAuthResult::SessionExpired) ++auth_other;
        }
        finish.Arrive();
      }
    });
  }
  std::thread compactor;
  if (compact_concurrently) {
    compactor = std::thread([&] {
      while (!stop.load()) CHECK(store.Compact());
    });
  }
  for (unsigned round = 0; round < rounds; ++round) {
    now = kNow + 1000 + round;
    row = Row(auth, current.c_str(), now, required);
    start.Arrive();
    finish.Arrive();
    unsigned winners = 0, winner = 0;
    for (unsigned index = 0; index < kRotators; ++index) {
      if (results[index] == DeviceRotateResult::Rotated) { ++winners; winner = index; ++tally.rotated; }
      else if (results[index] == DeviceRotateResult::SessionExpired) {
        ++tally.expired;
        CHECK(Untouched(outputs[index]));
      } else {
        ++tally.other;
      }
    }
    if (winners != 1 || tally.other) {
      fprintf(stderr, "%s round %u: %u winners, %u other\n", label, round, winners, tally.other);
      CHECK(false);
    }
    CHECK(AuthResult(auth, outputs[winner].token, now, required) == DeviceAuthResult::Valid);
    CHECK(AuthResult(auth, current.c_str(), now, required) == DeviceAuthResult::Unauthorized);
    CHECK(HashesFor(store, row.id, now) == 1);
    current = outputs[winner].token;
  }
  stop.store(true);
  for (auto& thread : threads) thread.join();
  if (compactor.joinable()) compactor.join();
  if (gated) Intercept(nullptr);
  CHECK(auth_other.load() == 0);
  *final_token = current;
  printf("%s: %u rounds x 8 rotators: %u rotated, %u SESSION_EXPIRED, 0 other: ok\n", label,
         rounds, tally.rotated, tally.expired);
}

void TestRaces() {
  const std::string directory = MakeDirectory("race");
  std::string token;
  DeviceCredentials device{};
  {
    KvStore store(group_commit);
    CHECK(store.Open(directory.c_str()));
    DeviceAuthStore auth(&store);
    device = Enroll(auth, kNow);
    RunSameBearerRace(store, "free race", 200, false, false, device.token, &token);
    RunSameBearerRace(store, "gated race", 100, true, false, token.c_str(), &token);
    RunSameBearerRace(store, "race beside compaction", 60, false, true, token.c_str(), &token);
  }
  // Restart (replay) after hundreds of rotation frames and compactions.
  {
    KvStore store(group_commit);
    CHECK(store.Open(directory.c_str()));
    DeviceAuthStore auth(&store);
    CHECK(AuthResult(auth, token.c_str(), kNow + 5000) == DeviceAuthResult::Valid);
    CHECK(HashesFor(store, device.id, kNow + 5000) == 1);
    CHECK(CountPrefix(store, "device/hash/", kNow + 5000) == 1);
    CHECK(AuthResult(auth, device.token, kNow + 5000) == DeviceAuthResult::Unauthorized);
    CHECK(store.Compact());
  }
  {
    KvStore store(group_commit);
    CHECK(store.Open(directory.c_str()));
    DeviceAuthStore auth(&store);
    CHECK(AuthResult(auth, token.c_str(), kNow + 5000) == DeviceAuthResult::Valid);
    DeviceCredentials out{};
    CHECK(auth.Rotate(Row(auth, token.c_str(), kNow + 5000), kNow + 5001, kTtl, &out) ==
          DeviceRotateResult::Rotated);
    CHECK(AuthResult(auth, token.c_str(), kNow + 5002) == DeviceAuthResult::Unauthorized);
  }
  RemoveDirectory(directory);
  puts("same-bearer races survive replay and compaction with one current bearer: ok");
}

void TestRevokeRace() {
  const std::string directory = MakeDirectory("revoke-race");
  KvStore store(group_commit);
  CHECK(store.Open(directory.c_str()));
  DeviceAuthStore auth(&store);
  constexpr unsigned kRounds = 100, kRotators = 7;
  unsigned rotation_first = 0, revoke_first = 0;
  for (unsigned round = 0; round < kRounds; ++round) {
    const DeviceCredentials device = Enroll(auth, kNow);
    const int64_t now = kNow + 1000 + round;
    const DeviceRecord row = Row(auth, device.token, now);
    Gate gate(kRotators + 1);
    DeviceRotateResult results[kRotators];
    DeviceCredentials outputs[kRotators]{};
    bool revoked = false;
    std::vector<std::thread> threads;
    for (unsigned index = 0; index < kRotators; ++index) {
      threads.emplace_back([&, index] {
        DeviceAuthStore local(&store);
        gate.Arrive();
        results[index] = local.Rotate(row, now, kTtl, &outputs[index]);
      });
    }
    threads.emplace_back([&] {
      DeviceAuthStore local(&store);
      gate.Arrive();
      revoked = local.Revoke(device.id, now);
    });
    for (auto& thread : threads) thread.join();
    CHECK(revoked);
    unsigned winners = 0, winner = 0;
    for (unsigned index = 0; index < kRotators; ++index) {
      if (results[index] == DeviceRotateResult::Rotated) { ++winners; winner = index; }
      else CHECK(results[index] == DeviceRotateResult::SessionExpired && Untouched(outputs[index]));
    }
    CHECK(winners <= 1);
    // The revocation always stands, whichever committed first, and every
    // bearer answers a clean 401 class (never a storage error).
    char hash[65];
    if (winners) {
      ++rotation_first;
      CHECK(AuthResult(auth, outputs[winner].token, now + 1) == DeviceAuthResult::SessionExpired);
      CHECK(AuthResult(auth, device.token, now + 1) == DeviceAuthResult::Unauthorized);
      CHECK(fortis::auth::HashDeviceToken(outputs[winner].token, hash));
    } else {
      ++revoke_first;
      CHECK(AuthResult(auth, device.token, now + 1) == DeviceAuthResult::SessionExpired);
      CHECK(fortis::auth::HashDeviceToken(device.token, hash));
    }
    const std::string raw = RawRow(store, device.id, now + 1);
    CHECK(raw.compare(30, 64, hash) == 0);
    CHECK(HashesFor(store, device.id, now + 1) == 1);
  }
  store.Close();
  RemoveDirectory(directory);
  printf("rotate/revoke race: %u rounds (%u rotation first, %u revoke first), revoke stands: ok\n",
         kRounds, rotation_first, revoke_first);
}

void TestAppleAssociation() {
  const std::string directory = MakeDirectory("apple");
  const char challenge[] = "C000000000000000000000000000000000000000900";
  const char second_challenge[] = "C000000000000000000000000000000000000000901";
  const auto proof = DeviceAttestationIssuer::Apple(challenge, "3.0.0", kNow + 10, 77);
  char association_key[82];
  fortis::auth::AppleKeyAssociationKey(proof.apple_key_id(), association_key);
  DeviceCredentials apple{};
  std::string association, token;
  {
    KvStore store(group_commit);
    CHECK(store.Open(directory.c_str()));
    DeviceAuthStore auth(&store);
    for (const char* value : {challenge, second_challenge}) {
      const std::string key = std::string("attest:challenge:") + value;
      const KvMutation marker{key.c_str(), "1", 1, kNow + 300000, false};
      CHECK(store.Write(&marker, 1));
    }
    CHECK(auth.EnrollAttested(proof, DevicePlatform::Ios, "3.0.0", 5, DeviceQuotaTier::Attested,
                              kNow + 20, kTtl, &apple) == DeviceEnrollResult::Enrolled);
    char data[16384];
    size_t size = 0;
    CHECK(store.GetStatus(association_key, kNow + 20, data, sizeof(data), &size) ==
          KvReadStatus::Found);
    association.assign(data, size);
    RunSameBearerRace(store, "attested gated race (required mode)", 20, true, false, apple.token,
                      &token, true);
    CHECK(store.GetStatus(association_key, kNow + 5000, data, sizeof(data), &size) ==
          KvReadStatus::Found);
    CHECK(std::string(data, size) == association);
    CHECK(store.Compact());
  }
  {
    KvStore store(group_commit);
    CHECK(store.Open(directory.c_str()));
    DeviceAuthStore auth(&store);
    const DeviceRecord row = Row(auth, token.c_str(), kNow + 5000, true);
    CHECK(strcmp(row.id, apple.id) == 0 && row.platform == DevicePlatform::Ios &&
          row.attestation == DeviceAttestation::AppleAppAttest &&
          row.quota_tier == DeviceQuotaTier::Attested && row.app_version_length == 5 &&
          memcmp(row.app_version, "3.0.0", 5) == 0 && row.created_at_ms == kNow + 20);
    CHECK(AuthResult(auth, apple.token, kNow + 5000, true) == DeviceAuthResult::Unauthorized);
    fortis::auth::AppleKeyAssociation stored{};
    CHECK(auth.LoadAppleKeyAssociation(proof.apple_key_id(), kNow + 5000, &stored) ==
          KvReadStatus::Found);
    CHECK(strcmp(stored.device_id, apple.id) == 0 && stored.counter == 0);
    char data[16384];
    size_t size = 0;
    CHECK(store.GetStatus(association_key, kNow + 5000, data, sizeof(data), &size) ==
          KvReadStatus::Found);
    CHECK(std::string(data, size) == association);
    // The key still backs exactly one device after rotations and restart.
    DeviceCredentials again{};
    const auto replay = DeviceAttestationIssuer::Apple(second_challenge, "3.0.0", kNow + 4900, 77);
    CHECK(auth.EnrollAttested(replay, DevicePlatform::Ios, "3.0.0", 5, DeviceQuotaTier::Attested,
                              kNow + 5000, kTtl, &again) == DeviceEnrollResult::KeyAlreadyAssociated);
  }
  RemoveDirectory(directory);
  puts("App Attest association unchanged by rotation races, compaction and restart: ok");
}

}  // namespace

int main(int argc, char** argv) try {
  CHECK(argc == 1 || (argc == 2 && strcmp(argv[1], "--group-commit") == 0));
  group_commit = argc == 2;
  TestArguments();
  TestPreservation();
  TestExpiry();
  TestRevocationAndStaleSnapshots();
  TestInterleavings();
  TestCollisions();
  TestMalformedRecords();
  TestInjectedFailures();
  TestCrashImages();
  TestRaces();
  TestRevokeRace();
  TestAppleAssociation();
  puts("device bearer rotation test passed");
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
