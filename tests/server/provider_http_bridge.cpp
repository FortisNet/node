#include "provider_http_bridge.h"

#include "http_server.h"
#include "outbound_https.h"
#include "provider_execution.h"
#include "provider_lane.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef FORTIS_UNIKRAFT
void* operator new(size_t, void* storage) noexcept;
#else
#include <new>
#endif

namespace {
constexpr size_t kIntentCapacity = 8;
constexpr uint64_t kProviderDeadlineMs = 29000;
constexpr uint32_t kAttemptTimeoutMs = 1500;
const char* fixed_lane(ProviderHttpTestMode mode) {
  switch (mode) {
    case ProviderHttpTestMode::kRateLimited: return "rpc:fortis-test-429";
    case ProviderHttpTestMode::kTlsFailure: return "rpc:fortis-test-tls";
    case ProviderHttpTestMode::kRetry503:
    case ProviderHttpTestMode::kAlways503: return "rpc:fortis-test-503";
    default: return "rpc:fortis-test-upstream";
  }
}

uint64_t monotonic_ms() {
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return static_cast<uint64_t>(now.tv_sec) * 1000ULL +
         static_cast<uint64_t>(now.tv_nsec / 1000000L);
}

bool allow_test_loopback(void*, uint32_t address) {
  return address == 0x7f000001U;
}

bool valid_port(const char* port) {
  if (!port) return false;
  const size_t length = strnlen(port, 6);
  if (length == 0 || length >= 6) return false;
  unsigned value = 0;
  for (size_t i = 0; i < length; ++i) {
    if (port[i] < '0' || port[i] > '9') return false;
    value = value * 10U + static_cast<unsigned>(port[i] - '0');
  }
  return value > 0 && value <= 65535;
}

const char* fixed_path(ProviderHttpTestMode mode) {
  switch (mode) {
    case ProviderHttpTestMode::kOk: return "/ok";
    case ProviderHttpTestMode::kSlow: return "/slow";
    case ProviderHttpTestMode::kRetry503: return "/retry500";
    case ProviderHttpTestMode::kAlways503: return "/always503";
    case ProviderHttpTestMode::kRateLimited: return "/429";
    case ProviderHttpTestMode::kTlsFailure: return "/ok";
    case ProviderHttpTestMode::kStall: return "/stall";
  }
  return nullptr;
}

unsigned retry_after_seconds(const char* value) {
  if (!value || !*value) return 0;
  char* end = nullptr;
  const double seconds = strtod(value, &end);
  if (end == value || *end || !isfinite(seconds) || seconds <= 0) return 0;
  const double rounded = ceil(seconds);
  return rounded > 3600 ? 3600 : static_cast<unsigned>(rounded);
}

const char* reason_for_status(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return status >= 500 ? "Upstream Error" : "Upstream Response";
  }
}

timespec realtime_after(uint64_t milliseconds) {
  timespec wake{};
  clock_gettime(CLOCK_REALTIME, &wake);
  wake.tv_sec += static_cast<time_t>(milliseconds / 1000ULL);
  wake.tv_nsec += static_cast<long>((milliseconds % 1000ULL) * 1000000ULL);
  if (wake.tv_nsec >= 1000000000L) {
    ++wake.tv_sec;
    wake.tv_nsec -= 1000000000L;
  }
  return wake;
}

void complete_error(fortis::http::HttpResponseHandoff* handoff, uint64_t token,
                    int status, const char* reason, const char* error,
                    unsigned retry_after = 0) {
  if (!handoff || !token) return;
  fortis::http::HttpResponse response = fortis::http::JsonResponse(status, reason, error);
  response.retry_after = retry_after;
  handoff->Complete(token, response);
}
}  // namespace

struct ProviderHttpBridge::Impl {
  enum class State : uint8_t { kFree, kQueued, kActive };
  struct Job {
    State state = State::kFree;
    ProviderHttpTestMode mode = ProviderHttpTestMode::kOk;
    uint64_t token = 0;
    uint64_t http_deadline_ms = 0;
    uint64_t provider_id = 0;
    bool expired = false;
  };

  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
  pthread_t owner_thread{};
  bool owner_started = false;
  bool stop_requested = false;
  bool shutdown_started = false;
  bool workers_joined = false;
  bool owner_exited = false;
  bool mutex_ready = true;
  bool cond_ready = true;
  fortis::providers::ProviderLaneRegistry lanes;
  std::optional<fortis::providers::ProviderExecutionAdapter> adapter;
  fortis::http::HttpResponseHandoff* handoff = nullptr;
  char upstream_port[6]{};
  char tls_failure_port[6]{};
  char ca_file[1024]{};
  Job jobs[kIntentCapacity]{};
  size_t queue[kIntentCapacity]{};
  size_t queue_head = 0;
  size_t queue_count = 0;
  unsigned char response_body[fortis::providers::kProviderExecutionDefaultResponseBody]{};

