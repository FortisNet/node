#include "defillama_price.h"

#include <cassert>
#include <cstring>
#include <string>

using namespace fortis::providers;

namespace {
constexpr int64_t kNow = 1700000000000;
fortis::codec::BoundedJsonToken tokens[2048];
const char* ids[] = {"bitcoin", "ethereum", "eip155:56:erc20:0x6ec90334d89dbdc89e08a133271be3d104128edb"};
CoinGeckoSpotResult extract(const std::string& body, size_t capacity = 2048, int64_t now = kNow) {
  return ExtractDefiLlamaPrices(reinterpret_cast<const unsigned char*>(body.data()), body.size(), ids, 3, now, tokens,
                                capacity);
}
std::string body(const char* price = "12.5", const char* timestamp = "1700000000", const char* confidence = "0.99") {
  return std::string("{\"coins\":{\"coingecko:bitcoin\":{\"price\":") + price + ",\"timestamp\":" + timestamp +
         ",\"confidence\":" + confidence + "}}}";
}
}  // namespace
int main() try {
  char key[kDefiLlamaKeyCapacity]{};
  for (const char* coin : {"bitcoin", "ethereum", "binancecoin", "ripple", "solana", "algorand", "tron", "avalanche-2",
                           "the-open-network", "unknown-slug"}) {
    assert(DefiLlamaPriceKey(coin, key, sizeof(key)));
    assert(std::string(key) == std::string("coingecko:") + coin);
  }
  assert(DefiLlamaPriceKey(ids[2], key, sizeof(key)));
  assert(std::string(key) == "bsc:0x6ec90334d89dbdc89e08a133271be3d104128edb");
  for (const char* bad : {"", "BTC", " bitcoin", "bitcoin/../../x", "bitcoin?x=y",
                          "eip155:999:erc20:0x6ec90334d89dbdc89e08a133271be3d104128edb", "asa:1"}) {
    assert(!DefiLlamaPriceKey(bad, key, sizeof(key)));
    assert(key[0] == '\0');
  }
  assert(!DefiLlamaPriceKey("bitcoin", key, 2));
  assert(!DefiLlamaPriceKey("bitcoin", nullptr, 0));
  const auto valid = extract(body());
  assert(valid.error == CoinGeckoSpotError::kNone && valid.count == 1);
  assert(valid.quotes[0].price == 12.5 && valid.quotes[0].provider_observed_at_ms == kNow);
  assert(valid.quotes[0].origin == SpotQuoteOrigin::kDefiLlama);
  assert(strcmp(valid.quotes[0].id, "bitcoin") == 0);
  assert(extract(body("1", "1699999700")).count == 1);
  for (const char* t : {"1699999699.999", "1700000000.001", "1e300", "0", "-1", "null", "true", "\"1700000000\""})
    assert(extract(body("1", t)).count == 0);
  for (const char* v : {"0", "-1", "1e309", "null", "true", "\"12.5\""}) assert(extract(body(v)).count == 0);
  for (const char* c : {"0.8999", "1.001", "-1", "null", "\"1\""})
    assert(extract(body("1", "1700000000", c)).count == 0);
  assert(extract(body("1", "1700000000", "0.9")).count == 1);
  for (const char* bad :
       {"{}", "{", "[]", "{\"coins\":null}", "{\"coins\":{},\"coins\":{}}",
        "{\"coins\":{\"coingecko:bitcoin\":{},\"coingecko:bitcoin\":{}}}",
        "{\"coins\":{\"coingecko:bitcoin\":{\"price\":1,\"price\":2,\"timestamp\":1700000000,\"confidence\":1}}}"})
    assert(extract(bad).error != CoinGeckoSpotError::kNone);
  assert(extract("{\"coins\":{\"coingecko:bitcoin\":{},\"coingecko:ethereum\":{\"price\":2,\"timestamp\":1700000000,"
                 "\"confidence\":1}}}")
             .count == 1);
  assert(extract("{\"coins\":{}}").error == CoinGeckoSpotError::kNone);
  assert(extract(body(), 1).error != CoinGeckoSpotError::kNone);
  assert(extract(body(), 2048, INT64_MAX).count == 0);
  assert(extract(std::string(kDefiLlamaMaxResponseBytes + 1, ' ')).count == 0);
  double rate = 1;
  int64_t observed = 1;
  auto fx_body = body("0.98");
  fx_body.replace(fx_body.find("bitcoin"), 7, "tether");
  assert(ExtractDefiLlamaTetherUsd(reinterpret_cast<const unsigned char*>(fx_body.data()), fx_body.size(), kNow, tokens,
                                   2048, &rate, &observed));
  assert(rate == .98 && observed == kNow);
  for (const auto& invalid : {body(), std::string("{\"coins\":{}}"), std::string("{}"),
                              std::string("{\"coins\":{\"coingecko:tether\":{},\"coingecko:tether\":{}}}")}) {
    assert(!ExtractDefiLlamaTetherUsd(reinterpret_cast<const unsigned char*>(invalid.data()), invalid.size(), kNow,
                                      tokens, 2048, &rate, &observed));
    assert(rate == 0 && observed == 0);
  }
  fortis::config::ProviderRuntimeConfig config{};
  config.ca_bundle_configured = true;
  strcpy(config.ca_bundle_path, "/trusted/ca.pem");
  fortis::http::OutboundRequest request{};
  request.header_count = 1;
  strcpy(request.headers[0].name, "x-cg-demo-api-key");
  strcpy(request.headers[0].value, "not-forwarded");
  char path[4096]{};
  assert(BuildDefiLlamaPriceRequest(config, ids, 3, path, sizeof(path), &request));
  assert(request.header_count == 0 && strcmp(request.host, "coins.llama.fi") == 0);
  assert(std::string(path) ==
         std::string(
             "/prices/current/"
             "coingecko:bitcoin,coingecko:ethereum,bsc:0x6ec90334d89dbdc89e08a133271be3d104128edb,coingecko:tether"));
  assert(request.max_response_bytes == kDefiLlamaMaxResponseBytes && request.timeout_ms == 5000);
  assert(!BuildDefiLlamaPriceRequest(config, ids, 3, path, 3, &request));
  assert(!path[0] && request.host == nullptr);
  const char* duplicate[] = {"bitcoin", "bitcoin"};
  assert(!BuildDefiLlamaPriceRequest(config, duplicate, 2, path, sizeof(path), &request));
  assert(!BuildDefiLlamaPriceRequest(config, nullptr, 1, path, sizeof(path), &request));
  assert(!BuildDefiLlamaPriceRequest(config, ids, kDefiLlamaMaxBatch + 1, path, sizeof(path), &request));
  strcpy(config.ca_bundle_path, "/bad ca");
  assert(!BuildDefiLlamaPriceRequest(config, ids, 3, path, sizeof(path), &request));
  config.ca_bundle_configured = false;
  assert(!BuildDefiLlamaPriceRequest(config, ids, 3, path, sizeof(path), &request));
} catch (...) { return 1; }
