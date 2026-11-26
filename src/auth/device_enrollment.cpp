#include "ownership.h"
#include "device_enrollment.h"
#include "log.h"
#include "apple_app_attest.h"
#include "device_auth.h"
#include "device_challenge.h"
#include "http_server.h"
#include "kv_runtime.h"
#include "play_integrity_enrollment.h"
#include <mbedtls/platform_util.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include "fortisnet/intelligence/v1/devices.pb.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::auth {

namespace {
bool g_android_attestation_required = true;
constexpr char kBadRequest[] = "{\"error\":\"BAD_REQUEST\"}";

class JsonReader {
 public:
  JsonReader(const char* text, size_t length) : p_(text), end_(text + length) {}
  bool ValidDocument() {
    whitespace();
    if (!skip_value()) return false;
    whitespace();
    return p_ == end_;
  }
  bool Enrollment(char platform[32], size_t* platform_bytes,
                  char app_version[201], size_t* app_bytes, size_t* app_units,
                  bool* has_app_version, char provider[32], size_t* provider_bytes,
                  char challenge[601], size_t* challenge_bytes, size_t* challenge_units,
                  char token[96001], size_t* token_bytes, size_t* token_units,
                  bool* has_attestation, bool* has_token, bool* schema_invalid) {
    whitespace();
    if (!take('{')) return false;
    bool platform_seen = false, app_seen = false, attestation_seen = false;
    whitespace();
    while (!take('}')) {
      char key[32]{};
      if (!string(key, sizeof(key)) || !take(':')) return false;
      bool* seen = nullptr;
      if (strcmp(key, "platform") == 0) seen = &platform_seen;
      else if (strcmp(key, "appVersion") == 0) seen = &app_seen;
      else if (strcmp(key, "attestation") == 0) seen = &attestation_seen;
      if (!seen) {
        if (strcmp(key, "") != 0) *schema_invalid = true;  // unrecognized key
        if (!skip_value()) return false;
        whitespace(); if (take('}')) break; if (!take(',')) return false; whitespace();
        continue;
      }
      if (*seen) return false;
      *seen = true;
      whitespace();
      if (seen == &attestation_seen) {
        if (!attestation(provider, provider_bytes, challenge, challenge_bytes,
                         challenge_units, token, token_bytes, token_units, has_token, schema_invalid)) return false;
        *has_attestation = true;
      } else {
        char* output = seen == &platform_seen ? platform : app_version;
        const size_t capacity = seen == &platform_seen ? 32 : 201;
        size_t length = 0, bytes = 0;
        if (!string(output, capacity, &length, &bytes)) {
          if (seen == &platform_seen) { if (!skip_value()) return false; }
          else if (p_ < end_ && *p_ == 'n') {
            if (!skip_value()) return false;
            *schema_invalid = true;  // appVersion: null
          } else return false;
          continue;
        }
        if (seen == &platform_seen && length == 0) return false;
        if (seen == &platform_seen) *platform_bytes = bytes;
        if (seen == &app_seen) { *has_app_version = true; *app_units = length; *app_bytes = bytes; }
      }
      whitespace();
      if (take('}')) break;
      if (!take(',')) return false;
      whitespace();
    }
    whitespace();
    if (!platform_seen) {}
    return p_ == end_;
  }

