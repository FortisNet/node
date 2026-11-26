#pragma once

#include "clock.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::providers {

constexpr size_t kProviderLaneNameCapacity = 128;
constexpr size_t kProviderLaneMaxActiveLanes = 64;
constexpr size_t kProviderLaneMaxQueuePerLane = 200;
constexpr size_t kProviderLaneMaxQueuedJobs = 1024;
constexpr size_t kProviderLaneMaxConcurrent = 8;

enum class ProviderBreakerState : uint8_t { kClosed, kOpen, kHalfOpen };
enum class ProviderLaneAdmissionStatus : uint8_t {
  kDispatched,
  kQueued,
  kQueueFull,
  kQueueTimeout,
  kBreakerOpen,
  kBackgroundBusy,
  kLaneCapacity,
  kClosed,
  kInvalidLane,
  kInvalidConfig,
  kUnknownJob,
};
enum class ProviderLaneOutcome : uint8_t {
  // Any completed answer, including 404 and other non-fault 4xx responses.
  kAnswered,
  // Timeout, socket/TLS failure, 408, 401/403, or 5xx.
  kFault,
  // 429: opens immediately and honors Retry-After.
  kRateLimited,
  // The caller cancelled the provider operation before reporting completion.
  // Matches TypeScript lane_queue: a non-UpstreamError rejection is an answer.
  kCancelled,
};

// Maps an HTTP result to lane health. The caller supplies transport_failure
// for connect/TLS/read failures; retry_after_ms is passed separately to Complete.
ProviderLaneOutcome ProviderLaneOutcomeForHttpStatus(int status,
                                                     bool transport_failure);
enum class ProviderLaneCancelReason : uint8_t {
  kQueueTimeout,
  kBreakerOpen,
  kRegistryClosed,
  kCallerCancelled,
};

struct ProviderLaneConfig {
  uint32_t requests_per_interval = 0;
  uint32_t interval_ms = 0;
  uint32_t burst = 0;
  uint32_t max_concurrent = 0;
  uint32_t max_queued = 0;
  uint32_t queue_timeout_ms = 0;
  uint32_t breaker_threshold = 5;
  uint32_t breaker_cooldown_ms = 30000;
};

struct ProviderLaneDefault {
  const char* name;
  ProviderLaneConfig config;
};

struct ProviderLaneDispatch {
  uint64_t job_id = 0;
  void* context = nullptr;
  bool half_open_probe = false;
};

struct ProviderLaneAdmission {
  ProviderLaneAdmissionStatus status = ProviderLaneAdmissionStatus::kInvalidConfig;
  uint64_t job_id = 0;
  uint64_t retry_after_ms = 0;
  bool has_dispatch = false;
  ProviderLaneDispatch dispatch{};
};

struct ProviderLaneCancelledJob {
  uint64_t job_id = 0;
  void* context = nullptr;
  ProviderLaneCancelReason reason = ProviderLaneCancelReason::kCallerCancelled;
};

struct ProviderLaneStats {
  uint32_t queued = 0;
  uint32_t cancelled_pending = 0;
  uint32_t in_flight = 0;
  uint64_t dispatched = 0;
  uint64_t answered = 0;
  uint64_t faults = 0;
  uint64_t rejected_queue_full = 0;
  uint64_t rejected_queue_timeout = 0;
  uint64_t rejected_breaker = 0;
  uint64_t rejected_background = 0;
  uint64_t breaker_opens = 0;
  uint32_t consecutive_faults = 0;
  ProviderBreakerState breaker = ProviderBreakerState::kClosed;
};


// The compact policy table mirrors the TypeScript implementation's lane table.
// rpc:{host} and news-feeds:{publisher} resolve to their corresponding defaults;
// other names use the same fallback as the TypeScript Lanes facade (rpc policy).
size_t ProviderLaneDefaultCount();
bool ProviderLaneDefaultAt(size_t index, ProviderLaneDefault* output);
bool ProviderLaneConfigValid(const ProviderLaneConfig& config);
bool ProviderLaneBudgetFor(const char* lane_name, ProviderLaneConfig* output);

