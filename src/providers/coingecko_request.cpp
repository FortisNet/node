#include "coingecko_request.h"

#include "bounded_json.h"
#include "price_id_parser.h"

#include <string.h>

namespace fortis::providers {

namespace {
constexpr char kFreeHost[] = "api.coingecko.com";
constexpr char kProHost[] = "pro-api.coingecko.com";
constexpr char kPort[] = "443";
constexpr char kPrefix[] = "/api/v3/simple/price?ids=";
constexpr char kBetweenIds[] = "%2C";
constexpr char kSuffix[] = "&vs_currencies=";
constexpr char kFields[] =
    "&include_24hr_change=true&include_24hr_vol=true";
constexpr char kObservationField[] = "&include_last_updated_at=true";
constexpr char kDemoHeader[] = "x-cg-demo-api-key";
constexpr char kProHeader[] = "x-cg-pro-api-key";
constexpr uint32_t kRequestTimeoutMs = 10000;

bool normalize_slug(const char* input, char output[101], size_t* length) {
  if (!input || !length) return false;
  const size_t n = strnlen(input, 101);
  if (!n || n >= 101) return false;
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = static_cast<unsigned char>(input[i]);
    if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + ('a' - 'A'));
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          (i && c == '-'))) return false;
    output[i] = static_cast<char>(c);
  }
  output[n] = '\0';
  *length = n;
  return true;
}

bool valid_fiat(const char* fiat, size_t* length) {
  if (!fiat || !length) return false;
  const size_t n = strnlen(fiat, 6);
  if (n < 3 || n > 5) return false;
  for (size_t i = 0; i < n; ++i)
    if (fiat[i] < 'a' || fiat[i] > 'z') return false;
  *length = n;
  return true;
}

bool valid_ca_path(const char* path) {
  if (!path || !*path || strnlen(path, 1024) >= 1024) return false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(path); *p; ++p)
    if (*p <= 0x20 || *p == 0x7f) return false;
  return true;
}

bool valid_configured_key(const config::ProviderRuntimeConfig& config) {
  if (!config.coingecko_api_key_configured) return true;
  const size_t length = strnlen(config.coingecko_api_key,
                                sizeof(config.coingecko_api_key));
  if (!length || length >= sizeof(config.coingecko_api_key)) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(config.coingecko_api_key[i]);
    if (c < 0x20 || c > 0x7e) return false;
  }
  return true;
}

void append(char* output, size_t* cursor, const char* source, size_t length) {
  memcpy(output + *cursor, source, length);
  *cursor += length;
}
}  // namespace

