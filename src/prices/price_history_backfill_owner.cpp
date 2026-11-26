#include <memory>
#include "exception_boundary.h"
#include "price_history_backfill_owner.h"
#include "ownership.h"

#include "bounded_json.h"
#include "clock.h"
#include "outbound_https.h"
#include "price_history_backfill.h"
#include "price_series_store.h"
#include "provider_execution.h"
#include "provider_execution_coordinator.h"
#include "provider_lane.h"
#include "threading.h"
#include "factory.h"
#include "upstream_redirect.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::prices {

namespace {
constexpr char kFreeHost[] = "api.coingecko.com";
constexpr char kProHost[] = "pro-api.coingecko.com";
constexpr char kPort[] = "443";
constexpr uint32_t kRequestTimeoutMs = 10000;
constexpr uint64_t kWaitSliceMs = 50;

uint64_t monotonic_ms() {
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return static_cast<uint64_t>(now.tv_sec) * 1000ULL +
         static_cast<uint64_t>(now.tv_nsec / 1000000L);
}

// The candle and coverage wall clock (see SetPriceHistoryCoverageClock).
fortis::base::Clock* g_coverage_clock = &fortis::base::SystemClock();

int64_t wall_ms() { return g_coverage_clock->NowMs(); }

timespec realtime_after(uint64_t ms) {
  timespec out{}; clock_gettime(CLOCK_REALTIME, &out);
  out.tv_sec += static_cast<time_t>(ms / 1000);
  out.tv_nsec += static_cast<long>((ms % 1000) * 1000000);
  if (out.tv_nsec >= 1000000000L) { ++out.tv_sec; out.tv_nsec -= 1000000000L; }
  return out;
}

bool valid_slug(const char* value, char output[101]) {
  if (!value) return false;
  const size_t n = strnlen(value, 101);
  if (!n || n >= 101) return false;
  for (size_t i = 0; i < n; ++i) {
    unsigned char c = static_cast<unsigned char>(value[i]);
    if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + 32);
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          (c == '-' && i))) return false;
    output[i] = static_cast<char>(c);
  }
  output[n] = 0;
  return true;
}

bool valid_source(const char* value, char output[513]) {
  if (!value) return false;
  const size_t n = strnlen(value, 513);
  if (!n || n > 512 || value[0] == ' ' || value[n - 1] == ' ') return false;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (c < 0x21 || c > 0x7e || c == '/' || c == '\\') return false;
  }
  memcpy(output, value, n + 1); return true;
}

bool valid_fiat(const char* value, char output[6]) {
  if (!value) return false;
  const size_t n = strnlen(value, 6);
  if (n < 3 || n > 5) return false;
  for (size_t i = 0; i < n; ++i)
    if (value[i] < 'a' || value[i] > 'z') return false;
  memcpy(output, value, n + 1); return true;
}

// The resolutions CandleHistory keys coverage by.
constexpr fortis::prices::PriceSeriesInterval kCoverageIntervals[] = {
    fortis::prices::PriceSeriesInterval::k1h, fortis::prices::PriceSeriesInterval::k1d};

uint8_t interval_bit(fortis::prices::PriceSeriesInterval interval) {
  return static_cast<uint8_t>(1u << static_cast<unsigned>(interval));
}

int64_t interval_seconds(fortis::prices::PriceSeriesInterval interval) {
  return interval == fortis::prices::PriceSeriesInterval::k1h ? 3600 : 86400;
}
}  // namespace

int64_t PriceHistoryCoverageNowMs() { return wall_ms(); }

void SetPriceHistoryCoverageClock(fortis::base::Clock* clock) {
  g_coverage_clock = clock ? clock : &fortis::base::SystemClock();
}

int64_t PriceHistoryOwnerMaxWindowSeconds(fortis::prices::PriceSeriesInterval interval) {
  return interval == fortis::prices::PriceSeriesInterval::k1d
      ? kPriceHistoryOwnerMaxDailyWindowSeconds
      : kPriceHistoryOwnerMaxHourlyWindowSeconds;
}

