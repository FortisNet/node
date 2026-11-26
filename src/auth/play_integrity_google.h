#include "ownership.h"
#include "threading.h"
#pragma once

// Google Play Integrity, the Google decode client: the server-to-Google
// trust boundary. A service-account credential read from a runtime-only
// file signs a short-lived RS256 JWT; Google's OAuth endpoint exchanges it
// for a Bearer access token (cached in memory only); the token
// authenticates one decodeIntegrityToken HTTPS call per enrollment; and
// only that call's 2xx body reaches the verdict validator
// (VerifyPlayIntegrityVerdict), through the only PlayIntegrityDecodedResponse
// factory in the server sources (PlayIntegrityDecodeSource, defined in
// play_integrity_google.cpp alone).
//
// References:
//   https://developers.google.com/identity/protocols/oauth2/service-account
//   https://developer.android.com/google/play/integrity/standard
//   https://developer.android.com/google/play/integrity/verdicts
//
// The release server links this client (build/fortis_node):
// play_integrity_enrollment.{h,cpp} owns the one
// process-wide instance, configured from FORTIS_PLAY_INTEGRITY_* at startup,
// and calls Decode from required-mode POST /v1/devices before it consumes
// the server challenge.
//
// Endpoints are compile-time constants. Every request goes through
// OutboundHttpsRequest with TLS 1.2+, the configured CA bundle, peer-name
// verification against the fixed host, the default public-address policy
// and no redirects. The only endpoint, transport or clock override is
// PlayIntegrityGoogleTestSeam, which exists only when the translation unit
// is compiled with FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT (host test binaries;
// the source refuses that flag in a guest build).
//
// Secrets: the private key, the signed assertion and the access token are
// never logged, counted by value, returned or written anywhere; buffers that
// held them are wiped with mbedtls_platform_zeroize. Status names are
// constants. Neither the integrity token nor the decoded verdict is
// persisted: the replay guard keeps only the token's SHA-256 for its TTL.

#include "clock.h"
#include "outbound_https.h"
#include "play_integrity.h"

#include <mbedtls/pk.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#if defined(FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT) && defined(FORTIS_UNIKRAFT)
#error "the Play Integrity test transport must never reach a guest build"
#endif

