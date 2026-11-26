#pragma once

#include "clock.h"
#include "coin_popularity.h"
#include "seams.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::providers { class TokenProviderOwner; }
namespace fortis::storage { class KvStore; }
namespace fortis::prices { class PriceSeriesStore; class PriceSpotCache; }

// The TypeScript coin price scheduler job and its part of
// the daily retention job, over the native token provider owner:
//  - Run(): popularity.due(2,500); the due coins are refreshed through
//    /coins/markets (TokenDiscoveryFacet.refreshMarkets) at FOREGROUND lane
//    priority, as TypeScript does. Each answered batch stores every coin's
//    `mkt:usd:<id>` entry (fresh for round(interval·1.5) jittered, stale
//    15 min; an unlisted coin stores {"market":null}) and primes the `px:`
//    spot row and price series for rows with a finite positive
//    current_price, fresh for the same jittered margin. Only the coins of
//    answered batches are marked refreshed; a failed, shed or unparseable
//    batch keeps its coins due. Then the dirty counters are saved;
//  - Restore(): the durable ranking at boot, without provider calls;
//  - Prune(): popularity.prune() plus one bounded KV sweep (256 rows), as
//    KvCoinPopularityStore.prune does.
//
// Native differences:
//  - a batch holds at most kMarketBatchIds (80) ids, and as many as fit the
//    4 KiB request path, instead of TypeScript's 250: a 7-day sparkline row
//    is ~4.3–5.1 KiB and the provider body bound is 512 KiB. A run makes at
//    most kMaxCallsPerRun (10) provider calls, the TypeScript run's ceiling
//    (2,500 / 250), so the provider call rate is unchanged while one run
//    refreshes at most 800 coins; due coins past that stay due and lead the
//    next run;
//  - on stop the batch loop ends early, and the answered batches are still
//    marked and the dirty counters saved.
namespace fortis::prices::coin_prices {

constexpr int64_t kIntervalMs = 60000;       // TOKEN_WORKLOADS.coinPrices.intervalMs
constexpr size_t kBudgetPerRun = 2500;       // TOKEN_WORKLOADS.coinPrices.budgetPerRun
constexpr size_t kTypeScriptMarketBatchIds = 250;
constexpr size_t kMarketBatchIds = 80;
constexpr size_t kMaxCallsPerRun = 10;
constexpr int64_t kMarketStaleMs = 15 * 60000;  // MARKET_STALE_MS
constexpr int64_t kPruneIntervalMs = 86400000;  // daily retention
constexpr size_t kPruneSweepLimit = 256;

// round(interval · FRESH_MARGIN): the freshness a refreshed entry gets.
int64_t FreshMsFor(int64_t interval_ms);

struct Deps {
  storage::KvStore* kv = nullptr;
  PriceSeriesStore* series = nullptr;
  PriceSpotCache* spot_cache = nullptr;
  providers::TokenProviderOwner* owner = nullptr;
  discovery::CoinPopularity* popularity = nullptr;
  // The cache clock and jitter (DiscoveryCache seams); null = wall clock and
  // CSPRNG jitter.
  base::Clock* clock = nullptr;
  base::JitterSource* jitter = nullptr;
  bool (*stop)(void* context) = nullptr;
  void* stop_context = nullptr;
  bool pinned_core = false;  // opt-in snapshot rollout; shares the existing call cap
  bool core_only = false;    // Dedicated snapshot warmer, no popularity work.
  bool snapshot_owns_core_quotes = false;  // Ranked work still maintains market rows.
  size_t budget = kBudgetPerRun;
  size_t batch_ids = kMarketBatchIds;
  size_t max_calls = kMaxCallsPerRun;
  uint64_t deadline_ms = 0;  // TokenMonotonicMs() deadline for provider calls
};

struct RunResult {
  size_t due = 0;          // coins popularity.due() returned
  size_t deferred = 0;     // due coins left for the next run by the call cap
  size_t calls = 0;        // provider calls submitted
  size_t failed_calls = 0; // unanswered, non-2xx, shed or unparseable
  size_t lane_rejected = 0;
  int last_provider_status = 0; // 0 when the provider sent no HTTP response
  uint32_t provider_retry_after_ms = 0;
  size_t refreshed = 0;    // coins marked refreshed
  size_t listed = 0;       // coins with a provider row
  size_t quotes = 0;       // spot quotes primed
  size_t market_write_failures = 0;
  size_t spot_write_failures = 0;
  size_t saved = 0;
  size_t save_frames = 0;
  size_t save_bytes = 0;
  bool aborted = false;    // a null row: TypeScript's run rejects
  bool stopped = false;
  bool failed = false;     // allocation, dirty or save failure
  uint64_t elapsed_us = 0;
};
RunResult Run(const Deps& deps, const char* fiat = "usd");

struct RestoreResult {
  size_t rows = 0;
  bool failed = false;
};
RestoreResult Restore(storage::KvStore& kv, discovery::CoinPopularity& popularity, int64_t now_ms);

struct PruneResult {
  size_t before = 0;
  size_t after = 0;
  size_t swept = 0;
};
PruneResult Prune(storage::KvStore& kv, discovery::CoinPopularity& popularity, int64_t now_ms);

}  // namespace fortis::prices::coin_prices
