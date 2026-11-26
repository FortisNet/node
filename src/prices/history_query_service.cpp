#include "history_query_service.h"
#include "price_history_view.h"
#include "price_id_parser.h"
#include "ownership.h"
#include "threading.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
namespace fortis::prices {

namespace {
struct HistoryWait {
  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable done;
  bool finished = false;
  bool complete = false;
};
void history_done(void* context, const PriceHistoryOwnerResult& result) {
  auto* wait = static_cast<HistoryWait*>(context);
  fortis::base::LockGuard lock(wait->mutex);
  wait->complete = result.status == PriceHistoryOwnerResultStatus::kComplete ||
                   result.status == PriceHistoryOwnerResultStatus::kCachedComplete;
  wait->finished = true;
  wait->done.Signal();
}
uint64_t monotonic_ms() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1000000;
}
}  // namespace

bool HistoryQueryService::BackgroundHistory(const char* price_id, int64_t from_seconds, int64_t to_seconds,
                                            bool hourly) {
  if (!price_id || from_seconds >= to_seconds) return false;
  providers::ParsedPriceId parsed{};
  if (!ParsePriceId(price_id, strlen(price_id), &parsed)) return false;
  const int64_t width = hourly ? 3600 : 86400;
  const fortis::prices::PriceSeriesInterval interval =
      hourly ? fortis::prices::PriceSeriesInterval::k1h : fortis::prices::PriceSeriesInterval::k1d;
  const fortis::prices::history_batch::HistoryWindow window =
      fortis::prices::history_batch::MakeHistoryWindow(from_seconds, to_seconds, width);
  const size_t capacity = fortis::prices::PriceSeriesMaxRangeCandles(interval);
  if (window.expected > capacity) return false;
  // Borrow the typed view only while its byte owner is alive, including throws.
  base::Buffer candle_storage = base::Buffer::Allocate(sizeof(PriceSeriesCandle) * capacity);
  if (!candle_storage) return false;
  auto* candles = reinterpret_cast<PriceSeriesCandle*>(candle_storage.data());
  const size_t range_capacity = window.expected < capacity ? window.expected + 1 : capacity;
  auto read = [&](size_t* count) {
    return series_.Range(interval, "usd", parsed.source_id, from_seconds * 1000, to_seconds * 1000,
                               PriceHistoryCoverageNowMs(), candles, range_capacity, count);
  };
  size_t count = 0;
  auto status = read(&count);
  // CandleHistory.history's gap plan, CoinGecko ids only (the route's rule):
  // missing buckets minus covered ranges, each gap one backfill request.
  if (parsed.kind == providers::PriceIdKind::kCoinGecko &&
      (status == fortis::prices::PriceSeriesReadStatus::kFound || status == fortis::prices::PriceSeriesReadStatus::kMissing)) {
    uint8_t bitmap[fortis::prices::history_batch::kHistoryBitmapBytes]{};
    fortis::prices::history_batch::MarkHistoryBuckets(window, candles, count, bitmap);
    if (fortis::prices::history_batch::HistoryAnyBucketMissing(window, bitmap)) {
      fortis::prices::HistoryCoverage covered{};
      const bool has_coverage = series_.ReadHistoryCoverage(interval, "usd", parsed.source_id,
                                                                  PriceHistoryCoverageNowMs(), &covered) ==
                                fortis::prices::PriceSeriesReadStatus::kFound;
      int64_t gaps[fortis::prices::history_batch::kHistoryMaxGaps][2]{};
      const size_t gap_count =
          HistoryBackfillPlanner::Plan(window, bitmap, has_coverage ? &covered : nullptr, gaps);
      for (size_t g = 0; g < gap_count; ++g) {
        HistoryWait wait;
        uint64_t request_id = 0;
        const uint64_t deadline = monotonic_ms() + kPriceHistoryOwnerMaxDeadlineMs;
        if (backfill_.Submit(parsed.address, parsed.source_id, "usd", interval, gaps[g][0], gaps[g][1],
                                   deadline, history_done, &wait, &request_id) != PriceHistoryOwnerSubmitStatus::kAccepted)
          continue;
        fortis::base::UniqueLock lock(wait.mutex);
        while (!wait.finished) wait.done.WaitLocked(wait.mutex);
        lock.Unlock();


      }
      if (gap_count) status = read(&count);
    }
  }
  return status == fortis::prices::PriceSeriesReadStatus::kFound && count > 0;
}

