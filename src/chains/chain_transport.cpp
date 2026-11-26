#include "fixed_string.h"
#include <new>
#include "exception_boundary.h"
#include "chain_transport.h"
#include "chain_transport_policy.h"
#include "ownership.h"

#include "outbound_https.h"
#include "provider_execution_coordinator.h"
#include "threading.h"
#include "upstream_redirect.h"
#include "token_provider.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

namespace tj = fortis::codec::tj;

namespace fortis::chains {
namespace {

constexpr size_t kClients = 64;
constexpr uint64_t kWakeMs = 40;
constexpr uint32_t kTimeoutMs = 10000;
constexpr uint64_t kMaxDeadlineMs = 60000;
constexpr size_t kHostBytes = 254;
constexpr size_t kPortBytes = 6;
constexpr size_t kLaneBytes = 128;
constexpr size_t kPathBytes = 8192;
timespec wake_at(uint64_t ms) {
  timespec out{};
  clock_gettime(CLOCK_REALTIME, &out);
  out.tv_sec += static_cast<time_t>(ms / 1000);
  out.tv_nsec += static_cast<long>((ms % 1000) * 1000000);
  if (out.tv_nsec >= 1000000000L) { ++out.tv_sec; out.tv_nsec -= 1000000000L; }
  return out;
}

struct Batch {
  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable done;
  size_t outstanding = 0;
  uint64_t arrival_sequence = 0;
};

// One Call's progress through its endpoints.
struct Attempt {
  Call* call = nullptr;
  std::unique_ptr<uint8_t[], base::StorageDeleter> body;  // request body (JSON-RPC envelope or POST JSON)
  size_t body_length = 0;
  EndpointFailover endpoints;
  size_t parse_retries = 0;
  bool done = false;
  // A parse retry waits until this monotonic time (0: no wait).
  uint64_t not_before = 0;
  // The last answer of the current endpoint; `arrived` until interpreted.
  bool pending = false;
  bool arrived = false;
  uint64_t arrival_order = 0;
  bool answered = false;
  bool transport = false;
  bool cancelled = false;
  bool lane_rejected = false;
  bool lane_busy_seen = false;
  int status = 0;
  uint32_t retry_after_ms = 0;
  std::unique_ptr<uint8_t[], base::StorageDeleter> response;
  size_t response_length = 0;
  uint64_t request_id = 0;
};

}  // namespace

uint64_t ChainMonotonicMs() { return providers::TokenMonotonicMs(); }

void Call::ToError(Error* error) const {
  switch (failure) {
    case CallFailure::kNone: error->Clear(); return;
    case CallFailure::kAnswer: error->Upstream("%s", message); return;
    case CallFailure::kTransport: error->Upstream("%s", message[0] ? message : "transport failure"); return;
    case CallFailure::kLaneBusy: error->LaneBusy(retry_after_ms); return;
    case CallFailure::kCancelled: error->Upstream("cancelled"); return;
    case CallFailure::kNoEndpoints: error->Range("no endpoints configured"); return;
  }
}

struct Transport::Impl {
  struct HeaderCopy {
    char name[http::kOutboundHeaderNameCapacity]{};
    char value[http::kOutboundHeaderValueCapacity]{};
  };
  struct Client {
    bool used = false, queued = false, active = false, cancelled = false, background = false;
    bool post = false, post_retry = false, raw = false;
    uint32_t timeout_ms = 0;
    size_t max_response = 0;
    char content_type[http::kOutboundContentTypeCapacity]{};
    uint64_t id = 0, deadline = 0, job = 0;
    char host[kHostBytes]{}, port[kPortBytes]{}, lane[kLaneBytes]{}, real_host[kHostBytes + kPortBytes]{};
    char path[kPathBytes]{};
    HeaderCopy headers[kCallMaxHeaders]{};
    size_t header_count = 0;
    const uint8_t* body = nullptr;
    size_t body_length = 0;
    Batch* batch = nullptr;
    Attempt* attempt = nullptr;
  };

  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable changed;
  fortis::base::Thread thread;
  bool started = false, stopping = false;
  uint64_t next_id = 1, owner_id = 0;
  TransportConfig config{};
  providers::ProviderExecutionCoordinator* coordinator = nullptr;
  Client clients[kClients]{};
  unsigned char response[providers::kProviderExecutionMaxResponseBody]{};
  bool skip_sleep = false;
  // Off in production (see upstream_redirect.h).
  http::UpstreamRedirect redirect;

