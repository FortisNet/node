#include "error_board.h"
#include "exception_boundary.h"
// The composition root. ServerMain() loads the startup configuration, hands
// each module its slice (module_configs.h), starts the enabled planes
// (service_wiring.h), binds the public listener, registers every route and
// serves until SIGINT/SIGTERM, then stops everything in reverse order. What
// another composition adds enters only through CompositionHooks
// (composition_hooks.h); the production entry point (main.cpp) adds nothing.

#include "api_telemetry.h"
#include "log.h"
#include "app_support.h"
#include "application.h"
#include "cache_explorer_route.h"
#include "composition_hooks.h"
#include "crash_report_route.h"
#include "device_enrollment.h"
#include "http_listener.h"
#include "https_listener.h"
#include "http_server.h"
#include "kv_runtime.h"
#include "kv_store.h"
#include "module_configs.h"
#include "native_config.h"
#include "network_preview_route.h"
#include "network_registry.h"
#include "network_route.h"
#include "operations_routes.h"
#include "release_routes.h"
#include "outbound_https.h"
#include "play_integrity_enrollment.h"
#include "price_snapshot.h"
#include "price_spot_cache.h"
#include "probe_routes.h"
#include "service_wiring.h"
#include "shutdown_signal.h"
#include "stdlib_check.h"
#include "token_prices_route.h"

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <new>