struct PriceHistoryBackfillOwner::Impl {
  enum class FlightState : uint8_t { kFree, kQueued, kActive, kApplying };
  struct Client {
    bool used = false;
    bool cancel = false;
    bool queued = false;
    bool completion_committed = false;
    bool joined = false;  // attached to a flight another caller started
    uint64_t id = 0;
    uint64_t deadline = 0;
    size_t flight = 0;
    int64_t from_sec = 0, to_sec = 0;
    fortis::prices::PriceSeriesInterval interval = fortis::prices::PriceSeriesInterval::k1h;
    char coin_id[101]{};
    char source_id[513]{};
    char fiat[6]{};
    PriceHistoryOwnerCompletionFn callback = nullptr;
    void* context = nullptr;
  };
  struct Flight {
    FlightState state = FlightState::kFree;
    // Keep a cancelled active job's slot until its adapter result is drained,
    // but never attach a new caller to that already-cancelled job.
    bool orphaned = false;
    char coin_id[101]{};
    char source_id[513]{};
    char fiat[6]{};
    int64_t from_sec = 0, to_sec = 0;
    uint64_t job_id = 0;
    size_t waiter_count = 0;
    uint8_t waiters[kPriceHistoryOwnerMaxRequests]{};
  };
  struct Completion {
    PriceHistoryOwnerCompletionFn callback = nullptr;
    void* context = nullptr;
    PriceHistoryOwnerResult result{};
  };

  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable changed;
  fortis::base::Thread thread;
  bool started = false, stopping = false, shutdown_started = false;
  bool owner_exited = false, workers_joined = false;
  uint64_t next_id = 1;
  config::ProviderRuntimeConfig config{};
  fortis::prices::PriceSeriesStore& series;
  providers::ProviderExecutionCoordinator* coordinator = nullptr;
  uint64_t coordinator_owner_id = 0;
  providers::ProviderLaneRegistry lanes;
  std::unique_ptr<providers::ProviderExecutionAdapter, fortis::base::ObjectDeleter<providers::ProviderExecutionAdapter>> adapter;
  Client clients[kPriceHistoryOwnerMaxRequests]{};
  Flight flights[kPriceHistoryOwnerMaxFlights]{};
  // Heap (Impl is allocated): the only provider buffer above the 256 KiB default.
  unsigned char response[kPriceHistoryMaxProviderBodyBytes]{};
  static_assert(kPriceHistoryMaxProviderBodyBytes <= providers::kProviderExecutionMaxResponseBody,
                "history provider body must fit the adapter's response storage");
  std::unique_ptr<PriceHistoryWorkspace, base::StorageDeleter> workspace;
  // Off in production (see upstream_redirect.h).
  http::UpstreamRedirect redirect;

  Impl(fortis::prices::PriceSeriesStore& store, const config::ProviderRuntimeConfig& native,
       providers::ProviderExecutionCoordinator* shared_coordinator)
      : config(native), series(store), coordinator(shared_coordinator),
        lanes(&fortis::base::SystemClock()) {
    if (!coordinator) {
      auto made = base::MakeOwned<providers::ProviderExecutionAdapter>([&] {
      return providers::ProviderExecutionAdapter::Create(lanes, base::SystemClock(), Wake, this);
    });
    if (made) adapter = made.TakeValue();
    }
  }
  ~Impl() {
    adapter.reset();
    workspace.reset();


  }
  static void Wake(void* opaque) {
    auto* self = static_cast<Impl*>(opaque);
    fortis::base::LockGuard lock(self->mutex); self->changed.Signal();
  }
  bool IsStopping() {
    fortis::base::LockGuard lock(mutex); const bool value = stopping;
    return value;
  }
  static void* Entry(void* opaque) { static_cast<Impl*>(opaque)->Loop(); return nullptr; }

  size_t ProviderLiveJobCount() const {
    return coordinator ? coordinator->live_job_count(coordinator_owner_id)
                       : adapter->live_job_count();
  }
  uint64_t ProviderNextWakeupMs() {
    return coordinator ? UINT64_MAX : adapter->NextWakeupMs();
  }
  void ProviderPoll() { if (!coordinator) adapter->Poll(); }
  bool ProviderCancel(uint64_t job_id) {
    return coordinator ? coordinator->Cancel(coordinator_owner_id, job_id)
                       : adapter->Cancel(job_id);
  }
  providers::ProviderExecutionSubmission ProviderSubmit(const http::OutboundRequest& request,
      Flight* flight, const providers::ProviderExecutionOptions& options) {
    return coordinator
        ? coordinator->Submit(coordinator_owner_id, "coingecko", request, flight, options)
        : adapter->Submit("coingecko", request, flight, options);
  }
  providers::ProviderExecutionTakeStatus ProviderTakeResult(providers::ProviderExecutionResult* result) {
    return coordinator
        ? coordinator->TryTakeResult(coordinator_owner_id, result, response, sizeof(response))
        : adapter->TryTakeResult(result, response, sizeof(response));
  }

