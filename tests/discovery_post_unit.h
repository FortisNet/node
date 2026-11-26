#pragma once

// Shared helpers of the R40/R47 discovery POST unit tests
// (bittensor_analytics_test.cpp, price_markets_test.cpp): fixture loading
// and a request record's body as ParseDiscoveryBody leaves it.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <string>
#include <vector>

#include "discovery_route_support.h"
#include "price_history_batch_codec.h"
#include "token_json.h"

namespace post_unit {

namespace tj = fortis::codec::tj;

inline int g_failures = 0;
inline int g_checks = 0;

inline void check(bool condition, const std::string& what, const std::string& detail = "") {
  ++g_checks;
  if (condition) return;
  ++g_failures;
  fprintf(stderr, "FAIL: %s %s\n", what.c_str(), detail.c_str());
}

inline std::vector<uint8_t> read_file(const char* path) {
  std::vector<uint8_t> data;
  FILE* file = fopen(path, "rb");
  if (!file) return data;
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  data.resize(static_cast<size_t>(size));
  if (fread(data.data(), 1, data.size(), file) != data.size()) data.clear();
  fclose(file);
  return data;
}

inline int hex_digit(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

inline std::vector<uint8_t> unhex(const tj::Value* hex) {
  std::vector<uint8_t> out;
  for (uint32_t i = 0; tj::IsString(hex) && i + 1 < hex->length; i += 2)
    out.push_back(static_cast<uint8_t>(hex_digit(hex->text[i]) * 16 + hex_digit(hex->text[i + 1])));
  return out;
}

inline std::string text(const tj::Value* value) {
  return tj::IsString(value) ? std::string(value->text, value->length) : std::string();
}

inline const tj::Value* header(const tj::Value* pairs, const char* name) {
  for (uint32_t i = 0; tj::IsArray(pairs) && i < pairs->count; ++i) {
    const tj::Value* pair = pairs->items[i];
    if (tj::IsArray(pair) && pair->count == 2 && tj::StringEquals(pair->items[0], name)) return pair->items[1];
  }
  return nullptr;
}

// True when the record's answer came from the body parser or a hook (no
// zod, no facet): 401/403/404/413/415, a Fastify error object, the quota 429.
inline bool before_validation(const tj::Value* record) {
  // compat-decisions.md#form-bodies: text/plain and form bodies are
  // refused by the body parser (415) and never reach validation.
  if (const tj::Value* type = header(tj::Get(record, "requestHeaders"), "content-type")) {
    std::string media = text(type);
    if (const auto end = media.find_first_of("; \t"); end != std::string::npos) media.resize(end);
    if (strcasecmp(media.c_str(), "text/plain") == 0 ||
        strcasecmp(media.c_str(), "application/x-www-form-urlencoded") == 0)
      return true;
  }
  const tj::Value* status = tj::Get(record, "status");
  const int code = tj::IsNumber(status) ? static_cast<int>(status->number) : 0;
  const std::string body = text(tj::Get(record, "responseText"));
  if (code == 401 || code == 403 || code == 404 || code == 413 || code == 415) return true;
  if (body.find("\"statusCode\":") != std::string::npos) return true;
  return code == 429 && body == "{\"error\":\"RATE_LIMITED\"}";
}

// The record's body as ParseDiscoveryBody leaves it (records the parser
// refused are skipped by the callers).
struct Body {
  std::vector<uint8_t> bytes;
  std::vector<fortis::codec::BoundedJsonToken> tokens = std::vector<fortis::codec::BoundedJsonToken>(fortis::discovery::kDiscoveryTokens);
  fortis::discovery::DiscoveryBody body;
};

inline bool make_body(const tj::Value* record, Body* out) {
  out->bytes = unhex(tj::Get(record, "bodyHex"));
  out->body = fortis::discovery::DiscoveryBody{};
  out->body.bytes = out->bytes.data();
  out->body.length = out->bytes.size();
  const tj::Value* type = header(tj::Get(record, "requestHeaders"), "content-type");
  if (!type) { out->body.kind = fortis::discovery::DiscoveryBodyKind::kUndefined; return out->bytes.empty(); }
  std::string media = text(type);
  if (const auto end = media.find_first_of("; \t"); end != std::string::npos) media.resize(end);
  if (strcasecmp(media.c_str(), "application/json") == 0) {
    const unsigned char* json = out->bytes.data();
    size_t length = out->bytes.size();
    if (length >= 3 && json[0] == 0xef && json[1] == 0xbb && json[2] == 0xbf) { json += 3; length -= 3; }
    fortis::codec::BoundedJsonError error{};
    if (!out->body.json.ParseWithLimit(json, length, out->tokens.data(), out->tokens.size(),
                                       fortis::codec::kBoundedJsonMaxTokens, &error)) return false;
    out->body.kind = fortis::discovery::DiscoveryBodyKind::kJson;
    return true;
  }
  if (strcasecmp(media.c_str(), "application/protobuf") == 0) {
    out->body.kind = fortis::discovery::DiscoveryBodyKind::kProtobuf;
    return true;
  }
  return false;
}

// google.protobuf.Value number_value -0 → 0 (TypeScript's L1 only).
inline std::string zero_negative_zero(std::string hex) {
  const std::string negative = "110000000000000080", zero = "110000000000000000";
  for (size_t at = hex.find(negative); at != std::string::npos; at = hex.find(negative, at + 1))
    hex.replace(at, negative.size(), zero);
  return hex;
}

inline std::string to_hex(const uint8_t* bytes, size_t length) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < length; ++i) { out += kHex[bytes[i] >> 4]; out += kHex[bytes[i] & 15]; }
  return out;
}

inline fortis::discovery::DiscoveryWorkspace* new_workspace() {
  return static_cast<fortis::discovery::DiscoveryWorkspace*>(calloc(1, sizeof(fortis::discovery::DiscoveryWorkspace)));
}

}  // namespace post_unit
