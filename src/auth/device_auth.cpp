#include <algorithm>
#include "ownership.h"
#include <atomic>
#include "device_auth.h"

#include "threading.h"

#include <mbedtls/sha256.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/random.h>
#include <time.h>
#ifdef FORTIS_DEVICE_ROTATION_TEST_ONLY
#include <unistd.h>
#endif

#if defined(FORTIS_DEVICE_ROTATION_TEST_ONLY) && defined(FORTIS_UNIKRAFT)
#error "FORTIS_DEVICE_ROTATION_TEST_ONLY is a host test seam"
#endif

namespace fortis::auth {
namespace {

bool g_apple_attestation_required = true;
bool g_desktop_attestation_required = true;
constexpr int64_t kRetainAfterExpiryMs = 7LL * 86400000;
constexpr size_t kFixed = 4 + 26 + 64 + 4 + 40;
constexpr size_t kMaxRecord = kFixed + 200;
constexpr uint8_t kHasAppVersion = 0x80;
constexpr char kBase64Url[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr char kCrockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr char kHex[] = "0123456789abcdef";

constexpr size_t kSnapshotAuthSlots = 4096;
// Price reads share this positive-only cache. Explicit mutation and store
// epoch fences invalidate entries independently of their periodic recheck.
constexpr int64_t kSnapshotAuthTtlMs = 30000;
struct SnapshotAuthEntry {
  uint64_t store_epoch = 0;
  uint64_t checked_monotonic_ms = 0;
  int64_t checked_wall_ms = 0;
  DeviceRecord row{};
};
struct SnapshotAuthSlot {
  SnapshotAuthEntry entry;
  uint64_t mutation_generation = 0;
  unsigned pending_mutations = 0;
};
fortis::base::Mutex snapshot_auth_mutex;
SnapshotAuthSlot snapshot_auth_slots[kSnapshotAuthSlots]{};
struct SnapshotAuthCounters {
  std::atomic<uint64_t> hits{0}, misses{0}, invalidations{0};
};
SnapshotAuthCounters snapshot_auth_stats;

SnapshotAuthSlot& snapshot_auth_slot(const char* hash) {
  // The bearer digest is already uniformly distributed. Sixteen bits avoid
  // reading the full digest just to select the slot.
  unsigned index = 0;
  for (size_t i = 0; i < 4; ++i) {
    const unsigned nibble = hash[i] <= '9' ? hash[i] - '0' : hash[i] - 'a' + 10;
    index = (index << 4) | nibble;
  }
  return snapshot_auth_slots[index % kSnapshotAuthSlots];
}

uint64_t snapshot_auth_monotonic_ms() {
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

// Wall expiry and the current attestation policy apply even on a cache hit.
DeviceAuthResult check_device_record(const DeviceRecord& row, int64_t now_ms,
                                     bool attestation_required,
                                     bool android_attestation_required) {
  if (row.revoked_at_ms || row.token_expires_at_ms <= now_ms)
    return DeviceAuthResult::SessionExpired;
  const bool unverified_android = !android_attestation_required &&
      row.platform == DevicePlatform::Android &&
      row.attestation == DeviceAttestation::Development;
  const bool unverified_apple = !g_apple_attestation_required &&
      (row.platform == DevicePlatform::Ios || row.platform == DevicePlatform::Macos) &&
      row.attestation == DeviceAttestation::Development;
  const bool unverified_desktop = !g_desktop_attestation_required &&
      (row.platform == DevicePlatform::Windows || row.platform == DevicePlatform::Linux) &&
      row.attestation == DeviceAttestation::Development;
  if (attestation_required &&
      !unverified_android &&
      !unverified_apple &&
      !unverified_desktop &&
      !(row.attestation == DeviceAttestation::AppleAppAttest &&
        row.platform == DevicePlatform::Ios) &&
      !(row.attestation == DeviceAttestation::PlayIntegrity &&
        row.platform == DevicePlatform::Android))
    return DeviceAuthResult::AttestationRequired;
  return DeviceAuthResult::Valid;
}

// Invalidate before the CAS. While it is in progress callers use the durable
// store, and generations prevent publishing a fill started before the CAS.
// The mutex protects cache metadata only; it is released before all KV I/O.
class SnapshotAuthMutation {
 public:
  explicit SnapshotAuthMutation(const char* old_hash) : slot_(snapshot_auth_slot(old_hash)) {
    fortis::base::LockGuard snapshot_auth_lock(snapshot_auth_mutex);
    if (slot_.entry.store_epoch && strcmp(slot_.entry.row.token_hash, old_hash) == 0) {
      slot_.entry.store_epoch = 0;
      (snapshot_auth_stats.invalidations).fetch_add(1, std::memory_order_relaxed);
    }
    ++slot_.mutation_generation;
    ++slot_.pending_mutations;
  }
  ~SnapshotAuthMutation() {
    fortis::base::LockGuard snapshot_auth_lock(snapshot_auth_mutex);
    --slot_.pending_mutations;
  }
  SnapshotAuthMutation(const SnapshotAuthMutation&) = delete;
  SnapshotAuthMutation& operator=(const SnapshotAuthMutation&) = delete;

 private:
  SnapshotAuthSlot& slot_;
};

bool random_bytes(uint8_t* output, size_t length) {
#ifdef __APPLE__
  return getentropy(output, length) == 0;
#else
  while (length) {
    const ssize_t n = getrandom(output, length, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    output += n;
    length -= static_cast<size_t>(n);
  }
  return true;
#endif
}

bool make_token(char output[55]) {
  uint8_t random[32];
  if (!random_bytes(random, sizeof(random))) return false;
  memcpy(output, "fintel_dev_", 11);
  size_t out = 11;
  for (size_t i = 0; i < 30; i += 3) {
    output[out++] = kBase64Url[random[i] >> 2];
    output[out++] = kBase64Url[((random[i] & 3) << 4) | (random[i + 1] >> 4)];
    output[out++] = kBase64Url[((random[i + 1] & 15) << 2) | (random[i + 2] >> 6)];
    output[out++] = kBase64Url[random[i + 2] & 63];
  }
  output[out++] = kBase64Url[random[30] >> 2];
  output[out++] = kBase64Url[((random[30] & 3) << 4) | (random[31] >> 4)];
  output[out++] = kBase64Url[(random[31] & 15) << 2];
  output[out] = '\0';
  return out == 54;
}

bool make_id(int64_t now_ms, char output[27]) {
  if (now_ms < 0 || static_cast<uint64_t>(now_ms) >= (1ULL << 48)) return false;
  uint8_t bytes[16];
  for (int i = 0; i < 6; ++i) bytes[i] = static_cast<uint8_t>(now_ms >> (40 - i * 8));
  if (!random_bytes(bytes + 6, 10)) return false;
  for (unsigned i = 0; i < 26; ++i) {
    unsigned value = 0;
    for (unsigned j = 0; j < 5; ++j) {
      const int bit = static_cast<int>(i * 5 + j) - 2;
      value = (value << 1) | (bit < 0 ? 0u :
          static_cast<unsigned>((bytes[bit / 8] >> (7 - bit % 8)) & 1));
    }
    output[i] = kCrockford[value];
  }
  output[26] = '\0';
  return true;
}

void put64(uint8_t* output, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i) output[i] = static_cast<uint8_t>(value >> (8 * i));
}
uint64_t get64(const uint8_t* input) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(input[i]) << (8 * i);
  return value;
}

bool id_key(const char* id, char output[37]) {
  if (!id || strnlen(id, 27) != 26) return false;
  for (unsigned i = 0; i < 26; ++i)
    if (!strchr(kCrockford, id[i])) return false;
  std::copy_n("device/id/", 10, output);
  memcpy(output + 10, id, 27);
  return true;
}
bool hash_key(const char* hash, char output[77]) {
  if (!hash || strnlen(hash, 65) != 64) return false;
  for (unsigned i = 0; i < 64; ++i)
    if (!strchr(kHex, hash[i])) return false;
  std::copy_n("device/hash/", 12, output);
  memcpy(output + 12, hash, 65);
  return true;
}
int64_t retention(int64_t now_ms, int64_t expires_ms) {
  const int64_t base = expires_ms > now_ms ? expires_ms : now_ms;
  return base <= INT64_MAX - kRetainAfterExpiryMs
      ? base + kRetainAfterExpiryMs : INT64_MAX;
}

bool encode(const DeviceRecord& row, uint8_t output[kMaxRecord], uint32_t* length) {
  const size_t app_length = row.app_version_length;
  if (app_length > 200 || app_length >= sizeof(row.app_version) ||
      strnlen(row.id, sizeof(row.id)) != 26 ||
      strnlen(row.token_hash, sizeof(row.token_hash)) != 64)
    return false;
  std::copy_n("FDV2", 4, output);
  memcpy(output + 4, row.id, 26);
  memcpy(output + 30, row.token_hash, 64);
  output[94] = static_cast<uint8_t>(row.platform);
  output[95] = static_cast<uint8_t>(row.attestation);
  output[96] = static_cast<uint8_t>(row.quota_tier) |
      (row.has_app_version ? kHasAppVersion : 0);
  output[97] = static_cast<uint8_t>(app_length);
  put64(output + 98, static_cast<uint64_t>(row.token_expires_at_ms));
  put64(output + 106, static_cast<uint64_t>(row.revoked_at_ms));
  put64(output + 114, static_cast<uint64_t>(row.last_seen_at_ms));
  put64(output + 122, static_cast<uint64_t>(row.created_at_ms));
  put64(output + 130, static_cast<uint64_t>(row.updated_at_ms));
  memcpy(output + kFixed, row.app_version, app_length);
  *length = static_cast<uint32_t>(kFixed + app_length);
  return true;
}

bool decode(const uint8_t* input, size_t length, DeviceRecord* row) {
  const bool v1 = length >= kFixed && memcmp(input, "FDV1", 4) == 0;
  const bool v2 = length >= kFixed && memcmp(input, "FDV2", 4) == 0;
  const uint8_t tier = v2 ? static_cast<uint8_t>(input[96] & ~kHasAppVersion) : input[96];
  const uint8_t max_app_length = v1 ? 50 : 200;
  if ((!v1 && !v2) || input[97] > max_app_length ||
      length != kFixed + input[97] || input[94] < 1 || input[94] > 5 ||
      input[95] < 1 || input[95] > 3 || tier < 1 || tier > 4) return false;
  *row = DeviceRecord{};
  memcpy(row->id, input + 4, 26);
  memcpy(row->token_hash, input + 30, 64);
  memcpy(row->app_version, input + kFixed, input[97]);
  row->app_version[input[97]] = '\0';
  row->app_version_length = input[97];
  row->has_app_version = v2 ? (input[96] & kHasAppVersion) != 0 : input[97] != 0;
  row->platform = static_cast<DevicePlatform>(input[94]);
  row->attestation = static_cast<DeviceAttestation>(input[95]);
  row->quota_tier = static_cast<DeviceQuotaTier>(tier);
  row->token_expires_at_ms = static_cast<int64_t>(get64(input + 98));
  row->revoked_at_ms = static_cast<int64_t>(get64(input + 106));
  row->last_seen_at_ms = static_cast<int64_t>(get64(input + 114));
  row->created_at_ms = static_cast<int64_t>(get64(input + 122));
  row->updated_at_ms = static_cast<int64_t>(get64(input + 130));
  char key[37];
  return id_key(row->id, key) && row->token_expires_at_ms > 0;
}

// Reads the live row under `idkey` at `now_ms` into `data` (its exact
// stored bytes, the expectation for a later conditional write) and `row`.
// Missing when absent or expired in the KV; IoError for a failed read, a row
// longer than kMaxRecord, one that does not decode, or one naming another id.
storage::KvReadStatus load_row(storage::KvStore* kv, const char* idkey, const char* id, int64_t now_ms,
                      uint8_t data[kMaxRecord], size_t* size, DeviceRecord* row) {
  switch (kv->GetStatus(idkey, now_ms, data, kMaxRecord, size)) {
    case storage::KvReadStatus::Found:
      return decode(data, *size, row) && strcmp(row->id, id) == 0
          ? storage::KvReadStatus::Found : storage::KvReadStatus::IoError;
    case storage::KvReadStatus::Missing: return storage::KvReadStatus::Missing;
    default: return storage::KvReadStatus::IoError;
  }
}

// Read-modify-write of one device row retries a conditional write that lost
// to an unrelated row change this many times before giving up.
constexpr unsigned kRowWriteAttempts = 8;

#ifdef FORTIS_DEVICE_ROTATION_TEST_ONLY
DeviceRowSnapshotHook g_snapshot_hook = nullptr;
void* g_snapshot_context = nullptr;
char g_forced_token[55]{};
#endif

// "FAK1" App Attest key association: magic, environment, validation
// category (LE), device id, public key, counter (LE), verified_at (LE),
// bundle version length, receipt length (LE), then the bundle version and
// receipt bytes.
constexpr size_t kAppleFixed = 4 + 1 + 4 + 26 + 65 + 4 + 8 + 1 + 2;
constexpr size_t kAppleMaxRecord = kAppleFixed + kAppleBundleVersionMaxBytes + kAppleReceiptMaxBytes;

bool valid_apple_evidence(const VerifiedDeviceAttestation& proof) {
  const size_t version_length = proof.apple_bundle_version_length();
  const uint32_t category = proof.apple_validation_category();
  const bool legacy_without_extensions = category == 0 && version_length == 0;
  const bool extended = category != 0 && version_length >= 1 &&
      version_length <= kAppleBundleVersionMaxBytes &&
      strnlen(proof.apple_bundle_version(), kAppleBundleVersionMaxBytes + 1) == version_length;
  return (proof.apple_environment() == AppleAttestEnvironment::Production ||
          proof.apple_environment() == AppleAttestEnvironment::Development) &&
      (legacy_without_extensions || extended) && proof.apple_public_key()[0] == 0x04 &&
      proof.apple_receipt() && proof.apple_receipt_length() >= 1 &&
      proof.apple_receipt_length() <= kAppleReceiptMaxBytes;
}

void put32(uint8_t* output, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) output[i] = static_cast<uint8_t>(value >> (8 * i));
}
uint32_t get32(const uint8_t* input) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(input[i]) << (8 * i);
  return value;
}

uint32_t encode_apple(const VerifiedDeviceAttestation& proof, const char device_id[27],
                      uint8_t* output) {
  const size_t version_length = proof.apple_bundle_version_length();
  const uint32_t receipt_length = proof.apple_receipt_length();
  std::copy_n("FAK1", 4, output);
  output[4] = static_cast<uint8_t>(proof.apple_environment());
  put32(output + 5, proof.apple_validation_category());
  memcpy(output + 9, device_id, 26);
  memcpy(output + 35, proof.apple_public_key(), 65);
  put32(output + 100, 0);
  put64(output + 104, static_cast<uint64_t>(proof.verified_at_ms()));
  output[112] = static_cast<uint8_t>(version_length);
  output[113] = static_cast<uint8_t>(receipt_length);
  output[114] = static_cast<uint8_t>(receipt_length >> 8);
  memcpy(output + kAppleFixed, proof.apple_bundle_version(), version_length);
  memcpy(output + kAppleFixed + version_length, proof.apple_receipt(), receipt_length);
  return static_cast<uint32_t>(kAppleFixed + version_length + receipt_length);
}

bool decode_apple(const uint8_t* input, size_t length, AppleKeyAssociation* row) {
  if (length < kAppleFixed || memcmp(input, "FAK1", 4) != 0 ||
      (input[4] != 1 && input[4] != 2) || input[35] != 0x04) return false;
  const size_t version_length = input[112];
  const size_t receipt_length = input[113] | (static_cast<size_t>(input[114]) << 8);
  const uint32_t category = get32(input + 5);
  if ((category == 0) != (version_length == 0) ||
      version_length > kAppleBundleVersionMaxBytes ||
      !receipt_length || receipt_length > kAppleReceiptMaxBytes ||
      length != kAppleFixed + version_length + receipt_length) return false;
  row->environment = static_cast<AppleAttestEnvironment>(input[4]);
  row->validation_category = get32(input + 5);
  memcpy(row->device_id, input + 9, 26);
  row->device_id[26] = '\0';
  memcpy(row->public_key, input + 35, 65);
  row->counter = get32(input + 100);
  row->verified_at_ms = static_cast<int64_t>(get64(input + 104));
  memcpy(row->bundle_version, input + kAppleFixed, version_length);
  row->bundle_version[version_length] = '\0';
  row->bundle_version_length = static_cast<uint8_t>(version_length);
  memcpy(row->receipt, input + kAppleFixed + version_length, receipt_length);
  row->receipt_length = static_cast<uint32_t>(receipt_length);
  char key[37];
  return id_key(row->device_id, key);
}
}  // namespace

