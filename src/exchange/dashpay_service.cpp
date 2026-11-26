#include <algorithm>
#include <memory>
#include "dashpay_service.h"
#include "log.h"
#include "ownership.h"
#include "query_string.h"

#include "chain_core.h"
#include "chain_transport.h"
#include "device_enrollment.h"
#include "device_policy_step.h"
#include "price_history_batch_codec.h"
#include "provider_execution_coordinator.h"
#include "threading.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::exchange {

namespace {

namespace hb = fortis::codec::history_batch;
namespace dp = fortis::exchange::dashpay;

constexpr const char* kJsonType = http::kHttpJsonUtf8;
constexpr const char* kProtobufType = "application/protobuf";
constexpr const char* kInvalidName = "{\"error\":\"INVALID_DASHPAY_NAME\"}";
constexpr const char* kUnavailable = "{\"error\":\"DASHPAY_UNAVAILABLE\"}";
constexpr uint32_t kHandoffTimeoutMs = 30000;

uint64_t monotonic_ms() { return fortis::chains::ChainMonotonicMs(); }

timespec realtime_after(uint64_t ms) {
  timespec out{};
  clock_gettime(CLOCK_REALTIME, &out);
  out.tv_sec += static_cast<time_t>(ms / 1000);
  out.tv_nsec += static_cast<long>((ms % 1000) * 1000000);
  if (out.tv_nsec >= 1000000000L) { ++out.tv_sec; out.tv_nsec -= 1000000000L; }
  return out;
}

using Policy = auth::DeviceQuota;

http::HttpResponse json_response(int status, const char* reason, const char* body, const Policy& policy) {
  http::HttpResponse response{status, reason, kJsonType, reinterpret_cast<const uint8_t*>(body), strlen(body)};
  response.rate_limit_cost = policy.cost;
  response.rate_limit_remaining = policy.remaining;
  return response;
}

// GET /v1/dashpay/names/resolve: the device hook at cost 1.
constexpr http::HttpStep<auth::DeviceStepState> kResolveSteps[] = {auth::RequireDevice<auth::DeviceStepState, 1>};

}  // namespace

}  // namespace fortis::exchange

