#include "provider_lane.h"
#include "log.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace fortis::providers {

namespace {
constexpr uint32_t kDefaultBreakerThreshold = 5;
constexpr uint32_t kDefaultBreakerCooldownMs = 30000;
constexpr uint32_t kMaxQueueTimeoutMs = 60000;
constexpr uint32_t kMaxBreakerThreshold = 100;
constexpr uint32_t kMaxBreakerCooldownMs = 300000;

ProviderLaneConfig budget(uint32_t rate, uint32_t interval, uint32_t burst,
                          uint32_t concurrency, uint32_t queued = 100,
                          uint32_t timeout = 2000) {
  return {rate, interval, burst, concurrency, queued, timeout,
          kDefaultBreakerThreshold, kDefaultBreakerCooldownMs};
}

const ProviderLaneDefault kDefaults[] = {
    {"coingecko", budget(24, 60000, 4, 2, 100, 3000)},
    {"mexc", budget(1, 1000, 1, 1, 8, 5000)},
    {"defillama", budget(2, 1000, 2, 2, 32, 5000)},
    {"kraken", budget(1, 1000, 1, 1, 20, 5000)},
    {"geckoterminal", budget(10, 60000, 2, 2)},
    {"dexscreener", budget(60, 60000, 15, 8)},
    {"4byte", budget(30, 60000, 30, 4)},
    {"saucerswap", budget(60, 60000, 15, 8)},
    {"goplus", budget(30, 60000, 30, 4)},
    {"ethplorer", budget(30, 60000, 30, 4)},
    {"etherscan", budget(3, 1000, 2, 2)},
    {"alchemy", budget(100, 1000, 25, 8)},
    {"alchemy-wallet", budget(20, 1000, 5, 4, 40)},
    {"alchemy-simulation", budget(10, 1000, 3, 2, 30)},
    {"blockscout", budget(30, 60000, 30, 4)},
    {"koios", budget(50, 60000, 13, 7)},
    {"horizon", budget(60, 60000, 15, 8)},
    {"esplora", budget(30, 60000, 30, 4)},
    {"algod", budget(60, 60000, 15, 8)},
    {"algorand-indexer", budget(60, 60000, 15, 8)},
    {"hedera-mirror", budget(60, 60000, 15, 8)},
    {"vestige", budget(30, 60000, 30, 4)},
    {"jupiter", budget(60, 60000, 15, 8)},
    {"robinhood-assets", budget(60, 1000, 15, 8)},
    {"helius", budget(50, 1000, 13, 7)},
    {"trongrid", budget(10, 1000, 3, 2)},
    {"tronscan", budget(20, 60000, 20, 3)},
    {"taostats", budget(30, 60000, 30, 4)},
    {"simpleswap", budget(30, 60000, 30, 4)},
    {"lifi", budget(100, 60000, 25, 8)},
    {"squid", budget(30, 60000, 30, 4)},
    {"near-intents", budget(30, 60000, 30, 4)},
    {"bridge-vet", budget(20, 60000, 20, 2)},
    {"axelarscan", budget(30, 60000, 30, 4)},
    {"nearblocks", budget(5, 60000, 5, 2)},
    {"news-feeds", budget(60, 60000, 15, 4)},
};

const ProviderLaneConfig kRpcDefault = budget(20, 1000, 5, 8, 200);

bool safe_lane_name(const char* name) {
  if (!name || !*name || strnlen(name, kProviderLaneNameCapacity) >=
                             kProviderLaneNameCapacity) return false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p)
    if (*p <= 0x20 || *p >= 0x7f) return false;
  return true;
}

uint64_t saturating_add(uint64_t left, uint64_t right) {
  return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

uint64_t refill_delay_ms(const ProviderLaneConfig& config, double tokens) {
  if (tokens >= 1.0) return 0;
  const double missing = 1.0 - tokens;
  const double delay = ceil(missing * config.interval_ms / config.requests_per_interval);
  if (delay >= static_cast<double>(UINT64_MAX)) return UINT64_MAX;
  return static_cast<uint64_t>(delay);
}
}  // namespace