void AppleKeyAssociationKey(const uint8_t key_id[32], char output[82]) {
  memcpy(output, "attest:apple:key:", 17);
  for (unsigned i = 0; i < 32; ++i) {
    output[17 + 2 * i] = kHex[key_id[i] >> 4];
    output[18 + 2 * i] = kHex[key_id[i] & 15];
  }
  output[81] = '\0';
}

bool IsDeviceToken(const char* token) {
  if (!token || strnlen(token, 55) != 54 || memcmp(token, "fintel_dev_", 11) != 0)
    return false;
  for (unsigned i = 11; i < 54; ++i)
    if (!strchr(kBase64Url, token[i])) return false;
  return true;
}

bool HashDeviceToken(const char* token, char output[65]) {
  if (!IsDeviceToken(token) || !output) return false;
  uint8_t digest[32];
  if (mbedtls_sha256(reinterpret_cast<const uint8_t*>(token), 54,
                     digest, 0) != 0) return false;
  for (unsigned i = 0; i < 32; ++i) {
    output[2 * i] = kHex[digest[i] >> 4];
    output[2 * i + 1] = kHex[digest[i] & 15];
  }
  output[64] = '\0';
  return true;
}

bool SecureRandomBytes(uint8_t* output, size_t length) {
  return output && length && random_bytes(output, length);
}

