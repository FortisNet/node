// The entry point of the replay server binaries (build/fortis_node_*_test
// and build/fortis_node_probe_test): the production composition
// (src/app/server.cpp) with the test hooks below. Each binary compiles this
// file and the test_probes_*.cpp units with its FORTIS_*_TEST_ONLY flags; the
// production binary and the guest image link src/app/main.cpp instead and
// contain none of this.

#include "composition_hooks.h"
#include "../env_clock.h"
#include "native_config.h"
#include "price_history_backfill_owner.h"
#include "provider_http_bridge.h"
#include "service_wiring.h"
#include "test_hook_env.h"
#include "test_probe_routes.h"
#include "test_probes.h"

#include <stdio.h>

using namespace fortis::app;
using namespace fortis::config;
using namespace fortis::prices;

// Every hook runs its planes in the order the composition ran them inline,
// so a binary with several test flags reports the same first refusal.
namespace fortis::app {
namespace {

#if defined(FORTIS_PRICE_ROUTE_TEST_ONLY) || defined(FORTIS_HISTORY_ROUTE_TEST_ONLY) || \
    defined(FORTIS_DISCOVERY_ROUTE_TEST_ONLY) || defined(FORTIS_NEWS_ROUTE_TEST_ONLY) || \
    defined(FORTIS_TOKEN_ROUTE_TEST_ONLY) || defined(FORTIS_ADDRESS_ROUTE_TEST_ONLY) || \
    defined(FORTIS_CROSS_CHAIN_ROUTE_TEST_ONLY) || defined(FORTIS_ROBINHOOD_ROUTE_TEST_ONLY) || \
    defined(FORTIS_DASHPAY_ROUTE_TEST_ONLY) || defined(FORTIS_AI_ROUTE_TEST_ONLY)
TestPlaneConfig test_plane_config(const NativeConfig& config) {
  TestPlaneConfig switches;
  switches.native_price_route_canary = config.native_price_route_canary;
  switches.native_news_canary = config.native_news_canary;
  switches.native_address_canary = config.native_address_canary;
  switches.native_ai_canary = config.native_ai_canary;
  switches.native_cross_chain_canary = config.native_cross_chain_canary;
  switches.native_robinhood_canary = config.native_robinhood_canary;
  switches.native_dashpay_canary = config.native_dashpay_canary;
  switches.ca_bundle_path = config.ca_bundle_path;
  return switches;
}
#endif

class TestHooks final : public CompositionHooks {
 public:
  bool LoadConfig() override { return LoadProviderTestRouteConfig(&provider_test_); }
  bool BeforeNetworkRegistry() override;
  bool ConfigurePlanes(const NativeConfig& config, PlaneOptions& options) override;
  bool StoreOpened(fortis::storage::KvStore* kv) override;
  bool MarketPlanesStarted(fortis::storage::KvStore* kv) override;
  bool ConfigureAiPlane(const NativeConfig& config, PlaneOptions& options) override;
  bool AiStarted(fortis::storage::KvStore* kv) override;
  bool CrossChainStarted(fortis::storage::KvStore* kv) override;
  bool RobinhoodStarted(fortis::storage::KvStore* kv) override;
  bool DashPayStarted() override;
  // The /probe/provider-test bridge, behind FORTIS_ENABLE_PROVIDER_TEST_ROUTE=1.
  bool Start(http::HttpResponseHandoff& handoff) override {
    provider_context_ = {&provider_bridge_, &handoff};
    if (provider_test_.enabled &&
        !provider_bridge_.Start(provider_test_.upstream_port, provider_test_.tls_failure_port,
                                provider_test_.upstream_ca, &handoff)) {
      fprintf(stderr, "Failed to start test-only provider HTTP bridge\n");
      return false;
    }
    return true;
  }
  void Stop() override { provider_bridge_.Stop(); }
  bool ConfigureOperations(operations::CrashReportRouteContext& crash_reports,
                           operations::OperationsRouteContext& operations, operations::ApiTelemetry& telemetry) override;
  bool RegisterOperationsRoutes(http::HttpRouter& router, Planes& planes) override;
  // The async probes, then /probe/provider-test (which only enqueues to its
  // bridge, so it runs without the route state lock).
  bool RegisterProbeRoutes(http::HttpRouter& router) override {
    if (!RegisterAsyncTestProbe(router) ||
        !RegisterProviderTestRoute(router, provider_test_.enabled ? &provider_context_ : nullptr))
      return false;
    if (provider_test_.enabled) router.RunWithoutStateLock("/probe/provider-test");
    return true;
  }
  bool RegisterPlaneRoutes(http::HttpRouter& router, Planes& planes) override;
  void ConfigureSweep(Planes& planes, uint32_t* interval_ms, fortis::base::Clock** clock) override;

