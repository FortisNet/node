#include "provider_lane.h"

#include <assert.h>
#include <stdio.h>

using namespace fortis::providers;

struct FakeClock final : fortis::base::Clock {
  uint64_t now_ms = 0;
  uint64_t MonotonicMs() override { return now_ms; }
};

static void complete(ProviderLaneRegistry& registry, const char* lane,
                     const ProviderLaneAdmission& admission,
                     ProviderLaneOutcome outcome) {
  assert(admission.has_dispatch);
  assert(registry.Complete(lane, admission.dispatch.job_id, outcome));
}

static void test_budgets_and_status_mapping() {
  assert(ProviderLaneDefaultCount() == 36);
  ProviderLaneConfig fallback{};
  assert(ProviderLaneBudgetFor("kraken", &fallback));
  assert(fallback.requests_per_interval == 1 && fallback.interval_ms == 1000 &&
         fallback.burst == 1 && fallback.max_concurrent == 1);
  assert(ProviderLaneBudgetFor("defillama", &fallback));
  assert(fallback.requests_per_interval == 2 && fallback.interval_ms == 1000 &&
         fallback.burst == 2 && fallback.max_concurrent == 2 &&
         fallback.max_queued == 32 && fallback.queue_timeout_ms == 5000);
  assert(ProviderLaneBudgetFor("mexc", &fallback));
  assert(fallback.requests_per_interval == 1 && fallback.interval_ms == 1000 &&
         fallback.burst == 1 && fallback.max_concurrent == 1 &&
         fallback.max_queued == 8 && fallback.queue_timeout_ms == 5000);
  for (size_t i = 0; i < ProviderLaneDefaultCount(); ++i) {
    ProviderLaneDefault entry{};
    assert(ProviderLaneDefaultAt(i, &entry));
    assert(ProviderLaneConfigValid(entry.config));
  }
  ProviderLaneConfig budget{};
  assert(ProviderLaneBudgetFor("rpc:node.example", &budget));
  assert(budget.requests_per_interval == 20 && budget.burst == 5 &&
         budget.max_concurrent == 8 && budget.max_queued == 200);
  assert(ProviderLaneBudgetFor("news-feeds:publisher", &budget));
  assert(budget.max_concurrent == 4 && budget.burst == 15);
  assert(ProviderLaneBudgetFor("helius", &budget));
  assert(budget.max_concurrent == 7 && budget.burst == 13);
  assert(ProviderLaneOutcomeForHttpStatus(404, false) == ProviderLaneOutcome::kAnswered);
  assert(ProviderLaneOutcomeForHttpStatus(200, true) == ProviderLaneOutcome::kFault);
  assert(ProviderLaneOutcomeForHttpStatus(0, false) == ProviderLaneOutcome::kFault);
  assert(ProviderLaneOutcomeForHttpStatus(401, false) == ProviderLaneOutcome::kFault);
  assert(ProviderLaneOutcomeForHttpStatus(403, false) == ProviderLaneOutcome::kFault);
  assert(ProviderLaneOutcomeForHttpStatus(408, false) == ProviderLaneOutcome::kFault);
  assert(ProviderLaneOutcomeForHttpStatus(429, false) == ProviderLaneOutcome::kRateLimited);
  assert(ProviderLaneOutcomeForHttpStatus(503, false) == ProviderLaneOutcome::kFault);
}

static void test_token_pacing_concurrency_and_fifo() {
  FakeClock clock;
  ProviderLaneRegistry registry(&clock);
  int contexts[11]{};
  ProviderLaneAdmission concurrent[8];
  for (int i = 0; i < 8; ++i) {
    concurrent[i] = registry.TryAdmit("alchemy", &contexts[i]);
    assert(concurrent[i].status == ProviderLaneAdmissionStatus::kDispatched);
  }
  assert(registry.TryAdmit("alchemy", &contexts[8]).status ==
         ProviderLaneAdmissionStatus::kQueued);
  for (int i = 0; i < 8; ++i)
    assert(registry.Complete("alchemy", concurrent[i].dispatch.job_id,
                             ProviderLaneOutcome::kAnswered));

  ProviderLaneAdmission first[5];
  for (int i = 0; i < 5; ++i) {
    first[i] = registry.TryAdmit("rpc:pacing", &contexts[i]);
    assert(first[i].status == ProviderLaneAdmissionStatus::kDispatched);
  }
  assert(registry.TryAdmit("rpc:pacing", &contexts[9]).status ==
         ProviderLaneAdmissionStatus::kQueued);
  assert(registry.TryAdmit("rpc:pacing", &contexts[10]).status ==
         ProviderLaneAdmissionStatus::kQueued);
  ProviderLaneDispatch dispatch{};
  assert(!registry.TryTakeNext("rpc:pacing", &dispatch));
  assert(registry.Complete("rpc:pacing", first[0].dispatch.job_id,
                           ProviderLaneOutcome::kAnswered));
  assert(!registry.TryTakeNext("rpc:pacing", &dispatch));
  assert(registry.NextWakeupMs("rpc:pacing") == 50);
  clock.now_ms = 50;
  assert(registry.TryTakeNext("rpc:pacing", &dispatch));
  assert(dispatch.context == &contexts[9]);
  assert(registry.Complete("rpc:pacing", first[1].dispatch.job_id,
                           ProviderLaneOutcome::kAnswered));
  clock.now_ms = 100;
  assert(registry.TryTakeNext("rpc:pacing", &dispatch));
  assert(dispatch.context == &contexts[10]);
}