size_t ProviderLaneDefaultCount() {
  return sizeof(kDefaults) / sizeof(kDefaults[0]);
}

bool ProviderLaneDefaultAt(size_t index, ProviderLaneDefault* output) {
  if (!output || index >= ProviderLaneDefaultCount()) return false;
  *output = kDefaults[index];
  return true;
}

bool ProviderLaneConfigValid(const ProviderLaneConfig& config) {
  return config.requests_per_interval > 0 && config.interval_ms > 0 &&
         config.burst > 0 && config.burst <= config.requests_per_interval &&
         config.max_concurrent > 0 && config.max_concurrent <= kProviderLaneMaxConcurrent &&
         config.max_queued <= kProviderLaneMaxQueuePerLane &&
         config.queue_timeout_ms > 0 && config.queue_timeout_ms <= kMaxQueueTimeoutMs &&
         config.breaker_threshold > 0 && config.breaker_threshold <= kMaxBreakerThreshold &&
         config.breaker_cooldown_ms > 0 && config.breaker_cooldown_ms <= kMaxBreakerCooldownMs;
}

bool ProviderLaneBudgetFor(const char* lane_name, ProviderLaneConfig* output) {
  if (!output || !safe_lane_name(lane_name)) return false;
  for (size_t i = 0; i < ProviderLaneDefaultCount(); ++i) {
    if (strcmp(kDefaults[i].name, lane_name) == 0) {
      *output = kDefaults[i].config;
#ifdef FORTIS_PROVIDER_TEST_COINGECKO_QUEUE_TIMEOUT_MS
      if (strcmp(lane_name, "coingecko") == 0)
        output->queue_timeout_ms = FORTIS_PROVIDER_TEST_COINGECKO_QUEUE_TIMEOUT_MS;
#endif
      return ProviderLaneConfigValid(*output);
    }
  }
  const size_t length = strlen(lane_name);
  if ((strncmp(lane_name, "rpc:", 4) == 0 && length > 4) ||
      (strncmp(lane_name, "news-feeds:", 11) == 0 && length > 11)) {
    if (strncmp(lane_name, "news-feeds:", 11) == 0) {
      for (size_t i = 0; i < ProviderLaneDefaultCount(); ++i) {
        if (strcmp(kDefaults[i].name, "news-feeds") == 0) {
          *output = kDefaults[i].config;
          return true;
        }
      }
    }
    *output = kRpcDefault;
    return true;
  }
  // Match the TypeScript facade: unknown lanes (including media host lanes)
  // use the bounded RPC fallback policy.
  *output = kRpcDefault;
  return true;
}

ProviderLaneOutcome ProviderLaneOutcomeForHttpStatus(int status,
                                                       bool transport_failure) {
  if (transport_failure || status <= 0 || status == 401 || status == 403 ||
      status == 408 || status == 429 || status >= 500)
    return status == 429 && !transport_failure ? ProviderLaneOutcome::kRateLimited
                                               : ProviderLaneOutcome::kFault;
  return ProviderLaneOutcome::kAnswered;
}

ProviderLaneRegistry::ProviderLaneRegistry(fortis::base::Clock* clock, bool keyless_coingecko)
    : clock_(clock), keyless_coingecko_(keyless_coingecko),
      valid_(clock != nullptr), closed_(false),
      next_job_id_(1), next_sequence_(1) {
  for (size_t i = 0; i < kProviderLaneMaxActiveLanes; ++i)
    lanes_[i] = LaneState{};
  memset(queued_jobs_, 0, sizeof(queued_jobs_));
  memset(active_jobs_, 0, sizeof(active_jobs_));
  for (size_t i = 0; i < ProviderLaneDefaultCount(); ++i) {
    ProviderLaneDefault entry{};
    if (!ProviderLaneDefaultAt(i, &entry) || !ProviderLaneConfigValid(entry.config))
      valid_ = false;
  }
  if (!ProviderLaneConfigValid(kRpcDefault)) valid_ = false;
}

