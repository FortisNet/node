#include "mexc_price.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
using namespace fortis::providers;
namespace {
constexpr int64_t kNow = 1700000000000;
fortis::codec::BoundedJsonToken tokens[128];
CoinGeckoSpotResult extract(const std::string& body, double fx = .98, int64_t observed = kNow - 1000,
                            int64_t now = kNow, size_t capacity = 128) {
  return ExtractMexcPrice(reinterpret_cast<const unsigned char*>(body.data()), body.size(), "wiki-cat", now, fx,
                          observed, tokens, capacity);
}
std::string body(const char* price = "0.00000004", const char* time = "1700000000000") {
  return std::string("[{\"price\":\"") + price + "\",\"time\":" + time + "}]";
}
}  // namespace
int main() try {
  assert(MexcUsdSupported("wiki-cat"));
  assert(MexcUsdSupported("eip155:56:erc20:0x6ec90334d89dbdc89e08a133271be3d104128edb"));
  for (const char* id : {"WKC", "wkc", "bitcoin", "eip155:1:erc20:0x6ec90334d89dbdc89e08a133271be3d104128edb",
                         "eip155:56:erc20:0x0000000000000000000000000000000000000000", "wiki-cat?x=1", ""})
    assert(!MexcUsdSupported(id));
  const auto quote = extract(body());
  assert(quote.error == CoinGeckoSpotError::kNone && quote.count == 1);
  assert(quote.quotes[0].price == 0.00000004 * .98);
  assert(quote.quotes[0].provider_observed_at_ms == kNow - 1000);
  assert(quote.quotes[0].origin == SpotQuoteOrigin::kMexc);
  assert(strcmp(quote.quotes[0].id, "wiki-cat") == 0);
  assert(extract(body("1", "1699999700000")).quotes[0].provider_observed_at_ms == kNow - 300000);
  for (const char* price : {"0", "-1", "+1", "1e8", "NaN", "inf", ".1", "1.", "1..2", "1 2", ""})
    assert(extract(body(price)).count == 0);
  for (const char* time : {"1699999699999", "1700000000001", "1700000000000.5", "1e300", "null", "\"1700000000000\""})
    assert(extract(body("1", time)).count == 0);
  for (const char* bad :
       {"{}", "[]", "[null]", "[{},{}]", "[{\"price\":\"1\",\"price\":\"2\",\"time\":1700000000000}]",
        "[{\"price\":\"1\",\"time\":1700000000000,\"time\":1700000000000}]", "[{\"price\":1,\"time\":1700000000000}]"})
    assert(extract(bad).count == 0);
  for (double fx : {0., -1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
    assert(extract(body(), fx).count == 0);
  assert(extract(body("9999999999999999999999999999999999999999"), 1e300).count == 0);
  assert(extract(body(), std::numeric_limits<double>::denorm_min()).count == 0);
  assert(extract(body(), .98, kNow - 300001).count == 0);
  assert(extract(body(), .98, kNow + 1).count == 0);
  assert(extract(body(), .98, 0, INT64_MAX).count == 0);
  assert(extract(body(), .98, kNow, kNow, 1).count == 0);
  assert(extract(std::string(kMexcMaxResponseBytes + 1, ' ')).count == 0);
  fortis::config::ProviderRuntimeConfig config{};
  config.ca_bundle_configured = true;
  strcpy(config.ca_bundle_path, "/trusted/ca.pem");
  char path[128]{};
  fortis::http::OutboundRequest request{};
  request.header_count = 1;
  assert(BuildMexcPriceRequest(config, "wiki-cat", path, sizeof(path), &request));
  assert(request.header_count == 0 && strcmp(request.host, "api.mexc.com") == 0);
  assert(strcmp(path, "/api/v3/trades?symbol=WKCUSDT&limit=1") == 0);
  assert(request.timeout_ms == 5000 && request.max_response_bytes == kMexcMaxResponseBytes);
  assert(!BuildMexcPriceRequest(config, "wiki-cat", path, 2, &request) && !path[0] && !request.host);
  assert(!BuildMexcPriceRequest(config, "bitcoin", path, sizeof(path), &request));
  strcpy(config.ca_bundle_path, "/bad ca");
  assert(!BuildMexcPriceRequest(config, "wiki-cat", path, sizeof(path), &request));
  config.ca_bundle_configured = false;
  assert(!BuildMexcPriceRequest(config, "wiki-cat", path, sizeof(path), &request));
} catch (...) { return 1; }
