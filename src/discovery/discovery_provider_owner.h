#pragma once

#include <memory>
#include "provider_runtime_config.h"
#include "ownership.h"
#include "provider_execution.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::providers { class ProviderExecutionCoordinator; }

namespace fortis::discovery {

// The upstreams the discovery routes read, each on its TypeScript lane
// with its TypeScript host and credential header:
//  - CoinGecko: api.coingecko.com or, for a pro-tier key, pro-api.coingecko.com
//    with x-cg-demo-api-key / x-cg-pro-api-key (lane "coingecko");
//  - Jupiter: api.jup.ag with x-api-key, or keyless lite-api.jup.ag ("jupiter");
//  - Taostats: api.taostats.io with the key as Authorization ("taostats");
//  - Robinhood stock tokens: api.robinhood.com, keyless ("robinhood-assets").
enum class DiscoveryUpstream : uint8_t { kCoinGecko, kJupiter, kTaostats, kRobinhood };

// The outbound transport's path bound.
constexpr size_t kDiscoveryMaxPathBytes = 8192;
// Per-job response storage: the adapter maximum. A /coins/markets body of 50
// rows with 7-day sparklines measured 217 KiB (100 rows: 433 KiB).
constexpr size_t kDiscoveryMaxResponseBytes = providers::kProviderExecutionMaxResponseBody;

struct DiscoveryProviderResult {
  int http_status = 0;
  uint32_t attempts = 0;
  uint32_t retry_after_ms = 0;
  bool transport_error = false;
  bool cancelled = false;
  // The lane refused or shed the job (open breaker, full queue, queue
  // timeout): the TypeScript LaneBusyError, back-pressure rather than a
  // provider verdict.
  bool lane_rejected = false;
};
// Called once per accepted Submit on the owner thread, never under the owner
// mutex. `body` is valid only for the duration of the call.
using DiscoveryProviderCompletionFn = void (*)(void* context,
    const DiscoveryProviderResult& result, const unsigned char* body, size_t body_length);

// One coordinator lease shared by every discovery route. Identical
// (upstream, path) requests in flight share one provider job, as the
// TypeScript in-process single flight does; the job runs at background lane
// priority only while every waiter is background.
class DiscoveryProviderOwner {
 public:
  DiscoveryProviderOwner(const config::ProviderRuntimeConfig& config, providers::ProviderExecutionCoordinator* coordinator);
  ~DiscoveryProviderOwner();
  DiscoveryProviderOwner(const DiscoveryProviderOwner&) = delete;
  DiscoveryProviderOwner& operator=(const DiscoveryProviderOwner&) = delete;
  bool Start();
  // `path` includes the query string. False (no callback) when the owner is
  // not running, every client slot is busy, or the request is invalid.
  // `retry_unparseable`: a 2xx whose body is not JSON is retried like a
  // transport failure (HttpClient.json); requests share a flight only with
  // the same setting.
  bool Submit(DiscoveryUpstream upstream, const char* path, uint64_t deadline_ms,
              bool background, DiscoveryProviderCompletionFn completion, void* context,
              uint64_t* request_id, bool retry_unparseable = false);
  bool Cancel(uint64_t request_id);
  // Answers every pending request as cancelled, then releases the lease.
  void Stop();
  bool running() const;
  // Start() with every upstream sent to one endpoint, its CA file and
  // address policy (upstream_redirect.h). Production composes with Start().
  bool StartRedirected(const char* host, const char* port, const char* ca,
                       bool (*allow_address)(void*, uint32_t), void* context);

 private:
  struct Impl;
  std::unique_ptr<Impl, fortis::base::ObjectDeleter<Impl>> impl_;
};

uint64_t DiscoveryMonotonicMs();

}  // namespace fortis::discovery
