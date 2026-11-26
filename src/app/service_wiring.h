#pragma once

// Per-plane service wiring: which planes a process composes, the objects
// that own them, their start order and their route registration.

#include <memory>
#include "ai_service.h"
#include "ownership.h"
#include "status.h"
#include "address_service.h"
#include "coin_popularity.h"
#include "cross_chain_service.h"
#include "dashpay_service.h"
#include "discovery_service.h"
#include "kv_expiry_sweeper.h"
#include "kv_store.h"
#include "news_service.h"
#include "price_history_service.h"
#include "price_spot_service.h"
#include "provider_execution_coordinator.h"
#include "robinhood_service.h"
#include "token_comparison_route.h"
#include "token_service.h"

#include <stdint.h>
#include <optional>

namespace fortis::config { struct NativeConfig; }

namespace fortis::app {

class CompositionHooks;
struct ModuleConfigs;

// The process-lifetime provider coordinator. It is constructed only when a
// provider-backed route is enabled, so a disabled image starts no coordinator
// thread or HTTPS workers. Every owner lease must be released before Stop.
class ProviderCoordinatorSlot {
 public:
  ProviderCoordinatorSlot() = default;
  ~ProviderCoordinatorSlot() {
    Stop();
    // After a refused Stop the coordinator is still held: leave it allocated
    // (see Stop) instead of letting the handle destroy it.
    if (coordinator_) (void)coordinator_.release();  // NOLINT(bugprone-unused-return-value): refused Stop deliberately retains the live coordinator; destruction would invalidate active leases.
  }
  ProviderCoordinatorSlot(const ProviderCoordinatorSlot&) = delete;
  ProviderCoordinatorSlot& operator=(const ProviderCoordinatorSlot&) = delete;

  providers::ProviderExecutionCoordinator* get() const { return coordinator_.get(); }
  // Ok when a coordinator is running (also when it already was).
  fortis::base::Status Start(bool keyless_coingecko, base::ErrorReporter* errors = nullptr);
  // A refused Stop means an owner lease is still live. Keep the running
  // coordinator allocated rather than destroying it beneath that owner.
  void Stop();

 private:
  std::unique_ptr<providers::ProviderExecutionCoordinator, fortis::base::ObjectDeleter<providers::ProviderExecutionCoordinator>> coordinator_;
  bool stop_refused_ = false;
};

// The process coin popularity tracker (TypeScript's one CoinPopularity,
// shared by PricesFacet, TokenDiscoveryFacet and CoinPriceScheduler). It is
// created only with the price plane and outlives every service that holds it.
class CoinPopularitySlot {
 public:
  CoinPopularitySlot() = default;
  ~CoinPopularitySlot() = default;
  CoinPopularitySlot(const CoinPopularitySlot&) = delete;
  CoinPopularitySlot& operator=(const CoinPopularitySlot&) = delete;
  // kAlreadyExists on a second call: there is one tracker per process.
  fortis::base::Status Create(fortis::base::Clock* clock);
  fortis::discovery::CoinPopularity* get() const { return value_.get(); }