  Impl(const TransportConfig& cfg, providers::ProviderExecutionCoordinator* shared) : config(cfg), coordinator(shared) {}
  ~Impl() = default;

  static void Wake(void* opaque) {
    auto* self = static_cast<Impl*>(opaque);
    fortis::base::LockGuard lock(self->mutex);
    self->changed.Signal();
  }
  static void* Entry(void* opaque) { static_cast<Impl*>(opaque)->Loop(); return nullptr; }

  bool Start() {
    if (started || !coordinator || !coordinator->valid() || !config.ca_bundle_configured ||
        !config.ca_bundle_path[0]) return false;
    owner_id = coordinator->RegisterOwner(Wake, this);
    if (!owner_id) return false;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) { coordinator->UnregisterOwner(owner_id); owner_id = 0; return false; }
    if (pthread_attr_setstacksize(&attr, 256 * 1024) != 0) {
      pthread_attr_destroy(&attr); coordinator->UnregisterOwner(owner_id); owner_id = 0; return false;
    }
    const int rc = (thread.StartEntry<Entry>(this, &attr) ? 0 : -1);
    pthread_attr_destroy(&attr);
    if (rc != 0) { coordinator->UnregisterOwner(owner_id); owner_id = 0; return false; }
    started = true;
    return true;
  }

  // Fills the client's target for the attempt's current endpoint.
  bool Target(Client& c, const Attempt& attempt) {
    const Call& call = *attempt.call;
    const char* base = call.urls[attempt.endpoints.index()];
    if (!base) return false;
    char url[kPathBytes + 512];
    if (call.kind == CallKind::kJsonRpc) {
      if (snprintf(url, sizeof(url), "%s", base) >= static_cast<int>(sizeof(url))) return false;
    } else {
      // joinUrl: `${base.replace(/\/$/, '')}${path}`.
      size_t base_length = strlen(base);
      if (base_length && base[base_length - 1] == '/') --base_length;
      const int written = snprintf(url, sizeof(url), "%.*s%s", static_cast<int>(base_length), base,
                                   call.path ? call.path : "");
      if (written < 0 || static_cast<size_t>(written) >= sizeof(url)) return false;
    }
    char lane_host[kHostBytes + kPortBytes + 2];
    if (!providers::TokenSplitHttpsUrl(url, c.host, sizeof(c.host), c.port, sizeof(c.port), c.path, sizeof(c.path),
                            lane_host, sizeof(lane_host)))
      return false;
    base::CopyCString(c.real_host, lane_host);
    const int lane = call.lane ? snprintf(c.lane, sizeof(c.lane), "%s", call.lane)
                               : snprintf(c.lane, sizeof(c.lane), "rpc:%s", lane_host);
    if (lane < 0 || static_cast<size_t>(lane) >= sizeof(c.lane)) return false;
    c.header_count = 0;
    for (size_t i = 0; i < call.header_count && i < kCallMaxHeaders; ++i) {
      const CallHeader& h = call.headers[i];
      if (!h.name || !h.value || strlen(h.name) >= http::kOutboundHeaderNameCapacity ||
          strlen(h.value) >= http::kOutboundHeaderValueCapacity) return false;
      snprintf(c.headers[c.header_count].name, http::kOutboundHeaderNameCapacity, "%s", h.name);
      snprintf(c.headers[c.header_count].value, http::kOutboundHeaderValueCapacity, "%s", h.value);
      ++c.header_count;
    }
    c.post = call.kind != CallKind::kGet;
    c.post_retry = call.kind == CallKind::kPost && call.retry_post;
    c.raw = call.kind == CallKind::kRawPost;
    c.content_type[0] = '\0';
    if (c.raw) {
      if (!call.content_type || strlen(call.content_type) >= sizeof(c.content_type)) return false;
      snprintf(c.content_type, sizeof(c.content_type), "%s", call.content_type);
    }
    c.max_response = call.max_response_bytes;
    c.body = attempt.body.get();
    c.body_length = attempt.body_length;
    c.background = call.background;
    c.timeout_ms = call.timeout_ms;
    return true;
  }

