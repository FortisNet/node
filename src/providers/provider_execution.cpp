#include <algorithm>
#include <iterator>
#include <utility>
#include <memory>
#include <atomic>
#include "exception_boundary.h"
#include "provider_execution.h"
#include "log.h"

#include "threading.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <new>

namespace fortis::providers {

namespace {
constexpr size_t kHostCapacity = 254;
constexpr size_t kPortCapacity = 6;
constexpr size_t kPathCapacity = 8193;  // matches the outbound transport cap
constexpr size_t kCaPathCapacity = 1024;
constexpr uint32_t kRetryDelaysMs[] = {250, 1000};
constexpr uint32_t kMaxRetryDelayMs = 5000;
constexpr uint32_t kMaxRetryAfterMs = 60000;
constexpr uint32_t kMaxRateLimitRetryAfterMs = 3600000;
constexpr uint64_t kMaxTotalDurationMs = 120000;
}

struct ProviderExecutionAdapter::Impl {
  enum class State : uint8_t {
    kFree,
    kPreparing,
    kLaneQueued,
    kWorkerQueued,
    kRunning,
    kAttemptDone,
    kProcessing,
    kRetryWait,
    kResultReady,
  };

  struct Job {
    State state = State::kFree;
    uint64_t id = 0;
    uint64_t lane_job_id = 0;
    uint64_t deadline_ms = 0;
    uint64_t retry_at_ms = UINT64_MAX;
    void* context = nullptr;
    char lane[kProviderLaneNameCapacity]{};
    char host[kHostCapacity]{};
    char port[kPortCapacity]{};
    char path[kPathCapacity]{};
    char ca_file[kCaPathCapacity]{};
    unsigned char request_body[kProviderExecutionMaxRequestBody]{};
    unsigned char response_body[kProviderExecutionMaxResponseBody]{};
    http::OutboundRequest request{};
    http::OutboundResponse response{};
    ProviderExecutionOptions options{};
    uint32_t attempts = 0;
    uint32_t retry_after_ms = 0;
    bool lane_active = false;
    bool cancel_requested = false;
    // OutboundRequest::cancel of the running attempt: raised with
    // cancel_requested so an in-flight request ends promptly.
    std::atomic<int> abort_attempt{0};
    bool cancelled = false;
    bool deadline_exceeded = false;
    bool retry_suppressed_by_lane = false;
    bool lane_rejected = false;
    ProviderLaneCancelReason lane_cancel_reason = ProviderLaneCancelReason::kCallerCancelled;
  };

  static void ResetJob(Job* job) {
    job->state = State::kFree;
    job->id = 0;
    job->lane_job_id = 0;
    job->deadline_ms = 0;
    job->retry_at_ms = UINT64_MAX;
    job->context = nullptr;
    memset(job->lane, 0, sizeof(job->lane));
    memset(job->host, 0, sizeof(job->host));
    memset(job->port, 0, sizeof(job->port));
    memset(job->path, 0, sizeof(job->path));
    memset(job->ca_file, 0, sizeof(job->ca_file));
    memset(job->request_body, 0, sizeof(job->request_body));
    memset(job->response_body, 0, sizeof(job->response_body));
    job->request = http::OutboundRequest{};
    job->response = http::OutboundResponse{};
    job->options = ProviderExecutionOptions{};
    job->attempts = 0;
    job->retry_after_ms = 0;
    job->lane_active = false;
    job->cancel_requested = false;
    (job->abort_attempt).store(0, std::memory_order_release);
    job->cancelled = false;
    job->deadline_exceeded = false;
    job->retry_suppressed_by_lane = false;
    job->lane_rejected = false;
    job->lane_cancel_reason = ProviderLaneCancelReason::kCallerCancelled;
  }

  ProviderLaneRegistry* lanes;
  fortis::base::Clock* clock;
  ProviderExecutionWakeFn wake_owner;
  void* wake_context;
  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable work_ready;
  fortis::base::Thread workers[2];
  size_t worker_count = 0;
  bool valid = false;
  bool stopping = false;
  bool joining = false;
  bool joined = true;
  uint64_t next_id = 1;
  size_t queue_head = 0;
  size_t queue_count = 0;
  size_t round_robin = 0;
  Job jobs[kProviderExecutionMaxJobs];

  Impl(ProviderLaneRegistry* lane_registry, fortis::base::Clock* monotonic,
       ProviderExecutionWakeFn wake, void* wake_arg)
      : lanes(lane_registry), clock(monotonic),
        wake_owner(wake), wake_context(wake_arg) {
    if (!lanes || !lanes->valid() || !clock) return;
    for (size_t i = 0; i < 2; ++i) {
      if ((workers[i].StartEntry<WorkerEntry>(this, nullptr) ? 0 : -1) != 0) break;
      ++worker_count;
      joined = false;
    }
    valid = worker_count > 0;
    if (!valid) {
      stopping = true;
      work_ready.Broadcast();
    }
  }