namespace fortis::auth {

// ---- Fixed Google endpoints -----------------------------------------------
constexpr char kGoogleOAuthHost[] = "oauth2.googleapis.com";
constexpr char kGoogleOAuthPath[] = "/token";
// The JWT aud claim; also the only token_uri a credential file may name.
constexpr char kGoogleOAuthAudience[] = "https://oauth2.googleapis.com/token";
constexpr char kPlayIntegrityScope[] = "https://www.googleapis.com/auth/playintegrity";
constexpr char kPlayIntegrityApiHost[] = "playintegrity.googleapis.com";
// POST /v1/{packageName}:decodeIntegrityToken for kPlayIntegrityPackageName.
constexpr char kPlayIntegrityDecodePath[] = "/v1/io.fortisnet.wallet:decodeIntegrityToken";

// ---- Bounds ---------------------------------------------------------------
// A Google JSON key file is about 2.4 KiB.
constexpr size_t kServiceAccountMaxFileBytes = 16384;
constexpr size_t kServiceAccountMaxTokens = 128;
constexpr size_t kServiceAccountEmailMaxBytes = 254;
constexpr size_t kServiceAccountKeyIdMaxBytes = 64;
constexpr size_t kServiceAccountPrivateKeyMaxBytes = 8192;
constexpr size_t kServiceAccountMinRsaBits = 2048;
constexpr size_t kServiceAccountMaxRsaBits = 4096;
// Google documents access tokens of up to 2,048 bytes.
constexpr size_t kGoogleAccessTokenMaxBytes = 2048;
constexpr size_t kGoogleOAuthResponseMaxBytes = 8192;
constexpr size_t kGoogleOAuthResponseMaxTokens = 64;
// header + claims + 4096-bit signature, base64url: well under this.
constexpr size_t kPlayOAuthAssertionMaxBytes = 2048;
// The standard-request integrity token the app forwards. Google does not
// publish a maximum; this bound and the character set below are ours.
constexpr size_t kPlayIntegrityTokenMinBytes = 16;
constexpr size_t kPlayIntegrityTokenMaxBytes = 16384;
// The JWT is valid for five minutes (Google allows up to an hour).
constexpr int64_t kPlayOAuthAssertionLifetimeSeconds = 300;
// expires_in accepted from Google (it answers 3599).
constexpr int64_t kPlayOAuthMinExpiresSeconds = 60;
constexpr int64_t kPlayOAuthMaxExpiresSeconds = 86400;
// A cached token is refreshed this long before expires_in runs out (or at
// half its lifetime, whichever is earlier).
constexpr int64_t kPlayOAuthRefreshMarginMs = 300000;
// After a failed refresh, callers get the cached failure for this long
// instead of each starting another OAuth exchange.
constexpr int64_t kPlayOAuthFailureBackoffMs = 5000;
constexpr uint32_t kPlayOAuthTimeoutMs = 10000;
constexpr uint32_t kPlayDecodeTimeoutMs = 10000;
// One Decode call, including any wait for another caller's refresh, a 401
// refresh and the one retry, ends by this deadline.
constexpr uint32_t kPlayDecodeDeadlineMs = 25000;
// Decode calls in flight at once; more answer Busy without network I/O.
constexpr unsigned kPlayDecodeMaxConcurrent = 4;
// Remembered integrity-token digests (40 bytes each: 160 KiB).
constexpr size_t kPlayReplayCacheEntries = 4096;

// ---- Service-account credential ------------------------------------------

enum class ServiceAccountStatus : uint8_t {
  Ok,
  NotConfigured,       // no path
  OpenFailed,          // missing, unreadable or a symlink (O_NOFOLLOW)
  NotRegularFile,
  OwnerMismatch,       // not owned by the effective user
  PermissionsTooOpen,  // any group or other permission bit
  SizeInvalid,         // empty or over kServiceAccountMaxFileBytes
  ReadFailed,          // short read or the file changed size while read
  Malformed,           // JSON syntax, bounds, wrong type or duplicate field
  TypeInvalid,         // "type" absent or not "service_account"
  EmailInvalid,        // client_email absent or not a service-account email
  KeyIdInvalid,        // private_key_id absent or not [A-Za-z0-9_-]{1,64}
  EndpointMismatch,    // token_uri or universe_domain names another endpoint
  PrivateKeyInvalid,   // not an unencrypted PKCS#8 RSA key of 2048-4096 bits
                       // that signs and verifies
};
const char* ServiceAccountStatusName(ServiceAccountStatus status);

// A parsed service-account key. Holds the RSA private key in Mbed TLS
// memory (freed and wiped on Clear or destruction); not copyable.
class PlayServiceAccount {
 public:
  PlayServiceAccount();
  ~PlayServiceAccount();
  PlayServiceAccount(const PlayServiceAccount&) = delete;
  PlayServiceAccount& operator=(const PlayServiceAccount&) = delete;

  bool loaded() const { return loaded_; }
  const char* client_email() const { return email_; }
  const char* key_id() const { return key_id_; }
  void Clear();

