#include <memory>
#include <new>
#include "threading.h"
#include "discovery_service.h"
#include "ownership.h"

#include "bittensor_route.h"
#include "discovery_route_support.h"
#include "price_markets_route.h"
#include "token_search_route.h"
#include "provider_execution_coordinator.h"

#include <pthread.h>
#include <stdlib.h>

namespace fortis::discovery {

struct DiscoveryService::Impl {
  Impl(const config::ProviderRuntimeConfig& cfg, fortis::storage::KvStore& kv, providers::ProviderExecutionCoordinator& coordinator,
       DiscoveryWorkspace* buffers, const DiscoveryCacheSeams* seams)
      : config(cfg), owner(cfg, &coordinator), workspace(buffers),
        cache(&kv, buffers->kv_scratch, sizeof(buffers->kv_scratch), seams) {
    new (workspace) DiscoveryWorkspace;
    workspace->cache = &cache;
    route.taostats_api_key_configured = config.taostats_api_key_configured;
    route.owner = &owner;
    route.workspace = workspace;
    route.attestation_required = config.attestation_required;
  }
  ~Impl() {
    DestroyPriceMarketsState(route.markets);
    DestroyTokenSearchState(route.search);

  }
  std::unique_ptr<DiscoveryWorkspace, base::StorageDeleter> workspace_storage;
  config::ProviderRuntimeConfig config;
  DiscoveryProviderOwner owner;
  DiscoveryWorkspace* workspace;
  DiscoveryCache cache;
  DiscoveryRouteContext route{};
};

void DiscoveryService::ImplDeleter::operator()(Impl* impl) const { fortis::base::Delete(impl); }

DiscoveryService::~DiscoveryService() {
  if (!impl_) return;
  impl_->owner.Stop();
  impl_.reset();
}

DiscoveryService::DiscoveryService(Impl* impl) : impl_(impl) {}
DiscoveryService::DiscoveryService(DiscoveryService&&) noexcept = default;

base::Result<DiscoveryService> DiscoveryService::Create(
    const config::ProviderRuntimeConfig& config, DiscoveryServiceDeps deps, const DiscoveryServiceOptions& options) try {
  auto& kv = deps.kv;
  auto& coordinator = deps.coordinator;
  auto* start_owner = options.start_owner;
  auto* seams = options.seams;
  if (!coordinator.valid()) return base::Status(base::StatusCode::kUnavailable, "discovery initialization");
  auto workspace_storage = base::AllocateStorage<DiscoveryWorkspace>(sizeof(DiscoveryWorkspace));
  auto* workspace = workspace_storage.get();
  Impl* impl = workspace ? fortis::base::New<Impl>(config, kv, coordinator, workspace, seams) : nullptr;
  if (!impl) { return base::Status(base::StatusCode::kResourceExhausted, "discovery allocation"); }
  DiscoveryService service(impl);
  impl->workspace_storage = std::move(workspace_storage);
  impl->route.popularity = deps.popularity;
  impl->route.markets = CreatePriceMarketsState();
  impl->route.search = CreateTokenSearchState();
  const bool started = impl->route.markets && impl->route.search &&
      (start_owner ? start_owner->Start(impl->owner) : impl->owner.Start());
  if (!started) {
    return base::Status(base::StatusCode::kUnavailable, "discovery initialization");
  }
  return service;
} catch (const std::bad_alloc&) {
  return base::Status(base::StatusCode::kResourceExhausted, "discovery allocation");
}

bool DiscoveryService::Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff) {
  if (impl_->route.handoff) return false;
  impl_->route.handoff = &handoff;
  return RegisterBittensorAnalyticsRoute(router, &impl_->route) &&
         RegisterPriceMarketsRoute(router, &impl_->route) &&
         RegisterTokenSearchRoute(router, &impl_->route);
}

void DiscoveryService::Stop() {
  impl_->owner.Stop();
}

}  // namespace fortis::discovery
