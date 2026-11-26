#include "fixed_string.h"
#include "threading.h"
#pragma once

// Internal to the token service and its route units (token_service.cpp,
// token_service_jobs.cpp, token_route_support.cpp, token_task_routes.cpp,
// token_history_routes.cpp): the service implementation's layout, the queued
// task and the request/reply helpers the route handlers share. Nothing
// outside those units includes this.
#include "token_service.h"
#include "token_runtime.h"

#include "coin_price_scheduler.h"
#include "device_auth.h"
#include "device_enrollment.h"
#include "kv_store.h"
#include "price_history_batch_codec.h"
#include "price_history_service.h"
#include "price_jobs.h"
#include "price_series_store.h"
#include "provider_execution_coordinator.h"
#include "robinhood_markets.h"
#include "robinhood_price.h"
#include "spot_provider_owner.h"
#include "token_bittensor.h"
#include "token_data.h"
#include "token_facts.h"
#include "token_history.h"
#include "token_jobs.h"
#include "token_page.h"
#include "token_provider.h"
#include "token_robinhood.h"
#include "token_swr.h"
#include "token_text.h"
#include "token_tron.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <new>

namespace fortis::tokens {

using fortis::tokens::token_facts::Work;
using fortis::tokens::token_facts::FactsOutcome;
using codec::tj::Value;

namespace token_service_internal {

constexpr uint32_t kCost = 5;
constexpr size_t kJobArenaBytes = 8 * 1024 * 1024;

// Measured (tests/token_history_test.cpp --measure): a full 90-day hourly
// liquidity chart and 400 days of three-source counts with 300 ranked holders
// on each comparison day stay well under this growing-arena bound.
constexpr size_t kHistoryArenaBytes = 16 * 1024 * 1024;
constexpr size_t kScratchBytes = 1024 * 1024 + 64;
constexpr size_t kResponseBytes = 2 * 1024 * 1024;
constexpr uint32_t kHandoffTimeoutMs = 30000;
constexpr uint64_t kTaskBudgetMs = 25000;
constexpr uint64_t kJobBudgetMs = 55000;
constexpr size_t kEtherscanUnsupported = 32;
constexpr const char* kJsonType = "application/json; charset=utf-8";
const char* const kMinuteCacheControl = "private, max-age=60";

timespec wake_after(uint64_t ms);
void send_text(int client, int status, const char* reason, const char* body, bool head, int cost,
               uint32_t remaining);
size_t bad_request_body(const char* detail, char* out, size_t capacity);
void send_bad_request(int client, const char* detail, bool head, int cost, uint32_t remaining);
bool reply_policy(int client, const auth::DevicePolicyResult& policy, bool head);
const char* const kUpstreamUnavailable = "{\"error\":\"UPSTREAM_UNAVAILABLE\"}";

const char* const kBusy =
    "{\"error\":\"RATE_LIMITED\",\"detail\":\"Upstream data is temporarily busy; retained data remains available.\"}";

}  // namespace token_service_internal
using namespace token_service_internal;

// ============================================================================
// Service internals
// ============================================================================

struct TokenTask {
  enum Kind : uint8_t { kPage, kToken360, kPools };
  Kind kind = kPage;
  uint64_t token = 0;
  bool head = false;
  bool protobuf = false;
  bool has_if_none_match = false;
  base::FixedString<160> if_none_match;
  int cost = 5;
  uint32_t remaining = 0;
  base::FixedString<260> network;
  size_t network_length = 0;
  base::FixedString<650> ref;
  size_t ref_length = 0;
  fortis::tokens::token_page::PageOptions page{};
  bool has_hint = false;
  base::FixedString<128> hint;
};

struct TokenService::Impl {
  config::ProviderRuntimeConfig config{};
  fortis::storage::KvStore* kv = nullptr;
  providers::TokenProviderOwner owner;
  prices::SpotProviderOwner* spot = nullptr;
  fortis::tokens::token_data::TokenDataStore data;
  fortis::prices::PriceSeriesStore series;
  fortis::tokens::token_swr::Seams swr_seams{};
  fortis::tokens::token_swr::Cache swr;
  fortis::tokens::token_jobs::TokenStats stats;
  // The Robinhood client's fetcher and wall clock (the cache clock,
  // Impl::Now), declared before the client so they outlive it.
  fortis::tokens::token_robinhood::OwnerFetcher robinhood_fetcher{&owner};
  fortis::tokens::token_swr::CacheClock swr_clock{&swr};
  // Token360's RobinhoodStockTokenClient: its 15-minute catalog is shared
  // with the price canary's spot owner (SetRobinhoodCatalog), as PricesFacet
  // and Token360 share one client in TypeScript.
  fortis::exchange::robinhood::StockClient robinhood;
  prices::SpotRobinhoodCatalog robinhood_catalog{};
  char robinhood_rpc_url[320]{};
  const char* robinhood_rpc_urls[1]{};
  fortis::tokens::token_facts::Env env{};
  TokenServiceSeams seams{};

