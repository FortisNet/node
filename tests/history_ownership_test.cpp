#include "history_query_service.h"
#include "unit_test_support.h"

#include <new>

namespace {
namespace prices = fortis::prices;
using fortis::testing::Require;
enum class Failure { None, Range, Coverage, Submit };

class Repository final : public prices::PriceSeriesRepository {
 public:
  Failure failure = Failure::None;
  prices::PriceSeriesReadStatus Range(prices::PriceSeriesInterval, const char*, const char*,
      int64_t, int64_t, int64_t, prices::PriceSeriesCandle*, size_t, size_t* count) const override {
    if (failure == Failure::Range) throw std::bad_alloc();
    *count = 0;
    return prices::PriceSeriesReadStatus::kMissing;
  }
  prices::PriceSeriesReadStatus ReadHistoryCoverage(prices::PriceSeriesInterval,
      const char*, const char*, int64_t, prices::HistoryCoverage*) const override {
    if (failure == Failure::Coverage) throw std::bad_alloc();
    return prices::PriceSeriesReadStatus::kMissing;
  }
};

class Gaps final : public prices::HistoryGapSource {
 public:
  Failure failure = Failure::None;
  prices::PriceHistoryOwnerSubmitStatus Submit(const char*, const char*, const char*,
      prices::PriceSeriesInterval, int64_t, int64_t, uint64_t,
      prices::PriceHistoryOwnerCompletionFn, void*, uint64_t*) override {
    // Throw before accepting any callback: no borrowed stack context escapes.
    if (failure == Failure::Submit) throw std::bad_alloc();
    return prices::PriceHistoryOwnerSubmitStatus::kBusy;
  }
  bool Cancel(uint64_t) override { return false; }
};

void Exercise(Failure failure, bool background) {
  Repository repository;
  Gaps gaps;
  prices::HistoryQueryService service(repository, gaps);
  prices::PriceSeriesCandle candles[8]{};
  prices::PriceHistorySeriesQuery query{};
  query.price_id = "bitcoin";
  query.from_seconds = 0;
  query.to_seconds = 7200;
  query.hourly = true;
  query.deadline_monotonic_ms = UINT64_MAX;
  query.candles = candles;
  query.capacity = sizeof(candles) / sizeof(candles[0]);
  // Repeated exceptions expose lost allocations to LSan rather than allowing
  // a lone stale pointer in a dead stack frame to make a leak appear reachable.
  for (size_t attempt = 0; attempt < 32; ++attempt) {
    repository.failure = failure;
    gaps.failure = failure;
    bool caught = false;
    try {
      prices::PriceHistorySeriesResult result{};
      if (background) (void)service.BackgroundHistory("bitcoin", 0, 7200, true);
      else (void)service.ReadSeries(query, &result);
    } catch (const std::bad_alloc&) {
      caught = true;
    }
    Require(caught, "dependency exception reaches the caller");
    repository.failure = Failure::None;
    gaps.failure = Failure::None;
    prices::PriceHistorySeriesResult result{};
    Require(!service.BackgroundHistory("bitcoin", 0, 7200, true), "reuse after unwind: no cached candles");
    Require(service.ReadSeries(query, &result) == prices::PriceHistorySeriesStatus::kOk &&
        result.count == 0 && result.gaps_submitted == 0, "reuse after unwind: busy gap leaves an empty cached series");
  }
}
}  // namespace

int main() {
  try {
    Exercise(Failure::Range, true);
    Exercise(Failure::Coverage, true);
    Exercise(Failure::Submit, true);
    Exercise(Failure::Coverage, false);
    Exercise(Failure::Submit, false);
    puts("history ownership: 5 exception/reuse cases passed (32 iterations each)");
    return 0;
  } catch (const std::exception& error) {
    fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
  }
}
