#include "kraken_price.h"

#include <cassert>
#include <cstring>
#include <string>

using namespace fortis::providers;

namespace {
constexpr int64_t kNow = 1700000000000;
fortis::codec::BoundedJsonToken tokens[256];

CoinGeckoSpotResult extract(const std::string& body, std::string_view id = "tron", int64_t now = kNow,
                            size_t capacity = 256) {
  return ExtractKrakenPrice(reinterpret_cast<const unsigned char*>(body.data()), body.size(), id, now, tokens,
                            capacity);
}

std::string body(const char* pair, const char* price, const char* time) {
  return std::string("{\"error\":[],\"result\":{\"") + pair + "\":[[\"" + price + "\",\"1\"," + time +
         ",\"b\",\"m\",\"\",1]],\"last\":\"x\"}}";
}
}  // namespace

int main() {
  const char* ids[] = {"tron", "avalanche-2", "the-open-network"};
  const char* pairs[] = {"TRXUSD", "AVAXUSD", "TONUSD"};
  for (size_t i = 0; i < 3; ++i) {
    const auto result = extract(body(pairs[i], "12.345", "1699999999.5"), ids[i]);
    assert(result.error == CoinGeckoSpotError::kNone && result.count == 1);
    assert(strcmp(result.quotes[0].id, ids[i]) == 0 && result.quotes[0].price == 12.345);
    assert(result.quotes[0].provider_observed_at_ms == kNow - 500);
    assert(result.quotes[0].origin == SpotQuoteOrigin::kKraken);
    assert(!result.quotes[0].hasChange24hPct && !result.quotes[0].hasVolume24h);
  }
  for (const char* invalid :
       {"0", "-1", "nan", "inf", "1e309", "1e2", " 1", "1x", ".", ".1", "1.", "1..2", "", "0.0000000000000000000"})
    assert(extract(body("TRXUSD", invalid, "1700000000")).error != CoinGeckoSpotError::kNone);
  for (const char* invalid : {"0", "-1", "1e300", "1700000006", "1699999699.999", "null", "true", "\"1700000000\""})
    assert(extract(body("TRXUSD", "1", invalid)).error != CoinGeckoSpotError::kNone);
  assert(extract(body("TRXUSD", "1", "1699999700")).count == 1);
  assert(extract(body("TRXUSD", "1", "1700000000.001")).count == 0);
  for (const char* invalid :
       {"{}", "[]", "{", "{\"error\":[\"EGeneral:failure\"],\"result\":{}}",
        "{\"error\":[],\"error\":[\"failure\"],\"result\":{}}",
        "{\"error\":[],\"result\":{\"TRXUSD\":[],\"TRXUSD\":[]}}", "{\"error\":[],\"result\":{\"TRXUSD\":[[]]}}",
        "{\"error\":[],\"result\":{\"TRXUSD\":[[\"1\"]]}}"})
    assert(extract(invalid).error != CoinGeckoSpotError::kNone);
  const auto valid = body("TRXUSD", "1", "1700000000");
  assert(extract(valid, "TRX").count == 0);
  assert(extract(valid, "bitcoin").count == 0);
  assert(extract(valid, "avalanche-2").count == 0);
  assert(extract(valid, "tron", -1).count == 0);
  assert(extract(valid, "tron", INT64_MAX).count == 0);
  assert(extract(valid, "tron", kNow, 1).count == 0);
  assert(extract(std::string(kKrakenMaxResponseBytes + 1, ' ')).count == 0);
  assert(extract(body("TRXUSD", "1", "1700000000") + "x").count == 0);

  fortis::config::ProviderRuntimeConfig config{};
  config.ca_bundle_configured = true;
  strcpy(config.ca_bundle_path, "/trusted/ca.pem");
  fortis::http::OutboundRequest request{};
  char path[128]{};
  for (size_t i = 0; i < 3; ++i) {
    request.header_count = 1;
    strcpy(request.headers[0].name, "x-cg-demo-api-key");
    strcpy(request.headers[0].value, "synthetic-key-must-not-be-forwarded");
    assert(BuildKrakenPriceRequest(config, ids[i], path, sizeof(path), &request));
    assert(strcmp(request.host, "api.kraken.com") == 0 && strcmp(request.port, "443") == 0);
    assert(request.header_count == 0 && request.timeout_ms == 5000);
    assert(request.max_response_bytes == kKrakenMaxResponseBytes);
    assert(std::string(path) == std::string("/0/public/Trades?pair=") + pairs[i] + "&count=1");
  }
  assert(!BuildKrakenPriceRequest(config, "tron", path, 2, &request));
  assert(path[0] == '\0' && request.host == nullptr);
  assert(!BuildKrakenPriceRequest(config, "TRX", path, sizeof(path), &request));
  assert(!BuildKrakenPriceRequest(config, "tron", nullptr, 0, &request));
  assert(!BuildKrakenPriceRequest(config, "tron", path, sizeof(path), nullptr));
  config.ca_bundle_path[0] = '\0';
  assert(!BuildKrakenPriceRequest(config, "tron", path, sizeof(path), &request));
  strcpy(config.ca_bundle_path, "/bad ca.pem");
  assert(!BuildKrakenPriceRequest(config, "tron", path, sizeof(path), &request));
  memset(config.ca_bundle_path, 'x', sizeof(config.ca_bundle_path));
  assert(!BuildKrakenPriceRequest(config, "tron", path, sizeof(path), &request));
  config.ca_bundle_configured = false;
  assert(!BuildKrakenPriceRequest(config, "tron", path, sizeof(path), &request));
}