namespace {
using fortis::prices::PriceSeriesReadStatus;

// Range() reads at most kPriceSeriesMaxRangeCandles per call. A token page
// may ask for 365 days of hourly candles (8,760 buckets), so the window is
// read in ascending chunks of 2,400 buckets each: every stored candle is
// bucket-aligned, so a chunk holds at most 2,400 rows and the per-call
// scratch stays at the history routes' bound.


bool series_read_failed(PriceSeriesReadStatus status, const char* source_id) {
  return status == PriceSeriesReadStatus::kIoError || status == PriceSeriesReadStatus::kCorrupt ||
         status == PriceSeriesReadStatus::kCapacityExceeded ||
         (status == PriceSeriesReadStatus::kInvalidKey &&
          fortis::prices::history_batch::HistorySourceIdStorable(source_id));
}

struct SeriesWait {
  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable done;
  bool finished = false;
  PriceHistoryOwnerResultStatus status = PriceHistoryOwnerResultStatus::kUpstreamFailure;
  ~SeriesWait() = default;
};

void series_done(void* context, const PriceHistoryOwnerResult& result) {
  auto* wait = static_cast<SeriesWait*>(context);
  fortis::base::LockGuard lock(wait->mutex);
  wait->status = result.status;
  wait->finished = true;
  wait->done.Signal();
}

timespec realtime_after_ms(uint32_t milliseconds) {
  timespec until{};
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += milliseconds / 1000;
  until.tv_nsec += static_cast<long>(milliseconds % 1000) * 1000000L;
  if (until.tv_nsec >= 1000000000L) {
    until.tv_sec += 1;
    until.tv_nsec -= 1000000000L;
  }
  return until;
}

// Waits for the one completion the owner delivers for every accepted
// request. The caller's cancellation is polled every 50 ms and forwarded to
// the owner; so is a deadline the owner has overrun by 5 s (a safety net:
// the owner itself answers kTimeout at the deadline). Either way the wait
// continues until the completion arrives, because the owner still holds
// `wait`. *caller_cancelled reports a forwarded caller cancellation.
PriceHistoryOwnerResultStatus wait_for_gap(HistoryGapSource& owner, uint64_t request_id,
                                           uint64_t owner_deadline_ms, const PriceHistorySeriesQuery& query,
                                           SeriesWait& wait, bool* caller_cancelled) {
  bool cancel_sent = false;
  fortis::base::UniqueLock lock(wait.mutex);
  while (!wait.finished) {
    const timespec until = realtime_after_ms(50);
    wait.done.WaitUntilLocked(wait.mutex, until);
    if (wait.finished || cancel_sent) continue;
    lock.Unlock();
    const bool cancelled = query.cancelled && query.cancelled(query.cancel_context);
    const bool overrun = monotonic_ms() > owner_deadline_ms + 5000;
    if (cancelled || overrun) {
      if (cancelled) *caller_cancelled = true;
      cancel_sent = true;
      owner.Cancel(request_id);  // false: the completion is already committed
    }
    lock.Lock();
  }
  const PriceHistoryOwnerResultStatus status = wait.status;
  return status;
}
}  // namespace

size_t HistoryQueryService::SeriesCapacity(int64_t from_seconds, int64_t to_seconds, bool hourly) {
  const fortis::prices::PriceSeriesInterval interval =
      hourly ? fortis::prices::PriceSeriesInterval::k1h : fortis::prices::PriceSeriesInterval::k1d;
  if (from_seconds < 0 || to_seconds <= from_seconds || to_seconds > INT64_MAX / 1000 ||
      to_seconds - from_seconds > PriceHistoryOwnerMaxWindowSeconds(interval))
    return 0;
  const fortis::prices::history_batch::HistoryWindow window =
      fortis::prices::history_batch::MakeHistoryWindow(from_seconds, to_seconds, hourly ? 3600 : 86400);
  // Every bucket of [first, last], the inclusive `to` bucket and one spare.
  return window.expected + 2;
}