 private:
  void whitespace() { while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) ++p_; }
  bool take(char c) { whitespace(); if (p_ == end_ || *p_ != c) return false; ++p_; return true; }
  bool hex4(uint32_t* value) {
    if (end_ - p_ < 4) return false;
    uint32_t result = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = *p_++; unsigned digit;
      if (c >= '0' && c <= '9') digit = c - '0';
      else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
      else return false;
      result = (result << 4) | digit;
    }
    *value = result; return true;
  }
  bool append(char* output, size_t capacity, size_t* used, uint32_t codepoint) {
    char bytes[4]; size_t count = 0;
    if (codepoint <= 0x7f) bytes[count++] = static_cast<char>(codepoint);
    else if (codepoint <= 0x7ff) {
      bytes[count++] = static_cast<char>(0xc0 | (codepoint >> 6));
      bytes[count++] = static_cast<char>(0x80 | (codepoint & 63));
    } else if (codepoint <= 0xffff) {
      bytes[count++] = static_cast<char>(0xe0 | (codepoint >> 12));
      bytes[count++] = static_cast<char>(0x80 | ((codepoint >> 6) & 63));
      bytes[count++] = static_cast<char>(0x80 | (codepoint & 63));
    } else {
      bytes[count++] = static_cast<char>(0xf0 | (codepoint >> 18));
      bytes[count++] = static_cast<char>(0x80 | ((codepoint >> 12) & 63));
      bytes[count++] = static_cast<char>(0x80 | ((codepoint >> 6) & 63));
      bytes[count++] = static_cast<char>(0x80 | (codepoint & 63));
    }
    if (*used + count >= capacity) return false;
    memcpy(output + *used, bytes, count); *used += count; output[*used] = '\0';
    return true;
  }
  bool string(char* output, size_t capacity, size_t* utf16_length = nullptr,
              size_t* byte_length = nullptr) {
    if (!take('"')) return false;
    size_t used = 0, units = 0;
    if (output) output[0] = '\0';
    while (p_ < end_) {
      unsigned char c = static_cast<unsigned char>(*p_++);
      if (c == '"') {
        if (utf16_length) *utf16_length = units;
        if (byte_length) *byte_length = used;
        return true;
      }
      uint32_t codepoint = c;
      if (c == '\\') {
        if (p_ == end_) return false;
        const char escape = *p_++;
        switch (escape) {
          case '"': codepoint = '"'; break; case '\\': codepoint = '\\'; break;
          case '/': codepoint = '/'; break; case 'b': codepoint = '\b'; break;
          case 'f': codepoint = '\f'; break; case 'n': codepoint = '\n'; break;
          case 'r': codepoint = '\r'; break; case 't': codepoint = '\t'; break;
          case 'u':
            if (!hex4(&codepoint)) return false;
            if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
              if (end_ - p_ < 6 || p_[0] != '\\' || p_[1] != 'u') return false;
              p_ += 2; uint32_t low = 0;
              if (!hex4(&low) || low < 0xdc00 || low > 0xdfff) return false;
              codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
            } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) return false;
            break;
          default: return false;
        }
      } else if (c >= 0x80) {
        unsigned trailing = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 99;
        if (trailing == 99 || end_ - p_ < static_cast<ptrdiff_t>(trailing)) return false;
        codepoint = c & (trailing == 1 ? 0x1f : trailing == 2 ? 0x0f : 0x07);
        for (unsigned i = 0; i < trailing; ++i) {
          const unsigned char next = static_cast<unsigned char>(*p_++);
          if ((next & 0xc0) != 0x80) return false;
          codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if ((trailing == 1 && codepoint < 0x80) ||
            (trailing == 2 && codepoint < 0x800) ||
            (trailing == 3 && codepoint < 0x10000) || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
      } else if (c < 0x20) return false;
      if (output && !append(output, capacity, &used, codepoint)) return false;
      units += codepoint > 0xffff ? 2 : 1;
    }
    return false;
  }
  bool skip_value(unsigned depth = 0) {
    if (depth > 16) return false;
    whitespace();
    if (p_ == end_) return false;
    if (*p_ == '"') return string(nullptr, 0);
    if (*p_ == '{' || *p_ == '[') {
      const char close = *p_++ == '{' ? '}' : ']'; whitespace();
      if (take(close)) return true;
      for (;;) {
        if (close == '}') { if (!string(nullptr, 0) || !take(':')) return false; }
        if (!skip_value(depth + 1)) return false;
        whitespace(); if (take(close)) return true; if (!take(',')) return false;
      }
    }
    if (*p_ == 't' && end_ - p_ >= 4 && memcmp(p_, "true", 4) == 0) { p_ += 4; return true; }
    if (*p_ == 'f' && end_ - p_ >= 5 && memcmp(p_, "false", 5) == 0) { p_ += 5; return true; }
    if (*p_ == 'n' && end_ - p_ >= 4 && memcmp(p_, "null", 4) == 0) { p_ += 4; return true; }
    const char* start = p_;
    if (take('-')) {}
    if (take('0')) {
      if (p_ < end_ && *p_ >= '0' && *p_ <= '9') return false;
    } else {
      if (p_ == end_ || *p_ < '1' || *p_ > '9') return false;
      while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_;
    }
    if (take('.')) {
      if (p_ == end_ || *p_ < '0' || *p_ > '9') return false;
      while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_;
    }
    if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
      ++p_; if (p_ < end_ && (*p_ == '+' || *p_ == '-')) ++p_;
      if (p_ == end_ || *p_ < '0' || *p_ > '9') return false;
      while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_;
    }
    return p_ != start;
  }
  bool attestation(char provider[32], size_t* provider_bytes, char challenge[601],
                   size_t* challenge_bytes, size_t* challenge_units, char token[96001],
                   size_t* token_bytes, size_t* token_units, bool* has_token,
                   bool* schema_invalid) {
    if (!take('{')) return false;
    bool provider_seen = false, challenge_seen = false;
    whitespace(); if (take('}')) return true;
    for (;;) {
      char key[32]{}; if (!string(key, sizeof(key)) || !take(':')) return false;
      bool* seen = nullptr;
      if (strcmp(key, "provider") == 0) seen = &provider_seen;
      else if (strcmp(key, "challenge") == 0) seen = &challenge_seen;
      else if (strcmp(key, "token") == 0) { if (*has_token) return false; *has_token = true; }
      if (seen) { if (*seen) return false; *seen = true; }
      whitespace();
      if (strcmp(key, "provider") == 0) { if (!string(provider, 32, nullptr, provider_bytes)) return false; }
      else if (strcmp(key, "challenge") == 0) {
        if (!string(challenge, 601, challenge_units, challenge_bytes)) return false;
      } else if (strcmp(key, "token") == 0) {
        if (!string(token, 96001, token_units, token_bytes)) return false;
      }
      else return false;
      whitespace(); if (take('}')) break; if (!take(',')) return false;
    }
    if (!challenge_seen) *schema_invalid = true;
    return provider_seen || !challenge_seen;
  }
  const char* p_; const char* end_;
};

struct EnrollmentPayload {
  char platform[32]{}; char app_version[201]{}; char provider[32]{};
  char challenge[601]{}; char token[96001]{};
  bool has_app_version = false; bool has_attestation = false; bool has_token = false;
  size_t platform_bytes = 0; size_t app_version_bytes = 0; size_t app_version_units = 0;
  size_t provider_bytes = 0; size_t challenge_bytes = 0; size_t challenge_units = 0;
  size_t token_bytes = 0; size_t token_units = 0;
  bool schema_invalid = false;
};

bool utf8_units(const char* text, size_t length, size_t* units) {
  size_t count = 0;
  for (size_t i = 0; i < length;) {
    const unsigned char c = static_cast<unsigned char>(text[i++]);
    uint32_t cp = c; unsigned trailing = 0;
    if (c < 0x80) {}
    else if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; trailing = 1; }
    else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; trailing = 2; }
    else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; trailing = 3; }
    else return false;
    if (length - i < trailing) return false;
    for (unsigned j = 0; j < trailing; ++j) {
      const unsigned char next = static_cast<unsigned char>(text[i++]);
      if ((next & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (next & 63);
    }
    if ((trailing == 1 && cp < 0x80) || (trailing == 2 && cp < 0x800) ||
        (trailing == 3 && cp < 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff)) return false;
    count += cp > 0xffff ? 2 : 1;
  }
  *units = count; return true;
}

bool platform_value(const char* name, size_t length, fortis::auth::DevicePlatform* platform);

