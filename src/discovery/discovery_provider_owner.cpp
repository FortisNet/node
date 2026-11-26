#include "exception_boundary.h"
#include "discovery_provider_owner.h"
#include "ownership.h"

#include "outbound_https.h"
#include "provider_execution_coordinator.h"
#include "threading.h"
#include "upstream_access.h"
#include "upstream_redirect.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::discovery {

namespace {
constexpr size_t kClients = 32;
constexpr size_t kFlights = 16;
constexpr uint64_t kWakeMs = 40;
constexpr uint32_t kTimeoutMs = 10000;
constexpr uint64_t kMaxDeadlineMs = 60000;
constexpr char kPort[] = "443";

timespec wake_at(uint64_t ms) {
  timespec out{};
  clock_gettime(CLOCK_REALTIME, &out);
  out.tv_sec += static_cast<time_t>(ms / 1000);
  out.tv_nsec += static_cast<long>((ms % 1000) * 1000000);
  if (out.tv_nsec >= 1000000000L) { ++out.tv_sec; out.tv_nsec -= 1000000000L; }
  return out;
}

// One row per DiscoveryUpstream, in enum order: its lane, its host and the
// deployment's credential rule (upstream_access.h). Adding an upstream is
// its enum value plus its row here.
struct UpstreamOps {
  DiscoveryUpstream upstream;
  const char* lane;
  // The upstream's fixed host; nullptr: `access` picks it.
  const char* host;
  // The deployment's host choice and credential; nullptr: keyless.
  providers::UpstreamAccessFn access;
};

constexpr UpstreamOps kUpstreams[] = {
    {DiscoveryUpstream::kCoinGecko, "coingecko", nullptr, providers::CoinGeckoAccess},
    {DiscoveryUpstream::kJupiter, "jupiter", nullptr, providers::JupiterAccess},
    // The route refuses before submitting when no key is configured.
    {DiscoveryUpstream::kTaostats, "taostats", "api.taostats.io", providers::TaostatsAccess},
    {DiscoveryUpstream::kRobinhood, "robinhood-assets", "api.robinhood.com", nullptr},
};
constexpr size_t kUpstreamCount = sizeof(kUpstreams) / sizeof(kUpstreams[0]);

constexpr bool upstreams_in_enum_order() {
  for (size_t i = 0; i < kUpstreamCount; ++i)
    if (static_cast<size_t>(kUpstreams[i].upstream) != i) return false;
  return true;
}
static_assert(kUpstreamCount == static_cast<size_t>(DiscoveryUpstream::kRobinhood) + 1 && upstreams_in_enum_order(),
              "kUpstreams needs one row per DiscoveryUpstream, in enum order");

const UpstreamOps& ops_of(DiscoveryUpstream upstream) {
  const size_t index = static_cast<size_t>(upstream);
  return kUpstreams[index < kUpstreamCount ? index : 0];
}
}  // namespace

uint64_t DiscoveryMonotonicMs() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1000000;
}

struct DiscoveryProviderOwner::Impl {
  struct Client {
    bool used = false, queued = true, cancelled = false, background = false, retry_unparseable = false;
    uint64_t id = 0, deadline = 0;
    size_t flight = kFlights;
    DiscoveryUpstream upstream = DiscoveryUpstream::kCoinGecko;
    DiscoveryProviderCompletionFn callback = nullptr;
    void* context = nullptr;
    char path[kDiscoveryMaxPathBytes]{};
  };
  struct Flight {
    bool used = false, active = false, orphaned = false, background = true, retry_unparseable = false;
    DiscoveryUpstream upstream = DiscoveryUpstream::kCoinGecko;
    uint64_t job = 0, deadline = 0;
    size_t waiter_count = 0;
    uint8_t waiters[kClients]{};
    char path[kDiscoveryMaxPathBytes]{};
  };
  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable changed;
  fortis::base::Thread thread;
  bool started = false, stopping = false;
  uint64_t next_id = 1, owner_id = 0;
  config::ProviderRuntimeConfig config{};
  providers::ProviderExecutionCoordinator* coordinator = nullptr;
  Client clients[kClients]{};
  Flight flights[kFlights]{};
  unsigned char response[kDiscoveryMaxResponseBytes]{};
  // Off in production (see upstream_redirect.h).
  http::UpstreamRedirect redirect;