  Impl(const char* port, const char* tls_port, const char* ca,
       fortis::http::HttpResponseHandoff* response_handoff)
      : lanes(&fortis::base::SystemClock()),
        adapter([&]() -> std::optional<fortis::providers::ProviderExecutionAdapter> {
          auto made = fortis::providers::ProviderExecutionAdapter::Create(lanes, fortis::base::SystemClock(), WakeOwner, this);
          if (!made) return std::nullopt;
          return made.TakeValue();
        }()),
        handoff(response_handoff) {
    snprintf(upstream_port, sizeof(upstream_port), "%s", port);
    snprintf(tls_failure_port, sizeof(tls_failure_port), "%s", tls_port);
    snprintf(ca_file, sizeof(ca_file), "%s", ca);
  }

  ~Impl() {
    if (cond_ready) pthread_cond_destroy(&changed);
    if (mutex_ready) pthread_mutex_destroy(&mutex);
  }

  static void WakeOwner(void* opaque) {
    auto* self = static_cast<Impl*>(opaque);
    pthread_mutex_lock(&self->mutex);
    pthread_cond_signal(&self->changed);
    pthread_mutex_unlock(&self->mutex);
  }

  static void* OwnerEntry(void* opaque) {
    static_cast<Impl*>(opaque)->OwnerLoop();
    return nullptr;
  }

