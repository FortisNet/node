#pragma once

// Apple App Attest server verification for required-mode enrollment, after
// https://developer.apple.com/documentation/devicecheck/validating-apps-that-connect-to-your-server
//
// DeviceAttestation.token for provider "apple-app-attest" (version 1) is
// one canonical base64url string (RFC 4648 section 5 alphabet, no padding,
// no whitespace, zero unused trailing bits) of this binary envelope:
//
//   offset  size  field
//   0       1     version, 0x01
//   1       32    key id: the raw 32 bytes DCAppAttestService returned in
//                 base64 (SHA-256 of the attested public key)
//   33      1     n: bundle build length, 1..32
//   34      n     bundle build: the iOS CFBundleVersion (Flutter's build
//                 number, not the marketing appVersion), ASCII digits and
//                 single interior dots, at most three components
//   34+n    4     m: attestation object length, big-endian, 1..16384
//   38+n    m     the CBOR attestation object from attestKey
//
// and nothing after it. Fixed-width fields replace delimiters, so there is
// one parse. Any other version, a non-canonical or oversized string, a
// short or long envelope, or a bad field is TokenMalformed. The challenge
// is never in the token: it is DeviceAttestation.challenge, and the app must
// pass SHA-256(challenge as UTF-8) as attestKey's clientDataHash.
//
// The marketing appVersion stays the separate EnrollDeviceRequest field;
// App Attest does not sign it. Required-mode Apple enrollment requires it
// to be present, records it on the device as before, and records the
// signed bundle build beside the key association when the attestation has
// the iOS 27 launch extensions. Older attestation objects carry neither
// launch category nor signed build; the key association records both as
// unknown (zero/empty), rather than treating the envelope build as signed.

#include <stddef.h>
#include <stdint.h>

#include "device_auth.h"

