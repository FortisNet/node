#include <memory>
#include <string_view>
#include "price_spot_service.h"
#include "clock.h"
#include "ownership.h"

#include "price_series_store.h"
#include "price_spot_cache.h"
#include "price_id_parser.h"
#include "price_snapshot.h"
#include "price_core_ids.h"
#include "coin_popularity.h"
#include "threading.h"
#include "provider_execution_coordinator.h"

#include <time.h>

namespace fortis::prices {

struct PriceSpotService::Impl {
  Impl(const config::ProviderRuntimeConfig& config, fortis::storage::KvStore& kv,
       providers::ProviderExecutionCoordinator& coordinator)
      : series(kv), cache(kv), owner(cache, series, config, &coordinator),
        publisher(cache, snapshot_view), snapshot_route(snapshot_view),
        snapshot_enabled(config.price_snapshot_enabled) {
    route.owner = &owner;
    route.attestation_required = config.attestation_required;
    route.shared_price_auth = config.split_price_store;
    if (snapshot_enabled) route.fast_cache = &cache;
  }
  // Declaration order is construction order: the owner borrows both stores.
  fortis::prices::PriceSeriesStore series;
  fortis::prices::PriceSpotCache cache;
  SpotProviderOwner owner;
  PriceSpotRouteContext route{};
  PriceSnapshotView snapshot_view;
  PriceSnapshotPublisher publisher;
  PriceSnapshotRoute snapshot_route;
  bool snapshot_enabled;
  std::unique_ptr<fortis::discovery::CoinPopularity, fortis::discovery::CoinPopularityDeleter> contracts;
  fortis::base::Thread warm_thread;
  fortis::base::Mutex warm_mutex;
  fortis::base::ConditionVariable warm_changed;
  bool warm_started = false;
  bool warm_stopping = false;
  bool warm_attempted = false;

  ~Impl() {
    StopWarm();
    contracts.reset();


  }

  // A 30 s spot entry must leave room for a scheduler tick and for
  // multiple network batches to pass through the shared provider lane.
  static constexpr uint32_t kWarmTickMs = 5000;
  static constexpr int64_t kContractRefreshLeadMs = 20000;

  void WarmCore() {
    std::string_view ids[kPriceCoreCount];
    for (size_t i = 0; i < kPriceCoreCount; ++i) ids[i] = kPriceCoreIds[i];
    // The shared owner retains the original bounded lane, fallback, deadline
    // and shutdown rules. Refresh before expiry without extending old quotes.
    (void)owner.Submit(ids, kPriceCoreCount, "usd", SpotProviderOwnerFormat::kJson,
        base::SystemClock().MonotonicMs() + 15000, [](void*, const SpotProviderOwnerResult&) {},
        nullptr, SpotSubmitMode::kBackground, nullptr, kContractRefreshLeadMs);
  }

  void WarmContracts() {
    if (!contracts) return;
    fortis::discovery::RankedCoinList ranked;
    if (!contracts->Due(6, &ranked, 6, 120000)) return;
    std::string_view ids[6];
    size_t count = 0;
    for (size_t i = 0; i < ranked.count; ++i) {
      const auto& row = ranked.items[i];
      providers::ParsedPriceId parsed{};
      if (!ParsePriceId(row.id, row.id_length, &parsed)) continue;
      ids[count++] = {row.id, row.id_length};
    }
    if (count) (void)owner.Submit(ids, count, {"usd", 3},
          SpotProviderOwnerFormat::kJson, base::SystemClock().MonotonicMs() + 15000,
          [](void*, const SpotProviderOwnerResult&) {}, nullptr, SpotSubmitMode::kBackground,
          nullptr, kContractRefreshLeadMs);
    ranked.Release();
  }

  static void WarmLoop(Impl* impl) {
    auto& self = *impl;
    if (self.snapshot_enabled) self.WarmCore();
    fortis::base::UniqueLock warm_lock(self.warm_mutex);
    self.warm_attempted = true;
    self.warm_changed.Broadcast();
    while (!self.warm_stopping) {
      timespec wake{};
      clock_gettime(CLOCK_REALTIME, &wake);
      const uint32_t delay_ms = kWarmTickMs;
      wake.tv_sec += delay_ms / 1000;
      wake.tv_nsec += static_cast<long>(delay_ms % 1000) * 1000000;
      if (wake.tv_nsec >= 1000000000) {
        ++wake.tv_sec;
        wake.tv_nsec -= 1000000000;
      }
      self.warm_changed.WaitUntilLocked(self.warm_mutex, wake);
      if (self.warm_stopping) break;
      warm_lock.Unlock();
      if (self.snapshot_enabled) self.WarmCore();
      self.WarmContracts();
      warm_lock.Lock();
    }
  }

