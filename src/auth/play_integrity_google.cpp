#include "ownership.h"
#include "exception_boundary.h"
#include "play_integrity_google.h"

#include "bounded_json.h"
#include "threading.h"

#include <errno.h>
#include <fcntl.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace fortis::auth {

// The one factory for PlayIntegrityDecodedResponse in the server sources.
// Decode below is its only caller, with the body of a 2xx response from the
// authenticated decodeIntegrityToken call.
class PlayIntegrityDecodeSource {
 public:
  static PlayIntegrityDecodedResponse Wrap(const uint8_t* body, size_t length) {
    return PlayIntegrityDecodedResponse(body, length);
  }
};

namespace {

using Document = codec::BoundedJsonDocument;

constexpr char kBase64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr char kOAuthGrantPrefix[] =
    "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion=";
constexpr char kDecodeBodyPrefix[] = "{\"integrity_token\":\"";
constexpr char kDecodeBodySuffix[] = "\"}";
constexpr char kPkcs8Begin[] = "-----BEGIN PRIVATE KEY-----";
constexpr char kServiceAccountEmailSuffix[] = ".gserviceaccount.com";
// 9999-12-31T23:59:59Z: a later wall clock is a broken one.
constexpr int64_t kMaxUnixSeconds = 253402300799;

// ---- Small helpers ----------------------------------------------------------

// Kernel randomness for RSA blinding and key checks (the same sources
// outbound_https.cpp uses for TLS).
int system_random(void*, unsigned char* output, size_t length) {
  size_t offset = 0;
  while (offset < length) {
#ifdef __APPLE__
    const size_t requested = length - offset > 256 ? 256 : length - offset;
    if (getentropy(output + offset, requested) != 0) return -1;
    offset += requested;
#else
    const size_t requested = length - offset > 256 ? 256 : length - offset;
    const ssize_t count = getrandom(output + offset, requested, 0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return -1;
    offset += static_cast<size_t>(count);
#endif
  }
  return 0;
}

int64_t clock_ms(clockid_t clock) {
  timespec value{};
  clock_gettime(clock, &value);
  return int64_t(value.tv_sec) * 1000 + value.tv_nsec / 1000000;
}

// Milliseconds left before a CLOCK_MONOTONIC deadline, 0 when passed.
uint32_t remaining_ms(int64_t deadline_ms) {
  const int64_t left = deadline_ms - clock_ms(CLOCK_MONOTONIC);
  if (left <= 0) return 0;
  return left > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(left);
}

uint32_t bounded_timeout(uint32_t limit, int64_t deadline_ms) {
  const uint32_t left = remaining_ms(deadline_ms);
  return left < limit ? left : limit;
}

// RFC 4648 section 5 without padding; NUL-terminated. 0 when it does not fit.
size_t base64url_encode(const uint8_t* input, size_t length, char* output, size_t capacity) {
  const size_t needed = length / 3 * 4 + (length % 3 ? length % 3 + 1 : 0);
  if (needed + 1 > capacity) return 0;
  size_t out = 0, i = 0;
  for (; i + 3 <= length; i += 3) {
    const uint32_t v = (uint32_t(input[i]) << 16) | (uint32_t(input[i + 1]) << 8) | input[i + 2];
    output[out++] = kBase64Url[(v >> 18) & 63];
    output[out++] = kBase64Url[(v >> 12) & 63];
    output[out++] = kBase64Url[(v >> 6) & 63];
    output[out++] = kBase64Url[v & 63];
  }
  if (length - i == 1) {
    output[out++] = kBase64Url[input[i] >> 2];
    output[out++] = kBase64Url[(input[i] & 3) << 4];
  } else if (length - i == 2) {
    output[out++] = kBase64Url[input[i] >> 2];
    output[out++] = kBase64Url[((input[i] & 3) << 4) | (input[i + 1] >> 4)];
    output[out++] = kBase64Url[(input[i + 1] & 15) << 2];
  }
  output[out] = '\0';
  return out;
}

bool ascii_alnum(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// RFC 6750 b64token, the form OutboundRequest::authorization_bearer takes.
bool is_b64token(const char* text, size_t length) {
  size_t i = 0;
  for (; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (ascii_alnum(c) || c == '-' || c == '.' || c == '_' || c == '~' || c == '+' || c == '/')
      continue;
    break;
  }
  if (i == 0) return false;
  for (; i < length; ++i)
    if (text[i] != '=') return false;
  return true;
}

// The integrity token is embedded in a JSON string without escaping, so
// only base64/base64url/JWE-compact characters are accepted.
bool is_integrity_token(const char* text, size_t length) {
  if (!text || length < kPlayIntegrityTokenMinBytes || length > kPlayIntegrityTokenMaxBytes)
    return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (!ascii_alnum(c) && c != '-' && c != '_' && c != '.' && c != '+' && c != '/' && c != '=')
      return false;
  }
  return true;
}

// local@domain, ASCII, ending in .gserviceaccount.com (both the
// iam.gserviceaccount.com and the older developer.gserviceaccount.com forms).
bool is_service_account_email(const char* text, size_t length) {
  const size_t suffix = sizeof(kServiceAccountEmailSuffix) - 1;
  if (length < suffix + 3 || length > kServiceAccountEmailMaxBytes ||
      memcmp(text + length - suffix, kServiceAccountEmailSuffix, suffix) != 0) return false;
  size_t at = length;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == '@') {
      if (at != length) return false;
      at = i;
      continue;
    }
    const bool local = at == length;
    if (ascii_alnum(c) || c == '.' || c == '-' || (local && (c == '_' || c == '+'))) continue;
    return false;
  }
  // A 1..64-byte local part and a domain with a label before the suffix.
  return at > 0 && at <= 64 && at < length && length - at - 1 > suffix;
}

