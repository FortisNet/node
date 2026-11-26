#pragma once

// Google Play Integrity, required-mode Android
// enrollment. This joins the verdict validator (play_integrity.h) and the
// Google decode client (play_integrity_google.h) to the one-use enrollment
// challenge and the atomic attested-device write (device_auth.h):
//
//   POST /v1/devices {"platform":"android","appVersion":V,
//                     "attestation":{"provider":"play-integrity",
//                                    "challenge":C,"token":T}}
//
//   1. C must be a live server-minted challenge (read-only precheck, before
//      any Google I/O).
//   2. PlayIntegrityGoogleClient::Decode sends T to Google's authenticated
//      decodeIntegrityToken endpoint and validates the answer
//      against the requestHash the server recomputes from (C, "android",
//      kPlayIntegrityPackageName, V) and the configured certificate,
//      versionCode and time policy.
//   3. Only a Verified decode makes PlayIntegrityAttestationIssuer (defined
//      in play_integrity_enrollment.cpp alone) build a PlayIntegrity
//      VerifiedDeviceAttestation, and DeviceAuthStore::EnrollAttested
//      consumes C and creates the Android/PlayIntegrity/Attested device in
//      one conditional KV frame. Of racing requests for one C at most one
//      device is created; every failure before that frame leaves C live.
//
// The integrity token T is burned in the client's replay ring on its first
// submission, whatever Google answers, so a retry after any failure needs a
// new standard token from the app (the challenge itself stays usable).
//
// The header deliberately does not include play_integrity_google.h: that
// class's layout changes with FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT, so only
// translation units compiled with the same flag as the client may see it.
// Everything here takes the client as an opaque pointer.

#include <stddef.h>
#include <stdint.h>

#include "clock.h"
#include "device_auth.h"
#include "config_environment.h"
#include "play_integrity.h"

