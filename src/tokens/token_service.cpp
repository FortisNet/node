#include <string_view>
#include "exception_boundary.h"
// The token service: its workers and queue, the background revalidator, the
// price seams it hands the facts, the completion paths and the lifecycle.
// The scheduled jobs live in token_service_jobs.cpp and the route handlers in
// token_task_routes.cpp and token_history_routes.cpp.
#include "threading.h"
#include "log.h"
#include "ownership.h"
#include "token_service_internal.h"

namespace tj = fortis::codec::tj;

namespace fortis::tokens {

namespace token_service_internal {

timespec wake_after(uint64_t ms) {
  timespec out{};
  clock_gettime(CLOCK_REALTIME, &out);
  out.tv_sec += static_cast<time_t>(ms / 1000);
  out.tv_nsec += static_cast<long>((ms % 1000) * 1000000);
  if (out.tv_nsec >= 1000000000L) { ++out.tv_sec; out.tv_nsec -= 1000000000L; }
  return out;
}

}  // namespace token_service_internal

TokenService::Impl::Impl(const config::ProviderRuntimeConfig& cfg, fortis::storage::KvStore* store, providers::ProviderExecutionCoordinator* coordinator,
                         prices::SpotProviderOwner* spot_owner, const TokenServiceSeams* test_seams, fortis::storage::KvStore& price_kv,
                         discovery::CoinPopularity* popularity_tracker, prices::PriceHistoryService* history_service,
                         prices::PriceSpotCache* spot_cache)
    : config(cfg), kv(store), owner(cfg, coordinator), spot(spot_owner), data(store), series(price_kv),
      swr(store, &swr_seams), execution(owner), workspaces(env),
      prices(spot_owner, history_service, control), worker(workspaces, control),
      revalidator(workspaces, control, env),
      jobs({workspaces, control, stats, data, series, swr, *store, owner, config, seams,
            spot_owner, popularity_tracker, spot_cache}),
      routes(env, workspaces, worker, cfg.attestation_required) {
  if (test_seams) seams = *test_seams;
  swr_seams.clock = seams.clock;
  swr_seams.jitter = seams.jitter;
  env.kv = kv;
  env.data = &data;
  env.swr = &swr;
  env.executor = &execution;
  env.stats = &stats;
  env.config = &config;
  env.revalidation_queue = &revalidator;
  env.spot = spot ? &prices : nullptr;
  env.series = history_service ? &prices : nullptr;
  env.etherscan_unsupported = &etherscan_unsupported;
  env.tron_flights = fortis::tokens::token_tron::NewFlights();
  env.bittensor = fortis::tokens::token_bittensor::NewWorkingSet();
  env.saucerswap = !seams.saucerswap_disabled;
  robinhood.Configure(&robinhood_fetcher, &swr_clock);
  env.robinhood = &robinhood;
  robinhood_catalog.fresh = &Impl::FreshRobinhoodCatalog;
  robinhood_catalog.adopt = &Impl::AdoptRobinhoodCatalog;
  robinhood_catalog.context = this;
  // registry options.rpcUrls: Alchemy's Robinhood endpoint with a key.
  if (cfg.alchemy_api_key_configured) {
    const int written = snprintf(robinhood_rpc_url, sizeof(robinhood_rpc_url),
                                 "https://robinhood-mainnet.g.alchemy.com/v2/%s", cfg.alchemy_api_key);
    if (written > 0 && static_cast<size_t>(written) < sizeof(robinhood_rpc_url)) {
      robinhood_rpc_urls[0] = robinhood_rpc_url;
      env.robinhood_rpc_urls = robinhood_rpc_urls;
      env.robinhood_rpc_url_count = 1;
    }
  }
}
TokenService::Impl::~Impl() {
  fortis::tokens::token_tron::DeleteFlights(env.tron_flights);
  fortis::tokens::token_bittensor::DeleteWorkingSet(env.bittensor);



}

bool TokenService::Impl::FreshRobinhoodCatalog(void* opaque, providers::RobinhoodAssetCatalog* out) {
  return static_cast<Impl*>(opaque)->robinhood.FreshCatalog(out);
}
bool TokenService::Impl::AdoptRobinhoodCatalog(void* opaque, const uint8_t* body, size_t length,
                                               providers::RobinhoodAssetCatalog* out) {
  return static_cast<Impl*>(opaque)->robinhood.AdoptCatalog(body, length, out);
}

// ============================================================================
// Lifecycle
// ============================================================================

void TokenService::ImplDeleter::operator()(Impl* impl) const { fortis::base::Delete(impl); }

TokenService::~TokenService() { Stop(); }

TokenService::TokenService(Impl* impl) : impl_(impl) {}
TokenService::TokenService(TokenService&&) noexcept = default;

base::Result<TokenService> TokenService::Create(
    const config::ProviderRuntimeConfig& config, TokenServiceDeps deps, const TokenServiceOptions& options) try {
  auto& kv = deps.kv;
  auto& coordinator = deps.coordinator;
  auto* start_owner = options.start_owner;
  auto* seams = options.seams;
  auto* spot = deps.spot;
  auto* popularity = deps.popularity;
  auto* history = deps.history;
  auto* price_cache = deps.price_cache;
  if (!coordinator.valid()) return base::Status(base::StatusCode::kUnavailable, "token initialization");
  Impl* impl = fortis::base::New<Impl>(config, &kv, &coordinator, spot, seams, deps.price_kv, popularity, history, price_cache);
  if (!impl) return base::Status(base::StatusCode::kResourceExhausted, "token allocation");
  struct InitGuard {
    Impl* impl;
    bool catalog_adopted = false;
    ~InitGuard() {
      if (!impl) return;
      if (catalog_adopted) impl->spot->SetRobinhoodCatalog(nullptr);
      impl->owner.Stop();
      base::Delete(impl);
    }
  } guard{impl};
  const bool started = impl->data.valid() && impl->stats.valid() &&
                       (start_owner ? start_owner->Start(impl->owner) : impl->owner.Start());
  if (!started) {
    return base::Status(base::StatusCode::kUnavailable, "token initialization");
  }
  if (spot) {
    spot->SetRobinhoodCatalog(&impl->robinhood_catalog);
    guard.catalog_adopted = true;
  }
  // Keep the seven-day coin popularity ranking across restarts. Restore
  // reads the KV only; the first scheduled refresh is a random point of its
  // minute, as RecurringJob.start() places it.
  if (popularity) fortis::prices::coin_prices::Restore(kv, *popularity, impl->Now());
  guard.impl = nullptr;
  return TokenService(impl);
} catch (const std::bad_alloc&) {
  return base::Status(base::StatusCode::kResourceExhausted, "token allocation");
}

bool TokenService::Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff) {
  if (!impl_->worker.BindHandoff(handoff)) return false;
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) return false;
  bool ok = pthread_attr_setstacksize(&attributes, 512 * 1024) == 0 &&
      impl_->worker.Start(&attributes);
  if (ok) ok = impl_->revalidator.Start(&attributes);
  if (ok) ok = impl_->jobs.Start(&attributes);
  pthread_attr_destroy(&attributes);
  return ok && impl_->routes.Register(router);
}