namespace fortis::exchange::dashpay {

namespace {

// The TypeScript normalizeDashPayName of the zod-trimmed value:
// `alice`, `@alice` or `alice.dash` (case-insensitive suffix) with a DPNS
// label /^[a-zA-Z0-9][a-zA-Z0-9-]{1,61}[a-zA-Z0-9]$/, lowercased + ".dash".
// Any non-ASCII code point fails the label, so the ASCII case fold here is
// String.prototype.toLowerCase for every name that can pass.
bool normalize_name(const char* text, size_t length, char* label, size_t* label_length) {
  if (length && text[0] == '@') { ++text; --length; }
  if (length >= 5) {
    const char* tail = text + length - 5;
    bool suffix = tail[0] == '.';
    static const char kDash[] = "dash";
    for (size_t i = 0; i < 4 && suffix; ++i) {
      char ch = tail[1 + i];
      if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
      suffix = ch == kDash[i];
    }
    if (suffix) length -= 5;
  }
  if (length < 3 || length > kMaxLabelBytes) return false;
  for (size_t i = 0; i < length; ++i) {
    char ch = text[i];
    const bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
    if (!alnum && !(ch == '-' && i != 0 && i + 1 != length)) return false;
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    label[i] = ch;
  }
  *label_length = length;
  return true;
}

}  // namespace

NameQuery ParseNameQuery(const char* raw_target, char label[kMaxLabelBytes + 1], size_t* label_length) {
  *label_length = 0;
  label[0] = '\0';
  if (!raw_target) return NameQuery::kInvalid;
  // Keep the route's existing ASCII request-target policy.
  for (const char* p = raw_target; *p; ++p)
    if (static_cast<unsigned char>(*p) >= 0x80) return NameQuery::kInvalid;
  const auto query = http::query_string::Query::FromTarget(raw_target);
  http::query_string::Component value;
  if (!query.Valid() || !query.First("name", &value)) return NameQuery::kInvalid;
  // Decoding never grows the component. Retain full padding for the trim
  // rule rather than imposing a new bound on the untrimmed input.
  auto scratch_storage = base::AllocateStorage<char[]>(value.size + 1);
  auto* scratch = scratch_storage.get();
  if (!scratch) return NameQuery::kNoMemory;
  const size_t length = value.Decode(scratch, value.size);
  bool valid = length != SIZE_MAX;
  if (valid) {
    size_t begin = 0, end = 0;
    hb::JsTrim(scratch, length, &begin, &end);
    // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
    // cppcheck-suppress arithOperationsOnVoidPointer
    const size_t units = hb::Utf16Units(scratch + begin, end - begin);
    // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
    // cppcheck-suppress arithOperationsOnVoidPointer
    valid = units >= 1 && units <= kNameUnits && normalize_name(scratch + begin, end - begin, label, label_length);
  }
  scratch_storage.reset();
  if (!valid) {
    *label_length = 0;
    return NameQuery::kInvalid;
  }
  label[*label_length] = '\0';
  return NameQuery::kValid;
}

bool ValidPlatformIdentifier(const char* text) {
  const size_t length = strlen(text);
  if (length < 32 || length > 50) return false;
  uint8_t bytes[64];
  size_t written = 0;
  return fortis::chains::Base58Decode(text, length, bytes, sizeof(bytes), &written) && written == 32;
}

size_t EncodeNameResponse(const char* label, size_t label_length, const char* identity, bool protobuf, uint8_t* out,
                          size_t capacity) {
  if (label_length < 3 || label_length > kMaxLabelBytes || (identity && strlen(identity) > 50) ||
      capacity < kNameResponseBytes)
    return 0;
  // The canonical name: the query's (the SDK echoes the input username).
  char name[kMaxLabelBytes + 6];
  memcpy(name, label, label_length);
  memcpy(name + label_length, ".dash", 6);
  const size_t name_length = label_length + 5;
  size_t length = 0;
  if (protobuf) {
    // DashPayNameResponse{string name = 1; optional string identity_id = 2}:
    // both lengths fit a one-byte varint.
    out[length++] = 0x0a;
    out[length++] = static_cast<uint8_t>(name_length);
    memcpy(out + length, name, name_length);
    length += name_length;
    if (identity) {
      const size_t identity_length = strlen(identity);
      out[length++] = 0x12;
      out[length++] = static_cast<uint8_t>(identity_length);
      std::copy_n(identity, identity_length, out + length);
      length += identity_length;
    }
    return length;
  }
  // JSON.stringify of {name, identityId}: [a-z0-9.-] and Base58 need no escape.
  const int written = identity ? snprintf(reinterpret_cast<char*>(out), capacity,
                                          "{\"name\":\"%s\",\"identityId\":\"%s\"}", name, identity)
                               : snprintf(reinterpret_cast<char*>(out), capacity,
                                          "{\"name\":\"%s\",\"identityId\":null}", name);
  return written > 0 && static_cast<size_t>(written) < capacity ? static_cast<size_t>(written) : 0;
}

}  // namespace fortis::exchange::dashpay

namespace fortis::exchange {

struct DashPayService::Impl {
  struct Job {
    bool used = false;
    bool head = false;
    bool protobuf = false;
    uint64_t token = 0;
    uint64_t deadline = 0;
    Policy policy;
    char label[dp::kMaxLabelBytes + 1]{};
    size_t label_length = 0;
  };

  DashPayConfig config{};
  DashPayServiceSeams seams{};
  std::unique_ptr<fortis::chains::Transport, fortis::base::ObjectDeleter<fortis::chains::Transport>> transport;
  dp::Client client;
  http::HttpResponseHandoff* handoff = nullptr;
  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable changed;
  fortis::base::ConditionVariable idle;
  fortis::base::Thread workers[kWorkers];
  size_t started_workers = 0;
  bool stopping = false;
  Job jobs[kJobs]{};
  Job* queue[kJobs]{};
  size_t queue_head = 0, queue_count = 0;
  size_t running = 0;
  char last_failure[96]{};
  uint64_t last_log_ms = 0;

  ~Impl() = default;