bool DeviceChallengeKey(const char* challenge, size_t length, char* key, size_t capacity) {
  constexpr char kPrefix[] = "attest:challenge:";
  constexpr size_t kPrefixLength = sizeof(kPrefix) - 1;
  if (!challenge || !length || !key || memchr(challenge, '\0', length) ||
      capacity <= kPrefixLength || length >= capacity - kPrefixLength) return false;
  memcpy(key, kPrefix, kPrefixLength);
  memcpy(key + kPrefixLength, challenge, length);
  key[kPrefixLength + length] = '\0';
  return true;
}

uint32_t DeviceQuotaPerMinute(DeviceQuotaTier tier) {
  switch (tier) {
    case DeviceQuotaTier::Development: return 240;
    case DeviceQuotaTier::Attested: return 600;
    case DeviceQuotaTier::Premium: return 1200;
    default: return 120;
  }
}

#ifdef FORTIS_DEVICE_ROTATION_TEST_ONLY
namespace {
// The seam between a row read and its conditional write. A registered hook
// runs instead of the environment knobs. For "rotate" only:
// FORTIS_TEST_ROTATION_PAUSE_MS sleeps (a SIGKILL there leaves nothing
// committed), and FORTIS_TEST_ROTATION_COMPETE commits a competing rotation
// of the same row (its bearer discarded), so the request in flight must lose
// with SessionExpired: the socket test's deterministic 401 SESSION_EXPIRED,
// which the HTTP route lock otherwise never lets two requests reach.
void after_snapshot(storage::KvStore* kv, const char* operation, const char* id, int64_t now_ms) {
  if (g_snapshot_hook) {
    g_snapshot_hook(operation, id, g_snapshot_context);
    return;
  }
  if (strcmp(operation, "rotate") != 0) return;
  if (const char* pause = getenv("FORTIS_TEST_ROTATION_PAUSE_MS"))
    usleep(static_cast<useconds_t>(atoi(pause)) * 1000);
  static thread_local bool competing = false;
  if (!getenv("FORTIS_TEST_ROTATION_COMPETE") || competing) return;
  competing = true;
  char key[37];
  uint8_t data[kMaxRecord];
  size_t size = 0;
  DeviceRecord row{};
  DeviceCredentials discarded{};
  if (id_key(id, key) && load_row(kv, key, id, now_ms, data, &size, &row) == storage::KvReadStatus::Found)
    DeviceAuthStore(kv).Rotate(row, now_ms, 300000, &discarded);
  competing = false;
}
}  // namespace