bool is_key_id(const char* text, size_t length) {
  if (length == 0 || length > kServiceAccountKeyIdMaxBytes) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (!ascii_alnum(c) && c != '-' && c != '_') return false;
  }
  return true;
}

// application/json, optionally followed by parameters.
bool json_media_type(const http::OutboundResponse& response) {
  if (!response.has_content_type) return false;
  static const char kJson[] = "application/json";
  const size_t length = sizeof(kJson) - 1;
  if (strncasecmp(response.content_type, kJson, length) != 0) return false;
  const char next = response.content_type[length];
  return next == '\0' || next == ';' || next == ' ' || next == '\t';
}

// ---- Bounded JSON members -------------------------------------------------

struct Member {
  const char* name;
  uint32_t value;
};

// Records each named member's value token; false when one appears twice
// (including through escapes) or `object` is not an object.
bool collect(const Document& document, uint32_t object, Member* members, size_t count) {
  for (size_t i = 0; i < count; ++i) members[i].value = codec::kBoundedJsonNoToken;
  const codec::BoundedJsonToken* item = document.token(object);
  if (!item || item->type != codec::BoundedJsonType::kObject) return false;
  for (uint32_t key = item->first_child; key != codec::kBoundedJsonNoToken;) {
    const codec::BoundedJsonToken* key_token = document.token(key);
    if (!key_token) return false;
    const uint32_t value = key_token->next_sibling;
    const codec::BoundedJsonToken* value_token = document.token(value);
    if (!value_token) return false;
    for (size_t i = 0; i < count; ++i) {
      if (!document.StringEquals(key, members[i].name)) continue;
      if (members[i].value != codec::kBoundedJsonNoToken) return false;
      members[i].value = value;
    }
    key = value_token->next_sibling;
  }
  return true;
}

// A string member of at most capacity - 1 bytes without NUL.
bool decode_text(const Document& document, uint32_t index, char* output, size_t capacity,
                 size_t* length) {
  const codec::BoundedJsonToken* item = document.token(index);
  size_t used = 0;
  if (!item || item->type != codec::BoundedJsonType::kString ||
      !document.DecodeString(index, output, capacity, &used) || strlen(output) != used)
    return false;
  *length = used;
  return true;
}

// A JSON number written as 1..6 plain decimal digits (no sign, fraction or
// exponent), read from the raw span.
bool small_integer(const Document& document, uint32_t index, int64_t* value) {
  const codec::BoundedJsonToken* item = document.token(index);
  if (!item || item->type != codec::BoundedJsonType::kNumber || item->length == 0 || item->length > 6)
    return false;
  const unsigned char* digits = document.input() + item->start;
  if (item->length > 1 && digits[0] == '0') return false;
  int64_t parsed = 0;
  for (uint32_t i = 0; i < item->length; ++i) {
    if (digits[i] < '0' || digits[i] > '9') return false;
    parsed = parsed * 10 + (digits[i] - '0');
  }
  *value = parsed;
  return true;
}

struct SecretStorageDeleter {
  size_t bytes;
  void operator()(void* storage) const noexcept {
    mbedtls_platform_zeroize(storage, bytes);
    base::StorageDeleter{}(storage);
  }
};
template <typename T>
std::unique_ptr<T, SecretStorageDeleter> ProtectSecret(
    std::unique_ptr<T, base::StorageDeleter> storage, size_t bytes) noexcept {
  return {storage.release(), SecretStorageDeleter{bytes}};
}

// ---- Service-account parsing ------------------------------------------------