namespace fortis::app {

namespace {

// The public router (~7 KiB of route and metrics tables) and the response
// handoff (16 pending slots, ~13 KiB) live here to leave room on the 64 KiB
// Unikraft boot stack. The handoff has a destructor, and the guest link has
// no __dso_handle to register a global one, so it is placement-constructed
// into this storage by ServerMain(). A scoped standard owner destroys it
// on every exit path without deallocating the static storage.
http::HttpRouter g_public_router;
alignas(http::HttpResponseHandoff) unsigned char g_response_handoff_storage[sizeof(http::HttpResponseHandoff)];

// End the object lifetime; its destructor stops the writers idempotently.
struct HandoffStorageDeleter {
  void operator()(http::HttpResponseHandoff* handoff) const noexcept { std::destroy_at(handoff); }
};
class NativeShutdown final : public ShutdownActions {
 public:
  NativeShutdown(int listener, HttpWorkerPool& workers, Planes& owners,
      CompositionHooks& hooks, http::HttpResponseHandoff& handoff, operations::ErrorBoard& errors)
      : listener_(listener), workers_(workers), owners_(owners), hooks_(hooks), handoff_(handoff), errors_(errors) {}
  HealthListener* health = nullptr;
  void Execute(ShutdownStage stage) override {
    switch (stage) {
      case ShutdownStage::Listener: close(listener_); break;
      case ShutdownStage::MarkShutdown: app::MarkShutdown(); break;
      case ShutdownStage::Health: if (health) health->Stop(); break;
      case ShutdownStage::Workers: app::StopHttpWorkers(&workers_); break;
      case ShutdownStage::Sampler: operations::StopOperationsHistorySampler(); break;
      case ShutdownStage::Attestation: auth::StopPlayIntegrityRuntime(); break;
      case ShutdownStage::Sweepers: app::StopSweepers(owners_); break;
      case ShutdownStage::Owners: owners_.StopOwners(); break;
      case ShutdownStage::Coordinator: owners_.StopCoordinator(); break;
      case ShutdownStage::Hooks: hooks_.Stop(); break;
      case ShutdownStage::Handoff: handoff_.Stop(); break;
      case ShutdownStage::Store:
        if (!errors_.Flush()) base::LogError("operations", "Node errors could not be flushed before shutdown");
        if (auto* store = storage::RuntimeKvStoreIfOpen()) store->Close();
        break;
    }
  }
 private:
  int listener_;
  HttpWorkerPool& workers_;
  Planes& owners_;
  CompositionHooks& hooks_;
  http::HttpResponseHandoff& handoff_;
  operations::ErrorBoard& errors_;
};

// The native configuration is ~16 KiB and is used for the process lifetime.
// Keeping it off the boot stack leaves room for TLS and JSON startup parsing.
// So are the per-module slices built from it.
config::NativeConfig g_native_config;
app::ModuleConfigs g_module_configs;

// Loads and validates the startup configuration and the outbound TLS state.
// False after printing why (the process exits 2).
bool load_startup_config(config::NativeConfig& config) {
  char config_error[256]{};
  if (!LoadNativeConfig(config::ProcessEnvironment, nullptr, &config, config_error,
                        sizeof(config_error))) {
    fortis::base::LogError("app", "%s", config_error);
    return false;
  }
  // From here on only lines at or above FORTIS_LOG_LEVEL are written.
  fortis::base::SetLogThreshold(config.log_level);
  auth::ConfigureAndroidAttestation(config.android_attestation_required);
  auth::ConfigureAppleAttestation(config.apple_attestation_required);
  auth::ConfigureDesktopAttestation(config.desktop_attestation_required);
  if (!http::OutboundHttpsInitialize()) {
    fortis::base::LogError("app", "Failed to initialize outbound TLS crypto state");
    return false;
  }
  if (config.ca_bundle_configured && !http::OutboundHttpsValidateCaBundle(config.ca_bundle_path)) {
    fortis::base::LogError("app", "Invalid CA bundle configured by FORTIS_CA_BUNDLE_PATH");
    return false;
  }
  // Required-mode Android enrollment (FORTIS_PLAY_INTEGRITY_*): parsed and
  // its Google client initialized (credential file, CA, policy) once, here,
  // before any route is registered or worker started. A supplied but invalid
  // configuration stops the server; an absent one leaves Android enrollment
  // answering ATTESTATION_REQUIRED. The client lives until
  // StopPlayIntegrityRuntime after the HTTP workers are joined; the error
  // exits below leave it to process teardown.
  if (config.android_attestation_required) {
    fortis::auth::PlayIntegrityRuntimeConfig play_config;
    char play_error[512]{};
    if (!fortis::auth::LoadPlayIntegrityConfig(config::ProcessEnvironment, nullptr,
                                         config.ca_bundle_configured ? config.ca_bundle_path
                                                                     : nullptr,
                                         &play_config, play_error, sizeof(play_error)) ||
        !fortis::auth::StartPlayIntegrityRuntime(play_config, play_error, sizeof(play_error))) {
      fortis::base::LogError("app", "%s", play_error);
      return false;
    }
    if (play_config.configured)
      fortis::base::LogInfo("app", "Play Integrity Android enrollment: configured (%zu release certificate "
              "digest(s), minimum versionCode %lld)", play_config.certificate_count,
              static_cast<long long>(play_config.min_version_code));
    else if (config.attestation_required)
      fortis::base::LogWarn("app", "Play Integrity Android enrollment: not configured; required-mode "
              "Android enrollment answers ATTESTATION_REQUIRED");
  }
  return true;
}

// The operator surface: the root pages, /statusz and its sampler inputs,
// the price snapshot and memory-hit gauges, and the crash report route.
void wire_operations(operations::OperationsRouteContext& operations, operations::CrashReportRouteContext& crash_reports,
                     operations::ApiTelemetry& telemetry, app::Planes& planes, const config::NativeConfig& config,
                     uint64_t process_started_ms) {
  operations.telemetry = &telemetry;
  operations.ai_config = &g_module_configs.ai;
  operations.services = {config.native_price_route_canary, config.native_news_canary,
                         config.native_address_canary, config.native_ai_canary,
                         config.native_cross_chain_canary, config.native_robinhood_canary,
                         config.native_dashpay_canary};
  if (auto* view = planes.price ? planes.price->snapshot_view() : nullptr) {
    operations.price_snapshot_context = view;
    operations.sample_price_snapshot = [](void* context, int64_t now) {
      return static_cast<prices::PriceSnapshotView*>(context)->Status(now);
    };
  }
  operations.price_store = planes.price_kv;
  if (planes.price_kv) {
    operations.price_cache_context = planes.price ? planes.price->cache() : nullptr;
    operations.sample_price_memory_hits = [](void* context, uint64_t& all, uint64_t& contracts) {
      auto* cache = static_cast<fortis::prices::PriceSpotCache*>(context);
      all = cache->MemoryHits();
      contracts = cache->ContractMemoryHits();
    };
  }
  operations.data_directory = config.kv_dir;
  operations.started_monotonic_ms = process_started_ms;
  crash_reports.attestation_required = config.attestation_required;
}

}  // namespace

int Application::Run(int argc, char** argv) {
  CompositionHooks& hooks = hooks_;
  // /statusz uptimeSec counts from here, like process.uptime(), not from
  // route registration after the KV store and owners have started.
  const uint64_t process_started_ms = operations::StatusMonotonicMs();
  if (argc > 2) {
    fortis::base::LogError("app", "Usage: %s [port]", argv[0]);
    return 2;
  }
  // The C++ runtime must work before anything depends on it: the allocator,
  // a standard container and a thrown-and-caught standard exception.
  if (!fortis::base::StandardLibraryUsable()) {
    fortis::base::LogError("app", "C++ runtime self-check failed");
    return 1;
  }
  config::NativeConfig& config = g_native_config;
  if (!load_startup_config(config)) return 2;
  app::BuildModuleConfigs(config, &g_module_configs);
  if (!hooks.LoadConfig()) return 2;
  const int port = argc == 2 ? app::parse_port(argv[1]) : config.port;
  if (port < 0) {
    fortis::base::LogError("app", "Invalid port");
    return 2;
  }
  fortis::storage::RuntimeKvSetDirectory(config.kv_dir, config.kv_group_commit,
                                config.kv_commit_slots ? fortis::storage::KvDurability::CommitSlots
                                                       : fortis::storage::KvDurability::RollbackMarker);
  // API telemetry outlives the router and the response handoff, whose
  // writers report reserved completions into it until they stop.
  operations::ApiTelemetry api_telemetry;
  if (!api_telemetry.Start()) {
    fortis::base::LogError("app", "Failed to allocate the API telemetry ring");
    return 2;
  }

  auto* error_store = storage::RuntimeKvStore();
  // Preserve the health/error surface when storage is unavailable. The unopened
  // fallback retains diagnostics in memory and exposes failed persistence until
  // the operator repairs storage and restarts the process.
  std::unique_ptr<storage::KvStore> unavailable_error_store;
  if (!error_store) {
    base::LogError("operations", "Failed to open node error storage");
    unavailable_error_store = std::make_unique<storage::KvStore>();
    error_store = unavailable_error_store.get();
  }
  auto errors = std::make_unique<operations::ErrorBoard>(*error_store);
  if (!errors->Load()) base::LogError("operations", "Unable to restore node error summaries");

  // Every plane's owners. Declared before the listener state, so on any
  // return they are destroyed after it; explicit exits stop the owners
  // before the coordinator they lease from.
  app::Planes planes;
  auto stop_planes = [&]() {
    planes.StopOwners();
    planes.StopCoordinator();
  };
  if (!hooks.BeforeNetworkRegistry()) return 2;
  networks::NativeNetworkRegistry network_registry;
  operations::NetworkRouteContext network_route_context{&network_registry, &config};
  // Every enabled owner starts here, before the listener binds. Any failure
  // stops whatever started, then the coordinator, and exits without serving
  // a partial surface.
  app::PlaneOptions options = app::PlaneOptionsFromConfig(config);
  options.errors = errors.get();
  ModuleComposition composition(config, g_module_configs, options, planes, hooks);
  if (!hooks.ConfigurePlanes(config, options) ||
      !composition.Start()) {
    stop_planes();
    return 2;
  }

  app::InstallShutdownSignals();
  const int server = app::ListenOnPort(port, static_cast<int>(app::kPendingConnectionCapacity));
  if (server < 0) {
    fortis::base::LogErrno(fortis::base::LogLevel::kError, "app", "bind/listen", errno);
    stop_planes();
    return 1;
  }

  // Static storage (see g_public_router): keeps the boot stack shallow.
  http::HttpRouter& router = g_public_router;
  std::unique_ptr<http::HttpResponseHandoff, HandoffStorageDeleter> response_handoff_guard(
      new (g_response_handoff_storage) http::HttpResponseHandoff());
  http::HttpResponseHandoff& response_handoff = *response_handoff_guard;
  if (!response_handoff.Start()) {
    fortis::base::LogError("app", "Failed to start bounded HTTP response writers");
    close(server);
    stop_planes();
    return 1;
  }
  router.SetResponseHandoff(&response_handoff);
  router.SetCompletionDetails(operations::ApiTelemetry::OnHttpDetails, &api_telemetry);
  if (!hooks.Start(response_handoff)) {
    close(server);
    stop_planes();
    response_handoff.Stop();
    return 1;
  }

  // From here a failure stops what has started, in the shutdown order below.
  static app::HttpWorkerPool worker_pool{};
  NativeShutdown shutdown_actions(server, worker_pool, planes, hooks, response_handoff, *errors);
  ApplicationShutdown shutdown(shutdown_actions);
  auto fail = [&](const char* message) {
    if (message) fortis::base::LogError("app", "%s", message);
    shutdown.Fail();
    return 1;
  };

  app::MetricsRouteContext metrics_context{&g_module_configs.probes, &router};
  operations::OperationsRouteContext operations_context{};
  operations_context.errors = errors.get();
  operations_context.error_reports_attestation_required = config.attestation_required;
  operations::CrashReportRouteContext crash_report_context{};
  wire_operations(operations_context, crash_report_context, api_telemetry, planes, config,
                  process_started_ms);
  if (!hooks.ConfigureOperations(crash_report_context, operations_context, api_telemetry))
    return fail(nullptr);
  network_route_context.planes.address = planes.address.has_value();
  network_route_context.planes.token = planes.token.has_value();
  network_route_context.planes.news = planes.news.has_value();
  network_route_context.planes.prices_spot = planes.price.has_value();
  network_route_context.planes.prices_history = planes.history.has_value();
  static const char* landing_capabilities[64][networks::kNetworkCapabilityCount]{};
  size_t landing_networks = 0;
  if (!DeriveNetworkCapabilities(network_registry, config, network_route_context.planes,
                                 landing_capabilities, 64, &landing_networks))
    return fail("Failed to derive network capabilities for landing page");
  operations_context.landing_counts = operations::LandingCountsFromCapabilities(
      network_registry, landing_capabilities, landing_networks);
  if (!RegisterOperationsRoutes(router, &operations_context) ||
      !operations::RegisterReleaseRoutes(router, config.release_status_dir) ||
      !operations::RegisterTokenPricesRoutes(router, planes.price_kv) ||
      !RegisterCrashReportRoute(router, &crash_report_context))
    return fail("Failed to register the operations routes");
  if (!hooks.RegisterOperationsRoutes(router, planes)) return fail(nullptr);
  // The catalog states what this process serves: every owner below either
  // started (and registers its routes, or the process exits) or is absent.
  if (!RegisterNetworksRoute(router, &network_route_context))
    return fail("Failed to register networks route");
  if (!app::RegisterProbeRoutes(router, &g_module_configs.probes, &g_module_configs.devices,
                                &metrics_context, hooks)) {
    fortis::base::LogError("app", "Failed to register required probe routes (capacity=%zu)",
            http::HttpRouter::kMaxRoutes);
    return fail(nullptr);
  }
  if (config.diagnostic_probes_enabled)
    fortis::base::LogWarn("app", "Diagnostic probes enabled (FORTIS_ENABLE_DIAGNOSTIC_PROBES=1); "
                    "keep this listener private");
  if (!operations::RegisterNetworkPreviewRoutes(router, &network_registry) ||
      !operations::RegisterCacheExplorerRoutes(router, planes.price_kv))
    return fail("Failed to register cache preview routes");
  // All plane routes register, or the process exits before any worker starts.
  if (!composition.RegisterRoutes(router, response_handoff))
    return fail("Failed to register the native price routes");

  uint32_t sweep_interval_ms = storage::KvExpirySweeper::kIntervalMs;
  fortis::base::Clock* sweep_clock = nullptr;
  hooks.ConfigureSweep(planes, &sweep_interval_ms, &sweep_clock);
  shutdown.SweepersStarting();
  if (!composition.StartSweepers(sweep_interval_ms, sweep_clock))
    return fail("Failed to start the KV expiry sweeper");
  worker_pool.router = &router;
  shutdown.WorkersStarting();
  const size_t started_workers = app::StartHttpWorkers(&worker_pool);
  if (started_workers != app::kHttpWorkerCount) {
    fortis::base::LogError("app", "Failed to start the fixed HTTP worker pool (%zu/%zu)",
            started_workers, app::kHttpWorkerCount);
    return fail(nullptr);
  }
  if (!StartOperationsHistorySampler(&operations_context))
    return fail("Failed to start operations history sampler");
  shutdown.SamplerStarted();
  static app::HealthListener health;
  shutdown_actions.health = &health;
  if ((config.health_port || HttpsListener::Enabled()) &&
      (!health.router.Add("GET", "/healthz", app::HandleProbe, &g_module_configs.probes) ||
       (config.health_port && !health.Start(config.health_port))))
    return fail("Failed to start isolated health listener");
  fortis::base::LogInfo("app", "HTTP admission: workers=%zu pending=%zu health_port=%u",
          started_workers, app::kPendingConnectionCapacity, config.health_port);
  printf("fortis_node probe listening on port %d\n", port);
  fflush(stdout);

  // Ingress outlives shutdown: deferred response owners may still hold its
  // transports until the handoff writers have joined.
  std::unique_ptr<HttpsListener> https;
  bool accept_failed = false;
  if (HttpsListener::Enabled()) {
    https = std::make_unique<HttpsListener>();
    if (!https->Start(server, worker_pool, health.router)) {
      fortis::base::LogError("https", "Native HTTPS configuration refused");
      shutdown.Complete();
      return 2;
    }
    accept_failed = !https->Run();
  } else {
    accept_failed = !app::RunAcceptLoop(server, &worker_pool, &api_telemetry);
  }

  shutdown.Complete();
  return accept_failed ? 1 : 0;
}

int ServerMain(int argc, char** argv, CompositionHooks& hooks) {
  return fortis::base::FatalBoundary("ServerMain", [&] { return Application(hooks).Run(argc, argv); });
}

}  // namespace fortis::app