  Impl(const config::ProviderRuntimeConfig& cfg, providers::ProviderExecutionCoordinator* shared)
      : config(cfg), coordinator(shared) {}
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
    if (pthread_attr_init(&attr) != 0) {
      coordinator->UnregisterOwner(owner_id); owner_id = 0; return false;
    }
    if (pthread_attr_setstacksize(&attr, 256 * 1024) != 0) {
      pthread_attr_destroy(&attr); coordinator->UnregisterOwner(owner_id); owner_id = 0;
      return false;
    }
    const int rc = (thread.StartEntry<Entry>(this, &attr) ? 0 : -1);
    pthread_attr_destroy(&attr);
    if (rc != 0) { coordinator->UnregisterOwner(owner_id); owner_id = 0; return false; }
    started = true;
    return true;
  }

  bool Submit(DiscoveryUpstream upstream, const char* path, uint64_t deadline, bool background,
              DiscoveryProviderCompletionFn callback, void* context, uint64_t* id, bool retry_unparseable) {
    const uint64_t now = DiscoveryMonotonicMs();
    if (!path || path[0] != '/' || !callback || !id || deadline <= now ||
        deadline - now > kMaxDeadlineMs) return false;
    const size_t length = strnlen(path, kDiscoveryMaxPathBytes);
    if (length >= kDiscoveryMaxPathBytes) return false;
    fortis::base::LockGuard lock(mutex);
    if (!started || stopping) return false;
    size_t ci = kClients;
    for (size_t i = 0; i < kClients; ++i) if (!clients[i].used) { ci = i; break; }
    if (ci == kClients) return false;
    uint64_t request_id = next_id++;
    if (!request_id) request_id = next_id++;
    Client& c = clients[ci];
    c.used = true; c.queued = true; c.cancelled = false; c.background = background;
    c.id = request_id; c.deadline = deadline; c.flight = kFlights; c.upstream = upstream;
    c.callback = callback; c.context = context; c.retry_unparseable = retry_unparseable;
    memcpy(c.path, path, length + 1);
    *id = request_id;
    changed.Signal();
    return true;
  }

  bool Cancel(uint64_t id) {
    fortis::base::LockGuard lock(mutex);
    for (Client& c : clients) if (c.used && c.id == id) {
      c.cancelled = true;
      changed.Signal();
      return true;
    }
    return false;
  }

  bool Build(const Flight& f, http::OutboundRequest* out) {
    http::OutboundRequest request{};
    request.method = http::OutboundMethod::kGet;
    request.port = kPort;
    request.path = f.path;
    request.ca_file = config.ca_bundle_path;
    request.timeout_ms = kTimeoutMs;
    request.max_response_bytes = kDiscoveryMaxResponseBytes;
    const UpstreamOps& ops = ops_of(f.upstream);
    providers::UpstreamAccess access{};
    access.host = ops.host;
    if (ops.access && !ops.access(config, &access)) return false;
    request.host = access.host;
    const char* header = access.header;
    const char* value = access.value;
    // HttpClient.json sends `accept: application/json` on every request
    // (CoinGecko, Jupiter, Taostats and Robinhood alike), then the key.
    request.header_count = 1;
    snprintf(request.headers[0].name, sizeof(request.headers[0].name), "accept");
    snprintf(request.headers[0].value, sizeof(request.headers[0].value), "application/json");
    if (header) {
      const size_t length = strnlen(value, 256);
      if (!length || length >= sizeof(request.headers[1].value)) return false;
      request.header_count = 2;
      snprintf(request.headers[1].name, sizeof(request.headers[1].name), "%s", header);
      memcpy(request.headers[1].value, value, length + 1);
    }
    redirect.Apply(request);
    if (!OutboundHttpsHeadersValid(request)) return false;
    *out = request;
    return true;
  }

  void Deliver(size_t ci, const DiscoveryProviderResult& result,
               const unsigned char* body = nullptr, size_t length = 0) {
    fortis::base::UniqueLock lock(mutex);
    if (ci >= kClients || !clients[ci].used) return;
    const DiscoveryProviderCompletionFn callback = clients[ci].callback;
    void* context = clients[ci].context;
    clients[ci].used = false;
    clients[ci].flight = kFlights;
    lock.Unlock();
    fortis::base::FatalBoundary("discovery completion", [&] { callback(context, result, body, length); });
  }

  void Finish(size_t fi, const DiscoveryProviderResult& result,
              const unsigned char* body = nullptr, size_t length = 0) {
    Flight& f = flights[fi];
    const size_t count = f.waiter_count;
    uint8_t waiters[kClients]{};
    memcpy(waiters, f.waiters, count);
    f.used = f.active = f.orphaned = false;
    f.waiter_count = 0;
    for (size_t i = 0; i < count; ++i) Deliver(waiters[i], result, body, length);
  }

  void Admit() {
    for (size_t ci = 0; ci < kClients; ++ci) {
      fortis::base::UniqueLock lock(mutex);
      Client& c = clients[ci];
      if (!c.used || !c.queued) continue;
      if (c.cancelled || DiscoveryMonotonicMs() >= c.deadline) {
        lock.Unlock();
        DiscoveryProviderResult r{}; r.cancelled = true; Deliver(ci, r);
        continue;
      }
      size_t fi = kFlights;
      for (size_t i = 0; i < kFlights; ++i)
        if (flights[i].used && !flights[i].orphaned && flights[i].upstream == c.upstream &&
            flights[i].retry_unparseable == c.retry_unparseable && strcmp(flights[i].path, c.path) == 0) {
          fi = i;
          break;
        }
      if (fi == kFlights) {
        for (size_t i = 0; i < kFlights; ++i) if (!flights[i].used) { fi = i; break; }
        if (fi == kFlights) continue;  // retry next pass
        Flight& f = flights[fi];
        f.used = true; f.active = false; f.orphaned = false; f.background = c.background;
        f.upstream = c.upstream; f.job = 0; f.deadline = c.deadline; f.waiter_count = 0;
        f.retry_unparseable = c.retry_unparseable;
        memcpy(f.path, c.path, strnlen(c.path, kDiscoveryMaxPathBytes) + 1);
      } else {
        Flight& f = flights[fi];
        f.background = f.background && c.background;
        if (c.deadline > f.deadline) f.deadline = c.deadline;
      }
      Flight& f = flights[fi];
      f.waiters[f.waiter_count++] = static_cast<uint8_t>(ci);
      c.queued = false;
      c.flight = fi;
    }
  }

  void CancelWaiters() {
    for (size_t ci = 0; ci < kClients; ++ci) {
      fortis::base::UniqueLock lock(mutex);
      const bool cancel = clients[ci].used && !clients[ci].queued &&
          (clients[ci].cancelled || stopping || DiscoveryMonotonicMs() >= clients[ci].deadline);
      const size_t fi = clients[ci].flight;
      lock.Unlock();
      if (!cancel) continue;
      if (fi < kFlights) {
        Flight& f = flights[fi];
        for (size_t i = 0; i < f.waiter_count; ++i) if (f.waiters[i] == ci) {
          for (size_t j = i + 1; j < f.waiter_count; ++j) f.waiters[j - 1] = f.waiters[j];
          --f.waiter_count;
          break;
        }
        if (!f.waiter_count) {
          if (f.active) { f.orphaned = true; coordinator->Cancel(owner_id, f.job); }
          else f.used = false;
        }
      }
      DiscoveryProviderResult r{}; r.cancelled = true; Deliver(ci, r);
    }
  }

  // Foreground flights are dispatched before background ones admitted in the
  // same pass: a revalidation submitted with (after) a request's cold reads
  // then meets them in flight and is shed, as TypeScript's lane sheds it.
  void Schedule() {
    for (size_t pass = 0; pass < 2 * kFlights; ++pass) {
      const size_t i = pass % kFlights;
      Flight& f = flights[i];
      if (f.background != (pass >= kFlights)) continue;
      if (!f.used || f.active || !f.waiter_count) continue;
      if (coordinator->live_job_count(owner_id) >= providers::kProviderExecutionMaxJobs) return;
      http::OutboundRequest request{};
      if (!Build(f, &request)) {
        DiscoveryProviderResult r{}; r.transport_error = true; Finish(i, r);
        continue;
      }
      providers::ProviderExecutionOptions options{};
      options.max_attempts = providers::kProviderExecutionMaxAttempts;
      options.deadline_ms = f.deadline;
      options.background = f.background;
      // HttpClient.json: a 2xx whose body is not JSON is a status-less
      // UpstreamError, retried like a transport failure (token search,
      // Bittensor analytics and price markets ask for it).
      options.retry_unparseable_json = f.retry_unparseable;
      const auto submitted = coordinator->Submit(owner_id, ops_of(f.upstream).lane, request, &f,
                                                 options);
      if (submitted.status == providers::ProviderExecutionSubmitStatus::kBusy) continue;
      if (submitted.status != providers::ProviderExecutionSubmitStatus::kAccepted) {
        DiscoveryProviderResult r{};
        r.transport_error = true;
        r.lane_rejected = submitted.status == providers::ProviderExecutionSubmitStatus::kLaneRejected;
        r.retry_after_ms = static_cast<uint32_t>(submitted.retry_after_ms);
        Finish(i, r);
        continue;
      }
      f.active = true;
      f.job = submitted.job_id;
    }
  }

  void Drain() {
    for (;;) {
      providers::ProviderExecutionResult r{};
      const auto taken = coordinator->TryTakeResult(owner_id, &r, response, sizeof(response));
      if (taken == providers::ProviderExecutionTakeStatus::kNone) return;
      if (taken != providers::ProviderExecutionTakeStatus::kTaken) continue;
      auto* f = static_cast<Flight*>(r.context);
      if (!f || f < &flights[0] || f >= &flights[kFlights] || !f->active || f->job != r.job_id)
        continue;
      DiscoveryProviderResult out{};
      out.http_status = r.response.status;
      out.attempts = r.attempts;
      out.retry_after_ms = r.retry_after_ms;
      out.transport_error = r.response.error != http::OutboundError::kNone;
      out.cancelled = r.cancelled || r.deadline_exceeded;
      out.lane_rejected = r.lane_rejected;
      Finish(static_cast<size_t>(f - flights), out, response, r.body_length);
    }
  }

  void Loop() {
    for (;;) {
      fortis::base::UniqueLock lock(mutex);
      const bool stop = stopping;
      lock.Unlock();
      CancelWaiters();
      for (Flight& f : flights) if (f.used && !f.active && !f.waiter_count) f.used = false;
      if (stop) {
        // Queued clients never reached a flight; answer them now.
        for (size_t ci = 0; ci < kClients; ++ci) {
          lock.Lock();
          const bool queued = clients[ci].used && clients[ci].queued;
          lock.Unlock();
          if (queued) { DiscoveryProviderResult r{}; r.cancelled = true; Deliver(ci, r); }
        }
      } else {
        Admit();
      }
      Drain();
      if (!stop) { Admit(); Schedule(); }
      bool clients_live = false, flights_live = false;
      lock.Lock();
      for (const Client& c : clients) clients_live = clients_live || c.used;
      lock.Unlock();
      for (const Flight& f : flights) flights_live = flights_live || f.used;
      if (stop && !clients_live && !flights_live && !coordinator->live_job_count(owner_id)) break;
      lock.Lock();
      const timespec wake = wake_at(kWakeMs);
      changed.WaitUntilLocked(mutex, wake);
    }
    if (!coordinator->UnregisterOwner(owner_id)) abort();
    owner_id = 0;
  }
};