void SetDeviceRowSnapshotHook(DeviceRowSnapshotHook hook, void* context) {
  g_snapshot_hook = hook;
  g_snapshot_context = context;
}

void SetDeviceRotationTokenForTest(const char* token) {
  if (token && IsDeviceToken(token)) memcpy(g_forced_token, token, sizeof(g_forced_token));
  else g_forced_token[0] = '\0';
}
#else
namespace {
inline void after_snapshot(storage::KvStore*, const char*, const char*, int64_t) {}
}  // namespace
#endif

bool DeviceAuthStore::LoadById(const char* id, int64_t now_ms, DeviceRecord* output) {
  if (!kv_ || !output) return false;
  char key[37];
  if (!id_key(id, key)) return false;
  uint8_t data[kMaxRecord];
  size_t size = 0;
  return load_row(kv_, key, id, now_ms, data, &size, output) == storage::KvReadStatus::Found;
}

bool DeviceAuthStore::EnrollDevelopment(DevicePlatform platform, const char* app_version,
                                        size_t app_version_length,
                                        int64_t now_ms, int64_t ttl_ms,
                                        DeviceCredentials* output) {
  return Enroll(nullptr, 0, platform, DeviceAttestation::Development,
                DeviceQuotaTier::Development, app_version, app_version_length,
                now_ms, ttl_ms, output) == DeviceEnrollResult::Enrolled;
}

DeviceEnrollResult DeviceAuthStore::EnrollDevelopmentWithChallenge(
    const char* challenge, size_t challenge_length, DevicePlatform platform,
    const char* app_version, size_t app_version_length, int64_t now_ms,
    int64_t ttl_ms, DeviceCredentials* output) {
  if (!challenge || !challenge_length) return DeviceEnrollResult::Rejected;
  return Enroll(challenge, challenge_length, platform, DeviceAttestation::Development,
                DeviceQuotaTier::Development, app_version, app_version_length,
                now_ms, ttl_ms, output);
}

DeviceEnrollResult DeviceAuthStore::EnrollAttested(
    const VerifiedDeviceAttestation& proof, DevicePlatform platform,
    const char* app_version, size_t app_version_length, DeviceQuotaTier quota_tier,
    int64_t now_ms, int64_t ttl_ms, DeviceCredentials* output) {
  if (proof.provider() != DeviceAttestation::AppleAppAttest &&
      proof.provider() != DeviceAttestation::PlayIntegrity) return DeviceEnrollResult::Rejected;
  const DevicePlatform proof_platform = proof.provider() == DeviceAttestation::AppleAppAttest
      ? DevicePlatform::Ios : DevicePlatform::Android;
  const size_t challenge_length = strnlen(proof.challenge(), 44);
  bool challenge_ok = challenge_length == 43;
  for (size_t i = 0; challenge_ok && i < challenge_length; ++i)
    challenge_ok = strchr(kBase64Url, proof.challenge()[i]) != nullptr;
  const bool version_ok = proof.app_version_length() <= 200 &&
      (app_version != nullptr) == proof.has_app_version() &&
      app_version_length == proof.app_version_length() &&
      (!app_version_length || (app_version && memcmp(app_version, proof.app_version(), app_version_length) == 0));
  if (proof_platform != platform || proof.platform() != platform || !challenge_ok ||
      !version_ok ||
      (quota_tier != DeviceQuotaTier::Standard && quota_tier != DeviceQuotaTier::Attested) ||
      proof.verified_at_ms() <= 0 || proof.verified_at_ms() > now_ms ||
      now_ms - proof.verified_at_ms() > kDeviceChallengeTtlMs)
    return DeviceEnrollResult::Rejected;
  const bool apple = proof.provider() == DeviceAttestation::AppleAppAttest;
  if (apple && !valid_apple_evidence(proof)) return DeviceEnrollResult::Rejected;
  // A Play Integrity proof carries the verified verdict's versionCode and
  // device label (play_integrity_enrollment.cpp); one without them was not
  // issued from a Verified decode.
  if (proof.provider() == DeviceAttestation::PlayIntegrity &&
      (proof.play_version_code() < 1 ||
       (proof.play_device_label() != 1 && proof.play_device_label() != 2)))
    return DeviceEnrollResult::Rejected;
  return Enroll(proof.challenge(), challenge_length, platform, proof.provider(), quota_tier,
                app_version, app_version_length, now_ms, ttl_ms, output,
                apple ? &proof : nullptr);
}