  bool Submit(Batch* batch, Attempt* attempt, uint64_t deadline) {
    const uint64_t now = providers::TokenMonotonicMs();
    if (deadline <= now) return false;
    fortis::base::LockGuard lock(mutex);
    if (!started || stopping) return false;
    size_t ci = kClients;
    for (size_t i = 0; i < kClients; ++i) if (!clients[i].used) { ci = i; break; }
    if (ci == kClients) return false;
    Client& c = clients[ci];
    if (!Target(c, *attempt)) return false;
    uint64_t id = next_id++;
    if (!id) id = next_id++;
    c.used = true; c.queued = true; c.active = false; c.cancelled = false;
    c.id = id; c.deadline = deadline; c.job = 0;
    c.batch = batch; c.attempt = attempt;
    attempt->request_id = id;
    changed.Signal();
    return true;
  }

  bool Build(const Client& c, http::OutboundRequest* out) {
    http::OutboundRequest request{};
    request.method = c.raw ? http::OutboundMethod::kPostBinary : c.post ? http::OutboundMethod::kPostJson : http::OutboundMethod::kGet;
    if (c.raw) memcpy(request.content_type, c.content_type, sizeof(request.content_type));
    request.host = c.host;
    request.port = c.port;
    request.path = c.path;
    request.ca_file = config.ca_bundle_path;
    request.timeout_ms = c.timeout_ms ? c.timeout_ms : kTimeoutMs;
    request.max_response_bytes = c.max_response && c.max_response < providers::kProviderExecutionMaxResponseBody
                                     ? c.max_response
                                     : providers::kProviderExecutionMaxResponseBody;
    if (c.post) {
      request.body = c.body;
      request.body_length = c.body_length;
    }
    // HttpClient sends `accept: application/json` unless the call names its
    // own accept; a `user-agent` header replaces the transport's User-Agent.
    size_t headers = 0;
    bool custom_accept = false;
    for (size_t i = 0; i < c.header_count; ++i)
      if (strcasecmp(c.headers[i].name, "accept") == 0) custom_accept = true;
    if (!custom_accept) {
      snprintf(request.headers[headers].name, sizeof(request.headers[0].name), "accept");
      snprintf(request.headers[headers].value, sizeof(request.headers[0].value), "application/json");
      ++headers;
    }
    for (size_t i = 0; i < c.header_count; ++i) {
      if (strcasecmp(c.headers[i].name, "user-agent") == 0) {
        request.user_agent = c.headers[i].value;
        continue;
      }
      memcpy(request.headers[headers].name, c.headers[i].name, sizeof(request.headers[0].name));
      memcpy(request.headers[headers].value, c.headers[i].value, sizeof(request.headers[0].value));
      ++headers;
    }
    if (redirect.enabled && redirect.real_host_header[0]) {
      base::CopyCString(request.headers[headers].name, redirect.real_host_header);
      base::CopyCString(request.headers[headers].value, c.real_host);
      ++headers;
    }
    redirect.Apply(request);
    request.header_count = headers;
    if (!OutboundHttpsHeadersValid(request)) return false;
    *out = request;
    return true;
  }

  // Runs on the owner thread without the owner mutex.
  static void Complete(Batch* batch, Attempt* attempt, bool answered, int status, bool transport,
                       bool cancelled, bool lane_rejected, uint32_t retry_after_ms, const unsigned char* body,
                       size_t length) {
    attempt->answered = answered;
    attempt->status = status;
    attempt->transport = transport;
    attempt->cancelled = cancelled;
    attempt->lane_rejected = lane_rejected;
    attempt->retry_after_ms = retry_after_ms;
    attempt->response.reset();
    attempt->response_length = 0;
    if (answered && body && length) {
      attempt->response = base::AllocateStorage<uint8_t[]>(length);
      if (attempt->response) {
        memcpy(attempt->response.get(), body, length);
        attempt->response_length = length;
      } else {
        attempt->answered = false;
        attempt->transport = true;
      }
    }
    fortis::base::LockGuard lock(batch->mutex);
    attempt->pending = false;
    attempt->arrival_order = ++batch->arrival_sequence;
    attempt->arrived = true;
    --batch->outstanding;
    batch->done.Broadcast();
  }

