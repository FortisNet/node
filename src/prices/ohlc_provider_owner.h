#pragma once

#include <memory>
#include "provider_runtime_config.h"
#include "ownership.h"
#include "provider_execution.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::providers { class ProviderExecutionCoordinator; }

namespace fortis::prices {

struct OhlcProviderResult {
  int http_status = 0;
  uint32_t attempts = 0;
  uint32_t retry_after_ms = 0;
  bool transport_error = false;
  bool cancelled = false;
  // The lane cancelled the job for a non-caller reason (shed background work,
  // queue timeout, open breaker): back-pressure rather than a provider verdict.
  bool lane_rejected = false;
  // With `cancelled`: the request's or flight's deadline passed. That is a
  // provider verdict (HttpClient's timeout), unlike a caller cancellation or
  // shutdown.
  bool deadline_exceeded = false;
};
using OhlcProviderCompletionFn = void (*)(void* context,
    const OhlcProviderResult& result, const unsigned char* body, size_t body_length);

// True when a 2xx body must be retried as a transport failure (a body that is
// not JSON: HttpClient.json's parse failure). Called on the owner thread.
using OhlcBodyRetryFn = bool (*)(const unsigned char* body, size_t body_length);

// The GET /v1/prices/ohlc CoinGecko provider owner, composed by
// PriceHistoryService behind the price canary. It leases the process-wide
// coordinator and never creates a second adapter or lane registry.
class OhlcProviderOwner {
 public:
  OhlcProviderOwner(const config::ProviderRuntimeConfig& config, providers::ProviderExecutionCoordinator* coordinator);
  ~OhlcProviderOwner();
  OhlcProviderOwner(const OhlcProviderOwner&) = delete;
  OhlcProviderOwner& operator=(const OhlcProviderOwner&) = delete;
  bool Start();
  bool Submit(const char* slug, int days, uint64_t deadline_ms,
      OhlcProviderCompletionFn completion, void* context, uint64_t* request_id,
      bool background = false);
  bool Cancel(uint64_t request_id);
  // Installs the 2xx body check. A flight whose 2xx body fails it is asked
  // again after 250 ms and then 1 s (HttpClient RETRY_DELAYS_MS); the third
  // answer is delivered as it is.
  void SetBodyRetryCheck(OhlcBodyRetryFn check);
  void Stop();
  // Start() with every provider request sent to one endpoint
  // (upstream_redirect.h). Production composes with Start().
  bool StartRedirected(const char* host, const char* port, const char* ca,
      bool (*allow_address)(void*, uint32_t), void* context);
#ifdef FORTIS_HISTORY_BACKFILL_TEST_HOOKS
  // Instrumentation for tests/ohlc_provider_owner_test.cpp only: runs as a
  // call is admitted. Compiled out of the production object.
  void SetAdmissionHookForTest(void (*hook)(void*, uint64_t), void* context);
#endif
 private:
  struct Impl;
  std::unique_ptr<Impl, fortis::base::ObjectDeleter<Impl>> impl_;
};

}  // namespace fortis::prices