ServiceAccountStatus parse_private_key(const Document& document, uint32_t index,
                                       mbedtls_pk_context* key) {
  auto pem_storage = ProtectSecret(base::AllocateStorage<char[]>(kServiceAccountPrivateKeyMaxBytes + 1),
                                  kServiceAccountPrivateKeyMaxBytes + 1);
  auto* pem = pem_storage.get();
  if (!pem) return ServiceAccountStatus::PrivateKeyInvalid;
  size_t length = 0;
  ServiceAccountStatus status = ServiceAccountStatus::PrivateKeyInvalid;
  if (decode_text(document, index, pem, kServiceAccountPrivateKeyMaxBytes + 1, &length) &&
      length > sizeof(kPkcs8Begin) &&
      memcmp(pem, kPkcs8Begin, sizeof(kPkcs8Begin) - 1) == 0 &&
      (pem[sizeof(kPkcs8Begin) - 1] == '\n' || pem[sizeof(kPkcs8Begin) - 1] == '\r') &&
      mbedtls_pk_parse_key(key, reinterpret_cast<const unsigned char*>(pem), length + 1,
                           nullptr, 0, fortis::base::FatalCallback<system_random>, nullptr) == 0 &&
      mbedtls_pk_get_type(key) == MBEDTLS_PK_RSA &&
      mbedtls_pk_get_bitlen(key) >= kServiceAccountMinRsaBits &&
      mbedtls_pk_get_bitlen(key) <= kServiceAccountMaxRsaBits) {
    // Prove the key signs and its public half verifies before trusting it.
    static const unsigned char kProbe[32] = {'f', 'o', 'r', 't', 'i', 's'};
    unsigned char signature[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t signature_length = 0;
    if (mbedtls_pk_sign(key, MBEDTLS_MD_SHA256, kProbe, sizeof(kProbe), signature,
                        sizeof(signature), &signature_length, fortis::base::FatalCallback<system_random>, nullptr) == 0 &&
        mbedtls_pk_verify(key, MBEDTLS_MD_SHA256, kProbe, sizeof(kProbe), signature,
                          signature_length) == 0)
      status = ServiceAccountStatus::Ok;
  }
  pem_storage.reset();
  return status;
}

ServiceAccountStatus parse_service_account(const unsigned char* file, size_t length,
                                           char* email, char* key_id,
                                           mbedtls_pk_context* key) {
  codec::BoundedJsonToken tokens[kServiceAccountMaxTokens];
  Document document;
  if (!document.ParseWithLimit(file, length, tokens, kServiceAccountMaxTokens,
                               kServiceAccountMaxTokens, nullptr))
    return ServiceAccountStatus::Malformed;
  enum { kType, kEmail, kKeyId, kKey, kTokenUri, kUniverse };
  Member members[] = {{"type", 0},        {"client_email", 0}, {"private_key_id", 0},
                      {"private_key", 0}, {"token_uri", 0},    {"universe_domain", 0}};
  if (!collect(document, document.root(), members, 6)) return ServiceAccountStatus::Malformed;
  for (const Member& member : members) {
    const codec::BoundedJsonToken* value = document.token(member.value);
    if (member.value != codec::kBoundedJsonNoToken && (!value || value->type != codec::BoundedJsonType::kString))
      return ServiceAccountStatus::Malformed;
  }
  if (!document.StringEquals(members[kType].value, "service_account"))
    return ServiceAccountStatus::TypeInvalid;
  size_t used = 0;
  if (!decode_text(document, members[kEmail].value, email, kServiceAccountEmailMaxBytes + 1,
                   &used) ||
      !is_service_account_email(email, used))
    return ServiceAccountStatus::EmailInvalid;
  if (!decode_text(document, members[kKeyId].value, key_id, kServiceAccountKeyIdMaxBytes + 1,
                   &used) ||
      !is_key_id(key_id, used))
    return ServiceAccountStatus::KeyIdInvalid;
  if ((members[kTokenUri].value != codec::kBoundedJsonNoToken &&
       !document.StringEquals(members[kTokenUri].value, kGoogleOAuthAudience)) ||
      (members[kUniverse].value != codec::kBoundedJsonNoToken &&
       !document.StringEquals(members[kUniverse].value, "googleapis.com")))
    return ServiceAccountStatus::EndpointMismatch;
  return parse_private_key(document, members[kKey].value, key);
}

}  // namespace

// ---- Status names -----------------------------------------------------------

const char* ServiceAccountStatusName(ServiceAccountStatus status) {
  switch (status) {
    case ServiceAccountStatus::Ok: return "Ok";
    case ServiceAccountStatus::NotConfigured: return "NotConfigured";
    case ServiceAccountStatus::OpenFailed: return "OpenFailed";
    case ServiceAccountStatus::NotRegularFile: return "NotRegularFile";
    case ServiceAccountStatus::OwnerMismatch: return "OwnerMismatch";
    case ServiceAccountStatus::PermissionsTooOpen: return "PermissionsTooOpen";
    case ServiceAccountStatus::SizeInvalid: return "SizeInvalid";
    case ServiceAccountStatus::ReadFailed: return "ReadFailed";
    case ServiceAccountStatus::Malformed: return "Malformed";
    case ServiceAccountStatus::TypeInvalid: return "TypeInvalid";
    case ServiceAccountStatus::EmailInvalid: return "EmailInvalid";
    case ServiceAccountStatus::KeyIdInvalid: return "KeyIdInvalid";
    case ServiceAccountStatus::EndpointMismatch: return "EndpointMismatch";
    case ServiceAccountStatus::PrivateKeyInvalid: return "PrivateKeyInvalid";
  }
  return "Unknown";
}

const char* PlayDecodeStatusName(PlayDecodeStatus status) {
  switch (status) {
    case PlayDecodeStatus::Verified: return "Verified";
    case PlayDecodeStatus::NotConfigured: return "NotConfigured";
    case PlayDecodeStatus::InputInvalid: return "InputInvalid";
    case PlayDecodeStatus::Replayed: return "Replayed";
    case PlayDecodeStatus::Busy: return "Busy";
    case PlayDecodeStatus::CredentialFailed: return "CredentialFailed";
    case PlayDecodeStatus::OAuthUnavailable: return "OAuthUnavailable";
    case PlayDecodeStatus::OAuthRejected: return "OAuthRejected";
    case PlayDecodeStatus::OAuthMalformed: return "OAuthMalformed";
    case PlayDecodeStatus::Unauthorized: return "Unauthorized";
    case PlayDecodeStatus::PermissionDenied: return "PermissionDenied";
    case PlayDecodeStatus::TokenRejected: return "TokenRejected";
    case PlayDecodeStatus::RateLimited: return "RateLimited";
    case PlayDecodeStatus::UpstreamUnavailable: return "UpstreamUnavailable";
    case PlayDecodeStatus::UpstreamRejected: return "UpstreamRejected";
    case PlayDecodeStatus::Timeout: return "Timeout";
    case PlayDecodeStatus::TlsFailed: return "TlsFailed";
    case PlayDecodeStatus::TransportFailed: return "TransportFailed";
    case PlayDecodeStatus::ResponseTooLarge: return "ResponseTooLarge";
    case PlayDecodeStatus::ResponseMalformed: return "ResponseMalformed";
    case PlayDecodeStatus::VerdictRejected: return "VerdictRejected";
  }
  return "Unknown";
}

