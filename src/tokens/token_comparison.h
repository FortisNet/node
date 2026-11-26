#pragma once

#include <stddef.h>
#include <stdint.h>

#include "price_history_batch_codec.h"
#include "token_ref.h"

// Pure port of the TypeScript compareTokenRecords and of the TypeScript
// TokenComparisonResponse encoders (JSON and protobuf). No I/O, no allocation;
// guest-safe. Frozen in tests/corpus/token_comparison_route.json.
namespace fortis::tokens::comparison {

constexpr size_t kMaxTokens = 20;
// Native bound on every stored string (symbol, name, media key, ids,
// sources). TypeScript has none; the native store refuses longer writes.
constexpr size_t kMaxStoredString = 256;
// Market rows stay fresh 15 minutes, liquidity rows 30 minutes
// (DEFAULT_COMPARISON_FRESHNESS).
constexpr int64_t kMarketFreshMs = 15 * 60000;
constexpr int64_t kLiquidityFreshMs = 30 * 60000;
// Date.prototype.toISOString range the native store accepts: 1970..9999.
constexpr int64_t kMaxDateMs = 253402300799999LL;

struct OptString {
  bool present = false;
  size_t length = 0;
  char text[kMaxStoredString + 1]{};
};
struct OptNumber {
  bool present = false;
  double value = 0;
};
struct OptDate {
  bool present = false;
  int64_t ms = 0;
};

enum class LiquidityStatus : uint8_t { kNone, kOk, kNotFound, kUnsupported, kError };

// TokenComparisonRecord after the repository read and the identity
// resolver's metadata merge.
struct Record {
  char network[networks::kTokenNetworkMaxGenericIdBytes + 1]{};
  char token_id[networks::kTokenRefMaxIdBytes + 1]{};
  OptString symbol, name, logo_media_key, market_source_id;
  OptNumber market_price_usd, price_change_1h_pct, price_change_24h_pct,
      price_change_7d_pct, price_change_30d_pct, market_cap_usd, volume_24h_usd;
  OptDate market_as_of;
  OptString market_source;
  OptNumber dex_price_usd, dex_market_cap_usd, dex_volume_24h_usd, liquidity_usd;
  OptDate liquidity_as_of, liquidity_checked_at;
  OptString liquidity_source;
  LiquidityStatus liquidity_status = LiquidityStatus::kNone;
  OptNumber spread_pct, top_pool_share, pool_count;
};

enum class MetricStatus : uint8_t { kAvailable, kStale, kUnavailable, kUnsupported, kError };

struct Metric {
  OptNumber value;
  OptDate as_of;
  const OptString* source = nullptr;  // into the record; null or !present = null
  MetricStatus status = MetricStatus::kUnavailable;
};

struct Item {
  const Record* record = nullptr;
  Metric price, market_cap, volume, liquidity;
  OptNumber turnover_pct, liquidity_to_market_cap_pct, volume_to_liquidity_ratio;
  uint32_t rank_market_cap = 0, rank_volume = 0, rank_liquidity = 0;  // 0 = null
};

// compareTokenRecords(records, now): items[i] describes records[i].
void CompareTokenRecords(const Record* records, size_t count, int64_t now_ms, Item* items);

struct ErrorEntry {
  uint32_t input_index = 0;
  const char* network = nullptr;
  size_t network_length = 0;
  const char* token_id = nullptr;
  size_t token_id_length = 0;
  bool duplicate = false;  // DUPLICATE_TOKEN, else INVALID_TOKEN
};

struct Response {
  int64_t computed_at_ms = 0;
  const Item* items = nullptr;
  size_t item_count = 0;
  const ErrorEntry* errors = nullptr;
  size_t error_count = 0;
};

// JSON.stringify(result) (fiat is always "usd"). With include_computed_at
// false this is the TypeScript validator representation `{fiat,items,errors}`.
void AppendResponseJson(fortis::codec::history_batch::OutBuffer& out, const Response& response,
                        bool include_computed_at);
// toBinary(TokenComparisonResponseSchema, comparisonMessage(result)); with
// include_computed_at false, the validator message (computedAt ''). False
// where protobuf-es throws: a pool count that is not a uint32.
bool AppendResponseProto(fortis::codec::history_batch::OutBuffer& out, const Response& response,
                         bool include_computed_at);

// Date.prototype.toISOString for 0 <= ms <= kMaxDateMs.
bool IsoTimestamp(int64_t ms, char output[32]);

}  // namespace fortis::tokens::comparison