 private:
  friend ServiceAccountStatus LoadPlayServiceAccount(const char* path,
                                                     PlayServiceAccount* account);
  friend bool BuildPlayOAuthAssertion(const PlayServiceAccount& account, int64_t now_seconds,
                                      char* output, size_t capacity, size_t* length);
  bool loaded_ = false;
  char email_[kServiceAccountEmailMaxBytes + 1]{};
  char key_id_[kServiceAccountKeyIdMaxBytes + 1]{};
  mutable mbedtls_pk_context key_;
};

// Reads the Google JSON key at `path`: opened with O_NOFOLLOW, a regular
// file owned by the effective user with no group/other permission bits,
// 1..kServiceAccountMaxFileBytes bytes, one bounded JSON object. Reads
// type, client_email, private_key_id, private_key, token_uri and
// universe_domain; each may appear once (other members are ignored). The
// file buffer and decoded PEM are wiped before return. On any status but
// Ok `account` is cleared.
ServiceAccountStatus LoadPlayServiceAccount(const char* path, PlayServiceAccount* account);

// The signed OAuth assertion, as compact JWS text:
//   base64url({"alg":"RS256","typ":"JWT","kid":"<private_key_id>"}) "."
//   base64url({"iss":"<client_email>","scope":"<kPlayIntegrityScope>",
//              "aud":"<kGoogleOAuthAudience>","exp":<now+300>,"iat":<now>}) "."
//   base64url(RSASSA-PKCS1-v1_5 SHA-256 signature)
// base64url is RFC 4648 section 5 without padding. False (and an empty
// output) on a bad clock, small buffer or signing failure.
bool BuildPlayOAuthAssertion(const PlayServiceAccount& account, int64_t now_seconds,
                             char* output, size_t capacity, size_t* length);

// ---- OAuth token response -------------------------------------------------

enum class GoogleOAuthParse : uint8_t {
  Ok,
  ErrorResponse,  // an "error" member: never a token, whatever the status
  Malformed,      // syntax, bounds, duplicate or wrong-typed fields, a
                  // token outside the b64token set or over 2,048 bytes, a
                  // token_type other than Bearer, expires_in outside
                  // [kPlayOAuthMinExpiresSeconds, kPlayOAuthMaxExpiresSeconds]
                  // or not an integer
};
// Parses a 2xx token-endpoint body: {"access_token": "...", "token_type":
// "Bearer", "expires_in": 3599, ...}. On Ok, `token` holds the NUL-
// terminated token; otherwise it is wiped.
GoogleOAuthParse ParseGoogleOAuthResponse(const unsigned char* body, size_t length,
                                          char token[kGoogleAccessTokenMaxBytes + 1],
                                          size_t* token_length, int64_t* expires_in_seconds);

// ---- Decode client --------------------------------------------------------

enum class PlayDecodeStatus : uint8_t {
  Verified,
  NotConfigured,       // Initialize did not succeed
  InputInvalid,        // malformed challenge, appVersion or integrity token
  Replayed,            // this integrity token was already submitted
  Busy,                // decode concurrency or replay-cache capacity
  CredentialFailed,    // the JWT could not be signed
  OAuthUnavailable,    // token endpoint unreachable, timeout, TLS, 429/5xx
                       // or inside the failure backoff
  OAuthRejected,       // token endpoint non-2xx (other than 429/5xx), a
                       // redirect, or an error body
  OAuthMalformed,      // token endpoint 2xx body unusable
  Unauthorized,        // decode answered 401 again after one refresh
  PermissionDenied,    // decode 403 (service account or API not enabled)
  TokenRejected,       // decode 400 (malformed or foreign integrity token)
  RateLimited,         // decode 429
  UpstreamUnavailable, // decode 5xx
  UpstreamRejected,    // any other non-2xx, including redirects
  Timeout,             // decode deadline
  TlsFailed,           // handshake, CA or peer-name verification
  TransportFailed,     // DNS, connect, address policy or request refused
  ResponseTooLarge,    // decode body over kPlayVerdictMaxBytes
  ResponseMalformed,   // truncated or badly framed body, no JSON media type
  VerdictRejected,     // VerifyPlayIntegrityVerdict did not verify
};
const char* PlayDecodeStatusName(PlayDecodeStatus status);

struct PlayDecodeResult {
  PlayDecodeStatus status = PlayDecodeStatus::NotConfigured;
  // The verdict status when a decoded body reached the validator (Verified or
  // VerdictRejected); PolicyInvalid otherwise.
  PlayIntegrityStatus verdict = PlayIntegrityStatus::PolicyInvalid;
  // Filled only on Verified.
  PlayIntegrityEvidence evidence{};
};

// One enrollment's inputs, borrowed for the call. The platform is fixed to
// "android" and the package to the policy's (kPlayIntegrityPackageName).
struct PlayDecodeInput {
  const char* challenge = nullptr;  // the server-minted 43-character challenge
  size_t challenge_length = 0;
  const char* app_version = nullptr;  // EnrollDeviceRequest.appVersion bytes
  size_t app_version_length = 0;
  const char* integrity_token = nullptr;  // the app's standard token
  size_t integrity_token_length = 0;
};

#ifdef FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT
// Host-test seam (absent from release and guest builds). Either a transport
// that replaces OutboundHttpsRequest outright, or an endpoint override that
// sends both requests to `host`:`port` (fixed paths, loopback allowed) over
// real TLS. Clocks replace CLOCK_REALTIME (JWT iat/exp, verdict time) and
// CLOCK_MONOTONIC (token cache, backoff, replay TTL); deadlines always use
// the real monotonic clock.
struct PlayIntegrityGoogleTestSeam {
  http::OutboundResponse (*transport)(void* context, const http::OutboundRequest& request,
                                unsigned char* body, size_t capacity) = nullptr;
  void* transport_context = nullptr;
  const char* host = nullptr;
  const char* port = nullptr;
  // NowMs replaces CLOCK_REALTIME and MonotonicMs CLOCK_MONOTONIC.
  fortis::base::Clock* clock = nullptr;
};
#endif

struct PlayIntegrityGoogleConfig {
  const char* credential_path = nullptr;  // runtime-only service-account JSON
  const char* ca_file = nullptr;          // PEM trust bundle for both hosts
  // Verdict policy: release certificate digests (required), minimum versionCode
  // and time window. package_name must be kPlayIntegrityPackageName and
  // allow_testing_response must be false.
  PlayIntegrityPolicy policy{};
#ifdef FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT
  PlayIntegrityGoogleTestSeam test{};
#endif
};

class PlayIntegrityGoogleClient {
 public:
  PlayIntegrityGoogleClient();
  ~PlayIntegrityGoogleClient();
  PlayIntegrityGoogleClient(const PlayIntegrityGoogleClient&) = delete;
  PlayIntegrityGoogleClient& operator=(const PlayIntegrityGoogleClient&) = delete;

