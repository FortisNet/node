#include "defillama_price.h"
#include "price_id_parser.h"
#include "mexc_price.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace fortis::providers {
namespace {
size_t key_count(const codec::BoundedJsonDocument& doc, uint32_t object, const char* key) {
  const auto* parent = doc.token(object);
  if (!parent || parent->type != codec::BoundedJsonType::kObject) return 0;
  size_t found = 0;
  for (uint32_t child = parent->first_child; child != codec::kBoundedJsonNoToken;) {
    const auto* name = doc.token(child);
    if (!name) return 0;
    found += doc.StringEquals(child, key);
    const auto* value = doc.token(name->next_sibling);
    if (!value) return 0;
    child = value->next_sibling;
  }
  return found;
}

bool read_quote(const codec::BoundedJsonDocument& doc, uint32_t coins, const char* key, int64_t now_ms, double* price,
                int64_t* observed) {
  const uint32_t row = doc.FindObjectValue(coins, key);
  if (key_count(doc, coins, key) != 1 || key_count(doc, row, "price") != 1 || key_count(doc, row, "timestamp") != 1 ||
      key_count(doc, row, "confidence") != 1)
    return false;
  double timestamp = 0, confidence = 0;
  if (!doc.Number(doc.FindObjectValue(row, "price"), price) || !std::isfinite(*price) || *price <= 0 ||
      !doc.Number(doc.FindObjectValue(row, "timestamp"), &timestamp) || !std::isfinite(timestamp) || timestamp <= 0 ||
      timestamp > 253402300799.999 || !doc.Number(doc.FindObjectValue(row, "confidence"), &confidence) ||
      !std::isfinite(confidence) || confidence < .9 || confidence > 1)
    return false;
  *observed = static_cast<int64_t>(timestamp * 1000);
  return *observed <= now_ms && *observed >= now_ms - 300000;
}

bool valid_ids(const char* const* ids, size_t count) {
  if (!ids || !count || count > kDefiLlamaMaxBatch) return false;
  char key[kDefiLlamaKeyCapacity]{};
  for (size_t i = 0; i < count; ++i) {
    if (!ids[i]) return false;
    const size_t size = strnlen(ids[i], kPriceIdMaxSourceIdBytes + 1);
    if (size > kPriceIdMaxSourceIdBytes || !DefiLlamaPriceKey({ids[i], size}, key, sizeof(key))) return false;
    for (size_t j = 0; j < i; ++j)
      if (strcmp(ids[i], ids[j]) == 0) return false;
  }
  return true;
}
}  // namespace

bool DefiLlamaPriceKey(std::string_view id, char* output, size_t capacity) {
  if (!output || !capacity) return false;
  output[0] = '\0';
  ParsedPriceId parsed{};
  if (!ParsePriceId(id.data(), id.size(), &parsed) || id != parsed.source_id) return false;
  const char* chain = nullptr;
  if (parsed.kind == PriceIdKind::kCoinGecko) chain = "coingecko";
  else if (parsed.kind == PriceIdKind::kContract && strcmp(parsed.standard, "erc20") == 0) {
    const char* platform = CoinGeckoPlatformForNetwork(parsed.network);
    if (platform) {
      struct Mapping {
        const char* platform;
        const char* chain;
      };
      static constexpr Mapping mappings[] = {{"ethereum", "ethereum"},
                                             {"binance-smart-chain", "bsc"},
                                             {"arbitrum-one", "arbitrum"},
                                             {"optimistic-ethereum", "optimism"},
                                             {"polygon-pos", "polygon"},
                                             {"avalanche", "avax"},
                                             {"base", "base"},
                                             {"cronos", "cronos"},
                                             {"xdc-network", "xdc"}};
      for (const auto& mapping : mappings)
        if (strcmp(platform, mapping.platform) == 0) chain = mapping.chain;
    }
  }
  if (!chain) return false;
  const int written = snprintf(output, capacity, "%s:%s", chain,
                               parsed.kind == PriceIdKind::kCoinGecko ? parsed.source_id : parsed.address);
  if (written < 0 || static_cast<size_t>(written) >= capacity) {
    output[0] = '\0';
    return false;
  }
  return true;
}