int ProviderLaneRegistry::FindLane(const char* lane_name) const {
  if (!safe_lane_name(lane_name)) return -1;
  for (size_t i = 0; i < kProviderLaneMaxActiveLanes; ++i) {
    if (lanes_[i].used && strcmp(lanes_[i].name, lane_name) == 0)
      return static_cast<int>(i);
  }
  return -1;
}

int ProviderLaneRegistry::GetOrCreateLane(const char* lane_name) {
  const int existing = FindLane(lane_name);
  if (existing >= 0) return existing;
  ProviderLaneConfig config{};
  if (!ProviderLaneBudgetFor(lane_name, &config)) return -1;
  if (keyless_coingecko_ && strcmp(lane_name, "coingecko") == 0) {
    // The anonymous API shares egress-IP quota. Pace physical attempts
    // at one every twelve seconds, the low end of the published quota.
    // Serve the cache or return pending immediately
    // when the next token is unavailable instead of parking cold requests.
    config = budget(5, 60000, 1, 1, 0);
    config.breaker_cooldown_ms = 120000;
  }
  if (!ProviderLaneConfigValid(config)) return -1;

  int slot = -1;
  for (size_t i = 0; i < kProviderLaneMaxActiveLanes; ++i) {
    if (!lanes_[i].used) { slot = static_cast<int>(i); break; }
  }
  // Never recycle a lane identity: doing so would reset a consumed token
  // budget or an open breaker and let lane-name churn bypass provider limits.
  if (slot < 0) return -1;

  LaneState* lane = &lanes_[slot];
  *lane = LaneState{};
  lane->used = true;
  snprintf(lane->name, sizeof(lane->name), "%s", lane_name);
  lane->config = config;
  lane->tokens = config.burst;
  lane->last_refill_ms = clock_->MonotonicMs();
  lane->breaker = ProviderBreakerState::kClosed;
  return slot;
}

void ProviderLaneRegistry::Refill(LaneState* lane, uint64_t now) {
  if (now <= lane->last_refill_ms) return;
  const uint64_t elapsed = now - lane->last_refill_ms;
  const double rate = static_cast<double>(lane->config.requests_per_interval) /
                      lane->config.interval_ms;
  lane->tokens += static_cast<double>(elapsed) * rate;
  if (lane->tokens > lane->config.burst) lane->tokens = lane->config.burst;
  lane->last_refill_ms = now;
}

bool ProviderLaneRegistry::CanDispatch(LaneState* lane, uint64_t now) {
  Refill(lane, now);
  if (lane->tokens < 1.0) return false;
  if (lane->breaker == ProviderBreakerState::kHalfOpen)
    return !lane->probe_in_flight && lane->in_flight == 0;
  return lane->breaker == ProviderBreakerState::kClosed &&
         lane->in_flight < lane->config.max_concurrent;
}

void ProviderLaneRegistry::AdvanceBreaker(LaneState* lane, uint64_t now) {
  if (lane->breaker == ProviderBreakerState::kOpen && now >= lane->open_until_ms) {
    lane->breaker = ProviderBreakerState::kHalfOpen;
  }
}

bool ProviderLaneRegistry::ReserveActive(int lane_index, uint64_t job_id, bool probe) {
  const size_t total = kProviderLaneMaxActiveLanes * kProviderLaneMaxConcurrent;
  for (size_t i = 0; i < total; ++i) {
    if (!active_jobs_[i].used) {
      active_jobs_[i] = {true, probe, static_cast<size_t>(lane_index), job_id};
      return true;
    }
  }
  return false;
}

bool ProviderLaneRegistry::StartDispatch(int lane_index, uint64_t job_id, void* context,
                                         ProviderLaneDispatch* output) {
  LaneState* lane = &lanes_[lane_index];
  const bool probe = lane->breaker == ProviderBreakerState::kHalfOpen;
  if (!ReserveActive(lane_index, job_id, probe)) return false;
  lane->tokens -= 1.0;
  ++lane->in_flight;
  ++lane->dispatched;
  if (probe) lane->probe_in_flight = true;
  output->job_id = job_id;
  output->context = context;
  output->half_open_probe = probe;
  return true;
}

