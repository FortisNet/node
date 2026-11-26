#pragma once

#include <stdint.h>
#include <stddef.h>

#include "kv_store.h"

namespace fortis::auth {

enum class DevicePlatform : uint8_t { Ios = 1, Android, Macos, Windows, Linux };
enum class DeviceAttestation : uint8_t { Development = 1, AppleAppAttest, PlayIntegrity };
enum class DeviceQuotaTier : uint8_t { Development = 1, Standard, Attested, Premium };
enum class DeviceAuthResult { Valid, Unauthorized, SessionExpired,
                              AttestationRequired, StorageError };
// Bearer rotation outcomes (DeviceAuthStore::Rotate). Only Rotated fills the
// output, and only after the rotation frame committed. SessionExpired: the
// authenticated bearer is no longer current at the rotation instant (another
// rotation consumed it, the device was revoked or deleted, or the token
// expired); nothing written. Rejected: invalid arguments (nothing read or
// written). StorageError: a failed or unacknowledged KV read or append, an
// unreadable/oversized/foreign row, a token-hash collision or entropy
// failure; nothing acknowledged (see KvStore::CompareAndWrite for what replay
// may show after an unacknowledged append).
enum class DeviceRotateResult { Rotated, SessionExpired, Rejected, StorageError };
// Challenge-bound enrollment outcomes. ChallengeUnavailable: the challenge
// is absent, expired at `now_ms` or already consumed. ChallengeMismatch: the
// key holds something other than a minted challenge marker. Rejected:
// invalid arguments (nothing read or written). StorageError: not
// acknowledged; see KvStore::CompareAndWrite. KeyAlreadyAssociated: the
// proof's App Attest key is already associated with a device (nothing
// written; the challenge stays live).
enum class DeviceEnrollResult { Enrolled, ChallengeUnavailable, ChallengeMismatch,
                                Rejected, StorageError, KeyAlreadyAssociated };

// App Attest environments, as the authenticator data's AAGUID states them.
enum class AppleAttestEnvironment : uint8_t { Production = 1, Development = 2 };
// Largest App Attest receipt that is verified-and-stored. Apple's published
// sample receipt is 3,977 bytes.
constexpr uint32_t kAppleReceiptMaxBytes = 8192;
constexpr uint8_t kAppleBundleVersionMaxBytes = 32;

// The durable App Attest key association: "attest:apple:key:<hex key id>"
// written in the enrollment frame and never expired, so one attested key
// can back at most one device. It keeps what a later assertion check and the
// fraud-metric receipt exchange need.
struct AppleKeyAssociation {
  char device_id[27]{};
  uint8_t public_key[65]{};  // X9.62 uncompressed P-256 point.
  AppleAttestEnvironment environment = AppleAttestEnvironment::Production;
  uint32_t validation_category = 0;  // 0 with empty version: legacy object, no launch extensions.
  char bundle_version[kAppleBundleVersionMaxBytes + 1]{};
  uint8_t bundle_version_length = 0;
  uint32_t counter = 0;
  int64_t verified_at_ms = 0;
  uint8_t receipt[kAppleReceiptMaxBytes]{};
  uint32_t receipt_length = 0;
};

constexpr int64_t kDeviceChallengeTtlMs = 300000;
// "attest:challenge:" + `challenge`; false when the challenge is empty, holds
// NUL, or the key would not fit `capacity`.
bool DeviceChallengeKey(const char* challenge, size_t length, char* key, size_t capacity);

struct DeviceRecord {
  char id[27]{};
  char token_hash[65]{};
  char app_version[201]{};
  uint16_t app_version_length = 0;
  bool has_app_version = false;
  DevicePlatform platform = DevicePlatform::Ios;
  DeviceAttestation attestation = DeviceAttestation::Development;
  DeviceQuotaTier quota_tier = DeviceQuotaTier::Development;
  int64_t token_expires_at_ms = 0;
  int64_t revoked_at_ms = 0;
  int64_t last_seen_at_ms = 0;
  int64_t created_at_ms = 0;
  int64_t updated_at_ms = 0;
};

struct SnapshotAuthStats {
  uint64_t hits;
  uint64_t misses;
  uint64_t invalidations;
};
SnapshotAuthStats ReadSnapshotAuthStats();

struct DeviceCredentials {
  char id[27]{};
  char token[55]{};
  int64_t expires_at_ms = 0;
};

class DeviceAttestationIssuer;
class PlayIntegrityAttestationIssuer;

// The outcome of a server-side platform proof verification: Apple App Attest
// or Play Integrity evidence that a verifier has checked, bound to one
// server-minted challenge, platform and app version. There is no public
// constructor and no parser from request bytes: only the two friend issuers
// can build one, and each production issuer is defined in exactly one
// translation unit:
//   - DeviceAttestationIssuer (apple_app_attest.cpp) builds an
//     AppleAppAttest proof only after VerifyAppleAttestation returned
//     Verified for the request's own challenge (apple_app_attest.h).
//   - PlayIntegrityAttestationIssuer (play_integrity_enrollment.cpp) builds
//     a PlayIntegrity proof only after PlayIntegrityGoogleClient::Decode
//     returned Verified: Google's authenticated decodeIntegrityToken answer,
//     validated by the verdict validator against the requestHash the
//     server recomputed from (minted challenge, "android", package,
//     appVersion) (play_integrity_enrollment.h).
// Both run only on the required-mode POST /v1/devices path. Test binaries
// that link neither may define a stand-in DeviceAttestationIssuer. A
// provider name, verdict or token string sent by a client therefore can
// never become one of these.
class VerifiedDeviceAttestation {
 public:
  DeviceAttestation provider() const { return provider_; }
  DevicePlatform platform() const { return platform_; }
  const char* challenge() const { return challenge_; }
  const char* app_version() const { return app_version_; }
  size_t app_version_length() const { return app_version_length_; }
  bool has_app_version() const { return has_app_version_; }
  int64_t verified_at_ms() const { return verified_at_ms_; }
  // App Attest evidence (AppleAppAttest proofs only): the verified key id
  // (SHA-256 of the credential certificate's public key), that key, the
  // environment, optional launch validation category and bundle build number
  // from the authenticator data, and the receipt. Legacy objects report
  // category zero and an empty build. The receipt is borrowed from
  // the issuer's input and must outlive the EnrollAttested call.
  const uint8_t* apple_key_id() const { return apple_key_id_; }
  const uint8_t* apple_public_key() const { return apple_public_key_; }
  AppleAttestEnvironment apple_environment() const { return apple_environment_; }
  uint32_t apple_validation_category() const { return apple_validation_category_; }
  const char* apple_bundle_version() const { return apple_bundle_version_; }
  size_t apple_bundle_version_length() const { return apple_bundle_version_length_; }
  const uint8_t* apple_receipt() const { return apple_receipt_; }
  uint32_t apple_receipt_length() const { return apple_receipt_length_; }
  // Play Integrity evidence (PlayIntegrity proofs only), from the verified
  // verdict: appIntegrity.versionCode (>= the configured minimum, so >= 1)
  // and the device label, numerically PlayDeviceLabel (play_integrity.h):
  // 1 = MEETS_DEVICE_INTEGRITY, 2 = also MEETS_STRONG_INTEGRITY. Not
  // persisted on the device record; EnrollAttested refuses a PlayIntegrity
  // proof without them.
  int64_t play_version_code() const { return play_version_code_; }
  uint8_t play_device_label() const { return play_device_label_; }

