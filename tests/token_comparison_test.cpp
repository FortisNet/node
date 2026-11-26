#include "test_support.h"
// Unit checks for the pure comparison projection (src/tokens/token_comparison.cpp)
// and the native read model (src/tokens/token_comparison_store.cpp). The full
// TypeScript differential replay is tests/token_comparison_route_test.py;
// the expectations here restate TypeScript comparison rules that the
// replay exercises only indirectly.
#include "kv_store.h"
#include "token_comparison.h"
#include "token_comparison_store.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

using namespace fortis::storage;
using namespace fortis::tokens;

using namespace fortis;
using namespace fortis::tokens::comparison;
using fortis::codec::history_batch::OutBuffer;

namespace {

int failures = 0;
#define CHECK(condition) do { if (!(condition)) { ++failures; \
  fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); } } while (0)

constexpr int64_t kNow = 1767323045000LL;  // 2026-01-02T03:04:05.000Z

OptNumber num(double value) { OptNumber n; n.present = true; n.value = value; return n; }
OptDate at(int64_t ms) { OptDate d; d.present = true; d.ms = ms; return d; }
OptString str(const char* text) {
  OptString s; s.present = true; s.length = strlen(text); memcpy(s.text, text, s.length + 1);
  return s;
}

Record base(const char* token) {
  Record r;
  snprintf(r.network, sizeof(r.network), "eth-mainnet");
  snprintf(r.token_id, sizeof(r.token_id), "%s", token);
  return r;
}

void projection() {
  static Record records[4];
  static Item items[4];
  // Freshness edges: market fresh at exactly 15 minutes, stale one ms later.
  records[0] = base("a");
  records[0].market_price_usd = num(2);
  records[0].market_as_of = at(kNow - kMarketFreshMs);
  records[0].market_source = str("coingecko");
  records[0].market_cap_usd = num(100);
  // Stale market loses to fresh DEX.
  records[1] = base("b");
  records[1].market_price_usd = num(3);
  records[1].market_as_of = at(kNow - kMarketFreshMs - 1);
  records[1].dex_price_usd = num(4);
  records[1].liquidity_as_of = at(kNow - kLiquidityFreshMs);
  records[1].liquidity_source = str("dexscreener");
  records[1].market_cap_usd = num(100);  // tie with records[0]
  // A future observation is never fresh.
  records[2] = base("c");
  records[2].market_price_usd = num(5);
  records[2].market_as_of = at(kNow + 1);
  records[2].liquidity_usd = num(10);
  records[2].liquidity_status = LiquidityStatus::kNotFound;
  records[2].liquidity_as_of = at(kNow);
  // Ratios: -0 rounding, overflow to Infinity (JSON null), negative denominator.
  records[3] = base("d");
  records[3].market_cap_usd = num(1e-300);
  records[3].market_as_of = at(kNow);
  records[3].volume_24h_usd = num(1e300);
  records[3].liquidity_usd = num(-5);
  records[3].liquidity_status = LiquidityStatus::kOk;
  records[3].liquidity_as_of = at(kNow);
  CompareTokenRecords(records, 4, kNow, items);
  CHECK(items[0].price.status == MetricStatus::kAvailable);
  CHECK(items[1].price.value.value == 4 && items[1].price.source == &records[1].liquidity_source);
  CHECK(items[1].price.status == MetricStatus::kAvailable);
  CHECK(items[2].price.status == MetricStatus::kStale);
  CHECK(items[2].liquidity.status == MetricStatus::kStale);
  CHECK(items[0].rank_market_cap == 1 && items[1].rank_market_cap == 1);
  CHECK(items[3].rank_market_cap == 3);
  CHECK(items[3].turnover_pct.present && isinf(items[3].turnover_pct.value));
  CHECK(!items[3].volume_to_liquidity_ratio.present);
  CHECK(items[3].rank_liquidity == 2 && items[2].rank_liquidity == 1);

  static uint8_t buffer[65536];
  OutBuffer json{buffer, sizeof(buffer) - 1};
  Response response{};
  response.computed_at_ms = kNow;
  response.items = items; response.item_count = 4;
  AppendResponseJson(json, response, true);
  buffer[json.used] = 0;
  CHECK(json.ok);
  CHECK(strstr(reinterpret_cast<char*>(buffer), "\"turnoverPct\":null") != nullptr);
  CHECK(strstr(reinterpret_cast<char*>(buffer), "\"computedAt\":\"2026-01-02T03:04:05.000Z\"") != nullptr);
  OutBuffer validator{buffer, sizeof(buffer) - 1};
  AppendResponseJson(validator, response, false);
  buffer[validator.used] = 0;
  CHECK(strstr(reinterpret_cast<char*>(buffer), "computedAt") == nullptr);

  // Math.round(-0.3e-6 * 1e6) is -0; JSON writes 0, protobuf keeps the sign.
  static Record negative[1];
  static Item negative_items[1];
  negative[0] = base("e");
  negative[0].volume_24h_usd = num(-0.3e-8);
  negative[0].market_cap_usd = num(1);
  negative[0].market_as_of = at(kNow);
  CompareTokenRecords(negative, 1, kNow, negative_items);
  CHECK(negative_items[0].turnover_pct.present && negative_items[0].turnover_pct.value == 0 &&
        signbit(negative_items[0].turnover_pct.value));

  // protobuf-es refuses a non-uint32 pool count.
  static Record pool[1];
  static Item pool_items[1];
  pool[0] = base("f");
  pool[0].pool_count = num(2.5);
  CompareTokenRecords(pool, 1, kNow, pool_items);
  Response pool_response{};
  pool_response.computed_at_ms = kNow;
  pool_response.items = pool_items; pool_response.item_count = 1;
  OutBuffer proto{buffer, sizeof(buffer)};
  CHECK(!AppendResponseProto(proto, pool_response, true));
  pool[0].pool_count = num(4294967295.0);
  OutBuffer proto_ok{buffer, sizeof(buffer)};
  CHECK(AppendResponseProto(proto_ok, pool_response, true));

  char iso[32];
  CHECK(IsoTimestamp(0, iso) && strcmp(iso, "1970-01-01T00:00:00.000Z") == 0);
  CHECK(IsoTimestamp(kMaxDateMs, iso) && strcmp(iso, "9999-12-31T23:59:59.999Z") == 0);
  CHECK(!IsoTimestamp(-1, iso) && !IsoTimestamp(kMaxDateMs + 1, iso));
}