static void test_background_queue_full_timeout_and_cancel_ownership() {
  FakeClock clock;
  ProviderLaneRegistry registry(&clock);
  int contexts[50]{};
  auto foreground = registry.TryAdmit("rpc:background", &contexts[0]);
  assert(foreground.status == ProviderLaneAdmissionStatus::kDispatched);
  assert(registry.TryAdmit("rpc:background", &contexts[1], true).status ==
         ProviderLaneAdmissionStatus::kBackgroundBusy);
  assert(registry.Complete("rpc:background", foreground.dispatch.job_id,
                           ProviderLaneOutcome::kAnswered));
  auto background = registry.TryAdmit("rpc:background", &contexts[1], true);
  assert(background.status == ProviderLaneAdmissionStatus::kDispatched);
  assert(registry.Complete("rpc:background", background.dispatch.job_id,
                           ProviderLaneOutcome::kAnswered));

  ProviderLaneAdmission active[4];
  for (int i = 0; i < 4; ++i) {
    active[i] = registry.TryAdmit("alchemy-wallet", &contexts[i]);
    assert(active[i].status == ProviderLaneAdmissionStatus::kDispatched);
  }
  ProviderLaneAdmission queued[40];
  for (int i = 0; i < 40; ++i) {
    queued[i] = registry.TryAdmit("alchemy-wallet", &contexts[i + 4]);
    assert(queued[i].status == ProviderLaneAdmissionStatus::kQueued);
  }
  assert(registry.TryAdmit("alchemy-wallet", &contexts[44]).status ==
         ProviderLaneAdmissionStatus::kQueueFull);
  assert(registry.CancelQueued("alchemy-wallet", queued[0].job_id));
  ProviderLaneCancelledJob cancelled{};
  assert(registry.TryTakeCancelled(&cancelled));
  assert(cancelled.context == &contexts[4] &&
         cancelled.reason == ProviderLaneCancelReason::kCallerCancelled);
  registry.Close();
  int drained = 0;
  while (registry.TryTakeCancelled(&cancelled)) ++drained;
  assert(drained == 39);

  FakeClock timeout_clock;
  ProviderLaneRegistry timeout_registry(&timeout_clock);
  int timeout_contexts[4]{};
  auto a = timeout_registry.TryAdmit("coingecko", &timeout_contexts[0]);
  auto b = timeout_registry.TryAdmit("coingecko", &timeout_contexts[1]);
  auto c = timeout_registry.TryAdmit("coingecko", &timeout_contexts[2]);
  assert(a.status == ProviderLaneAdmissionStatus::kDispatched);
  assert(b.status == ProviderLaneAdmissionStatus::kDispatched);
  assert(c.status == ProviderLaneAdmissionStatus::kQueued);
  timeout_clock.now_ms = 3000;
  ProviderLaneDispatch dispatch{};
  assert(!timeout_registry.TryTakeNext("coingecko", &dispatch));
  assert(timeout_registry.TryTakeCancelled(&cancelled));
  assert(cancelled.context == &timeout_contexts[2] &&
         cancelled.reason == ProviderLaneCancelReason::kQueueTimeout);
}