  void Deliver(size_t ci, bool answered, int status, bool transport, bool cancelled, bool lane_rejected,
               uint32_t retry_after_ms, const unsigned char* body = nullptr, size_t length = 0) {
    fortis::base::UniqueLock lock(mutex);
    if (ci >= kClients || !clients[ci].used) return;
    Batch* batch = clients[ci].batch;
    Attempt* attempt = clients[ci].attempt;
    clients[ci].used = false;
    clients[ci].active = false;
    clients[ci].queued = false;
    lock.Unlock();
    Complete(batch, attempt, answered, status, transport, cancelled, lane_rejected, retry_after_ms, body, length);
  }

  void Expire(bool stop) {
    const uint64_t now = providers::TokenMonotonicMs();
    for (size_t ci = 0; ci < kClients; ++ci) {
      fortis::base::UniqueLock lock(mutex);
      Client& c = clients[ci];
      const bool expired = c.used && (stop || c.cancelled || now >= c.deadline);
      const bool queued = expired && c.queued;
      const bool active = expired && c.active;
      const uint64_t job = c.job;
      if (active) c.cancelled = true;
      lock.Unlock();
      if (queued) Deliver(ci, false, 0, false, true, false, 0);
      else if (active) coordinator->Cancel(owner_id, job);
    }
  }

  void Schedule() {
    for (size_t ci = 0; ci < kClients; ++ci) {
      fortis::base::UniqueLock lock(mutex);
      Client& c = clients[ci];
      const bool ready = c.used && c.queued;
      lock.Unlock();
      if (!ready) continue;
      if (coordinator->live_job_count(owner_id) >= providers::kProviderExecutionMaxJobs) return;
      http::OutboundRequest request{};
      if (!Build(c, &request)) { Deliver(ci, false, 0, true, false, false, 0); continue; }
      providers::ProviderExecutionOptions options{};
      // DriverTransport: GETs keep HttpClient's three attempts; POSTs are
      // single-shot unless the read opted into retryPost.
      options.max_attempts = ChainRetryPolicy::MaxAttempts(c.post, c.post_retry);
      options.post_is_idempotent = c.post_retry;
      options.deadline_ms = c.deadline;
      options.background = c.background;
      const auto submitted = coordinator->Submit(owner_id, c.lane, request, &clients[ci], options);
      if (submitted.status == providers::ProviderExecutionSubmitStatus::kBusy) return;
      if (submitted.status != providers::ProviderExecutionSubmitStatus::kAccepted) {
        Deliver(ci, false, 0, true, false, submitted.status == providers::ProviderExecutionSubmitStatus::kLaneRejected,
                static_cast<uint32_t>(submitted.retry_after_ms));
        continue;
      }
      lock.Lock();
      c.queued = false;
      c.active = true;
      c.job = submitted.job_id;
    }
  }

  void Drain() {
    for (;;) {
      providers::ProviderExecutionResult r{};
      const auto taken = coordinator->TryTakeResult(owner_id, &r, response, sizeof(response));
      if (taken == providers::ProviderExecutionTakeStatus::kNone) return;
      if (taken != providers::ProviderExecutionTakeStatus::kTaken) continue;
      auto* c = static_cast<Client*>(r.context);
      if (!c || c < &clients[0] || c >= &clients[kClients]) continue;
      fortis::base::UniqueLock lock(mutex);
      const bool match = c->used && c->active && c->job == r.job_id;
      lock.Unlock();
      if (!match) continue;
      const bool transport = r.response.error != http::OutboundError::kNone;
      const bool cancelled = r.cancelled || r.deadline_exceeded;
      Deliver(static_cast<size_t>(c - clients), !transport && !cancelled && r.response.status > 0,
              r.response.status, transport, cancelled, r.lane_rejected, r.retry_after_ms, response,
              r.body_length);
    }
  }

  void Loop() {
    for (;;) {
      fortis::base::UniqueLock lock(mutex);
      const bool stop = stopping;
      lock.Unlock();
      Expire(stop);
      Drain();
      if (!stop) Schedule();
      bool live = false;
      lock.Lock();
      for (const Client& c : clients) live = live || c.used;
      lock.Unlock();
      if (stop && !live && !coordinator->live_job_count(owner_id)) break;
      lock.Lock();
      const timespec wake = wake_at(kWakeMs);
      changed.WaitUntilLocked(mutex, wake);
    }
    if (!coordinator->UnregisterOwner(owner_id)) abort();
    owner_id = 0;
  }

  // ---- one Run ----------------------------------------------------------------