 private:
  friend class DeviceAttestationIssuer;
  friend class PlayIntegrityAttestationIssuer;
  VerifiedDeviceAttestation() = default;
  DeviceAttestation provider_ = DeviceAttestation::Development;
  DevicePlatform platform_ = DevicePlatform::Ios;
  char challenge_[44]{};
  char app_version_[201]{};
  uint16_t app_version_length_ = 0;
  bool has_app_version_ = false;
  int64_t verified_at_ms_ = 0;
  uint8_t apple_key_id_[32]{};
  uint8_t apple_public_key_[65]{};
  AppleAttestEnvironment apple_environment_ = AppleAttestEnvironment::Production;
  uint32_t apple_validation_category_ = 0;
  char apple_bundle_version_[kAppleBundleVersionMaxBytes + 1]{};
  uint8_t apple_bundle_version_length_ = 0;
  const uint8_t* apple_receipt_ = nullptr;
  uint32_t apple_receipt_length_ = 0;
  int64_t play_version_code_ = 0;
  uint8_t play_device_label_ = 0;
};

// Borrowed single-writer KV. Development enrollment must be authorized by the
// HTTP layer's address/mode policy before it calls this repository.
class DeviceAuthStore {
 public:
  explicit DeviceAuthStore(storage::KvStore* kv) : kv_(kv) {}
  bool EnrollDevelopment(DevicePlatform platform, const char* app_version,
                         size_t app_version_length,
                         int64_t now_ms, int64_t ttl_ms, DeviceCredentials* output);
  // Development enrollment that consumes `challenge` (raw request bytes, no
  // NUL) in the same atomic frame as the new device record, so one challenge
  // enrolls at most one device and a failed enrollment leaves it unused.
  DeviceEnrollResult EnrollDevelopmentWithChallenge(
      const char* challenge, size_t challenge_length, DevicePlatform platform,
      const char* app_version, size_t app_version_length, int64_t now_ms,
      int64_t ttl_ms, DeviceCredentials* output);
  // Production enrollment from a verified proof only. Requires an Apple App
  // Attest proof for iOS or a Play Integrity proof for Android, `platform`
  // and the app version equal to what the proof bound, a proof verified no
  // later than `now_ms` and within one challenge lifetime of it, a token TTL
  // inside the development bounds and a Standard or Attested quota tier
  // (Development is not an attested tier; Premium is an entitlement no
  // platform proof grants). The proof's challenge is consumed in the same
  // frame as the record, after verification, so a replayed or concurrent
  // proof for one challenge enrolls at most one device. An AppleAppAttest
  // proof must carry its key association, which the same frame writes only
  // if no association for that key exists (else KeyAlreadyAssociated). A
  // PlayIntegrity proof must carry its verdict evidence (versionCode >= 1
  // and device label 1 or 2); nothing extra is written for it.
  // POST /v1/devices reaches this only through the Apple verifier
  // (apple_app_attest.h) or the Play Integrity enrollment
  // (play_integrity_enrollment.h) in required mode.
  DeviceEnrollResult EnrollAttested(const VerifiedDeviceAttestation& proof,
                                    DevicePlatform platform, const char* app_version,
                                    size_t app_version_length, DeviceQuotaTier quota_tier,
                                    int64_t now_ms, int64_t ttl_ms, DeviceCredentials* output);
  // Resolves a bearer to its device row at `now_ms`. Unauthorized: not a
  // device token, or no live hash mapping. SessionExpired: revoked, expired
  // at or before `now_ms`, or replaced by a rotation that committed between
  // this call's hash lookup and its row read (the row names another hash and
  // this bearer's hash key is gone). StorageError: unreadable or inconsistent
  // records. AttestationRequired: required mode and the row is not an
  // attested record whose provider matches its platform, unless Android
  // enrollment without attestation is enabled and the record is unverified Android.
  DeviceAuthResult Authenticate(const char* token, int64_t now_ms,
                                bool attestation_required, DeviceRecord* output,
                                bool android_attestation_required = true);
  // Snapshot authorization may reuse a verified positive result briefly.
  // Rotate and Revoke invalidate it before committing, and store recovery
  // invalidates the entire old cache epoch. The quota check still runs.
  DeviceAuthResult AuthenticateSnapshot(const char* token, int64_t now_ms,
                                        bool attestation_required, DeviceRecord* output,
                                        bool android_attestation_required = true);
  // Replaces the bearer of `old` (the row Authenticate returned for the
  // request's bearer) with a fresh one valid for `ttl_ms` from `now_ms`.
  // One conditional frame (KvStore::CompareAndWrite) requires the device row
  // to still equal, byte for byte, a snapshot read at `now_ms` that holds
  // `old.token_hash`, is not revoked and expires after `now_ms`, and requires
  // the new hash key to be absent; it then erases the old hash key, rewrites
  // the row and adds the new hash key. So of any number of concurrent calls
  // for one current bearer exactly one returns Rotated, and a revoke or
  // rotation that lands first wins over a later one. A row that changed
  // without consuming the bearer (a presence touch) is re-read and retried.
  // Everything but the hash, token expiry and updated_at is preserved
  // (id, platform, attestation, tier, app version, created/last-seen); the
  // App Attest key association is not touched. See DeviceRotateResult.
  // POST /v1/devices/rotate (device_enrollment.cpp handle_rotation).
  DeviceRotateResult Rotate(const DeviceRecord& old, int64_t now_ms, int64_t ttl_ms,
                            DeviceCredentials* output);
  // Marks the device revoked at `now_ms` with a conditional write against
  // the row it read, retried when the row changed meanwhile, so a revoke
  // never restores a bearer that a concurrent rotation replaced. False when
  // the row is missing, unreadable or the write fails.
  bool Revoke(const char* id, int64_t now_ms);
  // Records presence when the last record is at least five minutes old, with
  // the same conditional write as Revoke. Not called by a production route.
  bool TouchPresence(const char* id, int64_t now_ms);
  // Read-only challenge check: Found when `challenge` (raw bytes, no NUL)
  // holds the minted marker live at `now_ms`; Missing when it is absent,
  // expired, consumed or holds anything else; IoError otherwise.
  storage::KvReadStatus PeekChallenge(const char* challenge, size_t length, int64_t now_ms);
  // Reads the App Attest association for `key_id`: Found, Missing, or
  // IoError for an unreadable or undecodable record.
  storage::KvReadStatus LoadAppleKeyAssociation(const uint8_t key_id[32], int64_t now_ms,
                                       AppleKeyAssociation* output);
#if defined(FORTIS_PRICE_ROUTE_TEST_ONLY) || defined(FORTIS_NETWORK_ROUTE_TEST_ONLY) || \
    defined(FORTIS_HISTORY_ROUTE_TEST_ONLY) || defined(FORTIS_OPERATIONS_TEST_ONLY) || \
    defined(FORTIS_DISCOVERY_ROUTE_TEST_ONLY) || defined(FORTIS_NEWS_ROUTE_TEST_ONLY) || \
    defined(FORTIS_ROBINHOOD_ROUTE_TEST_ONLY)
  bool SeedPriceRouteFixture(const char* token, const char* id, int64_t now_ms,
                             int64_t token_ttl_ms = 3600000);
#endif
#if defined(FORTIS_ADDRESS_ROUTE_TEST_ONLY) || defined(FORTIS_AI_ROUTE_TEST_ONLY) || \
    defined(FORTIS_CROSS_CHAIN_ROUTE_TEST_ONLY) || defined(FORTIS_ROBINHOOD_ROUTE_TEST_ONLY) || \
    defined(FORTIS_DASHPAY_ROUTE_TEST_ONLY)
  // The address-route fixture device: premium quota tier, as the TypeScript
  // contract harness enrols it.
  bool SeedAddressRouteFixture(const char* token, const char* id, int64_t now_ms);
#endif

