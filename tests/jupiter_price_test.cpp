#include "native_config.h"
#include "jupiter_price.h"
#include "jupiter_request.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace fortis::codec;
using namespace fortis::config;
using namespace fortis::http;
using namespace fortis::providers;

namespace {
BoundedJsonToken g_tokens[kBoundedJsonMaxTokens];
unsigned char g_large[kBoundedJsonMaxInputBytes + 1];
char g_mints[kJupiterPriceMaxMints + 1][kJupiterMintCapacity];
const char* g_mint_pointers[kJupiterPriceMaxMints + 1];
char g_json[32768];
char g_path[kJupiterPriceMaxPathBytes + 64];

// Values match the frozen tests/corpus/price_spot_jupiter_route.json.
constexpr char kUsdc[] = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
constexpr char kSol[] = "So11111111111111111111111111111111111111112";
constexpr char kBonk[] = "DezXAZ8z7PnrnRJjz3wXBoRgixCa6xjnB7YaB1pPB263";

JupiterPriceResult extract(const char* json, const char* const* mints, size_t count) {
  return ExtractJupiterPriceV3(reinterpret_cast<const unsigned char*>(json),
                               strlen(json), mints, count, g_tokens,
                               kBoundedJsonMaxTokens);
}

void test_contract_values_and_omitted_fields() {
  static const char json[] =
      "{\"EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v\":{\"usdPrice\":0.9998,"
      "\"priceChange24h\":-0.0125,\"blockId\":123},"
      "\"DezXAZ8z7PnrnRJjz3wXBoRgixCa6xjnB7YaB1pPB263\":{\"usdPrice\":0.0000213},"
      "\"BadStringPrice111111111111111111\":{\"usdPrice\":\"12.5\"},"
      "\"BadNu11Row1111111111111111111111\":null,"
      "\"BadNoPrice1111111111111111111111\":{\"priceChange24h\":2},"
      "\"BadChange11111111111111111111111\":{\"usdPrice\":3.25,\"priceChange24h\":\"x\"},"
      "\"So11111111111111111111111111111111111111112\":{\"usdPrice\":182.37,"
      "\"priceChange24h\":2.5}}";
  const char* mints[] = {kUsdc, kBonk, "BadStringPrice111111111111111111",
                         "BadNu11Row1111111111111111111111",
                         "BadNoPrice1111111111111111111111",
                         "BadChange11111111111111111111111"};
  const JupiterPriceResult result = extract(json, mints, 6);
  assert(result.error == JupiterPriceError::kNone && result.count == 3);
  assert(strcmp(result.quotes[0].mint, kUsdc) == 0 && result.quotes[0].price == 0.9998 &&
         result.quotes[0].hasChange24hPct && result.quotes[0].change24hPct == -0.0125);
  assert(strcmp(result.quotes[1].mint, kBonk) == 0 && result.quotes[1].price == 0.0000213 &&
         !result.quotes[1].hasChange24hPct);
  // A string change is dropped (null), never coerced; the price still counts.
  assert(strcmp(result.quotes[2].mint, "BadChange11111111111111111111111") == 0 &&
         result.quotes[2].price == 3.25 && !result.quotes[2].hasChange24hPct);
  // Unrequested keys (SOL above) are ignored, like PricesFacet's keyed lookup.
  for (size_t i = 0; i < result.count; ++i) assert(strcmp(result.quotes[i].mint, kSol) != 0);
}

void test_non_object_documents_and_null_body() {
  const char* mints[] = {kUsdc};
  // Object.entries on arrays, strings, numbers and booleans yields no usable
  // row: answered with zero quotes, so the mint becomes a negative entry.
  const char* answered_empty[] = {"[]", "[{\"usdPrice\":1}]", "\"abc\"", "5", "true", "{}"};
  for (const char* json : answered_empty) {
    const JupiterPriceResult result = extract(json, mints, 1);
    assert(result.error == JupiterPriceError::kNone && result.count == 0);
  }
  // Object.entries(null) throws in the provider client: a failure, not an answer.
  const JupiterPriceResult null_body = extract("null", mints, 1);
  assert(null_body.error == JupiterPriceError::kNullBody && null_body.count == 0);
  const char* invalid[] = {"", "{", "{\"x\":1,}", "nul"};
  for (const char* json : invalid) {
    const JupiterPriceResult result = ExtractJupiterPriceV3(
        reinterpret_cast<const unsigned char*>(json), strlen(json), mints, 1,
        g_tokens, kBoundedJsonMaxTokens);
    assert(result.error == JupiterPriceError::kInvalidJson ||
           result.error == JupiterPriceError::kInvalidArguments);
  }
  // Non-finite JSON numbers never parse, so a price can never be infinite.
  static const char huge[] = "{\"EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v\":{\"usdPrice\":1e999}}";
  assert(extract(huge, mints, 1).error == JupiterPriceError::kInvalidJson);
}

void test_duplicates_case_and_request_bounds() {
  // JSON.parse keeps the last duplicate; a later row without usdPrice erases
  // the earlier price. Keys are case-sensitive base58.
  static const char duplicates[] =
      "{\"EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v\":{\"usdPrice\":1},"
      "\"epjfwdd5aufqssqem2qn1xzybapc8g4weggkzwytdt1v\":{\"usdPrice\":2},"
      "\"EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v\":{\"usdPrice\":3},"
      "\"So11111111111111111111111111111111111111112\":{\"usdPrice\":4},"
      "\"So11111111111111111111111111111111111111112\":{\"priceChange24h\":1}}";
  const char* mints[] = {kUsdc, kSol};
  JupiterPriceResult result = extract(duplicates, mints, 2);
  assert(result.error == JupiterPriceError::kNone && result.count == 1);
  assert(strcmp(result.quotes[0].mint, kUsdc) == 0 && result.quotes[0].price == 3);
  static const char escaped[] =
      "{\"EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1\\u0076\":{\"usdPrice\":7}}";
  result = extract(escaped, mints, 1);
  assert(result.error == JupiterPriceError::kNone && result.count == 1 &&
         result.quotes[0].price == 7);

  const char* invalid_mint[] = {"0OIl111111111111111111111111111111"};
  assert(extract("{}", invalid_mint, 1).error == JupiterPriceError::kInvalidArguments);
  const char* duplicate_request[] = {kUsdc, kUsdc};
  assert(extract("{}", duplicate_request, 2).error == JupiterPriceError::kInvalidArguments);

  // Exactly 50 mints parse; 51 exceed the provider request bound.
  for (size_t i = 0; i <= kJupiterPriceMaxMints; ++i) {
    memset(g_mints[i], 'a', 32);
    g_mints[i][32] = '\0';
    g_mints[i][0] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"[i % 58];
    g_mints[i][1] = i >= 58 ? 'b' : 'c';
    g_mint_pointers[i] = g_mints[i];
  }
  size_t used = static_cast<size_t>(snprintf(g_json, sizeof(g_json), "{"));
  for (size_t i = 0; i < kJupiterPriceMaxMints; ++i)
    used += static_cast<size_t>(snprintf(g_json + used, sizeof(g_json) - used,
        "%s\"%s\":{\"usdPrice\":%zu}", i ? "," : "", g_mints[i], i + 1));
  snprintf(g_json + used, sizeof(g_json) - used, "}");
  result = extract(g_json, g_mint_pointers, kJupiterPriceMaxMints);
  assert(result.error == JupiterPriceError::kNone && result.count == kJupiterPriceMaxMints);
  assert(result.quotes[49].price == 50);
  assert(extract(g_json, g_mint_pointers, kJupiterPriceMaxMints + 1).error ==
         JupiterPriceError::kInvalidArguments);
}

void test_realistic_256k_response() {
  static const char prefix[] =
      "{\"EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v\":{\"usdPrice\":1},\"padding\":\"";
  const size_t prefix_length = sizeof(prefix) - 1;
  const size_t length = kBoundedJsonMaxInputBytes;
  memcpy(g_large, prefix, prefix_length);
  memset(g_large + prefix_length, 'x', length - prefix_length - 2);
  g_large[length - 2] = '"';
  g_large[length - 1] = '}';
  const char* mints[] = {kUsdc};
  JupiterPriceResult result = ExtractJupiterPriceV3(g_large, length, mints, 1,
                                                    g_tokens, kBoundedJsonMaxTokens);
  assert(result.error == JupiterPriceError::kNone && result.count == 1);
  result = ExtractJupiterPriceV3(g_large, length + 1, mints, 1, g_tokens,
                                 kBoundedJsonMaxTokens);
  assert(result.error == JupiterPriceError::kResponseTooLarge);
}

void test_request_builder_lite_keyed_and_bounds() {
  NativeConfig config{};
  OutboundRequest request{};
  const char* mints[] = {kUsdc, kSol};
  // The CA bundle is mandatory: no request may fall back to unverified trust.
  assert(BuildJupiterPriceRequest(config, mints, 2, g_path, sizeof(g_path), &request) ==
         JupiterPriceRequestError::kInvalidConfig);
  config.ca_bundle_configured = true;
  snprintf(config.ca_bundle_path, sizeof(config.ca_bundle_path), "/data/ca-bundle.pem");
  assert(BuildJupiterPriceRequest(config, mints, 2, g_path, sizeof(g_path), &request) ==
         JupiterPriceRequestError::kNone);
  assert(strcmp(request.host, "lite-api.jup.ag") == 0 && strcmp(request.port, "443") == 0);
  assert(strcmp(g_path, "/price/v3?ids=EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"
                        "%2CSo11111111111111111111111111111111111111112") == 0);
  assert(request.path == g_path && request.header_count == 0 &&
         request.method == OutboundMethod::kGet && request.timeout_ms == 10000 &&
         request.max_response_bytes == kBoundedJsonMaxInputBytes &&
         strcmp(request.ca_file, "/data/ca-bundle.pem") == 0);

  config.jupiter_api_key_configured = true;
  snprintf(config.jupiter_api_key, sizeof(config.jupiter_api_key), "fixture-jupiter-key");
  assert(BuildJupiterPriceRequest(config, mints, 1, g_path, sizeof(g_path), &request) ==
         JupiterPriceRequestError::kNone);
  assert(strcmp(request.host, "api.jup.ag") == 0 && request.header_count == 1 &&
         strcmp(request.headers[0].name, "x-api-key") == 0 &&
         strcmp(request.headers[0].value, "fixture-jupiter-key") == 0);
  assert(strcmp(g_path, "/price/v3?ids=EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v") == 0);
  config.jupiter_api_key[0] = '\0';  // configured flag without a value is invalid
  assert(BuildJupiterPriceRequest(config, mints, 1, g_path, sizeof(g_path), &request) ==
         JupiterPriceRequestError::kInvalidConfig);
  config.jupiter_api_key_configured = false;

  const char* bad[] = {"EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v,x"};
  assert(BuildJupiterPriceRequest(config, bad, 1, g_path, sizeof(g_path), &request) ==
         JupiterPriceRequestError::kInvalidArguments);
  const char* short_mint[] = {"EPjFWdd5AufqSSqeM2qN1xzybapC8G4"};
  assert(BuildJupiterPriceRequest(config, short_mint, 1, g_path, sizeof(g_path), &request) ==
         JupiterPriceRequestError::kInvalidArguments);
  assert(BuildJupiterPriceRequest(config, mints, 0, g_path, sizeof(g_path), &request) ==
         JupiterPriceRequestError::kInvalidArguments);

  // Fifty maximum-length mints form one request of exactly the documented bound.
  for (size_t i = 0; i <= kJupiterPriceMaxMints; ++i) {
    memset(g_mints[i], 'z', kJupiterMintMaxBytes);
    g_mints[i][kJupiterMintMaxBytes] = '\0';
    g_mints[i][0] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"[i % 58];
    g_mint_pointers[i] = g_mints[i];
  }
  assert(BuildJupiterPriceRequest(config, g_mint_pointers, kJupiterPriceMaxMints, g_path,
                                  sizeof(g_path), &request) == JupiterPriceRequestError::kNone);
  assert(strlen(g_path) == kJupiterPriceMaxPathBytes && kJupiterPriceMaxPathBytes == 2361);
  assert(BuildJupiterPriceRequest(config, g_mint_pointers, kJupiterPriceMaxMints + 1, g_path,
                                  sizeof(g_path), &request) ==
         JupiterPriceRequestError::kInvalidArguments);
  assert(BuildJupiterPriceRequest(config, g_mint_pointers, kJupiterPriceMaxMints, g_path,
                                  kJupiterPriceMaxPathBytes, &request) ==
         JupiterPriceRequestError::kBufferTooSmall);
  assert(g_path[0] == '\0');
}
}  // namespace

int main() {
  test_contract_values_and_omitted_fields();
  test_non_object_documents_and_null_body();
  test_duplicates_case_and_request_bounds();
  test_realistic_256k_response();
  test_request_builder_lite_keyed_and_bounds();
  puts("Jupiter /price/v3 extraction and request construction tests passed");
  return 0;
}
