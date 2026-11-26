#include <cstdarg>
#include <atomic>
#include "play_integrity_enrollment.h"
#include "ownership.h"

#include "outbound_https.h"
#include "play_integrity_google.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
// The host-test variant of this file (FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT)
// reads a loopback Google endpoint from the environment; a guest build must
// never compile it (play_integrity_google.h refuses the flag there too).
#if defined(FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT) && defined(FORTIS_UNIKRAFT)
#error "the Play Integrity test transport must never reach a guest build"
#endif

namespace fortis::auth {

// The only issuer of Play Integrity verdicts in any build that links this
// file. Its one caller is EnrollWithPlayIntegrity below, after
// PlayIntegrityGoogleClient::Decode returned Verified for this request's own
// minted challenge and appVersion: Google's decodeIntegrityToken answer,
// fetched over verified TLS with the server's OAuth token, wrapped only by
// PlayIntegrityDecodeSource (play_integrity_google.cpp) and checked by
// VerifyPlayIntegrityVerdict against the server-computed requestHash and the
// configured certificate/versionCode/time policy. Nothing from the request
// body other than the challenge text and appVersion it was bound to reaches
// the proof; the evidence comes from the validated verdict.
class PlayIntegrityAttestationIssuer {
 public:
  static VerifiedDeviceAttestation Issue(const char challenge[44], const char* app_version,
                                         size_t app_version_length,
                                         const PlayIntegrityEvidence& evidence,
                                         int64_t verified_at_ms) {
    VerifiedDeviceAttestation proof;
    proof.provider_ = DeviceAttestation::PlayIntegrity;
    proof.platform_ = DevicePlatform::Android;
    memcpy(proof.challenge_, challenge, 44);
    memcpy(proof.app_version_, app_version, app_version_length);
    proof.app_version_[app_version_length] = '\0';
    proof.app_version_length_ = static_cast<uint16_t>(app_version_length);
    proof.has_app_version_ = true;
    proof.verified_at_ms_ = verified_at_ms;
    proof.play_version_code_ = evidence.version_code;
    proof.play_device_label_ = static_cast<uint8_t>(evidence.device_label);
    return proof;
  }
};

namespace {

constexpr char kCredentialVariable[] = "FORTIS_PLAY_INTEGRITY_CREDENTIAL_FILE";
constexpr char kCertificateVariable[] = "FORTIS_PLAY_INTEGRITY_CERT_SHA256";
constexpr char kFormatVariable[] = "FORTIS_PLAY_INTEGRITY_CERT_SHA256_FORMAT";
constexpr char kVersionVariable[] = "FORTIS_PLAY_INTEGRITY_MIN_VERSION_CODE";
constexpr char kCaVariable[] = "FORTIS_PLAY_INTEGRITY_CA_FILE";
constexpr char kMaxAgeVariable[] = "FORTIS_PLAY_INTEGRITY_MAX_AGE_SECONDS";
constexpr char kSkewVariable[] = "FORTIS_PLAY_INTEGRITY_MAX_FUTURE_SKEW_SECONDS";
// Every variable whose presence turns Android enrollment configuration on.
const char* const kPlayVariables[] = {kCredentialVariable, kCertificateVariable,
                                      kFormatVariable,     kVersionVariable,
                                      kCaVariable,         kMaxAgeVariable,
                                      kSkewVariable};
// Four "AB:CD:..." digests (95 characters each) and three commas.
constexpr size_t kCertificateListMaxBytes = kPlayPolicyMaxCertificates * 95 + 3;
constexpr char kBase64Url[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// `format` holds one "%s" for the variable name; values are never printed.
__attribute__((format(printf, 3, 4)))
void set_error(char* error, size_t capacity, const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  if (error && capacity) vsnprintf(error, capacity, format, arguments);
  va_end(arguments);
}

// An absolute path for a runtime file: '/' first, 1..kPlayConfigPathMaxBytes
// bytes, no control character (U+0000-U+001F, U+007F). Copied to `output`
// (kPlayConfigPathMaxBytes + 1 bytes) only when valid.
bool absolute_path(const char* value, char* output) {
  const size_t length = strnlen(value, kPlayConfigPathMaxBytes + 1);
  if (!length || length > kPlayConfigPathMaxBytes || value[0] != '/') return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (c < 0x20 || c == 0x7f) return false;
  }
  memcpy(output, value, length + 1);
  return true;
}

// A canonical decimal in [minimum, maximum]: "0" or [1-9][0-9]*, at most ten
// digits; no sign, whitespace, exponent or leading zero.
bool decimal(const char* value, int64_t minimum, int64_t maximum, int64_t* output) {
  const size_t length = strnlen(value, 11);
  if (!length || length > 10 || (value[0] == '0' && length > 1)) return false;
  int64_t number = 0;
  for (size_t i = 0; i < length; ++i) {
    if (value[i] < '0' || value[i] > '9') return false;
    number = number * 10 + (value[i] - '0');
  }
  if (number < minimum || number > maximum) return false;
  *output = number;
  return true;
}

bool digest_format(const char* value, PlayCertificateDigestFormat* format) {
  if (strcmp(value, "hex-colon") == 0) *format = PlayCertificateDigestFormat::HexColon;
  else if (strcmp(value, "hex") == 0) *format = PlayCertificateDigestFormat::Hex;
  else if (strcmp(value, "base64url") == 0) *format = PlayCertificateDigestFormat::Base64Url;
  else return false;
  return true;
}

// 1..kPlayPolicyMaxCertificates comma-separated digests, each parsed by
// ParsePlayCertificateDigest in `format`; no empty entry, whitespace (the
// parser refuses it) or duplicate digest.
bool digest_list(const char* value, PlayCertificateDigestFormat format,
                 uint8_t (*output)[32], size_t* count) {
  const size_t length = strnlen(value, kCertificateListMaxBytes + 1);
  if (!length || length > kCertificateListMaxBytes) return false;
  size_t parsed = 0, start = 0;
  for (size_t i = 0; i <= length; ++i) {
    if (i < length && value[i] != ',') continue;
    if (i == start || parsed == kPlayPolicyMaxCertificates ||
        !ParsePlayCertificateDigest(value + start, i - start, format, output[parsed]))
      return false;
    for (size_t j = 0; j < parsed; ++j)
      if (memcmp(output[j], output[parsed], 32) == 0) return false;
    ++parsed;
    start = i + 1;
  }
  *count = parsed;
  return parsed >= 1;
}

bool load_config(config::NativeEnvLookup lookup, void* context, const char* fallback_ca_file,
                 PlayIntegrityRuntimeConfig* output, char* error, size_t capacity) {
  bool any = false;
  for (const char* name : kPlayVariables) {
    const char* value = lookup(context, name);
    if (!value) continue;
    any = true;
    // A supplied but empty value is a configuration mistake, never "off".
    if (!*value) {
      set_error(error, capacity, "Invalid environment: %s is empty", name);
      return false;
    }
  }
  if (!any) return true;
  const char* credential = lookup(context, kCredentialVariable);
  const char* certificates = lookup(context, kCertificateVariable);
  const char* format_name = lookup(context, kFormatVariable);
  const char* version = lookup(context, kVersionVariable);
  static const char* const kRequired[] = {kCredentialVariable, kCertificateVariable,
                                          kFormatVariable, kVersionVariable};
  for (const char* required : kRequired) {
    if (!lookup(context, required)) {
      set_error(error, capacity,
                "Invalid environment: Play Integrity configuration requires %s", required);
      return false;
    }
  }
  if (!absolute_path(credential, output->credential_file)) {
    set_error(error, capacity,
              "Invalid environment: %s must be an absolute path to the service-account key "
              "file (never the key itself)", kCredentialVariable);
    return false;
  }
  PlayCertificateDigestFormat format{};
  if (!digest_format(format_name, &format)) {
    set_error(error, capacity,
              "Invalid environment: %s must be hex-colon, hex or base64url", kFormatVariable);
    return false;
  }
  if (!digest_list(certificates, format, output->certificates, &output->certificate_count)) {
    set_error(error, capacity,
              "Invalid environment: %s must be 1 to 4 distinct comma-separated SHA-256 "
              "digests in the declared format", kCertificateVariable);
    return false;
  }
  if (!decimal(version, 1, kPlayMaxVersionCode, &output->min_version_code)) {
    set_error(error, capacity,
              "Invalid environment: %s must be an integer from 1 to 2100000000",
              kVersionVariable);
    return false;
  }
  const char* ca = lookup(context, kCaVariable);
  if (ca) {
    if (!absolute_path(ca, output->ca_file)) {
      set_error(error, capacity, "Invalid environment: %s must be an absolute path",
                kCaVariable);
      return false;
    }
  } else if (!fallback_ca_file || !*fallback_ca_file ||
             !absolute_path(fallback_ca_file, output->ca_file)) {
    set_error(error, capacity,
              "Invalid environment: Play Integrity requires %s or an absolute "
              "FORTIS_CA_BUNDLE_PATH", kCaVariable);
    return false;
  }
  int64_t seconds = 0;
  if (const char* age = lookup(context, kMaxAgeVariable)) {
    if (!decimal(age, 1, kPlayMaxAgeLimitMs / 1000, &seconds)) {
      set_error(error, capacity, "Invalid environment: %s must be an integer from 1 to 3600",
                kMaxAgeVariable);
      return false;
    }
    output->max_age_ms = seconds * 1000;
  }
  if (const char* skew = lookup(context, kSkewVariable)) {
    if (!decimal(skew, 0, kPlayMaxFutureSkewLimitMs / 1000, &seconds)) {
      set_error(error, capacity, "Invalid environment: %s must be an integer from 0 to 300",
                kSkewVariable);
      return false;
    }
    output->max_future_skew_ms = seconds * 1000;
  }
  output->configured = true;
  return true;
}

// InitializePlayIntegrityClient, plus the loopback endpoint of host test
// builds (`test_host`/`test_port`, both null otherwise and always null in a
// release build, where the config has no such member).
bool initialize_client(const PlayIntegrityRuntimeConfig& config,
                       PlayIntegrityGoogleClient* client, const char* test_host,
                       const char* test_port, char* error, size_t capacity) {
  if (!client || !config.configured) {
    set_error(error, capacity, "Play Integrity configuration is incomplete");
    return false;
  }
  if (!http::OutboundHttpsInitialize() || !http::OutboundHttpsValidateCaBundle(config.ca_file)) {
    set_error(error, capacity,
              "Invalid CA bundle configured for Play Integrity (%s or FORTIS_CA_BUNDLE_PATH)",
              kCaVariable);
    return false;
  }
  PlayIntegrityGoogleConfig google;
  google.credential_path = config.credential_file;
  google.ca_file = config.ca_file;
  google.policy.certificates = config.certificates;
  google.policy.certificate_count = config.certificate_count;
  google.policy.min_version_code = config.min_version_code;
  google.policy.max_age_ms = config.max_age_ms;
  google.policy.max_future_skew_ms = config.max_future_skew_ms;
#ifdef FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT
  google.test.host = test_host;
  google.test.port = test_port;
#else
  (void)test_host;
  (void)test_port;
#endif
  ServiceAccountStatus credential = ServiceAccountStatus::NotConfigured;
  if (!client->Initialize(google, &credential)) {
    if (credential != ServiceAccountStatus::Ok && credential != ServiceAccountStatus::NotConfigured) {
      // The status name is a constant (no path, key or file content).
      char message[96];
      snprintf(message, sizeof(message), "%s was refused: %s", kCredentialVariable,
               ServiceAccountStatusName(credential));
      set_error(error, capacity, "Invalid Play Integrity credential: %s", message);
    } else {
      set_error(error, capacity,
                "Play Integrity client was refused (already initialized or invalid policy)");
    }
    return false;
  }
  return true;
}

// The process-wide client: published once by StartPlayIntegrityRuntime
// before any worker thread exists, withdrawn by StopPlayIntegrityRuntime
// after all have been joined. Release/acquire so a reader on another thread
// sees the fully initialized client.
std::atomic<PlayIntegrityGoogleClient*> g_client{nullptr};
bool g_started = false;

// Admitted Play enrollments (PlayEnrollRouteSlot). The relationship to the
// worker count and the client's cap is documented on
// kPlayEnrollRouteMaxConcurrent; changed only by CAS and fetch-sub.
static_assert(kPlayEnrollRouteMaxConcurrent >= 1 &&
                  kPlayEnrollRouteMaxConcurrent <= kPlayDecodeMaxConcurrent,
              "the route must not admit more Play enrollments than the client can decode");
std::atomic<unsigned> g_route_slots_in_use{0};

int64_t realtime_ms() {
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec <= 0) return 0;
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

// Only a server-minted challenge (43 base64url characters, no NUL) can be
// live; anything else is answered as unavailable without a KV read.
bool minted_challenge(const char* challenge, size_t length) {
  if (!challenge || length != kPlayChallengeChars) return false;
  for (size_t i = 0; i < length; ++i)
    if (!challenge[i] || !strchr(kBase64Url, challenge[i])) return false;
  return true;
}

// Every non-Verified decode outcome (the caller never passes Verified).
// Unknown values fail closed as Unavailable; none of them reaches the
// issuer.
PlayEnrollStatus decode_failure(PlayDecodeStatus status) {
  switch (status) {
    case PlayDecodeStatus::NotConfigured: return PlayEnrollStatus::NotConfigured;
    // The client's own proof is bad or spent: a new token cannot help unless
    // the app or device changes, so these are 401s.
    case PlayDecodeStatus::InputInvalid:
    case PlayDecodeStatus::Replayed:
    case PlayDecodeStatus::TokenRejected:
    case PlayDecodeStatus::VerdictRejected: return PlayEnrollStatus::InvalidProof;
    // Load the client may retry shortly (with a new token).
    case PlayDecodeStatus::Busy:
    case PlayDecodeStatus::RateLimited: return PlayEnrollStatus::Busy;
    // Server-side or upstream faults: credential, OAuth, permission, Google
    // availability, TLS, transport, deadline, unusable 2xx bodies.
    case PlayDecodeStatus::CredentialFailed:
    case PlayDecodeStatus::OAuthUnavailable:
    case PlayDecodeStatus::OAuthRejected:
    case PlayDecodeStatus::OAuthMalformed:
    case PlayDecodeStatus::Unauthorized:
    case PlayDecodeStatus::PermissionDenied:
    case PlayDecodeStatus::UpstreamUnavailable:
    case PlayDecodeStatus::UpstreamRejected:
    case PlayDecodeStatus::Timeout:
    case PlayDecodeStatus::TlsFailed:
    case PlayDecodeStatus::TransportFailed:
    case PlayDecodeStatus::ResponseTooLarge:
    case PlayDecodeStatus::ResponseMalformed:
    default: return PlayEnrollStatus::Unavailable;
  }
}

}  // namespace

bool LoadPlayIntegrityConfig(config::NativeEnvLookup lookup, void* context,
                             const char* fallback_ca_file, PlayIntegrityRuntimeConfig* output,
                             char* error, size_t error_capacity) {
  set_error(error, error_capacity, "%s", "");
  if (!lookup || !output) {
    set_error(error, error_capacity, "Play Integrity configuration loader is unavailable");
    return false;
  }
  *output = PlayIntegrityRuntimeConfig{};
  if (load_config(lookup, context, fallback_ca_file, output, error, error_capacity)) return true;
  *output = PlayIntegrityRuntimeConfig{};  // never leave a partial policy behind
  return false;
}

bool InitializePlayIntegrityClient(const PlayIntegrityRuntimeConfig& config,
                                   PlayIntegrityGoogleClient* client, char* error,
                                   size_t error_capacity) {
  return initialize_client(config, client, nullptr, nullptr, error, error_capacity);
}

bool StartPlayIntegrityRuntime(const PlayIntegrityRuntimeConfig& config, char* error,
                               size_t error_capacity) {
  set_error(error, error_capacity, "%s", "");
  if (g_started) {
    set_error(error, error_capacity, "Play Integrity runtime was already started");
    return false;
  }
  if (!config.configured) {
    g_started = true;
    return true;  // Android enrollment stays off (ATTESTATION_REQUIRED)
  }
  const char* test_host = nullptr;
  const char* test_port = nullptr;
#ifdef FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT
  test_host = getenv("FORTIS_TEST_PLAY_INTEGRITY_HOST");
  test_port = getenv("FORTIS_TEST_PLAY_INTEGRITY_PORT");
  if ((test_host == nullptr) != (test_port == nullptr)) {
    set_error(error, error_capacity,
              "FORTIS_TEST_PLAY_INTEGRITY_HOST and FORTIS_TEST_PLAY_INTEGRITY_PORT go together");
    return false;
  }
#endif
  // Its own storage block, like RuntimeKvStore: the client holds mutexes and
  // must never move.
  auto* client = fortis::base::New<PlayIntegrityGoogleClient>();
  if (!client) {
    set_error(error, error_capacity, "Failed to allocate the Play Integrity client");
    return false;
  }
  if (!initialize_client(config, client, test_host, test_port, error, error_capacity)) {
    fortis::base::Delete(client);
    return false;
  }
  g_started = true;
  (g_client).store(client, std::memory_order_release);
  return true;
}

PlayIntegrityGoogleClient* RuntimePlayIntegrityClient() {
  return (g_client).load(std::memory_order_acquire);
}

void StopPlayIntegrityRuntime() {
  g_started = false;
  PlayIntegrityGoogleClient* client = (g_client).exchange(nullptr, std::memory_order_acq_rel);
  if (!client) return;
  // The destructor wipes the cached access token and the PlayServiceAccount
  // member wipes and frees the RSA key.
  fortis::base::Delete(client);
}

// Never waits: a failed CAS reloads the count, and a full count refuses.
PlayEnrollRouteSlot::PlayEnrollRouteSlot() : admitted_(false) {
  unsigned current = (g_route_slots_in_use).load(std::memory_order_acquire);
  while (current < kPlayEnrollRouteMaxConcurrent) {
    if ((g_route_slots_in_use).compare_exchange_strong(current, current + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
      admitted_ = true;
      return;
    }
  }
}

PlayEnrollRouteSlot::~PlayEnrollRouteSlot() {
  if (admitted_) (g_route_slots_in_use).fetch_sub(1u, std::memory_order_acq_rel);
}

unsigned PlayEnrollRouteSlotsInUse() {
  return (g_route_slots_in_use).load(std::memory_order_acquire);
}

const char* PlayEnrollStatusName(PlayEnrollStatus status) {
  switch (status) {
    case PlayEnrollStatus::Enrolled: return "Enrolled";
    case PlayEnrollStatus::NotConfigured: return "NotConfigured";
    case PlayEnrollStatus::InvalidProof: return "InvalidProof";
    case PlayEnrollStatus::ChallengeUnavailable: return "ChallengeUnavailable";
    case PlayEnrollStatus::Busy: return "Busy";
    case PlayEnrollStatus::Unavailable: return "Unavailable";
    case PlayEnrollStatus::StorageError: return "StorageError";
    case PlayEnrollStatus::Rejected: return "Rejected";
  }
  return "Unknown";
}

// The contract is on the declaration. Every early return leaves the KV
// untouched; only the EnrollAttested frame writes.
PlayEnrollOutcome EnrollWithPlayIntegrity(PlayIntegrityGoogleClient* client,
                                          DeviceAuthStore& auth,
                                          const PlayEnrollRequest& request,
                                          DeviceCredentials* output) {
  PlayEnrollOutcome outcome;
  if (!output) return outcome;
  if (!client || !client->configured()) {
    outcome.status = PlayEnrollStatus::NotConfigured;
    return outcome;
  }
  if (!minted_challenge(request.challenge, request.challenge_length)) {
    outcome.status = PlayEnrollStatus::ChallengeUnavailable;
    return outcome;
  }
  // Cheap bounds before the KV read; Decode re-checks the token alphabet and
  // the appVersion's UTF-8/control rules before any network I/O.
  if (!request.app_version || !request.app_version_length ||
      request.app_version_length > kPlayAppVersionMaxBytes || !request.integrity_token ||
      request.integrity_token_length < kPlayIntegrityTokenMinBytes ||
      request.integrity_token_length > kPlayIntegrityTokenMaxBytes) {
    outcome.status = PlayEnrollStatus::InvalidProof;
    return outcome;
  }
  // The read-only liveness precheck: a dead challenge costs no Google call
  // and burns no integrity token. The consuming check is EnrollAttested's
  // conditional frame below.
  switch (auth.PeekChallenge(request.challenge, request.challenge_length, request.now_ms)) {
    case storage::KvReadStatus::Found: break;
    case storage::KvReadStatus::Missing:
      outcome.status = PlayEnrollStatus::ChallengeUnavailable;
      return outcome;
    default:
      outcome.status = PlayEnrollStatus::StorageError;
      return outcome;
  }
  PlayDecodeInput input;
  input.challenge = request.challenge;
  input.challenge_length = request.challenge_length;
  input.app_version = request.app_version;
  input.app_version_length = request.app_version_length;
  input.integrity_token = request.integrity_token;
  input.integrity_token_length = request.integrity_token_length;
  const PlayDecodeResult decoded = client->Decode(input);
  outcome.decode = PlayDecodeStatusName(decoded.status);
  outcome.verdict = decoded.verdict;
  if (decoded.status != PlayDecodeStatus::Verified) {
    outcome.status = decode_failure(decoded.status);
    return outcome;
  }
  // Decode reports Verified only with a Verified verdict, which implies
  // both evidence bounds; re-checked so an inconsistent result is refused
  // (EnrollAttested checks the evidence once more).
  if (decoded.verdict != PlayIntegrityStatus::Verified || decoded.evidence.version_code < 1 ||
      decoded.evidence.device_label == PlayDeviceLabel::None) {
    outcome.status = PlayEnrollStatus::InvalidProof;
    return outcome;
  }
  // The decode may have taken seconds: consume the challenge at the time the
  // verdict was accepted, never earlier than the precheck, so a challenge
  // that expired meanwhile is refused by the frame.
  int64_t decided = request.clock ? request.clock->NowMs() : realtime_ms();
  if (decided <= 0) {
    outcome.status = PlayEnrollStatus::StorageError;
    return outcome;
  }
  if (decided < request.now_ms) decided = request.now_ms;
  char minted[44];
  memcpy(minted, request.challenge, kPlayChallengeChars);
  minted[kPlayChallengeChars] = '\0';
  const VerifiedDeviceAttestation proof = PlayIntegrityAttestationIssuer::Issue(
      minted, request.app_version, request.app_version_length, decoded.evidence, decided);
  switch (auth.EnrollAttested(proof, DevicePlatform::Android, request.app_version,
                              request.app_version_length, DeviceQuotaTier::Attested, decided,
                              request.ttl_ms, output)) {
    case DeviceEnrollResult::Enrolled: outcome.status = PlayEnrollStatus::Enrolled; break;
    case DeviceEnrollResult::ChallengeUnavailable:
    case DeviceEnrollResult::ChallengeMismatch:
      outcome.status = PlayEnrollStatus::ChallengeUnavailable;
      break;
    case DeviceEnrollResult::Rejected: outcome.status = PlayEnrollStatus::Rejected; break;
    default: outcome.status = PlayEnrollStatus::StorageError; break;
  }
  return outcome;
}

}  // namespace fortis::auth