  bool Start() {
    if (started || stopping || workers_joined ||
        (coordinator ? !coordinator->valid()
                     : (!lanes.valid() || !adapter))) return false;
    if (!config.ca_bundle_configured || !config.ca_bundle_path[0] ||
        strnlen(config.ca_bundle_path, sizeof(config.ca_bundle_path)) >= sizeof(config.ca_bundle_path))
      return false;
    workspace = base::AllocateZeroedStorage<PriceHistoryWorkspace>(1, sizeof(PriceHistoryWorkspace));
    if (!workspace) return false;
    if (coordinator) {
      coordinator_owner_id = coordinator->RegisterOwner(Wake, this);
      if (!coordinator_owner_id) { workspace.reset(); return false; }
    }
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
      if (coordinator_owner_id) {
        coordinator->UnregisterOwner(coordinator_owner_id); coordinator_owner_id = 0;
      }
      workspace.reset(); return false;
    }
    // The large history workspace is heap allocated, never part of this stack.
    if (pthread_attr_setstacksize(&attr, 256 * 1024) != 0) {
      pthread_attr_destroy(&attr);
      if (coordinator_owner_id) {
        coordinator->UnregisterOwner(coordinator_owner_id); coordinator_owner_id = 0;
      }
      workspace.reset(); return false;
    }
    const int rc = (thread.StartEntry<Entry>(this, &attr) ? 0 : -1);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
      if (coordinator_owner_id) {
        coordinator->UnregisterOwner(coordinator_owner_id); coordinator_owner_id = 0;
      }
      workspace.reset(); return false;
    }
    started = true; return true;
  }

  bool StartRedirected(const char* host, const char* port, const char* ca,
      bool (*allow)(void*, uint32_t), void* context) {
    if (started || stopping || !redirect.Accepts(host, port, ca, allow, true)) return false;
    redirect.Set(host, port, ca, allow, context, true);
    config::ProviderRuntimeConfig saved = config;
    config.ca_bundle_configured = true;
    snprintf(config.ca_bundle_path, sizeof(config.ca_bundle_path), "%s", ca);
    const bool ok = Start();
    if (!ok) config = saved;
    return ok;
  }

  bool BuildRequest(const Flight& flight, char path[2049], http::OutboundRequest* request) {
    const bool pro = config.coingecko_tier == config::CoinGeckoTier::kPro &&
                     config.coingecko_api_key_configured;
    const int n = snprintf(path, 2049,
        "/api/v3/coins/%s/market_chart/range?vs_currency=%s&from=%lld&to=%lld",
        flight.coin_id, flight.fiat, static_cast<long long>(flight.from_sec),
        static_cast<long long>(flight.to_sec));
    if (n <= 0 || n >= 2049 || !config.ca_bundle_configured ||
        !config.ca_bundle_path[0]) return false;
    http::OutboundRequest out{};
    out.method = http::OutboundMethod::kGet;
    out.host = pro ? kProHost : kFreeHost;
    out.port = kPort; out.path = path; out.ca_file = config.ca_bundle_path;
    out.timeout_ms = kRequestTimeoutMs;
    // A larger body ends as OutboundError::kTooLarge (not retried):
    // kUpstreamFailure, no candles and no coverage.
    out.max_response_bytes = kPriceHistoryMaxProviderBodyBytes;
    // HttpClient.json sends `accept: application/json` on every CoinGecko
    // request, marketChartRange included.
    out.header_count = 1;
    snprintf(out.headers[0].name, sizeof(out.headers[0].name), "accept");
    snprintf(out.headers[0].value, sizeof(out.headers[0].value), "application/json");
    if (config.coingecko_api_key_configured) {
      const size_t key_length = strnlen(config.coingecko_api_key, sizeof(config.coingecko_api_key));
      if (!key_length || key_length >= sizeof(config.coingecko_api_key)) return false;
      out.header_count = 2;
      snprintf(out.headers[1].name, sizeof(out.headers[1].name), "%s",
               pro ? "x-cg-pro-api-key" : "x-cg-demo-api-key");
      memcpy(out.headers[1].value, config.coingecko_api_key, key_length + 1);
    }
    redirect.Apply(out);
    if (!OutboundHttpsHeadersValid(out)) return false;
    *request = out; return true;
  }

  PriceHistoryOwnerSubmitStatus Submit(const char* coin_id, const char* source_id,
      const char* fiat, fortis::prices::PriceSeriesInterval interval, int64_t from,
      int64_t to, uint64_t deadline,
      PriceHistoryOwnerCompletionFn callback, void* context, uint64_t* request_id) {
    char coin[101], source[513], currency[6];
    const uint64_t now = monotonic_ms();
    if (!valid_slug(coin_id, coin) || !valid_source(source_id, source) ||
        !valid_fiat(fiat, currency) ||
        (interval != fortis::prices::PriceSeriesInterval::k1h &&
         interval != fortis::prices::PriceSeriesInterval::k1d) || from < 0 || to <= from ||
        to - from > PriceHistoryOwnerMaxWindowSeconds(interval) ||
        from > INT64_MAX / 1000 || to > INT64_MAX / 1000 || !callback || !request_id ||
        deadline <= now || deadline - now > kPriceHistoryOwnerMaxDeadlineMs)
      return PriceHistoryOwnerSubmitStatus::kInvalidRequest;
    fortis::base::LockGuard lock(mutex);
    if (!started || stopping) {
      const auto status = stopping ? PriceHistoryOwnerSubmitStatus::kStopping
                                   : PriceHistoryOwnerSubmitStatus::kNotRunning;
      return status;
    }
    size_t client_index = kPriceHistoryOwnerMaxRequests;
    for (size_t i = 0; i < kPriceHistoryOwnerMaxRequests; ++i)
      if (!clients[i].used) { client_index = i; break; }
    if (client_index == kPriceHistoryOwnerMaxRequests) return PriceHistoryOwnerSubmitStatus::kBusy;
    uint64_t id = next_id++;
    if (!id) id = next_id++;
    Client& client = clients[client_index]; client = Client{};
    client.used = true; client.queued = true; client.id = id; client.deadline = deadline;
    client.from_sec = from; client.to_sec = to; client.interval = interval;
    memcpy(client.coin_id, coin, sizeof(coin)); memcpy(client.source_id, source, sizeof(source));
    memcpy(client.fiat, currency, sizeof(currency));
    client.callback = callback; client.context = context;
    *request_id = id;
    changed.Signal();
    return PriceHistoryOwnerSubmitStatus::kAccepted;
  }

  bool Cancel(uint64_t id) {
    fortis::base::LockGuard lock(mutex);
    for (Client& client : clients) if (client.used && client.id == id) {
      if (client.completion_committed) return false;
      client.cancel = true; changed.Signal();
      return true;
    }
    return false;
  }

  void RemoveWaiter(Flight& flight, size_t client_index) {
    for (size_t i = 0; i < flight.waiter_count; ++i) {
      if (flight.waiters[i] != client_index) continue;
      for (size_t j = i + 1; j < flight.waiter_count; ++j)
        flight.waiters[j - 1] = flight.waiters[j];
      --flight.waiter_count; return;
    }
  }

  void Deliver(size_t client_index, PriceHistoryOwnerResultStatus status,
      int http_status = 0, uint32_t attempts = 0, uint32_t retry_after = 0,
      size_t rows = 0, size_t chunks = 0, bool coverage_committed = false) {
    Client client{};
    fortis::base::UniqueLock lock(mutex);
    if (client_index >= kPriceHistoryOwnerMaxRequests || !clients[client_index].used) return;
    client = clients[client_index]; clients[client_index] = Client{};
    lock.Unlock();
    PriceHistoryOwnerResult result{};
    result.request_id = client.id; result.status = status;
    result.http_status = http_status; result.attempts = attempts;
    result.retry_after_ms = retry_after; result.candle_rows = rows;
    result.committed_chunks = chunks;
    result.coverage_committed = coverage_committed;
    result.joined = client.joined;
    fortis::base::FatalBoundary("history completion", [&] { client.callback(client.context, result); });
  }

  void FinishFlight(size_t flight_index, PriceHistoryOwnerResultStatus status,
      int http_status = 0, uint32_t attempts = 0, uint32_t retry_after = 0,
      size_t rows = 0, size_t chunks = 0, bool coverage_committed = false) {
    Flight& f = flights[flight_index];
    const size_t count = f.waiter_count;
    uint8_t waiters[kPriceHistoryOwnerMaxRequests]{};
    memcpy(waiters, f.waiters, count);
    f = Flight{};
    for (size_t i = 0; i < count; ++i)
      Deliver(waiters[i], status, http_status, attempts, retry_after, rows, chunks,
              coverage_committed);
  }

  // Intervals of the flight's current waiters; with committed_only, only
  // those whose completion is already reserved. Takes the owner mutex.
  uint8_t WaiterIntervals(const Flight& flight, bool committed_only) {
    uint8_t mask = 0;
    fortis::base::LockGuard lock(mutex);
    for (size_t i = 0; i < flight.waiter_count; ++i) {
      const Client& client = clients[flight.waiters[i]];
      if (client.used && (!committed_only || client.completion_committed))
        mask = static_cast<uint8_t>(mask | interval_bit(client.interval));
    }
    return mask;
  }

  // True only when every waiter interval's record covers the whole buckets
  // of the window. A missing, expired or corrupt record is uncovered.
  bool CoveredForWaiters(const Flight& flight) {
    const uint8_t mask = WaiterIntervals(flight, false);
    if (!mask) return false;
    const int64_t now = wall_ms();
    for (const auto interval : kCoverageIntervals) {
      if (!(mask & interval_bit(interval))) continue;
      int64_t from = 0, to = 0;
      fortis::prices::HistoryCoverage coverage{};
      if (!fortis::prices::AlignHistoryCoverageInward(flight.from_sec, flight.to_sec,
              interval_seconds(interval), &from, &to) ||
          series.ReadHistoryCoverage(interval, flight.fiat, flight.source_id, now,
              &coverage) != fortis::prices::PriceSeriesReadStatus::kFound ||
          !fortis::prices::HistoryCoverageContains(coverage, from, to))
        return false;
    }
    return true;
  }

  void ProcessClientCancellations(uint64_t now, bool stopping_now) {
    for (size_t i = 0; i < kPriceHistoryOwnerMaxRequests; ++i) {
      PriceHistoryOwnerResultStatus status{}; bool deliver = false;
      size_t fi = kPriceHistoryOwnerMaxFlights;
      fortis::base::UniqueLock lock(mutex);
      Client& client = clients[i];
      if (client.used && !client.completion_committed &&
          (stopping_now || client.cancel || now >= client.deadline)) {
        deliver = true;
        if (!client.queued) fi = client.flight;
        status = stopping_now ? PriceHistoryOwnerResultStatus::kShutdown
            : client.cancel ? PriceHistoryOwnerResultStatus::kCancelled
                            : PriceHistoryOwnerResultStatus::kTimeout;
      }
      lock.Unlock();
      if (!deliver) continue;
      if (fi < kPriceHistoryOwnerMaxFlights) {
        Flight& flight = flights[fi]; RemoveWaiter(flight, i);
        if (flight.waiter_count == 0) {
          if (flight.state == FlightState::kActive && flight.job_id) {
            flight.orphaned = true;
            ProviderCancel(flight.job_id);
          } else if (flight.state != FlightState::kActive) flight = Flight{};
        }
      }
      Deliver(i, status);
    }
  }

  void AdmitQueuedClients() {
    for (size_t ci = 0; ci < kPriceHistoryOwnerMaxRequests; ++ci) {
      Client request{};
      fortis::base::UniqueLock lock(mutex);
      if (!clients[ci].used || !clients[ci].queued) continue;
      request = clients[ci];
      lock.Unlock();
      if (request.cancel || monotonic_ms() >= request.deadline) continue;
      size_t fi = kPriceHistoryOwnerMaxFlights;
      for (size_t i = 0; i < kPriceHistoryOwnerMaxFlights; ++i) {
        const Flight& f = flights[i];
        if (f.state != FlightState::kFree && !f.orphaned &&
            f.from_sec == request.from_sec &&
            f.to_sec == request.to_sec && strcmp(f.coin_id, request.coin_id) == 0 &&
            strcmp(f.source_id, request.source_id) == 0 &&
            strcmp(f.fiat, request.fiat) == 0) { fi = i; break; }
      }
      if (fi == kPriceHistoryOwnerMaxFlights) {
        for (size_t i = 0; i < kPriceHistoryOwnerMaxFlights; ++i)
          if (flights[i].state == FlightState::kFree) { fi = i; break; }
        if (fi < kPriceHistoryOwnerMaxFlights) {
          Flight& f = flights[fi]; f = Flight{}; f.state = FlightState::kQueued;
          memcpy(f.coin_id, request.coin_id, sizeof(f.coin_id));
          memcpy(f.source_id, request.source_id, sizeof(f.source_id));
          memcpy(f.fiat, request.fiat, sizeof(f.fiat));
          f.from_sec = request.from_sec; f.to_sec = request.to_sec;
        }
      }
      if (fi == kPriceHistoryOwnerMaxFlights) {
        Deliver(ci, PriceHistoryOwnerResultStatus::kUpstreamFailure); continue;
      }
      Flight& flight = flights[fi];
      if (flight.waiter_count == kPriceHistoryOwnerMaxRequests) {
        Deliver(ci, PriceHistoryOwnerResultStatus::kUpstreamFailure); continue;
      }
      lock.Lock();
      Client& client = clients[ci];
      if (!client.used || client.id != request.id) {
        lock.Unlock();
        if (flight.state == FlightState::kQueued && !flight.waiter_count)
          flight = Flight{};
        continue;
      }
      if (client.cancel || monotonic_ms() >= client.deadline) {
        lock.Unlock();
        if (flight.state == FlightState::kQueued && !flight.waiter_count)
          flight = Flight{};
        continue;
      }
      // Flights belong to the owner thread; another live waiter means this
      // caller did not start the fetch.
      client.queued = false; client.flight = fi; client.joined = flight.waiter_count > 0;
      lock.Unlock();
      flight.waiters[flight.waiter_count++] = static_cast<uint8_t>(ci);
    }
  }

  uint64_t EffectiveDeadline(const Flight& flight) {
    uint64_t deadline = 0;
    fortis::base::LockGuard lock(mutex);
    for (size_t i = 0; i < flight.waiter_count; ++i) {
      const Client& client = clients[flight.waiters[i]];
      if (client.used && client.deadline > deadline) deadline = client.deadline;
    }
    return deadline;
  }

