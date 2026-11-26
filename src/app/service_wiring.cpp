#include <utility>
#include <memory>
#include "service_wiring.h"
#include "service_composition.h"
#include "log.h"
#include "ownership.h"

#include "kv_runtime.h"
#include "module_configs.h"
#include "native_config.h"
#include "price_jobs.h"
#include "price_series_store.h"
#include "composition_hooks.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace fortis::app {
namespace {

// TypeScript timer 3, second half: `priceSeries.sealDueYielding(Date.now(),
// 1_024)` after the expiry sweep, 128 rows per frame, yielding between
// frames and stopping with the sweeper.
bool price_seal_stopping(void* opaque) {
  return static_cast<const storage::KvExpirySweeper*>(opaque)->stopping();
}
size_t price_seal_tick(void*, fortis::storage::KvStore& kv, int64_t now_ms, const storage::KvExpirySweeper& sweeper) {
  fortis::prices::PriceSeriesStore series(kv);
  const auto result = fortis::tokens::price_jobs::SealDueYielding(
      series, now_ms, fortis::tokens::price_jobs::kTimerSealLimit, fortis::tokens::price_jobs::kSealBatch, price_seal_stopping,
      const_cast<storage::KvExpirySweeper*>(&sweeper));
  if (result.failed) fortis::base::LogError("app", "embedded-store expiry sweep failed: price seal");
  return result.sealed;
}

// Migrate only price records, preserving the original retention deadlines.
// No request workers or sweepers are running. A partial copy is restartable;
// existing destination rows win and the marker is written last.
bool open_split_prices(const config::NativeConfig& config, fortis::storage::KvStore& source,
                       fortis::storage::KvStore& destination) {
  char path[512];
  const int length = snprintf(path, sizeof(path), "%s/prices-v1", config.kv_dir);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(path) ||
      (mkdir(path, 0700) != 0 && errno != EEXIST)) return false;
  const int directory = open(config.kv_dir, O_RDONLY | O_DIRECTORY);
  if (directory < 0) return false;
  const bool synced = fsync(directory) == 0;
  const bool closed = close(directory) == 0;
  if (!synced || !closed || !destination.Open(path)) return false;
  constexpr char migration_key[] = "migration/price-store/v1";
  constexpr char marker = '1';
  const int64_t now = static_cast<int64_t>(time(nullptr)) * 1000;
  if (now <= 0) return false;
  char value = 0;
  size_t bytes = 0;
  const auto state = destination.GetStatus(migration_key, now, &value, 1, &bytes);
  if (state == fortis::storage::KvReadStatus::Found) return bytes == 1 && value == marker;
  if (state != fortis::storage::KvReadStatus::Missing) return false;

  struct Copy {
    fortis::storage::KvStore& destination;
    int64_t now;
    fortis::storage::KvBatch batch{fortis::storage::KvBatch::kMaxFrameBytes, 64};
  } copy{destination, now};
  fortis::storage::KvPageResult page;
  do {
    if (!source.Page("price/", page.next_cursor, 64, now, false, nullptr, &page, [opaque = &copy](const char* key, const void* data, size_t size, int64_t expiry) {
            auto& copy = *static_cast<Copy*>(opaque);
            size_t length = 0;
            const auto existing = copy.destination.GetStatus(key, copy.now, nullptr, 0, &length);
            if (existing == fortis::storage::KvReadStatus::Found ||
                existing == fortis::storage::KvReadStatus::BufferTooSmall) return true;
            if (existing != fortis::storage::KvReadStatus::Missing) return false;
            if (copy.batch.Put(key, data, size, expiry)) return true;
            return copy.batch.full() && copy.batch.Commit(&copy.destination) &&
                   copy.batch.Put(key, data, size, expiry);
          }) || !copy.batch.Commit(&destination)) return false;
  } while (!page.done);
  const fortis::storage::KvMutation complete{migration_key, &marker, 1, 0, false};
  return destination.Write(&complete, 1);
}

