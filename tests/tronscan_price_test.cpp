#include "native_config.h"
#include "tronscan_price.h"
#include "tronscan_request.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace fortis::codec;
using namespace fortis::config;
using namespace fortis::http;
using namespace fortis::providers;

namespace {
CoinGeckoSpotResult extract(const char* json, const char* const* ids, size_t count) {
  BoundedJsonToken tokens[kBoundedJsonMaxTokens]{};
  return ExtractTronScanTrc10Prices(reinterpret_cast<const unsigned char*>(json),
      strlen(json), ids, count, tokens, kBoundedJsonMaxTokens);
}
}

int main() {
  NativeConfig config{};
  config.ca_bundle_configured = true;
  strcpy(config.ca_bundle_path, "/tmp/ca.pem");
  config.tronscan_api_key_configured = true;
  strcpy(config.tronscan_api_key, "unit-test-key");
  const char* ids[] = {"tron:trc10:1001", "tron:trc10:1002", "tron:trc10:1003",
                       "tron:trc10:1004", "tron:trc10:1005", "tron:trc10:1006",
                       "tron:trc10:1007", "tron:trc10:1008"};
  char path[128]{};
  OutboundRequest request{};
  assert(BuildTronScanPriceRequest(config, ids, 8, false, path, sizeof(path), &request) ==
         TronScanPriceRequestError::kNone);
  assert(strcmp(request.host, "apilist.tronscanapi.com") == 0);
  assert(strcmp(request.path, "/api/getAssetWithPriceList") == 0);
  assert(request.header_count == 1 && strcmp(request.headers[0].name, "TRON-PRO-API-KEY") == 0 &&
         strcmp(request.headers[0].value, "unit-test-key") == 0);
  assert(BuildTronScanPriceRequest(config, ids, 8, true, path, sizeof(path), &request) ==
         TronScanPriceRequestError::kNone && request.header_count == 0);
  NativeConfig anonymous = config;
  anonymous.tronscan_api_key_configured = false;
  assert(BuildTronScanPriceRequest(anonymous, ids, 8, false, path, sizeof(path), &request) ==
         TronScanPriceRequestError::kNone && request.header_count == 0);
  const char* hundred[kTronScanPriceMaxIds]{};
  char storage[kTronScanPriceMaxIds][40]{};
  for (size_t i = 0; i < kTronScanPriceMaxIds; ++i) {
    snprintf(storage[i], sizeof(storage[i]), "tron:trc10:%zu", 20000000 + i);
    hundred[i] = storage[i];
  }
  assert(BuildTronScanPriceRequest(config, hundred, kTronScanPriceMaxIds, false,
      path, sizeof(path), &request) == TronScanPriceRequestError::kNone);
  assert(BuildTronScanPriceRequest(config, hundred, kTronScanPriceMaxIds + 1, false,
      path, sizeof(path), &request) == TronScanPriceRequestError::kInvalidArguments);
  const char* invalid[] = {"ethereum"};
  assert(BuildTronScanPriceRequest(config, invalid, 1, false, path, sizeof(path), &request) ==
         TronScanPriceRequestError::kInvalidArguments);
  NativeConfig no_ca{};
  assert(BuildTronScanPriceRequest(no_ca, ids, 1, false, path, sizeof(path), &request) ==
         TronScanPriceRequestError::kInvalidConfig);

  const char* json = R"([not used])";
  (void)json;
  const char* body = R"({"data":[
    {"id":1001,"type":"trc10","priceInUsd":1.25},
    {"id":"1002","type":"TRC10","priceInUsd":" 2.5 "},
    {"id":1003,"type":"trc10","priceInUsd":0},
    {"id":1004,"type":"trc10","priceInUsd":-2},
    {"id":1005,"type":"trc10","priceInUsd":""},
    {"id":1006,"type":"trc10","priceInUsd":"Infinity"},
    {"id":1007,"type":"trc10","priceInUsd":"1.25e2"},
    {"id":1008,"type":"trc10","priceInUsd":"0x10"},
    {"id":1001,"type":"trc20","priceInUsd":999},
    {"id":1001,"type":"trc10","priceInUsd":"bad"},
    {"id":1001,"type":"TrC10","priceInUsd":3.5}
  ]})";
  CoinGeckoSpotResult result = extract(body, ids, 8);
  assert(result.error == CoinGeckoSpotError::kNone && result.count == 4);
  assert(strcmp(result.quotes[0].id, ids[0]) == 0 && result.quotes[0].price == 3.5);
  assert(strcmp(result.quotes[1].id, ids[1]) == 0 && result.quotes[1].price == 2.5);
  assert(strcmp(result.quotes[2].id, ids[6]) == 0 && result.quotes[2].price == 125);
  assert(strcmp(result.quotes[3].id, ids[7]) == 0 && result.quotes[3].price == 16);
  const char* numeric_ids[] = {"tron:trc10:1010", "tron:trc10:1011", "tron:trc10:1012",
                               "tron:trc10:1013", "tron:trc10:1014"};
  const char* numeric_rules = R"({"data":[
    {"id":1010,"type":"trc10","priceInUsd":"0b101"},
    {"id":1011,"type":"trc10","priceInUsd":"0o10"},
    {"id":1012,"type":"trc10","priceInUsd":"+0x10"},
    {"id":1013,"type":"trc10","priceInUsd":" "},
    {"id":1014,"type":"trc10","priceInUsd":"1e309"}
  ]})";
  result = extract(numeric_rules, numeric_ids, 5);
  assert(result.error == CoinGeckoSpotError::kNone && result.count == 2);
  assert(strcmp(result.quotes[0].id, numeric_ids[0]) == 0 && result.quotes[0].price == 5);
  assert(strcmp(result.quotes[1].id, numeric_ids[1]) == 0 && result.quotes[1].price == 8);
  assert(extract("{}", ids, 1).error == CoinGeckoSpotError::kNone);
  assert(extract("{\"data\":null}", ids, 1).error == CoinGeckoSpotError::kNone);
  assert(extract("null", ids, 1).error == CoinGeckoSpotError::kInvalidJson);
  assert(extract("{", ids, 1).error == CoinGeckoSpotError::kInvalidJson);
  // Compact validation storage preserves the full 20-digit bound, leading
  // zero canonicalization, caller aliases, duplicate rows and maximum batch.
  const char* boundary_ids[] = {"tron:trc10:12345678901234567890", "tron:trc10:0001001"};
  result = extract(R"({"data":[{"id":"12345678901234567890","type":"trc10","priceInUsd":7},
      {"id":"1001","type":"trc10","priceInUsd":8}]})", boundary_ids, 2);
  assert(result.error == CoinGeckoSpotError::kNone && result.count == 2);
  assert(strcmp(result.quotes[0].id, boundary_ids[0]) == 0 && result.quotes[0].price == 7);
  assert(strcmp(result.quotes[1].id, boundary_ids[1]) == 0 && result.quotes[1].price == 8);
  const char* too_long[] = {"tron:trc10:123456789012345678901"};
  assert(extract("{}", too_long, 1).error == CoinGeckoSpotError::kInvalidArguments);
  const char* zero[] = {"tron:trc10:0000"};
  assert(extract("{}", zero, 1).error == CoinGeckoSpotError::kInvalidArguments);
  char maximum_body[16384]{};
  size_t used = static_cast<size_t>(snprintf(maximum_body, sizeof(maximum_body), "{\"data\":["));
  for (size_t i = 0; i < kTronScanPriceMaxIds; ++i) {
    const int written = snprintf(maximum_body + used, sizeof(maximum_body) - used,
        "%s{\"id\":\"%zu\",\"type\":\"trc10\",\"priceInUsd\":1}", i ? "," : "", 20000000 + i);
    assert(written > 0 && static_cast<size_t>(written) < sizeof(maximum_body) - used);
    used += static_cast<size_t>(written);
  }
  assert(used + 3 < sizeof(maximum_body));
  memcpy(maximum_body + used, "]}", 3);
  result = extract(maximum_body, hundred, kTronScanPriceMaxIds);
  assert(result.error == CoinGeckoSpotError::kNone && result.count == kTronScanPriceMaxIds);
  for (size_t i = 0; i < kTronScanPriceMaxIds; ++i)
    assert(strcmp(result.quotes[i].id, hundred[i]) == 0 && result.quotes[i].price == 1);
  puts("native TronScan TRC-10 request and extraction tests passed");
}
