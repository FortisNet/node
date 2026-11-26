#pragma once

// Per-plane test probes of the server's test binaries: each plane's switch
// (FORTIS_ENABLE_TEST_*), loopback upstream, clocks, fixture devices and
// /probe/test-* routes live in one test_probes_<area>.cpp, compiled with the
// variant's FORTIS_*_TEST_ONLY flags. test_main.cpp's hooks call them in the
// order the composition used to run them inline. Every switch and input is
// read through the test-hook environment (test_hook_env.h).

#include "clock.h"
#include "seams.h"

#include <stdint.h>

namespace fortis::operations { struct CrashReportRouteContext; struct OperationsRouteContext; class ApiTelemetry; }
namespace fortis::http { class HttpRouter; }
namespace fortis::storage { class KvStore; }

namespace fortis::app {

struct PlaneOptions;
struct ModuleOwners;
using Planes = ModuleOwners;

// The startup switches the test planes check (NativeConfig fields of the
// same names), built by test_main.cpp.
struct TestPlaneConfig {
  bool native_price_route_canary = false;
  bool native_news_canary = false;
  bool native_address_canary = false;
  bool native_ai_canary = false;
  bool native_cross_chain_canary = false;
  bool native_robinhood_canary = false;
  bool native_dashpay_canary = false;
  // FORTIS_CA_BUNDLE_PATH: the AI test's loopback model upstream trusts it.
  const char* ca_bundle_path = nullptr;
};

// Seam objects over a test binary's clock, jitter and id functions (file or
// environment driven). The probe handlers also call the functions directly.
template <int64_t (*NowFn)()>
class TestFnClock final : public fortis::base::Clock {
 public:
  int64_t NowMs() override { return NowFn(); }
};
template <bool (*SampleFn)(double*)>
class TestFnJitter final : public fortis::base::JitterSource {
 public:
  bool Sample(double* sample) override { return SampleFn(sample); }
};
template <bool (*NewIdFn)(char*)>
class TestFnIds final : public fortis::base::IdSource {
 public:
  bool NewId(char output[kIdBytes]) override { return NewIdFn(output); }
};

// ---- test_probes_market.cpp: spot, history and discovery --------------------
#ifdef FORTIS_HISTORY_ROUTE_TEST_ONLY
bool ConfigureHistoryTest(const TestPlaneConfig& config, PlaneOptions& options);
bool SeedHistoryTest(fortis::storage::KvStore* kv);
// Points the history owners at the loopback upstream `port`/`ca` unless the
// history test route already did (the token and address replays read
// series through the history service); `test_seams` adds the history seams.
void UseLoopbackHistoryOwners(int port, const char* ca, bool test_seams, PlaneOptions& options);
#endif
#ifdef FORTIS_PRICE_ROUTE_TEST_ONLY
bool ConfigurePriceTest(const TestPlaneConfig& config, PlaneOptions& options);
bool SeedPriceTest(fortis::storage::KvStore* kv);
bool RegisterPriceTestRoutes(fortis::http::HttpRouter& router, Planes& planes);
#endif
#ifdef FORTIS_DISCOVERY_ROUTE_TEST_ONLY
bool ConfigureDiscoveryTest(const TestPlaneConfig& config, PlaneOptions& options);
bool SeedDiscoveryTest(fortis::storage::KvStore* kv);
bool RegisterDiscoveryTestRoutes(fortis::http::HttpRouter& router);
void DiscoveryTestSweepClock(fortis::base::Clock** clock);
#endif
#if defined(FORTIS_TOKEN_ROUTE_TEST_ONLY) || defined(FORTIS_DISCOVERY_ROUTE_TEST_ONLY)
bool RegisterCoinPopularityTestRoute(fortis::http::HttpRouter& router, Planes& planes);
#endif

// ---- test_probes_token.cpp --------------------------------------------------
#ifdef FORTIS_TOKEN_ROUTE_TEST_ONLY
bool ConfigureTokenTest(const TestPlaneConfig& config, PlaneOptions& options);
bool SeedTokenTest(fortis::storage::KvStore* kv);
bool RegisterTokenTestRoutes(fortis::http::HttpRouter& router, Planes& planes);
void TokenTestSweepClock(fortis::base::Clock** clock);
#endif

// ---- test_probes_news.cpp ---------------------------------------------------
#ifdef FORTIS_NEWS_ROUTE_TEST_ONLY
bool ConfigureNewsTest(const TestPlaneConfig& config, PlaneOptions& options);
bool SeedNewsTest(fortis::storage::KvStore* kv);
bool RegisterNewsTestRoutes(fortis::http::HttpRouter& router, Planes& planes);
void NewsTestSweepClock(fortis::base::Clock** clock);
#endif

// ---- test_probes_address.cpp ------------------------------------------------
#ifdef FORTIS_ADDRESS_ROUTE_TEST_ONLY
bool ConfigureAddressTest(const TestPlaneConfig& config, PlaneOptions& options);
bool SeedAddressTest(fortis::storage::KvStore* kv);
bool RegisterAddressTestRoutes(fortis::http::HttpRouter& router, Planes& planes);
void AddressTestSweepClock(fortis::base::Clock** clock);
#endif

// ---- test_probes_services.cpp: AI, exchange, operations and networks ------
#ifdef FORTIS_AI_ROUTE_TEST_ONLY
bool ConfigureAiTest(const TestPlaneConfig& config, PlaneOptions& options);
bool SeedAiTest(fortis::storage::KvStore* kv);
bool RegisterAiTestRoutes(fortis::http::HttpRouter& router, Planes& planes);
void AiTestSweepClock(fortis::base::Clock** clock);
#endif
#ifdef FORTIS_CROSS_CHAIN_ROUTE_TEST_ONLY
bool ConfigureCrossChainTest(const TestPlaneConfig& config, PlaneOptions& options);
bool SeedCrossChainTest(fortis::storage::KvStore* kv);
bool RegisterCrossChainTestRoutes(fortis::http::HttpRouter& router, Planes& planes);
#endif
#ifdef FORTIS_ROBINHOOD_ROUTE_TEST_ONLY
bool ConfigureRobinhoodTest(const TestPlaneConfig& config, PlaneOptions& options);
bool SeedRobinhoodTest(fortis::storage::KvStore* kv);
bool RegisterRobinhoodTestRoutes(fortis::http::HttpRouter& router, Planes& planes);
#endif
#ifdef FORTIS_DASHPAY_ROUTE_TEST_ONLY
bool ConfigureDashPayTest(const TestPlaneConfig& config, PlaneOptions& options);
bool SeedDashPayTest();
bool RegisterDashPayTestRoutes(fortis::http::HttpRouter& router, Planes& planes);
#endif
#ifdef FORTIS_OPERATIONS_TEST_ONLY
bool ConfigureOperationsTestPlane(fortis::operations::CrashReportRouteContext& crash_reports,
                                  fortis::operations::OperationsRouteContext& operations, fortis::operations::ApiTelemetry& telemetry);
bool RegisterOperationsTestPlaneRoutes(fortis::http::HttpRouter& router, Planes& planes);
void OperationsTestSweep(Planes& planes, uint32_t* interval_ms, fortis::base::Clock** clock);
#endif
#ifdef FORTIS_NETWORK_ROUTE_TEST_ONLY
bool SeedNetworkTest();
#endif

}  // namespace fortis::app
