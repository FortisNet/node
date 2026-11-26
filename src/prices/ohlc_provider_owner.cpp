#include "exception_boundary.h"
#include "ohlc_provider_owner.h"
#include "ownership.h"

#include "outbound_https.h"
#include "provider_execution_coordinator.h"
#include "threading.h"
#include "upstream_redirect.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::prices {

namespace {
constexpr size_t kClients = 8;
constexpr size_t kFlights = 8;
constexpr uint64_t kWakeMs = 40;
constexpr uint32_t kTimeoutMs = 10000;
// HttpClient RETRY_DELAYS_MS, used only for a 2xx body that is not JSON (the
// coordinator retries transport errors, 5xx and 408 itself).
constexpr uint64_t kBodyRetryDelayMs[] = {250, 1000};
constexpr char kFreeHost[] = "api.coingecko.com";
constexpr char kProHost[] = "pro-api.coingecko.com";
constexpr char kPort[] = "443";
uint64_t now_ms(void*) {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}
timespec wake_at(uint64_t ms) {
  timespec out{}; clock_gettime(CLOCK_REALTIME, &out);
  out.tv_sec += static_cast<time_t>(ms / 1000);
  out.tv_nsec += static_cast<long>((ms % 1000) * 1000000);
  if (out.tv_nsec >= 1000000000L) { ++out.tv_sec; out.tv_nsec -= 1000000000L; }
  return out;
}
bool valid_slug(const char* in, char out[101]) {
  if (!in) return false;
  const size_t n = strnlen(in, 101);
  if (!n || n >= 101) return false;
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = static_cast<unsigned char>(in[i]);
    if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + 32);
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-' && i))) return false;
    out[i] = static_cast<char>(c);
  }
  out[n] = 0; return true;
}
}