PriceHistorySeriesStatus HistoryQueryService::ReadSeries(const PriceHistorySeriesQuery& query,
                                                         PriceHistorySeriesResult* result) {
  if (!result) return PriceHistorySeriesStatus::kUnavailable;
  *result = PriceHistorySeriesResult{};
  providers::ParsedPriceId parsed{};
  if (!query.price_id || !ParsePriceId(query.price_id, strlen(query.price_id), &parsed) ||
      parsed.kind == providers::PriceIdKind::kInvalid)
    return PriceHistorySeriesStatus::kNoSeries;
  const size_t capacity = SeriesCapacity(query.from_seconds, query.to_seconds, query.hourly);
  if (!capacity || !query.candles || query.capacity < capacity) return PriceHistorySeriesStatus::kUnavailable;
  const int64_t width = query.hourly ? 3600 : 86400;
  const fortis::prices::PriceSeriesInterval interval =
      query.hourly ? fortis::prices::PriceSeriesInterval::k1h : fortis::prices::PriceSeriesInterval::k1d;
  const fortis::prices::history_batch::HistoryWindow window =
      fortis::prices::history_batch::MakeHistoryWindow(query.from_seconds, query.to_seconds, width);
  const PriceHistoryRouteSeams* seams = seams_;
  auto read = [&](size_t* count) {
    if (seams && seams->fail_source_read && seams->fail_source_read(seams->context, parsed.source_id)) {
      *count = 0;
      return PriceSeriesReadStatus::kIoError;
    }
    const PriceSeriesReadStatus status = series_.ReadWindow(
        interval, parsed.source_id, query.from_seconds * 1000, query.to_seconds * 1000,
        width * 1000, PriceHistoryCoverageNowMs(), query.candles, capacity, count);
    if (status != PriceSeriesReadStatus::kFound) *count = 0;
    return status;
  };
  size_t count = 0;
  if (series_read_failed(read(&count), parsed.source_id)) return PriceHistorySeriesStatus::kUnavailable;
  result->has_timestamp = count > 0;
  result->timestamp_seconds = count ? query.candles[count - 1].ts / 1000 : 0;
  if (parsed.kind != providers::PriceIdKind::kCoinGecko) {
    // A provider-native id has no backfill source: its collected candles only.
    result->count = count;
    result->cached = count;
    return PriceHistorySeriesStatus::kOk;
  }
  // The bitmap holds every bucket of the window plus the inclusive `to` one
  // (1,096 bytes for 365 hourly days).
  base::Buffer initial_storage = base::Buffer::AllocateZeroed((window.expected + 8) / 8);
  if (!initial_storage) return PriceHistorySeriesStatus::kUnavailable;
  auto* initial = initial_storage.data();
  fortis::prices::history_batch::MarkHistoryBuckets(window, query.candles, count, initial);
  int64_t gaps[fortis::prices::history_batch::kHistoryMaxGaps][2]{};
  size_t gap_count = 0;
  if (fortis::prices::history_batch::HistoryAnyBucketMissing(window, initial)) {
    // A missing, expired, corrupt or unreadable record is uncovered.
    fortis::prices::HistoryCoverage covered{};
    const bool has_coverage = series_.ReadHistoryCoverage(interval, "usd", parsed.source_id,
                                                                PriceHistoryCoverageNowMs(), &covered) ==
                              PriceSeriesReadStatus::kFound;
    gap_count = HistoryBackfillPlanner::Plan(window, initial, has_coverage ? &covered : nullptr, gaps);
  }
  result->gaps_planned = gap_count;
  bool unavailable = false;
  for (size_t g = 0; g < gap_count && !unavailable; ++g) {
    const uint64_t now = monotonic_ms();
    if (now >= query.deadline_monotonic_ms) break;  // the route budget is spent
    uint64_t deadline = query.deadline_monotonic_ms;
    if (deadline - now > kPriceHistoryOwnerMaxDeadlineMs) deadline = now + kPriceHistoryOwnerMaxDeadlineMs;
    SeriesWait wait;
    uint64_t request_id = 0;
    const PriceHistoryOwnerSubmitStatus submitted =
        backfill_.Submit(parsed.address, parsed.source_id, "usd", interval, gaps[g][0], gaps[g][1], deadline,
                               series_done, &wait, &request_id);
    if (submitted == PriceHistoryOwnerSubmitStatus::kInvalidRequest) continue;  // skipped, like the GET route
    if (submitted == PriceHistoryOwnerSubmitStatus::kBusy) break;  // ends the plan: a failed backfill
    if (submitted != PriceHistoryOwnerSubmitStatus::kAccepted) { unavailable = true; break; }
    ++result->gaps_submitted;
    bool caller_cancelled = false;
    const PriceHistoryOwnerResultStatus status =
        wait_for_gap(backfill_, request_id, deadline, query, wait, &caller_cancelled);
    if (status == PriceHistoryOwnerResultStatus::kComplete) ++result->gaps_filled;
    // CandleHistory swallows every failed backfill; shutdown and a caller
    // cancellation have no TypeScript counterpart and answer unavailable.
    if (caller_cancelled || status == PriceHistoryOwnerResultStatus::kShutdown) unavailable = true;
  }
  if (!unavailable && gap_count && series_read_failed(read(&count), parsed.source_id)) unavailable = true;
  if (unavailable) {
    return PriceHistorySeriesStatus::kUnavailable;
  }
  result->count = count;
  result->hit = result->gaps_filled == 0;
  result->cached = fortis::prices::history_batch::CountInitiallyCached(window, initial, query.candles, count);
  return PriceHistorySeriesStatus::kOk;
}

}  // namespace fortis::prices