void ProviderLaneRegistry::MarkCancelled(int queue_index,
                                         ProviderLaneCancelReason reason) {
  QueuedJob* job = &queued_jobs_[queue_index];
  if (job->state != QueuedState::kQueued) return;
  LaneState* lane = &lanes_[job->lane_index];
  job->state = QueuedState::kCancelled;
  job->cancel_reason = reason;
  if (lane->queued) --lane->queued;
  ++lane->cancelled_pending;
  if (reason == ProviderLaneCancelReason::kQueueTimeout) ++lane->rejected_queue_timeout;
  if (reason == ProviderLaneCancelReason::kBreakerOpen) ++lane->rejected_breaker;
}

void ProviderLaneRegistry::ExpireQueued(int lane_index, uint64_t now) {
  for (size_t i = 0; i < kProviderLaneMaxQueuedJobs; ++i) {
    QueuedJob* job = &queued_jobs_[i];
    if (job->state == QueuedState::kQueued &&
        job->lane_index == static_cast<size_t>(lane_index) && job->deadline_ms <= now)
      MarkCancelled(static_cast<int>(i), ProviderLaneCancelReason::kQueueTimeout);
  }
}

void ProviderLaneRegistry::OpenBreaker(int lane_index, uint64_t duration_ms) {
  LaneState* lane = &lanes_[lane_index];
  if (lane->breaker != ProviderBreakerState::kOpen) ++lane->breaker_opens;
  lane->breaker = ProviderBreakerState::kOpen;
  lane->open_until_ms = saturating_add(clock_->MonotonicMs(), duration_ms);
  lane->consecutive_faults = 0;
  lane->probe_in_flight = false;
  for (size_t i = 0; i < kProviderLaneMaxQueuedJobs; ++i) {
    QueuedJob* job = &queued_jobs_[i];
    if (job->state == QueuedState::kQueued && job->lane_index == static_cast<size_t>(lane_index))
      MarkCancelled(static_cast<int>(i), ProviderLaneCancelReason::kBreakerOpen);
  }
}