 private:
  DeviceAuthResult AuthenticateHash(const char* hash, int64_t now_ms,
                                    bool attestation_required, DeviceRecord* output,
                                    bool android_attestation_required);
  DeviceEnrollResult Enroll(const char* challenge, size_t challenge_length,
                            DevicePlatform platform, DeviceAttestation attestation,
                            DeviceQuotaTier quota_tier, const char* app_version,
                            size_t app_version_length, int64_t now_ms, int64_t ttl_ms,
                            DeviceCredentials* output,
                            const VerifiedDeviceAttestation* apple = nullptr);
  bool LoadById(const char* id, int64_t now_ms, DeviceRecord* output);
  storage::KvStore* kv_;
};

#ifdef FORTIS_DEVICE_ROTATION_TEST_ONLY
// Test-only seams for tests/device_rotation_test.cpp and the
// build/fortis_node_rotation_test server; never compiled into
// build/fortis_node or the guest (a compile error there). `hook` runs
// in Rotate, Revoke and TouchPresence after the row snapshot is read and
// before the conditional write, with `operation` "rotate", "revoke" or
// "touch", and in Authenticate between the hash lookup and the row read
// ("authenticate"). Snapshot auth reports "snapshot_auth_verified" after a
// valid store read, before caching it. Without a hook, Rotate sleeps FORTIS_TEST_ROTATION_PAUSE_MS
// milliseconds at that point, and with FORTIS_TEST_ROTATION_COMPETE set it
// commits a competing rotation of the same row there (see after_snapshot).
using DeviceRowSnapshotHook = void (*)(const char* operation, const char* id, void* context);
void SetDeviceRowSnapshotHook(DeviceRowSnapshotHook hook, void* context);
// Rotate mints `token` (a valid device token) instead of a random bearer
// until called again with nullptr: collision and crash-image tests.
void SetDeviceRotationTokenForTest(const char* token);
#endif

bool IsDeviceToken(const char* token);
// The Apple (iOS and macOS) exception to required mode, set once at startup
// before HTTP workers start. False lets unverified Apple device records pass
// required-mode authentication, as the Android exception does for Android.
void ConfigureAppleAttestation(bool required);
bool AppleAttestationRequired();
// The same exception for desktop platforms (Windows and Linux).
void ConfigureDesktopAttestation(bool required);
bool DesktopAttestationRequired();
// "attest:apple:key:" + 64 lowercase hex digits of `key_id`.
void AppleKeyAssociationKey(const uint8_t key_id[32], char output[82]);
bool HashDeviceToken(const char* token, char output[65]);
bool SecureRandomBytes(uint8_t* output, size_t length);
uint32_t DeviceQuotaPerMinute(DeviceQuotaTier tier);

}  // namespace fortis::auth