struct OhlcProviderOwner::Impl {
  struct Client {
    bool used = false, queued = true, cancelled = false, background = false;
    uint64_t id = 0, deadline = 0;
    size_t flight = kFlights;
    char slug[101]{};
    int days = 0;
    OhlcProviderCompletionFn callback = nullptr;
    void* context = nullptr;
  };
  struct Flight {
    bool used = false, active = false, orphaned = false, background = true;
    char slug[101]{}; int days = 0;
    uint64_t job = 0;
    unsigned body_retries = 0;
    uint64_t retry_at = 0;  // monotonic ms before which the flight is not scheduled
    size_t waiter_count = 0;
    uint8_t waiters[kClients]{};
  };
  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable changed;
  fortis::base::Thread thread;
  bool started = false, stopping = false, exited = false;
  uint64_t next_id = 1, owner_id = 0;
  config::ProviderRuntimeConfig config{};
  providers::ProviderExecutionCoordinator* coordinator = nullptr;
  OhlcBodyRetryFn body_retry = nullptr;  // under mutex
  Client clients[kClients]{};
  Flight flights[kFlights]{};
  unsigned char response[providers::kProviderExecutionDefaultResponseBody]{};
  // Off in production (see upstream_redirect.h).
  http::UpstreamRedirect redirect;
#ifdef FORTIS_HISTORY_BACKFILL_TEST_HOOKS
  void (*test_admission_hook)(void*, uint64_t) = nullptr;
  void* test_admission_context = nullptr;
#endif
  Impl(const config::ProviderRuntimeConfig& cfg, providers::ProviderExecutionCoordinator* shared)
      : config(cfg), coordinator(shared) {}
  ~Impl() = default;
  static void Wake(void* p) {
    auto* self = static_cast<Impl*>(p); fortis::base::LockGuard lock(self->mutex);
    self->changed.Signal();
  }
  static void* Entry(void* p) { static_cast<Impl*>(p)->Loop(); return nullptr; }
  bool Start() {
    if (started || !coordinator || !coordinator->valid() || !config.ca_bundle_configured ||
        !config.ca_bundle_path[0]) return false;
    owner_id = coordinator->RegisterOwner(Wake, this);
    if (!owner_id) return false;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) { coordinator->UnregisterOwner(owner_id); owner_id = 0; return false; }
    if (pthread_attr_setstacksize(&attr, 256 * 1024) != 0) {
      pthread_attr_destroy(&attr); coordinator->UnregisterOwner(owner_id); owner_id = 0;
      return false;
    }
    const int rc = (thread.StartEntry<Entry>(this, &attr) ? 0 : -1);
    pthread_attr_destroy(&attr);
    if (rc != 0) { coordinator->UnregisterOwner(owner_id); owner_id = 0; return false; }
    started = true; return true;
  }
  bool Submit(const char* raw_slug, int days, uint64_t deadline,
      OhlcProviderCompletionFn callback, void* context, uint64_t* id, bool background) {
    char slug[101]; const uint64_t now = now_ms(nullptr);
    const bool valid_days = days == 1 || days == 7 || days == 14 || days == 30 ||
        days == 90 || days == 180 || days == 365;
    if (!valid_slug(raw_slug, slug) || !valid_days || !callback || !id || deadline <= now ||
        deadline - now > 30000) return false;
    fortis::base::LockGuard lock(mutex);
    if (!started || stopping) return false;
    size_t ci = kClients;
    for (size_t i = 0; i < kClients; ++i) if (!clients[i].used) { ci = i; break; }
    if (ci == kClients) return false;
    uint64_t request_id = next_id++; if (!request_id) request_id = next_id++;
    Client& c = clients[ci]; c = Client{}; c.used = true; c.queued = true;
    c.id = request_id; c.deadline = deadline; c.days = days; c.callback = callback; c.context = context;
    c.background = background;
    memcpy(c.slug, slug, sizeof(slug)); *id = request_id;
    changed.Signal(); return true;
  }
  bool Cancel(uint64_t id) {
    fortis::base::LockGuard lock(mutex);
    for (Client& c : clients) if (c.used && c.id == id) {
      c.cancelled = true; changed.Signal(); return true;
    }
    return false;
  }
  bool Build(const Flight& f, char path[256], http::OutboundRequest* out) {
    const bool pro = config.coingecko_tier == config::CoinGeckoTier::kPro && config.coingecko_api_key_configured;
    const int n = snprintf(path, 256, "/api/v3/coins/%s/ohlc?vs_currency=usd&days=%d", f.slug, f.days);
    if (n <= 0 || n >= 256) return false;
    http::OutboundRequest request{}; request.method = http::OutboundMethod::kGet;
    request.host = pro ? kProHost : kFreeHost; request.port = kPort; request.path = path;
    request.ca_file = config.ca_bundle_path; request.timeout_ms = kTimeoutMs;
    request.max_response_bytes = providers::kProviderExecutionDefaultResponseBody;
    if (config.coingecko_api_key_configured) {
      const size_t len = strnlen(config.coingecko_api_key, sizeof(config.coingecko_api_key));
      if (!len || len >= sizeof(config.coingecko_api_key)) return false;
      request.header_count = 1;
      snprintf(request.headers[0].name, sizeof(request.headers[0].name), "%s",
               pro ? "x-cg-pro-api-key" : "x-cg-demo-api-key");
      memcpy(request.headers[0].value, config.coingecko_api_key, len + 1);
    }
    redirect.Apply(request);
    if (!OutboundHttpsHeadersValid(request)) return false;
    *out = request; return true;
  }
  void Deliver(size_t ci, OhlcProviderResult result, const unsigned char* body = nullptr, size_t len = 0) {
    Client c{}; fortis::base::UniqueLock lock(mutex);
    if (ci >= kClients || !clients[ci].used) return;
    c = clients[ci]; clients[ci] = Client{}; lock.Unlock();
    fortis::base::FatalBoundary("ohlc completion", [&] { c.callback(c.context, result, body, len); });
  }
  void Finish(size_t fi, OhlcProviderResult result, const unsigned char* body = nullptr, size_t len = 0) {
    Flight& f = flights[fi];
    const size_t n = f.waiter_count; uint8_t waiters[kClients]{};
    memcpy(waiters, f.waiters, n); f = Flight{};
    for (size_t i = 0; i < n; ++i) Deliver(waiters[i], result, body, len);
  }
  void Admit() {
    for (size_t ci = 0; ci < kClients; ++ci) {
      Client c{}; fortis::base::UniqueLock lock(mutex);
      if (!clients[ci].used || !clients[ci].queued) continue;
      c = clients[ci]; lock.Unlock();
      size_t fi = kFlights;
      for (size_t i = 0; i < kFlights; ++i)
        if (flights[i].used && !flights[i].orphaned && flights[i].days == c.days &&
            strcmp(flights[i].slug, c.slug) == 0) { fi = i; break; }
      if (fi == kFlights) for (size_t i = 0; i < kFlights; ++i)
        if (!flights[i].used) { fi = i; break; }
      if (fi == kFlights) { OhlcProviderResult r{}; r.transport_error = true; Deliver(ci, r); continue; }
      Flight& f = flights[fi];
      if (!f.used) { f = Flight{}; f.used = true; f.days = c.days;
        f.background = c.background; memcpy(f.slug, c.slug, sizeof(f.slug)); }
      else f.background = f.background && c.background;
#ifdef FORTIS_HISTORY_BACKFILL_TEST_HOOKS
      if (test_admission_hook) test_admission_hook(test_admission_context, c.id);
#endif
      lock.Lock();
      if (!clients[ci].used || clients[ci].id != c.id) {
        lock.Unlock();
        if (!f.active && !f.waiter_count) f = Flight{};
        continue;
      }
      if (clients[ci].cancelled || now_ms(nullptr) >= clients[ci].deadline) {
        const bool expired = !clients[ci].cancelled;
        lock.Unlock();
        if (!f.active && !f.waiter_count) f = Flight{};
        OhlcProviderResult r{}; r.cancelled = true; r.deadline_exceeded = expired;
        Deliver(ci, r); continue;
      }
      clients[ci].queued = false; clients[ci].flight = fi;
      lock.Unlock();
      if (f.waiter_count < kClients) f.waiters[f.waiter_count++] = static_cast<uint8_t>(ci);
    }
  }
  void CancelWaiters() {
    for (size_t ci = 0; ci < kClients; ++ci) {
      fortis::base::UniqueLock lock(mutex); const bool cancel = clients[ci].used &&
          (clients[ci].cancelled || stopping || now_ms(nullptr) >= clients[ci].deadline);
      const bool expired = cancel && !clients[ci].cancelled && !stopping;
      const size_t fi = clients[ci].flight; lock.Unlock();
      if (!cancel) continue;
      if (fi < kFlights) {
        Flight& f = flights[fi];
        for (size_t i = 0; i < f.waiter_count; ++i) if (f.waiters[i] == ci) {
          for (size_t j = i + 1; j < f.waiter_count; ++j) f.waiters[j - 1] = f.waiters[j];
          --f.waiter_count; break;
        }
        if (!f.waiter_count) {
          if (f.active) { f.orphaned = true; coordinator->Cancel(owner_id, f.job); }
          else f = Flight{};
        }
      }
      OhlcProviderResult r{}; r.cancelled = true; r.deadline_exceeded = expired; Deliver(ci, r);
    }
  }
  void Schedule() {
    for (size_t i = 0; i < kFlights; ++i) {
      Flight& f = flights[i]; if (!f.used || f.active || !f.waiter_count) continue;
      if (f.retry_at && now_ms(nullptr) < f.retry_at) continue;
      if (coordinator->live_job_count(owner_id) >= providers::kProviderExecutionMaxJobs) continue;
      char path[256]{}; http::OutboundRequest request{};
      if (!Build(f, path, &request)) { OhlcProviderResult r{}; r.transport_error = true; Finish(i, r); continue; }
      providers::ProviderExecutionOptions options{}; options.max_attempts = providers::kProviderExecutionMaxAttempts;
      options.deadline_ms = now_ms(nullptr) + 30000;
      options.background = f.background;
      const auto sub = coordinator->Submit(owner_id, "coingecko", request, &f, options);
      if (sub.status == providers::ProviderExecutionSubmitStatus::kBusy) continue;
      if (sub.status != providers::ProviderExecutionSubmitStatus::kAccepted) {
        OhlcProviderResult r{}; r.transport_error = true;
        // A lane refusing admission (open breaker, full queue) is back-pressure,
        // the TypeScript LaneBusyError, not a provider verdict.
        r.lane_rejected = sub.status == providers::ProviderExecutionSubmitStatus::kLaneRejected;
        // The lane's remaining cooldown, the LaneBusyError retryAfterMs.
        r.retry_after_ms = static_cast<uint32_t>(sub.retry_after_ms);
        Finish(i, r); continue;
      }
      f.active = true; f.job = sub.job_id;
    }
  }
  void Drain() {
    for (;;) {
      providers::ProviderExecutionResult r{};
      const auto taken = coordinator->TryTakeResult(owner_id, &r, response, sizeof(response));
      if (taken == providers::ProviderExecutionTakeStatus::kNone) return;
      if (taken != providers::ProviderExecutionTakeStatus::kTaken) continue;
      auto* f = static_cast<Flight*>(r.context);
      if (!f || f < &flights[0] || f >= &flights[kFlights] || !f->active || f->job != r.job_id) continue;
      const size_t fi = static_cast<size_t>(f - flights); OhlcProviderResult out{};
      out.http_status = r.response.status; out.attempts = r.attempts;
      out.retry_after_ms = r.retry_after_ms; out.transport_error = r.response.error != http::OutboundError::kNone;
      out.cancelled = r.cancelled || r.deadline_exceeded;
      out.deadline_exceeded = r.deadline_exceeded;
      out.lane_rejected = r.lane_rejected;
      fortis::base::UniqueLock lock(mutex); const OhlcBodyRetryFn check = body_retry;
      lock.Unlock();
      if (!out.transport_error && !out.cancelled && out.http_status >= 200 &&
          out.http_status < 300 && f->body_retries < 2 && check &&
          check(response, r.body_length)) {
        // Unparseable 2xx: the waiters stay attached and the flight is
        // scheduled again (a new lane admission) after the retry delay.
        f->retry_at = now_ms(nullptr) + kBodyRetryDelayMs[f->body_retries++];
        f->active = false; f->job = 0;
        continue;
      }
      Finish(fi, out, response, r.body_length);
    }
  }
  void Loop() {
    for (;;) {
      fortis::base::UniqueLock lock(mutex); const bool stop = stopping; lock.Unlock();
      CancelWaiters();
      // A request can be cancelled while Admit is claiming a new flight.
      // Keep shutdown independent of such an empty, unscheduled slot.
      for (Flight& f : flights) if (f.used && !f.active && !f.waiter_count) f = Flight{};
      if (!stop) Admit();
      Drain();
      if (!stop) { Admit(); Schedule(); }
      bool clients_live = false, flights_live = false;
      lock.Lock(); for (const Client& c : clients) clients_live |= c.used;
      lock.Unlock(); for (const Flight& f : flights) flights_live |= f.used;
      if (stop && !clients_live && !flights_live && !coordinator->live_job_count(owner_id)) break;
      lock.Lock(); const timespec wake = wake_at(kWakeMs);
      changed.WaitUntilLocked(mutex, wake);
    }
    if (!coordinator->UnregisterOwner(owner_id)) abort();
    owner_id = 0;
    exited = true;
  }
};