storage::KvReadStatus DeviceAuthStore::PeekChallenge(const char* challenge, size_t length,
                                            int64_t now_ms) {
  char key[640];
  if (!kv_ || !DeviceChallengeKey(challenge, length, key, sizeof(key)))
    return kv_ ? storage::KvReadStatus::Missing : storage::KvReadStatus::IoError;
  char value[2];
  size_t size = 0;
  switch (kv_->GetStatus(key, now_ms, value, sizeof(value), &size)) {
    case storage::KvReadStatus::Found: return size == 1 && value[0] == '1'
        ? storage::KvReadStatus::Found : storage::KvReadStatus::Missing;
    case storage::KvReadStatus::Missing:
    case storage::KvReadStatus::BufferTooSmall: return storage::KvReadStatus::Missing;
    default: return storage::KvReadStatus::IoError;
  }
}

storage::KvReadStatus DeviceAuthStore::LoadAppleKeyAssociation(const uint8_t key_id[32], int64_t now_ms,
                                                      AppleKeyAssociation* output) {
  if (!kv_ || !key_id || !output) return storage::KvReadStatus::IoError;
  char key[82];
  AppleKeyAssociationKey(key_id, key);
  auto data_storage = base::AllocateStorage<uint8_t[]>(kAppleMaxRecord);
  auto* data = data_storage.get();
  if (!data) return storage::KvReadStatus::IoError;
  size_t size = 0;
  storage::KvReadStatus status = kv_->GetStatus(key, now_ms, data, kAppleMaxRecord, &size);
  if (status == storage::KvReadStatus::Found && !decode_apple(data, size, output))
    status = storage::KvReadStatus::IoError;
  else if (status == storage::KvReadStatus::BufferTooSmall) status = storage::KvReadStatus::IoError;
  data_storage.reset();
  return status;
}

DeviceEnrollResult DeviceAuthStore::Enroll(const char* challenge, size_t challenge_length,
                                           DevicePlatform platform,
                                           DeviceAttestation attestation,
                                           DeviceQuotaTier quota_tier,
                                           const char* app_version, size_t app_version_length,
                                           int64_t now_ms, int64_t ttl_ms,
                                           DeviceCredentials* output,
                                           const VerifiedDeviceAttestation* apple) {
  if (!kv_ || !output || now_ms <= 0 || ttl_ms < 300000 || (apple && !challenge) ||
      ttl_ms > 2592000000LL ||
      ttl_ms > INT64_MAX - now_ms ||
      static_cast<unsigned>(platform) < 1 || static_cast<unsigned>(platform) > 5 ||
      static_cast<unsigned>(attestation) < 1 || static_cast<unsigned>(attestation) > 3 ||
      static_cast<unsigned>(quota_tier) < 1 || static_cast<unsigned>(quota_tier) > 4 ||
      (app_version && (app_version_length > 200 || app_version_length >= 201)) ||
      (!app_version && app_version_length != 0)) return DeviceEnrollResult::Rejected;
  char challenge_key[640];
  if (challenge && !DeviceChallengeKey(challenge, challenge_length, challenge_key,
                                       sizeof(challenge_key)))
    return DeviceEnrollResult::Rejected;
  DeviceCredentials next{};
  if (!make_id(now_ms, next.id) || !make_token(next.token))
    return DeviceEnrollResult::StorageError;
  next.expires_at_ms = now_ms + ttl_ms;
  DeviceRecord row{};
  memcpy(row.id, next.id, sizeof(row.id));
  if (!HashDeviceToken(next.token, row.token_hash)) return DeviceEnrollResult::StorageError;
  if (app_version) {
    memcpy(row.app_version, app_version, app_version_length);
    row.app_version[app_version_length] = '\0';
    row.app_version_length = static_cast<uint16_t>(app_version_length);
    row.has_app_version = true;
  }
  row.platform = platform;
  row.attestation = attestation;
  row.quota_tier = quota_tier;
  row.token_expires_at_ms = next.expires_at_ms;
  row.last_seen_at_ms = row.created_at_ms = row.updated_at_ms = now_ms;
  char idkey[37], hashkey[77];
  uint8_t data[kMaxRecord];
  uint32_t size = 0;
  if (!id_key(row.id, idkey) || !hash_key(row.token_hash, hashkey) ||
      !encode(row, data, &size)) return DeviceEnrollResult::StorageError;
  // Fresh 80-bit-random ids and 256-bit tokens; this guards a broken RNG,
  // not a race, so it stays outside the conditional frame.
  char existing[kMaxRecord]; size_t existing_size = 0;
  if (kv_->GetStatus(idkey, now_ms, existing, sizeof(existing), &existing_size) !=
          storage::KvReadStatus::Missing ||
      kv_->GetStatus(hashkey, now_ms, existing, sizeof(existing), &existing_size) !=
          storage::KvReadStatus::Missing) return DeviceEnrollResult::StorageError;
  const int64_t expiry = retention(now_ms, row.token_expires_at_ms);
  if (!challenge) {
    const storage::KvMutation writes[] = {{idkey, data, size, expiry, false},
                                 {hashkey, row.id, 26, expiry, false}};
    if (!kv_->Write(writes, 2)) return DeviceEnrollResult::StorageError;
    *output = next;
    return DeviceEnrollResult::Enrolled;
  }
  // The challenge is consumed by the same frame that creates the device, and
  // only if it is still live at `now_ms`; see KvStore::CompareAndWrite.
  // An App Attest proof adds its key association to the same frame, guarded
  // by that association's absence; it never expires.
  const char marker = '1';
  const storage::KvExpectation live_challenge{challenge_key, &marker, 1};
  char apple_key[82];
  std::unique_ptr<uint8_t[], base::StorageDeleter> apple_storage;
  uint8_t* apple_record = nullptr;
  uint32_t apple_size = 0;
  if (apple) {
    apple_storage = base::AllocateStorage<uint8_t[]>(kAppleMaxRecord);
    apple_record = apple_storage.get();
    if (!apple_record) return DeviceEnrollResult::StorageError;
    AppleKeyAssociationKey(apple->apple_key_id(), apple_key);
    apple_size = encode_apple(*apple, row.id, apple_record);
  }
  const storage::KvMutation writes[] = {{challenge_key, nullptr, 0, 0, true},
                               {idkey, data, size, expiry, false},
                               {hashkey, row.id, 26, expiry, false},
                               {apple_key, apple_record, apple_size, 0, false}};
  const char* const absent[] = {apple_key};
  const storage::KvCompareStatus status = apple
      ? kv_->CompareAndWrite(live_challenge, absent, 1, now_ms, writes, 4)
      : kv_->CompareAndWrite(live_challenge, now_ms, writes, 3);
  apple_storage.reset();
  switch (status) {
    case storage::KvCompareStatus::Committed: *output = next; return DeviceEnrollResult::Enrolled;
    case storage::KvCompareStatus::MissingOrExpired: return DeviceEnrollResult::ChallengeUnavailable;
    case storage::KvCompareStatus::ValueMismatch: return DeviceEnrollResult::ChallengeMismatch;
    case storage::KvCompareStatus::AbsentKeyPresent: return DeviceEnrollResult::KeyAlreadyAssociated;
    case storage::KvCompareStatus::InvalidArgument: return DeviceEnrollResult::Rejected;
    default: return DeviceEnrollResult::StorageError;
  }
}