void store() {
  char directory[] = "/tmp/fortis-token-store-XXXXXX";
  CHECK(mkdtemp(directory) != nullptr);
  static KvStore kv;
  CHECK(kv.Open(directory));
  TokenComparisonStore store(&kv);
  token_store::IdentityPatch identity;
  identity.symbol = {token_store::FieldState::kValue, "usdc", 4};
  identity.coingecko_id = {token_store::FieldState::kValue, "usd-coin", 8};
  CHECK(store.PatchIdentity("eth-mainnet", "erc20:0xabc", identity, kNow));
  static token_store::ComparisonPatch patch;
  patch = token_store::ComparisonPatch{};
  patch.fiat = {token_store::FieldState::kValue, "usd", 3};
  patch.market_price_usd = {token_store::FieldState::kValue, 1.5};
  patch.market_as_of = {token_store::FieldState::kValue, kNow};
  patch.pool_count = {token_store::FieldState::kValue, 7};
  patch.liquidity_status = {token_store::FieldState::kValue, LiquidityStatus::kError};
  CHECK(store.PatchComparison("eth-mainnet", "erc20:0xabc", patch, kNow));
  // A later patch keeps absent fields, removes nulls, drops non-finite values.
  patch = token_store::ComparisonPatch{};
  patch.pool_count = {token_store::FieldState::kNull, 0};
  patch.market_cap_usd = {token_store::FieldState::kValue, INFINITY};
  patch.volume_24h_usd = {token_store::FieldState::kValue, 3};
  CHECK(store.PatchComparison("eth-mainnet", "erc20:0xabc", patch, kNow));
  // Invalid writes are refused without changing the document.
  patch = token_store::ComparisonPatch{};
  patch.market_as_of = {token_store::FieldState::kValue, -1};
  patch.volume_24h_usd = {token_store::FieldState::kValue, 99};
  CHECK(!store.PatchComparison("eth-mainnet", "erc20:0xabc", patch, kNow));
  static char long_text[300];
  memset(long_text, 'x', sizeof(long_text));
  identity = token_store::IdentityPatch{};
  identity.name = {token_store::FieldState::kValue, long_text, sizeof(long_text)};
  CHECK(!store.PatchIdentity("eth-mainnet", "erc20:0xabc", identity, kNow));

  static Record records[3];
  token_store::Key keys[3] = {{"eth-mainnet", "erc20:0xabc"}, {"eth-mainnet", "missing"},
                              {"eth mainnet/x", "a%b"}};
  CHECK(store.Read(keys, 3, "usd", kNow, records) == token_store::ReadStatus::kOk);
  CHECK(records[0].symbol.present && strcmp(records[0].symbol.text, "USDC") == 0);
  CHECK(records[0].market_source_id.present &&
        strcmp(records[0].market_source_id.text, "usd-coin") == 0);
  CHECK(records[0].market_price_usd.present && records[0].market_price_usd.value == 1.5);
  CHECK(!records[0].pool_count.present && !records[0].market_cap_usd.present);
  CHECK(records[0].volume_24h_usd.present && records[0].volume_24h_usd.value == 3);
  CHECK(records[0].liquidity_status == LiquidityStatus::kError);
  CHECK(records[0].market_as_of.present && records[0].market_as_of.ms == kNow);
  CHECK(!records[1].symbol.present && !records[1].market_price_usd.present);
  CHECK(strcmp(records[2].network, "eth mainnet/x") == 0);
  // String.prototype.toUpperCase on read: ß becomes SS; a symbol whose
  // upper-case form outgrows the bound is refused at write time.
  identity = token_store::IdentityPatch{};
  identity.symbol = {token_store::FieldState::kValue, "stra\xc3\x9f""e", 7};
  CHECK(store.PatchIdentity("eth-mainnet", "sharp", identity, kNow));
  token_store::Key sharp[1] = {{"eth-mainnet", "sharp"}};
  CHECK(store.Read(sharp, 1, "usd", kNow, records) == token_store::ReadStatus::kOk);
  CHECK(records[0].symbol.present && strcmp(records[0].symbol.text, "STRASSE") == 0);
  static char eszett[256];
  for (size_t i = 0; i < sizeof(eszett); i += 2) { eszett[i] = '\xc3'; eszett[i + 1] = '\x9f'; }
  identity.symbol = {token_store::FieldState::kValue, eszett, sizeof(eszett)};
  CHECK(store.PatchIdentity("eth-mainnet", "sharp", identity, kNow));  // 256 bytes -> 256
  static char eszett_plus[256];
  memcpy(eszett_plus, eszett, sizeof(eszett_plus));
  eszett_plus[0] = '\xc5'; eszett_plus[1] = '\x89';  // U+0149 -> two code points, 3 bytes
  identity.symbol = {token_store::FieldState::kValue, eszett_plus, sizeof(eszett_plus)};
  CHECK(!store.PatchIdentity("eth-mainnet", "sharp", identity, kNow));
  // Another fiat's metrics are not read; identity still is.
  CHECK(store.Read(keys, 1, "eur", kNow, records) == token_store::ReadStatus::kOk);
  CHECK(!records[0].market_price_usd.present && records[0].symbol.present);
  // Keys are encodeURIComponent'd like TypeScript.
  char value[64];
  size_t size = 0;
  identity = token_store::IdentityPatch{};
  identity.name = {token_store::FieldState::kValue, "n", 1};
  CHECK(store.PatchIdentity("eth mainnet/x", "a%b", identity, kNow));
  CHECK(kv.Get("token/registry/eth%20mainnet%2Fx/a%25b", kNow, value, sizeof(value), &size));
  // A corrupt document is a read failure, never an empty row.
  const char garbage[] = "FTS1\x01\x09";
  const KvMutation corrupt{"token/comparison/eth-mainnet/bad", garbage, sizeof(garbage) - 1, 0,
                           false};
  CHECK(kv.Write(&corrupt, 1));
  token_store::Key bad[1] = {{"eth-mainnet", "bad"}};
  CHECK(store.Read(bad, 1, "usd", kNow, records) == token_store::ReadStatus::kCorrupt);
  kv.Close();
  fortis::test::RemoveTree(directory);
}

}  // namespace

int main() {
  projection();
  store();
  if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
  printf("token_comparison_test: ok\n");
  return 0;
}