CoinGeckoSpotRequestError BuildCoinGeckoSpotRequest(
    const config::ProviderRuntimeConfig& config, const char* const* ids, size_t id_count,
    const char* fiat, char* path_storage, size_t path_capacity,
    http::OutboundRequest* request) {
  if (request) *request = http::OutboundRequest{};
  if (path_storage && path_capacity) path_storage[0] = '\0';
  if (!request || !path_storage || !path_capacity || !ids || !id_count ||
      id_count > 100) return CoinGeckoSpotRequestError::kInvalidArguments;
  size_t fiat_length = 0;
  if (!valid_fiat(fiat, &fiat_length))
    return CoinGeckoSpotRequestError::kInvalidArguments;
  if (!config.ca_bundle_configured ||
      !valid_ca_path(config.ca_bundle_path) ||
      (config.coingecko_tier != config::CoinGeckoTier::kDemo &&
       config.coingecko_tier != config::CoinGeckoTier::kPro) ||
      !valid_configured_key(config))
    return CoinGeckoSpotRequestError::kInvalidConfig;

  char normalized[100][101]{};
  size_t lengths[100]{};
  size_t ids_length = 0;
  for (size_t i = 0; i < id_count; ++i) {
    if (!normalize_slug(ids[i], normalized[i], &lengths[i]))
      return CoinGeckoSpotRequestError::kInvalidArguments;
    const size_t separator = i ? sizeof(kBetweenIds) - 1 : 0;
    if (ids_length > SIZE_MAX - separator - lengths[i])
      return CoinGeckoSpotRequestError::kBufferTooSmall;
    ids_length += separator + lengths[i];
  }
  const size_t prefix_length = sizeof(kPrefix) - 1;
  const size_t suffix_length = sizeof(kSuffix) - 1;
  const size_t fields_length = sizeof(kFields) - 1;
  const size_t observation_length = config.price_snapshot_enabled &&
      strcmp(fiat, "usd") == 0 ? sizeof(kObservationField) - 1 : 0;
  if (ids_length > SIZE_MAX - prefix_length - suffix_length - fiat_length -
          fields_length - observation_length)
    return CoinGeckoSpotRequestError::kBufferTooSmall;
  const size_t path_length = prefix_length + ids_length + suffix_length +
                             fiat_length + fields_length + observation_length;
  if (path_length > kCoinGeckoSpotMaxPathBytes ||
      path_capacity <= path_length)
    return CoinGeckoSpotRequestError::kBufferTooSmall;

  http::OutboundRequest built{};
  built.method = http::OutboundMethod::kGet;
  const bool pro = config.coingecko_tier == config::CoinGeckoTier::kPro &&
                   config.coingecko_api_key_configured;
  built.host = pro ? kProHost : kFreeHost;
  built.port = kPort;
  built.ca_file = config.ca_bundle_path;
  built.timeout_ms = kRequestTimeoutMs;
  built.max_response_bytes = codec::kBoundedJsonMaxInputBytes;
  built.path = path_storage;

  size_t cursor = 0;
  append(path_storage, &cursor, kPrefix, prefix_length);
  for (size_t i = 0; i < id_count; ++i) {
    if (i) append(path_storage, &cursor, kBetweenIds, sizeof(kBetweenIds) - 1);
    append(path_storage, &cursor, normalized[i], lengths[i]);
  }
  append(path_storage, &cursor, kSuffix, suffix_length);
  append(path_storage, &cursor, fiat, fiat_length);
  append(path_storage, &cursor, kFields, fields_length);
  append(path_storage, &cursor, kObservationField, observation_length);
  path_storage[cursor] = '\0';

  // HttpClient.json sends `accept: application/json` on every CoinGecko request.
  built.header_count = 1;
  memcpy(built.headers[0].name, "accept", sizeof("accept"));
  memcpy(built.headers[0].value, "application/json", sizeof("application/json"));
  if (config.coingecko_api_key_configured) {
    built.header_count = 2;
    const char* header = pro ? kProHeader : kDemoHeader;
    memcpy(built.headers[1].name, header, strlen(header) + 1);
    memcpy(built.headers[1].value, config.coingecko_api_key,
           strlen(config.coingecko_api_key) + 1);
  }
  if (!OutboundHttpsHeadersValid(built)) {
    path_storage[0] = '\0';
    return CoinGeckoSpotRequestError::kInvalidConfig;
  }
  *request = built;
  return CoinGeckoSpotRequestError::kNone;
}

