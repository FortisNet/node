// The token service's scheduled jobs and the scheduler thread.
#include "token_service_internal.h"
#include "log.h"

#include "threading.h"

namespace fortis::tokens {

double TokenJobScheduler::JitterSample() const {
  double sample = 0.5;
  if (deps_.seams.jitter && deps_.seams.jitter->Sample(&sample)) return sample;
  uint32_t bits = 0;
  if (!fortis::auth::SecureRandomBytes(reinterpret_cast<uint8_t*>(&bits), sizeof(bits))) return 0.5;
  return static_cast<double>(bits) / 4294967295.0;
}

bool TokenJobScheduler::RunJob(Job job, char* summary, size_t capacity) {
  fortis::base::UniqueLock job_lock(job_mutex_);
  Work work{};
  // The coin price jobs bring their own bounded buffers: no 9 MiB job arena.
  const bool coin_job = job == Job::kCoinPrices || job == Job::kCoinPopularityPrune;
  const bool made = coin_job || deps_.workspaces.Make(&work, kJobArenaBytes, true, kJobBudgetMs);
  if (!made) return false;
  const int64_t now = Now();
  fortis::tokens::token_jobs::JobResult result{};
  fortis::tokens::price_jobs::RefreshResult prices{};
  fortis::tokens::price_jobs::SealResult seal{};
  fortis::prices::coin_prices::RunResult coins{};
  fortis::prices::coin_prices::PruneResult pruned{};
  bool ran_prices = false, ran_seal = false, ran_coins = false, ran_prune = false;
  size_t written = 0;
  bool failed = false;
  switch (job) {
    case Job::kStatsFlush: written = deps_.stats.Flush(&deps_.data, now, &failed); break;
    case Job::kDemand: {
      deps_.stats.Flush(&deps_.data, now, &failed);
      written = fortis::tokens::token_jobs::RefreshDemand(work, now);
      ran_prices = RefreshActivePrices(Now(), &prices);
      // A composed price service already seals the same series from the
      // KV maintenance timer. Keep manual and token-only sealing available.
      if (!deps_.spot) {
        seal = SealPrices(Now());
        ran_seal = true;
      }
      break;
    }
    case Job::kActivePrices: ran_prices = RefreshActivePrices(now, &prices); break;
    case Job::kPriceSeal: seal = SealPrices(now); ran_seal = true; break;
    case Job::kMarketActive: result = fortis::tokens::token_jobs::WarmMarketsActive(work, now); break;
    case Job::kMarketBroad: result = fortis::tokens::token_jobs::WarmMarketsBroad(work, now); break;
    case Job::kLiquidity: result = fortis::tokens::token_jobs::WarmLiquidity(work, now); break;
    case Job::kHolders: result = fortis::tokens::token_jobs::WarmHolders(work, now); break;
    case Job::kRollup: written = fortis::tokens::token_history::RollupLiquidityCandles(work, now); break;
    case Job::kCoinPrices: ran_coins = RunCoinPrices(&coins); break;
    case Job::kCoinPopularityPrune:
      if (deps_.popularity) {
        pruned = fortis::prices::coin_prices::Prune(deps_.kv, *deps_.popularity, now);
        ran_prune = true;
      }
      break;
  }
  TokenWorkspaces::Free(&work);
  job_lock.Unlock();
  if (summary && capacity) {
    int used = snprintf(summary, capacity,
             "{\"written\":%zu,\"failed\":%s,\"selected\":%zu,\"refreshed\":%zu,\"unavailable\":%zu,"
             "\"unsupported\":%zu,\"jobFailed\":%zu",
             written, failed ? "true" : "false", result.selected, result.refreshed, result.unavailable,
             result.unsupported, result.failed);
    if (used > 0 && static_cast<size_t>(used) < capacity && ran_prices)
      used += snprintf(summary + used, capacity - static_cast<size_t>(used),
                       ",\"prices\":{\"ranked\":%zu,\"chosen\":%zu,\"fromRecent\":%zu,\"groups\":%zu,"
                       "\"refreshed\":%zu,\"touched\":%zu,\"touchFrames\":%zu,\"recentScanned\":%zu,"
                       "\"failed\":%s,\"stopped\":%s}",
                       prices.ranked, prices.chosen, prices.from_recent, prices.groups, prices.refreshed,
                       prices.touched, prices.touch_frames, prices.recent_scanned,
                       prices.demand_failed || prices.recent_failed || prices.touch_failed ? "true" : "false",
                       prices.stopped ? "true" : "false");
    if (used > 0 && static_cast<size_t>(used) < capacity && ran_seal)
      used += snprintf(summary + used, capacity - static_cast<size_t>(used),
                       ",\"seal\":{\"sealed\":%zu,\"frames\":%zu,\"failed\":%s}", seal.sealed, seal.frames,
                       seal.failed ? "true" : "false");
    if (used > 0 && static_cast<size_t>(used) < capacity && ran_coins)
      used += snprintf(summary + used, capacity - static_cast<size_t>(used),
                       ",\"coins\":{\"due\":%zu,\"deferred\":%zu,\"calls\":%zu,\"failedCalls\":%zu,"
                       "\"laneRejected\":%zu,\"refreshed\":%zu,\"listed\":%zu,\"quotes\":%zu,"
                       "\"marketWriteFailures\":%zu,\"spotWriteFailures\":%zu,\"saved\":%zu,"
                       "\"saveFrames\":%zu,\"saveBytes\":%zu,\"aborted\":%s,\"stopped\":%s,"
                       "\"failed\":%s,\"elapsedUs\":%llu}",
                       coins.due, coins.deferred, coins.calls, coins.failed_calls, coins.lane_rejected,
                       coins.refreshed, coins.listed, coins.quotes, coins.market_write_failures,
                       coins.spot_write_failures, coins.saved, coins.save_frames, coins.save_bytes,
                       coins.aborted ? "true" : "false", coins.stopped ? "true" : "false",
                       coins.failed ? "true" : "false", static_cast<unsigned long long>(coins.elapsed_us));
    if (used > 0 && static_cast<size_t>(used) < capacity && ran_prune)
      used += snprintf(summary + used, capacity - static_cast<size_t>(used),
                       ",\"prune\":{\"before\":%zu,\"after\":%zu,\"swept\":%zu}", pruned.before,
                       pruned.after, pruned.swept);
    if (used > 0 && static_cast<size_t>(used) < capacity)
      snprintf(summary + used, capacity - static_cast<size_t>(used), "}");
  }
  return true;
}

int64_t TokenJobScheduler::Now() const { return deps_.swr.Now(); }
bool TokenJobScheduler::Stopping(void* opaque) {
  return static_cast<TokenJobScheduler*>(opaque)->deps_.control.IsStopping();
}

// PricesFacet.refreshActive() (a failure is logged and the job goes on to
// the seal, as the TypeScript job's .catch does). False without a spot owner.
bool TokenJobScheduler::RefreshActivePrices(int64_t now, fortis::tokens::price_jobs::RefreshResult* out) {
  if (!deps_.spot) return false;
  fortis::tokens::price_jobs::RefreshOptions options{};
  options.stop = &TokenJobScheduler::Stopping;
  options.stop_context = this;
  fortis::tokens::price_jobs::SpotOwnerFetchContext fetch{deps_.spot, &TokenJobScheduler::Stopping, this};
  *out = fortis::tokens::price_jobs::RefreshActive(deps_.data, deps_.series, now, fortis::tokens::price_jobs::SpotOwnerFetch, &fetch, options);
  if (out->demand_failed || out->recent_failed || out->touch_failed)
    fortis::base::LogError("tokens", "active price refresh failed");
  return true;
}

// CoinPriceScheduler.run(): the due coins through the token owner's
// CoinGecko lane, then the dirty counters. False without a tracker.
bool TokenJobScheduler::RunCoinPrices(fortis::prices::coin_prices::RunResult* out) {
  if (!deps_.popularity) return false;
  fortis::prices::coin_prices::Deps deps;
  deps.kv = &deps_.kv;
  deps.series = &deps_.series;
  deps.spot_cache = deps_.price_cache;
  deps.snapshot_owns_core_quotes = deps_.config.price_snapshot_enabled && deps_.spot != nullptr;
  deps.owner = &deps_.owner;
  deps.popularity = deps_.popularity;
  deps.clock = deps_.seams.clock;
  deps.jitter = deps_.seams.jitter;
  deps.stop = &TokenJobScheduler::Stopping;
  deps.stop_context = this;
  deps.deadline_ms = providers::TokenMonotonicMs() + kJobBudgetMs;
  *out = fortis::prices::coin_prices::Run(deps);
  if (out->aborted || out->failed) fortis::base::LogError("tokens", "coin price schedule failed");
  return true;
}

// sealDueYielding(): the job's default 200 rows in 128-row frames.
fortis::tokens::price_jobs::SealResult TokenJobScheduler::SealPrices(int64_t now) {
  const auto result = fortis::tokens::price_jobs::SealDueYielding(deps_.series, now, fortis::tokens::price_jobs::kJobSealLimit,
                                                          fortis::tokens::price_jobs::kSealBatch, &TokenJobScheduler::Stopping, this);
  if (result.failed) fortis::base::LogError("tokens", "ranked price collect failed: price seal");
  return result;
}

void* TokenJobScheduler::Entry(void* opaque) {
  auto* self = static_cast<TokenJobScheduler*>(opaque);
  struct JobSlot { Job job; int64_t interval_ms; uint64_t due_ms; };
  JobSlot slots[] = {
      {Job::kStatsFlush, fortis::tokens::token_jobs::kStatsFlushMs, 0},
      {Job::kDemand, fortis::tokens::token_jobs::kDemandIntervalMs, 0},
      {Job::kMarketActive, fortis::tokens::token_jobs::kMarketActiveIntervalMs, 0},
      {Job::kMarketBroad, fortis::tokens::token_jobs::kMarketBroadIntervalMs, 0},
      {Job::kLiquidity, fortis::tokens::token_jobs::kLiquidityIntervalMs, 0},
      {Job::kHolders, fortis::tokens::token_jobs::kHoldersIntervalMs, 0},
      {Job::kRollup, fortis::tokens::token_jobs::kRollupIntervalMs, 0},
      // The coin price schedule and the coin popularity part of daily
      // retention; only with a popularity tracker.
      {Job::kCoinPrices, fortis::prices::coin_prices::kIntervalMs, 0},
      {Job::kCoinPopularityPrune, fortis::prices::coin_prices::kPruneIntervalMs, 0},
  };
  const size_t slot_count = self->deps_.popularity ? sizeof(slots) / sizeof(slots[0])
                                             : sizeof(slots) / sizeof(slots[0]) - 2;
  const uint64_t start = providers::TokenMonotonicMs();
  // RecurringJob.start(): a random initial delay within the interval.
  for (size_t i = 0; i < slot_count; ++i)
    slots[i].due_ms = start + static_cast<uint64_t>(self->JitterSample() * static_cast<double>(slots[i].interval_ms));
  for (;;) {
    fortis::base::UniqueLock lock(self->deps_.control.mutex());
    if (self->deps_.control.StoppingLocked()) break;
    uint64_t next = UINT64_MAX;
    for (size_t i = 0; i < slot_count; ++i) if (slots[i].due_ms < next) next = slots[i].due_ms;
    const uint64_t now = providers::TokenMonotonicMs();
    if (next > now) {
      const timespec wake = wake_after(next - now > 1000 ? 1000 : next - now);
      self->deps_.control.changed().WaitUntilLocked(self->deps_.control.mutex(), wake);
      continue;
    }
    lock.Unlock();
    for (size_t i = 0; i < slot_count; ++i) {
      JobSlot& slot = slots[i];
      if (slot.due_ms > providers::TokenMonotonicMs()) continue;
      self->RunJob(slot.job, nullptr, 0);
      // Completion-scheduled with +-10 % jitter (jitteredDelay).
      const double unit = self->JitterSample();
      const double delay = floor(static_cast<double>(slot.interval_ms) * (1 + 0.1 * (2 * unit - 1)) + 0.5);
      slot.due_ms = providers::TokenMonotonicMs() + static_cast<uint64_t>(delay > 1 ? delay : 1);
      lock.Lock();
      const bool stop = self->deps_.control.StoppingLocked();
      lock.Unlock();
      if (stop) break;
    }
  }
  return nullptr;
}

bool TokenJobScheduler::Start(const pthread_attr_t* attributes) {
  if (started_) return false;
  if (deps_.seams.manual_jobs) return true;
  started_ = thread_.StartEntry<Entry>(this, attributes);
  return started_;
}
void TokenJobScheduler::Join() { if (started_) thread_.Join(); }

}  // namespace fortis::tokens
