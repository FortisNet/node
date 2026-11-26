#include "mexc_price.h"
#include "price_id_parser.h"
#include <algorithm>
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

bool MexcUsdSupported(std::string_view id) {
  ParsedPriceId parsed{};
  if (!ParsePriceId(id.data(), id.size(), &parsed) || id != parsed.source_id) return false;
  if (parsed.kind == PriceIdKind::kCoinGecko) return id == "wiki-cat";
  const char* platform = CoinGeckoPlatformForNetwork(parsed.network);
  return parsed.kind == PriceIdKind::kContract && strcmp(parsed.standard, "erc20") == 0 && platform &&
         strcmp(platform, "binance-smart-chain") == 0 &&
         strcmp(parsed.address, "0x6ec90334d89dbdc89e08a133271be3d104128edb") == 0;
}
bool BuildMexcPriceRequest(const config::ProviderRuntimeConfig& config, std::string_view id, char* path,
                           size_t capacity, http::OutboundRequest* request) {
  if (request) *request = http::OutboundRequest{};
  if (path && capacity) path[0] = '\0';
  if (!request || !path || !capacity || !MexcUsdSupported(id) || !config.ca_bundle_configured) return false;
  const size_t ca_length = strnlen(config.ca_bundle_path, sizeof(config.ca_bundle_path));
  if (!ca_length || ca_length == sizeof(config.ca_bundle_path)) return false;
  for (size_t i = 0; i < ca_length; ++i)
    if (static_cast<unsigned char>(config.ca_bundle_path[i]) <= 0x20 || config.ca_bundle_path[i] == 0x7f) return false;
  const int written = snprintf(path, capacity, "/api/v3/trades?symbol=WKCUSDT&limit=1");
  if (written < 0 || static_cast<size_t>(written) >= capacity) {
    path[0] = '\0';
    return false;
  }
  http::OutboundRequest built{};
  built.host = "api.mexc.com";
  built.port = "443";
  built.path = path;
  built.ca_file = config.ca_bundle_path;
  built.timeout_ms = 5000;
  built.max_response_bytes = kMexcMaxResponseBytes;
  if (!http::OutboundHttpsHeadersValid(built)) {
    path[0] = '\0';
    return false;
  }
  *request = built;
  return true;
}
CoinGeckoSpotResult ExtractMexcPrice(const unsigned char* input, size_t length, std::string_view id, int64_t now_ms,
                                     double usdt_usd, int64_t fx_observed_at_ms, codec::BoundedJsonToken* tokens,
                                     size_t capacity) {
  CoinGeckoSpotResult result{};
  result.error = CoinGeckoSpotError::kInvalidJson;
  if (!MexcUsdSupported(id) || now_ms < 300000 || now_ms > 253402300799999LL || length > kMexcMaxResponseBytes ||
      !std::isfinite(usdt_usd) || usdt_usd <= 0 || fx_observed_at_ms < now_ms - 300000 || fx_observed_at_ms > now_ms)
    return result;
  codec::BoundedJsonDocument doc;
  codec::BoundedJsonError error{};
  if (!doc.Parse(input, length, tokens, capacity, &error)) return result;
  const auto* root = doc.token(doc.root());
  if (!root || root->type != codec::BoundedJsonType::kArray) return result;
  const uint32_t row = root->first_child;
  const auto* trade = doc.token(row);
  if (!trade || trade->next_sibling != codec::kBoundedJsonNoToken || !unique_key(doc, row, "price") ||
      !unique_key(doc, row, "time"))
    return result;
  char decimal[64]{};
  size_t size = 0;
  if (!doc.DecodeString(doc.FindObjectValue(row, "price"), decimal, sizeof(decimal), &size) || !size) return result;
  size_t dots = 0;
  for (size_t i = 0; i < size; ++i) {
    if (decimal[i] == '.') ++dots;
    else if (decimal[i] < '0' || decimal[i] > '9') return result;
  }
  if (dots > 1 || decimal[0] == '.' || decimal[size - 1] == '.') return result;
  char* end = nullptr;
  const double price = strtod(decimal, &end);
  const double usd = price * usdt_usd;
  if (end != decimal + size || !std::isfinite(price) || price <= 0 || !std::isfinite(usd) || usd <= 0) return result;
  double time = 0;
  if (!doc.Number(doc.FindObjectValue(row, "time"), &time) || !std::isfinite(time) ||
      time < static_cast<double>(now_ms - 300000) || time > static_cast<double>(now_ms) || std::trunc(time) != time)
    return result;
  auto& quote = result.quotes[0];
  memcpy(quote.id, id.data(), id.size());
  quote.price = usd;
  quote.provider_observed_at_ms = std::min(static_cast<int64_t>(time), fx_observed_at_ms);
  quote.origin = SpotQuoteOrigin::kMexc;
  result.count = 1;
  result.error = CoinGeckoSpotError::kNone;
  return result;
}
}  // namespace fortis::providers