bool parse_enrollment(const http::HttpRequest& request, EnrollmentPayload* payload) {
  if (!payload || !request.has_content_length || !request.body || !request.body_length) return false;
  if (request.protobuf_content_type) {
    auto message_storage = base::AllocateStorage<fortisnet_intelligence_v1_EnrollDeviceRequest>(sizeof(fortisnet_intelligence_v1_EnrollDeviceRequest));
    auto* message = message_storage.get();
    if (!message) return false;
    *message = fortisnet_intelligence_v1_EnrollDeviceRequest_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(
        reinterpret_cast<const pb_byte_t*>(request.body), request.body_length);
    bool decoded = pb_decode(&stream, fortisnet_intelligence_v1_EnrollDeviceRequest_fields, message);
    if (decoded) {
      memcpy(payload->platform, message->platform, sizeof(message->platform));
      payload->platform_bytes = strnlen(payload->platform, sizeof(payload->platform));
      if (message->has_app_version) {
        memcpy(payload->app_version, message->app_version, sizeof(message->app_version));
        payload->has_app_version = true;
        payload->app_version_bytes = strnlen(payload->app_version, sizeof(payload->app_version));
        decoded = utf8_units(payload->app_version, payload->app_version_bytes,
                             &payload->app_version_units);
      }
      if (decoded && message->has_attestation) {
        memcpy(payload->provider, message->attestation.provider,
               sizeof(message->attestation.provider));
        payload->provider_bytes = strnlen(payload->provider, sizeof(payload->provider));
        memcpy(payload->challenge, message->attestation.challenge, sizeof(message->attestation.challenge));
        payload->has_attestation = true;
        payload->challenge_bytes = strnlen(payload->challenge, sizeof(payload->challenge));
        decoded = utf8_units(payload->challenge, payload->challenge_bytes,
                             &payload->challenge_units);
        if (decoded && message->attestation.has_token) {
          memcpy(payload->token, message->attestation.token, sizeof(message->attestation.token));
          payload->has_token = true;
          payload->token_bytes = strnlen(payload->token, sizeof(payload->token));
          decoded = utf8_units(payload->token, payload->token_bytes, &payload->token_units);
        }
      }
    }
    message_storage.reset();
    if (!decoded) return false;
  } else {
    JsonReader reader(request.body, request.body_length);
    if (!reader.Enrollment(payload->platform, &payload->platform_bytes,
                           payload->app_version, &payload->app_version_bytes,
                           &payload->app_version_units, &payload->has_app_version,
                           payload->provider, &payload->provider_bytes, payload->challenge,
                           &payload->challenge_bytes, &payload->challenge_units,
                           payload->token, &payload->token_bytes, &payload->token_units,
                           &payload->has_attestation, &payload->has_token, &payload->schema_invalid)) return false;
  }
  return true;
}

// The schema: platform enum, appVersion <= 50 units, attestation provider
// enum, challenge 16..200 units, token <= 32,000 units.
bool payload_valid(const EnrollmentPayload& payload) {
  if (payload.schema_invalid) return false;
  if (!platform_value(payload.platform, payload.platform_bytes, nullptr)) return false;
  if (payload.has_app_version && payload.app_version_units > 50) return false;
  if (!payload.has_attestation) return true;
  if (payload.provider_bytes != strlen(payload.provider) ||
      (strcmp(payload.provider, "development") != 0 &&
       strcmp(payload.provider, "apple-app-attest") != 0 &&
       strcmp(payload.provider, "play-integrity") != 0))
    return false;
  if (payload.challenge_units < 16 || payload.challenge_units > 200) return false;
  return !(payload.has_token && payload.token_units > 32000);
}

bool platform_value(const char* name, size_t length, fortis::auth::DevicePlatform* platform) {
  if (length == 3 && memcmp(name, "ios", 3) == 0) { if (platform) *platform = fortis::auth::DevicePlatform::Ios; }
  else if (length == 7 && memcmp(name, "android", 7) == 0) { if (platform) *platform = fortis::auth::DevicePlatform::Android; }
  else if (length == 5 && memcmp(name, "macos", 5) == 0) { if (platform) *platform = fortis::auth::DevicePlatform::Macos; }
  else if (length == 7 && memcmp(name, "windows", 7) == 0) { if (platform) *platform = fortis::auth::DevicePlatform::Windows; }
  else if (length == 5 && memcmp(name, "linux", 5) == 0) { if (platform) *platform = fortis::auth::DevicePlatform::Linux; }
  else return false;
  return true;
}

bool current_time(int64_t* now) {
  timespec value{};
  if (clock_gettime(CLOCK_REALTIME, &value) != 0 || value.tv_sec <= 0 ||
      value.tv_sec > (INT64_MAX - value.tv_nsec / 1000000) / 1000) return false;
  *now = static_cast<int64_t>(value.tv_sec) * 1000 + value.tv_nsec / 1000000;
  return true;
}

void send_device_error(int client, int status, const char* reason, const char* body,
                       unsigned retry_after = 0) {
  http::HttpResponse response{status, reason, "application/json; charset=utf-8",
      reinterpret_cast<const uint8_t*>(body), strlen(body)};
  response.retry_after = retry_after;
  WriteHttpResponse(client, response);
}

// Wall clock for the Play Integrity enrollment's post-decode instant; 0 on a
// clock failure (EnrollWithPlayIntegrity answers StorageError).
class EnrollmentClock final : public fortis::base::Clock {
 public:
  int64_t NowMs() override {
    int64_t now = 0;
    return current_time(&now) ? now : 0;
  }
};
EnrollmentClock enrollment_clock;