  ~Impl() = default;

  static void* WorkerEntry(void* context) {
    static_cast<Impl*>(context)->WorkerLoop();
    return nullptr;
  }

  void WorkerLoop() {
    for (;;) {
      fortis::base::UniqueLock lock(mutex);
      while (queue_count == 0 && !stopping) work_ready.WaitLocked(mutex);
      if (queue_count == 0 && stopping) return;
      const size_t index = task_queue[queue_head];
      queue_head = (queue_head + 1) % kProviderExecutionMaxJobs;
      --queue_count;
      Job* job = &jobs[index];
      if (job->state != State::kWorkerQueued) continue;
      job->state = State::kRunning;
      const bool cancelled = job->cancel_requested;
      const uint64_t now_ms = clock->MonotonicMs();
      if (cancelled || now_ms >= job->deadline_ms) {
        job->cancelled = cancelled;
        job->deadline_exceeded = !cancelled;
        job->response = {};
        if (!cancelled) job->response.error = http::OutboundError::kTimeout;
        job->state = State::kAttemptDone;
        lock.Unlock();
        NotifyOwner();
        continue;
      }
      http::OutboundRequest request = job->request;
      request.cancel = &job->abort_attempt;
      const uint64_t remaining = job->deadline_ms - now_ms;
      if (remaining < request.timeout_ms) request.timeout_ms = static_cast<uint32_t>(remaining);
      ++job->attempts;
      lock.Unlock();

      const http::OutboundResponse response = OutboundHttpsRequest(
          request, job->response_body, sizeof(job->response_body));

      lock.Lock();
      job->response = response;
      job->retry_after_ms = RetryAfterMilliseconds(response, job->lane);
      job->state = State::kAttemptDone;
      lock.Unlock();
      NotifyOwner();
    }
  }

  void NotifyOwner() {
    if (wake_owner) wake_owner(wake_context);
  }

  bool QueueTask(size_t index) {
    fortis::base::LockGuard lock(mutex);
    if (queue_count >= kProviderExecutionMaxJobs ||
        (jobs[index].state != State::kLaneQueued &&
         jobs[index].state != State::kRetryWait &&
         jobs[index].state != State::kPreparing)) {
      return false;
    }
    const size_t tail = (queue_head + queue_count) % kProviderExecutionMaxJobs;
    task_queue[tail] = index;
    ++queue_count;
    jobs[index].state = State::kWorkerQueued;
    work_ready.Signal();
    return true;
  }

  int FindJob(uint64_t id) const {
    for (size_t i = 0; i < kProviderExecutionMaxJobs; ++i)
      if (jobs[i].state != State::kFree && jobs[i].id == id) return static_cast<int>(i);
    return -1;
  }

  int FindContext(void* context) {
    fortis::base::LockGuard lock(mutex);
    int found = -1;
    for (size_t i = 0; i < kProviderExecutionMaxJobs; ++i)
      if (context == &jobs[i] && jobs[i].state != State::kFree) {
        found = static_cast<int>(i);
        break;
      }
    return found;
  }

  void MakeResult(size_t index, bool cancelled, bool deadline_exceeded) {
    fortis::base::LockGuard lock(mutex);
    Job& job = jobs[index];
    job.cancelled = cancelled;
    job.deadline_exceeded = deadline_exceeded;
    job.retry_at_ms = UINT64_MAX;
    job.state = State::kResultReady;
  }

  void SettleLane(size_t index, ProviderLaneOutcome outcome, uint32_t retry_after_ms = 0) {
    char lane[kProviderLaneNameCapacity];
    uint64_t lane_job_id = 0;
    fortis::base::UniqueLock lock(mutex);
    Job& job = jobs[index];
    if (!job.lane_active) return;
    snprintf(lane, sizeof(lane), "%s", job.lane);
    lane_job_id = job.lane_job_id;
    job.lane_active = false;
    lock.Unlock();
    lanes->Complete(lane, lane_job_id, outcome, retry_after_ms);
  }