// The planes behind the provider coordinator: spot, history, discovery,
// news, token and address, with the shared KV store (and the separate price
// store) and the coin popularity tracker.
bool start_market_planes(const config::NativeConfig& config, const config::ProviderRuntimeConfig& provider_config, PlaneOptions& options, Planes& planes,
                         CompositionHooks& hooks) {
  bool keyless_coingecko = !config.coingecko_api_key_configured;
  if (options.coingecko_loopback) keyless_coingecko = false;
  if (!planes.provider_coordinator.Start(keyless_coingecko, options.errors).ok()) {
    fortis::base::LogError("app", "Failed to start the provider execution coordinator");
    return false;
  }
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
  if (!kv) {
    fortis::base::LogError("app", "Failed to open the native price route KV");
    return false;
  }
  if (config.split_price_store) {
    if (!open_split_prices(config, *kv, planes.split_prices)) {
      fortis::base::LogError("app", "Failed to open or seed the separate price store");
      return false;
    }
    planes.price_kv = &planes.split_prices;
  }
  fortis::storage::KvStore* price_kv = planes.price_kv;
  auto& prices = price_kv ? *price_kv : *kv;
  if (!hooks.StoreOpened(kv)) return false;
  // Coin popularity: the foreground price and market
  // reads record into it and the token scheduler refreshes from it.
  if (options.price || options.discovery || options.token) {
    if (!planes.coin_popularity.Create(options.popularity_clock).ok()) {
      fortis::base::LogError("app", "Failed to allocate the coin popularity tracker");
      return false;
    }
  }
  providers::ProviderExecutionCoordinator& coordinator = *planes.provider_coordinator.get();
  if (options.price) {
    fortis::prices::PriceSpotServiceDeps deps{prices, coordinator};
    deps.popularity = planes.coin_popularity.get();
    deps.wall_clock = options.price_owner_clock;
    deps.errors = options.errors;
    if (!detail::StartService(planes.price, "Failed to start the native price provider owner", [&] {
          return fortis::prices::PriceSpotService::Create(provider_config, deps, {options.price_owner});
        })) return false;
  }
  if (options.history) {
    const auto status = detail::EmplaceService(planes.history, [&] {
      return fortis::prices::PriceHistoryService::Create(provider_config, {prices, coordinator},
                                                        {options.history_owners, options.history_seams});
    });
    if (!status.ok()) {
      fortis::base::LogError("app", "Failed to start the native price history %s", status.message());
      return false;
    }
  }
  if (options.discovery) {
    fortis::discovery::DiscoveryServiceDeps deps{*kv, coordinator};
    deps.popularity = planes.coin_popularity.get();
    if (!detail::StartService(planes.discovery, "Failed to start the native discovery service", [&] {
          return fortis::discovery::DiscoveryService::Create(provider_config, deps, {options.discovery_owner, options.discovery_seams});
        })) return false;
  }
  if (options.news) {
    if (!detail::StartService(planes.news, "Failed to start the native news service", [&] {
          return fortis::news::NewsService::Create(provider_config, {*kv, coordinator}, {options.news_owner, options.news_seams});
        })) return false;
  }
  if (options.token) {
    fortis::tokens::TokenServiceDeps deps{*kv, coordinator, prices};
    deps.spot = planes.price ? planes.price->route_context()->owner : nullptr;
    deps.history = planes.history ? &*planes.history : nullptr;
    deps.popularity = planes.coin_popularity.get();
    deps.price_cache = price_kv && planes.price ? planes.price->cache() : nullptr;
    if (!detail::StartService(planes.token, "Failed to start the native token service", [&] {
          return fortis::tokens::TokenService::Create(provider_config, deps, {options.token_owner, options.token_seams});
        })) return false;
  }
  planes.comparison_context.stats = planes.token ? planes.token->stats() : nullptr;
  if (options.address) {
    fortis::wallet::AddressServiceDeps deps{*kv, coordinator};
    deps.spot = planes.price ? planes.price->route_context()->owner : nullptr;
    deps.history = planes.history ? &*planes.history : nullptr;
    if (!detail::StartService(planes.address, "Failed to start the native address service", [&] {
          return fortis::wallet::AddressService::Create(provider_config, deps, {options.address_transport, options.address_seams});
        })) return false;
  }
  planes.comparison_context.kv = kv;
  return hooks.MarketPlanesStarted(kv);
}

}  // namespace

