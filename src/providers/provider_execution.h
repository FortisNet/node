#pragma once

#include <memory>
#include "outbound_https.h"
#include "ownership.h"
#include "status.h"
#include <optional>
#include "provider_lane.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::providers {

constexpr size_t kProviderExecutionMaxJobs = 8;
// An EVM Multicall3 balance read for the 200 tracked tokens
// TypeScript accepts, and a 500-sub-call change-detection fingerprint, are
// one eth_call body each of up to ~250 KiB of hex; the outbound transport
// cap is 256 KiB too.
constexpr size_t kProviderExecutionMaxRequestBody = 256 * 1024;
// Per-job response storage in the adapter and coordinator. Only the price
// history owner requests more than the default (a ten-year CoinGecko
// market_chart/range body is at most ~450 KiB); spot, OHLC and the bridge
// keep requesting, and buffering, the 256 KiB default.
constexpr size_t kProviderExecutionMaxResponseBody = 512 * 1024;
constexpr size_t kProviderExecutionDefaultResponseBody = 256 * 1024;
static_assert(kProviderExecutionDefaultResponseBody <= kProviderExecutionMaxResponseBody,
              "default provider body must fit the adapter's storage");
constexpr uint32_t kProviderExecutionMaxAttempts = 3;

enum class ProviderExecutionSubmitStatus : uint8_t {
  kAccepted,
  kBusy,
  kInvalidRequest,
  kDeadline,
  kLaneRejected,
  kStopping,
};

struct ProviderExecutionOptions {
  // Absolute deadline from the same monotonic clock passed to the adapter.
  // Submit rejects deadlines more than 120 seconds in the future.
  uint64_t deadline_ms = 0;
  uint32_t max_attempts = 3;
  // Required to retry POST. GET is considered idempotent by default.
  bool post_is_idempotent = false;
  // HttpClient.json's parse failure: a 2xx whose body is not JSON text is a
  // status-less UpstreamError, retried like a transport failure inside the
  // same lane call and, when the last attempt is still unparseable, settled
  // as a provider fault. Off: any 2xx is the answer.
  bool retry_unparseable_json = false;
  bool background = false;
};

struct ProviderExecutionSubmission {
  ProviderExecutionSubmitStatus status = ProviderExecutionSubmitStatus::kInvalidRequest;
  uint64_t job_id = 0;
  uint64_t retry_after_ms = 0;
  ProviderLaneAdmissionStatus lane_status = ProviderLaneAdmissionStatus::kInvalidConfig;
};
const char* ProviderExecutionSubmitStatusName(ProviderExecutionSubmitStatus status);

struct ProviderExecutionResult {
  uint64_t job_id = 0;
  void* context = nullptr;
  http::OutboundResponse response{};
  uint32_t attempts = 0;
  uint32_t retry_after_ms = 0;
  size_t body_length = 0;
  bool cancelled = false;
  bool deadline_exceeded = false;
  bool retry_suppressed_by_lane = false;
  bool lane_rejected = false;
  ProviderLaneCancelReason lane_cancel_reason = ProviderLaneCancelReason::kCallerCancelled;
};

enum class ProviderExecutionTakeStatus : uint8_t {
  kTaken,
  kNone,
  kBufferTooSmall,
};

// The clock (MonotonicMs) is called on owner and worker threads and must be
// thread-safe.
// Optional worker-thread notification. It must only signal/wake the owner
// loop, must be thread-safe, and must not call into the adapter or lane registry.
using ProviderExecutionWakeFn = void (*)(void* context);

// Bounded bridge between the single-owner lane event loop and blocking HTTPS.
// Submit/Poll/Cancel/NextWakeupMs/BeginShutdown/TryTakeResult must all be called
// on the lane registry's owner thread. Two private workers execute HTTPS; they
// never call the registry or user/provider callbacks. The adapter is the sole
// consumer of this registry's queued dispatch/cancellation events while active.
// Future HTTP routes need an asynchronous response/completion path: a handler
// that calls Submit() and waits for TryTakeResult would still pin an HTTP worker.
//
// Construct at static/heap lifetime. Submit copies the request and body; on
// acceptance the adapter owns context until TryTakeResult returns it. The
// response is copied into caller storage only when TryTakeResult succeeds;
// a too-small buffer leaves the result and output structure untouched.
// allow_address and policy_context are borrowed by the copied request and must
// remain valid until the final result is taken.
// BeginShutdown is nonblocking; the owner keeps polling and draining results,
// then a lifecycle thread calls JoinWorkers. All accepted contexts must be
// returned via TryTakeResult before destruction. A failed Create returns a Status and publishes no adapter. Destruction before every result is drained and workers are joined is
// a programming error and aborts rather than silently abandoning contexts.
// Cancellation cannot interrupt an active TLS
// syscall; the worker retains the lane lease until that attempt's deadline.
class ProviderExecutionAdapter {
 public:
  static base::Result<ProviderExecutionAdapter> Create(
      ProviderLaneRegistry& lanes, base::Clock& monotonic_clock,
      ProviderExecutionWakeFn wake_owner = nullptr, void* wake_context = nullptr);
  ProviderExecutionAdapter(ProviderExecutionAdapter&&) noexcept;
  ProviderExecutionAdapter& operator=(ProviderExecutionAdapter&&) = delete;
  ~ProviderExecutionAdapter();
  ProviderExecutionAdapter(const ProviderExecutionAdapter&) = delete;
  ProviderExecutionAdapter& operator=(const ProviderExecutionAdapter&) = delete;

  ProviderExecutionSubmission Submit(const char* lane_name,
                                     const http::OutboundRequest& request,
                                     void* context,
                                     const ProviderExecutionOptions& options);
  // Advances lane admissions, dispatches ready attempts to the worker pool,
  // settles completed attempts, and schedules safe retries without sleeping.
  size_t Poll();
  // Cancellation linearizes on the owner thread. Running TLS cannot be
  // interrupted; cancelling after a completion enters settlement returns false.
  bool Cancel(uint64_t job_id);
  uint64_t NextWakeupMs();
  ProviderExecutionTakeStatus TryTakeResult(ProviderExecutionResult* output,
                                            unsigned char* response_body,
                                            size_t response_capacity);
  void BeginShutdown();
  bool shutdown_started() const;
  // Blocking lifecycle operation. Call away from HTTP workers and owner loop.
  void JoinWorkers();
  bool workers_joined() const;
  size_t live_job_count() const;

 private:
  struct Impl;
  explicit ProviderExecutionAdapter(std::unique_ptr<Impl, fortis::base::ObjectDeleter<Impl>> impl);
  std::unique_ptr<Impl, fortis::base::ObjectDeleter<Impl>> impl_;
};

}  // namespace fortis::providers