  void ProcessAttempt(size_t index) {
    Job* job = &jobs[index];
    http::OutboundResponse response{};
    ProviderExecutionOptions options{};
    uint64_t deadline_ms = 0;
    uint32_t attempts = 0;
    uint32_t retry_after_ms = 0;
    http::OutboundMethod method = http::OutboundMethod::kGet;
    fortis::base::UniqueLock lock(mutex);
    if (job->state != State::kAttemptDone) return;
    job->state = State::kProcessing;
    const bool was_cancelled = job->cancel_requested || job->cancelled;
    const uint64_t now_ms = clock->MonotonicMs();
    response = job->response;
    options = job->options;
    deadline_ms = job->deadline_ms;
    attempts = job->attempts;
    retry_after_ms = job->retry_after_ms;
    method = job->request.method;
    lock.Unlock();

    if (was_cancelled) {
      SettleLane(index, ProviderLaneOutcome::kCancelled);
      MakeResult(index, true, false);
      return;
    }

    // The worker wrote the body before kAttemptDone; the job is ours now.
    const bool unparseable = options.retry_unparseable_json && response.error == http::OutboundError::kNone &&
                             response.status >= 200 && response.status < 300 &&
                             !JsonText(job->response_body, response.body_length);
    const bool eligible = RetryEligible(method, options, response) ||
                          (unparseable && (method == http::OutboundMethod::kGet || options.post_is_idempotent));
    if (!eligible || attempts >= options.max_attempts || now_ms >= deadline_ms) {
      SettleLane(index, unparseable ? ProviderLaneOutcome::kFault
                                    : ProviderLaneOutcomeForHttpStatus(
                                          response.status, response.error != http::OutboundError::kNone),
                 retry_after_ms);
      MakeResult(index, false, now_ms >= deadline_ms);
      return;
    }

    const uint64_t delay = RetryDelay(attempts, retry_after_ms);
    if (delay >= deadline_ms - now_ms) {
      SettleLane(index, ProviderLaneOutcome::kFault, retry_after_ms);
      MakeResult(index, false, true);
      return;
    }
    lock.Lock();
    job->retry_at_ms = now_ms + delay;
    job->state = State::kRetryWait;
  }