  void Resolve(const http::HttpRequest& request, http::HttpReply& reply) {
    auth::DeviceStepState state;
    state.attestation_required = config.attestation_required;
    if (!RunHttpSteps(kResolveSteps, request, reply, state)) return;
    const Policy policy = DeviceQuotaOf(state.device);
    char label[dp::kMaxLabelBytes + 1];
    size_t label_length = 0;
    const dp::NameQuery query = dp::ParseNameQuery(request.raw_target, label, &label_length);
    if (query == dp::NameQuery::kNoMemory) {
      reply.Send(json_response(503, "Service Unavailable", kUnavailable, policy));
      return;
    }
    if (query != dp::NameQuery::kValid) {
      reply.Send(json_response(400, "Bad Request", kInvalidName, policy));
      return;
    }
    Job* job = Reserve();
    if (!job) {
      // Native admission: every worker slot is taken. Unverified, so the
      // route's own failure answer.
      reply.Send(json_response(503, "Service Unavailable", kUnavailable, policy));
      return;
    }
    job->head = reply.head();
    job->protobuf = AcceptsProtobuf(request);
    job->policy = policy;
    memcpy(job->label, label, label_length);
    job->label_length = label_length;
    uint64_t token = 0;
    if (!reply.Reserve(kHandoffTimeoutMs, &token)) {
      reply.Send(json_response(503, "Service Unavailable", kUnavailable, policy));
      Release(job);
      return;
    }
    job->token = token;
    job->deadline = monotonic_ms() + kLookupBudgetMs;
    fortis::base::LockGuard lock(mutex);
    queue[(queue_head + queue_count) % kJobs] = job;
    ++queue_count;
    changed.Broadcast();
  }

  Job* Reserve() {
    fortis::base::LockGuard lock(mutex);
    Job* job = nullptr;
    if (!stopping)
      for (Job& candidate : jobs)
        if (!candidate.used) { job = &candidate; break; }
    if (job) job->used = true;
    return job;
  }

  void Release(Job* job) {
    fortis::base::LockGuard lock(mutex);
    *job = Job{};
    idle.Broadcast();
  }

  static void WorkerMain(Impl* self) { self->WorkerLoop(); }

  void WorkerLoop() {
    for (;;) {
      fortis::base::UniqueLock lock(mutex);
      while (!queue_count && !stopping) changed.WaitLocked(mutex);
      if (!queue_count && stopping) return;
      Job* job = queue[queue_head];
      queue_head = (queue_head + 1) % kJobs;
      --queue_count;
      ++running;
      lock.Unlock();
      Run(job);
      lock.Lock();
      --running;
      lock.Unlock();
      Release(job);
    }
  }

  void Run(Job* job) {
    fortis::base::UniqueLock lock(mutex);
    const bool shutting_down = stopping;
    lock.Unlock();
    if (shutting_down) {
      // A job still queued at shutdown is answered, not looked up.
      handoff->Complete(job->token, json_response(503, "Service Unavailable", kUnavailable, job->policy));
      return;
    }
    const dp::LookupResult lookup = client.Resolve(job->label, job->label_length, job->deadline);
    if (lookup.outcome == dp::Outcome::kUnavailable) {
      // request.log.warn('proof-verified DPNS lookup unavailable'): the
      // diagnostic only (never the name or an identity), at most every 10 s.
      lock.Lock();
      snprintf(last_failure, sizeof(last_failure), "%s", lookup.reason);
      const uint64_t now = monotonic_ms();
      const bool log = !last_log_ms || now - last_log_ms >= 10000;
      if (log) last_log_ms = now;
      lock.Unlock();
      if (log) fortis::base::LogWarn("dashpay", "dashpay: proof-verified DPNS lookup unavailable (%s)", lookup.reason);
    }
    dp::Outcome outcome = lookup.outcome;
    char identity[64] = "";
    if (outcome == dp::Outcome::kFound &&
        !fortis::chains::Base58Encode(lookup.identity, dp::kHashBytes, identity, sizeof(identity)))
      outcome = dp::Outcome::kUnavailable;
    if (seams.lookup_hook) seams.lookup_hook->AfterLookup(&outcome, identity, sizeof(identity));
    // The route's own fail-closed check of a returned identity.
    if (outcome == dp::Outcome::kFound && !dp::ValidPlatformIdentifier(identity)) outcome = dp::Outcome::kUnavailable;
    uint8_t body[dp::kNameResponseBytes];
    const size_t length =
        outcome == dp::Outcome::kUnavailable
            ? 0
            : dp::EncodeNameResponse(job->label, job->label_length, outcome == dp::Outcome::kFound ? identity : nullptr,
                                     job->protobuf, body, sizeof(body));
    if (!length) {
      handoff->Complete(job->token, json_response(503, "Service Unavailable", kUnavailable, job->policy));
      return;
    }
    http::HttpResponse response{200, "OK", job->protobuf ? kProtobufType : kJsonType, body, length};
    response.vary_accept = true;
    response.rate_limit_cost = job->policy.cost;
    response.rate_limit_remaining = job->policy.remaining;
    handoff->Complete(job->token, response);
  }