  chains::BoundedChainIdSet<kEtherscanUnsupported> etherscan_unsupported;

  token_facts::ProviderOwnerExecution execution;
  TokenRuntimeControl control;
  TokenWorkspaces workspaces;
  TokenPriceAccess prices;
  TokenRequestWorker worker;
  TokenRevalidator revalidator;
  TokenJobScheduler jobs;
  TokenRoutes routes;

  Impl(const config::ProviderRuntimeConfig& cfg, fortis::storage::KvStore* store, providers::ProviderExecutionCoordinator* coordinator,
       prices::SpotProviderOwner* spot_owner, const TokenServiceSeams* test_seams, fortis::storage::KvStore& price_kv,
       discovery::CoinPopularity* popularity_tracker, prices::PriceHistoryService* history_service,
       prices::PriceSpotCache* spot_cache);
  ~Impl();

  int64_t Now() const { return swr.Now(); }
  static bool FreshRobinhoodCatalog(void* opaque, providers::RobinhoodAssetCatalog* out);
  static bool AdoptRobinhoodCatalog(void* opaque, const uint8_t* body, size_t length, providers::RobinhoodAssetCatalog* out);

};

namespace token_service_internal {

// ---- handlers -----------------------------------------------------------------

TokenTask* new_task(TokenTask::Kind kind, const http::HttpRequest& request, const auth::DevicePolicyResult& policy);
void dispatch(TokenRequestWorker& owner, int client, bool head, TokenTask* task);
bool copy_params(TokenTask* task, const char* network, size_t network_length, const char* token, size_t token_length);
bool reject_long_params(int client, const http::HttpRequest& request, bool head);
bool reject_raw_non_ascii(int client, const http::HttpRequest& request, bool head);
size_t path_param(const http::HttpRequest& request, const char* name, char* out, size_t capacity);
void send_detail(int client, const char* prefix, const char* text, size_t length, bool head, int cost,
                 uint32_t remaining);
bool history_params(const http::HttpRequest& request, char (&network)[520], size_t* network_length, char (&token)[520],
                    size_t* token_length);

// The first value of each `names[i]` (fast-querystring over the whole
// target) in buffers as long as the target, so a padded or zero-led value
// and an unrelated long key parse as in Node. Values are NUL-terminated.
class TargetQuery {
 public:
  TargetQuery(const char* target, const char* const* names, size_t count);
  ~TargetQuery() = default;
  TargetQuery(const TargetQuery&) = delete;
  TargetQuery& operator=(const TargetQuery&) = delete;
  bool ok() const { return memory_ != nullptr && count_ <= kMax; }
  bool valid() const { return valid_; }
  size_t capacity() const { return capacity_; }
  size_t count(size_t i) const { return counts_[i]; }
  const char* value(size_t i) const { return values_[i]; }
  size_t length(size_t i) const { return lengths_[i]; }

 private:
  static constexpr size_t kMax = 4;
  size_t count_;
  bool valid_ = false;
  size_t capacity_ = 0;
  std::unique_ptr<char[], base::StorageDeleter> memory_;
  char* values_[kMax]{};
  size_t counts_[kMax]{};
  size_t lengths_[kMax]{};
};

bool facet_resolve(int client, bool head, int cost, uint32_t remaining, const char* network, size_t network_length,
                   const char* token, size_t token_length);

}  // namespace token_service_internal

}  // namespace fortis::tokens