  static bool PrepareBody(Attempt& attempt, Arena& arena) {
    Call& call = *attempt.call;
    if (call.kind == CallKind::kGet) return true;
    if (call.kind == CallKind::kRawPost) {
      if (call.raw_body_length > providers::kProviderExecutionMaxRequestBody || (call.raw_body_length && !call.raw_body))
        return false;
      attempt.body = base::AllocateStorage<uint8_t[]>(call.raw_body_length ? call.raw_body_length : 1);
      if (!attempt.body) return false;
      if (call.raw_body_length) memcpy(attempt.body.get(), call.raw_body, call.raw_body_length);
      attempt.body_length = call.raw_body_length;
      return true;
    }
    Value* document = nullptr;
    if (call.kind == CallKind::kJsonRpc) {
      document = JsonRpcClient::Envelope(arena, call);
      if (!document) return false;
    } else {
      document = const_cast<Value*>(call.body);
      if (!document) return false;
    }
    attempt.body = base::AllocateStorage<uint8_t[]>(providers::kProviderExecutionMaxRequestBody);
    if (!attempt.body) return false;
    OutBuffer out{attempt.body.get(), providers::kProviderExecutionMaxRequestBody, 0, true};
    if (!tj::Stringify(document, out) || !out.ok) return false;
    attempt.body_length = out.used;
    return true;
  }

  // Parses the last answer. False when the body is not JSON (HttpClient's
  // wrapped parse failure).
  static bool ParseBody(Attempt& attempt, Arena& arena, Value** parsed) {
    const uint8_t* text = attempt.response.get();
    size_t length = attempt.response_length;
    std::unique_ptr<uint8_t[], base::StorageDeleter> quoted;
    if (attempt.call->exact_integers && length) {
      const size_t capacity = length + length / 8 + 64;
      quoted = base::AllocateStorage<uint8_t[]>(capacity);
      if (!quoted) return false;
      OutBuffer out{quoted.get(), capacity, 0, true};
      if (!QuoteLongIntegers(text, length, out) || !out.ok) { quoted.reset(); return false; }
      text = quoted.get();
      length = out.used;
    }
    Value* value = text ? tj::Parse(arena, text, length) : nullptr;
    quoted.reset();
    if (!value) return false;
    *parsed = value;
    return true;
  }

  enum class Next : uint8_t { kDone, kFailover, kRetrySame };

  // Interprets the current endpoint's answer for the call.
  static Next Interpret(Attempt& attempt, Arena& arena) {
    Call& call = *attempt.call;
    call.endpoint = call.urls[attempt.endpoints.index()];
    if (attempt.cancelled) {
      call.failure = CallFailure::kCancelled;
      snprintf(call.message, sizeof(call.message), "cancelled");
      return Next::kDone;
    }
    if (attempt.lane_rejected) {
      attempt.lane_busy_seen = true;
      call.failure = CallFailure::kLaneBusy;
      call.retry_after_ms = attempt.retry_after_ms;
      snprintf(call.message, sizeof(call.message), "lane busy");
      return Next::kFailover;
    }
    attempt.lane_busy_seen = false;
    if (!attempt.answered) {
      call.failure = CallFailure::kTransport;
      snprintf(call.message, sizeof(call.message), "transport failure");
      return Next::kFailover;
    }
    call.http_status = attempt.status;
    const bool success = attempt.status >= 200 && attempt.status < 300;
    if (!success) {
      if (call.kind != CallKind::kJsonRpc && attempt.status >= 400 && attempt.status < 500) {
        // rethrowAnswer: a 4xx is an answer, not an outage.
        call.failure = CallFailure::kAnswer;
        snprintf(call.message, sizeof(call.message), "http %d", attempt.status);
        return Next::kDone;
      }
      call.failure = CallFailure::kTransport;
      snprintf(call.message, sizeof(call.message), "upstream %d", attempt.status);
      return Next::kFailover;
    }
    if (call.kind == CallKind::kRawPost) {
      uint8_t* copy = static_cast<uint8_t*>(arena.Alloc(attempt.response_length ? attempt.response_length : 1, 1));
      if (!copy) {
        call.failure = CallFailure::kTransport;
        snprintf(call.message, sizeof(call.message), "out of memory");
        return Next::kFailover;
      }
      if (attempt.response_length) memcpy(copy, attempt.response.get(), attempt.response_length);
      call.failure = CallFailure::kNone;
      call.message[0] = '\0';
      call.raw_response = copy;
      call.raw_response_length = attempt.response_length;
      return Next::kDone;
    }
    Value* body = nullptr;
    if (!ParseBody(attempt, arena, &body)) {
      call.failure = CallFailure::kTransport;
      snprintf(call.message, sizeof(call.message), "invalid JSON");
      if (ChainRetryPolicy::RetryParse(call, attempt.parse_retries)) {
        ++attempt.parse_retries;
        return Next::kRetrySame;
      }
      return Next::kFailover;
    }
    if (call.kind == CallKind::kJsonRpc) {
      return JsonRpcClient::Interpret(call, body) == JsonRpcClient::Outcome::kTransport ? Next::kFailover : Next::kDone;
    }
    if (call.validate && !call.validate(body)) {
      call.failure = CallFailure::kTransport;
      snprintf(call.message, sizeof(call.message), "invalid success response");
      return Next::kFailover;
    }
    call.failure = CallFailure::kNone;
    call.message[0] = '\0';
    call.value = body;
    return Next::kDone;
  }