namespace fortis::auth {

class PlayIntegrityGoogleClient;

// ---- Runtime configuration -------------------------------------------------
//
// Read once at startup from the process environment (LoadPlayIntegrityConfig),
// never in a request handler. Android enrollment is enabled only when the
// variables below are present and valid; when none is present it stays off
// and required-mode Android enrollment answers 401 ATTESTATION_REQUIRED
// while iOS App Attest is unaffected. When any is present the
// whole set must be valid, or the server refuses to start with a message
// naming the variable (the value is never printed).
//
//   FORTIS_PLAY_INTEGRITY_CREDENTIAL_FILE   required
//       Absolute path of the Google service-account JSON key file. The key is
//       delivered only as a runtime file (a mounted secret or volume): never
//       in an environment value, image, source tree, fixture or log. The file
//       must be a regular file (not a symlink) owned by the server's
//       effective user with mode 0600 or stricter, at most 16 KiB
//       (LoadPlayServiceAccount enforces this at startup). A value that is
//       not an absolute path (for example pasted JSON) is refused.
//   FORTIS_PLAY_INTEGRITY_CERT_SHA256       required
//       1 to 4 comma-separated SHA-256 digests of the Play app signing
//       certificate (Play Console > App integrity > App signing), no spaces,
//       no duplicates. More than one only during a signing-key rotation.
//   FORTIS_PLAY_INTEGRITY_CERT_SHA256_FORMAT   required
//       The text form of every digest above (nothing is guessed):
//       "hex-colon" (AB:CD:..., as Play Console and keytool print it),
//       "hex" (64 hex digits) or "base64url" (43 characters, unpadded).
//   FORTIS_PLAY_INTEGRITY_MIN_VERSION_CODE  required
//       Lowest accepted appIntegrity.versionCode: decimal 1..2100000000
//       (Android's maximum), no sign, leading zero or whitespace.
//   FORTIS_PLAY_INTEGRITY_CA_FILE           optional
//       Absolute path of the PEM trust bundle for oauth2.googleapis.com and
//       playintegrity.googleapis.com. Defaults to FORTIS_CA_BUNDLE_PATH; one
//       of the two must be set. Validated at startup.
//   FORTIS_PLAY_INTEGRITY_MAX_AGE_SECONDS   optional, default 300
//       Oldest accepted requestDetails.timestampMillis, 1..3600 seconds.
//   FORTIS_PLAY_INTEGRITY_MAX_FUTURE_SKEW_SECONDS   optional, default 60
//       Largest accepted clock lead of that timestamp, 0..300 seconds.
//
// The package name is fixed (kPlayIntegrityPackageName), tester responses
// (testingDetails) are always refused and the Google endpoints are
// compile-time constants: none of them is configurable.

constexpr size_t kPlayConfigPathMaxBytes = 1023;
constexpr int64_t kPlayMaxVersionCode = 2100000000;

struct PlayIntegrityRuntimeConfig {
  bool configured = false;
  char credential_file[kPlayConfigPathMaxBytes + 1]{};
  char ca_file[kPlayConfigPathMaxBytes + 1]{};
  uint8_t certificates[kPlayPolicyMaxCertificates][32]{};
  size_t certificate_count = 0;
  int64_t min_version_code = 0;
  int64_t max_age_ms = kPlayDefaultMaxAgeMs;
  int64_t max_future_skew_ms = kPlayDefaultMaxFutureSkewMs;
};

// Parses the variables above through `lookup`. `fallback_ca_file` is the
// configured FORTIS_CA_BUNDLE_PATH or nullptr. True with `configured` false
// when no FORTIS_PLAY_INTEGRITY_* variable is present; true with
// `configured` set for a complete valid set; false with a bounded message in
// `error` otherwise. Performs no file I/O (see InitializePlayIntegrityClient).
bool LoadPlayIntegrityConfig(config::NativeEnvLookup lookup, void* context,
                             const char* fallback_ca_file, PlayIntegrityRuntimeConfig* output,
                             char* error, size_t error_capacity);

// Validates the CA bundle, then PlayIntegrityGoogleClient::Initialize loads
// the credential file and copies the policy into `client`. False with a
// message naming the refused variable (and the constant credential status
// name) otherwise. `config.configured` must be true.
bool InitializePlayIntegrityClient(const PlayIntegrityRuntimeConfig& config,
                                   PlayIntegrityGoogleClient* client, char* error,
                                   size_t error_capacity);

// ---- Process runtime ---------------------------------------------------------
//
// main() calls StartPlayIntegrityRuntime once, after LoadNativeConfig and
// OutboundHttpsInitialize and before any route is registered or worker
// started; StopPlayIntegrityRuntime once after every HTTP worker has been
// joined. Between the two, RuntimePlayIntegrityClient returns the one
// process-wide client (thread-safe; see PlayIntegrityGoogleClient::Decode),
// or nullptr when Android enrollment is not configured. Start with an
// unconfigured `config` succeeds and leaves the client absent; a configured
// one that fails InitializePlayIntegrityClient returns false (main exits).
//
// Host test builds of this file (FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT, never
// a guest or build/fortis_node) also read FORTIS_TEST_PLAY_INTEGRITY_HOST
// and FORTIS_TEST_PLAY_INTEGRITY_PORT here and point both Google requests at
// that loopback TLS stub; see tests/play_integrity_enrollment_socket_test.py.
bool StartPlayIntegrityRuntime(const PlayIntegrityRuntimeConfig& config, char* error,
                               size_t error_capacity);
PlayIntegrityGoogleClient* RuntimePlayIntegrityClient();
void StopPlayIntegrityRuntime();

// ---- Route admission -----------------------------------------------------------
//
// POST /v1/devices runs on one of main.cpp's kHttpWorkerCount (4) HTTP
// workers, outside the router's handler mutex, and a Play enrollment holds
// its worker for the whole Google exchange (up to kPlayDecodeDeadlineMs,
// 25 s, during an outage). If every worker held one, nothing else, not even
// /healthz, would be served until Google answered. The route therefore admits
// at most kPlayEnrollRouteMaxConcurrent Play enrollments at once:
//
//   kPlayEnrollRouteMaxConcurrent (3) < kHttpWorkerCount (4)
//     at least one worker is never waiting on Google (static_assert in
//     main.cpp, where the worker count lives);
//   kPlayEnrollRouteMaxConcurrent (3) <= kPlayDecodeMaxConcurrent (4)
//     the route, not the client, is the binding limit for HTTP. The client's
//     own four-slot cap stays as defence in depth and for non-HTTP callers
//     (static_assert in play_integrity_enrollment.cpp). With these values
//     its capacity Busy cannot be reached through the route alone.
//
// One fixed-size, process-local counter owned by play_integrity_enrollment.cpp
// (shared by host and guest builds) holds the admitted count. It is changed
// only with atomic compare-and-swap / fetch-sub: no mutex or condition
// variable, so admission never waits and nothing is held across Google I/O
// except the count itself. A request that finds every slot taken is answered
// 503 ATTESTATION_BUSY (Retry-After: 1) at once, before any KV read, Google
// request or replay-ring record, so its challenge and integrity token stay
// usable for a retry.
//
// enroll_play_integrity (device_enrollment.cpp) holds a PlayEnrollRouteSlot
// from just after its platform/token checks until it returns, so the slot is
// released on every outcome (Enrolled, refusals, Busy/Unavailable, storage
// and clock failures). The route writes its answer into the dispatch's
// response capture, so the slot is free before any byte reaches the client.
// This reserves a worker from Play only: other slow routes can still occupy
// every worker.
constexpr unsigned kPlayEnrollRouteMaxConcurrent = 3;

// RAII admission: admitted() is true when this object took one of the
// kPlayEnrollRouteMaxConcurrent slots, which its destructor returns. A
// refused object holds nothing and releases nothing. Not copyable or movable.
class PlayEnrollRouteSlot {
 public:
  PlayEnrollRouteSlot();
  ~PlayEnrollRouteSlot();
  PlayEnrollRouteSlot(const PlayEnrollRouteSlot&) = delete;
  PlayEnrollRouteSlot& operator=(const PlayEnrollRouteSlot&) = delete;
  bool admitted() const { return admitted_; }

