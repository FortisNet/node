#pragma once

#include <memory>
#include "provider_runtime_config.h"
#include "ownership.h"
#include <stddef.h>
#include <stdint.h>

namespace fortis::base {
class Clock;
}  // namespace fortis::base
namespace fortis::prices {
class PriceSeriesStore;
enum class PriceSeriesInterval : uint8_t;
}  // namespace fortis::prices
namespace fortis::providers { class ProviderExecutionCoordinator; }

namespace fortis::prices {

constexpr size_t kPriceHistoryOwnerMaxRequests = 8;
// Orphaned active TLS jobs remain bounded in the adapter after all callers
// cancel. Keep enough flight entries for those jobs plus every queued caller.
constexpr size_t kPriceHistoryOwnerMaxFlights = 16;
// Each TypeScript gap stays one CoinGecko range request, so provider
// auto-granularity and daily volume match. Hourly windows keep the 365-day
// owner bound (the route already refuses hourly windows over 90 days). Daily
// windows allow TypeScript's 10 * 366 days plus one bucket, because a gap is
// aligned outward to [ceil(from), ceil(to)].
constexpr int64_t kPriceHistoryOwnerMaxHourlyWindowSeconds = 365 * 86400LL;
constexpr int64_t kPriceHistoryOwnerMaxDailyWindowSeconds = (10 * 366 + 1) * 86400LL;
int64_t PriceHistoryOwnerMaxWindowSeconds(fortis::prices::PriceSeriesInterval interval);
constexpr uint32_t kPriceHistoryOwnerMaxDeadlineMs = 30000;

enum class PriceHistoryOwnerSubmitStatus : uint8_t {
  kAccepted, kBusy, kInvalidRequest, kNotRunning, kStopping,
};
enum class PriceHistoryOwnerResultStatus : uint8_t {
  kComplete, kCachedComplete, kUpstreamFailure, kRateLimited,
  kInvalidResponse, kStorageFailure, kTimeout, kCancelled, kShutdown,
};
struct PriceHistoryOwnerResult {
  uint64_t request_id = 0;
  PriceHistoryOwnerResultStatus status = PriceHistoryOwnerResultStatus::kUpstreamFailure;
  int http_status = 0;
  uint32_t attempts = 0;
  uint32_t retry_after_ms = 0;
  size_t candle_rows = 0;
  size_t committed_chunks = 0;
  // kComplete only: false when every candle frame committed but a coverage
  // record write failed. CandleHistory swallows that kv.set failure and still
  // reports the fetch, so the result stays complete without any coverage.
  bool coverage_committed = false;
  // True when this caller was attached to a flight another caller had
  // already started. CandleHistory's lock loser waits for the winner and
  // reports nothing fetched itself, so the GET route keeps cache.hit true.
  bool joined = false;
};
using PriceHistoryOwnerCompletionFn = void (*)(void* context,
    const PriceHistoryOwnerResult& result);

// The wall clock the owner uses for candle and coverage expiry. Routes read
// coverage with it, so an installed clock applies to both sides.
int64_t PriceHistoryCoverageNowMs();
// Installs that clock for the process (nullptr: the system clock again).
// Call before any owner starts; the clock must outlive every owner and
// route. Production never calls it.
void SetPriceHistoryCoverageClock(fortis::base::Clock* clock);

// Asynchronous CoinGecko range owner. PriceHistoryService composes it for the
// history routes behind the price canary, wired through HttpResponseHandoff. All
// provider I/O runs on ProviderExecutionAdapter workers; callbacks and KV
// writes are serialized on this owner thread. Stop()
// from a callback only requests shutdown and returns; call it again from a
// lifecycle thread to join. Destruction must always happen off the owner thread.
//
// Identical (coin, source, fiat, from, to) requests share one flight whatever
// their interval. A flight is answered from coverage only when the window is
// covered for every waiter's interval (1h or 1d). After all candle frames
// commit, coverage is marked only for the intervals of waiters whose
// completion was committed, so a caller cancelled or timed out before commit
// never creates coverage.
//
// Lock order: callers may hold their own route locks when calling Submit or
// Cancel, which take only the owner mutex. The owner thread never holds the
// owner mutex while calling the series store, the provider coordinator or a
// completion callback, and the series store never calls back out.
class HistoryGapSource {
 public:
  virtual PriceHistoryOwnerSubmitStatus Submit(const char* coin_id, const char* source_id,
      const char* fiat, PriceSeriesInterval interval, int64_t from, int64_t to, uint64_t deadline,
      PriceHistoryOwnerCompletionFn completion, void* context, uint64_t* request_id) = 0;
  virtual bool Cancel(uint64_t request_id) = 0;
 protected:
  ~HistoryGapSource() = default;
};
class PriceHistoryBackfillOwner final : public HistoryGapSource {
 public:
  // When supplied, coordinator must already be started and must outlive this
  // owner. Stop drains and unregisters only this owner's coordinator lease;
  // it never stops the shared coordinator.
  PriceHistoryBackfillOwner(fortis::prices::PriceSeriesStore& series,
                            const config::ProviderRuntimeConfig& config,
                            providers::ProviderExecutionCoordinator* coordinator = nullptr);
  ~PriceHistoryBackfillOwner();
  PriceHistoryBackfillOwner(const PriceHistoryBackfillOwner&) = delete;
  PriceHistoryBackfillOwner& operator=(const PriceHistoryBackfillOwner&) = delete;

  bool Start();
  PriceHistoryOwnerSubmitStatus Submit(const char* coin_id,
      const char* source_id, const char* fiat,
      fortis::prices::PriceSeriesInterval interval, int64_t from,
      int64_t to, uint64_t deadline,
      PriceHistoryOwnerCompletionFn callback, void* context,
      uint64_t* request_id) override;
  bool Cancel(uint64_t request_id) override;
  void Stop();
  bool valid() const;

  // Start() with every provider request sent to one endpoint
  // (upstream_redirect.h). Production composes with Start().
  bool StartRedirected(const char* host, const char* port, const char* ca_file,
      bool (*allow_address)(void*, uint32_t), void* context);

 private:
  struct Impl;
  std::unique_ptr<Impl, fortis::base::ObjectDeleter<Impl>> impl_;
};

}  // namespace fortis::prices