DeviceAuthResult DeviceAuthStore::Authenticate(const char* token, int64_t now_ms,
                                               bool attestation_required,
                                               DeviceRecord* output,
                                               bool android_attestation_required) {
  if (!output || !IsDeviceToken(token)) return DeviceAuthResult::Unauthorized;
  char hash[65];
  if (!HashDeviceToken(token, hash) || !kv_)
    return DeviceAuthResult::StorageError;
  return AuthenticateHash(hash, now_ms, attestation_required, output,
                          android_attestation_required);
}

void ConfigureAppleAttestation(bool required) { g_apple_attestation_required = required; }

bool AppleAttestationRequired() { return g_apple_attestation_required; }

void ConfigureDesktopAttestation(bool required) { g_desktop_attestation_required = required; }

bool DesktopAttestationRequired() { return g_desktop_attestation_required; }

SnapshotAuthStats ReadSnapshotAuthStats() {
  return {(snapshot_auth_stats.hits).load(std::memory_order_relaxed),
          (snapshot_auth_stats.misses).load(std::memory_order_relaxed),
          (snapshot_auth_stats.invalidations).load(std::memory_order_relaxed)};
}

DeviceAuthResult DeviceAuthStore::AuthenticateSnapshot(const char* token, int64_t now_ms,
                                                       bool attestation_required,
                                                       DeviceRecord* output,
                                                       bool android_attestation_required) {
  if (!output || !IsDeviceToken(token)) return DeviceAuthResult::Unauthorized;
  char hash[65];
  if (!HashDeviceToken(token, hash) || !kv_) return DeviceAuthResult::StorageError;
  const uint64_t checked_ms = snapshot_auth_monotonic_ms();
  SnapshotAuthSlot& slot = snapshot_auth_slot(hash);
  fortis::base::UniqueLock snapshot_auth_lock(snapshot_auth_mutex);
  const uint64_t epoch = kv_->CacheEpoch();
  const uint64_t generation = slot.mutation_generation;
  const bool can_fill = slot.pending_mutations == 0;
  if (epoch && can_fill && checked_ms && slot.entry.store_epoch == epoch &&
      checked_ms >= slot.entry.checked_monotonic_ms &&
      checked_ms - slot.entry.checked_monotonic_ms < static_cast<uint64_t>(kSnapshotAuthTtlMs) &&
      now_ms >= slot.entry.checked_wall_ms &&
      now_ms - slot.entry.checked_wall_ms < kSnapshotAuthTtlMs &&
      strcmp(slot.entry.row.token_hash, hash) == 0) {
    const auto result = check_device_record(slot.entry.row, now_ms, attestation_required,
                                            android_attestation_required);
    if (result == DeviceAuthResult::Valid) *output = slot.entry.row;
    (snapshot_auth_stats.hits).fetch_add(1, std::memory_order_relaxed);
    return result;
  }
  snapshot_auth_lock.Unlock();
  (snapshot_auth_stats.misses).fetch_add(1, std::memory_order_relaxed);
  if (!epoch) return DeviceAuthResult::StorageError;

  const auto result = AuthenticateHash(hash, now_ms, attestation_required, output,
                                       android_attestation_required);
  if (result != DeviceAuthResult::Valid) return result;
  after_snapshot(kv_, "snapshot_auth_verified", output->id, now_ms);
  snapshot_auth_lock.Lock();
  const bool same_epoch = kv_->CacheEpoch() == epoch;
  if (same_epoch && can_fill && checked_ms &&
      slot.mutation_generation == generation && slot.pending_mutations == 0) {
    slot.entry = {epoch, checked_ms, now_ms, *output};
  }
  return same_epoch ? DeviceAuthResult::Valid : DeviceAuthResult::StorageError;
}

