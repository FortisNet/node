#include "kraken_price.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fortis::providers {

namespace {
bool unique_key(const codec::BoundedJsonDocument& doc, uint32_t object, const char* key) {
  const auto* parent = doc.token(object);
  if (!parent || parent->type != codec::BoundedJsonType::kObject) return false;
  size_t found = 0;
  for (uint32_t child = parent->first_child; child != codec::kBoundedJsonNoToken;) {
    const auto* name = doc.token(child);
    if (!name) return false;
    found += doc.StringEquals(child, key);
    const auto* value = doc.token(name->next_sibling);
    if (!value) return false;
    child = value->next_sibling;
  }
  return found == 1;
}
}  // namespace

bool BuildKrakenPriceRequest(const config::ProviderRuntimeConfig& config, std::string_view id, char* path,
                             size_t capacity, http::OutboundRequest* request) {
  if (request) *request = http::OutboundRequest{};
  if (path && capacity) path[0] = '\0';
  const char* pair = KrakenUsdPair(id);
  if (!request || !path || !capacity || !pair || !config.ca_bundle_configured) return false;
  const size_t ca_length = strnlen(config.ca_bundle_path, sizeof(config.ca_bundle_path));
  if (!ca_length || ca_length == sizeof(config.ca_bundle_path)) return false;
  for (size_t i = 0; i < ca_length; ++i)
    if (static_cast<unsigned char>(config.ca_bundle_path[i]) <= 0x20 || config.ca_bundle_path[i] == 0x7f) return false;
  const int written = snprintf(path, capacity, "/0/public/Trades?pair=%s&count=1", pair);
  if (written < 0 || static_cast<size_t>(written) >= capacity) {
    path[0] = '\0';
    return false;
  }
  http::OutboundRequest built{};
  built.host = "api.kraken.com";
  built.port = "443";
  built.path = path;
  built.ca_file = config.ca_bundle_path;
  built.timeout_ms = 5000;
  built.max_response_bytes = kKrakenMaxResponseBytes;
  if (!http::OutboundHttpsHeadersValid(built)) {
    path[0] = '\0';
    return false;
  }
  *request = built;
  return true;
}

CoinGeckoSpotResult ExtractKrakenPrice(const unsigned char* input, size_t length, std::string_view id, int64_t now_ms,
                                       codec::BoundedJsonToken* tokens, size_t capacity) {
  CoinGeckoSpotResult result{};
  result.error = CoinGeckoSpotError::kInvalidJson;
  const char* pair = KrakenUsdPair(id);
  if (!pair || now_ms < kKrakenMaxTradeAgeMs || now_ms > 253402300799999LL || length > kKrakenMaxResponseBytes)
    return result;
  codec::BoundedJsonDocument doc;
  codec::BoundedJsonError error{};
  if (!doc.Parse(input, length, tokens, capacity, &error)) return result;
  if (!unique_key(doc, doc.root(), "error") || !unique_key(doc, doc.root(), "result")) return result;
  const auto* errors = doc.token(doc.FindObjectValue(doc.root(), "error"));
  if (!errors || errors->type != codec::BoundedJsonType::kArray || errors->first_child != codec::kBoundedJsonNoToken)
    return result;
  const uint32_t body = doc.FindObjectValue(doc.root(), "result");
  if (!unique_key(doc, body, pair)) return result;
  const auto* trades = doc.token(doc.FindObjectValue(body, pair));
  if (!trades || trades->type != codec::BoundedJsonType::kArray) return result;
  const auto* trade = doc.token(trades->first_child);
  if (!trade || trade->type != codec::BoundedJsonType::kArray || trade->next_sibling != codec::kBoundedJsonNoToken)
    return result;
  const auto* price = doc.token(trade->first_child);
  char decimal[64]{};
  size_t decimal_length = 0;
  if (!price || !doc.DecodeString(trade->first_child, decimal, sizeof(decimal), &decimal_length) || !decimal_length)
    return result;
  size_t dots = 0;
  for (size_t i = 0; i < decimal_length; ++i) {
    if (decimal[i] == '.') ++dots;
    else if (decimal[i] < '0' || decimal[i] > '9') return result;
  }
  if (dots > 1 || decimal[0] == '.' || decimal[decimal_length - 1] == '.') return result;
  char* end = nullptr;
  const double value = strtod(decimal, &end);
  if (end != decimal + decimal_length || !std::isfinite(value) || value <= 0) return result;
  const auto* volume = doc.token(price->next_sibling);
  if (!volume) return result;
  double seconds = 0;
  if (!doc.Number(volume->next_sibling, &seconds) || !std::isfinite(seconds) || seconds <= 0 ||
      seconds > 253402300799.999)
    return result;
  const int64_t observed = static_cast<int64_t>(seconds * 1000);
  if (observed < now_ms - kKrakenMaxTradeAgeMs || observed > now_ms) return result;
  auto& quote = result.quotes[0];
  memcpy(quote.id, id.data(), id.size());
  quote.price = value;
  quote.provider_observed_at_ms = observed;
  quote.origin = SpotQuoteOrigin::kKraken;
  result.count = 1;
  result.error = CoinGeckoSpotError::kNone;
  return result;
}

}  // namespace fortis::providers
