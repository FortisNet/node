#pragma once

#include <memory>
#include "status.h"

#include "provider_runtime_config.h"
#include "ownership.h"
#include "price_spot_route.h"
#include "seams.h"

namespace fortis::storage { class KvStore; }
namespace fortis::prices { class PriceSpotCache; }
namespace fortis::providers { class ProviderExecutionCoordinator; }

namespace fortis::prices {

class PriceSnapshotView;

// Starts the composed owner. Normal startup passes none and always uses
// SpotProviderOwnerStartup::Start(): production provider hosts, the configured CA
// bundle and provider keys, and the public-address policy. A replay binary
// supplies a start that redirects the owner (SpotProviderOwnerStartup::StartRedirected).
using PriceSpotOwnerStarter = fortis::base::Starter<SpotProviderOwnerStartup>;

// The native GET /v1/prices composition: one PriceSeriesStore and one
// PriceSpotCache over the runtime KV, and one SpotProviderOwner leased on the
// process ProviderExecutionCoordinator. Nothing is allocated before Create, so
// a disabled route costs no owner memory, threads or KV work.
//
// Lifecycle operations are serialized by the caller. Create once with a
// started coordinator; Register once the response handoff is running. Call
// Stop after HTTP workers have joined and before the coordinator and handoff
// stop: it drains the owner, which answers pending requests through the
// still-running handoff and releases its coordinator lease. Stop is
// idempotent, and destruction stops a started service.
// Core and recent-contract warmers use this same owner and its bounded fallback
// lanes; they stop before the owner and never retain caller request views.
struct PriceSpotServiceDeps {
  storage::KvStore& kv;
  providers::ProviderExecutionCoordinator& coordinator;
  discovery::CoinPopularity* popularity = nullptr;
  base::Clock* wall_clock = nullptr;
  base::ErrorReporter* errors = nullptr;
};
struct PriceSpotServiceOptions {
  PriceSpotOwnerStarter* start_owner = nullptr;
};

// Factory-created resource handle. Moved-from handles are destruction-only.
class PriceSpotService {
 public:
  PriceSpotService(PriceSpotService&&) noexcept;
  PriceSpotService& operator=(PriceSpotService&&) = delete;
  ~PriceSpotService();
  PriceSpotService(const PriceSpotService&) = delete;
  PriceSpotService& operator=(const PriceSpotService&) = delete;

  // Failure publishes no service and leaves the coordinator running.
  // deps.wall_clock is only the cache/timestamp clock; coordinator timing
  // remains monotonic SystemClock. Borrowed dependencies must outlive this handle.
  static base::Result<PriceSpotService> Create(const config::ProviderRuntimeConfig& config, PriceSpotServiceDeps deps,
                                      const PriceSpotServiceOptions& options = {});
  bool Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff);
  void Stop();
  // Valid from Create until destruction; available on every initialized handle.
  PriceSpotRouteContext* route_context();
  PriceSnapshotView* snapshot_view();
  fortis::prices::PriceSpotCache* cache();

 private:
  struct Impl;
  explicit PriceSpotService(Impl* impl);
  // Defined beside Impl: only that unit can destroy one.
  struct ImplDeleter { void operator()(Impl* impl) const; };
  std::unique_ptr<Impl, ImplDeleter> impl_;
};

}  // namespace fortis::prices
