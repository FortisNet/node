#include "vestige_request.h"

#include "bounded_json.h"
#include "price_id_parser.h"

#include <string.h>

namespace fortis::providers {

namespace {
constexpr char kHost[] = "api.vestigelabs.org";
constexpr char kPort[] = "443";
constexpr char kPrefix[] = "/assets/price?network_id=0&asset_ids=";
constexpr char kBetween[] = "%2C";
constexpr char kSuffix[] = "&denominating_asset_id=31566704";
constexpr uint32_t kTimeoutMs = 10000;

bool valid_ca_path(const char* path) {
  if (!path || !*path || strnlen(path, 1024) >= 1024) return false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(path); *p; ++p)
    if (*p <= 0x20 || *p == 0x7f) return false;
  return true;
}
}

VestigePriceRequestError BuildVestigePriceRequest(
    const config::ProviderRuntimeConfig& config, const char* const* source_ids, size_t id_count,
    char* path_storage, size_t path_capacity, http::OutboundRequest* request) {
  if (request) *request = http::OutboundRequest{};
  if (path_storage && path_capacity) path_storage[0] = '\0';
  if (!request || !path_storage || !path_capacity || !source_ids || !id_count || id_count > kVestigePriceMaxIds)
    return VestigePriceRequestError::kInvalidArguments;
  if (!config.ca_bundle_configured || !valid_ca_path(config.ca_bundle_path))
    return VestigePriceRequestError::kInvalidConfig;
  char ids[kVestigePriceMaxIds][21]{};
  size_t lengths[kVestigePriceMaxIds]{};
  size_t path_length = sizeof(kPrefix) - 1 + sizeof(kSuffix) - 1;
  for (size_t i = 0; i < id_count; ++i) {
    ParsedPriceId parsed{};
    if (!source_ids[i] || !ParsePriceId(source_ids[i], strlen(source_ids[i]), &parsed) ||
        parsed.kind != PriceIdKind::kAsa) return VestigePriceRequestError::kInvalidArguments;
    lengths[i] = strlen(parsed.address);
    memcpy(ids[i], parsed.address, lengths[i] + 1);
    path_length += lengths[i] + (i ? sizeof(kBetween) - 1 : 0);
  }
  if (path_length > kVestigePriceMaxPathBytes || path_capacity <= path_length)
    return VestigePriceRequestError::kBufferTooSmall;
  size_t cursor = 0;
  memcpy(path_storage + cursor, kPrefix, sizeof(kPrefix) - 1); cursor += sizeof(kPrefix) - 1;
  for (size_t i = 0; i < id_count; ++i) {
    if (i) { memcpy(path_storage + cursor, kBetween, sizeof(kBetween) - 1); cursor += sizeof(kBetween) - 1; }
    memcpy(path_storage + cursor, ids[i], lengths[i]); cursor += lengths[i];
  }
  memcpy(path_storage + cursor, kSuffix, sizeof(kSuffix) - 1); cursor += sizeof(kSuffix) - 1;
  path_storage[cursor] = '\0';
  http::OutboundRequest built{};
  built.method = http::OutboundMethod::kGet;
  built.host = kHost;
  built.port = kPort;
  built.ca_file = config.ca_bundle_path;
  built.timeout_ms = kTimeoutMs;
  built.max_response_bytes = codec::kBoundedJsonMaxInputBytes;
  built.path = path_storage;
  if (!OutboundHttpsHeadersValid(built)) { path_storage[0] = '\0'; return VestigePriceRequestError::kInvalidConfig; }
  *request = built;
  return VestigePriceRequestError::kNone;
}

}  // namespace fortis::providers
