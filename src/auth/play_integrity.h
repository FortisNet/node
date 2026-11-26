#pragma once

// Google Play Integrity (standard requests), the verdict validator:
// the enrollment requestHash binding and a pure validator for the verdict
// that Google's decodeIntegrityToken endpoint returns, after
// https://developer.android.com/google/play/integrity/standard
// https://developer.android.com/google/play/integrity/verdicts
// and the v1 discovery document's TokenPayloadExternal schema.
//
// Nothing here talks to the network, holds a credential, builds a device
// verdict or touches storage. The production caller is the Google decode
// client (play_integrity_google.h), reached from required-mode Android
// enrollment (play_integrity_enrollment.h). The validator
// only accepts a PlayIntegrityDecodedResponse, which a client cannot send:
// only PlayIntegrityDecodeSource (the decode client; test stand-ins in
// test binaries) can construct one.
//
// ---- Enrollment requestHash, version 1 ---------------------------------
//
// The app computes this after POST /v1/devices/challenge and passes it as
// StandardIntegrityTokenRequest.setRequestHash; the server recomputes it
// from the enrollment request and compares it with the decoded
// requestDetails.requestHash. It binds the token to this one enrollment
// request. Replay is still refused by the server's one-use challenge.
//
//   LP(x)     = uint32 big-endian byte length of x || the bytes of x
//   preimage  = LP("fortisnet.play-integrity.enroll-request.v1")
//            || LP(challenge)    the exact 43-character challenge text the
//                                server minted, as ASCII bytes
//            || LP(platform)     "android"
//            || LP(package)      the Android application id, ASCII
//                                ("io.fortisnet.wallet" today)
//            || LP(appVersion)   the exact UTF-8 bytes the app sends as
//                                EnrollDeviceRequest.appVersion
//   requestHash = base64url(SHA-256(preimage)), RFC 4648 section 5
//                 alphabet, no padding: always 43 characters
//
// The version is part of the domain string; a later layout must change it.
// Inputs are refused, never normalized: the challenge must be the minted
// form (43 base64url characters whose last one carries zero unused bits),
// the platform exactly "android", the package a dotted Java identifier
// (kPlayPackageNameMaxBytes at most), and the appVersion 1 to
// kPlayAppVersionMaxBytes bytes of well-formed UTF-8 without NUL or other
// control characters (U+0000-U+001F, U+007F). The marketing appVersion is
// chosen by the app; the hash shows only that the Google-recognized app
// binary that requested the token reported it. Google's signed fact about
// the build is appIntegrity.versionCode.

#include <stddef.h>
#include <stdint.h>

