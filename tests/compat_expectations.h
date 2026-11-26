// Expected-answer rewrites for the behaviours removed on purpose
// (docs/compat-decisions.md, "Replaced behaviours" and "Expectation
// mapping in the replay tests").
//
// The fixtures record the earlier TypeScript implementation's answers. The
// oracle replays keep them and map a recorded answer to this server's answer
// with the helpers below, so each removed behaviour is named in one place.
// Host-only test code.
#pragma once

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <map>
#include <string>

namespace compat_expectations {

// zod-issues: a schema refusal is this body, without a `detail` issue list.
constexpr char kBadRequest[] = "{\"error\":\"BAD_REQUEST\"}";
constexpr char kZodPrefix[] = "{\"error\":\"BAD_REQUEST\",\"detail\":[";

inline bool IsZodRefusal(const char* body, size_t length) {
  const size_t prefix = sizeof(kZodPrefix) - 1;
  return body && length > prefix && memcmp(body, kZodPrefix, prefix) == 0;
}

// form-bodies: text/plain and application/x-www-form-urlencoded bodies are
// unsupported media types, refused by the body parser (since fastify-errors:
// after the device policy, charged, in this server's envelope).
constexpr char kUnsupportedMediaType[] = "{\"error\":\"UNSUPPORTED_MEDIA_TYPE\"}";

inline bool IsRemovedBodyMedia(const char* content_type) {
  if (!content_type) return false;
  size_t length = strcspn(content_type, "; \t");
  return (length == 10 && strncasecmp(content_type, "text/plain", 10) == 0) ||
         (length == 33 && strncasecmp(content_type, "application/x-www-form-urlencoded", 33) == 0);
}

// js-prototype-keys: a JSON body with an own `__proto__` key or a
// `constructor` object holding `prototype` is no longer refused by the body
// parser (400 FST_ERR_CTP_INVALID_JSON_BODY before the hooks); it reaches the
// route's schema, after the quota.
inline bool IsPrototypePoisoningCase(const char* request_body, size_t request_length, const char* oracle_body) {
  if (!oracle_body || !strstr(oracle_body, "FST_ERR_CTP_INVALID_JSON_BODY")) return false;
  const std::string body(request_body ? request_body : "", request_body ? request_length : 0);
  return body.find("__proto__") != std::string::npos || body.find("\"prototype\"") != std::string::npos;
}

// The quota a replay sees moves with the two changes above: a removed-media
// request the oracle charged is no longer charged, a prototype-key request
// the oracle refused before the quota now is. The shift is per bearer, for
// the oracle's later x-rate-limit-remaining values in the same window.
class QuotaShift {
 public:
  void Reset() { shift_.clear(); }
  void Uncharged(const std::string& bearer, int cost) { shift_[bearer] += cost; }
  void Charged(const std::string& bearer, int cost) { shift_[bearer] -= cost; }
  // The oracle's remaining count adjusted for this bearer. A recorded "0"
  // stays "0": once exhausted the oracle's count is not known.
  std::string Remaining(const std::string& bearer, const char* oracle) const {
    if (!oracle) return std::string();
    const auto found = shift_.find(bearer);
    if (found == shift_.end() || found->second == 0) return oracle;
    const long recorded = strtol(oracle, nullptr, 10);
    if (recorded <= 0) return oracle;  // exhausted: the oracle's count is unknown
    const long value = recorded + found->second;
    return std::to_string(value < 0 ? 0 : value);
  }

 private:
  std::map<std::string, int> shift_;
};

// fastify-errors: a recorded Fastify / Node refusal and this server's answer.
// The body-framing refusals (415, the empty/invalid JSON 400s, a schema's
// FST_ERR_VALIDATION) are judged after the device policy, so they carry the
// quota headers, and an unauthenticated request gets the policy's refusal
// instead; the 414 and Node's raw-byte 400 stay before every hook.
struct FastifyExpectation {
  int status = 0;
  std::string body;
  bool charged = false;
};

// False when `oracle` is not such an envelope.
inline bool FastifyExpected(const char* oracle, const char* authorization, bool origin, FastifyExpectation* out) {
  if (!oracle) return false;
  struct Code { const char* code; const char* error; int status; bool framing; };
  static const Code kCodes[] = {
      {"\"code\":\"FST_ERR_CTP_INVALID_MEDIA_TYPE\"", "UNSUPPORTED_MEDIA_TYPE", 415, true},
      {"\"code\":\"FST_ERR_CTP_EMPTY_JSON_BODY\"", "BAD_REQUEST", 400, true},
      {"\"code\":\"FST_ERR_CTP_INVALID_JSON_BODY\"", "BAD_REQUEST", 400, true},
      {"\"code\":\"FST_ERR_CTP_INVALID_CONTENT_LENGTH\"", "BAD_REQUEST", 400, true},
      {"\"code\":\"FST_ERR_VALIDATION\"", "BAD_REQUEST", 400, true},
      {"\"code\":\"FST_ERR_MAX_PARAM_LENGTH\"", "URI_TOO_LONG", 414, false},
  };
  const Code* found = nullptr;
  if (strstr(oracle, "\"statusCode\":"))
    for (const Code& c : kCodes)
      if (strstr(oracle, c.code)) found = &c;
  static const Code kClientError{"", "BAD_REQUEST", 400, false};
  if (!found && strcmp(oracle, "{\"error\":\"Bad Request\",\"message\":\"Client Error\",\"statusCode\":400}") == 0)
    found = &kClientError;
  if (!found) return false;
  const bool credential = authorization && strncmp(authorization, "Bearer fintel_dev_", 18) == 0;
  if (found->framing && origin) {
    *out = FastifyExpectation{403, "{\"error\":\"BROWSER_ORIGIN_FORBIDDEN\"}", false};
  } else if (found->framing && !credential) {
    *out = FastifyExpectation{401, "{\"error\":\"UNAUTHORIZED\"}", false};
  } else {
    *out = FastifyExpectation{found->status, std::string("{\"error\":\"") + found->error + "\"}", found->framing};
  }
  return true;
}

}  // namespace compat_expectations