// Required-mode Android enrollment (play_integrity_enrollment.h). Reached
// only for provider "play-integrity". Ordering, each answer final:
//   no Android configuration           401 ATTESTATION_REQUIRED (nothing
//                                      read or sent)
//   platform not android, or no token  401 ATTESTATION_INVALID
//   kPlayEnrollRouteMaxConcurrent Play
//     enrollments already admitted     503 ATTESTATION_BUSY, Retry-After 1
//                                      (nothing read, sent or recorded: the
//                                      same challenge and token may retry)
//   KV or clock unavailable            500 INTERNAL
//   then EnrollWithPlayIntegrity:
//     challenge not minted/live/spent  401 ATTESTATION_EXPIRED
//     bad/replayed token, Google 400,
//       missing appVersion, any
//       verdict refusal                401 ATTESTATION_INVALID
//     decode capacity, Google 429      503 ATTESTATION_BUSY, Retry-After 1
//     OAuth/credential/permission,
//       Google 401/403/5xx, TLS,
//       transport, timeout             503 ATTESTATION_UNAVAILABLE,
//                                      Retry-After 5 (constant status name
//                                      logged to stderr for operators)
//     storage, TTL                     500 INTERNAL
// Every refusal leaves the challenge live. A token submitted to Decode is
// burned by the client, so that retry needs a new standard integrity token;
// the route-capacity refusal above happens before Decode and can retry the
// same token.
// The admission slot (PlayEnrollRouteSlot, play_integrity_enrollment.h) is
// taken after every request-shape check and held to the end of this function,
// so it is returned on every answer above; at most three Play enrollments
// wait on Google at once and at least one of the four HTTP workers stays free
// of them. Checks placed before the slot must not touch KV, the clock or
// Google, and EnrollWithPlayIntegrity must stay inside the slot's scope.
// True with `credentials` set, or false once answered.
bool enroll_play_integrity(int client, const EnrollmentPayload& payload,
                           fortis::auth::DevicePlatform platform, const DeviceEnrollmentConfig& config,
                           fortis::auth::DeviceCredentials* credentials) {
  fortis::auth::PlayIntegrityGoogleClient* play = fortis::auth::RuntimePlayIntegrityClient();
  if (!play) {
    send_device_error(client, 401, "Unauthorized", "{\"error\":\"ATTESTATION_REQUIRED\"}"); return false;
  }
  if (platform != fortis::auth::DevicePlatform::Android || !payload.has_token) {
    send_device_error(client, 401, "Unauthorized", "{\"error\":\"ATTESTATION_INVALID\"}"); return false;
  }
  const fortis::auth::PlayEnrollRouteSlot slot;
  if (!slot.admitted()) {
    send_device_error(client, 503, "Service Unavailable", "{\"error\":\"ATTESTATION_BUSY\"}", 1); return false;
  }
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
  int64_t now = 0;
  if (!kv || !current_time(&now)) {
    send_device_error(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}"); return false;
  }
  fortis::auth::DeviceAuthStore auth(kv);
  fortis::auth::PlayEnrollRequest request;
  request.challenge = payload.challenge;
  request.challenge_length = payload.challenge_bytes;
  request.integrity_token = payload.token;
  request.integrity_token_length = payload.token_bytes;
  request.app_version = payload.has_app_version ? payload.app_version : nullptr;
  request.app_version_length = payload.has_app_version ? payload.app_version_bytes : 0;
  request.now_ms = now;
  request.ttl_ms = config.device_token_ttl_ms;
  request.clock = &enrollment_clock;
  const fortis::auth::PlayEnrollOutcome outcome =
      fortis::auth::EnrollWithPlayIntegrity(play, auth, request, credentials);
  switch (outcome.status) {
    case fortis::auth::PlayEnrollStatus::Enrolled: return true;
    case fortis::auth::PlayEnrollStatus::NotConfigured:
      send_device_error(client, 401, "Unauthorized", "{\"error\":\"ATTESTATION_REQUIRED\"}"); return false;
    case fortis::auth::PlayEnrollStatus::ChallengeUnavailable:
      send_device_error(client, 401, "Unauthorized", "{\"error\":\"ATTESTATION_EXPIRED\"}"); return false;
    case fortis::auth::PlayEnrollStatus::InvalidProof:
      send_device_error(client, 401, "Unauthorized", "{\"error\":\"ATTESTATION_INVALID\"}"); return false;
    case fortis::auth::PlayEnrollStatus::Busy:
      send_device_error(client, 503, "Service Unavailable", "{\"error\":\"ATTESTATION_BUSY\"}", 1); return false;
    case fortis::auth::PlayEnrollStatus::Unavailable:
      fortis::base::LogWarn("auth", "play-integrity enrollment unavailable: %s", outcome.decode);
      send_device_error(client, 503, "Service Unavailable", "{\"error\":\"ATTESTATION_UNAVAILABLE\"}", 5); return false;
    default:
      send_device_error(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}"); return false;
  }
}

// Required mode admits only a verified platform proof: App Attest for iOS
// (below) or Play Integrity for Android (enroll_play_integrity). No
// attestation, development and anything else keep the earlier
// ATTESTATION_REQUIRED answer, before any challenge is read. True with
// `credentials` set, or false once answered.
bool enroll_required(int client, const EnrollmentPayload& payload,
                     fortis::auth::DevicePlatform platform, const DeviceEnrollmentConfig& config,
                     fortis::auth::DeviceCredentials* credentials) {
  const int64_t ttl_ms = config.device_token_ttl_ms;
  const char* app_version = payload.has_app_version ? payload.app_version : nullptr;
  const size_t app_version_bytes = payload.has_app_version ? payload.app_version_bytes : 0;
  int64_t now = 0;
  if (payload.has_attestation && strcmp(payload.provider, "play-integrity") == 0)
    return enroll_play_integrity(client, payload, platform, config, credentials);
  if (!payload.has_attestation || strcmp(payload.provider, "apple-app-attest") != 0) {
    send_device_error(client, 401, "Unauthorized", "{\"error\":\"ATTESTATION_REQUIRED\"}"); return false;
  }
  if (platform != fortis::auth::DevicePlatform::Ios || !payload.has_token) {
    fortis::base::LogWarn("auth", "apple-app-attest enrollment refused: PlatformOrTokenInvalid");
    send_device_error(client, 401, "Unauthorized", "{\"error\":\"ATTESTATION_INVALID\"}"); return false;
  }
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
  if (!kv || !current_time(&now)) {
    send_device_error(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}"); return false;
  }
  fortis::auth::DeviceAuthStore auth(kv);
  const fortis::auth::AppleEnrollOutcome outcome = fortis::auth::EnrollWithAppleAppAttest(
      auth, config.apple_attest_development ? fortis::auth::AppleAttestEnvironment::Development
                                            : fortis::auth::AppleAttestEnvironment::Production,
      payload.challenge, payload.challenge_bytes, payload.token, payload.token_bytes,
      app_version, app_version_bytes, now, ttl_ms, credentials);
  switch (outcome.status) {
    case fortis::auth::AppleEnrollStatus::Enrolled: return true;
    case fortis::auth::AppleEnrollStatus::ChallengeUnavailable:
      fortis::base::LogWarn("auth", "apple-app-attest enrollment refused: ChallengeUnavailable");
      send_device_error(client, 401, "Unauthorized", "{\"error\":\"ATTESTATION_EXPIRED\"}"); return false;
    case fortis::auth::AppleEnrollStatus::InvalidProof:
      // Only the fixed verifier stage is logged. The challenge, attestation
      // object, and token never reach the device or server diagnostics.
      fortis::base::LogWarn("auth", "apple-app-attest enrollment refused: %s",
              fortis::auth::AppleAttestStatusName(outcome.verification));
      send_device_error(client, 401, "Unauthorized", "{\"error\":\"ATTESTATION_INVALID\"}"); return false;
    case fortis::auth::AppleEnrollStatus::KeyAlreadyAssociated:
      fortis::base::LogWarn("auth", "apple-app-attest enrollment refused: KeyAlreadyAssociated");
      send_device_error(client, 401, "Unauthorized", "{\"error\":\"ATTESTATION_INVALID\"}"); return false;
    case fortis::auth::AppleEnrollStatus::Busy:
      send_device_error(client, 503, "Service Unavailable", "{\"error\":\"ATTESTATION_BUSY\"}", 1); return false;
    default:
      send_device_error(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}"); return false;
  }
}