// ---- Service account --------------------------------------------------------

PlayServiceAccount::PlayServiceAccount() { mbedtls_pk_init(&key_); }

PlayServiceAccount::~PlayServiceAccount() { Clear(); }

void PlayServiceAccount::Clear() {
  mbedtls_pk_free(&key_);  // wipes the key material
  mbedtls_pk_init(&key_);
  mbedtls_platform_zeroize(email_, sizeof(email_));
  mbedtls_platform_zeroize(key_id_, sizeof(key_id_));
  loaded_ = false;
}

ServiceAccountStatus LoadPlayServiceAccount(const char* path, PlayServiceAccount* account) {
  if (!account) return ServiceAccountStatus::NotConfigured;
  account->Clear();
  if (!path || !*path || strnlen(path, 1024) >= 1024) return ServiceAccountStatus::NotConfigured;
  const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY);
  if (fd < 0) return ServiceAccountStatus::OpenFailed;
  struct stat status{};
  ServiceAccountStatus result = ServiceAccountStatus::Ok;
  if (fstat(fd, &status) != 0) result = ServiceAccountStatus::OpenFailed;
  else if (!S_ISREG(status.st_mode)) result = ServiceAccountStatus::NotRegularFile;
  else if (status.st_uid != geteuid()) result = ServiceAccountStatus::OwnerMismatch;
  else if (status.st_mode & 077) result = ServiceAccountStatus::PermissionsTooOpen;
  else if (status.st_size <= 0 || status.st_size > off_t(kServiceAccountMaxFileBytes))
    result = ServiceAccountStatus::SizeInvalid;
  if (result != ServiceAccountStatus::Ok) {
    close(fd);
    return result;
  }
  // One byte of headroom detects a file that grew after fstat.
  const size_t expected = static_cast<size_t>(status.st_size);
  auto file_storage = ProtectSecret(base::AllocateStorage<unsigned char[]>(kServiceAccountMaxFileBytes + 1),
                                   kServiceAccountMaxFileBytes + 1);
  auto* file = file_storage.get();
  if (!file) {
    close(fd);
    return ServiceAccountStatus::ReadFailed;
  }
  size_t total = 0;
  for (;;) {
    // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
    // cppcheck-suppress arithOperationsOnVoidPointer
    const ssize_t count = read(fd, file + total, kServiceAccountMaxFileBytes + 1 - total);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      if (count < 0) total = SIZE_MAX;
      break;
    }
    total += static_cast<size_t>(count);
    if (total > expected) break;
  }
  close(fd);
  if (total != expected) result = ServiceAccountStatus::ReadFailed;
  else result = parse_service_account(file, total, account->email_, account->key_id_,
                                      &account->key_);
  file_storage.reset();
  if (result == ServiceAccountStatus::Ok) account->loaded_ = true;
  else account->Clear();
  return result;
}

