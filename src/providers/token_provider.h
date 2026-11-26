#pragma once

#include <memory>
#include "provider_runtime_config.h"
#include "ownership.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::providers {

class ProviderExecutionCoordinator;

// The upstreams of the token routes and jobs, each on its TypeScript lane
// with its TypeScript host and credential:
//  - CoinGecko ("coingecko"): api.coingecko.com, or pro-api.coingecko.com
//    for a pro key, with x-cg-demo-api-key / x-cg-pro-api-key;
//  - DexScreener ("dexscreener"): api.dexscreener.com, keyless;
//  - GoPlus ("goplus"): api.gopluslabs.io, keyless;
//  - Ethplorer ("ethplorer"): api.ethplorer.io, the key in the path query;
//  - Etherscan V2 ("etherscan"): api.etherscan.io, the key in the query;
//  - Jupiter ("jupiter"): api.jup.ag with x-api-key for a configured
//    JUPITER_API_KEY, else the keyless lite-api.jup.ag (JupiterClient);
//  - TronScan ("tronscan"): apilist.tronscanapi.com with TRON-PRO-API-KEY
//    for a configured TRONSCAN_API_KEY unless the fetch is `anonymous`
//    (TronscanClient.get's retry after a rejected key);
//  - TronGrid ("trongrid"): POST {rpc_url}{path} to one of the network's
//    rpc_urls with TRON-PRO-API-KEY for a configured TRONGRID_API_KEY
//    (DriverTransport.postJson on the Tron driver's lane), failing over
//    like kRpc and never retried in place;
//  - Horizon ("horizon"), Koios ("koios"), algod ("algod") and the Hedera
//    Mirror Node ("hedera-mirror"): keyless GET {rpc_url}{path} on one of the
//    network's rpc_urls (DriverTransport.getJson on the Stellar, Cardano,
//    Algorand and Hedera drivers' lanes). HttpClient's three
//    attempts per endpoint; a transport failure, 5xx or busy lane fails over
//    to the next endpoint, any 4xx (429 included) is the answer;
//  - SaucerSwap ("saucerswap"): api.saucerswap.finance (kSaucerSwap) or
//    test-api.saucerswap.finance (kSaucerSwapTestnet), with x-api-key for a
//    configured SAUCERSWAP_API_KEY (SaucerSwapClient.getOrNull; keyless
//    works). Every request of one process carries the same credential
//    class, and the host is fixed by the upstream, so the in-flight key
//    (upstream, path) never shares a mainnet answer with testnet;
//  - EVM JSON-RPC (kRpc): POST to one of the network's rpc_urls on lane
//    "rpc:{host}" (DriverTransport.jsonRpc), never retried in place;
//  - shared JSON-RPC (kJsonRpc, the XRPL driver): the same POST on lane
//    "rpc:{host}", single attempt, with DriverTransport.jsonRpc's failover:
//    any non-2xx answer, transport failure or busy lane moves on to the next
//    endpoint (the caller fails over an unparseable or null 2xx body);
//  - Sui GraphQL (kGraphql, the Sui driver's coinMetadata): keyless POST
//    {graphql_url}{path} (the path is empty) to one of the network's GraphQL
//    endpoints on lane "rpc:{host}" (DriverTransport.postJson with
//    `retry: true`): HttpClient's three attempts per endpoint (retryPost);
//    a transport failure, 5xx or busy lane fails over, any 4xx (429
//    included) is the answer, exactly like the network GETs;
//  - Taostats ("taostats", kTaostats, the Bittensor driver's subnet pool and
//    TAO/USD reads): GET {rpc_url}{path} on one of the network's rpc_urls
//    with the configured TAOSTATS_API_KEY as Authorization
//    (DriverTransport.getJson with the driver's laneFor and headers), the
//    network-GET policy above. Never sent without a key; the key is only a
//    request header (no response, trace or in-flight key carries it);
//  - Robinhood ("robinhood-assets", kRobinhood, RobinhoodStockTokenClient's
//    `/rhj/assets` catalog and `/rhj/prices/{symbol}` reference quotes):
//    api.robinhood.com, keyless, HttpClient's three GET attempts. The caller
//    (tokens::token_robinhood::OwnerFetch) retries an unparseable 2xx.
// Identical in-flight network GETs, kJsonRpc and kGraphql POSTs share one job
// only for the same network, endpoint host, port and path (and, for a POST,
// body).
enum class TokenUpstream : uint8_t {
  kCoinGecko, kDexScreener, kGoPlus, kEthplorer, kEtherscan, kJupiter, kTronScan, kTronGrid, kRpc,
  kHorizon, kKoios, kAlgod, kJsonRpc, kHederaMirror, kSaucerSwap, kSaucerSwapTestnet, kGraphql, kTaostats,
  kRobinhood
};
// Each upstream's lane, transport, host and credential rule is its row of
// kUpstreams in token_provider.cpp.

// Longest network id an in-flight key holds (TokenNetwork ids are shorter).
constexpr size_t kTokenMaxNetworkBytes = 128;

constexpr size_t kTokenMaxPathBytes = 4096;
constexpr size_t kTokenMaxRpcBodyBytes = 4096;
constexpr size_t kTokenMaxRpcUrls = 4;
// Per-request response bound: the provider adapter maximum. A CoinGecko
// /coins/{id} profile with 100 tickers measured about 200 KiB.
constexpr size_t kTokenMaxResponseBytes = 512 * 1024;