namespace fortis::auth {

constexpr size_t kAppleAttestationMaxBytes = 16384;
constexpr size_t kAppleCertificateMaxBytes = 4096;
constexpr size_t kAppleAuthDataMaxBytes = 1024;
constexpr size_t kAppleEnvelopeMaxBytes =
    1 + 32 + 1 + kAppleBundleVersionMaxBytes + 4 + kAppleAttestationMaxBytes;
constexpr size_t kAppleTokenMaxChars = (kAppleEnvelopeMaxBytes * 8 + 5) / 6;
// Concurrent proof verifications admitted process-wide; more answer Busy.
constexpr int kAppleVerifyConcurrency = 2;

// The app's App ID (team id "." bundle id); its SHA-256 is the RP ID.
constexpr char kAppleAppId[] = "C7Z98WBK8H.io.fortisnet.wallet";

// The official Apple App Attestation Root CA, DER, from
// https://www.apple.com/certificateauthority/Apple_App_Attestation_Root_CA.pem
// (SHA-256 1cb9823b a28ba6ad 2d33a006 941de2ae 4f513ef1 d4e831b9 f7e0fa7b
// 6242c932, valid 2020-03-18 to 2045-03-15). The verifier re-checks the
// fingerprint on every use.
extern const uint8_t kAppleAppAttestRootDer[];
extern const size_t kAppleAppAttestRootDerLength;
extern const uint8_t kAppleAppAttestRootSha256[32];

enum class AppleAttestStatus : uint8_t {
  Verified,
  TokenMalformed,         // envelope encoding, version or field bounds
  AttestationMalformed,   // CBOR syntax, shape, bounds or trailing bytes
  FormatUnsupported,      // fmt is not "apple-appattest"
  AuthDataMalformed,      // authenticator data layout, flags or COSE key
  ExtensionsMalformed,    // authenticator extensions map
  CertificateMalformed,   // x5c DER, extensions or key type
  CertificateUntrusted,   // chain, signature, profile or path to the root
  CertificateNotValidNow, // a chain certificate outside its validity at now
  CertificateUsage,       // CA flags, key usage, EKU or path length
  NonceMismatch,          // credCert nonce != SHA-256(authData || clientDataHash)
  KeyIdMismatch,          // SHA-256(credCert public key) != key id
  CredentialIdMismatch,   // authData credentialId or COSE key != credCert key
  RpIdMismatch,           // RP ID hash != SHA-256(App ID)
  CounterNotZero,
  EnvironmentMismatch,    // AAGUID is not the configured environment's
  CategoryRejected,       // apple_validation_category_01 not admitted
  BundleVersionMismatch,  // apple_bundle_version_01 != the envelope's build
  Internal,               // allocation, pinned-root or library failure
};
const char* AppleAttestStatusName(AppleAttestStatus status);

// A parsed envelope. `attestation` points into the caller's decode buffer.
struct AppleAttestToken {
  uint8_t key_id[32]{};
  char bundle_version[kAppleBundleVersionMaxBytes + 1]{};
  uint8_t bundle_version_length = 0;
  const uint8_t* attestation = nullptr;
  size_t attestation_length = 0;
};
// Decodes `token` into `envelope` (at least kAppleEnvelopeMaxBytes) and
// parses it. Verified or TokenMalformed.
AppleAttestStatus ParseAppleAttestToken(const char* token, size_t length, uint8_t* envelope,
                                        size_t capacity, AppleAttestToken* output);
// True for 1..32 ASCII digits in one to three dot-separated components.
bool IsAppleBundleVersion(const char* text, size_t length);

// Launch validation categories (bit n admits category n): production admits
// TestFlight (2) and App Store (4); development admits development-signed
// executables (3). Every other category in an extended object is rejected.
// Legacy objects have no category extension to check.
uint32_t AppleAllowedCategories(AppleAttestEnvironment environment);

struct AppleAttestPolicy {
  const char* app_id = kAppleAppId;
  size_t app_id_length = sizeof(kAppleAppId) - 1;
  AppleAttestEnvironment environment = AppleAttestEnvironment::Production;
  uint32_t allowed_categories = 0;
  const uint8_t* root_der = nullptr;
  size_t root_der_length = 0;
};

// What a successful verification established. `receipt` points into the
// token's attestation object.
struct AppleAttestEvidence {
  uint8_t key_id[32]{};
  uint8_t public_key[65]{};
  AppleAttestEnvironment environment = AppleAttestEnvironment::Production;
  uint32_t validation_category = 0;  // 0 with empty version: legacy object, no launch extensions.
  char bundle_version[kAppleBundleVersionMaxBytes + 1]{};
  uint8_t bundle_version_length = 0;
  const uint8_t* receipt = nullptr;
  uint32_t receipt_length = 0;
};

// The verification steps of Apple's guide over one attestation object, at
// `now_ms` for certificate validity. `client_data_hash` is what the app gave
// attestKey; production passes SHA-256(challenge). Builds no verdict and
// touches no storage.
AppleAttestStatus VerifyAppleAttestation(const AppleAttestPolicy& policy,
                                         const AppleAttestToken& token,
                                         const uint8_t* client_data_hash,
                                         size_t client_data_hash_length, int64_t now_ms,
                                         AppleAttestEvidence* output);

enum class AppleEnrollStatus : uint8_t {
  Enrolled,
  InvalidProof,          // token, verification or a missing appVersion
  ChallengeUnavailable,  // not a minted challenge, expired or consumed
  KeyAlreadyAssociated,  // the attested key already backs a device
  Busy,                  // kAppleVerifyConcurrency verifications in flight
  StorageError,
  Rejected,              // EnrollAttested refused the arguments (e.g. TTL)
};
struct AppleEnrollOutcome {
  AppleEnrollStatus status = AppleEnrollStatus::StorageError;
  AppleAttestStatus verification = AppleAttestStatus::Internal;
};

// Required-mode iOS enrollment: checks the challenge is a live minted one
// (read only), verifies the token against SHA-256(challenge), the pinned
// official root, kAppleAppId and `environment`, and only then issues the
// verdict and calls DeviceAuthStore::EnrollAttested at the Attested tier,
// whose one conditional frame consumes the challenge, creates the device
// and writes the key association. Nothing is written on any failure.
AppleEnrollOutcome EnrollWithAppleAppAttest(DeviceAuthStore& auth,
                                            AppleAttestEnvironment environment,
                                            const char* challenge, size_t challenge_length,
                                            const char* token, size_t token_length,
                                            const char* app_version, size_t app_version_length,
                                            int64_t now_ms, int64_t ttl_ms,
                                            DeviceCredentials* output);

// The process-wide verification gate (exposed for tests).
bool TryAcquireAppleVerifySlot();
void ReleaseAppleVerifySlot();

#ifdef FORTIS_APPLE_ATTEST_TEST_ROOT
// Test builds only: EnrollWithAppleAppAttest trusts the DER root named by
// FORTIS_TEST_APPLE_ATTEST_ROOT instead of the official one. Release and
// guest builds never compile this.
constexpr char kAppleTestRootEnvironment[] = "FORTIS_TEST_APPLE_ATTEST_ROOT";
#endif

}  // namespace fortis::auth