bool BuildPlayOAuthAssertion(const PlayServiceAccount& account, int64_t now_seconds,
                             char* output, size_t capacity, size_t* length) {
  if (output && capacity) output[0] = '\0';
  if (length) *length = 0;
  if (!account.loaded_ || !output || !length || now_seconds <= 0 ||
      now_seconds > kMaxUnixSeconds - kPlayOAuthAssertionLifetimeSeconds)
    return false;
  char header[128];
  char claims[512];
  const int header_length = snprintf(header, sizeof(header),
                                     "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"%s\"}",
                                     account.key_id_);
  const int claims_length = snprintf(
      claims, sizeof(claims),
      "{\"iss\":\"%s\",\"scope\":\"%s\",\"aud\":\"%s\",\"exp\":%lld,\"iat\":%lld}",
      account.email_, kPlayIntegrityScope, kGoogleOAuthAudience,
      static_cast<long long>(now_seconds) + kPlayOAuthAssertionLifetimeSeconds,
      static_cast<long long>(now_seconds));
  if (header_length <= 0 || size_t(header_length) >= sizeof(header) || claims_length <= 0 ||
      size_t(claims_length) >= sizeof(claims))
    return false;
  size_t used = base64url_encode(reinterpret_cast<const uint8_t*>(header),
                                 size_t(header_length), output, capacity);
  if (!used || used + 2 > capacity) goto fail;
  output[used++] = '.';
  {
    const size_t part = base64url_encode(reinterpret_cast<const uint8_t*>(claims),
                                         size_t(claims_length), output + used, capacity - used);
    if (!part || used + part + 2 > capacity) goto fail;
    used += part;
  }
  {
    uint8_t hash[32];
    unsigned char signature[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t signature_length = 0;
    const bool signed_ok =
        mbedtls_sha256(reinterpret_cast<const unsigned char*>(output), used, hash, 0) == 0 &&
        mbedtls_pk_sign(&account.key_, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature,
                        sizeof(signature), &signature_length, fortis::base::FatalCallback<system_random>, nullptr) == 0;
    mbedtls_platform_zeroize(hash, sizeof(hash));
    if (!signed_ok) {
      mbedtls_platform_zeroize(signature, sizeof(signature));
      goto fail;
    }
    output[used++] = '.';
    const size_t part = base64url_encode(signature, signature_length, output + used,
                                         capacity - used);
    mbedtls_platform_zeroize(signature, sizeof(signature));
    if (!part) goto fail;
    used += part;
  }
  *length = used;
  return true;
fail:
  mbedtls_platform_zeroize(output, capacity);
  return false;
}

GoogleOAuthParse ParseGoogleOAuthResponse(const unsigned char* body, size_t length,
                                          char token[kGoogleAccessTokenMaxBytes + 1],
                                          size_t* token_length, int64_t* expires_in_seconds) {
  if (token) mbedtls_platform_zeroize(token, kGoogleAccessTokenMaxBytes + 1);
  if (token_length) *token_length = 0;
  if (expires_in_seconds) *expires_in_seconds = 0;
  if (!body || !length || length > kGoogleOAuthResponseMaxBytes || !token || !token_length ||
      !expires_in_seconds)
    return GoogleOAuthParse::Malformed;
  codec::BoundedJsonToken tokens[kGoogleOAuthResponseMaxTokens];
  Document document;
  if (!document.ParseWithLimit(body, length, tokens, kGoogleOAuthResponseMaxTokens,
                               kGoogleOAuthResponseMaxTokens, nullptr))
    return GoogleOAuthParse::Malformed;
  enum { kToken, kType, kExpires, kError };
  Member members[] = {{"access_token", 0}, {"token_type", 0}, {"expires_in", 0}, {"error", 0}};
  if (!collect(document, document.root(), members, 4)) return GoogleOAuthParse::Malformed;
  if (members[kError].value != codec::kBoundedJsonNoToken) return GoogleOAuthParse::ErrorResponse;
  size_t used = 0;
  int64_t expires = 0;
  if (!decode_text(document, members[kToken].value, token, kGoogleAccessTokenMaxBytes + 1,
                   &used) ||
      !is_b64token(token, used) ||
      !document.StringEquals(members[kType].value, "Bearer", true) ||
      !small_integer(document, members[kExpires].value, &expires) ||
      expires < kPlayOAuthMinExpiresSeconds || expires > kPlayOAuthMaxExpiresSeconds) {
    mbedtls_platform_zeroize(token, kGoogleAccessTokenMaxBytes + 1);
    return GoogleOAuthParse::Malformed;
  }
  *token_length = used;
  *expires_in_seconds = expires;
  return GoogleOAuthParse::Ok;
}

// ---- Client -----------------------------------------------------------------

namespace {

#ifdef FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT
bool allow_test_loopback(void*, uint32_t ip) { return (ip >> 24) == 127; }
#endif

// Per-call heap scratch (about 35 KiB): nothing secret or decoded stays on
// a request thread's stack, and every byte is wiped before it is freed.
struct DecodeScratch {
  char bearer[kGoogleAccessTokenMaxBytes + 1];
  unsigned char request[sizeof(kDecodeBodyPrefix) + kPlayIntegrityTokenMaxBytes +
                        sizeof(kDecodeBodySuffix)];
  unsigned char response[kPlayVerdictMaxBytes];
};
struct RefreshScratch {
  char assertion[kPlayOAuthAssertionMaxBytes + 1];
  unsigned char form[sizeof(kOAuthGrantPrefix) + kPlayOAuthAssertionMaxBytes];
  unsigned char response[kGoogleOAuthResponseMaxBytes];
};

PlayDecodeStatus transport_status(http::OutboundError error) {
  switch (error) {
    case http::OutboundError::kTimeout: return PlayDecodeStatus::Timeout;
    case http::OutboundError::kTls: return PlayDecodeStatus::TlsFailed;
    case http::OutboundError::kTooLarge: return PlayDecodeStatus::ResponseTooLarge;
    case http::OutboundError::kHttp:
    case http::OutboundError::kUnsupportedFraming: return PlayDecodeStatus::ResponseMalformed;
    case http::OutboundError::kBusy: return PlayDecodeStatus::Busy;
    default: return PlayDecodeStatus::TransportFailed;
  }
}

PlayDecodeStatus http_status(int status) {
  if (status == 400) return PlayDecodeStatus::TokenRejected;
  if (status == 401) return PlayDecodeStatus::Unauthorized;
  if (status == 403) return PlayDecodeStatus::PermissionDenied;
  if (status == 429) return PlayDecodeStatus::RateLimited;
  if (status >= 500 && status <= 599) return PlayDecodeStatus::UpstreamUnavailable;
  return PlayDecodeStatus::UpstreamRejected;
}

}  // namespace

PlayIntegrityGoogleClient::PlayIntegrityGoogleClient() = default;

PlayIntegrityGoogleClient::~PlayIntegrityGoogleClient() {
  mbedtls_platform_zeroize(access_token_, sizeof(access_token_));
  replay_.reset();


}

bool PlayIntegrityGoogleClient::Initialize(const PlayIntegrityGoogleConfig& config,
                                           ServiceAccountStatus* credential_status) {
  if (credential_status) *credential_status = ServiceAccountStatus::NotConfigured;
  if (initialized_) return false;
  const PlayIntegrityPolicy& policy = config.policy;
  const size_t package_length = sizeof(kPlayIntegrityPackageName) - 1;
  if (!config.ca_file || !*config.ca_file || strnlen(config.ca_file, sizeof(ca_file_)) >=
                                                 sizeof(ca_file_) ||
      !policy.package_name || policy.package_name_length != package_length ||
      memcmp(policy.package_name, kPlayIntegrityPackageName, package_length) != 0 ||
      !policy.certificates || policy.certificate_count < 1 ||
      policy.certificate_count > kPlayPolicyMaxCertificates || policy.min_version_code < 1 ||
      policy.max_age_ms < 1 || policy.max_age_ms > kPlayMaxAgeLimitMs ||
      policy.max_future_skew_ms < 0 || policy.max_future_skew_ms > kPlayMaxFutureSkewLimitMs ||
      policy.allow_testing_response)
    return false;
#ifdef FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT
  const PlayIntegrityGoogleTestSeam& seam = config.test;
  if (seam.host && (!*seam.host || strnlen(seam.host, sizeof(test_host_)) >= sizeof(test_host_) ||
                    !seam.port || strnlen(seam.port, sizeof(test_port_)) >= sizeof(test_port_)))
    return false;
#endif
  if (!http::OutboundHttpsInitialize() || !http::OutboundHttpsValidateCaBundle(config.ca_file)) return false;
  const ServiceAccountStatus loaded = LoadPlayServiceAccount(config.credential_path, &account_);
  if (credential_status) *credential_status = loaded;
  if (loaded != ServiceAccountStatus::Ok) return false;
  replay_ = base::AllocateZeroedStorage<ReplayEntry[]>(kPlayReplayCacheEntries, sizeof(ReplayEntry));
  if (!replay_) {
    account_.Clear();
    return false;
  }
  memcpy(ca_file_, config.ca_file, strlen(config.ca_file) + 1);
  memcpy(certificates_, policy.certificates, policy.certificate_count * 32);
  policy_ = policy;
  policy_.package_name = kPlayIntegrityPackageName;
  policy_.certificates = certificates_;
#ifdef FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT
  test_ = seam;
  if (seam.host) {
    memcpy(test_host_, seam.host, strlen(seam.host) + 1);
    memcpy(test_port_, seam.port, strlen(seam.port) + 1);
  }
#endif
  initialized_ = true;
  return true;
}

int64_t PlayIntegrityGoogleClient::WallMs() const {
#ifdef FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT
  if (test_.clock) return test_.clock->NowMs();
#endif
  return clock_ms(CLOCK_REALTIME);
}

int64_t PlayIntegrityGoogleClient::MonotonicMs() const {
#ifdef FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT
  if (test_.clock) return static_cast<int64_t>(test_.clock->MonotonicMs());
#endif
  return clock_ms(CLOCK_MONOTONIC);
}

// Every Google request leaves through here. In a release build this is
// exactly OutboundHttpsRequest with the fixed host and the default address
// policy.
http::OutboundResponse PlayIntegrityGoogleClient::Send(http::OutboundRequest* request, unsigned char* body,
                                                 size_t capacity) const {
#ifdef FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT
  if (test_.transport) return test_.transport(test_.transport_context, *request, body, capacity);
  if (test_host_[0]) {
    request->host = test_host_;
    request->port = test_port_;
    request->allow_address = allow_test_loopback;
  }
#endif
  return OutboundHttpsRequest(*request, body, capacity);
}

bool PlayIntegrityGoogleClient::AcquireSlot() {
  fortis::base::LockGuard lock(mutex_);
  const bool admitted = in_flight_ < kPlayDecodeMaxConcurrent;
  if (admitted) ++in_flight_;
  return admitted;
}

void PlayIntegrityGoogleClient::ReleaseSlot() {
  fortis::base::LockGuard lock(mutex_);
  --in_flight_;
}

// Remembers SHA-256(integrity token) for the verdict's acceptance window
// (max_age_ms + max_future_skew_ms); the token itself is never stored. A
// token is burned on first submission, whatever Google answers: a retry
// needs a new token. The ring's oldest entry is reused only once expired;
// a ring full of live entries answers Busy rather than forget one.
bool PlayIntegrityGoogleClient::RecordToken(const uint8_t digest[32], PlayDecodeStatus* status) {
  fortis::base::LockGuard lock(mutex_);
  const int64_t now = MonotonicMs();
  bool ok = true;
  for (size_t i = 0; i < kPlayReplayCacheEntries; ++i) {
    if (replay_[i].expires_ms > now && memcmp(replay_[i].digest, digest, 32) == 0) {
      *status = PlayDecodeStatus::Replayed;
      ok = false;
      break;
    }
  }
  if (ok) {
    ReplayEntry& slot = replay_[replay_next_];
    if (slot.expires_ms > now) {
      *status = PlayDecodeStatus::Busy;
      ok = false;
    } else {
      memcpy(slot.digest, digest, 32);
      slot.expires_ms = now + policy_.max_age_ms + policy_.max_future_skew_ms;
      replay_next_ = (replay_next_ + 1) % kPlayReplayCacheEntries;
    }
  }
  return ok;
}

// One OAuth exchange: sign the assertion, POST the form grant, parse the
// token. Runs outside mutex_, by one caller at a time (refreshing_).
PlayIntegrityGoogleClient::TokenResult PlayIntegrityGoogleClient::Refresh(
    int64_t deadline_ms, char* token, int64_t* expires_in_seconds) {
  auto scratch_storage = ProtectSecret(base::AllocateZeroedStorage<RefreshScratch>(1, sizeof(RefreshScratch)),
                                      sizeof(RefreshScratch));
  auto* scratch = scratch_storage.get();
  if (!scratch) return TokenResult::Unavailable;
  TokenResult result = TokenResult::Unavailable;
  size_t assertion_length = 0;
  const uint32_t timeout = bounded_timeout(kPlayOAuthTimeoutMs, deadline_ms);
  if (!BuildPlayOAuthAssertion(account_, WallMs() / 1000, scratch->assertion,
                               sizeof(scratch->assertion), &assertion_length)) {
    result = TokenResult::Signing;
  } else if (!timeout) {
    result = TokenResult::Unavailable;
  } else {
    const size_t prefix = sizeof(kOAuthGrantPrefix) - 1;
    memcpy(scratch->form, kOAuthGrantPrefix, prefix);
    memcpy(scratch->form + prefix, scratch->assertion, assertion_length);
    http::OutboundRequest request{};
    request.method = http::OutboundMethod::kPostBinary;
    snprintf(request.content_type, sizeof(request.content_type), "%s",
             "application/x-www-form-urlencoded");
    request.host = kGoogleOAuthHost;
    request.path = kGoogleOAuthPath;
    request.ca_file = ca_file_;
    request.body = scratch->form;
    request.body_length = prefix + assertion_length;
    request.timeout_ms = timeout;
    request.max_response_bytes = sizeof(scratch->response);
    const http::OutboundResponse response = Send(&request, scratch->response, sizeof(scratch->response));
    mbedtls_platform_zeroize(scratch->form, sizeof(scratch->form));
    if (response.error != http::OutboundError::kNone) {
      result = TokenResult::Unavailable;
    } else if (response.status >= 200 && response.status <= 299) {
      size_t token_length = 0;
      const GoogleOAuthParse parsed =
          json_media_type(response)
              ? ParseGoogleOAuthResponse(scratch->response, response.body_length, token,
                                         &token_length, expires_in_seconds)
              : GoogleOAuthParse::Malformed;
      result = parsed == GoogleOAuthParse::Ok              ? TokenResult::Ok
               : parsed == GoogleOAuthParse::ErrorResponse ? TokenResult::Rejected
                                                           : TokenResult::Malformed;
    } else if (response.status == 429 || (response.status >= 500 && response.status <= 599)) {
      result = TokenResult::Unavailable;
    } else {
      result = TokenResult::Rejected;
    }
  }
  scratch_storage.reset();
  return result;
}

// A cached token, or a fresh one. While one caller refreshes, others wait
// (bounded by the deadline) and then reuse its token or its failure; after
// a failure, callers get that failure until kPlayOAuthFailureBackoffMs
// passes. `generation` names the token handed out, for InvalidateToken.
PlayIntegrityGoogleClient::TokenResult PlayIntegrityGoogleClient::AccessToken(
    int64_t deadline_ms, char* output, uint64_t* generation) {
  fortis::base::UniqueLock lock(mutex_);
  for (;;) {
    const int64_t now = MonotonicMs();
    if (have_token_ && now < refresh_at_ms_) {
      memcpy(output, access_token_, sizeof(access_token_));
      *generation = generation_;
      return TokenResult::Ok;
    }
    if (!refreshing_) {
      if (now < failure_until_ms_) {
        const TokenResult failure = last_failure_;
        return failure;
      }
      break;
    }
    const uint32_t left = remaining_ms(deadline_ms);
    if (!left) return TokenResult::Timeout;
    // CLOCK_REALTIME absolute time in slices of at most 50 ms, so a wall
    // clock step cannot stretch the wait past the monotonic deadline.
    timespec until{};
    clock_gettime(CLOCK_REALTIME, &until);
    const uint32_t slice = left < 50 ? left : 50;
    until.tv_nsec += long(slice) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
      until.tv_sec += 1;
      until.tv_nsec -= 1000000000L;
    }
    changed_.WaitUntilLocked(mutex_, until);
  }
  refreshing_ = true;
  ++oauth_exchanges_;
  const int64_t started = MonotonicMs();
  lock.Unlock();

  char fresh[kGoogleAccessTokenMaxBytes + 1];
  int64_t expires_in = 0;
  const TokenResult result = Refresh(deadline_ms, fresh, &expires_in);

  lock.Lock();
  refreshing_ = false;
  if (result == TokenResult::Ok) {
    memcpy(access_token_, fresh, sizeof(access_token_));
    have_token_ = true;
    ++generation_;
    // Counted from before the request: the token cannot be older than that.
    const int64_t lifetime = expires_in * 1000;
    const int64_t margin = lifetime / 2 < kPlayOAuthRefreshMarginMs ? lifetime / 2
                                                                    : kPlayOAuthRefreshMarginMs;
    refresh_at_ms_ = started + lifetime - margin;
    failure_until_ms_ = 0;
    memcpy(output, access_token_, sizeof(access_token_));
    *generation = generation_;
  } else {
    have_token_ = false;
    mbedtls_platform_zeroize(access_token_, sizeof(access_token_));
    last_failure_ = result == TokenResult::Timeout ? TokenResult::Unavailable : result;
    failure_until_ms_ = MonotonicMs() + kPlayOAuthFailureBackoffMs;
  }
  changed_.Broadcast();
  lock.Unlock();
  mbedtls_platform_zeroize(fresh, sizeof(fresh));
  return result;
}