 private:
  ProviderTestRouteConfig provider_test_{};
  ProviderHttpBridge provider_bridge_;
  ProviderTestRouteContext provider_context_{};
};

bool TestHooks::BeforeNetworkRegistry() {
#ifdef FORTIS_NETWORK_ROUTE_TEST_ONLY
  if (!SeedNetworkTest()) return false;
#endif
  return true;
}

bool TestHooks::ConfigurePlanes(const NativeConfig& config, PlaneOptions& options) {
  (void)config;
  (void)options;
#if defined(FORTIS_PRICE_ROUTE_TEST_ONLY) || defined(FORTIS_HISTORY_ROUTE_TEST_ONLY) || \
    defined(FORTIS_DISCOVERY_ROUTE_TEST_ONLY) || defined(FORTIS_NEWS_ROUTE_TEST_ONLY) || \
    defined(FORTIS_TOKEN_ROUTE_TEST_ONLY) || defined(FORTIS_ADDRESS_ROUTE_TEST_ONLY) || \
    defined(FORTIS_CROSS_CHAIN_ROUTE_TEST_ONLY) || defined(FORTIS_ROBINHOOD_ROUTE_TEST_ONLY) || \
    defined(FORTIS_DASHPAY_ROUTE_TEST_ONLY)
  const TestPlaneConfig switches = test_plane_config(config);
#endif
#ifdef FORTIS_PRICE_ROUTE_TEST_ONLY
  // The spot owner's wall clock in every binary with the price test plane:
  // FORTIS_TEST_PRICE_NOW_FILE / _MS, whichever start the owner gets.
  options.price_owner_clock = &PriceOwnerEnvClock();
#endif
#ifdef FORTIS_HISTORY_ROUTE_TEST_ONLY
  if (!ConfigureHistoryTest(switches, options)) return false;
#endif
#ifdef FORTIS_PRICE_ROUTE_TEST_ONLY
  if (!ConfigurePriceTest(switches, options)) return false;
#endif
#ifdef FORTIS_DISCOVERY_ROUTE_TEST_ONLY
  if (!ConfigureDiscoveryTest(switches, options)) return false;
#endif
#ifdef FORTIS_NEWS_ROUTE_TEST_ONLY
  if (!ConfigureNewsTest(switches, options)) return false;
#endif
#ifdef FORTIS_TOKEN_ROUTE_TEST_ONLY
  if (!ConfigureTokenTest(switches, options)) return false;
#endif
#ifdef FORTIS_ADDRESS_ROUTE_TEST_ONLY
  if (!ConfigureAddressTest(switches, options)) return false;
#endif
#ifdef FORTIS_CROSS_CHAIN_ROUTE_TEST_ONLY
  if (!ConfigureCrossChainTest(switches, options)) return false;
#endif
#ifdef FORTIS_ROBINHOOD_ROUTE_TEST_ONLY
  if (!ConfigureRobinhoodTest(switches, options)) return false;
#endif
#ifdef FORTIS_DASHPAY_ROUTE_TEST_ONLY
  if (!ConfigureDashPayTest(switches, options)) return false;
#endif
  return true;
}

bool TestHooks::StoreOpened(fortis::storage::KvStore* kv) {
  (void)kv;
#ifdef FORTIS_HISTORY_ROUTE_TEST_ONLY
  if (!SeedHistoryTest(kv)) return false;
#endif
  return true;
}

bool TestHooks::MarketPlanesStarted(fortis::storage::KvStore* kv) {
  (void)kv;
#ifdef FORTIS_ADDRESS_ROUTE_TEST_ONLY
  if (!SeedAddressTest(kv)) return false;
#endif
#ifdef FORTIS_TOKEN_ROUTE_TEST_ONLY
  if (!SeedTokenTest(kv)) return false;
#endif
#ifdef FORTIS_NEWS_ROUTE_TEST_ONLY
  if (!SeedNewsTest(kv)) return false;
#endif
#ifdef FORTIS_DISCOVERY_ROUTE_TEST_ONLY
  if (!SeedDiscoveryTest(kv)) return false;
#endif
#ifdef FORTIS_PRICE_ROUTE_TEST_ONLY
  if (!SeedPriceTest(kv)) return false;
#endif
  return true;
}

bool TestHooks::ConfigureAiPlane(const NativeConfig& config, PlaneOptions& options) {
  (void)config;
  (void)options;
#ifdef FORTIS_AI_ROUTE_TEST_ONLY
  if (!ConfigureAiTest(test_plane_config(config), options)) return false;
#endif
  return true;
}

bool TestHooks::AiStarted(fortis::storage::KvStore* kv) {
  (void)kv;
#ifdef FORTIS_AI_ROUTE_TEST_ONLY
  if (!SeedAiTest(kv)) return false;
#endif
  return true;
}

bool TestHooks::CrossChainStarted(fortis::storage::KvStore* kv) {
  (void)kv;
#ifdef FORTIS_CROSS_CHAIN_ROUTE_TEST_ONLY
  if (!SeedCrossChainTest(kv)) return false;
#endif
  return true;
}

bool TestHooks::RobinhoodStarted(fortis::storage::KvStore* kv) {
  (void)kv;
#ifdef FORTIS_ROBINHOOD_ROUTE_TEST_ONLY
  if (!SeedRobinhoodTest(kv)) return false;
#endif
  return true;
}

bool TestHooks::DashPayStarted() {
#ifdef FORTIS_DASHPAY_ROUTE_TEST_ONLY
  if (!SeedDashPayTest()) return false;
#endif
  return true;
}

bool TestHooks::ConfigureOperations(operations::CrashReportRouteContext& crash_reports,
                                    operations::OperationsRouteContext& operations,
                                    operations::ApiTelemetry& telemetry) {
  (void)crash_reports;
  (void)operations;
  (void)telemetry;
#ifdef FORTIS_OPERATIONS_TEST_ONLY
  if (!ConfigureOperationsTestPlane(crash_reports, operations, telemetry)) return false;
#endif
  return true;
}

bool TestHooks::RegisterOperationsRoutes(http::HttpRouter& router, Planes& planes) {
  (void)router;
  (void)planes;
#ifdef FORTIS_OPERATIONS_TEST_ONLY
  if (!RegisterOperationsTestPlaneRoutes(router, planes)) return false;
#endif
  return true;
}

bool TestHooks::RegisterPlaneRoutes(http::HttpRouter& router, Planes& planes) {
  (void)router;
  (void)planes;
#ifdef FORTIS_DASHPAY_ROUTE_TEST_ONLY
  if (!RegisterDashPayTestRoutes(router, planes)) return false;
#endif
#ifdef FORTIS_ROBINHOOD_ROUTE_TEST_ONLY
  if (!RegisterRobinhoodTestRoutes(router, planes)) return false;
#endif
#ifdef FORTIS_CROSS_CHAIN_ROUTE_TEST_ONLY
  if (!RegisterCrossChainTestRoutes(router, planes)) return false;
#endif
#ifdef FORTIS_AI_ROUTE_TEST_ONLY
  if (!RegisterAiTestRoutes(router, planes)) return false;
#endif
#ifdef FORTIS_ADDRESS_ROUTE_TEST_ONLY
  if (!RegisterAddressTestRoutes(router, planes)) return false;
#endif
#ifdef FORTIS_TOKEN_ROUTE_TEST_ONLY
  if (!RegisterTokenTestRoutes(router, planes)) return false;
#endif
#ifdef FORTIS_DISCOVERY_ROUTE_TEST_ONLY
  if (!RegisterDiscoveryTestRoutes(router)) return false;
#endif
#if defined(FORTIS_TOKEN_ROUTE_TEST_ONLY) || defined(FORTIS_DISCOVERY_ROUTE_TEST_ONLY)
  if (!RegisterCoinPopularityTestRoute(router, planes)) return false;
#endif
#ifdef FORTIS_NEWS_ROUTE_TEST_ONLY
  if (!RegisterNewsTestRoutes(router, planes)) return false;
#endif
#ifdef FORTIS_PRICE_ROUTE_TEST_ONLY
  if (!RegisterPriceTestRoutes(router, planes)) return false;
#endif
  return true;
}

void TestHooks::ConfigureSweep(Planes& planes, uint32_t* interval_ms, fortis::base::Clock** clock) {
  (void)planes;
  (void)interval_ms;
  (void)clock;
#ifdef FORTIS_OPERATIONS_TEST_ONLY
  OperationsTestSweep(planes, interval_ms, clock);
#endif
#ifdef FORTIS_NEWS_ROUTE_TEST_ONLY
  // The fixture clock lags the wall clock: sweep by the test clock, as the
  // routes read by it, so the sweeper's expiry agrees with theirs.
  NewsTestSweepClock(clock);
#endif
#ifdef FORTIS_TOKEN_ROUTE_TEST_ONLY
  TokenTestSweepClock(clock);
#endif
#ifdef FORTIS_DISCOVERY_ROUTE_TEST_ONLY
  // The fixture clock lags the wall clock: the sweeper expires by it too, so
  // answers, coin details and media records live as long as the routes see.
  DiscoveryTestSweepClock(clock);
#endif
#ifdef FORTIS_ADDRESS_ROUTE_TEST_ONLY
  AddressTestSweepClock(clock);
#endif
#ifdef FORTIS_AI_ROUTE_TEST_ONLY
  // The fixture clock lags the wall clock: the sweeper expires by it too.
  AiTestSweepClock(clock);
#endif
}

}  // namespace
}  // namespace fortis::app

int main(int argc, char** argv) {
  // The test hooks and probes read their switches and inputs only through
  // this lookup (test_hook_env.h).
  LoadTestHookEnv(ProcessEnvironment, nullptr);
#ifdef FORTIS_HISTORY_ROUTE_TEST_ONLY
  // The history owner and coverage clock in every binary with the history
  // test plane: FORTIS_TEST_HISTORY_NOW_FILE / _MS.
  SetPriceHistoryCoverageClock(&HistoryCoverageEnvClock());
#endif
  static fortis::app::TestHooks hooks;
  return fortis::app::ServerMain(argc, argv, hooks);
}