// One request of a FetchAll batch. Inputs are filled by the caller; outputs
// by FetchAll. Each body has a standard owner; TokenFetchRelease allows
// early release before the fetch is reused.
struct TokenFetch {
  TokenUpstream upstream = TokenUpstream::kCoinGecko;
  // GET: the request path with its query. kTronGrid, kGraphql and the
  // network GETs: the path appended to each endpoint (joinUrl). kRpc /
  // kJsonRpc: unused (the URL's own path).
  char path[kTokenMaxPathBytes]{};
  // Network GETs, kJsonRpc and kGraphql: the network id, part of the
  // in-flight key.
  const char* network = nullptr;
  // kRpc / kTronGrid / kJsonRpc / kGraphql / network GETs: the endpoints to
  // try in order (outages fail over) and, for the POSTs, the JSON request
  // body.
  // `first_url` is the endpoint FetchAll starts from (a caller retrying or
  // failing over one endpoint).
  const char* const* rpc_urls = nullptr;
  size_t rpc_url_count = 0;
  size_t first_url = 0;
  char rpc_body[kTokenMaxRpcBodyBytes]{};
  size_t rpc_body_length = 0;
  bool background = false;
  // kTronScan: send without the configured key.
  bool anonymous = false;

  // Results. `answered` is a provider HTTP response (any status).
  bool answered = false;
  int status = 0;
  bool transport_error = false;
  bool cancelled = false;
  // The lane refused or shed the job (open breaker, full queue, queue
  // timeout): the TypeScript LaneBusyError.
  bool lane_rejected = false;
  uint32_t retry_after_ms = 0;
  std::unique_ptr<uint8_t[], base::StorageDeleter> body;
  size_t body_length = 0;
  // The rpc_urls index that produced the result (network GETs read it to
  // retry or fail over an unparseable answer). Internal: the owner request
  // id.
  size_t next_url = 0;
  uint64_t request_id = 0;
  bool pending = false;
};

// Arena bytes are borrowed; this owner runs each constructed fetch's destructor
// before the caller rewinds or destroys its arena. The bodies own their bytes.
struct ArenaFetchDeleter {
  size_t count = 0;
  void operator()(TokenFetch* fetches) const noexcept { std::destroy_n(fetches, count); }
};

[[nodiscard]] inline std::unique_ptr<TokenFetch[], ArenaFetchDeleter>
ConstructArenaFetches(TokenFetch* storage, size_t count) {
  if (storage) std::uninitialized_value_construct_n(storage, count);
  return {storage, ArenaFetchDeleter{count}};
}

void TokenFetchRelease(TokenFetch* fetch);
// True for a 2xx answer with a body.
inline bool TokenFetchOk(const TokenFetch& fetch) {
  return fetch.answered && fetch.status >= 200 && fetch.status < 300;
}

// One coordinator lease shared by the token service. Identical GET requests
// in flight share one provider job (the TypeScript SWR single flight); a job
// runs at background lane priority only while every waiter is background.
// Network GETs share only within one network, endpoint host, port and path;
// kJsonRpc and kGraphql POSTs also need the same body. The other POSTs (kRpc,
// kTronGrid) are never shared.
class TokenProviderOwner {
 public:
  TokenProviderOwner(const config::ProviderRuntimeConfig& config, ProviderExecutionCoordinator* coordinator);
  ~TokenProviderOwner();
  TokenProviderOwner(const TokenProviderOwner&) = delete;
  TokenProviderOwner& operator=(const TokenProviderOwner&) = delete;
  bool Start();
  // Runs every fetch to its final result (an answer, a failure after the
  // last failover endpoint, or cancellation at `deadline_ms` monotonic).
  // Blocks the calling worker thread; never call it from an HTTP worker.
  void FetchAll(TokenFetch* fetches, size_t count, uint64_t deadline_ms);
  // Answers every pending request as cancelled, then releases the lease.
  void Stop();
  bool running() const;
  // Configured provider keys (the composition reads these to decide which
  // TypeScript sources exist, e.g. Etherscan only with a key).
  bool etherscan_configured() const;
  // Start() with every upstream sent to one endpoint, its CA file and
  // address policy (upstream_redirect.h); with `real_host_header`, each
  // request carries the host it would have dialled in that header.
  // Production composes with Start().
  bool StartRedirected(const char* host, const char* port, const char* ca,
                       bool (*allow_address)(void*, uint32_t), void* context,
                       const char* real_host_header = nullptr);
  // Before the owner starts: `spec` ("primary=url,url;...", at most four
  // entries) replaces the endpoints of network GETs, kJsonRpc and kGraphql
  // POSTs whose first endpoint is `primary`, so a replay can list a failing
  // endpoint first. False (no override kept) for a malformed spec. Production
  // never calls it: endpoints come from the network catalog.
  bool SetNetworkEndpoints(const char* spec);

 private:
  struct Impl;
  std::unique_ptr<Impl, fortis::base::ObjectDeleter<Impl>> impl_;
};

uint64_t TokenMonotonicMs();

// Splits "https://host[:port]/path" into its parts (a missing path is "/").
// False for any other scheme, userinfo, an empty host or oversized parts.
bool TokenSplitHttpsUrl(const char* url, char* host, size_t host_capacity, char* port,
                        size_t port_capacity, char* path, size_t path_capacity,
                        char* lane_host, size_t lane_host_capacity);

}  // namespace fortis::providers