static void test_breaker_answer_fault_rate_limit_and_half_open() {
  FakeClock clock;
  ProviderLaneRegistry registry(&clock);
  int contexts[12]{};
  ProviderLaneAdmission answers[5];
  for (int i = 0; i < 5; ++i) {
    answers[i] = registry.TryAdmit("rpc:404", &contexts[i]);
    assert(answers[i].status == ProviderLaneAdmissionStatus::kDispatched);
    complete(registry, "rpc:404", answers[i],
             ProviderLaneOutcomeForHttpStatus(404, false));
  }
  ProviderLaneStats stats{};
  assert(registry.Stats("rpc:404", &stats));
  assert(stats.breaker == ProviderBreakerState::kClosed && stats.answered == 5);

  ProviderLaneAdmission limited = registry.TryAdmit("rpc:429", &contexts[5]);
  assert(limited.status == ProviderLaneAdmissionStatus::kDispatched);
  assert(registry.Complete("rpc:429", limited.dispatch.job_id,
                           ProviderLaneOutcome::kRateLimited, 60000));
  auto rejected = registry.TryAdmit("rpc:429", &contexts[6]);
  assert(rejected.status == ProviderLaneAdmissionStatus::kBreakerOpen);
  assert(rejected.retry_after_ms == 60000);
  assert(registry.NextWakeupMs("rpc:429") == 60000);

  ProviderLaneAdmission faults[5];
  for (int i = 0; i < 5; ++i) {
    faults[i] = registry.TryAdmit("rpc:faults", &contexts[i]);
    assert(faults[i].status == ProviderLaneAdmissionStatus::kDispatched);
    const int status = i == 4 ? 403 : 401;
    complete(registry, "rpc:faults", faults[i],
             ProviderLaneOutcomeForHttpStatus(status, false));
  }
  assert(registry.Stats("rpc:faults", &stats));
  assert(stats.breaker == ProviderBreakerState::kOpen && stats.faults == 5);

  for (int i = 0; i < 5; ++i) {
    faults[i] = registry.TryAdmit("rpc:probe", &contexts[i]);
    assert(faults[i].status == ProviderLaneAdmissionStatus::kDispatched);
    assert(registry.Complete("rpc:probe", faults[i].dispatch.job_id,
                             ProviderLaneOutcome::kFault));
  }
  clock.now_ms = 30000;
  auto probe = registry.TryAdmit("rpc:probe", &contexts[5]);
  assert(probe.status == ProviderLaneAdmissionStatus::kDispatched);
  assert(probe.dispatch.half_open_probe);
  ProviderLaneAdmission waiting[4];
  for (int i = 0; i < 4; ++i) {
    waiting[i] = registry.TryAdmit("rpc:probe", &contexts[i + 6]);
    assert(waiting[i].status == ProviderLaneAdmissionStatus::kQueued);
  }
  assert(registry.Complete("rpc:probe", probe.dispatch.job_id,
                           ProviderLaneOutcome::kAnswered));
  assert(registry.Stats("rpc:probe", &stats));
  assert(stats.breaker == ProviderBreakerState::kClosed);
  ProviderLaneDispatch dispatch{};
  for (int i = 0; i < 4; ++i) {
    assert(registry.TryTakeNext("rpc:probe", &dispatch));
    assert(dispatch.context == &contexts[i + 6]);
    assert(registry.Complete("rpc:probe", dispatch.job_id,
                             ProviderLaneOutcome::kAnswered));
  }

  // Caller cancellation is a non-UpstreamError settlement in the TS lane
  // queue, so cancelling the single half-open probe counts as an answer.
  for (int i = 0; i < 5; ++i) {
    faults[i] = registry.TryAdmit("rpc:cancel-probe", &contexts[i]);
    assert(faults[i].status == ProviderLaneAdmissionStatus::kDispatched);
    assert(registry.Complete("rpc:cancel-probe", faults[i].dispatch.job_id,
                             ProviderLaneOutcome::kFault));
  }
  clock.now_ms = 60000;
  auto cancelled_probe = registry.TryAdmit("rpc:cancel-probe", &contexts[5]);
  assert(cancelled_probe.has_dispatch && cancelled_probe.dispatch.half_open_probe);
  assert(registry.Complete("rpc:cancel-probe", cancelled_probe.dispatch.job_id,
                           ProviderLaneOutcome::kCancelled));
  assert(registry.Stats("rpc:cancel-probe", &stats));
  assert(stats.breaker == ProviderBreakerState::kClosed && stats.answered == 1);
}

static void test_lane_cap_never_recycles_lane_state() {
  FakeClock clock;
  ProviderLaneRegistry registry(&clock);
  int contexts[kProviderLaneMaxActiveLanes]{};
  for (size_t i = 0; i < kProviderLaneMaxActiveLanes; ++i) {
    char name[32];
    snprintf(name, sizeof(name), "rpc:busy-%zu", i);
    auto admission = registry.TryAdmit(name, &contexts[i]);
    assert(admission.status == ProviderLaneAdmissionStatus::kDispatched);
  }
  assert(registry.active_lane_count() == kProviderLaneMaxActiveLanes);
  assert(registry.TryAdmit("rpc:capacity", &contexts[0]).status ==
         ProviderLaneAdmissionStatus::kLaneCapacity);
  assert(sizeof(registry) < 100 * 1024);
  registry.Close();
  // Active dispatch contexts remain owned by the executor and remain completable.
  for (size_t i = 0; i < kProviderLaneMaxActiveLanes; ++i) {
    char name[32];
    snprintf(name, sizeof(name), "rpc:busy-%zu", i);
    assert(registry.Complete(name, i + 1, ProviderLaneOutcome::kCancelled));
  }
}