DiscoveryProviderOwner::DiscoveryProviderOwner(const config::ProviderRuntimeConfig& config,
    providers::ProviderExecutionCoordinator* coordinator) : impl_(fortis::base::New<Impl>(config, coordinator)) {}

DiscoveryProviderOwner::~DiscoveryProviderOwner() {
  if (impl_ && impl_->started && pthread_equal(pthread_self(), impl_->thread.native())) abort();
  Stop();
  impl_.reset();
}

bool DiscoveryProviderOwner::Start() { return impl_ && impl_->Start(); }

bool DiscoveryProviderOwner::Submit(DiscoveryUpstream upstream, const char* path,
    uint64_t deadline_ms, bool background, DiscoveryProviderCompletionFn completion,
    void* context, uint64_t* request_id, bool retry_unparseable) {
  return impl_ && impl_->Submit(upstream, path, deadline_ms, background, completion, context,
                                request_id, retry_unparseable);
}

bool DiscoveryProviderOwner::Cancel(uint64_t request_id) {
  return impl_ && impl_->Cancel(request_id);
}

bool DiscoveryProviderOwner::running() const {
  if (!impl_) return false;
  fortis::base::LockGuard lock(impl_->mutex);
  const bool running = impl_->started && !impl_->stopping;
  return running;
}

void DiscoveryProviderOwner::Stop() {
  if (!impl_ || !impl_->started) return;
  fortis::base::UniqueLock lock(impl_->mutex);
  impl_->stopping = true;
  impl_->changed.Broadcast();
  lock.Unlock();
  if (pthread_equal(pthread_self(), impl_->thread.native())) return;
  impl_->thread.Join();
  impl_->started = false;
}

bool DiscoveryProviderOwner::StartRedirected(const char* host, const char* port, const char* ca,
    bool (*allow)(void*, uint32_t), void* context) {
  if (!impl_ || !impl_->redirect.Set(host, port, ca, allow, context)) return false;
  impl_->config.ca_bundle_configured = true;
  snprintf(impl_->config.ca_bundle_path, sizeof(impl_->config.ca_bundle_path), "%s", ca);
  return impl_->Start();
}

}  // namespace fortis::discovery