ProviderLaneAdmission ProviderLaneRegistry::TryAdmit(const char* lane_name, void* context,
                                                      bool background,
                                                      bool has_caller_deadline,
                                                      uint32_t caller_deadline_ms) {
  ProviderLaneAdmission result{};
  if (!valid_) { result.status = ProviderLaneAdmissionStatus::kInvalidConfig; return result; }
  if (!safe_lane_name(lane_name)) { result.status = ProviderLaneAdmissionStatus::kInvalidLane; return result; }
  if (closed_) { result.status = ProviderLaneAdmissionStatus::kClosed; return result; }
  const int lane_index = GetOrCreateLane(lane_name);
  if (lane_index < 0) { result.status = ProviderLaneAdmissionStatus::kLaneCapacity; return result; }
  LaneState* lane = &lanes_[lane_index];
  const uint64_t now = clock_->MonotonicMs();
  AdvanceBreaker(lane, now);
  ExpireQueued(lane_index, now);
  if (lane->breaker == ProviderBreakerState::kOpen) {
    ++lane->rejected_breaker;
    result.status = ProviderLaneAdmissionStatus::kBreakerOpen;
    result.retry_after_ms = lane->open_until_ms > now ? lane->open_until_ms - now : 0;
    return result;
  }

  const bool can_dispatch = lane->queued == 0 && CanDispatch(lane, now);
  // The snapshot publisher must be able to probe anonymous CoinGecko after
  // cooldown even when every foreground request can be served from cache.
  const bool background_probe = keyless_coingecko_ &&
      lane->breaker == ProviderBreakerState::kHalfOpen &&
      strcmp(lane_name, "coingecko") == 0;
  if (background && ((!background_probe && lane->breaker != ProviderBreakerState::kClosed) ||
                     lane->in_flight > 0 || !can_dispatch)) {
    ++lane->rejected_background;
    result.status = ProviderLaneAdmissionStatus::kBackgroundBusy;
    return result;
  }

  uint64_t wait_ms = lane->config.queue_timeout_ms;
  if (has_caller_deadline && caller_deadline_ms < wait_ms) wait_ms = caller_deadline_ms;
  const uint64_t deadline = saturating_add(now, wait_ms);
  uint64_t job_id = next_job_id_++;
  if (job_id == 0) job_id = next_job_id_++;
  result.job_id = job_id;

  if (can_dispatch) {
    if (!StartDispatch(lane_index, job_id, context, &result.dispatch)) {
      result.status = ProviderLaneAdmissionStatus::kLaneCapacity;
      result.job_id = 0;
      return result;
    }
    result.status = ProviderLaneAdmissionStatus::kDispatched;
    result.has_dispatch = true;
    return result;
  }
  if (deadline <= now) {
    ++lane->rejected_queue_timeout;
    result.status = ProviderLaneAdmissionStatus::kQueueTimeout;
    result.job_id = 0;
    return result;
  }
  if (lane->queued >= lane->config.max_queued) {
    ++lane->rejected_queue_full;
    result.status = ProviderLaneAdmissionStatus::kQueueFull;
    result.job_id = 0;
    return result;
  }
  int free_slot = -1;
  for (size_t i = 0; i < kProviderLaneMaxQueuedJobs; ++i) {
    if (queued_jobs_[i].state == QueuedState::kFree) { free_slot = static_cast<int>(i); break; }
  }
  if (free_slot < 0) {
    ++lane->rejected_queue_full;
    result.status = ProviderLaneAdmissionStatus::kQueueFull;
    result.job_id = 0;
    return result;
  }
  queued_jobs_[free_slot] = {QueuedState::kQueued, static_cast<size_t>(lane_index), job_id,
                             next_sequence_++, deadline, context,
                             ProviderLaneCancelReason::kCallerCancelled};
  ++lane->queued;
  result.status = ProviderLaneAdmissionStatus::kQueued;
  return result;
}

void ProviderLaneRegistry::DispatchQueued(int lane_index, int queue_index,
                                          ProviderLaneDispatch* output) {
  QueuedJob* job = &queued_jobs_[queue_index];
  if (!StartDispatch(lane_index, job->job_id, job->context, output)) return;
  if (lanes_[lane_index].queued) --lanes_[lane_index].queued;
  memset(job, 0, sizeof(*job));
}

bool ProviderLaneRegistry::TryTakeNext(const char* lane_name,
                                       ProviderLaneDispatch* output) {
  if (!valid_ || !output || closed_) return false;
  *output = {};
  const int lane_index = FindLane(lane_name);
  if (lane_index < 0) return false;
  LaneState* lane = &lanes_[lane_index];
  const uint64_t now = clock_->MonotonicMs();
  AdvanceBreaker(lane, now);
  ExpireQueued(lane_index, now);
  if (lane->breaker == ProviderBreakerState::kOpen || lane->queued == 0 ||
      !CanDispatch(lane, now)) return false;

  int oldest = -1;
  uint64_t sequence = UINT64_MAX;
  for (size_t i = 0; i < kProviderLaneMaxQueuedJobs; ++i) {
    const QueuedJob& job = queued_jobs_[i];
    if (job.state == QueuedState::kQueued &&
        job.lane_index == static_cast<size_t>(lane_index) && job.sequence < sequence) {
      oldest = static_cast<int>(i);
      sequence = job.sequence;
    }
  }
  if (oldest < 0) return false;
  DispatchQueued(lane_index, oldest, output);
  return output->job_id != 0;
}

bool ProviderLaneRegistry::TryTakeCancelled(ProviderLaneCancelledJob* output) {
  if (!output) return false;
  int oldest = -1;
  uint64_t sequence = UINT64_MAX;
  for (size_t i = 0; i < kProviderLaneMaxQueuedJobs; ++i) {
    const QueuedJob& job = queued_jobs_[i];
    if (job.state == QueuedState::kCancelled && job.sequence < sequence) {
      oldest = static_cast<int>(i);
      sequence = job.sequence;
    }
  }
  if (oldest < 0) return false;
  QueuedJob* job = &queued_jobs_[oldest];
  output->job_id = job->job_id;
  output->context = job->context;
  output->reason = job->cancel_reason;
  LaneState* lane = &lanes_[job->lane_index];
  if (lane->cancelled_pending) --lane->cancelled_pending;
  memset(job, 0, sizeof(*job));
  return true;
}