#ifdef FORTIS_HISTORY_BACKFILL_TEST_HOOKS
  void TestPauseBeforeCoverage() {
    const char* marker = getenv("FORTIS_HISTORY_TEST_COMMIT_MARKER");
    if (marker && *marker && strnlen(marker, 1024) < 1024) {
      FILE* file = fopen(marker, "wb"); if (file) fclose(file);
    }
    const char* value = getenv("FORTIS_HISTORY_TEST_COMMIT_PAUSE_MS");
    if (!value || !*value) return;
    char* end = nullptr; const unsigned long delay = strtoul(value, &end, 10);
    if (!end || *end || delay > 5000) return;
    timespec left{static_cast<time_t>(delay / 1000),
                  static_cast<long>((delay % 1000) * 1000000UL)};
    while (nanosleep(&left, &left) != 0) {}
  }
#endif

  void Schedule() {
    for (size_t i = 0; i < kPriceHistoryOwnerMaxFlights; ++i) {
      Flight& f = flights[i];
      if (f.state != FlightState::kQueued || !f.waiter_count) continue;
      if (CoveredForWaiters(f)) {
        FinishFlight(i, PriceHistoryOwnerResultStatus::kCachedComplete);
        continue;
      }
      // The adapter is smaller than the owner queue. Leave accepted work in
      // the owner queue rather than translating temporary executor pressure
      // into an upstream failure.
      if (ProviderLiveJobCount() >= providers::kProviderExecutionMaxJobs) continue;
      char path[2049]{}; http::OutboundRequest request{};
      if (!BuildRequest(f, path, &request)) {
        FinishFlight(i, PriceHistoryOwnerResultStatus::kInvalidResponse);
        continue;
      }
      providers::ProviderExecutionOptions options{};
      options.max_attempts = providers::kProviderExecutionMaxAttempts;
      options.deadline_ms = EffectiveDeadline(f);
      const uint64_t adapter_deadline = monotonic_ms() + 60000;
      // Per-caller deadlines are enforced by the owner. Keep the transport
      // lease long enough for a later single-flight waiter to extend demand;
      // if no waiter remains the owner cancels the adapter job immediately.
      if (adapter_deadline > options.deadline_ms) options.deadline_ms = adapter_deadline;
      if (!options.deadline_ms) {
        FinishFlight(i, PriceHistoryOwnerResultStatus::kTimeout); continue;
      }
      const auto submitted = ProviderSubmit(request, &f, options);
      if (submitted.status != providers::ProviderExecutionSubmitStatus::kAccepted) {
        if (submitted.status == providers::ProviderExecutionSubmitStatus::kBusy)
          continue; // Shared coordinator capacity is temporary; retain queue state.
        FinishFlight(i, submitted.status == providers::ProviderExecutionSubmitStatus::kDeadline
            ? PriceHistoryOwnerResultStatus::kTimeout
            : PriceHistoryOwnerResultStatus::kUpstreamFailure);
        continue;
      }
      f.state = FlightState::kActive; f.job_id = submitted.job_id;
    }
  }

  void ApplyResponse(const providers::ProviderExecutionResult& response_result) {
    auto* f = static_cast<Flight*>(response_result.context);
    if (!f || f < &flights[0] || f >= &flights[kPriceHistoryOwnerMaxFlights] ||
        f->state != FlightState::kActive || f->job_id != response_result.job_id)
      return;
    f->state = FlightState::kApplying;
    const int status = response_result.response.status;
    const uint32_t attempts = response_result.attempts;
    const uint32_t retry_after = response_result.retry_after_ms;
    if (response_result.cancelled || response_result.deadline_exceeded) {
      FinishFlight(static_cast<size_t>(f - flights),
          response_result.deadline_exceeded ? PriceHistoryOwnerResultStatus::kTimeout
                                            : PriceHistoryOwnerResultStatus::kCancelled,
          status, attempts, retry_after);
      return;
    }
    if (response_result.response.error != http::OutboundError::kNone || status != 200) {
      FinishFlight(static_cast<size_t>(f - flights), status == 429
          ? PriceHistoryOwnerResultStatus::kRateLimited
          : PriceHistoryOwnerResultStatus::kUpstreamFailure,
          status, attempts, retry_after);
      return;
    }
    if (CandleAggregator::Parse(response, response_result.body_length, workspace.get()) !=
        PriceHistoryParseStatus::kOk) {
      FinishFlight(static_cast<size_t>(f - flights),
          PriceHistoryOwnerResultStatus::kInvalidResponse, status, attempts, retry_after);
      return;
    }
    size_t h = 0, d = 0, rows = 0, chunks = 0;
    fortis::prices::PriceSeriesCandleBatchInput batch[fortis::prices::kPriceSeriesMaxUpsertItems]{};
    while (h < workspace->hourly_count || d < workspace->daily_count) {
      ProcessClientCancellations(monotonic_ms(), false);
      const bool stopping_now = IsStopping();
      if (!f->waiter_count || stopping_now) {
        FinishFlight(static_cast<size_t>(f - flights), stopping_now
            ? PriceHistoryOwnerResultStatus::kShutdown
            : PriceHistoryOwnerResultStatus::kCancelled, status, attempts, retry_after,
            rows, chunks);
        return;
      }
      const size_t count = CandleAggregator::BuildChunk(*workspace, &h, &d, batch);
      if (!count) break;
      const auto stored = series.UpsertCandlesAtomic(f->fiat, f->source_id,
          batch, count, wall_ms());
      if (stored != fortis::prices::PriceSeriesStoreStatus::kStored &&
          stored != fortis::prices::PriceSeriesStoreStatus::kNoItems) {
        FinishFlight(static_cast<size_t>(f - flights),
            PriceHistoryOwnerResultStatus::kStorageFailure, status, attempts,
            retry_after, rows, chunks);
        return;
      }
      rows += count; ++chunks;
    }
    ProcessClientCancellations(monotonic_ms(), false);
    const bool stopping_now = IsStopping();
    if (!f->waiter_count || stopping_now) {
      FinishFlight(static_cast<size_t>(f - flights), stopping_now
          ? PriceHistoryOwnerResultStatus::kShutdown
          : PriceHistoryOwnerResultStatus::kCancelled, status, attempts,
          retry_after, rows, chunks);
      return;
    }
    // Reserve the final completion point before writing durable coverage.
    // Cancellation after this point returns false; before it, all waiters are
    // detached and no coverage is written.
    fortis::base::UniqueLock lock(mutex);
    bool any_live_waiter = false;
    uint8_t mark_intervals = 0;
    const bool stopping_before_commit = stopping;
    if (!stopping_before_commit) {
      for (size_t i = 0; i < f->waiter_count; ++i) {
        Client& client = clients[f->waiters[i]];
        if (client.used && !client.cancel && monotonic_ms() < client.deadline) {
          client.completion_committed = true;
          any_live_waiter = true;
          // CandleHistory marks the coverage key of the caller that fetched;
          // an interval whose callers all left before this point gets none.
          mark_intervals = static_cast<uint8_t>(mark_intervals | interval_bit(client.interval));
        }
      }
    }
    lock.Unlock();
    if (stopping_before_commit) {
      ProcessClientCancellations(monotonic_ms(), true);
      FinishFlight(static_cast<size_t>(f - flights),
          PriceHistoryOwnerResultStatus::kShutdown, status, attempts,
          retry_after, rows, chunks);
      return;
    }
    ProcessClientCancellations(monotonic_ms(), false);
    if (!any_live_waiter || !f->waiter_count) {
      FinishFlight(static_cast<size_t>(f - flights),
          PriceHistoryOwnerResultStatus::kCancelled, status, attempts,
          retry_after, rows, chunks);
      return;
    }
#ifdef FORTIS_HISTORY_BACKFILL_TEST_HOOKS
    TestPauseBeforeCoverage();
#endif
    // Every candle frame is durable before any coverage is written, so a
    // crash between the two only costs a refetch. A failed coverage write
    // leaves the previous record (or none) and is reported, not retried.
    bool coverage_committed = true;
    const int64_t now = wall_ms();
    for (const auto interval : kCoverageIntervals) {
      if (!(mark_intervals & interval_bit(interval))) continue;
      int64_t from = 0, to = 0;
      if (!fortis::prices::AlignHistoryCoverageInward(f->from_sec, f->to_sec,
              interval_seconds(interval), &from, &to))
        continue;  // No whole bucket was fetched; nothing to claim.
      if (series.MarkHistoryCoverageRange(interval, f->fiat, f->source_id, from, to,
              now) != fortis::prices::PriceSeriesStoreStatus::kStored)
        coverage_committed = false;
    }
    FinishFlight(static_cast<size_t>(f - flights), PriceHistoryOwnerResultStatus::kComplete,
        status, attempts, retry_after, rows, chunks, coverage_committed);
  }

  void DrainResults() {
    for (;;) {
      providers::ProviderExecutionResult result{};
      const auto taken = ProviderTakeResult(&result);
      if (taken == providers::ProviderExecutionTakeStatus::kNone) return;
      if (taken == providers::ProviderExecutionTakeStatus::kTaken) ApplyResponse(result);
    }
  }

  void BeginShutdown() {
    if (shutdown_started) return;
    ProcessClientCancellations(monotonic_ms(), true);
    for (Flight& flight : flights) {
      if (flight.state == FlightState::kActive && flight.job_id)
        ProviderCancel(flight.job_id);
      else if (flight.state != FlightState::kFree) flight = Flight{};
    }
    if (!coordinator) adapter->BeginShutdown();
    shutdown_started = true;
  }

  void Loop() {
    for (;;) {
      fortis::base::UniqueLock lock(mutex); const bool stop = stopping; lock.Unlock();
      if (stop && !shutdown_started) BeginShutdown();
      ProcessClientCancellations(monotonic_ms(), stop);
      for (Flight& flight : flights)
        if (flight.state == FlightState::kQueued && !flight.waiter_count)
          flight = Flight{};
      if (!stop) AdmitQueuedClients();
      ProviderPoll(); DrainResults();
      if (!stop) { AdmitQueuedClients(); Schedule(); }
      bool any_clients = false, any_flights = false;
      for (const Flight& f : flights) any_flights |= f.state != FlightState::kFree;
      lock.Lock();
      for (const Client& c : clients) any_clients |= c.used;
      lock.Unlock();
      if (stop && ProviderLiveJobCount() == 0 && !any_clients && !any_flights) break;
      uint64_t wait_ms = kWaitSliceMs;
      const uint64_t next = ProviderNextWakeupMs(), now = monotonic_ms();
      if (next != UINT64_MAX && next > now && next - now < wait_ms) wait_ms = next - now;
      lock.Lock();
      // Shutdown can still wait on an active TLS lease after cancellation.
      // Keep that wait bounded instead of polling the adapter/coordinator in
      // a tight loop; provider completions signal this condition as well.
      const timespec wake = realtime_after(wait_ms);
      changed.WaitUntilLocked(mutex, wake);
    }
    if (coordinator) {
      if (!coordinator->UnregisterOwner(coordinator_owner_id)) abort();
      coordinator_owner_id = 0;
    } else {
      lanes.Close();
    }
    owner_exited = true;
  }
};