 private:
  std::unique_ptr<fortis::discovery::CoinPopularity, fortis::discovery::CoinPopularityDeleter> value_;
};

// Which planes start and with which owner starters and seams. A normal image
// takes the canary switches of its configuration and no starters or seams
// (every owner uses its production Start()); another composition substitutes
// them through CompositionHooks::ConfigurePlanes (composition_hooks.h).
struct PlaneOptions {
  bool price = false;
  bool history = false;
  bool discovery = false;
  bool news = false;
  bool token = false;
  bool address = false;
  bool cross_chain = false;
  bool robinhood = false;
  bool dashpay = false;
  bool ai = false;
  // Loopback fixture servers do not share CoinGecko's anonymous quota.
  bool coingecko_loopback = false;
  prices::PriceSpotOwnerStarter* price_owner = nullptr;
  // The spot owner's wall clock (null: the system clock).
  fortis::base::Clock* price_owner_clock = nullptr;
  prices::PriceHistoryOwnersStarter* history_owners = nullptr;
  const prices::PriceHistoryRouteSeams* history_seams = nullptr;
  discovery::DiscoveryOwnerStarter* discovery_owner = nullptr;
  const discovery::DiscoveryCacheSeams* discovery_seams = nullptr;
  news::NewsOwnerStarter* news_owner = nullptr;
  const news::NewsServiceSeams* news_seams = nullptr;
  tokens::TokenOwnerStarter* token_owner = nullptr;
  const tokens::TokenServiceSeams* token_seams = nullptr;
  wallet::AddressTransportStarter* address_transport = nullptr;
  const wallet::AddressServiceSeams* address_seams = nullptr;
  const ai::AiServiceSeams* ai_seams = nullptr;
  const exchange::CrossChainServiceSeams* cross_chain_seams = nullptr;
  const exchange::RobinhoodServiceSeams* robinhood_seams = nullptr;
  const exchange::DashPayServiceSeams* dashpay_seams = nullptr;
  // The coin popularity tracker's and the comparison route's clocks.
  fortis::base::Clock* popularity_clock = nullptr;
  fortis::base::Clock* comparison_clock = nullptr;
  base::ErrorReporter* errors = nullptr;
};

// The planes a normal image composes: the explicit canary opt-ins.
// FORTIS_NATIVE_PRICE_ROUTE_CANARY=1 composes the spot, history, discovery
// and token planes; news, address, AI, cross-chain, Robinhood and DashPay
// ride their own switches. LoadNativeConfig accepts each only with a CA bundle.
PlaneOptions PlaneOptionsFromConfig(const config::NativeConfig& config);

// Every plane's owner objects, the stores and their sweepers. Member order
// is the destruction order main() relied on: services first, then the
// coordinator they lease from, the popularity tracker, the sweepers and the
// separate price store.
struct ModuleOwners {
  ModuleOwners() = default;
  ModuleOwners(const ModuleOwners&) = delete;
  ModuleOwners& operator=(const ModuleOwners&) = delete;
  fortis::storage::KvStore split_prices;
  // The separate price store when FORTIS_SPLIT_PRICE_STORE=1 opened it.
  fortis::storage::KvStore* price_kv = nullptr;
  storage::KvExpirySweeper kv_sweeper;
  storage::KvExpirySweeper price_sweeper;
  CoinPopularitySlot coin_popularity;
  ProviderCoordinatorSlot provider_coordinator;
  std::optional<prices::PriceSpotService> price;
  std::optional<prices::PriceHistoryService> history;
  std::optional<discovery::DiscoveryService> discovery;
  std::optional<news::NewsService> news;
  std::optional<tokens::TokenService> token;
  std::optional<wallet::AddressService> address;
  std::optional<ai::AiConversationService> ai;
  std::optional<exchange::CrossChainService> cross_chain;
  std::optional<exchange::RobinhoodService> robinhood;
  std::optional<exchange::DashPayService> dashpay;
  tokens::TokenComparisonRouteContext comparison_context{};

  // Owners stop before the coordinator they lease from, and both before the
  // response handoff that delivers their pending completions. The history
  // service stops its batch driver before its backfill owner.
  void StopOwners();
  void StopCoordinator() { provider_coordinator.Stop(); }
};

using Planes = ModuleOwners;

// Per-application composition workflow. Configuration/options/hooks and owners
// are borrowed for this synchronous startup/registration lifetime.
class ModuleComposition {
 public:
  ModuleComposition(const config::NativeConfig& config, const ModuleConfigs& modules,
      PlaneOptions& options, ModuleOwners& owners, CompositionHooks& hooks)
      : config_(config), modules_(modules), options_(options), owners_(owners), hooks_(hooks) {}
  bool Start();
  bool RegisterRoutes(http::HttpRouter& router, http::HttpResponseHandoff& handoff);
  static bool RegisterStartedRoutes(http::HttpRouter& router, http::HttpResponseHandoff& handoff,
      ModuleOwners& planes, CompositionHooks& hooks);
  bool StartSweepers(uint32_t interval_ms, base::Clock* clock);
  void StopSweepers();
 private:
  const config::NativeConfig& config_;
  const ModuleConfigs& modules_;
  PlaneOptions& options_;
  ModuleOwners& owners_;
  CompositionHooks& hooks_;
};

// Starts every enabled plane before the listener binds: the coordinator,
// the KV store (and the separate price store), the market planes, then AI,
// cross-chain, Robinhood and DashPay, calling `hooks` as each store and
// plane comes up. False after printing why; the caller stops the
// owners and the coordinator and exits 2, never serving a partial surface.
bool StartPlanes(const config::NativeConfig& config, const ModuleConfigs& modules,
                 PlaneOptions& options, Planes& planes, CompositionHooks& hooks);

// Registers every started plane's routes, then the hooks' routes (all
// register, or the process exits before any worker starts).
bool RegisterPlaneRoutes(http::HttpRouter& router, http::HttpResponseHandoff& handoff, Planes& planes,
                         CompositionHooks& hooks);

// The 10 s timer's yielding price seal rides each sweep tick whenever the
// price plane is composed, then both sweepers start. The hook builds its
// series view over the sweeper's store per tick, so it holds nothing the
// route owners free.
bool StartSweepers(Planes& planes, uint32_t interval_ms, fortis::base::Clock* clock);
void StopSweepers(Planes& planes);

}  // namespace fortis::app