  // JSON.parse over Response.json()'s UTF-8 decode (a leading BOM is
  // dropped; a byte >= 0x80 only ever appears inside a string, where an
  // invalid sequence decodes to U+FFFD): RFC 8259 text with JSON's four
  // whitespace bytes. Nesting deeper than 32,768 levels is accepted unchecked.
  static bool JsonText(const unsigned char* text, size_t length) {
    size_t i = length >= 3 && text[0] == 0xEF && text[1] == 0xBB && text[2] == 0xBF ? 3 : 0;
    uint8_t arrays[4096] = {};  // per depth: 1 = array, 0 = object
    size_t depth = 0;
    constexpr size_t kMaxDepth = sizeof(arrays) * 8;
    auto space = [&] { while (i < length && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) ++i; };
    auto digits = [&] { const size_t start = i; while (i < length && text[i] >= '0' && text[i] <= '9') ++i; return i > start; };
    auto string = [&] {
      ++i;  // the opening quote
      while (i < length) {
        const unsigned char c = text[i++];
        if (c == '"') return true;
        if (c < 0x20) return false;
        if (c != '\\') continue;
        if (i >= length) return false;
        const unsigned char e = text[i++];
        if (e == 'u') {
          for (int k = 0; k < 4; ++k, ++i)
            if (i >= length || !((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f') ||
                                 (text[i] >= 'A' && text[i] <= 'F')))
              return false;
        } else if (!strchr("\"\\/bfnrt", e) || e == 0) {
          return false;
        }
      }
      return false;
    };
    auto literal = [&](const char* word) {
      const size_t n = strlen(word);
      if (length - i < n || memcmp(text + i, word, n) != 0) return false;
      i += n;
      return true;
    };
    auto is_array = [&](size_t d) { return (arrays[(d - 1) / 8] >> ((d - 1) % 8) & 1) != 0; };
    // A value, then (inside a container) ',' and the next element or the
    // container's close, until the outermost value ends.
    for (;;) {
      space();
      if (i >= length) return false;
      const unsigned char c = text[i];
      bool opened = false;
      if (c == '{' || c == '[') {
        ++i;
        if (++depth > kMaxDepth) return true;
        const uint8_t bit = static_cast<uint8_t>(1u << ((depth - 1) % 8));
        if (c == '[') arrays[(depth - 1) / 8] |= bit;
        else arrays[(depth - 1) / 8] &= static_cast<uint8_t>(~bit);
        space();
        if (i < length && text[i] == static_cast<unsigned char>(c == '{' ? '}' : ']')) { ++i; --depth; }
        else opened = true;
        if (opened && c == '{') {
          if (i >= length || text[i] != '"' || !string()) return false;
          space();
          if (i >= length || text[i] != ':') return false;
          ++i;
          continue;
        }
        if (opened) continue;
      } else if (c == '"') {
        if (!string()) return false;
      } else if (c == '-' || (c >= '0' && c <= '9')) {
        if (c == '-') ++i;
        if (i < length && text[i] == '0') ++i;
        else if (!digits()) return false;
        if (i < length && text[i] == '.') { ++i; if (!digits()) return false; }
        if (i < length && (text[i] == 'e' || text[i] == 'E')) {
          ++i;
          if (i < length && (text[i] == '+' || text[i] == '-')) ++i;
          if (!digits()) return false;
        }
      } else if (!literal("true") && !literal("false") && !literal("null")) {
        return false;
      }
      // After a value: close containers or move to the next member.
      for (;;) {
        space();
        if (!depth) return i == length;
        if (i >= length) return false;
        const bool array = is_array(depth);
        if (text[i] == static_cast<unsigned char>(array ? ']' : '}')) { ++i; --depth; continue; }
        if (text[i] != ',') return false;
        ++i;
        if (!array) {
          space();
          if (i >= length || text[i] != '"' || !string()) return false;
          space();
          if (i >= length || text[i] != ':') return false;
          ++i;
        }
        break;
      }
    }
  }

  static bool RetryEligible(http::OutboundMethod method,
                            const ProviderExecutionOptions& options,
                            const http::OutboundResponse& response) {
    if (method != http::OutboundMethod::kGet && !options.post_is_idempotent) return false;
    if (response.status == 429) return false;  // TS opens lane; never retry through it.
    if (response.status == 408 || response.status >= 500) return true;
    switch (response.error) {
      case http::OutboundError::kResolve:
      case http::OutboundError::kTimeout:
      case http::OutboundError::kConnect:
      case http::OutboundError::kTls:
      case http::OutboundError::kHttp:
        return response.status == 0;
      default:
        return false;
    }
  }

  static uint64_t RetryDelay(uint32_t completed_attempts, uint32_t retry_after_ms) {
    const size_t delay_index = completed_attempts <= 1 ? 0 : 1;
    uint64_t delay = kRetryDelaysMs[delay_index];
    if (retry_after_ms > delay) delay = retry_after_ms;
    return delay > kMaxRetryDelayMs ? kMaxRetryDelayMs : delay;
  }

  static uint32_t RetryAfterMilliseconds(const http::OutboundResponse& response, const char* lane) {
    if (!response.has_retry_after || !response.retry_after[0]) return 0;
    const uint32_t maximum = response.status == 429 && strcmp(lane, "coingecko") == 0
        ? kMaxRateLimitRetryAfterMs : kMaxRetryAfterMs;
    char* end = nullptr;
    errno = 0;
    const double seconds = strtod(response.retry_after, &end);
    if (errno == 0 && end != response.retry_after && *end == '\0' &&
        isfinite(seconds) && seconds >= 0) {
      const double milliseconds = seconds * 1000.0;
      return milliseconds > maximum ? maximum :
             static_cast<uint32_t>(milliseconds);
    }
    // Match the HTTP-date form accepted by TypeScript Date.parse for the
    // standard IMF-fixdate spelling. Convert to a bounded delay once, then
    // schedule exclusively on the injected monotonic clock.
    struct tm parsed{};
    if (!strptime(response.retry_after, "%a, %d %b %Y %H:%M:%S GMT", &parsed)) return 0;
    const time_t target = timegm(&parsed);
    const time_t wall_now = time(nullptr);
    if (target <= wall_now) return 0;
    const double delay = difftime(target, wall_now) * 1000.0;
    return delay > maximum ? maximum : static_cast<uint32_t>(delay);
  }

  void DrainLaneCancellations(size_t* changes) {
    ProviderLaneCancelledJob cancelled{};
    while (lanes->TryTakeCancelled(&cancelled)) {
      const int found = FindContext(cancelled.context);
      if (found < 0) continue;  // The adapter exclusively owns these events by contract.
      fortis::base::UniqueLock lock(mutex);
      Job& job = jobs[found];
      if (job.state != State::kLaneQueued || job.lane_job_id != cancelled.job_id) continue;
      job.lane_rejected = cancelled.reason != ProviderLaneCancelReason::kCallerCancelled;
      job.lane_cancel_reason = cancelled.reason;
      job.cancelled = cancelled.reason == ProviderLaneCancelReason::kCallerCancelled;
      job.deadline_exceeded = cancelled.reason == ProviderLaneCancelReason::kQueueTimeout;
      job.retry_at_ms = UINT64_MAX;
      job.state = State::kResultReady;
      lock.Unlock();
      ++*changes;
    }
  }

  size_t task_queue[kProviderExecutionMaxJobs]{};
};

ProviderExecutionAdapter::ProviderExecutionAdapter(std::unique_ptr<Impl, fortis::base::ObjectDeleter<Impl>> impl)
    : impl_(std::move(impl)) {}
ProviderExecutionAdapter::ProviderExecutionAdapter(ProviderExecutionAdapter&&) noexcept = default;

base::Result<ProviderExecutionAdapter> ProviderExecutionAdapter::Create(
    ProviderLaneRegistry& lanes, base::Clock& monotonic_clock,
    ProviderExecutionWakeFn wake_owner, void* wake_context) try {
  std::unique_ptr<Impl, fortis::base::ObjectDeleter<Impl>> impl(base::New<Impl>(&lanes, &monotonic_clock, wake_owner, wake_context));
  if (!impl) return base::Status(base::StatusCode::kResourceExhausted, "provider adapter allocation");
  if (!impl->valid) return base::Status(base::StatusCode::kUnavailable, "provider adapter initialization");
  return ProviderExecutionAdapter(std::move(impl));
} catch (const std::bad_alloc&) {
  return base::Status(base::StatusCode::kResourceExhausted, "adapter allocation");
}

ProviderExecutionAdapter::~ProviderExecutionAdapter() {
  if (!impl_) return;
  if (live_job_count() != 0 || !workers_joined()) {
    fortis::base::LogError("providers", "ProviderExecutionAdapter destroyed before results were drained and workers joined");
    abort();
  }
  impl_.reset();
}

ProviderExecutionSubmission ProviderExecutionAdapter::Submit(
    const char* lane_name, const http::OutboundRequest& request, void* context,
    const ProviderExecutionOptions& options) {
  ProviderExecutionSubmission result{};
  if (!lane_name || !*lane_name ||
      strnlen(lane_name, kProviderLaneNameCapacity) >= kProviderLaneNameCapacity ||
      !request.host || !request.port || !request.path || !request.ca_file ||
      strnlen(request.host, kHostCapacity) >= kHostCapacity ||
      strnlen(request.port, kPortCapacity) >= kPortCapacity ||
      strnlen(request.path, kPathCapacity) >= kPathCapacity ||
      strnlen(request.ca_file, kCaPathCapacity) >= kCaPathCapacity ||
      request.body_length > kProviderExecutionMaxRequestBody ||
      (request.body_length && !request.body) ||
      (request.method != http::OutboundMethod::kGet &&
       request.method != http::OutboundMethod::kPostJson &&
       request.method != http::OutboundMethod::kPostBinary) ||
      request.max_response_bytes == 0 ||
      request.max_response_bytes > kProviderExecutionMaxResponseBody ||
      !OutboundHttpsHeadersValid(request) ||
      // Jobs outlive the caller's stack; a borrowed bearer could dangle.
      request.authorization_bearer ||
      !request.timeout_ms || request.timeout_ms > 60000 ||
      (request.method == http::OutboundMethod::kGet && request.body_length != 0) ||
      options.max_attempts == 0 || options.max_attempts > kProviderExecutionMaxAttempts) {
    result.status = ProviderExecutionSubmitStatus::kInvalidRequest;
    return result;
  }
  fortis::base::UniqueLock lock(impl_->mutex);
  const bool stopping = impl_->stopping;
  lock.Unlock();
  if (stopping) {
    result.status = ProviderExecutionSubmitStatus::kStopping;
    return result;
  }
  const uint64_t now_ms = impl_->clock->MonotonicMs();
  if (options.deadline_ms <= now_ms ||
      options.deadline_ms - now_ms > kMaxTotalDurationMs) {
    result.status = ProviderExecutionSubmitStatus::kDeadline;
    return result;
  }

  size_t index = kProviderExecutionMaxJobs;
  lock.Lock();
  if (impl_->stopping) {
    lock.Unlock();
    result.status = ProviderExecutionSubmitStatus::kStopping;
    return result;
  }
  for (size_t i = 0; i < kProviderExecutionMaxJobs; ++i) {
    if (impl_->jobs[i].state == Impl::State::kFree) { index = i; break; }
  }
  if (index == kProviderExecutionMaxJobs) {
    lock.Unlock();
    result.status = ProviderExecutionSubmitStatus::kBusy;
    return result;
  }
  Impl::Job& job = impl_->jobs[index];
  Impl::ResetJob(&job);
  job.state = Impl::State::kPreparing;
  job.id = impl_->next_id++;
  if (!job.id) job.id = impl_->next_id++;
  job.context = context;
  job.deadline_ms = options.deadline_ms;
  job.options = options;
  snprintf(job.lane, sizeof(job.lane), "%s", lane_name);
  snprintf(job.host, sizeof(job.host), "%s", request.host);
  snprintf(job.port, sizeof(job.port), "%s", request.port);
  snprintf(job.path, sizeof(job.path), "%s", request.path);
  snprintf(job.ca_file, sizeof(job.ca_file), "%s", request.ca_file);
  if (request.body_length) memcpy(job.request_body, request.body, request.body_length);
  job.request = request;
  // Make ownership of authentication bytes explicit: these copies live in the
  // fixed Job slot for all network attempts and retries, independent of the
  // caller's request object or stack-backed buffers.
  std::fill(std::begin(job.request.headers), std::end(job.request.headers), http::OutboundHeader{});
  for (size_t i = 0; i < request.header_count; ++i) {
    memcpy(job.request.headers[i].name, request.headers[i].name,
           sizeof(job.request.headers[i].name));
    memcpy(job.request.headers[i].value, request.headers[i].value,
           sizeof(job.request.headers[i].value));
  }
  job.request.host = job.host;
  job.request.port = job.port;
  job.request.path = job.path;
  job.request.ca_file = job.ca_file;
  job.request.body = request.body_length ? job.request_body : nullptr;
  const uint64_t logical_id = job.id;
  lock.Unlock();

  const uint64_t remaining = options.deadline_ms - now_ms;
  const uint32_t caller_wait = remaining > UINT32_MAX ? UINT32_MAX :
                               static_cast<uint32_t>(remaining);
  ProviderLaneAdmission admission = impl_->lanes->TryAdmit(
      job.lane, &job, options.background, true, caller_wait);
  result.lane_status = admission.status;
  result.retry_after_ms = admission.retry_after_ms;
  if (admission.status == ProviderLaneAdmissionStatus::kDispatched) {
    lock.Lock();
    job.lane_job_id = admission.dispatch.job_id;
    job.lane_active = true;
    lock.Unlock();
    if (!impl_->QueueTask(index)) {
      impl_->SettleLane(index, ProviderLaneOutcome::kCancelled);
      lock.Lock();
      Impl::ResetJob(&job);
      lock.Unlock();
      result.status = ProviderExecutionSubmitStatus::kBusy;
      return result;
    }
  } else if (admission.status == ProviderLaneAdmissionStatus::kQueued) {
    lock.Lock();
    job.lane_job_id = admission.job_id;
    job.state = Impl::State::kLaneQueued;
    lock.Unlock();
  } else {
    lock.Lock();
    Impl::ResetJob(&job);
    lock.Unlock();
    result.status = admission.status == ProviderLaneAdmissionStatus::kLaneCapacity ?
                    ProviderExecutionSubmitStatus::kBusy :
                    ProviderExecutionSubmitStatus::kLaneRejected;
    return result;
  }
  result.status = ProviderExecutionSubmitStatus::kAccepted;
  result.job_id = logical_id;
  return result;
}

size_t ProviderExecutionAdapter::Poll() {
  size_t changes = 0;
  for (size_t i = 0; i < kProviderExecutionMaxJobs; ++i)
    impl_->ProcessAttempt(i);

  const uint64_t now_ms = impl_->clock->MonotonicMs();
  for (size_t i = 0; i < kProviderExecutionMaxJobs; ++i) {
    bool due = false;
    bool expired = false;
    fortis::base::UniqueLock lock(impl_->mutex);
    Impl::Job& job = impl_->jobs[i];
    if (job.state == Impl::State::kRetryWait) {
      if (now_ms >= job.deadline_ms) {
        job.deadline_exceeded = true;
        job.state = Impl::State::kResultReady;
        expired = true;
      } else if (now_ms >= job.retry_at_ms) {
        due = true;
        job.state = Impl::State::kPreparing;
      }
    }
    lock.Unlock();
    if (expired) {
      impl_->SettleLane(i, ProviderLaneOutcome::kFault);
      ++changes;
      continue;
    }
    if (!due) continue;
    // A retry is another physical attempt in the same logical lane call. Keep
    // its original admission, token debit and breaker probe reservation.
    if (impl_->QueueTask(i)) {
      ++changes;
    } else {
      lock.Lock();
      impl_->jobs[i].retry_suppressed_by_lane = true;
      lock.Unlock();
      impl_->SettleLane(i, ProviderLaneOutcome::kCancelled);
      impl_->MakeResult(i, false, false);
      ++changes;
    }
  }

  // Cancel and shutdown may make a retry-wait result ready without an attempt
  // completion. Settle its still-active lease here on the registry owner.
  for (size_t i = 0; i < kProviderExecutionMaxJobs; ++i) {
    ProviderLaneOutcome outcome = ProviderLaneOutcome::kAnswered;
    uint32_t retry_after_ms = 0;
    bool settle = false;
    fortis::base::UniqueLock lock(impl_->mutex);
    const Impl::Job& job = impl_->jobs[i];
    if (job.state == Impl::State::kResultReady && job.lane_active) {
      settle = true;
      outcome = job.cancelled ? ProviderLaneOutcome::kCancelled :
          ProviderLaneOutcomeForHttpStatus(job.response.status,
              job.response.error != http::OutboundError::kNone || job.deadline_exceeded);
      retry_after_ms = job.retry_after_ms;
    }
    lock.Unlock();
    if (settle) impl_->SettleLane(i, outcome, retry_after_ms);
  }

  // Pull queued lane work only when one of this adapter's jobs is waiting.
  // The owner event loop is the only code that touches the lane registry.
  for (size_t i = 0; i < kProviderExecutionMaxJobs; ++i) {
    char lane_name[kProviderLaneNameCapacity];
    fortis::base::UniqueLock lock(impl_->mutex);
    Impl::Job& waiting = impl_->jobs[i];
    if (waiting.state != Impl::State::kLaneQueued) continue;
    bool seen = false;
    for (size_t prior = 0; prior < i; ++prior) {
      if (impl_->jobs[prior].state == Impl::State::kLaneQueued &&
          strcmp(impl_->jobs[prior].lane, waiting.lane) == 0) { seen = true; break; }
    }
    snprintf(lane_name, sizeof(lane_name), "%s", waiting.lane);
    lock.Unlock();
    if (seen) continue;
    for (;;) {
      ProviderLaneDispatch dispatch{};
      if (!impl_->lanes->TryTakeNext(lane_name, &dispatch)) break;
      const int job_index = impl_->FindContext(dispatch.context);
      if (job_index < 0) break;
      Impl::Job& job = impl_->jobs[job_index];
      lock.Lock();
      job.lane_job_id = dispatch.job_id;
      job.lane_active = true;
      lock.Unlock();
      if (impl_->QueueTask(static_cast<size_t>(job_index))) ++changes;
      else {
        impl_->SettleLane(static_cast<size_t>(job_index),
                          ProviderLaneOutcome::kCancelled);
        lock.Lock();
        job.retry_suppressed_by_lane = true;
        lock.Unlock();
        impl_->MakeResult(static_cast<size_t>(job_index), false, false);
        ++changes;
        break;
      }
    }
  }

  impl_->DrainLaneCancellations(&changes);
  return changes;
}

bool ProviderExecutionAdapter::Cancel(uint64_t job_id) {
  if (!job_id) return false;
  fortis::base::UniqueLock lock(impl_->mutex);
  const int found = impl_->FindJob(job_id);
  if (found < 0) return false;
  Impl::Job& job = impl_->jobs[found];
  const Impl::State state = job.state;
  if (state == Impl::State::kWorkerQueued || state == Impl::State::kRunning ||
      state == Impl::State::kAttemptDone) {
    job.cancel_requested = true;
    (job.abort_attempt).store(1, std::memory_order_release);
  }
  lock.Unlock();
  switch (state) {
    case Impl::State::kLaneQueued:
      return impl_->lanes->CancelQueued(job.lane, job.lane_job_id);
    case Impl::State::kRetryWait:
      impl_->MakeResult(static_cast<size_t>(found), true, false);
      return true;
    case Impl::State::kWorkerQueued:
    case Impl::State::kRunning:
    case Impl::State::kAttemptDone: return true;
    case Impl::State::kProcessing: return false;  // Settlement has linearized.
    default:
      return false;
  }
}

uint64_t ProviderExecutionAdapter::NextWakeupMs() {
  fortis::base::LockGuard lock(impl_->mutex);
  uint64_t next = UINT64_MAX;
  for (size_t i = 0; i < kProviderExecutionMaxJobs; ++i) {
    const Impl::Job& job = impl_->jobs[i];
    if (job.state == Impl::State::kRetryWait && job.retry_at_ms < next)
      next = job.retry_at_ms;
    if (job.state == Impl::State::kLaneQueued) {
      bool seen = false;
      for (size_t prior = 0; prior < i; ++prior)
        if (impl_->jobs[prior].state == Impl::State::kLaneQueued &&
            strcmp(impl_->jobs[prior].lane, job.lane) == 0) { seen = true; break; }
      if (!seen) {
        const uint64_t lane_wakeup = impl_->lanes->NextWakeupMs(job.lane);
        if (lane_wakeup < next) next = lane_wakeup;
      }
    }
    if (job.state == Impl::State::kAttemptDone) {
      next = impl_->clock->MonotonicMs();
      break;
    }
  }
  return next;
}

ProviderExecutionTakeStatus ProviderExecutionAdapter::TryTakeResult(
    ProviderExecutionResult* output, unsigned char* response_body,
    size_t response_capacity) {
  if (!output) return ProviderExecutionTakeStatus::kNone;
  fortis::base::LockGuard lock(impl_->mutex);
  for (size_t n = 0; n < kProviderExecutionMaxJobs; ++n) {
    const size_t i = (impl_->round_robin + n) % kProviderExecutionMaxJobs;
    Impl::Job& job = impl_->jobs[i];
    if (job.state != Impl::State::kResultReady || job.lane_active) continue;
    if (job.response.body_length > response_capacity ||
        (job.response.body_length && !response_body)) {
      return ProviderExecutionTakeStatus::kBufferTooSmall;
    }
    if (job.response.body_length)
      memcpy(response_body, job.response_body, job.response.body_length);
    *output = {job.id, job.context, job.response, job.attempts,
               job.retry_after_ms, job.response.body_length, job.cancelled,
               job.deadline_exceeded, job.retry_suppressed_by_lane,
               job.lane_rejected, job.lane_cancel_reason};
    Impl::ResetJob(&job);
    impl_->round_robin = (i + 1) % kProviderExecutionMaxJobs;
    return ProviderExecutionTakeStatus::kTaken;
  }
  return ProviderExecutionTakeStatus::kNone;
}

void ProviderExecutionAdapter::BeginShutdown() {
  char queued_lanes[kProviderExecutionMaxJobs][kProviderLaneNameCapacity]{};
  uint64_t queued_ids[kProviderExecutionMaxJobs]{};
  size_t queued_count = 0;
  fortis::base::UniqueLock lock(impl_->mutex);
  if (impl_->stopping) return;
  impl_->stopping = true;
  for (size_t i = 0; i < kProviderExecutionMaxJobs; ++i) {
    Impl::Job& job = impl_->jobs[i];
    if (job.state == Impl::State::kLaneQueued) {
      snprintf(queued_lanes[queued_count], sizeof(queued_lanes[queued_count]),
               "%s", job.lane);
      queued_ids[queued_count++] = job.lane_job_id;
    } else if (job.state == Impl::State::kRetryWait) {
      job.cancelled = true;
      job.retry_at_ms = UINT64_MAX;
      job.state = Impl::State::kResultReady;
    } else if (job.state == Impl::State::kWorkerQueued ||
        job.state == Impl::State::kRunning ||
        job.state == Impl::State::kAttemptDone) {
      job.cancel_requested = true;
      (job.abort_attempt).store(1, std::memory_order_release);
    }
  }
  impl_->work_ready.Broadcast();
  lock.Unlock();
  for (size_t i = 0; i < queued_count; ++i)
    impl_->lanes->CancelQueued(queued_lanes[i], queued_ids[i]);
  size_t ignored = 0;
  impl_->DrainLaneCancellations(&ignored);
  impl_->NotifyOwner();
}

bool ProviderExecutionAdapter::shutdown_started() const {
  fortis::base::LockGuard lock(impl_->mutex);
  const bool stopping = impl_->stopping;
  return stopping;
}

void ProviderExecutionAdapter::JoinWorkers() {
  fortis::base::UniqueLock lock(impl_->mutex);
  if (!impl_->stopping || impl_->joined || impl_->joining) return;
  impl_->joining = true;
  lock.Unlock();
  for (size_t i = 0; i < impl_->worker_count; ++i) impl_->workers[i].Join();
  lock.Lock();
  impl_->joined = true;
  impl_->joining = false;
}

bool ProviderExecutionAdapter::workers_joined() const {
  fortis::base::LockGuard lock(impl_->mutex);
  const bool joined = impl_->joined;
  return joined;
}

size_t ProviderExecutionAdapter::live_job_count() const {
  fortis::base::LockGuard lock(impl_->mutex);
  size_t count = 0;
  for (size_t i = 0; i < kProviderExecutionMaxJobs; ++i)
    count += impl_->jobs[i].state != Impl::State::kFree;
  return count;
}

const char* ProviderExecutionSubmitStatusName(ProviderExecutionSubmitStatus status) {
  switch (status) {
    case ProviderExecutionSubmitStatus::kAccepted: return "accepted";
    case ProviderExecutionSubmitStatus::kBusy: return "busy";
    case ProviderExecutionSubmitStatus::kInvalidRequest: return "invalid_request";
    case ProviderExecutionSubmitStatus::kDeadline: return "deadline";
    case ProviderExecutionSubmitStatus::kLaneRejected: return "lane_rejected";
    case ProviderExecutionSubmitStatus::kStopping: return "stopping";
  }
  return "unknown";
}

}  // namespace fortis::providers