// Nonblocking, fixed-memory lane state. This object is single-owner: every
// method must run on the same event-loop/coordination thread. HTTP workers and
// warmers must post admission/completion intents to that owner; they must not
// call this registry directly. The owner polls TryTakeNext/TryTakeCancelled and
// dispatches work to executors. This avoids locks and callbacks on request
// workers while keeping provider operations asynchronous.
//
// Store the registry at static or heap lifetime (not on an HTTP worker stack).
// Successful admission transfers context
// ownership to the registry until TryTakeNext() dispatches it or
// TryTakeCancelled() returns it. A returned dispatch transfers context ownership
// to the caller/executor; the caller must Complete() its lease after the provider
// operation ends. For active cancellation, cancel the operation first, then
// Complete(..., kCancelled). No method waits, sleeps, invokes provider code, or
// blocks an HTTP worker.
class ProviderLaneRegistry {
 public:
  // `clock` supplies MonotonicMs; null makes the registry invalid.
  explicit ProviderLaneRegistry(fortis::base::Clock* clock, bool keyless_coingecko = false);

  bool valid() const { return valid_; }
  ProviderLaneAdmission TryAdmit(const char* lane_name, void* context,
                                 bool background = false,
                                 bool has_caller_deadline = false,
                                 uint32_t caller_deadline_ms = 0);
  bool TryTakeNext(const char* lane_name, ProviderLaneDispatch* output);
  bool TryTakeCancelled(ProviderLaneCancelledJob* output);
  bool CancelQueued(const char* lane_name, uint64_t job_id);
  bool Complete(const char* lane_name, uint64_t job_id,
                ProviderLaneOutcome outcome, uint32_t retry_after_ms = 0);
  // Absolute monotonic ms for the next token, queued deadline, or open->probe
  // transition. UINT64_MAX means external completion/admission is required.
  uint64_t NextWakeupMs(const char* lane_name);
  bool Stats(const char* lane_name, ProviderLaneStats* output);
  void Close();
  size_t active_lane_count() const;
  size_t queued_job_count() const;
  size_t in_flight_count() const;

 private:
  enum class QueuedState : uint8_t { kFree, kQueued, kCancelled };
  struct LaneState {
    bool used;
    char name[kProviderLaneNameCapacity];
    ProviderLaneConfig config;
    double tokens;
    uint64_t last_refill_ms;
    uint64_t open_until_ms;
    uint64_t dispatched;
    uint64_t answered;
    uint64_t faults;
    uint64_t rejected_queue_full;
    uint64_t rejected_queue_timeout;
    uint64_t rejected_breaker;
    uint64_t rejected_background;
    uint64_t breaker_opens;
    uint32_t queued;
    uint32_t cancelled_pending;
    uint32_t in_flight;
    uint32_t consecutive_faults;
    ProviderBreakerState breaker;
    bool probe_in_flight;
  };
  struct QueuedJob {
    QueuedState state;
    size_t lane_index;
    uint64_t job_id;
    uint64_t sequence;
    uint64_t deadline_ms;
    void* context;
    ProviderLaneCancelReason cancel_reason;
  };
  struct ActiveJob {
    bool used;
    bool probe;
    size_t lane_index;
    uint64_t job_id;
  };

  fortis::base::Clock* clock_;
  const bool keyless_coingecko_;
  bool valid_;
  bool closed_;
  uint64_t next_job_id_;
  uint64_t next_sequence_;
  LaneState lanes_[kProviderLaneMaxActiveLanes];
  QueuedJob queued_jobs_[kProviderLaneMaxQueuedJobs];
  ActiveJob active_jobs_[kProviderLaneMaxActiveLanes * kProviderLaneMaxConcurrent];

  int FindLane(const char* lane_name) const;
  int GetOrCreateLane(const char* lane_name);
  bool ReserveActive(int lane_index, uint64_t job_id, bool probe);
  bool StartDispatch(int lane_index, uint64_t job_id, void* context,
                     ProviderLaneDispatch* output);
  void DispatchQueued(int lane_index, int queue_index, ProviderLaneDispatch* output);
  void Refill(LaneState* lane, uint64_t now);
  bool CanDispatch(LaneState* lane, uint64_t now);
  void AdvanceBreaker(LaneState* lane, uint64_t now);
  void OpenBreaker(int lane_index, uint64_t duration_ms);
  void ExpireQueued(int lane_index, uint64_t now);
  void MarkCancelled(int queue_index, ProviderLaneCancelReason reason);
};

const char* ProviderLaneAdmissionName(ProviderLaneAdmissionStatus status);

}  // namespace fortis::providers