  void Run(Call* calls, size_t count, Arena& arena, uint64_t deadline) {
    if (!count) return;
    const uint64_t now = providers::TokenMonotonicMs();
    if (deadline > now + kMaxDeadlineMs) deadline = now + kMaxDeadlineMs;
    std::unique_ptr<Attempt[]> attempts;
    try { attempts = std::make_unique<Attempt[]>(count); } catch (const std::bad_alloc&) { attempts.reset(); }
    if (!attempts) {
      for (size_t i = 0; i < count; ++i) {
        calls[i].failure = CallFailure::kTransport;
        snprintf(calls[i].message, sizeof(calls[i].message), "out of memory");
      }
      return;
    }
    Batch batch;
    // Every submitted callback borrows the batch and attempt array. Drain it
    // before either object is destroyed, including when validation throws.
    auto drain = [](Batch* pending) noexcept {
      base::UniqueLock lock(pending->mutex);
      while (pending->outstanding) pending->done.WaitLocked(pending->mutex);
    };
    std::unique_ptr<Batch, decltype(drain)> pending(&batch, drain);
    for (size_t i = 0; i < count; ++i) {
      Attempt& a = attempts[i];
      Call& call = calls[i];
      a.call = &call;
      call.failure = CallFailure::kTransport;
      call.http_status = 0;
      call.value = nullptr;
      call.message[0] = '\0';
      call.has_code = false;
      call.code = 0;
      call.retry_after_ms = 0;
      call.endpoint = nullptr;
      call.raw_response = nullptr;
      call.raw_response_length = 0;
      call.settled_rank = 0;
      if (!call.urls || !call.url_count) {
        call.failure = CallFailure::kNoEndpoints;
        snprintf(call.message, sizeof(call.message), "no endpoints configured");
        a.done = true;
        continue;
      }
      if (!PrepareBody(a, arena)) {
        snprintf(call.message, sizeof(call.message), "request body too large");
        a.done = true;
      }
    }
    // Event-driven, as each withFailover runs on its own: submit what needs
    // a request, wait for any answer, interpret it, fail over at once.
    uint32_t settled = 0;
    for (size_t i = 0; i < count; ++i)
      if (attempts[i].done) attempts[i].call->settled_rank = ++settled;
    for (;;) {
      fortis::base::UniqueLock lock(batch.mutex);
      const uint64_t now_ms = providers::TokenMonotonicMs();
      for (size_t i = 0; i < count; ++i) {
        Attempt& a = attempts[i];
        if (a.done || a.pending || a.arrived || (a.not_before > now_ms && !skip_sleep)) continue;
        a.not_before = 0;
        a.pending = true;
        ++batch.outstanding;
        lock.Unlock();
        const bool ok = Submit(&batch, &a, deadline);
        lock.Lock();
        if (!ok) {
          --batch.outstanding;
          a.pending = false;
          a.arrival_order = ++batch.arrival_sequence;
          a.arrived = true;
          a.answered = false;
          a.transport = true;
          a.cancelled = providers::TokenMonotonicMs() >= deadline;
          a.lane_rejected = false;
        }
      }
      bool open = false, ready = false;
      uint64_t next_retry = UINT64_MAX;
      for (size_t i = 0; i < count; ++i) {
        const Attempt& a = attempts[i];
        if (a.done) continue;
        open = true;
        if (a.arrived) ready = true;
        else if (!a.pending && a.not_before < next_retry) next_retry = a.not_before;
      }
      if (!open) break;
      if (!ready) {
        if (next_retry == UINT64_MAX) {
          batch.done.WaitLocked(batch.mutex);
        } else {
          const uint64_t at = providers::TokenMonotonicMs();
          const timespec wake = wake_at(next_retry > at ? next_retry - at : 0);
          batch.done.WaitUntilLocked(batch.mutex, wake);
        }
        continue;
      }
      lock.Unlock();
      // Several completions may be queued before this thread wakes. Consume
      // the earliest completion first, rather than the lowest call index:
      // Promise.all rejects with the first promise that settled.
      lock.Lock();
      size_t earliest = count;
      for (size_t i = 0; i < count; ++i)
        if (!attempts[i].done && attempts[i].arrived &&
            (earliest == count || attempts[i].arrival_order < attempts[earliest].arrival_order)) earliest = i;
      if (earliest < count) attempts[earliest].arrived = false;
      lock.Unlock();
      if (earliest < count) {
        Attempt& a = attempts[earliest];
        const Next next = Interpret(a, arena);
        if (next == Next::kRetrySame) {
          a.not_before = providers::TokenMonotonicMs() + ChainRetryPolicy::ParseDelayMs(a.parse_retries);
          continue;
        }
        if (next == Next::kFailover && a.endpoints.Advance(a.call->url_count, providers::TokenMonotonicMs(), deadline)) {
          // Failover: the next endpoint, submitted on the next pass.
          a.parse_retries = 0;
          continue;
        }
        a.done = true;
        a.call->settled_rank = ++settled;
      }
    }



  }
};