  bool Start() {
    if (!lanes.valid() || !adapter.has_value()) return false;
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) {
      adapter->BeginShutdown();
      adapter->JoinWorkers();
      workers_joined = true;
      return false;
    }
    if (pthread_attr_setstacksize(&attributes, 256 * 1024) != 0) {
      pthread_attr_destroy(&attributes);
      adapter->BeginShutdown();
      adapter->JoinWorkers();
      workers_joined = true;
      return false;
    }
    const int result = pthread_create(&owner_thread, &attributes, OwnerEntry, this);
    pthread_attr_destroy(&attributes);
    if (result != 0) {
      adapter->BeginShutdown();
      adapter->JoinWorkers();
      workers_joined = true;
      return false;
    }
    owner_started = true;
    return true;
  }

  bool Enqueue(uint64_t token, ProviderHttpTestMode mode,
               uint64_t http_deadline_ms) {
    if (!token || !fixed_path(mode)) return false;
    pthread_mutex_lock(&mutex);
    if (stop_requested || queue_count == kIntentCapacity) {
      pthread_mutex_unlock(&mutex);
      return false;
    }
    size_t index = kIntentCapacity;
    for (size_t i = 0; i < kIntentCapacity; ++i)
      if (jobs[i].state == State::kFree) { index = i; break; }
    if (index == kIntentCapacity) {
      pthread_mutex_unlock(&mutex);
      return false;
    }
    Job& job = jobs[index];
    job = Job{};
    job.state = State::kQueued;
    job.mode = mode;
    job.token = token;
    job.http_deadline_ms = http_deadline_ms;
    const size_t tail = (queue_head + queue_count) % kIntentCapacity;
    queue[tail] = index;
    ++queue_count;
    pthread_cond_signal(&changed);
    pthread_mutex_unlock(&mutex);
    return true;
  }

  void SubmitQueued(size_t index) {
    uint64_t token = 0;
    uint64_t http_deadline = 0;
    ProviderHttpTestMode mode = ProviderHttpTestMode::kOk;
    pthread_mutex_lock(&mutex);
    Job& job = jobs[index];
    if (job.state != State::kQueued) {
      pthread_mutex_unlock(&mutex);
      return;
    }
    token = job.token;
    http_deadline = job.http_deadline_ms;
    mode = job.mode;
    pthread_mutex_unlock(&mutex);

    const uint64_t now = monotonic_ms();
    if (now >= http_deadline) {
      complete_error(handoff, token, 504, "Gateway Timeout", "{\"error\":\"TIMEOUT\"}");
      Release(index);
      return;
    }
    fortis::http::OutboundRequest request{};
    request.method = fortis::http::OutboundMethod::kGet;
    request.host = "localhost";
    request.port = mode == ProviderHttpTestMode::kTlsFailure
        ? tls_failure_port : upstream_port;
    request.path = fixed_path(mode);
    request.ca_file = ca_file;
    request.timeout_ms = kAttemptTimeoutMs;
    request.max_response_bytes = fortis::providers::kProviderExecutionDefaultResponseBody;
    request.allow_address = allow_test_loopback;
    fortis::providers::ProviderExecutionOptions options{};
    options.deadline_ms = now + kProviderDeadlineMs;
    options.max_attempts = 3;
    const fortis::providers::ProviderExecutionSubmission submission = adapter->Submit(
        fixed_lane(mode), request, &jobs[index], options);
    if (submission.status != fortis::providers::ProviderExecutionSubmitStatus::kAccepted) {
      const bool breaker = submission.lane_status == fortis::providers::ProviderLaneAdmissionStatus::kBreakerOpen;
      const unsigned retry_after = static_cast<unsigned>(
          (submission.retry_after_ms + 999ULL) / 1000ULL);
      complete_error(handoff, token, 503, "Service Unavailable",
                     "{\"error\":\"UPSTREAM_UNAVAILABLE\"}",
                     breaker ? (retry_after ? retry_after : 1) : 0);
      Release(index);
      return;
    }
    pthread_mutex_lock(&mutex);
    if (jobs[index].state == State::kQueued) {
      jobs[index].state = State::kActive;
      jobs[index].provider_id = submission.job_id;
    }
    pthread_mutex_unlock(&mutex);
  }

  void Release(size_t index) {
    pthread_mutex_lock(&mutex);
    jobs[index] = Job{};
    pthread_mutex_unlock(&mutex);
  }

  void DrainIntents(bool stopping) {
    for (;;) {
      size_t index = kIntentCapacity;
      uint64_t token = 0;
      pthread_mutex_lock(&mutex);
      if (!queue_count) {
        pthread_mutex_unlock(&mutex);
        return;
      }
      index = queue[queue_head];
      queue_head = (queue_head + 1) % kIntentCapacity;
      --queue_count;
      if (jobs[index].state == State::kQueued) token = jobs[index].token;
      pthread_mutex_unlock(&mutex);
      if (stopping) {
        complete_error(handoff, token, 503, "Service Unavailable",
                       "{\"error\":\"SHUTTING_DOWN\"}");
        Release(index);
      } else {
        SubmitQueued(index);
      }
    }
  }

  void CompleteResult(const fortis::providers::ProviderExecutionResult& result, size_t body_length) {
    auto* job = static_cast<Job*>(result.context);
    if (!job || job < jobs || job >= jobs + kIntentCapacity) return;
    const size_t index = static_cast<size_t>(job - jobs);
    uint64_t token = 0;
    bool expired = false;
    pthread_mutex_lock(&mutex);
    if (job->state != State::kActive || job->provider_id != result.job_id) {
      pthread_mutex_unlock(&mutex);
      return;
    }
    token = job->token;
    expired = job->expired || monotonic_ms() >= job->http_deadline_ms;
    const bool cancelled = result.cancelled;
    pthread_mutex_unlock(&mutex);

    if (!expired) {
      fortis::http::HttpResponse response{};
      char content_type[256] = "application/octet-stream";
      const uint8_t* body = response_body;
      if (cancelled) {
        response = fortis::http::JsonResponse(503, "Service Unavailable",
                                "{\"error\":\"UPSTREAM_CANCELLED\"}");
      } else if (result.deadline_exceeded || result.response.error == fortis::http::OutboundError::kTimeout) {
        response = fortis::http::JsonResponse(504, "Gateway Timeout", "{\"error\":\"UPSTREAM_TIMEOUT\"}");
      } else if (result.response.error != fortis::http::OutboundError::kNone ||
                 result.response.status == 0) {
        response = fortis::http::JsonResponse(502, "Bad Gateway", "{\"error\":\"UPSTREAM_FAILURE\"}");
      } else {
        if (result.response.has_content_type && result.response.content_type[0])
          snprintf(content_type, sizeof(content_type), "%s", result.response.content_type);
        response = {result.response.status, reason_for_status(result.response.status),
                    content_type, body, body_length};
        response.retry_after = retry_after_seconds(result.response.retry_after);
      }
      handoff->Complete(token, response);
    }
    Release(index);
  }

  void DrainResults(bool stopping) {
    for (;;) {
      fortis::providers::ProviderExecutionResult result{};
      const auto taken = adapter->TryTakeResult(
          &result, response_body, sizeof(response_body));
      if (taken == fortis::providers::ProviderExecutionTakeStatus::kNone) return;
      if (taken != fortis::providers::ProviderExecutionTakeStatus::kTaken) continue;
      if (stopping) {
        auto* job = static_cast<Job*>(result.context);
        if (job && job >= jobs && job < jobs + kIntentCapacity) {
          const uint64_t token = job->token;
          complete_error(handoff, token, 503, "Service Unavailable",
                         "{\"error\":\"SHUTTING_DOWN\"}");
          Release(static_cast<size_t>(job - jobs));
        }
      } else {
        CompleteResult(result, result.body_length);
      }
    }
  }

  void ExpireHttpJobs(uint64_t now) {
    uint64_t cancel_ids[kIntentCapacity]{};
    size_t cancel_count = 0;
    pthread_mutex_lock(&mutex);
    for (size_t i = 0; i < kIntentCapacity; ++i) {
      Job& job = jobs[i];
      if (job.state == State::kActive && !job.expired &&
          now >= job.http_deadline_ms) {
        job.expired = true;
        cancel_ids[cancel_count++] = job.provider_id;
      }
    }
    pthread_mutex_unlock(&mutex);
    for (size_t i = 0; i < cancel_count; ++i) adapter->Cancel(cancel_ids[i]);
  }

  void BeginShutdown() {
    pthread_mutex_lock(&mutex);
    const bool should_start = stop_requested && !shutdown_started;
    pthread_mutex_unlock(&mutex);
    if (should_start) {
      adapter->BeginShutdown();
      pthread_mutex_lock(&mutex);
      shutdown_started = true;
      pthread_cond_broadcast(&changed);
      pthread_mutex_unlock(&mutex);
    }
  }

  void WaitForWork(bool stopping) {
    const uint64_t now = monotonic_ms();
    uint64_t next = adapter->NextWakeupMs();
    pthread_mutex_lock(&mutex);
    if (stopping && workers_joined && adapter->live_job_count() == 0) {
      pthread_mutex_unlock(&mutex);
      return;
    }
    for (size_t i = 0; i < kIntentCapacity; ++i)
      if (jobs[i].state != State::kFree && !jobs[i].expired &&
          jobs[i].http_deadline_ms < next)
        next = jobs[i].http_deadline_ms;
    if (queue_count || (!stopping && stop_requested) ||
        (next != UINT64_MAX && next <= now)) {
      pthread_mutex_unlock(&mutex);
      return;
    }
    if (next == UINT64_MAX) {
      pthread_cond_wait(&changed, &mutex);
    } else {
      const uint64_t remaining = next - now;
      const timespec wake = realtime_after(remaining > 100 ? 100 : remaining);
      pthread_cond_timedwait(&changed, &mutex, &wake);
    }
    pthread_mutex_unlock(&mutex);
  }

  void OwnerLoop() {
    for (;;) {
      pthread_mutex_lock(&mutex);
      const bool stopping = stop_requested;
      pthread_mutex_unlock(&mutex);
      if (stopping) {
        DrainIntents(true);
        BeginShutdown();
      } else {
        DrainIntents(false);
      }
      adapter->Poll();
      DrainResults(stopping);
      ExpireHttpJobs(monotonic_ms());

      pthread_mutex_lock(&mutex);
      const bool done = stop_requested && shutdown_started && workers_joined &&
                        adapter->live_job_count() == 0;
      pthread_mutex_unlock(&mutex);
      if (done) break;
      WaitForWork(stopping);
    }
    lanes.Close();
    pthread_mutex_lock(&mutex);
    owner_exited = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
  }
};