 private:
  bool admitted_;
};

// Slots currently held (0..kPlayEnrollRouteMaxConcurrent); for tests and
// diagnostics, a snapshot that may be stale by the time it is read.
unsigned PlayEnrollRouteSlotsInUse();

// ---- Enrollment ----------------------------------------------------------------

enum class PlayEnrollStatus : uint8_t {
  Enrolled,
  NotConfigured,         // no initialized client: nothing read or sent
  InvalidProof,          // missing/oversized appVersion or token, malformed
                         // token, replayed token, Google 400, or a verdict
                         // the validator refused (hash, package, time,
                         // testing, app recognition, certificate,
                         // versionCode, device)
  ChallengeUnavailable,  // not a minted challenge, expired or consumed
  Busy,                  // decode concurrency/replay capacity, Google 429
  Unavailable,           // OAuth/credential, Google 401/403/5xx/other,
                         // TLS, transport, timeout or a malformed 2xx body
  StorageError,          // KV read/write or clock failure
  Rejected,              // EnrollAttested refused the arguments (e.g. TTL)
};
const char* PlayEnrollStatusName(PlayEnrollStatus status);

struct PlayEnrollOutcome {
  PlayEnrollStatus status = PlayEnrollStatus::StorageError;
  // PlayDecodeStatusName of the Decode result, or "NotRun" when the request
  // stopped before Decode. A constant: never request or payload text.
  const char* decode = "NotRun";
  // The verdict status when a decoded body reached the validator.
  PlayIntegrityStatus verdict = PlayIntegrityStatus::PolicyInvalid;
};

// One enrollment's inputs, borrowed for the call: the raw request bytes.
struct PlayEnrollRequest {
  const char* challenge = nullptr;  // attestation.challenge
  size_t challenge_length = 0;
  const char* integrity_token = nullptr;  // attestation.token
  size_t integrity_token_length = 0;
  const char* app_version = nullptr;  // appVersion; nullptr when absent
  size_t app_version_length = 0;
  int64_t now_ms = 0;  // request time, for the challenge precheck
  int64_t ttl_ms = 0;  // device token lifetime (DEVICE_TOKEN_TTL_SECONDS)
  // Wall clock (NowMs, Unix milliseconds), read after Decode returns (the
  // decode can take up to kPlayDecodeDeadlineMs); 0 or less is a clock
  // failure. nullptr (the route) is CLOCK_REALTIME; unit tests pass their own.
  fortis::base::Clock* clock = nullptr;
};

// The required-mode Android enrollment above. In order: client configured;
// challenge in minted form (43 base64url characters); appVersion and token
// present and within their bounds; challenge live at `now_ms`
// (DeviceAuthStore::PeekChallenge); Decode; on Verified only, the proof is
// issued at max(clock(), now_ms) and EnrollAttested(proof, Android,
// appVersion, Attested, that time, ttl_ms) consumes the challenge and writes
// the device, whose bearer is returned in `output`. Nothing is written on
// any other path.
PlayEnrollOutcome EnrollWithPlayIntegrity(PlayIntegrityGoogleClient* client,
                                          DeviceAuthStore& auth,
                                          const PlayEnrollRequest& request,
                                          DeviceCredentials* output);

}  // namespace fortis::auth