fortis::base::Status ProviderCoordinatorSlot::Start(bool keyless_coingecko, base::ErrorReporter* errors) {
  if (coordinator_) return fortis::base::Status::Ok();
  // A candidate that fails to start is destroyed on the way out.
  std::unique_ptr<providers::ProviderExecutionCoordinator, fortis::base::ObjectDeleter<providers::ProviderExecutionCoordinator>> candidate(
      fortis::base::New<providers::ProviderExecutionCoordinator>(keyless_coingecko, errors));
  if (!candidate) return fortis::base::Status(fortis::base::StatusCode::kResourceExhausted, "coordinator allocation");
  if (!candidate->Start()) return fortis::base::Status(fortis::base::StatusCode::kUnavailable, "coordinator start");
  coordinator_ = std::move(candidate);
  return fortis::base::Status::Ok();
}

void ProviderCoordinatorSlot::Stop() {
  if (!coordinator_ || stop_refused_) return;
  if (!coordinator_->Stop()) {
    stop_refused_ = true;
    fortis::base::LogError("app", "Failed to stop the provider execution coordinator");
    return;
  }
  coordinator_.reset();
}

fortis::base::Status CoinPopularitySlot::Create(fortis::base::Clock* clock) {
  fortis::discovery::CoinPopularityOptions options;
  options.clock = clock;
  // One tracker per process; services hold it.
  if (value_) return fortis::base::Status(fortis::base::StatusCode::kAlreadyExists, "coin popularity tracker");
  auto created = fortis::discovery::CoinPopularity::Create(options);
  if (!created.ok()) return created.status();
  value_ = created.TakeValue();
  return fortis::base::Status::Ok();
}

PlaneOptions PlaneOptionsFromConfig(const config::NativeConfig& config) {
  PlaneOptions options;
  options.price = config.native_price_route_canary;
  options.history = config.native_price_route_canary;
  options.discovery = config.native_price_route_canary;
  options.news = config.native_news_canary;
  options.token = config.native_price_route_canary;
  options.address = config.native_address_canary;
  options.cross_chain = config.native_cross_chain_canary;
  options.robinhood = config.native_robinhood_canary;
  options.dashpay = config.native_dashpay_canary;
  options.ai = config.native_ai_canary;
  return options;
}

void Planes::StopOwners() {
  // The AI service owns no provider lease; it stops with the owners so its
  // queued requests are answered before the handoff stops.
  ai.reset();
  // The cross-chain workers finish their queued tasks on their own
  // coordinator lease before it stops.
  cross_chain.reset();
  // The Robinhood workers finish on their own coordinator lease, too.
  robinhood.reset();
  // So do the DashPay lookup workers.
  dashpay.reset();
  // The address and token services read the spot owner (and the address
  // service the history owner): they stop first.
  address.reset();
  token.reset();
  if (news) news->Stop();
  if (discovery) discovery->Stop();
  if (history) history->Stop();
  if (price) price->Stop();
}