bool ProviderLaneRegistry::CancelQueued(const char* lane_name, uint64_t job_id) {
  const int lane_index = FindLane(lane_name);
  if (lane_index < 0 || !job_id) return false;
  for (size_t i = 0; i < kProviderLaneMaxQueuedJobs; ++i) {
    QueuedJob& job = queued_jobs_[i];
    if (job.state == QueuedState::kQueued && job.lane_index == static_cast<size_t>(lane_index) &&
        job.job_id == job_id) {
      MarkCancelled(static_cast<int>(i), ProviderLaneCancelReason::kCallerCancelled);
      return true;
    }
  }
  return false;
}

bool ProviderLaneRegistry::Complete(const char* lane_name, uint64_t job_id,
                                    ProviderLaneOutcome outcome,
                                    uint32_t retry_after_ms) {
  const int lane_index = FindLane(lane_name);
  if (lane_index < 0 || !job_id) return false;
  size_t active_index = kProviderLaneMaxActiveLanes * kProviderLaneMaxConcurrent;
  for (size_t i = 0; i < kProviderLaneMaxActiveLanes * kProviderLaneMaxConcurrent; ++i) {
    if (active_jobs_[i].used && active_jobs_[i].lane_index == static_cast<size_t>(lane_index) &&
        active_jobs_[i].job_id == job_id) { active_index = i; break; }
  }
  if (active_index == kProviderLaneMaxActiveLanes * kProviderLaneMaxConcurrent) return false;

  const bool probe = active_jobs_[active_index].probe;
  memset(&active_jobs_[active_index], 0, sizeof(active_jobs_[active_index]));
  LaneState* lane = &lanes_[lane_index];
  if (lane->in_flight) --lane->in_flight;
  if (probe) lane->probe_in_flight = false;

  if (outcome == ProviderLaneOutcome::kAnswered) {
    ++lane->answered;
    lane->consecutive_faults = 0;
    if (probe) {
      lane->breaker = ProviderBreakerState::kClosed;
      lane->open_until_ms = 0;
    }
    return true;
  }
  if (outcome == ProviderLaneOutcome::kCancelled) {
    if (probe && keyless_coingecko_ && strcmp(lane_name, "coingecko") == 0)
      return true;  // A cancelled probe has not established recovery.
    // The TS queue treats an AbortError (anything other than UpstreamError)
    // as an answer; preserve that settlement rule, including half-open close.
    ++lane->answered;
    lane->consecutive_faults = 0;
    if (probe) {
      lane->breaker = ProviderBreakerState::kClosed;
      lane->open_until_ms = 0;
    }
    return true;
  }

  ++lane->faults;
  if (outcome == ProviderLaneOutcome::kRateLimited) {
    const uint32_t duration = retry_after_ms > lane->config.breaker_cooldown_ms ?
                              retry_after_ms : lane->config.breaker_cooldown_ms;
    if (strcmp(lane_name, "coingecko") == 0)
      fortis::base::LogWarn("providers", "CoinGecko HTTP 429: paused lane for %u ms (%s)", duration,
              keyless_coingecko_ ? "keyless" : "authenticated");
    OpenBreaker(lane_index, duration);
    return true;
  }
  if (probe) {
    OpenBreaker(lane_index, lane->config.breaker_cooldown_ms);
    return true;
  }
  ++lane->consecutive_faults;
  if (lane->consecutive_faults >= lane->config.breaker_threshold)
    OpenBreaker(lane_index, lane->config.breaker_cooldown_ms);
  return true;
}

