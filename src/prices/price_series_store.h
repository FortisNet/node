#pragma once

#include "price_history_coverage.h"
#include "price_spot_cache.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::prices {

class PriceSpotCache;

constexpr size_t kPriceSeriesMaxBatchItems = 100;
constexpr size_t kPriceSeriesEncodedBytes = 84;
constexpr size_t kPriceSeriesMaxUpsertItems = 200;
constexpr size_t kPriceSeriesMaxRangeCandles = 2500;
constexpr size_t kPriceProviderBatchMaxMutations =
    kPriceSeriesMaxBatchItems * 7 + 2 * kPriceSpotMaxBatchItems;

enum class PriceSeriesStoreStatus : uint8_t {
  kStored,
  kNoItems,
  kInvalidArguments,
  kAllocationFailure,
  kReadFailure,
  kWriteFailure,
};

enum class PriceSeriesReadStatus : uint8_t {
  kFound,
  kMissing,
  kCorrupt,
  kInvalidKey,
  kIoError,
  kCapacityExceeded,
};
enum class PriceSeriesInterval : uint8_t { k5m, k1h, k1d };

// Range() capacity per interval. 5m and 1h keep 2,500 (a 90-day hourly
// window is 2,161 buckets). A ten-year daily window (TypeScript allows
// 10 * 366 days) is at most 3,660 buckets plus the inclusive `to` bucket.
constexpr size_t kPriceSeriesMaxDailyRangeCandles = 3700;
constexpr size_t PriceSeriesMaxRangeCandles(PriceSeriesInterval interval) {
  return interval == PriceSeriesInterval::k1d ? kPriceSeriesMaxDailyRangeCandles
                                              : kPriceSeriesMaxRangeCandles;
}
static_assert(10 * 366 + 1 <= kPriceSeriesMaxDailyRangeCandles,
              "daily range capacity must hold the TypeScript ten-year window");

struct PriceSeriesSpot {
  char source[kPriceSpotMaxProviderBytes + 1]{};
  double price = 0;
  double change24h_pct = 0;
  double volume24h = 0;
  bool has_change24h_pct = false;
  bool has_volume24h = false;
  int64_t as_of_ms = 0;
};

struct PriceSeriesCandle {
  int64_t ts = 0;
  double open = 0;
  double high = 0;
  double low = 0;
  double close = 0;
  double volume = 0;
  bool has_volume = false;
  int64_t first_at_ms = 0;
  int64_t last_at_ms = 0;
};

struct PriceSeriesCandleInput {
  int64_t ts = 0;
  double open = 0;
  double high = 0;
  double low = 0;
  double close = 0;
  double volume = 0;
  bool has_volume = false;
};

struct PriceSeriesCandleBatchInput {
  PriceSeriesInterval interval = PriceSeriesInterval::k1h;
  PriceSeriesCandleInput candle{};
};

// One durable spot observation of RecentSpots (PriceSeriesStore.recentSpots).
struct PriceSeriesRecentSpot {
  char fiat[6]{};
  char source_id[kPriceSpotMaxSourceIdBytes + 1]{};
  int64_t as_of_ms = 0;
  double price = 0;
};
constexpr size_t kPriceSeriesMaxRecentSpots = 1000;

struct PriceSeriesAvailability {
  int64_t oldest = 0;
  int64_t newest = 0;
};

// Bounded durable spot + per-bucket open 5m/1h/1d candles and packed closed
// segments (hour/week/month). Fiat and source ID are lookup inputs; the stored
// spot source is returned in PriceSeriesSpot. Quote batches merge up to 100
// unique IDs in one KV frame. Sealing uses a fixed work budget per frame.
class PriceSeriesStore {
 public:
  explicit PriceSeriesStore(storage::KvStore& store);
  ~PriceSeriesStore() = default;
  PriceSeriesStore(const PriceSeriesStore&) = delete;
  PriceSeriesStore& operator=(const PriceSeriesStore&) = delete;

  PriceSeriesStoreStatus RecordSpots(const char* fiat, const char* provider,
      const PriceSpotQuoteInput* quotes, size_t quote_count, int64_t now_ms);
  PriceSeriesReadStatus GetSpot(const char* fiat, const char* source_id,
      int64_t now_ms, PriceSeriesSpot* output) const;
  PriceSeriesReadStatus Range(PriceSeriesInterval interval, const char* fiat,
      const char* source_id, int64_t from_ms, int64_t to_ms, int64_t now_ms,
      PriceSeriesCandle* output, size_t capacity, size_t* count) const;
  PriceSeriesStoreStatus UpsertCandles(PriceSeriesInterval interval,
      const char* fiat, const char* id,
      const PriceSeriesCandleInput* inputs, size_t count, int64_t now);
  // Atomically writes candle rows from multiple resolutions in one durable KV
  // frame. The total row count shares the same fixed 200-row bound.
  PriceSeriesStoreStatus UpsertCandlesAtomic(const char* fiat,
      const char* id, const PriceSeriesCandleBatchInput* inputs,
      size_t count, int64_t now);
  // Provider-history coverage: one bounded record of merged half-open second
  // ranges per (1h|1d interval, fiat, source ID); see price_history_coverage.h.
  // Read reports kMissing for an absent or expired record and kCorrupt for
  // one that fails validation; callers treat both as uncovered. Mark merges
  // [from_s, to_s) into the stored record and rewrites it, with a fresh
  // six-hour TTL, in one KV frame under the series lock. A corrupt or expired
  // record is replaced; a read I/O error writes nothing. Nothing is cached in
  // memory, so a failed write cannot leave coverage that is not on disk.
  PriceSeriesReadStatus ReadHistoryCoverage(PriceSeriesInterval interval,
      const char* fiat, const char* source_id, int64_t now_ms,
      HistoryCoverage* output) const;
  PriceSeriesStoreStatus MarkHistoryCoverageRange(PriceSeriesInterval interval,
      const char* fiat, const char* source_id, int64_t from_s, int64_t to_s,
      int64_t now_ms);
  PriceSeriesStoreStatus SealDue(int64_t now_ms, size_t limit, size_t* sealed);
  // recentSpots(since, limit): the first `capacity` durable spot rows whose
  // as-of is at or after `since_ms`, oldest first; equal instants order by
  // encodeURIComponent(fiat) + '/' + encodeURIComponent(sourceId), the
  // TypeScript `price/recent/` key order. Native keeps no recent index (one
  // spot record per fiat and source ID already holds its latest as-of, the
  // set TypeScript's index holds), so this is one scan of the spot records
  // in pages of 256 with the series lock released between pages, keeping a
  // bounded ordered selection. Corrupt rows are skipped and counted.
  PriceSeriesReadStatus RecentSpots(int64_t since_ms, int64_t now_ms,
      PriceSeriesRecentSpot* output, size_t capacity, size_t* count,
      size_t* scanned = nullptr, size_t* corrupt = nullptr) const;
  PriceSeriesReadStatus Availability(PriceSeriesInterval interval,
      const char* fiat, const char* source_id, int64_t now_ms,
      PriceSeriesAvailability* output) const;

 private:
  friend class PriceSpotCache;
  PriceSeriesStoreStatus RecordSpotsWithAdditional(
      const char* fiat, const char* provider,
      const PriceSpotQuoteInput* quotes, size_t quote_count, int64_t now_ms,
      const storage::KvMutation* additional, size_t additional_count);
  storage::KvStore& store_;
};

}  // namespace fortis::prices