ProviderHttpBridge::ProviderHttpBridge() : impl_(nullptr) {}

ProviderHttpBridge::~ProviderHttpBridge() {
  Stop();
  if (impl_) {
    impl_->~Impl();
    free(impl_);
  }
}

bool ProviderHttpBridge::Start(const char* upstream_port,
                               const char* tls_failure_port,
                               const char* ca_file,
                               fortis::http::HttpResponseHandoff* handoff) {
  if (impl_ || !valid_port(upstream_port) || !valid_port(tls_failure_port) ||
      !ca_file || !*ca_file || strnlen(ca_file, 1024) >= 1024 || !handoff)
    return false;
  void* storage = malloc(sizeof(Impl));
  if (!storage) return false;
  impl_ = new (storage) Impl(upstream_port, tls_failure_port, ca_file, handoff);
  if (!impl_->Start()) return false;
  return true;
}

bool ProviderHttpBridge::Enqueue(uint64_t http_token, ProviderHttpTestMode mode,
                                 uint64_t http_deadline_ms) {
  return impl_ && impl_->Enqueue(http_token, mode, http_deadline_ms);
}

void ProviderHttpBridge::Stop() {
  if (!impl_ || !impl_->owner_started) return;
  pthread_mutex_lock(&impl_->mutex);
  impl_->stop_requested = true;
  pthread_cond_broadcast(&impl_->changed);
  while (!impl_->shutdown_started && !impl_->owner_exited)
    pthread_cond_wait(&impl_->changed, &impl_->mutex);
  pthread_mutex_unlock(&impl_->mutex);

  impl_->adapter->JoinWorkers();
  pthread_mutex_lock(&impl_->mutex);
  impl_->workers_joined = true;
  pthread_cond_broadcast(&impl_->changed);
  pthread_mutex_unlock(&impl_->mutex);
  pthread_join(impl_->owner_thread, nullptr);
  impl_->owner_started = false;
}

bool ProviderHttpBridge::valid() const {
  return impl_ && impl_->adapter.has_value() && impl_->owner_started;
}