OhlcProviderOwner::OhlcProviderOwner(const config::ProviderRuntimeConfig& config,
    providers::ProviderExecutionCoordinator* coordinator) : impl_(fortis::base::New<Impl>(config, coordinator)) {}
OhlcProviderOwner::~OhlcProviderOwner() {
  if (impl_ && impl_->started && pthread_equal(pthread_self(), impl_->thread.native())) abort();
  Stop(); impl_.reset();
}
bool OhlcProviderOwner::Start() { return impl_ && impl_->Start(); }
bool OhlcProviderOwner::Submit(const char* slug, int days, uint64_t deadline,
    OhlcProviderCompletionFn completion, void* context, uint64_t* id, bool background) {
  return impl_ && impl_->Submit(slug, days, deadline, completion, context, id, background);
}
bool OhlcProviderOwner::Cancel(uint64_t id) { return impl_ && impl_->Cancel(id); }
void OhlcProviderOwner::SetBodyRetryCheck(OhlcBodyRetryFn check) {
  if (!impl_) return;
  fortis::base::LockGuard lock(impl_->mutex); impl_->body_retry = check;
}
void OhlcProviderOwner::Stop() {
  if (!impl_ || !impl_->started) return;
  fortis::base::UniqueLock lock(impl_->mutex); impl_->stopping = true;
  impl_->changed.Broadcast(); lock.Unlock();
  if (pthread_equal(pthread_self(), impl_->thread.native())) return;
  impl_->thread.Join();
  impl_->started = false;
}
bool OhlcProviderOwner::StartRedirected(const char* host, const char* port, const char* ca,
    bool (*allow)(void*, uint32_t), void* context) {
  if (!impl_ || !impl_->redirect.Set(host, port, ca, allow, context)) return false;
  impl_->config.ca_bundle_configured = true;
  snprintf(impl_->config.ca_bundle_path, sizeof(impl_->config.ca_bundle_path), "%s", ca);
  return impl_->Start();
}
#ifdef FORTIS_HISTORY_BACKFILL_TEST_HOOKS
void OhlcProviderOwner::SetAdmissionHookForTest(
    void (*hook)(void*, uint64_t), void* context) {
  if (!impl_ || impl_->started) return;
  impl_->test_admission_hook = hook;
  impl_->test_admission_context = context;
}
#endif

}  // namespace fortis::prices
