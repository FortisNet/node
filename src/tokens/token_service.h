#pragma once

#include <memory>
#include "status.h"

#include "clock.h"
#include "ownership.h"
#include "http_server.h"
#include "provider_runtime_config.h"
#include "seams.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::storage { class KvStore; }
namespace fortis::discovery { class CoinPopularity; }
namespace fortis::prices { class PriceSpotCache; }
namespace fortis::tokens::token_jobs { class TokenStats; }
namespace fortis::prices { class PriceHistoryService; }
namespace fortis::providers { class ProviderExecutionCoordinator; }
namespace fortis::prices { class SpotProviderOwner; }
namespace fortis::providers { class TokenProviderOwner; }

namespace fortis::tokens {

// Normal startup passes none and uses TokenProviderOwner::Start(). A replay
// binary supplies a start that redirects the owner
// (TokenProviderOwner::StartRedirected).
using TokenOwnerStarter = fortis::base::Starter<providers::TokenProviderOwner>;

// Test seams. A normal composition passes none: the wall clock, CSPRNG
// jitter and the recurring jobs on their own scheduler thread.
struct TokenServiceSeams {
  // The wall clock (a negative NowMs means the real one) and the jitter.
  fortis::base::Clock* clock = nullptr;
  fortis::base::JitterSource* jitter = nullptr;
  // True: no scheduler thread; jobs run only through RunJobNow.
  bool manual_jobs = false;
  // True: Token360 without its SaucerSwap client, as fixtures frozen
  // before the SaucerSwap port build their TypeScript apps.
  bool saucerswap_disabled = false;
};

// Token, holder and liquidity routes behind the price canary
// (FORTIS_NATIVE_PRICE_ROUTE_CANARY, which requires a validated CA bundle):
//   GET /v1/networks/:network/token/:tokenId               (legacy Token 360)
//   GET /v1/networks/:network/token/:tokenId/page
//   GET /v1/networks/:network/token/:tokenId/holders/history
//   GET /v1/networks/:network/token/:tokenId/liquidity/history
//   GET /v1/networks/:network/token/:tokenId/liquidity/pools
//   GET /v1/networks/:network/token/:tokenId/liquidity/summary
// The three history routes read only the KV and answer on the HTTP worker.
// The provider-backed routes are queued (16 slots) to two token workers that
// block on provider batches and answer through the response handoff (a page
// with include=series also waits there on the price history backfill owner
// for missing CoinGecko candle ranges, bounded by the 25 s task budget); one
// background worker runs stale-while-revalidate refreshes, and one scheduler
// thread runs the recurring jobs (token-stat flush 60 s, demand projection
// 5 min, active/broad market warmers 60 s/15 min, liquidity warmer 5 min,
// holder warmer 6 h, liquidity rollup 1 h; each first run at a random point
// of its interval, later runs completion-scheduled with +-10 % jitter). The
// 5-minute demand job is the whole TypeScript demand job: one stats flush,
// the demand projection, PricesFacet.refreshActive() through the price
// canary's spot owner in background mode, then sealDueYielding() (200
// rows); see price_jobs.h. With a CoinPopularity tracker (the price canary
// composes one) the scheduler also runs the TypeScript coin
// price popularity schedule (60 s, coin_price_scheduler.h), and its daily
// retention prune; Start restores the durable ranking without provider calls.
// Robinhood Chain stock tokens read the issuer catalog and reference quotes
// through the service's own RobinhoodStockTokenClient (exchange::robinhood::StockClient
// over the token provider owner, token_robinhood.h); its 15-minute catalog
// is shared with `spot` (SpotProviderOwner::SetRobinhoodCatalog) while the
// service runs, as TypeScript's Token360 and PricesFacet share one client.
// Work arenas (8 MiB per foreground task or job, 6 MiB per revalidation,
// 4 MiB per history read, each plus a 1 MiB KV scratch and up to two 2 MiB
// response buffers) are allocated per task and freed after it. An idle
// service holds about 6.8 MiB: the provider owner (~1.1 MiB: 64 request
// slots and the 512 KiB response buffer), the token store's read-modify-write
// buffers (4.5 MiB) and the 10,000-bucket fetch-stat table (~1.2 MiB).
struct TokenServiceDeps {
  storage::KvStore& kv;
  providers::ProviderExecutionCoordinator& coordinator;
  storage::KvStore& price_kv;
  prices::SpotProviderOwner* spot = nullptr;
  prices::PriceHistoryService* history = nullptr;
  discovery::CoinPopularity* popularity = nullptr;
  prices::PriceSpotCache* price_cache = nullptr;
};
struct TokenServiceOptions {
  TokenOwnerStarter* start_owner = nullptr;
  const TokenServiceSeams* seams = nullptr;
};

// Factory-created resource handle. Moved-from handles are destruction-only.
class TokenService {
 public:
  // kActivePrices and kPriceSeal are the two price steps of kDemand on their
  // own, for manual runs (tests); the scheduler runs only kDemand.
  enum class Job : uint8_t {
    kStatsFlush, kDemand, kMarketActive, kMarketBroad, kLiquidity, kHolders, kRollup, kActivePrices, kPriceSeal,
    kCoinPrices, kCoinPopularityPrune
  };

  TokenService(TokenService&&) noexcept;
  TokenService& operator=(TokenService&&) = delete;
  ~TokenService();
  TokenService(const TokenService&) = delete;
  TokenService& operator=(const TokenService&) = delete;

  // `spot` is the price canary's spot owner (the token page quote reads the
  // shared price plane through it); it may be null, which turns the spot
  // fallback into "no hit". `popularity` (may be null) is the process coin
  // popularity tracker the price routes record into; it must outlive the
  // service. `history` (may be null) is the started price history service
  // whose store and backfill owner back the page `series` section; without
  // it a requested series answers errors.series UPSTREAM_UNAVAILABLE. Stop
  // this service before the history service.
  static base::Result<TokenService> Create(const config::ProviderRuntimeConfig& config, TokenServiceDeps deps,
                                      const TokenServiceOptions& options = {});
  bool Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff);
  fortis::tokens::token_jobs::TokenStats* stats();

  // Runs one job on the caller's thread (serialized with the scheduler).
  // `summary` receives a short JSON result.
  bool RunJobNow(Job job, char* summary, size_t capacity);
  // Waits until the revalidation queue is empty and idle (tests).
  bool WaitIdle(uint32_t timeout_ms);

  struct Impl;

 private:
  explicit TokenService(Impl* impl);
  void Stop();
  // Defined beside Impl: only that unit can destroy one.
  struct ImplDeleter { void operator()(Impl* impl) const; };
  std::unique_ptr<Impl, ImplDeleter> impl_;
};

}  // namespace fortis::tokens