DeviceAuthResult DeviceAuthStore::AuthenticateHash(const char* hash, int64_t now_ms,
                                                   bool attestation_required,
                                                   DeviceRecord* output,
                                                   bool android_attestation_required) {
  char key[77];
  if (!hash_key(hash, key)) return DeviceAuthResult::StorageError;
  char id[27]{}; size_t size = 0;
  const auto lookup = kv_->GetStatus(key, now_ms, id, 26, &size);
  if (lookup == storage::KvReadStatus::Missing) return DeviceAuthResult::Unauthorized;
  if (lookup != storage::KvReadStatus::Found || size != 26)
    return DeviceAuthResult::StorageError;
  after_snapshot(kv_, "authenticate", id, now_ms);
  DeviceRecord row{};
  if (!LoadById(id, now_ms, &row)) return DeviceAuthResult::StorageError;
  if (strcmp(row.token_hash, hash) != 0) {
    // The hash key and the row are written in one frame, so a row naming
    // another hash means a rotation committed between the two reads. Its
    // frame erased this bearer's hash key: re-read it to tell that consumed
    // bearer (401 SESSION_EXPIRED, as a rotation loser gets) from a mapping
    // that disagrees with a live row (500).
    const auto again = kv_->GetStatus(key, now_ms, id, 26, &size);
    return again == storage::KvReadStatus::Missing ? DeviceAuthResult::SessionExpired
                                          : DeviceAuthResult::StorageError;
  }
  const auto result = check_device_record(row, now_ms, attestation_required,
                                          android_attestation_required);
  if (result == DeviceAuthResult::Valid) *output = row;
  return result;
}

// Atomic bearer rotation; the contract is on the declaration. The flow:
// validate arguments; mint one bearer (revealed only on Committed); then
// read the row's exact bytes, require the snapshot to hold `old`'s hash, be
// unrevoked and unexpired at `now_ms`, and submit one CompareAndWrite that
// expects those bytes and the new hash key's absence. A ValueMismatch means
// the row changed after the snapshot; the next pass re-reads it, so a
// consumed bearer, a revoke or an expiry ends as SessionExpired and only an
// unrelated change (presence) is retried. Nothing is written unless the
// frame commits, and a StorageError never reveals the minted bearer.
// TypeScript (DeviceStore.rotate) rotates by device id without comparing the
// old hash, so concurrent TS rotations all answer 200 and only the last
// bearer stays valid; here exactly one does.
DeviceRotateResult DeviceAuthStore::Rotate(const DeviceRecord& old, int64_t now_ms,
                                           int64_t ttl_ms, DeviceCredentials* output) {
  char idkey[37], oldkey[77];
  if (!output || !kv_ || now_ms <= 0 || ttl_ms < 300000 || ttl_ms > 2592000000LL ||
      ttl_ms > INT64_MAX - now_ms || !id_key(old.id, idkey) ||
      !hash_key(old.token_hash, oldkey)) return DeviceRotateResult::Rejected;
  if (old.revoked_at_ms || old.token_expires_at_ms <= now_ms)
    return DeviceRotateResult::SessionExpired;
  DeviceCredentials next{};
  memcpy(next.id, old.id, sizeof(next.id));
  bool minted = false;
#ifdef FORTIS_DEVICE_ROTATION_TEST_ONLY
  if (g_forced_token[0]) {
    memcpy(next.token, g_forced_token, sizeof(next.token));
    minted = true;
  }
#endif
  if (!minted && !make_token(next.token)) return DeviceRotateResult::StorageError;
  next.expires_at_ms = now_ms + ttl_ms;
  char new_hash[65], newkey[77];
  if (!HashDeviceToken(next.token, new_hash) || !hash_key(new_hash, newkey))
    return DeviceRotateResult::StorageError;
  for (unsigned attempt = 0; attempt < kRowWriteAttempts; ++attempt) {
    uint8_t snapshot[kMaxRecord];
    size_t snapshot_size = 0;
    DeviceRecord current{};
    switch (load_row(kv_, idkey, old.id, now_ms, snapshot, &snapshot_size, &current)) {
      case storage::KvReadStatus::Found: break;
      case storage::KvReadStatus::Missing: return DeviceRotateResult::SessionExpired;
      default: return DeviceRotateResult::StorageError;
    }
    if (strcmp(current.token_hash, old.token_hash) != 0 || current.revoked_at_ms ||
        current.token_expires_at_ms <= now_ms) return DeviceRotateResult::SessionExpired;
    after_snapshot(kv_, "rotate", old.id, now_ms);
    DeviceRecord updated = current;
    memcpy(updated.token_hash, new_hash, sizeof(new_hash));
    updated.token_expires_at_ms = next.expires_at_ms;
    updated.updated_at_ms = now_ms;
    uint8_t data[kMaxRecord];
    uint32_t size = 0;
    if (!encode(updated, data, &size)) return DeviceRotateResult::StorageError;
    // The snapshot's bytes pin the hash, revocation and expiry checked above
    // to the commit instant; the new hash key must not exist (a collision
    // would hand one bearer to two devices).
    const storage::KvExpectation unchanged{idkey, snapshot, static_cast<uint32_t>(snapshot_size)};
    const char* const absent[] = {newkey};
    const int64_t expiry = retention(now_ms, updated.token_expires_at_ms);
    const storage::KvMutation writes[] = {{oldkey, nullptr, 0, 0, true},
                                 {idkey, data, size, expiry, false},
                                 {newkey, updated.id, 26, expiry, false}};
    const SnapshotAuthMutation invalidate(old.token_hash);
    switch (kv_->CompareAndWrite(unchanged, absent, 1, now_ms, writes, 3)) {
      case storage::KvCompareStatus::Committed: *output = next; return DeviceRotateResult::Rotated;
      case storage::KvCompareStatus::ValueMismatch: continue;
      case storage::KvCompareStatus::MissingOrExpired: return DeviceRotateResult::SessionExpired;
      default: return DeviceRotateResult::StorageError;
    }
  }
  return DeviceRotateResult::StorageError;
}