// The loopback/private-address development path, unchanged by step B. True
// with `credentials` set, or false once answered.
bool enroll_development(int client, const http::HttpRequest& request, const EnrollmentPayload& payload,
                        fortis::auth::DevicePlatform platform, const DeviceEnrollmentConfig& config,
                        fortis::auth::DeviceCredentials* credentials) {
  const int64_t ttl_ms = config.device_token_ttl_ms;
  const char* app_version = payload.has_app_version ? payload.app_version : nullptr;
  const size_t app_version_bytes = payload.has_app_version ? payload.app_version_bytes : 0;
  int64_t now = 0;
  if (!fortis::auth::IsDevelopmentAddress(request.remote_ip)) {
    send_device_error(client, 403, "Forbidden", "{\"error\":\"DEVELOPMENT_ATTESTATION_LOCAL_ONLY\"}"); return false;
  }
  if (payload.has_attestation && strcmp(payload.provider, "development") != 0) {
    send_device_error(client, 401, "Unauthorized", "{\"error\":\"ATTESTATION_INVALID\"}"); return false;
  }
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
  if (!kv || !current_time(&now)) {
    send_device_error(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}"); return false;
  }
  fortis::auth::DeviceAuthStore auth(kv);
  if (payload.has_attestation) {
    if (memchr(payload.challenge, '\0', payload.challenge_bytes)) {
      send_device_error(client, 401, "Unauthorized", "{\"error\":\"ATTESTATION_EXPIRED\"}"); return false;
    }
    // One conditional frame consumes the live challenge and creates the
    // device, so concurrent enrollments with one challenge enroll at most
    // one device; the others read it as consumed. EnrollAttested consumes
    // the same way, and only after its platform proof has been verified.
    const fortis::auth::DeviceEnrollResult enrolled = auth.EnrollDevelopmentWithChallenge(
        payload.challenge, payload.challenge_bytes, platform, app_version,
        app_version_bytes, now, ttl_ms, credentials);
    if (enrolled == fortis::auth::DeviceEnrollResult::ChallengeUnavailable ||
        enrolled == fortis::auth::DeviceEnrollResult::ChallengeMismatch) {
      send_device_error(client, 401, "Unauthorized", "{\"error\":\"ATTESTATION_EXPIRED\"}"); return false;
    }
    if (enrolled != fortis::auth::DeviceEnrollResult::Enrolled) {
      send_device_error(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}"); return false;
    }
  } else if (!current_time(&now) || !auth.EnrollDevelopment(platform, app_version,
                 app_version_bytes, now, ttl_ms, credentials)) {
    send_device_error(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}"); return false;
  }
  return true;
}