// Drops the cached token after Google refused it (401), unless a newer one
// already replaced it.
void PlayIntegrityGoogleClient::InvalidateToken(uint64_t generation) {
  fortis::base::LockGuard lock(mutex_);
  if (have_token_ && generation_ == generation) {
    have_token_ = false;
    refresh_at_ms_ = 0;
    mbedtls_platform_zeroize(access_token_, sizeof(access_token_));
  }
}

uint64_t PlayIntegrityGoogleClient::oauth_exchanges() const {
  fortis::base::LockGuard lock(mutex_);
  const uint64_t value = oauth_exchanges_;
  return value;
}

uint64_t PlayIntegrityGoogleClient::decode_requests() const {
  fortis::base::LockGuard lock(mutex_);
  const uint64_t value = decode_requests_;
  return value;
}

PlayDecodeResult PlayIntegrityGoogleClient::Decode(const PlayDecodeInput& input) {
  PlayDecodeResult result{};
  if (!initialized_) return result;  // NotConfigured

  char expected_hash[kPlayRequestHashChars + 1];
  if (!is_integrity_token(input.integrity_token, input.integrity_token_length) ||
      ComputePlayIntegrityRequestHash(input.challenge, input.challenge_length,
                                      kPlayRequestPlatform, sizeof(kPlayRequestPlatform) - 1,
                                      policy_.package_name, policy_.package_name_length,
                                      input.app_version, input.app_version_length,
                                      expected_hash) != PlayRequestHashStatus::Ok) {
    result.status = PlayDecodeStatus::InputInvalid;
    return result;
  }
  if (!AcquireSlot()) {
    result.status = PlayDecodeStatus::Busy;
    return result;
  }
  auto release_slot = [](PlayIntegrityGoogleClient* client) noexcept { client->ReleaseSlot(); };
  std::unique_ptr<PlayIntegrityGoogleClient, decltype(release_slot)> slot(this, release_slot);
  const int64_t deadline = clock_ms(CLOCK_MONOTONIC) + kPlayDecodeDeadlineMs;
  uint8_t digest[32];
  std::unique_ptr<DecodeScratch, SecretStorageDeleter> scratch_storage(nullptr, SecretStorageDeleter{sizeof(DecodeScratch)});
  DecodeScratch* scratch = nullptr;
  if (mbedtls_sha256(reinterpret_cast<const unsigned char*>(input.integrity_token),
                     input.integrity_token_length, digest, 0) != 0) {
    result.status = PlayDecodeStatus::InputInvalid;
  } else if (RecordToken(digest, &result.status)) {
    scratch_storage = ProtectSecret(base::AllocateZeroedStorage<DecodeScratch>(1, sizeof(DecodeScratch)), sizeof(DecodeScratch));
    scratch = scratch_storage.get();
    if (!scratch) result.status = PlayDecodeStatus::Busy;
  }
  if (!scratch) {
    slot.reset();
    return result;
  }

  // {"integrity_token":"<token>"}: the token's characters need no escaping.
  size_t body_length = 0;
  memcpy(scratch->request, kDecodeBodyPrefix, sizeof(kDecodeBodyPrefix) - 1);
  body_length += sizeof(kDecodeBodyPrefix) - 1;
  memcpy(scratch->request + body_length, input.integrity_token, input.integrity_token_length);
  body_length += input.integrity_token_length;
  memcpy(scratch->request + body_length, kDecodeBodySuffix, sizeof(kDecodeBodySuffix) - 1);
  body_length += sizeof(kDecodeBodySuffix) - 1;

  for (int attempt = 0; attempt < 2; ++attempt) {
    uint64_t generation = 0;
    const TokenResult token = AccessToken(deadline, scratch->bearer, &generation);
    if (token != TokenResult::Ok) {
      result.status = token == TokenResult::Rejected   ? PlayDecodeStatus::OAuthRejected
                      : token == TokenResult::Malformed ? PlayDecodeStatus::OAuthMalformed
                      : token == TokenResult::Signing   ? PlayDecodeStatus::CredentialFailed
                      : token == TokenResult::Timeout   ? PlayDecodeStatus::Timeout
                                                        : PlayDecodeStatus::OAuthUnavailable;
      break;
    }
    const uint32_t timeout = bounded_timeout(kPlayDecodeTimeoutMs, deadline);
    if (!timeout) {
      result.status = PlayDecodeStatus::Timeout;
      break;
    }
    http::OutboundRequest request{};
    request.method = http::OutboundMethod::kPostJson;
    request.host = kPlayIntegrityApiHost;
    request.path = kPlayIntegrityDecodePath;
    request.ca_file = ca_file_;
    request.body = scratch->request;
    request.body_length = body_length;
    request.timeout_ms = timeout;
    request.max_response_bytes = sizeof(scratch->response);
    request.authorization_bearer = scratch->bearer;
    const http::OutboundResponse response = Send(&request, scratch->response, sizeof(scratch->response));
    mbedtls_platform_zeroize(scratch->bearer, sizeof(scratch->bearer));
    fortis::base::UniqueLock lock(mutex_);
    ++decode_requests_;
    lock.Unlock();

    if (response.error != http::OutboundError::kNone) {
      result.status = transport_status(response.error);
      break;
    }
    if (response.status == 401 && attempt == 0) {
      InvalidateToken(generation);
      continue;
    }
    if (response.status < 200 || response.status > 299) {
      result.status = http_status(response.status);
      break;
    }
    if (!json_media_type(response)) {
      result.status = PlayDecodeStatus::ResponseMalformed;
      break;
    }
    // The only place a decoded response is made: a 2xx JSON body from the
    // fixed decode endpoint, authenticated by our OAuth token over verified
    // TLS. Error bodies never get here.
    const PlayIntegrityDecodedResponse decoded =
        PlayIntegrityDecodeSource::Wrap(scratch->response, response.body_length);
    result.verdict = VerifyPlayIntegrityVerdict(policy_, decoded, expected_hash,
                                                kPlayRequestHashChars, WallMs(), &result.evidence);
    result.status = result.verdict == PlayIntegrityStatus::Verified
                        ? PlayDecodeStatus::Verified
                        : PlayDecodeStatus::VerdictRejected;
    break;
  }
  scratch_storage.reset();
  slot.reset();
  if (result.status != PlayDecodeStatus::Verified) result.evidence = PlayIntegrityEvidence{};
  return result;
}

}  // namespace fortis::auth