  bool WaitIdle(uint32_t timeout_ms) {
    const uint64_t deadline = monotonic_ms() + timeout_ms;
    fortis::base::LockGuard lock(mutex);
    for (;;) {
      bool busy = queue_count || running;
      for (const Job& job : jobs) busy = busy || job.used;
      if (!busy) return true;
      const uint64_t now = monotonic_ms();
      if (now >= deadline) return false;
      const timespec wake = realtime_after(deadline - now < 50 ? deadline - now : 50);
      idle.WaitUntilLocked(mutex, wake);
    }
  }
};

void DashPayService::ImplDeleter::operator()(Impl* impl) const { fortis::base::Delete(impl); }

DashPayService::~DashPayService() { Stop(); }

DashPayService::DashPayService(Impl* impl) : impl_(impl) {}
DashPayService::DashPayService(DashPayService&&) noexcept = default;

base::Result<DashPayService> DashPayService::Create(
    const DashPayConfig& config, DashPayServiceDeps deps, const DashPayServiceSeams* seams) try {
  auto& coordinator = deps.coordinator;
  Impl* impl = fortis::base::New<Impl>();
  if (!impl) return base::Status(base::StatusCode::kResourceExhausted, "dashpay allocation");
  DashPayService service(impl);
  impl->config = config;
  if (seams) impl->seams = *seams;
  impl->transport.reset(fortis::base::New<fortis::chains::Transport>(config.transport, &coordinator));
  bool ok = impl->transport != nullptr;
  if (ok) {
    ok = impl->seams.transport_starter ? impl->seams.transport_starter->Start(*impl->transport)
                                       : impl->transport->Start();
  }
  if (ok) {
    dp::ClientSeams client_seams;
    client_seams.clock = impl->seams.clock;
    client_seams.node_choice = impl->seams.node_choice;
    client_seams.connect_ms = impl->seams.connect_ms;
    client_seams.lookup_ms = impl->seams.lookup_ms;
    client_seams.request_ms = impl->seams.request_ms;
    ok = impl->client.Start(impl->transport.get(), &client_seams);
  }
  for (size_t i = 0; ok && i < kWorkers; ++i) {
    ok = impl->workers[i].Start(Impl::WorkerMain, impl, 256 * 1024);
    if (ok) ++impl->started_workers;
  }
  if (!ok) {
    return base::Status(base::StatusCode::kUnavailable, "dashpay initialization");
  }
  return service;
} catch (const std::bad_alloc&) {
  return base::Status(base::StatusCode::kResourceExhausted, "dashpay allocation");
}

bool DashPayService::Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff) {
  impl_->handoff = &handoff;
  return router.Add<&Impl::Resolve>("GET", "/v1/dashpay/names/resolve", *impl_);
}

void DashPayService::Stop() {
  if (!impl_) return;
  fortis::base::UniqueLock lock(impl_->mutex);
  impl_->stopping = true;
  impl_->changed.Broadcast();
  lock.Unlock();
  // Stopping the transport first cancels in-flight evonode and quorum
  // requests, so a running lookup ends (unverified, 503) instead of waiting
  // for its deadline; later attempts fail at once.
  if (impl_->transport) impl_->transport->Stop();
  for (size_t i = 0; i < impl_->started_workers; ++i) impl_->workers[i].Join();
  impl_->client.Stop();
  impl_->transport.reset();
  impl_.reset();
}

bool DashPayService::WaitIdle(uint32_t timeout_ms) { return impl_->WaitIdle(timeout_ms); }

uint64_t DashPayService::generation() const { return impl_->client.generation(); }

void DashPayService::last_failure(char* out, size_t capacity) const {
  if (!capacity) return;
  out[0] = '\0';
  fortis::base::LockGuard lock(impl_->mutex);
  snprintf(out, capacity, "%s", impl_->last_failure);
}

}  // namespace fortis::exchange
