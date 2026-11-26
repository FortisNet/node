#include "native_config.h"
#include "../src/providers/coingecko_contract_price.h"
#include "../src/providers/coingecko_request.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace fortis::codec;
using namespace fortis::config;
using namespace fortis::http;
using namespace fortis::providers;

int main() {
  NativeConfig config{};
  config.ca_bundle_configured = true;
  strcpy(config.ca_bundle_path, "/tmp/ca.pem");
  config.coingecko_tier = CoinGeckoTier::kDemo;
  const char* ids[] = {
      "eth-mainnet:erc20:0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "tron:trc20:TXLAQ63Xg1NAzckPwKHvzw7CSEmLMEqcdj",
      "aptos:0x1::coin::Coin"};
  char path[kCoinGeckoTokenPriceMaxPathBytes + 1]{};
  OutboundRequest request{};
  assert(BuildCoinGeckoTokenPriceRequest(config, "ethereum", ids, 1, "usd",
      path, sizeof(path), &request) == CoinGeckoSpotRequestError::kNone);
  assert(strcmp(path, "/api/v3/simple/token_price/ethereum?contract_addresses=0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa&vs_currencies=usd&include_24hr_change=true&include_last_updated_at=true") == 0);
  assert(BuildCoinGeckoTokenPriceRequest(config, "ethereum", ids, 2, "usd",
      path, sizeof(path), &request) == CoinGeckoSpotRequestError::kInvalidArguments);
  const char* null_id[] = {nullptr};
  assert(BuildCoinGeckoTokenPriceRequest(config, "ethereum", null_id, 1, "usd",
      path, sizeof(path), &request) == CoinGeckoSpotRequestError::kInvalidArguments);
  assert(BuildCoinGeckoTokenPriceRequest(config, "tron", ids + 1, 1, "usd",
      path, sizeof(path), &request) == CoinGeckoSpotRequestError::kNone);
  assert(strstr(path, "TXLAQ63Xg1NAzckPwKHvzw7CSEmLMEqcdj") != nullptr);
  assert(BuildCoinGeckoTokenPriceRequest(config, "aptos", ids + 2, 1, "usd",
      path, sizeof(path), &request) == CoinGeckoSpotRequestError::kNone);
  assert(strstr(path, "contract_addresses=0x1%3A%3Acoin%3A%3ACoin") != nullptr);
  char bounded_ids[101][96]{};
  const char* bounded_ptrs[101]{};
  for (size_t i = 0; i < 101; ++i) {
    snprintf(bounded_ids[i], sizeof(bounded_ids[i]),
        "eip155:1:erc20:0x%040zx", i);
    bounded_ptrs[i] = bounded_ids[i];
  }
  assert(BuildCoinGeckoTokenPriceRequest(config, "ethereum", bounded_ptrs, 100,
      "usd", path, sizeof(path), &request) == CoinGeckoSpotRequestError::kNone);
  assert(strlen(path) <= kCoinGeckoTokenPriceMaxPathBytes);
  assert(BuildCoinGeckoTokenPriceRequest(config, "ethereum", bounded_ptrs, 101,
      "usd", path, sizeof(path), &request) == CoinGeckoSpotRequestError::kInvalidArguments);

  const unsigned char json[] =
      "{\"0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\":{\"usd\":12.5,\"usd_24h_change\":1.25,\"last_updated_at\":1794679959},"
      "\"TXLAQ63Xg1NAzckPwKHvzw7CSEmLMEqcdj\":{\"usd\":7.25,\"last_updated_at\":1.25},"
      "\"0x1::coin::Coin\":{\"usd\":9},\"0xbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\":{\"usd\":99}}";
  BoundedJsonToken tokens[kBoundedJsonMaxTokens];
  uint32_t keys[kBoundedJsonMaxTokens];
  CoinGeckoSpotResult result = ExtractCoinGeckoTokenPrice(json, sizeof(json) - 1,
      ids, 3, "usd", tokens, kBoundedJsonMaxTokens, keys, kBoundedJsonMaxTokens);
  assert(result.error == CoinGeckoSpotError::kNone && result.count == 3);
  assert(strcmp(result.quotes[0].id, ids[0]) == 0 && result.quotes[0].price == 12.5 &&
         result.quotes[0].hasChange24hPct && result.quotes[0].change24hPct == 1.25 &&
         !result.quotes[0].hasVolume24h);
  assert(strcmp(result.quotes[1].id, ids[1]) == 0 && result.quotes[1].price == 7.25);
  assert(strcmp(result.quotes[2].id, ids[2]) == 0 && result.quotes[2].price == 9);
  assert(result.quotes[0].provider_observed_at_ms == 1794679959000LL);
  assert(result.quotes[1].provider_observed_at_ms == 0 && result.quotes[2].provider_observed_at_ms == 0);
  const char* alias_ids[] = {
      "eth-mainnet:erc20:0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "eip155:1:erc20:0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
  result = ExtractCoinGeckoTokenPrice(json, sizeof(json) - 1, alias_ids, 2,
      "usd", tokens, kBoundedJsonMaxTokens, keys, kBoundedJsonMaxTokens);
  assert(result.error == CoinGeckoSpotError::kNone && result.count == 2);
  assert(strcmp(result.quotes[0].id, alias_ids[0]) == 0 &&
         strcmp(result.quotes[1].id, alias_ids[1]) == 0);
  const unsigned char null_json[] = "null";
  result = ExtractCoinGeckoTokenPrice(null_json, sizeof(null_json) - 1, ids, 1,
      "usd", tokens, kBoundedJsonMaxTokens, keys, kBoundedJsonMaxTokens);
  assert(result.error == CoinGeckoSpotError::kInvalidJson && result.count == 0);
  const unsigned char array_json[] = "[]";
  result = ExtractCoinGeckoTokenPrice(array_json, sizeof(array_json) - 1, ids, 1,
      "usd", tokens, kBoundedJsonMaxTokens, keys, kBoundedJsonMaxTokens);
  assert(result.error == CoinGeckoSpotError::kNone && result.count == 0);
  const unsigned char uppercase_evm[] =
      "{\"0xAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\":{\"usd\":12.5}}";
  result = ExtractCoinGeckoTokenPrice(uppercase_evm, sizeof(uppercase_evm) - 1,
      ids, 1, "usd", tokens, kBoundedJsonMaxTokens, keys, kBoundedJsonMaxTokens);
  assert(result.error == CoinGeckoSpotError::kNone && result.count == 0);
  puts("native CoinGecko contract request and extraction tests passed");
}