CoinGeckoSpotRequestError BuildCoinGeckoTokenPriceRequest(
    const config::ProviderRuntimeConfig& config, const char* platform,
    const char* const* source_ids, size_t id_count, const char* fiat,
    char* path_storage, size_t path_capacity, http::OutboundRequest* request) {
  if (request) *request = http::OutboundRequest{};
  if (path_storage && path_capacity) path_storage[0] = '\0';
  if (!request || !path_storage || !path_capacity || !platform || !source_ids ||
      !id_count || id_count > kCoinGeckoSpotMaxBatchCount)
    return CoinGeckoSpotRequestError::kInvalidArguments;
  size_t fiat_length = 0;
  if (!valid_fiat(fiat, &fiat_length)) return CoinGeckoSpotRequestError::kInvalidArguments;
  if (!config.ca_bundle_configured || !valid_ca_path(config.ca_bundle_path) ||
      (config.coingecko_tier != config::CoinGeckoTier::kDemo && config.coingecko_tier != config::CoinGeckoTier::kPro) ||
      !valid_configured_key(config)) return CoinGeckoSpotRequestError::kInvalidConfig;

  size_t platform_length = strnlen(platform, 64);
  if (!platform_length || platform_length >= 64) return CoinGeckoSpotRequestError::kInvalidArguments;
  for (size_t i = 0; i < platform_length; ++i)
    if (!((platform[i] >= 'a' && platform[i] <= 'z') ||
          (platform[i] >= '0' && platform[i] <= '9') || platform[i] == '-'))
      return CoinGeckoSpotRequestError::kInvalidArguments;
  constexpr char prefix[] = "/api/v3/simple/token_price/";
  constexpr char query[] = "?contract_addresses=";
  constexpr char fiat_prefix[] = "&vs_currencies=";
  constexpr char fields[] = "&include_24hr_change=true";
  const size_t fixed = sizeof(prefix) - 1 + platform_length + sizeof(query) - 1 +
      sizeof(fiat_prefix) - 1 + fiat_length + sizeof(fields) - 1 +
      sizeof(kObservationField) - 1;
  size_t addresses_length = 0;
  for (size_t i = 0; i < id_count; ++i) {
    if (!source_ids[i]) return CoinGeckoSpotRequestError::kInvalidArguments;
    ParsedPriceId parsed{};
    if (!ParsePriceId(source_ids[i], strnlen(source_ids[i], kPriceIdMaxSourceIdBytes + 1), &parsed) ||
        parsed.kind != PriceIdKind::kContract ||
        !CoinGeckoPlatformForNetwork(parsed.network) ||
        strcmp(CoinGeckoPlatformForNetwork(parsed.network), platform) != 0)
      return CoinGeckoSpotRequestError::kInvalidArguments;
    const size_t address_length = strlen(parsed.address);
    size_t encoded_length = 0;
    for (size_t j = 0; j < address_length; ++j) {
      const unsigned char c = static_cast<unsigned char>(parsed.address[j]);
      const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
      encoded_length += unreserved ? 1 : 3;
    }
    if (addresses_length > SIZE_MAX - encoded_length - (i ? 3 : 0))
      return CoinGeckoSpotRequestError::kBufferTooSmall;
    addresses_length += encoded_length + (i ? 3 : 0);
  }
  if (fixed > SIZE_MAX - addresses_length) return CoinGeckoSpotRequestError::kBufferTooSmall;
  const size_t path_length = fixed + addresses_length;
  if (path_length > kCoinGeckoTokenPriceMaxPathBytes || path_capacity <= path_length)
    return CoinGeckoSpotRequestError::kBufferTooSmall;

  const bool pro = config.coingecko_tier == config::CoinGeckoTier::kPro && config.coingecko_api_key_configured;
  http::OutboundRequest built{};
  built.method = http::OutboundMethod::kGet;
  built.host = pro ? kProHost : kFreeHost;
  built.port = kPort;
  built.ca_file = config.ca_bundle_path;
  built.timeout_ms = kRequestTimeoutMs;
  built.max_response_bytes = codec::kBoundedJsonMaxInputBytes;
  built.path = path_storage;
  size_t cursor = 0;
  append(path_storage, &cursor, prefix, sizeof(prefix) - 1);
  append(path_storage, &cursor, platform, platform_length);
  append(path_storage, &cursor, query, sizeof(query) - 1);
  static constexpr char hex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < id_count; ++i) {
    ParsedPriceId parsed{};
    ParsePriceId(source_ids[i], strlen(source_ids[i]), &parsed);
    if (i) append(path_storage, &cursor, "%2C", 3);
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(parsed.address); *p; ++p) {
      const unsigned char c = *p;
      const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
      if (unreserved) append(path_storage, &cursor, reinterpret_cast<const char*>(p), 1);
      else { const char escape[] = {'%', hex[c >> 4], hex[c & 15]}; append(path_storage, &cursor, escape, sizeof(escape)); }
    }
  }
  append(path_storage, &cursor, fiat_prefix, sizeof(fiat_prefix) - 1);
  append(path_storage, &cursor, fiat, fiat_length);
  append(path_storage, &cursor, fields, sizeof(fields) - 1);
  append(path_storage, &cursor, kObservationField, sizeof(kObservationField) - 1);
  path_storage[cursor] = '\0';
  built.header_count = 1;
  memcpy(built.headers[0].name, "accept", sizeof("accept"));
  memcpy(built.headers[0].value, "application/json", sizeof("application/json"));
  if (config.coingecko_api_key_configured) {
    built.header_count = 2;
    const char* header = pro ? kProHeader : kDemoHeader;
    memcpy(built.headers[1].name, header, strlen(header) + 1);
    memcpy(built.headers[1].value, config.coingecko_api_key, strlen(config.coingecko_api_key) + 1);
  }
  if (!OutboundHttpsHeadersValid(built)) { path_storage[0] = '\0'; return CoinGeckoSpotRequestError::kInvalidConfig; }
  *request = built;
  return CoinGeckoSpotRequestError::kNone;
}

