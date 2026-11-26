#include "native_config.h"
#include "vestige_price.h"
#include "vestige_request.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace fortis::codec;
using namespace fortis::config;
using namespace fortis::http;
using namespace fortis::providers;

namespace {
BoundedJsonToken tokens[kBoundedJsonMaxTokens];
char path[kVestigePriceMaxPathBytes + 1];
CoinGeckoSpotResult extract(const char* json, const char* const* ids, size_t count) {
  return ExtractVestigePrices(reinterpret_cast<const unsigned char*>(json), strlen(json), ids, count,
                              tokens, kBoundedJsonMaxTokens);
}
}
int main() {
  NativeConfig config{};
  OutboundRequest request{};
  const char* ids[] = {"asa:31566705", "asa:31566706", "asa:31566707", "asa:31566708"};
  assert(BuildVestigePriceRequest(config, ids, 2, path, sizeof(path), &request) ==
         VestigePriceRequestError::kInvalidConfig);
  config.ca_bundle_configured = true;
  strcpy(config.ca_bundle_path, "/tmp/vestige-ca.pem");
  assert(BuildVestigePriceRequest(config, ids, 2, path, sizeof(path), &request) ==
         VestigePriceRequestError::kNone);
  assert(strcmp(request.host, "api.vestigelabs.org") == 0 && strcmp(request.port, "443") == 0);
  assert(strcmp(path, "/assets/price?network_id=0&asset_ids=31566705%2C31566706&denominating_asset_id=31566704") == 0);
  assert(request.path == path && request.header_count == 0 && request.timeout_ms == 10000 &&
         request.max_response_bytes == kBoundedJsonMaxInputBytes);
  const char* bad[] = {"asa:12x"};
  assert(BuildVestigePriceRequest(config, bad, 1, path, sizeof(path), &request) ==
         VestigePriceRequestError::kInvalidArguments);
  char ids_storage[101][25]{};
  const char* ids_ptr[100]{};
  for (size_t i = 0; i < 100; ++i) {
    snprintf(ids_storage[i], sizeof(ids_storage[i]), "asa:%llu",
        1000000000000000000ULL + static_cast<unsigned long long>(i));
    ids_ptr[i] = ids_storage[i];
  }
  assert(BuildVestigePriceRequest(config, ids_ptr, 100, path, sizeof(path), &request) ==
         VestigePriceRequestError::kNone);
  assert(strlen(path) <= kVestigePriceMaxPathBytes);
  assert(BuildVestigePriceRequest(config, ids_ptr, 101, path, sizeof(path), &request) ==
         VestigePriceRequestError::kInvalidArguments);

  const char* requested[] = {"asa:31566705", "asa:31566706", "asa:31566707", "asa:31566708", "asa:31566709"};
  const char json[] = "[{\"asset_id\":31566705,\"price\":1.25},{\"asset_id\":31566706,\"price\":0},"
      "{\"asset_id\":31566707,\"price\":-2.5},{\"asset_id\":31566708,\"price\":3},"
      "{\"asset_id\":31566708,\"price\":4},{\"asset_id\":31566709,\"price\":\"2\"},"
      "{\"asset_id\":31566705,\"price\":\"bad\"},{\"asset_id\":999,\"price\":2}]";
  CoinGeckoSpotResult result = extract(json, requested, 5);
  assert(result.error == CoinGeckoSpotError::kNone && result.count == 4);
  assert(strcmp(result.quotes[0].id, requested[0]) == 0 && result.quotes[0].price == 1.25);
  assert(strcmp(result.quotes[1].id, requested[1]) == 0 && result.quotes[1].price == 0);
  assert(strcmp(result.quotes[2].id, requested[2]) == 0 && result.quotes[2].price == -2.5);
  assert(strcmp(result.quotes[3].id, requested[3]) == 0 && result.quotes[3].price == 4);
  // Non-array JSON is a valid empty Vestige response; malformed JSON fails.
  assert(extract("null", requested, 1).error == CoinGeckoSpotError::kNone);
  assert(extract("{}", requested, 1).error == CoinGeckoSpotError::kNone);
  assert(extract("[", requested, 1).error == CoinGeckoSpotError::kInvalidJson);
  // Vestige maps String(number), so a leading-zero request key does not match numeric 31566705.
  const char* padded[] = {"asa:031566705"};
  assert(extract("[{\"asset_id\":31566705,\"price\":1}]", padded, 1).count == 0);
  const char* zero_id[] = {"asa:0"};
  CoinGeckoSpotResult zero = extract("[{\"asset_id\":-0,\"price\":1}]", zero_id, 1);
  assert(zero.error == CoinGeckoSpotError::kNone && zero.count == 1 &&
         strcmp(zero.quotes[0].id, "asa:0") == 0 && zero.quotes[0].price == 1);
  puts("native Vestige request and extraction tests passed");
}
