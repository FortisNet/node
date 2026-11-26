#include "coingecko_price.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

using namespace fortis::codec;
using namespace fortis::providers;

namespace {
BoundedJsonToken g_tokens[kBoundedJsonMaxTokens];
uint32_t g_key_index[kBoundedJsonMaxTokens];
unsigned char g_large[kBoundedJsonMaxInputBytes + 1];
char g_many_ids[100][kCoinGeckoIdCapacity];
const char* g_id_pointers[100];
char g_many_json[32768];
BoundedJsonToken g_small_tokens[64];

bool parse(const char* json, BoundedJsonDocument* document,
           BoundedJsonError* error = nullptr) {
  BoundedJsonError local = BoundedJsonError::kNone;
  return document->Parse(reinterpret_cast<const unsigned char*>(json), strlen(json),
                         g_small_tokens, 64, error ? error : &local);
}

void test_reader_values_and_unicode() {
  static const char json[] =
      "{\"s\":\"escaped\\n\\u03bb\\uD83D\\uDE80\","
      "\"array\":[true,false,null,-0,1.25e2]}";
  BoundedJsonDocument document;
  assert(parse(json, &document));
  const uint32_t string = document.FindObjectValue(document.root(), "s");
  char decoded[32]{}; size_t decoded_length = 0;
  assert(document.DecodeString(string, decoded, sizeof(decoded), &decoded_length));
  static const char expected[] = "escaped\n\xce\xbb\xf0\x9f\x9a\x80";
  assert(decoded_length == sizeof(expected) - 1 &&
         memcmp(decoded, expected, sizeof(expected) - 1) == 0);
  const uint32_t array = document.FindObjectValue(document.root(), "array");
  const BoundedJsonToken* array_token = document.token(array);
  assert(array_token && array_token->type == BoundedJsonType::kArray);
  uint32_t item = array_token->first_child;
  assert(document.token(item)->type == BoundedJsonType::kTrue);
  item = document.token(item)->next_sibling;
  assert(document.token(item)->type == BoundedJsonType::kFalse);
  item = document.token(item)->next_sibling;
  assert(document.token(item)->type == BoundedJsonType::kNull);
  item = document.token(item)->next_sibling;
  double number = 10;
  assert(document.Number(item, &number) && number == 0 && signbit(number));
  item = document.token(item)->next_sibling;
  assert(document.Number(item, &number) && number == 125.0);
}

void test_malformed_and_finite_numbers() {
  const char* const invalid[] = {
      "", "{}x", "{", "{\"x\":}", "{\"x\":01}", "{\"x\":1,}",
      "[true false]", "{\"x\":\"\\q\"}",
      "{\"x\":1e999}", "{\"x\":1e}", "{\"x\":-}",
  };
  for (const char* json : invalid) {
    BoundedJsonDocument document;
    BoundedJsonError error = BoundedJsonError::kNone;
    assert(!parse(json, &document, &error));
    assert(error == BoundedJsonError::kInvalidInput);
  }
  const unsigned char bad_utf8[] = {'"', 0xc0, 0xaf, '"'};
  BoundedJsonDocument document;
  BoundedJsonError error = BoundedJsonError::kNone;
  assert(!document.Parse(bad_utf8, sizeof(bad_utf8), g_small_tokens, 64, &error));
  assert(error == BoundedJsonError::kInvalidInput);
}

void test_depth_token_and_input_caps() {
  char deep[64]{};
  size_t used = 0;
  for (size_t i = 0; i < kBoundedJsonMaxDepth + 1; ++i) deep[used++] = '[';
  deep[used++] = '0';
  for (size_t i = 0; i < kBoundedJsonMaxDepth + 1; ++i) deep[used++] = ']';
  BoundedJsonDocument document;
  BoundedJsonError error = BoundedJsonError::kNone;
  assert(!document.Parse(reinterpret_cast<const unsigned char*>(deep), used,
                         g_tokens, kBoundedJsonMaxTokens, &error));
  assert(error == BoundedJsonError::kDepthLimit);

  static char many_tokens[kBoundedJsonMaxTokens * 2 + 8];
  size_t position = 0;
  many_tokens[position++] = '[';
  for (size_t i = 0; i < kBoundedJsonMaxTokens; ++i) {
    if (i) many_tokens[position++] = ',';
    many_tokens[position++] = '0';
  }
  many_tokens[position++] = ']';
  assert(!document.Parse(reinterpret_cast<const unsigned char*>(many_tokens), position,
                         g_tokens, kBoundedJsonMaxTokens, &error));
  assert(error == BoundedJsonError::kTokenLimit);

  memset(g_large, 'x', sizeof(g_large));
  assert(!document.Parse(g_large, sizeof(g_large), g_tokens,
                         kBoundedJsonMaxTokens, &error));
  assert(error == BoundedJsonError::kInputTooLarge);
}

void test_simple_price_contract_values() {
  // The numeric values match coldJson.prices in the frozen
  // tests/corpus/price_spot_route.json contract.
  static const unsigned char json[] =
      "{\"ETHEREUM\":{\"usd\":3012.44,\"usd_24h_change\":-1.2},"
      "\"Bitcoin\":{\"usd\":97000.5,\"usd_24h_change\":0.8,\"usd_24h_vol\":5,\"last_updated_at\":1700000000},"
      "\"doge\":{\"usd\":null},\"wrapped\":{\"usd\":1,"
      "\"usd_24h_change\":false,\"usd_24h_vol\":\"bad\"},"
      "\"not-requested\":{\"usd\":99}}";
  const char* ids[] = {"ethereum", "bitcoin", "doge", "wrapped"};
  const CoinGeckoSpotResult result = ExtractCoinGeckoSimplePrice(
      json, sizeof(json) - 1, ids, 4, "usd", g_tokens, kBoundedJsonMaxTokens,
      g_key_index, kBoundedJsonMaxTokens);
  assert(result.error == CoinGeckoSpotError::kNone && result.count == 3);
  assert(strcmp(result.quotes[0].id, "ethereum") == 0 &&
         result.quotes[0].price == 3012.44 && result.quotes[0].hasChange24hPct &&
         result.quotes[0].change24hPct == -1.2 && !result.quotes[0].hasVolume24h);
  assert(strcmp(result.quotes[1].id, "bitcoin") == 0 &&
         result.quotes[1].price == 97000.5 && result.quotes[1].hasChange24hPct &&
         result.quotes[1].change24hPct == 0.8 && result.quotes[1].hasVolume24h &&
         result.quotes[1].volume24h == 5);
  assert(result.quotes[1].provider_observed_at_ms == 1700000000000LL);
  assert(result.quotes[0].provider_observed_at_ms == 0);
  assert(strcmp(result.quotes[2].id, "wrapped") == 0 &&
         result.quotes[2].price == 1 && !result.quotes[2].hasChange24hPct &&
         !result.quotes[2].hasVolume24h);
}

void test_escaped_keys_duplicates_and_request_cap() {
  static const unsigned char escaped[] = "{\"bit\\u0063oin\":{\"usd\":4}}";
  const char* bitcoin[] = {"bitcoin"};
  CoinGeckoSpotResult result = ExtractCoinGeckoSimplePrice(
      escaped, sizeof(escaped) - 1, bitcoin, 1, "usd", g_tokens,
      kBoundedJsonMaxTokens, g_key_index, kBoundedJsonMaxTokens);
  assert(result.error == CoinGeckoSpotError::kNone && result.count == 1 &&
         result.quotes[0].price == 4);
  // JSON.parse keeps the exact duplicate at its first insertion position,
  // then Object.entries feeds differently-cased keys to Map in that order.
  static const unsigned char duplicates[] =
      "{\"bitcoin\":{\"usd\":1},\"BITCOIN\":{\"usd\":2},"
      "\"bitcoin\":{\"usd\":3}}";
  const char* ids[] = {"BITCOIN", "bitcoin"};
  result = ExtractCoinGeckoSimplePrice(
      duplicates, sizeof(duplicates) - 1, ids, 2, "usd", g_tokens,
      kBoundedJsonMaxTokens, g_key_index, kBoundedJsonMaxTokens);
  assert(result.error == CoinGeckoSpotError::kNone && result.count == 1 &&
         strcmp(result.quotes[0].id, "bitcoin") == 0 && result.quotes[0].price == 2);

  size_t used = 0;
  used += static_cast<size_t>(snprintf(g_many_json + used, sizeof(g_many_json) - used, "{"));
  for (size_t i = 0; i < 100; ++i) {
    snprintf(g_many_ids[i], sizeof(g_many_ids[i]), "coin-%zu", i);
    g_id_pointers[i] = g_many_ids[i];
    used += static_cast<size_t>(snprintf(g_many_json + used, sizeof(g_many_json) - used,
        "%s\"%s\":{\"usd\":%zu}", i ? "," : "", g_many_ids[i], i + 1));
  }
  used += static_cast<size_t>(snprintf(g_many_json + used, sizeof(g_many_json) - used, "}"));
  result = ExtractCoinGeckoSimplePrice(
      reinterpret_cast<const unsigned char*>(g_many_json), used, g_id_pointers, 100,
      "usd", g_tokens, kBoundedJsonMaxTokens, g_key_index, kBoundedJsonMaxTokens);
  assert(result.error == CoinGeckoSpotError::kNone && result.count == 100);
  assert(strcmp(result.quotes[0].id, "coin-0") == 0 && result.quotes[0].price == 1);
  assert(strcmp(result.quotes[99].id, "coin-99") == 0 && result.quotes[99].price == 100);
  result = ExtractCoinGeckoSimplePrice(
      reinterpret_cast<const unsigned char*>(g_many_json), used, g_id_pointers, 101,
      "usd", g_tokens, kBoundedJsonMaxTokens, g_key_index, kBoundedJsonMaxTokens);
  assert(result.error == CoinGeckoSpotError::kInvalidArguments);
}

void test_realistic_256k_response() {
  static const char prefix[] = "{\"bitcoin\":{\"usd\":1},\"padding\":\"";
  const size_t prefix_length = sizeof(prefix) - 1;
  const size_t length = kBoundedJsonMaxInputBytes;
  assert(prefix_length + 2 < length);
  memcpy(g_large, prefix, prefix_length);
  memset(g_large + prefix_length, 'x', length - prefix_length - 2);
  g_large[length - 2] = '"';
  g_large[length - 1] = '}';
  const char* ids[] = {"bitcoin"};
  const CoinGeckoSpotResult result = ExtractCoinGeckoSimplePrice(
      g_large, length, ids, 1, "usd", g_tokens, kBoundedJsonMaxTokens,
      g_key_index, kBoundedJsonMaxTokens);
  assert(result.error == CoinGeckoSpotError::kNone && result.count == 1 &&
         result.quotes[0].price == 1);
}
}  // namespace

int main() {
  test_reader_values_and_unicode();
  test_malformed_and_finite_numbers();
  test_depth_token_and_input_caps();
  test_simple_price_contract_values();
  test_escaped_keys_duplicates_and_request_cap();
  test_realistic_256k_response();
  puts("bounded JSON and CoinGecko spot extraction tests passed");
  return 0;
}