void handle_enrollment_request(int client, const http::HttpRequest& request,
                               EnrollmentPayload* payload, const DeviceEnrollmentConfig& config) {
  // The enrollment route is not authenticated: its body framing is judged
  // before the Origin/IP hooks, its schema after them.
  if (request.json_content_type && request.body_length) {
    JsonReader reader(request.body, request.body_length);
    if (!reader.ValidDocument()) {
      send_device_error(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}"); return;
    }
  }
  // Unsupported content types (text/plain and form included) are refused
  // before the hooks.
  if (request.unsupported_content_type && request.body_length) {
    send_device_error(client, 415, "Unsupported Media Type", "{\"error\":\"UNSUPPORTED_MEDIA_TYPE\"}"); return;
  }
  if (request.origin) {
    send_device_error(client, 403, "Forbidden", "{\"error\":\"BROWSER_ORIGIN_FORBIDDEN\"}"); return;
  }
  int64_t now = 0; char quota_key[128]; uint64_t hits = 0;
  const bool quota_ok = current_time(&now) && request.remote_ip[0] &&
      snprintf(quota_key, sizeof(quota_key), "q:ip:%s:%lld", request.remote_ip,
               static_cast<long long>(now / 60000)) > 0 &&
      fortis::storage::RuntimeKvIncrementWindow(quota_key, now, 60000, &hits);
  if (quota_ok && hits > 10) {
    send_device_error(client, 429, "Too Many Requests", "{\"error\":\"RATE_LIMITED\"}", 60); return;
  }
  *payload = EnrollmentPayload{};
  if (!parse_enrollment(request, payload)) {
    send_device_error(client, 400, "Bad Request", kBadRequest); return;
  }
  if (!payload_valid(*payload)) {
    send_device_error(client, 400, "Bad Request", kBadRequest); return;
  }
  fortis::auth::DevicePlatform platform{};
  platform_value(payload->platform, payload->platform_bytes, &platform);
  fortis::auth::DeviceCredentials credentials{};
  // TestFlight sends App Attest even to a development server. Verify it
  // through the required-mode Apple path, while retaining local development
  // enrollment for debug clients.
  const bool apple_proof = payload->has_attestation &&
      strcmp(payload->provider, "apple-app-attest") == 0;
  // A platform whose attestation is not required enrolls unverified, and
  // whatever proof it sent (none, development, App Attest, Play Integrity)
  // is ignored: macOS, Windows and Linux release builds send "development".
  const bool unattested =
      (platform == fortis::auth::DevicePlatform::Android && !g_android_attestation_required) ||
      ((platform == fortis::auth::DevicePlatform::Ios ||
        platform == fortis::auth::DevicePlatform::Macos) &&
       !fortis::auth::AppleAttestationRequired()) ||
      ((platform == fortis::auth::DevicePlatform::Windows ||
        platform == fortis::auth::DevicePlatform::Linux) &&
       !fortis::auth::DesktopAttestationRequired());
  if (unattested) {
    fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
    const char* app_version = payload->has_app_version ? payload->app_version : nullptr;
    // Use the existing unverified record and quota. Any supplied Play or App
    // Attest proof is ignored, and the row must re-enroll if required mode
    // later checks its platform again.
    if (!kv || !current_time(&now) ||
        !fortis::auth::DeviceAuthStore(kv).EnrollDevelopment(platform, app_version,
            payload->app_version_bytes, now, config.device_token_ttl_ms, &credentials)) {
      send_device_error(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}");
      return;
    }
  } else if (config.attestation_required || apple_proof
                 ? !enroll_required(client, *payload, platform, config, &credentials)
                 : !enroll_development(client, request, *payload, platform, config, &credentials)) {
    return;
  }
  struct tm expiry{};
  const time_t seconds = static_cast<time_t>(credentials.expires_at_ms / 1000);
  const int millis = static_cast<int>(credentials.expires_at_ms % 1000);
  char expires_at[25];
  if (!gmtime_r(&seconds, &expiry) || snprintf(expires_at, sizeof(expires_at),
      "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", expiry.tm_year + 1900,
      expiry.tm_mon + 1, expiry.tm_mday, expiry.tm_hour, expiry.tm_min,
      expiry.tm_sec, millis) != 24) {
    send_device_error(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}"); return;
  }
  if (AcceptsProtobuf(request)) {
    fortisnet_intelligence_v1_EnrollDeviceResponse message =
        fortisnet_intelligence_v1_EnrollDeviceResponse_init_zero;
    memcpy(message.device_id, credentials.id, sizeof(credentials.id));
    memcpy(message.device_token, credentials.token, sizeof(credentials.token));
    memcpy(message.expires_at, expires_at, sizeof(expires_at));
    uint8_t wire[128]; pb_ostream_t stream = pb_ostream_from_buffer(wire, sizeof(wire));
    if (!pb_encode(&stream, fortisnet_intelligence_v1_EnrollDeviceResponse_fields, &message)) {
      send_device_error(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}"); return;
    }
    http::HttpResponse response{201, "Created", "application/protobuf", wire, stream.bytes_written};
    response.vary_accept = true; WriteHttpResponse(client, response);
  } else {
    char body[192];
    const int size = snprintf(body, sizeof(body),
        "{\"deviceId\":\"%s\",\"deviceToken\":\"%s\",\"expiresAt\":\"%s\"}",
        credentials.id, credentials.token, expires_at);
    if (size <= 0 || static_cast<size_t>(size) >= sizeof(body)) {
      send_device_error(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}"); return;
    }
    http::HttpResponse response{201, "Created", "application/json; charset=utf-8",
        reinterpret_cast<const uint8_t*>(body), static_cast<size_t>(size)};
    response.vary_accept = true; WriteHttpResponse(client, response);
  }
}

// POST /v1/devices. HttpRouter::Dispatch runs this handler without its
// global route_state_mutex (route_runs_without_state_lock in
// http_server.cpp), so a Play Integrity decode waiting on Google, or an App
// Attest verification, never stalls the other locked routes. Requests run
// concurrently on the HTTP workers, and the handler relies only on state with
// its own owner lock:
//   - the per-IP quota is one atomic RuntimeKvIncrementWindow per request,
//     so the 11th request of a minute is refused however they interleave;
//   - the payload, credentials and response bodies are per-call (the payload
//     is heap-allocated and wiped), and WriteHttpResponse fills this
//     dispatch's thread-local capture;
//   - a challenge is consumed only inside the KvStore conditional frame that
//     writes the device (DeviceAuthStore::Enroll), so concurrent requests
//     with one challenge enroll exactly one device; the read-only
//     PeekChallenge before verification is only an early refusal;
//   - verification capacity is bounded by the route's
//     kPlayEnrollRouteMaxConcurrent Play admission slots (one fewer than the
//     HTTP workers, so Google cannot hold every worker), behind them the
//     Play client's kPlayDecodeMaxConcurrent decode slots, and the App Attest
//     slot counter; each answers 503 ATTESTATION_BUSY without I/O when full,
//     and all three are atomic or own their lock and never wait.
// Do not add state here that relies on the router lock.
void handle_enrollment(int client, const http::HttpRequest& request, bool, void* context) {
  const auto* config = static_cast<const DeviceEnrollmentConfig*>(context);
  if (!config) {
    send_device_error(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}");
    return;
  }
  auto wipe_payload = [](EnrollmentPayload* payload) noexcept {
    mbedtls_platform_zeroize(payload, sizeof(*payload));
    base::StorageDeleter{}(payload);
  };
  std::unique_ptr<EnrollmentPayload, decltype(wipe_payload)> payload_storage(
      base::AllocateStorage<EnrollmentPayload>(sizeof(EnrollmentPayload)).release(), wipe_payload);
  auto* payload = payload_storage.get();
  if (!payload) {
    send_device_error(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}");
    return;
  }
  handle_enrollment_request(client, request, payload, *config);
  // The envelope can hold an App Attest or Play Integrity token. Neither is
  // retained after the response, including on a refusal path.
  payload_storage.reset();
}

// A rotation failure after the device policy charged the quota: JSON error
// with the quota headers, as TypeScript keeps the headers its auth hook set
// when the route handler then fails.
void send_rotation_error(int client, int status, const char* reason, const char* body,
                         int cost, uint32_t remaining) {
  http::HttpResponse response{status, reason, "application/json; charset=utf-8",
      reinterpret_cast<const uint8_t*>(body), strlen(body)};
  response.rate_limit_cost = cost;
  response.rate_limit_remaining = remaining;
  WriteHttpResponse(client, response);
}