CoinGeckoSpotPlanError BuildCoinGeckoSpotBatchPlan(
    const config::ProviderRuntimeConfig& config, const char* const* ids, size_t id_count,
    const char* fiat, CoinGeckoSpotBatchPlan* plan) {
  if (!plan) return CoinGeckoSpotPlanError::kInvalidArguments;
  plan->id_count = 0;
  plan->batch_count = 0;
  if (!ids || id_count == 0 || id_count > kCoinGeckoSpotMaxBatchCount)
    return CoinGeckoSpotPlanError::kInvalidArguments;
  const auto fail = [plan](CoinGeckoSpotPlanError error) {
    plan->id_count = 0;
    plan->batch_count = 0;
    return error;
  };

  for (size_t i = 0; i < id_count; ++i) {
    char normalized[101]; size_t length = 0;
    if (!normalize_slug(ids[i], normalized, &length))
      return fail(CoinGeckoSpotPlanError::kInvalidArguments);
    bool duplicate = false;
    for (size_t j = 0; j < plan->id_count; ++j) {
      if (strcmp(plan->ids[j], normalized) == 0) { duplicate = true; break; }
    }
    if (duplicate) continue;
    memcpy(plan->ids[plan->id_count], normalized, length + 1);
    ++plan->id_count;
  }
  if (plan->id_count == 0) return fail(CoinGeckoSpotPlanError::kInvalidArguments);

  const char* pointers[kCoinGeckoSpotMaxBatchCount]{};
  for (size_t i = 0; i < plan->id_count; ++i) pointers[i] = plan->ids[i];
  size_t start = 0;
  while (start < plan->id_count) {
    size_t best_count = 0;
    size_t best_path_length = 0;
    char path[kCoinGeckoSpotMaxPathBytes + 1];
    for (size_t count = 1; start + count <= plan->id_count; ++count) {
      http::OutboundRequest request{};
      const CoinGeckoSpotRequestError error = BuildCoinGeckoSpotRequest(
          config, pointers + start, count, fiat, path, sizeof(path), &request);
      if (error == CoinGeckoSpotRequestError::kNone &&
          strlen(path) <= kCoinGeckoSpotMaxBatchPathBytes) {
        best_count = count;
        best_path_length = strlen(path);
        continue;
      }
      if (error == CoinGeckoSpotRequestError::kNone) break;
      if (error == CoinGeckoSpotRequestError::kBufferTooSmall) {
        if (best_count == 0) return fail(CoinGeckoSpotPlanError::kPathLimit);
        break;
      }
      if (error == CoinGeckoSpotRequestError::kInvalidConfig)
        return fail(CoinGeckoSpotPlanError::kInvalidConfig);
      return fail(CoinGeckoSpotPlanError::kInvalidArguments);
    }
    if (best_count == 0 || plan->batch_count >= kCoinGeckoSpotMaxBatchCount)
      return fail(CoinGeckoSpotPlanError::kPathLimit);
    plan->batches[plan->batch_count++] = {start, best_count, best_path_length};
    start += best_count;
  }
  return CoinGeckoSpotPlanError::kNone;
}

}  // namespace fortis::providers