// Conditional read-modify-write, as Rotate does it: the write expects the
// row bytes it read, so a rotation that commits in between is never undone
// (the retry revokes the rotated row instead).
bool DeviceAuthStore::Revoke(const char* id, int64_t now_ms) {
  char idkey[37];
  if (!kv_ || !id_key(id, idkey)) return false;
  for (unsigned attempt = 0; attempt < kRowWriteAttempts; ++attempt) {
    uint8_t snapshot[kMaxRecord], data[kMaxRecord];
    size_t snapshot_size = 0;
    uint32_t size = 0;
    DeviceRecord row{};
    if (load_row(kv_, idkey, id, now_ms, snapshot, &snapshot_size, &row) != storage::KvReadStatus::Found)
      return false;
    after_snapshot(kv_, "revoke", id, now_ms);
    row.revoked_at_ms = row.updated_at_ms = now_ms;
    if (!encode(row, data, &size)) return false;
    const storage::KvExpectation unchanged{idkey, snapshot, static_cast<uint32_t>(snapshot_size)};
    const storage::KvMutation mutation{idkey, data, size, retention(now_ms, row.token_expires_at_ms), false};
    const SnapshotAuthMutation invalidate(row.token_hash);
    const storage::KvCompareStatus status = kv_->CompareAndWrite(unchanged, now_ms, &mutation, 1);
    if (status == storage::KvCompareStatus::Committed) return true;
    if (status != storage::KvCompareStatus::ValueMismatch) return false;
  }
  return false;
}

#if defined(FORTIS_PRICE_ROUTE_TEST_ONLY) || defined(FORTIS_NETWORK_ROUTE_TEST_ONLY) || \
    defined(FORTIS_HISTORY_ROUTE_TEST_ONLY) || defined(FORTIS_OPERATIONS_TEST_ONLY) || \
    defined(FORTIS_DISCOVERY_ROUTE_TEST_ONLY) || defined(FORTIS_NEWS_ROUTE_TEST_ONLY) || \
    defined(FORTIS_ROBINHOOD_ROUTE_TEST_ONLY)
bool DeviceAuthStore::SeedPriceRouteFixture(const char* token, const char* id,
                                            int64_t now_ms, int64_t token_ttl_ms) {
  if (!token || !id || now_ms <= 0 || token_ttl_ms <= 0 || token_ttl_ms > INT64_MAX - now_ms) return false;
  DeviceRecord row{};
  if (strnlen(id, sizeof(row.id)) != sizeof(row.id) - 1 ||
      strnlen(token, sizeof(row.token_hash)) > 54) return false;
  memcpy(row.id, id, sizeof(row.id) - 1);
  if (!HashDeviceToken(token, row.token_hash)) return false;
  row.platform = DevicePlatform::Ios;
  row.attestation = DeviceAttestation::Development;
  row.quota_tier = DeviceQuotaTier::Development;
  row.token_expires_at_ms = now_ms + token_ttl_ms;
  row.last_seen_at_ms = row.created_at_ms = row.updated_at_ms = now_ms;
  char idkey[37], hashkey[77]; uint8_t data[kMaxRecord]; uint32_t size = 0;
  if (!id_key(row.id, idkey) || !hash_key(row.token_hash, hashkey) ||
      !encode(row, data, &size)) return false;
  const int64_t expiry = retention(now_ms, row.token_expires_at_ms);
  const storage::KvMutation writes[] = {{idkey, data, size, expiry, false},
                               {hashkey, row.id, 26, expiry, false}};
  return kv_ && kv_->Write(writes, 2);
}
#endif

#if defined(FORTIS_ADDRESS_ROUTE_TEST_ONLY) || defined(FORTIS_AI_ROUTE_TEST_ONLY) || \
    defined(FORTIS_CROSS_CHAIN_ROUTE_TEST_ONLY) || defined(FORTIS_ROBINHOOD_ROUTE_TEST_ONLY) || \
    defined(FORTIS_DASHPAY_ROUTE_TEST_ONLY)
bool DeviceAuthStore::SeedAddressRouteFixture(const char* token, const char* id, int64_t now_ms) {
  if (!token || !id || now_ms <= 0) return false;
  DeviceRecord row{};
  if (strnlen(id, sizeof(row.id)) != sizeof(row.id) - 1 ||
      strnlen(token, sizeof(row.token_hash)) > 54) return false;
  memcpy(row.id, id, sizeof(row.id) - 1);
  if (!HashDeviceToken(token, row.token_hash)) return false;
  row.platform = DevicePlatform::Ios;
  row.attestation = DeviceAttestation::Development;
  row.quota_tier = DeviceQuotaTier::Premium;
  row.token_expires_at_ms = now_ms + 400LL * 86400000;
  row.last_seen_at_ms = row.created_at_ms = row.updated_at_ms = now_ms;
  char idkey[37], hashkey[77]; uint8_t data[kMaxRecord]; uint32_t size = 0;
  if (!id_key(row.id, idkey) || !hash_key(row.token_hash, hashkey) ||
      !encode(row, data, &size)) return false;
  const int64_t expiry = retention(now_ms, row.token_expires_at_ms);
  const storage::KvMutation writes[] = {{idkey, data, size, expiry, false},
                               {hashkey, row.id, 26, expiry, false}};
  return kv_ && kv_->Write(writes, 2);
}
#endif


// Conditional like Revoke: a stale presence write must not put back a
// token hash that a concurrent rotation replaced.
bool DeviceAuthStore::TouchPresence(const char* id, int64_t now_ms) {
  char idkey[37];
  if (!kv_ || !id_key(id, idkey)) return false;
  for (unsigned attempt = 0; attempt < kRowWriteAttempts; ++attempt) {
    uint8_t snapshot[kMaxRecord], data[kMaxRecord];
    size_t snapshot_size = 0;
    uint32_t size = 0;
    DeviceRecord row{};
    if (load_row(kv_, idkey, id, now_ms, snapshot, &snapshot_size, &row) != storage::KvReadStatus::Found)
      return false;
    if (now_ms - row.last_seen_at_ms < 300000) return true;
    after_snapshot(kv_, "touch", id, now_ms);
    row.last_seen_at_ms = now_ms;
    if (!encode(row, data, &size)) return false;
    const storage::KvExpectation unchanged{idkey, snapshot, static_cast<uint32_t>(snapshot_size)};
    const storage::KvMutation mutation{idkey, data, size, retention(now_ms, row.token_expires_at_ms), false};
    const storage::KvCompareStatus status = kv_->CompareAndWrite(unchanged, now_ms, &mutation, 1);
    if (status == storage::KvCompareStatus::Committed) return true;
    if (status != storage::KvCompareStatus::ValueMismatch) return false;
  }
  return false;
}
}  // namespace fortis::auth