bool BuildDefiLlamaPriceRequest(const config::ProviderRuntimeConfig& config, const char* const* ids, size_t count,
                                char* path, size_t capacity, http::OutboundRequest* request) {
  if (request) *request = http::OutboundRequest{};
  if (path && capacity) path[0] = '\0';
  if (!request || !path || !capacity || !valid_ids(ids, count) || !config.ca_bundle_configured) return false;
  const size_t ca_length = strnlen(config.ca_bundle_path, sizeof(config.ca_bundle_path));
  if (!ca_length || ca_length == sizeof(config.ca_bundle_path)) return false;
  for (size_t i = 0; i < ca_length; ++i)
    if (static_cast<unsigned char>(config.ca_bundle_path[i]) <= 0x20 || config.ca_bundle_path[i] == 0x7f) return false;
  size_t used = 0;
  for (size_t i = 0; i < count; ++i) {
    char key[kDefiLlamaKeyCapacity]{};
    if (!DefiLlamaPriceKey(ids[i], key, sizeof(key))) {
      path[0] = '\0';
      return false;
    }
    const int written = snprintf(path + used, capacity - used, "%s%s", i ? "," : "/prices/current/", key);
    if (written < 0 || static_cast<size_t>(written) >= capacity - used) {
      path[0] = '\0';
      return false;
    }
    used += static_cast<size_t>(written);
  }
  bool needs_fx = false, has_fx = false;
  for (size_t i = 0; i < count; ++i) {
    needs_fx |= MexcUsdSupported(ids[i]);
    has_fx |= strcmp(ids[i], "tether") == 0;
  }
  if (needs_fx && !has_fx) {
    const int written = snprintf(path + used, capacity - used, ",coingecko:tether");
    if (written < 0 || static_cast<size_t>(written) >= capacity - used) {
      path[0] = '\0';
      return false;
    }
  }
  http::OutboundRequest built{};
  built.host = "coins.llama.fi";
  built.port = "443";
  built.path = path;
  built.ca_file = config.ca_bundle_path;
  built.timeout_ms = 5000;
  built.max_response_bytes = kDefiLlamaMaxResponseBytes;
  if (!http::OutboundHttpsHeadersValid(built)) {
    path[0] = '\0';
    return false;
  }
  *request = built;
  return true;
}

CoinGeckoSpotResult ExtractDefiLlamaPrices(const unsigned char* input, size_t length, const char* const* ids,
                                           size_t count, int64_t now_ms, codec::BoundedJsonToken* tokens,
                                           size_t capacity) {
  CoinGeckoSpotResult result{};
  result.error = CoinGeckoSpotError::kInvalidJson;
  if (!valid_ids(ids, count) || now_ms < 300000 || now_ms > 253402300799999LL || length > kDefiLlamaMaxResponseBytes)
    return result;
  codec::BoundedJsonDocument doc;
  codec::BoundedJsonError error{};
  if (!doc.Parse(input, length, tokens, capacity, &error) || key_count(doc, doc.root(), "coins") != 1) return result;
  const uint32_t coins = doc.FindObjectValue(doc.root(), "coins");
  const auto* parent = doc.token(coins);
  if (!parent || parent->type != codec::BoundedJsonType::kObject) return result;
  for (size_t i = 0; i < count; ++i) {
    char key[kDefiLlamaKeyCapacity]{};
    if (!DefiLlamaPriceKey(ids[i], key, sizeof(key))) return result;
    const uint32_t row = doc.FindObjectValue(coins, key);
    if (row == codec::kBoundedJsonNoToken) continue;
    if (key_count(doc, coins, key) != 1 || key_count(doc, row, "price") > 1 || key_count(doc, row, "timestamp") > 1 ||
        key_count(doc, row, "confidence") > 1)
      return result;
    double price = 0;
    int64_t observed = 0;
    if (!read_quote(doc, coins, key, now_ms, &price, &observed)) continue;
    auto& quote = result.quotes[result.count++];
    memcpy(quote.id, ids[i], strlen(ids[i]) + 1);
    quote.price = price;
    quote.origin = SpotQuoteOrigin::kDefiLlama;
    quote.provider_observed_at_ms = observed;
  }
  result.error = CoinGeckoSpotError::kNone;
  return result;
}
bool ExtractDefiLlamaTetherUsd(const unsigned char* input, size_t length, int64_t now_ms,
                               codec::BoundedJsonToken* tokens, size_t capacity, double* rate,
                               int64_t* observed_at_ms) {
  if (rate) *rate = 0;
  if (observed_at_ms) *observed_at_ms = 0;
  if (!rate || !observed_at_ms || now_ms < 300000 || now_ms > 253402300799999LL || length > kDefiLlamaMaxResponseBytes)
    return false;
  codec::BoundedJsonDocument doc;
  codec::BoundedJsonError error{};
  if (!doc.Parse(input, length, tokens, capacity, &error) || key_count(doc, doc.root(), "coins") != 1) return false;
  double value = 0;
  int64_t observed = 0;
  if (!read_quote(doc, doc.FindObjectValue(doc.root(), "coins"), "coingecko:tether", now_ms, &value, &observed))
    return false;
  *rate = value;
  *observed_at_ms = observed;
  return true;
}
}  // namespace fortis::providers
