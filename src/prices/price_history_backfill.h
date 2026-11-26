#pragma once

#include "price_series_store.h"
#include "bounded_json.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::prices {

// One market_chart/range response is bounded by its largest window:
//  - a 90-day hourly window is about 2,161 hourly points;
//  - a ten-year daily window (TypeScript allows 10 * 366 days) is at most
//    3,661 midnight points plus a trailing sample.
// CandleHistory buckets the same feed into BOTH 1h and 1d rows, so a daily
// decade also yields one hourly row per point; both bucket arrays therefore
// need the decade bound. This is a per-response bound, not an API limit:
// the hourly route still refuses windows over 90 days.
// Scratch is heap/caller-owned: never put this workspace on a worker stack.
constexpr size_t kPriceHistoryMaxCandles = 200; // one atomic KV frame
constexpr size_t kPriceHistoryMaxPointCount = 3700;
constexpr size_t kPriceHistoryMaxHourlyCandles = kPriceHistoryMaxPointCount;
constexpr size_t kPriceHistoryMaxDailyCandles = kPriceHistoryMaxPointCount;
// {"prices":[[t,v],...],"market_caps":[...],"total_volumes":[...]}: one root,
// three keys and three arrays, then three tokens per point in each series.
constexpr size_t kPriceHistoryMaxJsonTokens = 9 * kPriceHistoryMaxPointCount + 108;
// Worst case for 3,662 points: 3 series x 3,662 x 41 bytes (13-digit
// millisecond key, a 24-character double, brackets and commas) is 450,426
// bytes. Larger provider bodies fail as kTooLarge before parsing.
constexpr size_t kPriceHistoryMaxProviderBodyBytes = 512 * 1024;

enum class PriceHistoryParseStatus : uint8_t {
  kOk, kInvalidJson, kInvalidShape, kInvalidPoint, kTooManyCandles,
};

struct PriceHistoryCandles {
  fortis::prices::PriceSeriesCandleBatchInput items[kPriceHistoryMaxCandles]{};
  size_t count = 0;
};

struct PriceHistoryBucket {
  fortis::prices::PriceSeriesCandleInput candle{};
};

struct PriceHistoryWorkspace {
  codec::BoundedJsonToken tokens[kPriceHistoryMaxJsonTokens]{};
  PriceHistoryBucket hourly[kPriceHistoryMaxHourlyCandles]{};
  PriceHistoryBucket daily[kPriceHistoryMaxDailyCandles]{};
  size_t hourly_count = 0;
  size_t daily_count = 0;
};

// 33,408 tokens x 24 bytes + 2 x 3,700 buckets x 56 bytes + two counts
// = 1,216,208 bytes on LP64 hosts.
static_assert(sizeof(PriceHistoryWorkspace) <= 1280 * 1024,
              "history parse workspace exceeds its 1.25 MiB memory budget");
static_assert(kPriceHistoryMaxProviderBodyBytes <= codec::kBoundedJsonMaxExpandedInputBytes,
              "history provider body exceeds the expanded JSON input ceiling");

class CandleAggregator {
 public:
  static PriceHistoryParseStatus Parse(const unsigned char* body, size_t body_length,
      PriceHistoryWorkspace* workspace);
  static PriceHistoryParseStatus Parse(const unsigned char* body, size_t body_length,
      codec::BoundedJsonToken* tokens, size_t token_capacity, PriceHistoryCandles* output);
  static size_t BuildChunk(const PriceHistoryWorkspace& workspace, size_t* hourly_cursor,
      size_t* daily_cursor, PriceSeriesCandleBatchInput output[kPriceHistoryMaxCandles]);
  static PriceSeriesStoreStatus Store(PriceSeriesStore& store, const char* fiat,
      const char* source_id, const PriceHistoryCandles& candles, int64_t now_ms);
};

// Accepts bodies up to kPriceHistoryMaxProviderBodyBytes. Over the token,
// point or bucket bounds the whole response fails (kInvalidJson or
// kTooManyCandles) before anything is stored; it never yields a partial or
// empty success.
PriceHistoryParseStatus ParseCoinGeckoMarketChartRange(
    const unsigned char* body, size_t body_length,
    PriceHistoryWorkspace* workspace);

// Produces a timestamp-ordered mixed-resolution batch with at most 200 rows.
// Cursors are in/out and advance only for rows copied into output.
size_t BuildPriceHistoryChunk(const PriceHistoryWorkspace& workspace,
    size_t* hourly_cursor, size_t* daily_cursor,
    fortis::prices::PriceSeriesCandleBatchInput output[kPriceHistoryMaxCandles]);

PriceHistoryParseStatus ParseCoinGeckoMarketChartRange(
    const unsigned char* body, size_t body_length,
    codec::BoundedJsonToken* tokens, size_t token_capacity,
    PriceHistoryCandles* output);

// Persist hourly and daily rows in a single durable KV frame. Empty valid
// histories are a successful no-op.
fortis::prices::PriceSeriesStoreStatus StorePriceHistoryCandles(
    fortis::prices::PriceSeriesStore& store, const char* fiat, const char* source_id,
    const PriceHistoryCandles& candles, int64_t now_ms);

}  // namespace fortis::prices