  bool StartWarm() {
    warm_started = warm_thread.Start(WarmLoop, this, 256 * 1024);
    if (warm_started) {
      timespec deadline{};
      clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_sec += 5;
      fortis::base::LockGuard warm_lock(warm_mutex);
      while (!warm_attempted &&
             warm_changed.WaitUntilLocked(warm_mutex, deadline) == 0) {}
    }
    return warm_started;
  }

  void StopWarm() {
    if (warm_started) {
      fortis::base::UniqueLock warm_lock(warm_mutex);
      warm_stopping = true;
      warm_changed.Signal();
      warm_lock.Unlock();
      warm_thread.Join();
      warm_started = false;
    }
  }
};

void PriceSpotService::ImplDeleter::operator()(Impl* impl) const { fortis::base::Delete(impl); }

PriceSpotService::~PriceSpotService() {
  if (!impl_) return;
  impl_->publisher.Stop();
  impl_->StopWarm();
  impl_->owner.Stop();
  impl_.reset();
}

PriceSpotService::PriceSpotService(Impl* impl) : impl_(impl) {}
PriceSpotService::PriceSpotService(PriceSpotService&&) noexcept = default;

base::Result<PriceSpotService> PriceSpotService::Create(
    const config::ProviderRuntimeConfig& config, PriceSpotServiceDeps deps, const PriceSpotServiceOptions& options) try {
  auto& kv = deps.kv;
  auto& coordinator = deps.coordinator;
  auto* start_owner = options.start_owner;
  auto* owner_clock = deps.wall_clock;
  if (!coordinator.valid()) return base::Status(base::StatusCode::kUnavailable, "price initialization");
  // Destroyed on every early return below.
  Impl* impl = base::New<Impl>(config, kv, coordinator);
  if (!impl) return base::Status(base::StatusCode::kResourceExhausted, "price allocation");
  PriceSpotService service(impl);
  impl->owner.SetCoinPopularity(deps.popularity);
  impl->owner.SetErrorReporter(deps.errors);
  impl->route.errors = deps.errors;
  if (config.split_price_store && !impl->cache.EnableMemory()) return base::Status(base::StatusCode::kUnavailable, "price initialization");
  if (config.split_price_store) {
    fortis::discovery::CoinPopularityOptions limits;
    limits.preserve_case = true;
    limits.max_coins = 1024;
    limits.max_seen_per_day = 16384;
    auto contracts = fortis::discovery::CoinPopularity::Create(limits);
    if (!contracts.ok()) return base::Status(base::StatusCode::kUnavailable, "price initialization");
    impl->contracts = contracts.TakeValue();
    impl->owner.SetContractPopularity(impl->contracts.get());
    impl->route.fast_cache = &impl->cache;
  }
  if (owner_clock) impl->owner.SetClock(owner_clock);
  const auto started = impl->owner.Initialize(start_owner);
  if (!started.ok()) {
    // A failed Start has already released any coordinator lease it took.
    return base::Status(base::StatusCode::kUnavailable, "price initialization");
  }
  if (((impl->snapshot_enabled || impl->contracts) && !impl->StartWarm()) ||
      (impl->snapshot_enabled && !impl->publisher.Start())) {
    impl->StopWarm(); impl->owner.Stop(); return base::Status(base::StatusCode::kUnavailable, "price initialization");
  }
  return service;
} catch (const std::bad_alloc&) {
  return base::Status(base::StatusCode::kResourceExhausted, "price allocation");
}

bool PriceSpotService::Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff) {
  if (impl_->route.handoff) return false;
  impl_->route.handoff = &handoff;
  return RegisterPriceSpotRoute(router, &impl_->route) &&
      (!impl_->snapshot_enabled || impl_->snapshot_route.Register(router, impl_->route.attestation_required));
}

void PriceSpotService::Stop() {
  impl_->publisher.Stop(); impl_->StopWarm(); impl_->owner.Stop();
}

PriceSpotRouteContext* PriceSpotService::route_context() {
  return &impl_->route;
}


PriceSnapshotView* PriceSpotService::snapshot_view() {
  return impl_->snapshot_enabled ? &impl_->snapshot_view : nullptr;
}

fortis::prices::PriceSpotCache* PriceSpotService::cache() {
  return &impl_->cache;
}

}  // namespace fortis::prices