uint64_t ProviderLaneRegistry::NextWakeupMs(const char* lane_name) {
  const int lane_index = FindLane(lane_name);
  if (lane_index < 0 || closed_) return UINT64_MAX;
  LaneState* lane = &lanes_[lane_index];
  const uint64_t now = clock_->MonotonicMs();
  AdvanceBreaker(lane, now);
  uint64_t next = UINT64_MAX;
  if (lane->breaker == ProviderBreakerState::kOpen) next = lane->open_until_ms;
  for (size_t i = 0; i < kProviderLaneMaxQueuedJobs; ++i) {
    const QueuedJob& job = queued_jobs_[i];
    if (job.state == QueuedState::kQueued && job.lane_index == static_cast<size_t>(lane_index) &&
        job.deadline_ms < next) next = job.deadline_ms;
  }
  if (lane->queued && lane->tokens < 1.0 &&
      lane->config.requests_per_interval > 0) {
    Refill(lane, now);
    const uint64_t delay = refill_delay_ms(lane->config, lane->tokens);
    const uint64_t token_at = saturating_add(now, delay);
    if (token_at < next) next = token_at;
  }
  return next;
}

bool ProviderLaneRegistry::Stats(const char* lane_name, ProviderLaneStats* output) {
  if (!output) return false;
  const int lane_index = FindLane(lane_name);
  if (lane_index < 0) return false;
  LaneState& lane = lanes_[lane_index];
  AdvanceBreaker(&lane, clock_->MonotonicMs());
  *output = {lane.queued, lane.cancelled_pending, lane.in_flight,
             lane.dispatched, lane.answered, lane.faults,
             lane.rejected_queue_full, lane.rejected_queue_timeout,
             lane.rejected_breaker, lane.rejected_background,
             lane.breaker_opens, lane.consecutive_faults, lane.breaker};
  return true;
}

void ProviderLaneRegistry::Close() {
  if (closed_) return;
  closed_ = true;
  for (size_t i = 0; i < kProviderLaneMaxQueuedJobs; ++i) {
    if (queued_jobs_[i].state == QueuedState::kQueued)
      MarkCancelled(static_cast<int>(i), ProviderLaneCancelReason::kRegistryClosed);
  }
}

size_t ProviderLaneRegistry::active_lane_count() const {
  size_t count = 0;
  for (size_t i = 0; i < kProviderLaneMaxActiveLanes; ++i) count += lanes_[i].used ? 1 : 0;
  return count;
}

size_t ProviderLaneRegistry::queued_job_count() const {
  size_t count = 0;
  for (size_t i = 0; i < kProviderLaneMaxQueuedJobs; ++i)
    count += queued_jobs_[i].state == QueuedState::kQueued ? 1 : 0;
  return count;
}

size_t ProviderLaneRegistry::in_flight_count() const {
  size_t count = 0;
  for (size_t i = 0; i < kProviderLaneMaxActiveLanes * kProviderLaneMaxConcurrent; ++i)
    count += active_jobs_[i].used ? 1 : 0;
  return count;
}

const char* ProviderLaneAdmissionName(ProviderLaneAdmissionStatus status) {
  switch (status) {
    case ProviderLaneAdmissionStatus::kDispatched: return "dispatched";
    case ProviderLaneAdmissionStatus::kQueued: return "queued";
    case ProviderLaneAdmissionStatus::kQueueFull: return "queue_full";
    case ProviderLaneAdmissionStatus::kQueueTimeout: return "queue_timeout";
    case ProviderLaneAdmissionStatus::kBreakerOpen: return "breaker_open";
    case ProviderLaneAdmissionStatus::kBackgroundBusy: return "background_busy";
    case ProviderLaneAdmissionStatus::kLaneCapacity: return "lane_capacity";
    case ProviderLaneAdmissionStatus::kClosed: return "closed";
    case ProviderLaneAdmissionStatus::kInvalidLane: return "invalid_lane";
    case ProviderLaneAdmissionStatus::kInvalidConfig: return "invalid_config";
    case ProviderLaneAdmissionStatus::kUnknownJob: return "unknown_job";
  }
  return "unknown";
}

}  // namespace fortis::providers