// POST /v1/devices/rotate. The device policy (Origin 403, bearer 401s,
// required-mode ATTESTATION_REQUIRED, quota 429 at cost 1) runs first; then
// DeviceAuthStore::Rotate replaces the bearer in one conditional frame. Of
// concurrent requests carrying one bearer exactly one gets 200 with the new
// bearer (JSON or protobuf by Accept); a request whose bearer was consumed
// (or revoked or expired) after it authenticated gets 401 SESSION_EXPIRED,
// and one that arrives after the winner committed gets the policy's 401
// UNAUTHORIZED (the old hash key is gone). HttpRouter::Dispatch currently
// does not take its global route mutex for this handler. The conditional
// write resolves overlapping rotation and revocation. Storage failures stay
// 500 INTERNAL. Post-policy errors
// carry the quota headers. TypeScript answers
// every concurrent rotation 200 (last writer's bearer wins) and a 500 when
// the device is revoked mid-request.
void handle_rotation(int client, const http::HttpRequest& request, bool, void* context) {
  const auto* config = static_cast<const DeviceEnrollmentConfig*>(context);
  if (!config) {
    send_device_error(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}");
    return;
  }
  const DevicePolicyResult policy = AuthorizeDeviceRequest(request, 1,
                                                           config->attestation_required);
  if (policy.status) {
    http::HttpResponse failure{policy.status, policy.reason, "application/json; charset=utf-8",
        reinterpret_cast<const uint8_t*>(policy.error_body), strlen(policy.error_body)};
    failure.retry_after = policy.retry_after;
    failure.rate_limit_cost = policy.rate_limit_cost;
    failure.rate_limit_remaining = policy.rate_limit_remaining;
    WriteHttpResponse(client, failure); return;
  }
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
  fortis::auth::DeviceAuthStore auth(kv);
  const fortis::auth::DeviceRecord& device = policy.device;
  const int cost = policy.rate_limit_cost;
  const uint32_t remaining = policy.rate_limit_remaining;
  constexpr char kInternal[] = "{\"error\":\"INTERNAL\"}";
  int64_t now = 0;
  const int64_t ttl_ms = config->device_token_ttl_ms;
  fortis::auth::DeviceCredentials credentials{};
  const fortis::auth::DeviceRotateResult rotated = current_time(&now)
      ? auth.Rotate(device, now, ttl_ms, &credentials)
      : fortis::auth::DeviceRotateResult::StorageError;
  if (rotated == fortis::auth::DeviceRotateResult::SessionExpired) {
    send_rotation_error(client, 401, "Unauthorized", "{\"error\":\"SESSION_EXPIRED\"}",
                        cost, remaining);
    return;
  }
  // Rejected only follows from a configured TTL outside Rotate's bounds.
  if (rotated != fortis::auth::DeviceRotateResult::Rotated) {
    send_rotation_error(client, 500, "Internal Server Error", kInternal, cost, remaining);
    return;
  }
  // The new bearer committed; from here a 500 loses it (the client must
  // re-enroll), exactly as before this route became conditional.
  struct tm expiry{};
  const time_t seconds = static_cast<time_t>(credentials.expires_at_ms / 1000);
  char expires_at[25];
  if (!gmtime_r(&seconds, &expiry) || snprintf(expires_at, sizeof(expires_at),
      "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", expiry.tm_year + 1900,
      expiry.tm_mon + 1, expiry.tm_mday, expiry.tm_hour, expiry.tm_min,
      expiry.tm_sec, static_cast<int>(credentials.expires_at_ms % 1000)) != 24) {
    send_rotation_error(client, 500, "Internal Server Error", kInternal, cost, remaining);
    return;
  }
  http::HttpResponse response{};
  uint8_t wire[128];
  char body[128];
  if (AcceptsProtobuf(request)) {
    fortisnet_intelligence_v1_RotateDeviceTokenResponse message =
        fortisnet_intelligence_v1_RotateDeviceTokenResponse_init_zero;
    memcpy(message.device_token, credentials.token, sizeof(credentials.token));
    memcpy(message.expires_at, expires_at, sizeof(expires_at));
    pb_ostream_t stream = pb_ostream_from_buffer(wire, sizeof(wire));
    if (!pb_encode(&stream, fortisnet_intelligence_v1_RotateDeviceTokenResponse_fields, &message)) {
      send_rotation_error(client, 500, "Internal Server Error", kInternal, cost, remaining);
      return;
    }
    response = {200, "OK", "application/protobuf", wire, stream.bytes_written};
  } else {
    const int size = snprintf(body, sizeof(body),
        "{\"deviceToken\":\"%s\",\"expiresAt\":\"%s\"}", credentials.token, expires_at);
    if (size <= 0 || static_cast<size_t>(size) >= sizeof(body)) {
      send_rotation_error(client, 500, "Internal Server Error", kInternal, cost, remaining);
      return;
    }
    response = {200, "OK", "application/json; charset=utf-8",
        reinterpret_cast<const uint8_t*>(body), static_cast<size_t>(size)};
  }
  response.vary_accept = true; response.rate_limit_cost = cost;
  response.rate_limit_remaining = remaining;
  WriteHttpResponse(client, response);
}

// Revocation is authenticated with the current bearer and commits before the
// response. The bearer cannot be used for any later request once this returns.
void handle_revocation(int client, const http::HttpRequest& request, bool, void* context) {
  const auto* config = static_cast<const DeviceEnrollmentConfig*>(context);
  if (!config) {
    send_device_error(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}");
    return;
  }
  const DevicePolicyResult policy = AuthorizeDeviceRequest(request, 1,
                                                           config->attestation_required);
  if (policy.status) {
    http::HttpResponse failure{policy.status, policy.reason, "application/json; charset=utf-8",
        reinterpret_cast<const uint8_t*>(policy.error_body), strlen(policy.error_body)};
    failure.retry_after = policy.retry_after;
    failure.rate_limit_cost = policy.rate_limit_cost;
    failure.rate_limit_remaining = policy.rate_limit_remaining;
    WriteHttpResponse(client, failure);
    return;
  }
  int64_t now = 0;
  fortis::auth::DeviceAuthStore auth(fortis::storage::RuntimeKvStore());
  if (!current_time(&now) || !auth.Revoke(policy.device.id, now)) {
    send_rotation_error(client, 500, "Internal Server Error",
                        "{\"error\":\"INTERNAL\"}",
                        policy.rate_limit_cost, policy.rate_limit_remaining);
    return;
  }
  http::HttpResponse response{204, "No Content", "application/json; charset=utf-8", nullptr, 0};
  response.rate_limit_cost = policy.rate_limit_cost;
  response.rate_limit_remaining = policy.rate_limit_remaining;
  WriteHttpResponse(client, response);
}
}