namespace fortis::auth {

constexpr char kPlayIntegrityPackageName[] = "io.fortisnet.wallet";
constexpr char kPlayRequestHashDomain[] = "fortisnet.play-integrity.enroll-request.v1";
constexpr char kPlayRequestPlatform[] = "android";
constexpr size_t kPlayRequestHashChars = 43;
constexpr size_t kPlayChallengeChars = 43;
constexpr size_t kPlayPackageNameMaxBytes = 255;
constexpr size_t kPlayAppVersionMaxBytes = 200;

enum class PlayRequestHashStatus : uint8_t {
  Ok,
  ChallengeMalformed,  // not the minted 43-character canonical base64url form
  PlatformInvalid,     // not exactly "android"
  PackageInvalid,      // not a dotted Java identifier of at most 255 bytes
  AppVersionInvalid,   // empty, too long, ill-formed UTF-8 or a control
  Internal,            // the SHA-256 implementation failed
};
const char* PlayRequestHashStatusName(PlayRequestHashStatus status);

// The version 1 requestHash above, written to `output` as 43 characters
// and a NUL. On any other status `output` is the empty string.
PlayRequestHashStatus ComputePlayIntegrityRequestHash(
    const char* challenge, size_t challenge_length, const char* platform,
    size_t platform_length, const char* package_name, size_t package_name_length,
    const char* app_version, size_t app_version_length,
    char output[kPlayRequestHashChars + 1]);

// Two or more '.'-separated segments of [A-Za-z][A-Za-z0-9_]*, 1 to
// kPlayPackageNameMaxBytes bytes in total.
bool IsPlayPackageName(const char* text, size_t length);

// ---- Release signing certificate (policy input) ------------------------
//
// The configured digest is the SHA-256 of the app signing certificate Play
// signs releases with (Play Console > App integrity > App signing). The
// operator states which text form is being supplied; nothing is guessed:
//   HexColon   "AB:CD:..."  32 colon-separated pairs, either case (the
//              form the Play Console and keytool print)
//   Hex        64 hex digits, either case
//   Base64Url  43 characters, RFC 4648 section 5, no padding, zero unused
//              bits (the form Google writes into certificateSha256Digest)
enum class PlayCertificateDigestFormat : uint8_t { HexColon, Hex, Base64Url };
bool ParsePlayCertificateDigest(const char* text, size_t length,
                                PlayCertificateDigestFormat format, uint8_t output[32]);

// ---- Decoded verdict ---------------------------------------------------
//
// Fixed bounds: one verification reads at most kPlayVerdictMaxBytes of
// input into a kPlayVerdictMaxTokens-token arena (about 12 KiB of stack)
// with nesting depth at most kBoundedJsonMaxDepth, does work linear in the
// input times a constant field count, allocates nothing and keeps nothing.
// A real payload is about 1 KiB and under 100 tokens.
constexpr size_t kPlayVerdictMaxBytes = 16384;
constexpr size_t kPlayVerdictMaxTokens = 512;
constexpr size_t kPlayPolicyMaxCertificates = 4;
constexpr size_t kPlayVerdictMaxCertificates = 8;
constexpr size_t kPlayVerdictMaxDeviceLabels = 16;
constexpr size_t kPlayVerdictMaxLabelBytes = 64;
// Defaults: a token may be as old as one challenge lifetime
// (kDeviceChallengeTtlMs) and stamped at most one minute in the future.
constexpr int64_t kPlayDefaultMaxAgeMs = 300000;
constexpr int64_t kPlayDefaultMaxFutureSkewMs = 60000;
constexpr int64_t kPlayMaxAgeLimitMs = 3600000;
constexpr int64_t kPlayMaxFutureSkewLimitMs = 300000;

struct PlayIntegrityPolicy {
  const char* package_name = kPlayIntegrityPackageName;
  size_t package_name_length = sizeof(kPlayIntegrityPackageName) - 1;
  // 1..kPlayPolicyMaxCertificates accepted release certificate digests
  // (ParsePlayCertificateDigest); more than one only for a key rotation.
  const uint8_t (*certificates)[32] = nullptr;
  size_t certificate_count = 0;
  int64_t min_version_code = 1;
  // Accepted when -max_future_skew_ms <= now_ms - timestampMillis <= max_age_ms.
  int64_t max_age_ms = kPlayDefaultMaxAgeMs;
  int64_t max_future_skew_ms = kPlayDefaultMaxFutureSkewMs;
  // accountDetails.appLicensingVerdict must be LICENSED. Off by default:
  // the enrollment gate is app and device integrity; the verdict is
  // reported in the evidence either way.
  bool require_play_license = false;
  // Accept payloads carrying testingDetails (Play Console tester
  // overrides). Never in production.
  bool allow_testing_response = false;
};

enum class PlayIntegrityStatus : uint8_t {
  Verified,
  PolicyInvalid,        // bad policy, expected hash, now_ms or output
  PayloadMalformed,     // size, JSON syntax, depth or token bounds, a wrong
                        // type, a duplicate or snake_case alias of a field
                        // the validator reads, an out-of-bounds string or
                        // array, a non-canonical integer string
  PackageMismatch,      // requestPackageName or packageName absent or wrong
  RequestHashMismatch,  // requestHash absent or wrong, or a classic nonce
  TimestampMissing,     // requestDetails.timestampMillis absent
  TimestampStale,       // older than max_age_ms
  TimestampFuture,      // more than max_future_skew_ms ahead of now_ms
  TestingResponse,      // testingDetails present and not allowed
  AppNotRecognized,     // appRecognitionVerdict absent or not PLAY_RECOGNIZED
  CertificateMismatch,  // digest list absent, empty or without a policy digest
  VersionCodeRejected,  // versionCode absent or below min_version_code
  LicenseRequired,      // require_play_license and not LICENSED
  DeviceIntegrityFailed,// deviceRecognitionVerdict absent, empty or without
                        // MEETS_DEVICE_INTEGRITY
};
// Constant names for logs and metrics; they never include payload text.
const char* PlayIntegrityStatusName(PlayIntegrityStatus status);

enum class PlayDeviceLabel : uint8_t {
  None,
  DeviceIntegrity,  // MEETS_DEVICE_INTEGRITY
  StrongIntegrity,  // MEETS_DEVICE_INTEGRITY and MEETS_STRONG_INTEGRITY
};
enum class PlayLicensing : uint8_t { Absent, Licensed, Unlicensed, Unevaluated, Other };

// What a Verified call established, for the attestation issuer. Holds no
// payload text.
struct PlayIntegrityEvidence {
  int64_t version_code = 0;
  int64_t timestamp_ms = 0;
  PlayDeviceLabel device_label = PlayDeviceLabel::None;
  PlayLicensing licensing = PlayLicensing::Absent;
};

class PlayIntegrityDecodeSource;

// The body of a successful decodeIntegrityToken HTTPS response
// ({"tokenPayloadExternal": {...}}), borrowed. There is no public
// constructor: only PlayIntegrityDecodeSource can wrap bytes, so request
// body bytes cannot reach the validator. The one production definition of
// PlayIntegrityDecodeSource is in play_integrity_google.cpp.
class PlayIntegrityDecodedResponse {
 public:
  const uint8_t* body() const { return body_; }
  size_t length() const { return length_; }

 private:
  friend class PlayIntegrityDecodeSource;
  PlayIntegrityDecodedResponse(const uint8_t* body, size_t length)
      : body_(body), length_(length) {}
  const uint8_t* body_;
  size_t length_;
};

// Validates one decoded response against `policy`, the requestHash the
// server computed for this enrollment (ComputePlayIntegrityRequestHash)
// and the server clock. In order: structure and types; requestDetails
// (package, requestHash, no nonce, timestamp window); testingDetails;
// appIntegrity (PLAY_RECOGNIZED, package, certificate, versionCode);
// accountDetails if required; deviceIntegrity. Every field the validator
// reads must appear at most once under its camelCase name and with
// Google's JSON type (int64 values as decimal strings). Other fields and
// unknown enum strings are ignored and never satisfy a check. `output` is
// reset first and filled only on Verified.
PlayIntegrityStatus VerifyPlayIntegrityVerdict(const PlayIntegrityPolicy& policy,
                                               const PlayIntegrityDecodedResponse& response,
                                               const char* expected_request_hash,
                                               size_t expected_request_hash_length,
                                               int64_t now_ms, PlayIntegrityEvidence* output);

}  // namespace fortis::auth
