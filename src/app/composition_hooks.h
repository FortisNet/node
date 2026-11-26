#pragma once
#include "http_server.h"

// The composition root's extension seam. ServerMain (server.cpp) and the
// plane wiring call one hook at each point where a composition may add to
// the server: before the planes start, as each store or plane comes up, as
// each group of routes registers, and at shutdown. The base class adds
// nothing: every hook succeeds without doing anything, and that is the
// object the production entry point (main.cpp) passes.
//
// The replay binaries under tests/ are the other compositions: their entry
// point (tests/server/test_main.cpp) passes hooks that substitute loopback
// upstreams, seed fixtures and register probe routes. None of that code is
// compiled into the production binary or the guest image
// (scripts/check_production_binary.py checks the linked binary).
//
// A hook that returns false has printed why; the caller exits as it does for
// its own failure at that point. No method is pure and the destructor is not
// virtual (see base/clock.h): the guest needs neither __cxa_pure_virtual nor
// operator delete.

#include <stdint.h>

namespace fortis::config { struct NativeConfig; }
namespace fortis::operations { struct CrashReportRouteContext; struct OperationsRouteContext; class ApiTelemetry; }

namespace fortis::base { class Clock; }
namespace fortis::storage { class KvStore; }

namespace fortis::app {

struct PlaneOptions;
struct ModuleOwners;
using Planes = ModuleOwners;

class CompositionHooks {
 public:
  // After the startup configuration is loaded and sliced, before the port is
  // chosen. False exits 2.
  virtual bool LoadConfig() { return true; }
  // Before the network registry is built and any plane starts.
  virtual bool BeforeNetworkRegistry() { return true; }
  // Before any plane starts: may change which planes start and with which
  // owner starters, seams and clocks.
  virtual bool ConfigurePlanes(const config::NativeConfig& config, PlaneOptions& options) {
    (void)config; (void)options; return true;
  }
  // Inside the market plane start: once the KV store (and the separate price
  // store) is open, and again after the last market plane has started.
  virtual bool StoreOpened(fortis::storage::KvStore* kv) { (void)kv; return true; }
  virtual bool MarketPlanesStarted(fortis::storage::KvStore* kv) { (void)kv; return true; }
  // After the market planes, before the AI plane starts.
  virtual bool ConfigureAiPlane(const config::NativeConfig& config, PlaneOptions& options) {
    (void)config; (void)options; return true;
  }
  // After each of these planes has started.
  virtual bool AiStarted(fortis::storage::KvStore* kv) { (void)kv; return true; }
  virtual bool CrossChainStarted(fortis::storage::KvStore* kv) { (void)kv; return true; }
  virtual bool RobinhoodStarted(fortis::storage::KvStore* kv) { (void)kv; return true; }
  virtual bool DashPayStarted() { return true; }
  // Once the listener is bound and the response handoff runs: starts what
  // the composition adds beside the planes. False exits 1. Stop() stops it,
  // after the planes and before the response handoff, on every exit path
  // from then on.
  virtual bool Start(http::HttpResponseHandoff& handoff) { (void)handoff; return true; }
  virtual void Stop() {}
  // The operator surface's contexts, before its routes register.
  virtual bool ConfigureOperations(operations::CrashReportRouteContext& crash_reports,
                                   operations::OperationsRouteContext& operations, operations::ApiTelemetry& telemetry) {
    (void)crash_reports; (void)operations; (void)telemetry; return true;
  }
  // Route registration, in table order: after the operations routes, inside
  // the probe routes (after the diagnostic probes, before POST
  // /probe/protobuf) and after every plane's routes.
  virtual bool RegisterOperationsRoutes(http::HttpRouter& router, Planes& planes) {
    (void)router; (void)planes; return true;
  }
  virtual bool RegisterProbeRoutes(http::HttpRouter& router) { (void)router; return true; }
  virtual bool RegisterPlaneRoutes(http::HttpRouter& router, Planes& planes) {
    (void)router; (void)planes; return true;
  }
  // The KV sweepers' interval and clock, before they start.
  virtual void ConfigureSweep(Planes& planes, uint32_t* interval_ms, fortis::base::Clock** clock) {
    (void)planes; (void)interval_ms; (void)clock;
  }

 protected:
  constexpr CompositionHooks() = default;
  ~CompositionHooks() = default;
};

// The production composition: nothing added.
class NoCompositionHooks final : public CompositionHooks {
 public:
  constexpr NoCompositionHooks() = default;
};

// Loads the configuration, starts the enabled planes, binds the public
// listener, registers every route and serves until SIGINT/SIGTERM, then stops
// everything in reverse order. Returns the process exit status.
int ServerMain(int argc, char** argv, CompositionHooks& hooks);

}  // namespace fortis::app