bool ModuleComposition::Start() {
  const auto& config = config_;
  const auto& modules = modules_;
  auto& options = options_;
  auto& planes = owners_;
  auto& hooks = hooks_;
  planes.comparison_context.attestation_required = config.attestation_required;
  planes.comparison_context.clock = options.comparison_clock;
  if ((options.price || options.history || options.discovery || options.news || options.token ||
       options.address || options.cross_chain || options.robinhood || options.dashpay) &&
      !start_market_planes(config, modules.providers, options, planes, hooks))
    return false;
  // AI conversations ride FORTIS_NATIVE_AI_CANARY=1 (CA
  // bundle validated at startup; SUPPORT_AUTH_SECRET required). They hold no
  // provider-coordinator lease: the model is called by the service's own
  // bounded workers.
  if (!hooks.ConfigureAiPlane(config, options)) return false;
  if (options.ai) {
    fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
    if (!detail::StartService(planes.ai, "Failed to start the native AI conversation service", [&] {
          return ai::AiConversationService::Create(modules.ai, {*kv, options.errors}, options.ai_seams);
        }, kv != nullptr)) return false;
    if (!hooks.AiStarted(kv)) return false;
  }
  // The cross-chain exchange, Robinhood and the DashPay resolver each run on
  // their own transport lease of the provider coordinator.
  if (options.cross_chain) {
    fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
    if (!detail::StartService(planes.cross_chain, "Failed to start the native cross-chain service", [&] {
          return exchange::CrossChainService::Create(modules.cross_chain, {*planes.provider_coordinator.get()}, options.cross_chain_seams);
        }, kv != nullptr)) return false;
    if (!hooks.CrossChainStarted(kv)) return false;
  }
  if (options.robinhood) {
    fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
    if (!detail::StartService(planes.robinhood, "Failed to start the native Robinhood service", [&] {
          return exchange::RobinhoodService::Create(modules.robinhood, {*planes.provider_coordinator.get(), kv}, options.robinhood_seams);
        }, kv != nullptr)) return false;
    if (!hooks.RobinhoodStarted(kv)) return false;
  }
  if (options.dashpay) {
    if (!detail::StartService(planes.dashpay, "Failed to start the native DashPay service", [&] {
          return exchange::DashPayService::Create(modules.dashpay, {*planes.provider_coordinator.get()}, options.dashpay_seams);
        })) return false;
    if (!hooks.DashPayStarted()) return false;
  }
  return true;
}

bool StartPlanes(const config::NativeConfig& config, const ModuleConfigs& modules,
                 PlaneOptions& options, Planes& planes, CompositionHooks& hooks) {
  return ModuleComposition(config, modules, options, planes, hooks).Start();
}

bool ModuleComposition::RegisterStartedRoutes(http::HttpRouter& router, http::HttpResponseHandoff& handoff,
    ModuleOwners& planes, CompositionHooks& hooks) {

  return detail::RegisterServices(router, handoff, planes.price, planes.history, planes.discovery) &&
      (!planes.discovery || RegisterTokenComparisonRoute(router, &planes.comparison_context)) &&
      detail::RegisterServices(router, handoff, planes.news, planes.token, planes.address, planes.ai,
                               planes.cross_chain, planes.robinhood, planes.dashpay) &&
      hooks.RegisterPlaneRoutes(router, planes);
}

bool ModuleComposition::RegisterRoutes(http::HttpRouter& router, http::HttpResponseHandoff& handoff) {
  return RegisterStartedRoutes(router, handoff, owners_, hooks_);
}

bool RegisterPlaneRoutes(http::HttpRouter& router, http::HttpResponseHandoff& handoff, Planes& planes,
                         CompositionHooks& hooks) {
  // Registration depends only on started owners and composition hooks.
  return ModuleComposition::RegisterStartedRoutes(router, handoff, planes, hooks);
}

bool StartSweepers(Planes& planes, uint32_t interval_ms, fortis::base::Clock* clock) {
  if (planes.price.has_value())
    (planes.price_kv ? planes.price_sweeper : planes.kv_sweeper).SetMaintenance(price_seal_tick, nullptr);
  return planes.kv_sweeper.Start(nullptr, interval_ms, clock) &&
         (!planes.price_kv || planes.price_sweeper.Start(planes.price_kv, interval_ms, clock));
}

void StopSweepers(Planes& planes) {
  planes.kv_sweeper.Stop();
  planes.price_sweeper.Stop();
}

bool ModuleComposition::StartSweepers(uint32_t interval_ms, base::Clock* clock) {
  return app::StartSweepers(owners_, interval_ms, clock);
}
void ModuleComposition::StopSweepers() { app::StopSweepers(owners_); }

}  // namespace fortis::app
