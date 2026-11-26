#include "jupiter_request.h"

#include "bounded_json.h"

#include <string.h>

namespace fortis::providers {

namespace {
constexpr char kLiteHost[] = "lite-api.jup.ag";
constexpr char kKeyedHost[] = "api.jup.ag";
constexpr char kPort[] = "443";
constexpr char kPrefix[] = "/price/v3?ids=";
constexpr char kBetweenMints[] = "%2C";
constexpr char kKeyHeader[] = "x-api-key";
constexpr uint32_t kRequestTimeoutMs = 10000;

bool base58_char(unsigned char c) {
  if (c >= '1' && c <= '9') return true;
  if (c >= 'A' && c <= 'Z') return c != 'I' && c != 'O';
  if (c >= 'a' && c <= 'z') return c != 'l';
  return false;
}

bool valid_ca_path(const char* path) {
  if (!path || !*path || strnlen(path, 1024) >= 1024) return false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(path); *p; ++p)
    if (*p <= 0x20 || *p == 0x7f) return false;
  return true;
}

bool valid_configured_key(const config::ProviderRuntimeConfig& config) {
  if (!config.jupiter_api_key_configured) return true;
  const size_t length = strnlen(config.jupiter_api_key, sizeof(config.jupiter_api_key));
  if (!length || length >= sizeof(config.jupiter_api_key)) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(config.jupiter_api_key[i]);
    if (c < 0x20 || c > 0x7e) return false;
  }
  return true;
}

void append(char* output, size_t* cursor, const char* source, size_t length) {
  memcpy(output + *cursor, source, length);
  *cursor += length;
}
}  // namespace

bool JupiterValidMint(const char* mint, size_t* length) {
  if (!mint) return false;
  const size_t n = strnlen(mint, kJupiterMintMaxBytes + 1);
  if (n < kJupiterMintMinBytes || n > kJupiterMintMaxBytes) return false;
  for (size_t i = 0; i < n; ++i)
    if (!base58_char(static_cast<unsigned char>(mint[i]))) return false;
  if (length) *length = n;
  return true;
}

JupiterPriceRequestError BuildJupiterPriceRequest(
    const config::ProviderRuntimeConfig& config, const char* const* mints, size_t mint_count,
    char* path_storage, size_t path_capacity, http::OutboundRequest* request) {
  if (request) *request = http::OutboundRequest{};
  if (path_storage && path_capacity) path_storage[0] = '\0';
  if (!request || !path_storage || !path_capacity || !mints || !mint_count ||
      mint_count > kJupiterPriceMaxMints)
    return JupiterPriceRequestError::kInvalidArguments;
  if (!config.ca_bundle_configured || !valid_ca_path(config.ca_bundle_path) ||
      !valid_configured_key(config))
    return JupiterPriceRequestError::kInvalidConfig;

  size_t lengths[kJupiterPriceMaxMints]{};
  size_t path_length = sizeof(kPrefix) - 1;
  for (size_t i = 0; i < mint_count; ++i) {
    if (!JupiterValidMint(mints[i], &lengths[i]))
      return JupiterPriceRequestError::kInvalidArguments;
    path_length += lengths[i] + (i ? sizeof(kBetweenMints) - 1 : 0);
  }
  if (path_length > kJupiterPriceMaxPathBytes || path_capacity <= path_length)
    return JupiterPriceRequestError::kBufferTooSmall;

  http::OutboundRequest built{};
  built.method = http::OutboundMethod::kGet;
  built.host = config.jupiter_api_key_configured ? kKeyedHost : kLiteHost;
  built.port = kPort;
  built.ca_file = config.ca_bundle_path;
  built.timeout_ms = kRequestTimeoutMs;
  built.max_response_bytes = codec::kBoundedJsonMaxInputBytes;
  built.path = path_storage;

  size_t cursor = 0;
  append(path_storage, &cursor, kPrefix, sizeof(kPrefix) - 1);
  for (size_t i = 0; i < mint_count; ++i) {
    if (i) append(path_storage, &cursor, kBetweenMints, sizeof(kBetweenMints) - 1);
    append(path_storage, &cursor, mints[i], lengths[i]);
  }
  path_storage[cursor] = '\0';

  if (config.jupiter_api_key_configured) {
    built.header_count = 1;
    memcpy(built.headers[0].name, kKeyHeader, sizeof(kKeyHeader));
    memcpy(built.headers[0].value, config.jupiter_api_key,
           strlen(config.jupiter_api_key) + 1);
  }
  if (!OutboundHttpsHeadersValid(built)) {
    path_storage[0] = '\0';
    return JupiterPriceRequestError::kInvalidConfig;
  }
  *request = built;
  return JupiterPriceRequestError::kNone;
}

}  // namespace fortis::providers
