#pragma once
#include "price_series_store.h"
namespace fortis::prices {
// Borrowed read capabilities; concrete storage and transaction ownership stay
// in PriceSeriesStore. Wallet callers need no coverage/write/lifecycle API.
class PriceSeriesReader {
 public:
  virtual PriceSeriesReadStatus Range(PriceSeriesInterval interval, const char* fiat,
      const char* source_id, int64_t from, int64_t to, int64_t now,
      PriceSeriesCandle* output, size_t capacity, size_t* count) const = 0;
 protected:
  ~PriceSeriesReader() = default;
};
class PriceSeriesRepository : public PriceSeriesReader {
 public:
  virtual PriceSeriesReadStatus ReadHistoryCoverage(PriceSeriesInterval interval,
      const char* fiat, const char* source_id, int64_t now, HistoryCoverage* output) const = 0;
  // Validated window, positive width and caller-owned bounded output.
  PriceSeriesReadStatus ReadWindow(PriceSeriesInterval interval, const char* source_id,
      int64_t from, int64_t to, int64_t width, int64_t now,
      PriceSeriesCandle* output, size_t capacity, size_t* count) const;
 protected:
  ~PriceSeriesRepository() = default;
};
class StoredPriceSeriesRepository final : public PriceSeriesRepository {
 public:
  explicit StoredPriceSeriesRepository(const PriceSeriesStore& store) : store_(store) {}
  PriceSeriesReadStatus Range(PriceSeriesInterval interval, const char* fiat,
      const char* source_id, int64_t from, int64_t to, int64_t now,
      PriceSeriesCandle* output, size_t capacity, size_t* count) const override;
  PriceSeriesReadStatus ReadHistoryCoverage(PriceSeriesInterval interval,
      const char* fiat, const char* source_id, int64_t now, HistoryCoverage* output) const override;
 private:
  const PriceSeriesStore& store_;
};
}  // namespace fortis::prices