namespace {
DevicePolicyResult authorize_device(const http::HttpRequest& request, uint32_t cost,
                                    bool attestation_required, bool snapshot = false) {
  DevicePolicyResult policy{};
  auto reject = [&policy](int status, const char* reason, const char* body) {
    policy.status = status; policy.reason = reason; policy.error_body = body;
  };
  if (request.origin) {
    reject(403, "Forbidden", "{\"error\":\"BROWSER_ORIGIN_FORBIDDEN\"}");
    return policy;
  }
  const char* token = request.authorization && strncmp(request.authorization, "Bearer ", 7) == 0
      ? request.authorization + 7 : "";
  if (!fortis::auth::IsDeviceToken(token)) {
    reject(401, "Unauthorized", "{\"error\":\"UNAUTHORIZED\"}");
    return policy;
  }
  int64_t now = 0;
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
  if (!kv || !current_time(&now)) {
    reject(500, "Internal Server Error", "{\"error\":\"INTERNAL\"}");
    return policy;
  }
  fortis::auth::DeviceAuthStore auth(kv);
  const auto auth_result = snapshot
      ? auth.AuthenticateSnapshot(token, now, attestation_required, &policy.device,
                                  g_android_attestation_required)
      : auth.Authenticate(token, now, attestation_required, &policy.device,
                          g_android_attestation_required);
  if (auth_result == fortis::auth::DeviceAuthResult::Unauthorized) {
    reject(401, "Unauthorized", "{\"error\":\"UNAUTHORIZED\"}"); return policy;
  }
  if (auth_result == fortis::auth::DeviceAuthResult::SessionExpired) {
    reject(401, "Unauthorized", "{\"error\":\"SESSION_EXPIRED\"}"); return policy;
  }
  if (auth_result == fortis::auth::DeviceAuthResult::AttestationRequired) {
    reject(401, "Unauthorized", "{\"error\":\"ATTESTATION_REQUIRED\"}"); return policy;
  }
  if (auth_result != fortis::auth::DeviceAuthResult::Valid || !cost || cost > INT32_MAX) {
    reject(500, "Internal Server Error", "{\"error\":\"INTERNAL\"}"); return policy;
  }
  const uint32_t limit = fortis::auth::DeviceQuotaPerMinute(policy.device.quota_tier);
  policy.rate_limit_cost = static_cast<int>(cost);
  char key[96]; uint64_t used = 0;
  const bool counted = snprintf(key, sizeof(key), "q:dev:%s:%lld", policy.device.id,
      static_cast<long long>(now / 60000)) > 0 &&
      fortis::storage::RuntimeKvIncrementByWindow(key, cost, now, 60000, &used);
  // The TS quota hook is deliberately fail-open when its KV counter fails.
  if (!counted) used = 0;
  policy.rate_limit_remaining = used >= limit ? 0 : limit - static_cast<uint32_t>(used);
  if (used > limit) {
    http::HttpNoteDevicePlatform(static_cast<uint8_t>(policy.device.platform));
    reject(429, "Too Many Requests", "{\"error\":\"RATE_LIMITED\"}");
    policy.retry_after = 60;
  } else {
    // TypeScript sets request.device only after the quota check; telemetry
    // counts the device as a client of this response from here on.
    http::HttpNoteAuthenticatedDevice(policy.device.id, static_cast<uint8_t>(policy.device.platform));
  }
  return policy;
}
}  // namespace

DevicePolicyResult AuthorizeDeviceRequest(const http::HttpRequest& request, uint32_t cost,
                                          bool attestation_required) {
  return authorize_device(request, cost, attestation_required);
}

DevicePolicyResult AuthorizeSnapshotDeviceRequest(const http::HttpRequest& request,
                                                  bool attestation_required) {
  return authorize_device(request, 1, attestation_required, true);
}

bool IsDevelopmentAddress(const char* address) {
  if (!address) return false;
  in_addr ipv4{};
  if (inet_pton(AF_INET, address, &ipv4) == 1) {
    const uint32_t ip = ntohl(ipv4.s_addr);
    const unsigned a = ip >> 24, b = (ip >> 16) & 255;
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || ip == 0x7f000001;
  }
  in6_addr ipv6{};
  if (inet_pton(AF_INET6, address, &ipv6) != 1) return false;
  bool mapped = true;
  for (unsigned i = 0; i < 10; ++i) if (ipv6.s6_addr[i] != 0) mapped = false;
  if (ipv6.s6_addr[10] != 0xff || ipv6.s6_addr[11] != 0xff) mapped = false;
  if (mapped) {
    const uint32_t ip = (static_cast<uint32_t>(ipv6.s6_addr[12]) << 24) |
        (static_cast<uint32_t>(ipv6.s6_addr[13]) << 16) |
        (static_cast<uint32_t>(ipv6.s6_addr[14]) << 8) | ipv6.s6_addr[15];
    const unsigned a = ip >> 24, b = (ip >> 16) & 255;
    return a == 10 || (a == 172 && b >= 16 && b <= 31) ||
           (a == 192 && b == 168) || ip == 0x7f000001;
  }
  static const uint8_t loopback[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
  if (memcmp(&ipv6, loopback, sizeof(loopback)) == 0) return true;
  if ((ipv6.s6_addr[0] & 0xfe) == 0xfc) return true;
  // The TypeScript development policy deliberately allows only the textual
  // fe80: prefix, not the broader IPv6 link-local fe80::/10 allocation.
  return ipv6.s6_addr[0] == 0xfe && ipv6.s6_addr[1] == 0x80;
}
}

namespace fortis::auth {

void ConfigureAndroidAttestation(bool required) {
  g_android_attestation_required = required;
}

bool RegisterDeviceEnrollmentRoute(http::HttpRouter& router, DeviceEnrollmentConfig* config) {
  return config && router.Add("POST", "/v1/devices", handle_enrollment, config);
}

bool RegisterDeviceRotationRoute(http::HttpRouter& router, DeviceEnrollmentConfig* config) {
  return config && router.Add("POST", "/v1/devices/rotate", handle_rotation, config);
}

bool RegisterDeviceRevocationRoute(http::HttpRouter& router, DeviceEnrollmentConfig* config) {
  return config && router.Add("POST", "/v1/devices/revoke", handle_revocation, config);
}

}  // namespace fortis::auth
