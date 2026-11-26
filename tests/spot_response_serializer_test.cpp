#include "test_support.h"
#include <string_view>
#include "spot_response_serializer.h"

#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <locale.h>
#include <string>

using namespace fortis::prices;

namespace {
std::string_view view(const char* text) { return {text, strlen(text)}; }
std::string hex(const uint8_t* data, size_t size) {
  static const char digits[] = "0123456789abcdef";
  std::string out(size * 2, '0');
  for (size_t i = 0; i < size; ++i) {
    out[i * 2] = digits[data[i] >> 4]; out[i * 2 + 1] = digits[data[i] & 15];
  }
  return out;
}
}

int main() try {
  constexpr int64_t kTime = 1767323045000LL;  // 2026-01-02T03:04:05.000Z
  SpotPriceInput prices[2]{};
  prices[0].raw_key = view("ethereum"); prices[0].source_id = view("ethereum");
  prices[0].price = 3012.44; prices[0].has_change_24h_pct = true;
  prices[0].change_24h_pct = -1.2; prices[0].as_of_ms = kTime;
  prices[0].source = view("coingecko");
  prices[1].raw_key = view("bitcoin"); prices[1].source_id = view("bitcoin");
  prices[1].price = 97000.5; prices[1].has_change_24h_pct = true;
  prices[1].change_24h_pct = 0.8; prices[1].as_of_ms = kTime;
  prices[1].source = view("coingecko");
  SpotResponseInput input{view("usd"), prices, 2, nullptr, 0, nullptr, 0};

  char json[2048]; size_t json_size = 0;
  assert(SerializeSpotResponseJson(input, json, sizeof(json), &json_size) == SpotSerializeResult::kOk);
  const std::string expected_json =
      "{\"fiat\":\"usd\",\"prices\":{\"ethereum\":{\"sourceId\":\"ethereum\",\"price\":3012.44,\"change24hPct\":-1.2,\"asOf\":\"2026-01-02T03:04:05.000Z\",\"stale\":false,\"source\":\"coingecko\",\"cache\":{\"hit\":false,\"timestamp\":null}},\"bitcoin\":{\"sourceId\":\"bitcoin\",\"price\":97000.5,\"change24hPct\":0.8,\"asOf\":\"2026-01-02T03:04:05.000Z\",\"stale\":false,\"source\":\"coingecko\",\"cache\":{\"hit\":false,\"timestamp\":null}}},\"missing\":[],\"invalid\":[]}";
  assert(json_size == expected_json.size());
  assert(std::string(json, json_size) == expected_json);

  uint8_t protobuf[2048]; size_t protobuf_size = 0;
  assert(SerializeSpotResponseProtobuf(input, protobuf, sizeof(protobuf), &protobuf_size) == SpotSerializeResult::kOk);
  const std::string expected_wire = "0a03757364124f0a08657468657265756d12430a08657468657265756d117b14ae47e188a74019333333333333f3bf2218323032362d30312d30325430333a30343a30352e3030305a3209636f696e6765636b6f3a00124d0a07626974636f696e12420a07626974636f696e110000000088aef740199a9999999999e93f2218323032362d30312d30325430333a30343a30352e3030305a3209636f696e6765636b6f3a00";
  assert(hex(protobuf, protobuf_size) == expected_wire);

  // Proto3 omits the non-optional zero price while preserving explicit
  // presence for optional change24hPct=0; JSON retains the null/value field.
  SpotPriceInput zero = prices[0]; zero.raw_key = view("zero"); zero.source_id = view("zero");
  zero.price = 0; zero.has_change_24h_pct = true; zero.change_24h_pct = 0;
  input = {view("usd"), &zero, 1, nullptr, 0, nullptr, 0};
  assert(SerializeSpotResponseProtobuf(input, protobuf, sizeof(protobuf), &protobuf_size) == SpotSerializeResult::kOk);
  const std::string zero_wire = hex(protobuf, protobuf_size);
  assert(zero_wire == "0a03757364123e0a047a65726f12360a047a65726f1900000000000000002218323032362d30312d30325430333a30343a30352e3030305a3209636f696e6765636b6f3a00");
  zero.price = -0.0;
  assert(SerializeSpotResponseProtobuf(input, protobuf, sizeof(protobuf), &protobuf_size) == SpotSerializeResult::kOk);
  assert(protobuf_size == zero_wire.size() / 2 + 9);
  assert(hex(protobuf, protobuf_size).find("110000000000000080") != std::string::npos);
  zero.has_change_24h_pct = false;
  assert(SerializeSpotResponseJson(input, json, sizeof(json), &json_size) == SpotSerializeResult::kOk);
  assert(std::string(json, json_size).find("\"change24hPct\":null") != std::string::npos);

  // JSON.stringify number formatting thresholds and round-trip extremes.
  const double numbers[] = {-0.0, 1e-7, 1e-6, 1e20, 1e21, 0.1,
                            1.2345678901234567, DBL_TRUE_MIN, DBL_MAX};
  const char* number_text[] = {"0", "1e-7", "0.000001", "100000000000000000000",
                               "1e+21", "0.1", "1.2345678901234567", "5e-324",
                               "1.7976931348623157e+308"};
  for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); ++i) {
    zero.price = numbers[i];
    input = {view("usd"), &zero, 1, nullptr, 0, nullptr, 0};
    assert(SerializeSpotResponseJson(input, json, sizeof(json), &json_size) == SpotSerializeResult::kOk);
    char field[96]; snprintf(field, sizeof(field), "\"price\":%s", number_text[i]);
    assert(std::string(json, json_size).find(field) != std::string::npos);
  }
  if (setlocale(LC_NUMERIC, "de_DE.UTF-8") != nullptr) {
    zero.price = 1.25;
    assert(SerializeSpotResponseJson(input, json, sizeof(json), &json_size) == SpotSerializeResult::kOk);
    assert(std::string(json, json_size).find("\"price\":1.25") != std::string::npos);
    assert(std::string(json, json_size).find("\"price\":1,25") == std::string::npos);
    setlocale(LC_NUMERIC, "C");
  }
  // Raw aliases remain separate map entries, cached/stale metadata is kept,
  // and optional values are present even when their numeric value is zero.
  SpotPriceInput alias[2]{prices[0], prices[0]};
  alias[0].raw_key = view("ethereum");
  alias[1].raw_key = view("ethereum:mainnet"); alias[1].cache_hit = true;
  alias[1].has_cache_timestamp = true; alias[1].cache_timestamp_ms = kTime;
  alias[1].stale = true; alias[1].has_change_24h_pct = true; alias[1].change_24h_pct = 0;
  std::string_view missing[] = {view("unknown-token")};
  std::string_view invalid[] = {view("bad/id")};
  input = {view("usd"), alias, 2, missing, 1, invalid, 1};
  assert(SerializeSpotResponseJson(input, json, sizeof(json), &json_size) == SpotSerializeResult::kOk);
  const std::string alias_json(json, json_size);
  assert(alias_json.find("\"ethereum:mainnet\"") != std::string::npos);
  assert(alias_json.find("\"stale\":true") != std::string::npos);
  assert(alias_json.find("\"timestamp\":\"2026-01-02T03:04:05.000Z\"") != std::string::npos);
  assert(alias_json.find("\"missing\":[\"unknown-token\"]") != std::string::npos);
  assert(alias_json.find("\"invalid\":[\"bad/id\"]") != std::string::npos);
  assert(SerializeSpotResponseProtobuf(input, protobuf, sizeof(protobuf), &protobuf_size) == SpotSerializeResult::kOk);
  const std::string alias_wire = hex(protobuf, protobuf_size);
  assert(alias_wire.find("657468657265756d3a6d61696e6e6574") != std::string::npos);
  assert(alias_wire.find("2801") != std::string::npos);  // stale=true
  assert(alias_wire.find("3a1c08011218323032362d30312d30325430333a30343a30352e3030305a") != std::string::npos);
  assert(alias_wire.find("1a0d756e6b6e6f776e2d746f6b656e") != std::string::npos);
  assert(alias_wire.find("22066261642f6964") != std::string::npos);

  // JS object key enumeration: canonical array-index keys are numeric sorted.
  SpotPriceInput numeric[3]{prices[0], prices[1], prices[0]};
  numeric[0].raw_key = view("bitcoin"); numeric[1].raw_key = view("10");
  numeric[2].raw_key = view("2");
  input = {view("usd"), numeric, 3, nullptr, 0, nullptr, 0};
  assert(SerializeSpotResponseJson(input, json, sizeof(json), &json_size) == SpotSerializeResult::kOk);
  const std::string numeric_json(json, json_size);
  assert(numeric_json.find("\"prices\":{\"bitcoin\":") != std::string::npos);
  assert(numeric_json.find("\"bitcoin\":") < numeric_json.find("\"10\":") &&
         numeric_json.find("\"10\":") < numeric_json.find("\"2\":"));

  // Duplicate object keys are rejected instead of producing ambiguous JSON
  // and duplicate protobuf map entries.
  numeric[1].raw_key = view("2");
  char untouched[16]; memset(untouched, 0x5a, sizeof(untouched)); size_t unchanged = 123;
  assert(SerializeSpotResponseJson(input, untouched, sizeof(untouched), &unchanged) == SpotSerializeResult::kInvalidInput);
  assert(untouched[0] == 0x5a && unchanged == 123);

  input = {view("usd"), prices, 1, nullptr, 0, nullptr, 0};
  char short_json[8]; memset(short_json, 0x33, sizeof(short_json));
  assert(SerializeSpotResponseJson(input, short_json, sizeof(short_json), nullptr) == SpotSerializeResult::kOutputTooSmall);
  assert(short_json[0] == 0x33);
  prices[0].price = NAN;
  assert(SerializeSpotResponseJson(input, json, sizeof(json), nullptr) == SpotSerializeResult::kNonFinite);
  prices[0].price = 1;
  const char invalid_utf8[] = {static_cast<char>(0xc0), static_cast<char>(0x80)};
  prices[0].source = {invalid_utf8, sizeof(invalid_utf8)};
  assert(SerializeSpotResponseProtobuf(input, protobuf, sizeof(protobuf), nullptr) == SpotSerializeResult::kInvalidUtf8);
  // Views need no terminator and never serialize their backing suffix.
  const char bounded_id[] = {'b','i','t','c','o','i','n'};
  SpotPriceInput bounded_price{{bounded_id, sizeof(bounded_id)}, view("bitcoin"),
      1.0, false, 0, 0, false, view("coingecko"), false, false, 0};
  input = {view("usd"), &bounded_price, 1, nullptr, 0, nullptr, 0};
  assert(SerializeSpotResponseJson(input, json, sizeof(json), &json_size) == SpotSerializeResult::kOk);
  assert(std::string(json, json_size).find("\"bitcoin\":") != std::string::npos);

  puts("spot response serializer: ok");
} catch (...) {
  return fortis::test::ReportException();
}