static void test_lane_name_churn_cannot_reset_pacing_or_cooldown() {
  FakeClock clock;
  ProviderLaneRegistry paced(&clock);
  int contexts[70]{};
  ProviderLaneAdmission first[5];
  for (int i = 0; i < 5; ++i) {
    first[i] = paced.TryAdmit("rpc:paced", &contexts[i]);
    assert(first[i].status == ProviderLaneAdmissionStatus::kDispatched);
  }
  auto queued = paced.TryAdmit("rpc:paced", &contexts[5]);
  assert(queued.status == ProviderLaneAdmissionStatus::kQueued);
  for (int i = 0; i < 63; ++i) {
    char name[32];
    snprintf(name, sizeof(name), "rpc:churn-%d", i);
    auto admission = paced.TryAdmit(name, &contexts[i + 6]);
    assert(admission.status == ProviderLaneAdmissionStatus::kDispatched);
    assert(paced.Complete(name, admission.dispatch.job_id, ProviderLaneOutcome::kAnswered));
  }
  assert(paced.active_lane_count() == 64);
  assert(paced.TryAdmit("rpc:paced", &contexts[69]).status ==
         ProviderLaneAdmissionStatus::kQueued);
  assert(paced.TryAdmit("rpc:one-too-many", &contexts[68]).status ==
         ProviderLaneAdmissionStatus::kLaneCapacity);

  FakeClock breaker_clock;
  ProviderLaneRegistry breaker(&breaker_clock);
  int breaker_contexts[64]{};
  auto limited = breaker.TryAdmit("rpc:protected", &breaker_contexts[0]);
  assert(limited.status == ProviderLaneAdmissionStatus::kDispatched);
  assert(breaker.Complete("rpc:protected", limited.dispatch.job_id,
                          ProviderLaneOutcome::kRateLimited, 60000));
  for (int i = 0; i < 63; ++i) {
    char name[32];
    snprintf(name, sizeof(name), "rpc:idle-%d", i);
    auto admission = breaker.TryAdmit(name, &breaker_contexts[i + 1]);
    assert(admission.status == ProviderLaneAdmissionStatus::kDispatched);
    assert(breaker.Complete(name, admission.dispatch.job_id,
                            ProviderLaneOutcome::kAnswered));
  }
  auto still_open = breaker.TryAdmit("rpc:protected", &breaker_contexts[0]);
  assert(still_open.status == ProviderLaneAdmissionStatus::kBreakerOpen);
  assert(still_open.retry_after_ms == 60000);
}

static void test_keyless_coingecko_pacing_and_background_recovery() {
  FakeClock clock;
  ProviderLaneRegistry registry(&clock, true);
  const auto first = registry.TryAdmit("coingecko", nullptr, true);
  complete(registry, "coingecko", first, ProviderLaneOutcome::kAnswered);
  clock.now_ms = 11999;
  assert(registry.TryAdmit("coingecko", nullptr, true).status ==
         ProviderLaneAdmissionStatus::kBackgroundBusy);
  assert(registry.TryAdmit("coingecko", nullptr).status ==
         ProviderLaneAdmissionStatus::kQueueFull);
  clock.now_ms = 12001;
  const auto limited = registry.TryAdmit("coingecko", nullptr, true);
  complete(registry, "coingecko", limited, ProviderLaneOutcome::kRateLimited);
  clock.now_ms += 119999;
  const auto waiting = registry.TryAdmit("coingecko", nullptr, true);
  assert(waiting.status == ProviderLaneAdmissionStatus::kBreakerOpen);
  assert(waiting.retry_after_ms == 1);
  ++clock.now_ms;
  const auto probe = registry.TryAdmit("coingecko", nullptr, true);
  assert(probe.has_dispatch);
  assert(registry.TryAdmit("coingecko", nullptr, true).status ==
         ProviderLaneAdmissionStatus::kBackgroundBusy);
  complete(registry, "coingecko", probe, ProviderLaneOutcome::kCancelled);
  clock.now_ms += 12001;
  const auto next_probe = registry.TryAdmit("coingecko", nullptr, true);
  assert(next_probe.has_dispatch && next_probe.dispatch.half_open_probe);
  complete(registry, "coingecko", next_probe, ProviderLaneOutcome::kAnswered);
}

int main() {
  test_budgets_and_status_mapping();
  test_token_pacing_concurrency_and_fifo();
  test_background_queue_full_timeout_and_cancel_ownership();
  test_breaker_answer_fault_rate_limit_and_half_open();
  test_lane_cap_never_recycles_lane_state();
  test_lane_name_churn_cannot_reset_pacing_or_cooldown();
  test_keyless_coingecko_pacing_and_background_recovery();
  puts("provider lane tests passed");
  return 0;
}