void TokenService::Stop() {
  if (!impl_) return;
  // The spot owner keeps its own catalog again before the client goes.
  if (impl_->spot) impl_->spot->SetRobinhoodCatalog(nullptr);
  impl_->control.Stop();
  // Stop the provider owner first: it cancels every in-flight fetch, so a job
  // or worker blocked in FetchAll (a hung provider) returns at once instead
  // of holding the joins below until the provider answers. Later FetchAll
  // calls fail fast.
  impl_->owner.Stop();
  impl_->worker.Join();
  impl_->revalidator.Join();
  impl_->jobs.Join();
  impl_->worker.DiscardPending();
  impl_->revalidator.DiscardPending();
  // The final flush of the TypeScript onClose hook.
  bool failed = false;
  impl_->stats.Flush(&impl_->data, impl_->Now(), &failed);
  impl_.reset();
}

fortis::tokens::token_jobs::TokenStats* TokenService::stats() { return &impl_->stats; }

bool TokenService::RunJobNow(Job job, char* summary, size_t capacity) {
  return impl_->jobs.RunJob(job, summary, capacity);
}

bool TokenService::WaitIdle(uint32_t timeout_ms) { return impl_->revalidator.WaitIdle(timeout_ms); }

bool TokenRoutes::Register(http::HttpRouter& router) {
  return router.Add<&TokenRoutes::HandlePage>("GET", "/v1/networks/:network/token/:tokenId/page", *this) &&
         router.Add<&TokenRoutes::HandleToken360>("GET", "/v1/networks/:network/token/:tokenId", *this) &&
         router.Add<&TokenRoutes::HandleHolders>("GET", "/v1/networks/:network/token/:tokenId/holders/history", *this) &&
         router.Add<&TokenRoutes::HandleLiquidityHistory>("GET", "/v1/networks/:network/token/:tokenId/liquidity/history", *this) &&
         router.Add<&TokenRoutes::HandlePools>("GET", "/v1/networks/:network/token/:tokenId/liquidity/pools", *this) &&
         router.Add<&TokenRoutes::HandleLiquiditySummary>("GET", "/v1/networks/:network/token/:tokenId/liquidity/summary", *this);
}

}  // namespace fortis::tokens
