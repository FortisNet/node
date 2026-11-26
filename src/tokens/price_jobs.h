#pragma once

#include "price_id_parser.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::prices { class PriceSeriesStore; }
namespace fortis::tokens::token_data { class TokenDataStore; }
namespace fortis::prices { class SpotProviderOwner; }

// The scheduled price work of the TypeScript demand job and the 10 s
// maintenance timer (started by the TypeScript service's main()):
//   PricesFacet.refreshActive(24, TOKEN_WORKLOADS.price.cap)
//   PriceSeriesStore.sealDueYielding(now, limit, 128)
// TokenService::Job::kDemand runs both after its stats flush and demand
// projection (the TypeScript job order); the KV expiry sweeper's tick runs
// the 1,024-row seal. Guest-safe: no C++ standard-library headers.
namespace fortis::tokens::price_jobs {

// The TypeScript TOKEN_WORKLOADS.price.
constexpr int64_t kRefreshIntervalMs = 5 * 60000;
constexpr size_t kRefreshCap = 200;
constexpr int64_t kTier1DueMs = 15 * 60000;
// refreshActive(sinceHours = 24): the recent-spot fallback window.
constexpr int64_t kRecentWindowMs = 24LL * 3600000;
// The transactional touch batch of refreshActive.
constexpr size_t kTouchBatch = 128;
// sealDueYielding(now, limit = 200, batchSize = 128) in the job, and the
// timer's sealDueYielding(Date.now(), 1_024).
constexpr size_t kSealBatch = 128;
constexpr size_t kJobSealLimit = 200;
constexpr size_t kTimerSealLimit = 1024;

// One PricesFacet.spot(ids, fiat, { foreground: false, background: true })
// call. Sets fresh[i] for every id a provider priced in this call (the
// response's `cache.hit` false); cached, stale, unknown and failed ids stay
// false. Returns false when the call could not be made at all.
using SpotFetchFn = bool (*)(void* context, const char* fiat, const char* const* ids, size_t count,
                             bool* fresh);
// Observes each planned spot call before it is made (tests record the plan).
using PlanObserverFn = void (*)(void* context, const char* fiat, const char* const* ids, size_t count);
using StopFn = bool (*)(void* context);

struct RefreshOptions {
  size_t cap = kRefreshCap;
  int64_t recent_window_ms = kRecentWindowMs;
  PlanObserverFn observe = nullptr;
  void* observe_context = nullptr;
  // Checked between fiat groups; a stop leaves later groups unfetched.
  StopFn stop = nullptr;
  void* stop_context = nullptr;
};

struct RefreshResult {
  size_t ranked = 0;         // demand rows kept by the tier/due filter (before the cap)
  size_t chosen = 0;         // unique source IDs planned (<= cap)
  size_t from_recent = 0;    // of which recent-spot fallbacks
  size_t groups = 0;         // fiat groups (spot calls)
  size_t refreshed = 0;      // refreshActive's return value
  size_t touched = 0;        // demand rows given lastRefreshedAt
  size_t touch_frames = 0;   // KV frames written by the touch
  size_t recent_scanned = 0; // spot records visited by the recent scan
  bool demand_failed = false, recent_failed = false, touch_failed = false, fetch_failed = false;
  bool stopped = false;
};

// refreshActive: at most `cap` demand rows with a source ID and price tier
// 0, or tier 1 not refreshed within 15 minutes, ordered by tier, then
// lastRefreshedAt (never = 0), then Number(priceScore) descending (equal or
// NaN keeps key order); their unique source IDs are fetched in USD. Unused
// slots take recently observed spot rows (oldest first, any fiat, a source
// ID already chosen is skipped). One spot call per fiat in first-seen
// order; demand rows whose source ID a provider freshly priced get
// lastRefreshedAt = now_ms in frames of 128.
RefreshResult RefreshActive(token_data::TokenDataStore& data, const prices::PriceSeriesStore& series, int64_t now_ms,
                            SpotFetchFn fetch, void* fetch_context, const RefreshOptions& options = RefreshOptions{});

// The spot fetch through the price canary's SpotProviderOwner in background
// mode: chunks of 100 IDs submitted together (they share one batch window),
// each waited for (the owner's 30 s deadline bounds the wait; a raised stop
// cancels the outstanding requests); a busy owner or a failed submission
// leaves that chunk unrefreshed. `context` is a SpotOwnerFetchContext.
struct SpotOwnerFetchContext {
  prices::SpotProviderOwner* owner = nullptr;
  StopFn stop = nullptr;
  void* stop_context = nullptr;
};
bool SpotOwnerFetch(void* opaque, const char* fiat, const char* const* ids, size_t count, bool* fresh);

struct SealResult {
  size_t sealed = 0;  // open rows moved into packed segments
  size_t frames = 0;  // KV frames written (one per SealDue call that stored)
  bool failed = false;
  bool stopped = false;
};

// sealDueYielding(now, limit, batch): SealDue in calls of at most `batch`
// rows until `limit` rows are sealed or nothing is due, sleeping `yield_us`
// between calls with the series lock released so foreground reads and
// writes run. Each call is one atomic KV frame; an interrupted pass resumes
// from the remaining markers on the next pass.
SealResult SealDueYielding(prices::PriceSeriesStore& series, int64_t now_ms, size_t limit, size_t batch = kSealBatch,
                           StopFn stop = nullptr, void* stop_context = nullptr, uint32_t yield_us = 1000);

}  // namespace fortis::tokens::price_jobs
