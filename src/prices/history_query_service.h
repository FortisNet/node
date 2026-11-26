#pragma once
#include "price_history_service.h"
#include "price_series_repository.h"
namespace fortis::prices {
class HistoryQueryService {
 public:
  HistoryQueryService(PriceSeriesRepository& series, HistoryGapSource& backfill,
                       const PriceHistoryRouteSeams* seams = nullptr)
      : series_(series), backfill_(backfill), seams_(seams) {}
  bool BackgroundHistory(const char* price_id, int64_t from, int64_t to, bool hourly);
  PriceHistorySeriesStatus ReadSeries(const PriceHistorySeriesQuery& query, PriceHistorySeriesResult* result);
  static size_t SeriesCapacity(int64_t from, int64_t to, bool hourly);
 private:
  PriceSeriesRepository& series_;
  HistoryGapSource& backfill_;
  const PriceHistoryRouteSeams* seams_;
};
}  // namespace fortis::prices