  // Validates `config` (credential, CA bundle, policy), copies the policy
  // digests and allocates the replay cache. Call once, before any Decode
  // and before sharing the client between threads. False for an absent or
  // invalid config (Decode then answers NotConfigured); `credential_status`
  // (optional) says why a credential was refused.
  bool Initialize(const PlayIntegrityGoogleConfig& config,
                  ServiceAccountStatus* credential_status);
  // True once Initialize succeeded (it is never reset). Initialize runs
  // before the client is shared, so concurrent reads are safe.
  bool configured() const { return initialized_; }

  // Validates the inputs, refuses a replayed token, obtains an access token
  // (cached or refreshed by exactly one caller at a time), POSTs
  // {"integrity_token": "..."} to the fixed decode endpoint with it,
  // refreshes once on 401, and passes a 2xx JSON body to
  // VerifyPlayIntegrityVerdict with the requestHash computed from
  // (challenge, "android", package, appVersion). Thread-safe.
  PlayDecodeResult Decode(const PlayDecodeInput& input);

  // Counters for tests and future metrics (no secret content).
  uint64_t oauth_exchanges() const;
  uint64_t decode_requests() const;

 private:
  struct ReplayEntry {
    uint8_t digest[32];
    int64_t expires_ms;
  };
  enum class TokenResult : uint8_t { Ok, Unavailable, Rejected, Malformed, Signing, Timeout };

  int64_t WallMs() const;
  int64_t MonotonicMs() const;
  http::OutboundResponse Send(http::OutboundRequest* request, unsigned char* body, size_t capacity) const;
  bool RecordToken(const uint8_t digest[32], PlayDecodeStatus* status);
  TokenResult AccessToken(int64_t deadline_ms, char* output, uint64_t* generation);
  TokenResult Refresh(int64_t deadline_ms, char* token, int64_t* expires_in_seconds);
  void InvalidateToken(uint64_t generation);
  bool AcquireSlot();
  void ReleaseSlot();

  bool initialized_ = false;
  char ca_file_[1024]{};
  uint8_t certificates_[kPlayPolicyMaxCertificates][32]{};
  PlayIntegrityPolicy policy_{};
  PlayServiceAccount account_;
#ifdef FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT
  PlayIntegrityGoogleTestSeam test_{};
  char test_host_[254]{};
  char test_port_[6]{};
#endif

  mutable fortis::base::Mutex mutex_;
  fortis::base::ConditionVariable changed_;
  // Guarded by mutex_.
  char access_token_[kGoogleAccessTokenMaxBytes + 1]{};
  bool have_token_ = false;
  bool refreshing_ = false;
  uint64_t generation_ = 0;
  int64_t refresh_at_ms_ = 0;
  int64_t failure_until_ms_ = 0;
  TokenResult last_failure_ = TokenResult::Unavailable;
  unsigned in_flight_ = 0;
  uint64_t oauth_exchanges_ = 0;
  uint64_t decode_requests_ = 0;
  std::unique_ptr<ReplayEntry[], base::StorageDeleter> replay_;  // kPlayReplayCacheEntries ring
  size_t replay_next_ = 0;
};

}  // namespace fortis::auth
