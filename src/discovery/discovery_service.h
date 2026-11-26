#pragma once

#include <memory>
#include "status.h"

#include "discovery_cache.h"
#include "ownership.h"
#include "discovery_provider_owner.h"
#include "http_server.h"
#include "provider_runtime_config.h"
#include "seams.h"

namespace fortis::storage { class KvStore; }
namespace fortis::discovery { class CoinPopularity; }
namespace fortis::providers { class ProviderExecutionCoordinator; }

namespace fortis::discovery {

// Normal startup passes none and always uses DiscoveryProviderOwner::Start():
// production provider hosts, the configured CA bundle and provider keys, and
// the public-address policy. A replay binary supplies a start that redirects
// the owner (DiscoveryProviderOwner::StartRedirected).
using DiscoveryOwnerStarter = fortis::base::Starter<DiscoveryProviderOwner>;

// The discovery composition behind FORTIS_NATIVE_PRICE_ROUTE_CANARY:
// POST /v1/bittensor/analytics, POST /v1/prices/markets and
// GET /v1/networks/{network}/tokens/search over one
// DiscoveryCache on the runtime KV and one DiscoveryProviderOwner leased on
// the process ProviderExecutionCoordinator. The workspace (about 5 MiB of
// buffers, see discovery_route_support.h) and the owner are allocated by
// Create, so a disabled image pays nothing.
//
// Lifecycle mirrors PriceSpotService: Create once with a started coordinator;
// Register once the response handoff runs; Stop after HTTP workers have
// joined and before the coordinator and handoff stop. `seams` must outlive
// the service; normal startup passes none.
struct DiscoveryServiceDeps {
  storage::KvStore& kv;
  providers::ProviderExecutionCoordinator& coordinator;
  CoinPopularity* popularity = nullptr;
};
struct DiscoveryServiceOptions {
  DiscoveryOwnerStarter* start_owner = nullptr;
  const DiscoveryCacheSeams* seams = nullptr;
};

// Factory-created resource handle. Moved-from handles are destruction-only.
class DiscoveryService {
 public:
  DiscoveryService(DiscoveryService&&) noexcept;
  DiscoveryService& operator=(DiscoveryService&&) = delete;
  ~DiscoveryService();
  DiscoveryService(const DiscoveryService&) = delete;
  DiscoveryService& operator=(const DiscoveryService&) = delete;

  static base::Result<DiscoveryService> Create(const config::ProviderRuntimeConfig& config, DiscoveryServiceDeps deps,
                                      const DiscoveryServiceOptions& options = {});
  bool Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff);
  void Stop();

 private:
  struct Impl;
  explicit DiscoveryService(Impl* impl);
  // Defined beside Impl: only that unit can destroy one.
  struct ImplDeleter { void operator()(Impl* impl) const; };
  std::unique_ptr<Impl, ImplDeleter> impl_;
};

}  // namespace fortis::discovery