Transport::Transport(const TransportConfig& config, providers::ProviderExecutionCoordinator* coordinator)
    : impl_(fortis::base::New<Impl>(config, coordinator)) {}

Transport::~Transport() {
  if (impl_ && impl_->started && pthread_equal(pthread_self(), impl_->thread.native())) abort();
  Stop();
  impl_.reset();
}

bool Transport::Start() { return impl_ && impl_->Start(); }

void Transport::Stop() {
  if (!impl_ || !impl_->started) return;
  fortis::base::UniqueLock lock(impl_->mutex);
  impl_->stopping = true;
  impl_->changed.Broadcast();
  lock.Unlock();
  if (pthread_equal(pthread_self(), impl_->thread.native())) return;
  impl_->thread.Join();
  impl_->started = false;
}

bool Transport::running() const {
  if (!impl_) return false;
  fortis::base::LockGuard lock(impl_->mutex);
  const bool running = impl_->started && !impl_->stopping;
  return running;
}

void Transport::Run(Call* calls, size_t count, Arena& arena, uint64_t deadline_ms) {
  if (!impl_) {
    for (size_t i = 0; i < count; ++i) calls[i].failure = CallFailure::kTransport;
    return;
  }
  if (count > kTransportMaxCalls) {
    // A caller bug: never fan out beyond the bound.
    for (size_t i = 0; i < count; ++i) {
      calls[i].failure = CallFailure::kTransport;
      snprintf(calls[i].message, sizeof(calls[i].message), "too many calls");
    }
    return;
  }
  if (background_thread_set_ && pthread_equal(pthread_self(), background_thread_))
    for (size_t i = 0; i < count; ++i) calls[i].background = true;
  impl_->Run(calls, count, arena, deadline_ms);
}

void Transport::SetBackgroundThread(pthread_t thread) {
  background_thread_ = thread;
  background_thread_set_ = true;
}

const TransportConfig& Transport::config() const { return impl_->config; }

bool Transport::StartRedirected(const char* host, const char* port, const char* ca,
                                bool (*allow)(void*, uint32_t), void* context,
                                const char* real_host_header) {
  if (!impl_ || !impl_->redirect.Accepts(host, port, ca, allow, false) ||
      (real_host_header && !impl_->redirect.SetRealHostHeader(real_host_header)))
    return false;
  impl_->redirect.Set(host, port, ca, allow, context);
  impl_->skip_sleep = true;
  impl_->config.ca_bundle_configured = true;
  snprintf(impl_->config.ca_bundle_path, sizeof(impl_->config.ca_bundle_path), "%s", ca);
  return impl_->Start();
}

}  // namespace fortis::chains
