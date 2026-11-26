#pragma once

#include <memory>
#include "provider_execution.h"
#include "error_reporter.h"
#include "ownership.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::providers {

// Spot, history backfill, OHLC, discovery, token, news, the chain transport,
// the cross-chain and Robinhood transports, the DashPay transport, plus
// headroom. An owner record is a few dozen bytes; jobs are shared.
constexpr size_t kProviderCoordinatorMaxOwners = 12;
constexpr size_t kProviderCoordinatorCommandCapacity = 16;

using ProviderCoordinatorWakeFn = void (*)(void* context);

// Process-level CoinGecko execution owner. Exactly one thread touches the
// lane registry and adapter. Owner threads use bounded synchronous commands
// and drain copied, owner-specific results; they never call the adapter.
class ProviderExecutionCoordinator {
 public:
  /** errors is optional, borrowed and must outlive the stopped coordinator.
   * Final dependency failures are reported once per job, without payloads.
   */
  explicit ProviderExecutionCoordinator(bool keyless_coingecko = false,
                                       base::ErrorReporter* errors = nullptr);
  ~ProviderExecutionCoordinator();
  ProviderExecutionCoordinator(const ProviderExecutionCoordinator&) = delete;
  ProviderExecutionCoordinator& operator=(const ProviderExecutionCoordinator&) = delete;

  bool valid() const;
  bool Start();
  // All owner leases must be unregistered before Stop. A false return leaves
  // the coordinator running so callers can stop owners first.
  bool Stop();
  uint64_t RegisterOwner(ProviderCoordinatorWakeFn wake, void* context);
  bool UnregisterOwner(uint64_t owner_id);
  ProviderExecutionSubmission Submit(uint64_t owner_id, const char* lane,
      const http::OutboundRequest& request, void* context,
      const ProviderExecutionOptions& options);
  bool Cancel(uint64_t owner_id, uint64_t job_id);
  ProviderExecutionTakeStatus TryTakeResult(uint64_t owner_id,
      ProviderExecutionResult* output, unsigned char* response_body,
      size_t response_capacity);
  size_t live_job_count(uint64_t owner_id) const;
  size_t in_flight_count() const;

 private:
  struct Impl;
  std::unique_ptr<Impl, fortis::base::ObjectDeleter<Impl>> impl_;
  const bool keyless_coingecko_;
};

}  // namespace fortis::providers
