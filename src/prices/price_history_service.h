#pragma once

#include <memory>
#include "status.h"

#include "provider_runtime_config.h"
#include "ownership.h"
#include "price_history_route.h"

namespace fortis::storage { class KvStore; }
namespace fortis::prices { struct PriceSeriesCandle; }
namespace fortis::providers { class ProviderExecutionCoordinator; }

namespace fortis::prices {

// One CandleHistory.history(id, 'usd', from, to, interval) read for an
// in-process caller (the token page `series` section). The caller owns the
// candle storage: PriceHistoryService::SeriesCapacity(query) entries.
struct PriceHistorySeriesQuery {
  const char* price_id = nullptr;
  int64_t from_seconds = 0;
  int64_t to_seconds = 0;
  bool hourly = false;
  // Monotonic route deadline. Every backfill gap is submitted with it (capped
  // at the owner's 30 s), and none starts once it has passed.
  uint64_t deadline_monotonic_ms = 0;
  // Polled about every 50 ms while a gap is pending. True cancels the gap in
  // the owner, waits for its completion and answers kUnavailable.
  bool (*cancelled)(void* context) = nullptr;
  void* cancel_context = nullptr;
  fortis::prices::PriceSeriesCandle* candles = nullptr;
  size_t capacity = 0;
};
enum class PriceHistorySeriesStatus : uint8_t {
  kOk,           // a (possibly empty) series
  kNoSeries,     // parsePriceId(id) is null: TypeScript's `history` null
  kUnavailable,  // a failed store read, shutdown or cancellation (a throw)
};
struct PriceHistorySeriesResult {
  size_t count = 0;
  bool hit = true;             // cache.hit: no gap was filled
  bool has_timestamp = false;  // cache.timestamp: the newest initially cached point
  int64_t timestamp_seconds = 0;
  size_t cached = 0;           // coverage.cachedPoints
  size_t gaps_planned = 0;
  size_t gaps_submitted = 0;
  size_t gaps_filled = 0;      // kComplete
};

// Starts both composed owners. Normal startup passes none and always uses
// PriceHistoryBackfillOwner::Start() and OhlcProviderOwner::Start():
// production CoinGecko hosts, the configured CA bundle and key, and the
// public-address policy. Only a binary built with
// FORTIS_HISTORY_BACKFILL_TEST_HOOKS can supply a target-substituting start.
// It may leave either owner started when it fails; the service stops both.
class PriceHistoryOwnersStarter {
 public:
  // True when both owners started; the base starts nothing and fails.
  virtual bool Start(PriceHistoryBackfillOwner& backfill, OhlcProviderOwner& ohlc) {
    (void)backfill; (void)ohlc;
    return false;
  }

 protected:
  constexpr PriceHistoryOwnersStarter() = default;
  ~PriceHistoryOwnersStarter() = default;
};

// The native price history composition: GET /v1/prices/history, its batch
// POST and GET /v1/prices/ohlc over one PriceSeriesStore on the runtime KV,
// with a PriceHistoryBackfillOwner and an OhlcProviderOwner both leased on the
// process ProviderExecutionCoordinator, plus the batch driver thread. Nothing
// is allocated and no thread starts before Create, so a disabled route costs
// no owner memory, threads or KV work. The batch driver is process-global, so
// at most one service may be started.
//
// Lifecycle operations are serialized by the caller. Create once with a
// started coordinator, before the listener binds; Register once the response
// handoff is running. Call Stop after HTTP workers have joined and before the
// coordinator and handoff stop: it stops the batch driver, which answers
// waiting batches, then the OHLC and backfill owners, which answer pending
// requests through the still-running handoff and release their leases. Stop
// is idempotent, and destruction stops a started service.
struct PriceHistoryServiceDeps {
  storage::KvStore& kv;
  providers::ProviderExecutionCoordinator& coordinator;
};
struct PriceHistoryServiceOptions {
  PriceHistoryOwnersStarter* start_owners = nullptr;
  const PriceHistoryRouteSeams* seams = nullptr;
};

// Factory-created resource handle. Moved-from handles are destruction-only.
class PriceHistoryService {
 public:
  PriceHistoryService(PriceHistoryService&&) noexcept;
  PriceHistoryService& operator=(PriceHistoryService&&) = delete;
  ~PriceHistoryService();
  PriceHistoryService(const PriceHistoryService&) = delete;
  PriceHistoryService& operator=(const PriceHistoryService&) = delete;

  // A failure leaves nothing allocated or started (the coordinator keeps
  // running); its message names the part that failed ("backfill owner",
  // "OHLC owner", "batch driver", ...). `seams` must outlive the service;
  // normal startup passes none.
  static base::Result<PriceHistoryService> Create(const config::ProviderRuntimeConfig& config, PriceHistoryServiceDeps deps,
                                      const PriceHistoryServiceOptions& options = {});
  // Registers all three routes or returns false; on false the caller must
  // not serve with this router.
  bool Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff);
  void Stop();
  // The TypeScript PricesFacet.history(id, 'usd', from, to, interval,
  // {foreground: false}) for the operation candle repair job. A CoinGecko
  // id's window is filled (or answered from coverage) by the backfill owner;
  // the result is whether the series then holds any candle in [from, to].
  // Other price ids answer false: native history covers CoinGecko ids only.
  // Blocks the calling (job) thread up to the owner deadline.
  bool BackgroundHistory(const char* price_id, int64_t from_seconds, int64_t to_seconds, bool hourly);

  // Final gate: CandleHistory.history() for TokenPageFacet.series, on the
  // calling (token worker) thread over this service's store and backfill
  // owner, the same ones the history routes use. Stored candles are read in
  // bounded chunks; for a CoinGecko id the missing buckets minus coverage
  // (more than eight collapse into the aligned window) are filled one gap at
  // a time through the backfill owner, then the window is read again. Every
  // other id answers its stored candles only. A failed backfill (provider
  // error, owner busy, timeout) is swallowed: the cached points answer with
  // cache.hit true. Not started, a store read failure, shutdown and
  // cancellation answer kUnavailable.
  PriceHistorySeriesStatus ReadSeries(const PriceHistorySeriesQuery& query, PriceHistorySeriesResult* result);
  // Candle storage ReadSeries needs for the query window (0 when the window
  // is empty, inverted or wider than the backfill owner accepts).
  static size_t SeriesCapacity(int64_t from_seconds, int64_t to_seconds, bool hourly);

 private:
  struct Impl;
  explicit PriceHistoryService(Impl* impl);
  // Defined beside Impl: only that unit can destroy one.
  struct ImplDeleter { void operator()(Impl* impl) const; };
  std::unique_ptr<Impl, ImplDeleter> impl_;
};

}  // namespace fortis::prices