PriceHistoryBackfillOwner::PriceHistoryBackfillOwner(
    fortis::prices::PriceSeriesStore& series, const config::ProviderRuntimeConfig& config,
    providers::ProviderExecutionCoordinator* coordinator) : impl_(fortis::base::New<Impl>(series, config, coordinator)) {}
PriceHistoryBackfillOwner::~PriceHistoryBackfillOwner() {
  Stop();
  if (!impl_) return;
  if (!impl_->workers_joined && impl_->adapter) {
    impl_->adapter->BeginShutdown(); impl_->adapter->JoinWorkers(); impl_->workers_joined = true;
  }
  impl_.reset();
}
bool PriceHistoryBackfillOwner::Start() { return impl_ && impl_->Start(); }
PriceHistoryOwnerSubmitStatus PriceHistoryBackfillOwner::Submit(
    const char* coin, const char* source, const char* fiat,
    fortis::prices::PriceSeriesInterval interval, int64_t from, int64_t to,
    uint64_t deadline, PriceHistoryOwnerCompletionFn callback, void* context,
    uint64_t* id) {
  return impl_ ? impl_->Submit(coin, source, fiat, interval, from, to, deadline,
                               callback, context, id)
               : PriceHistoryOwnerSubmitStatus::kBusy;
}
bool PriceHistoryBackfillOwner::Cancel(uint64_t id) { return impl_ && impl_->Cancel(id); }
void PriceHistoryBackfillOwner::Stop() {
  if (!impl_ || !impl_->started || impl_->workers_joined) return;
  fortis::base::UniqueLock lock(impl_->mutex); impl_->stopping = true;
  impl_->changed.Broadcast(); lock.Unlock();
  if (pthread_equal(pthread_self(), impl_->thread.native())) return;
  impl_->thread.Join();
  if (impl_->adapter) impl_->adapter->JoinWorkers();
  impl_->workers_joined = true;
}
bool PriceHistoryBackfillOwner::valid() const {
  return impl_ && (impl_->coordinator ? impl_->coordinator->valid()
                                     : static_cast<bool>(impl_->adapter));
}
bool PriceHistoryBackfillOwner::StartRedirected(const char* host, const char* port,
    const char* ca, bool (*allow)(void*, uint32_t), void* context) {
  return impl_ && impl_->StartRedirected(host, port, ca, allow, context);
}

}  // namespace fortis::prices
