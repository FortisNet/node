#include "tronscan_request.h"

#include "bounded_json.h"
#include "price_id_parser.h"

#include <string.h>

namespace fortis::providers {

namespace {
constexpr char kHost[] = "apilist.tronscanapi.com";
constexpr char kPort[] = "443";
constexpr char kPath[] = "/api/getAssetWithPriceList";
constexpr char kKeyHeader[] = "TRON-PRO-API-KEY";
constexpr uint32_t kTimeoutMs = 10000;

bool valid_ca_path(const char* path) {
  if (!path || !*path || strnlen(path, 1024) >= 1024) return false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(path); *p; ++p)
    if (*p <= 0x20 || *p == 0x7f) return false;
  return true;
}
bool valid_key(const config::ProviderRuntimeConfig& config) {
  if (!config.tronscan_api_key_configured) return true;
  const size_t length = strnlen(config.tronscan_api_key, sizeof(config.tronscan_api_key));
  if (!length || length >= sizeof(config.tronscan_api_key)) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(config.tronscan_api_key[i]);
    if (c < 0x20 || c > 0x7e) return false;
  }
  return true;
}
}

TronScanPriceRequestError BuildTronScanPriceRequest(
    const config::ProviderRuntimeConfig& config, const char* const* source_ids, size_t id_count,
    bool anonymous, char* path_storage, size_t path_capacity,
    http::OutboundRequest* request) {
  if (request) *request = http::OutboundRequest{};
  if (path_storage && path_capacity) path_storage[0] = '\0';
  if (!request || !path_storage || !path_capacity || !source_ids || !id_count ||
      id_count > kTronScanPriceMaxIds) return TronScanPriceRequestError::kInvalidArguments;
  if (!config.ca_bundle_configured || !valid_ca_path(config.ca_bundle_path) ||
      !valid_key(config)) return TronScanPriceRequestError::kInvalidConfig;
  for (size_t i = 0; i < id_count; ++i) {
    ParsedPriceId parsed{};
    if (!source_ids[i] || !ParsePriceId(source_ids[i], strlen(source_ids[i]), &parsed) ||
        parsed.kind != PriceIdKind::kTrc10) return TronScanPriceRequestError::kInvalidArguments;
  }
  if (path_capacity <= sizeof(kPath) - 1) return TronScanPriceRequestError::kBufferTooSmall;
  memcpy(path_storage, kPath, sizeof(kPath));
  http::OutboundRequest built{};
  built.method = http::OutboundMethod::kGet;
  built.host = kHost;
  built.port = kPort;
  built.ca_file = config.ca_bundle_path;
  built.timeout_ms = kTimeoutMs;
  built.max_response_bytes = codec::kBoundedJsonMaxInputBytes;
  built.path = path_storage;
  if (config.tronscan_api_key_configured && !anonymous) {
    built.header_count = 1;
    memcpy(built.headers[0].name, kKeyHeader, sizeof(kKeyHeader));
    memcpy(built.headers[0].value, config.tronscan_api_key,
           strlen(config.tronscan_api_key) + 1);
  }
  if (!OutboundHttpsHeadersValid(built)) {
    path_storage[0] = '\0';
    return TronScanPriceRequestError::kInvalidConfig;
  }
  *request = built;
  return TronScanPriceRequestError::kNone;
}

}  // namespace fortis::providers
