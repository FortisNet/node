#include "price_series_repository.h"
namespace fortis::prices {
namespace { constexpr int64_t kSeriesChunkBuckets = 2400; }
PriceSeriesReadStatus PriceSeriesRepository::ReadWindow(fortis::prices::PriceSeriesInterval interval, const char* source_id,
                                          int64_t from_ms, int64_t to_ms, int64_t width_ms, int64_t now_ms,
                                          fortis::prices::PriceSeriesCandle* out, size_t capacity, size_t* count) const {
  *count = 0;
  bool found = false;
  for (int64_t start = from_ms; start <= to_ms;) {
    const int64_t span = kSeriesChunkBuckets * width_ms;
    const int64_t end = to_ms - start < span ? to_ms : start + span - 1;
    fortis::prices::PriceSeriesCandle overflow_probe{};
    const size_t remaining = capacity - *count;
    fortis::prices::PriceSeriesCandle* target = remaining ? out + *count : &overflow_probe;
    size_t chunk_capacity = static_cast<size_t>(kSeriesChunkBuckets) + 1;
    if (!remaining) chunk_capacity = 1;
    else if (chunk_capacity > remaining) chunk_capacity = remaining;
    size_t got = 0;
    const PriceSeriesReadStatus status =
        Range(interval, "usd", source_id, start, end, now_ms, target, chunk_capacity, &got);
    if (status == PriceSeriesReadStatus::kFound) {
      if (!remaining && got) return PriceSeriesReadStatus::kCapacityExceeded;
      *count += got;
      found = found || got > 0;
    } else if (status != PriceSeriesReadStatus::kMissing) {
      *count = 0;
      return status;
    }
    if (end == to_ms) break;
    start = end + 1;
  }
  return found ? PriceSeriesReadStatus::kFound : PriceSeriesReadStatus::kMissing;
}
PriceSeriesReadStatus StoredPriceSeriesRepository::Range(PriceSeriesInterval interval, const char* fiat,
    const char* id, int64_t from, int64_t to, int64_t now, PriceSeriesCandle* out,
    size_t capacity, size_t* count) const { return store_.Range(interval, fiat, id, from, to, now, out, capacity, count); }
PriceSeriesReadStatus StoredPriceSeriesRepository::ReadHistoryCoverage(PriceSeriesInterval interval,
    const char* fiat, const char* id, int64_t now, HistoryCoverage* out) const {
  return store_.ReadHistoryCoverage(interval, fiat, id, now, out);
}
}  // namespace fortis::prices
