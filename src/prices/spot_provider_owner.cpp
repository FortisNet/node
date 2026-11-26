#include <cstdio>
#include <string_view>
#include "exception_boundary.h"
// The spot provider owner: its thread and loop, the provider-execution
// seam, deadlines, cancellation, shutdown and the public interface. Request
// slots, the shared flights and batches, and the Jupiter and Robinhood groups
// live in the spot_owner_*.cpp units.
#include "spot_provider_owner_internal.h"
#include "ownership.h"
#include "threading.h"
#include "factory.h"

namespace fortis::prices {

namespace {

static_assert(providers::kJupiterPriceMaxMints <= providers::kCoinGeckoSpotMaxBatchCount,
              "shared batch flight storage must hold a full Jupiter batch");

static_assert(kJupiterGroupMaxMints <= providers::kCoinGeckoSpotMaxBatchCount,
              "a committed group must fit one CoinGeckoSpotResult");

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

bool valid_fiat(std::string_view fiat) {
  if (!fiat.data() || fiat.size() < 3 || fiat.size() > 5) return false;
  for (size_t i = 0; i < fiat.size(); ++i)
    if (fiat.data()[i] < 'a' || fiat.data()[i] > 'z') return false;
  return true;
}

}  // namespace

namespace spot_provider_owner_internal {

// A kind's lane is its provider's name (the spot source table in
// spot_resolution.cpp); anything else rides the CoinGecko lane.
const char* lane_for_kind(providers::PriceIdKind kind) {
  const char* provider = SpotResolutionProviderName(kind);
  return provider ? provider : "coingecko";
}

const char* flight_platform(providers::PriceIdKind kind, const char* source_id) {
  if (kind != providers::PriceIdKind::kContract) return "";
  providers::ParsedPriceId parsed{};
  if (!source_id || !ParsePriceId(source_id, strlen(source_id), &parsed)) return "";
  const char* platform = providers::CoinGeckoPlatformForNetwork(parsed.network);
  return platform ? platform : "";
}



}  // namespace spot_provider_owner_internal

SpotRuntime::SpotRuntime(fortis::prices::PriceSpotCache& price_cache, fortis::prices::PriceSeriesStore& price_series,
                              const config::ProviderRuntimeConfig& native_config,
                              providers::ProviderExecutionCoordinator* shared_coordinator, base::Clock& monotonic)
    : config(native_config), cache(price_cache), series(price_series),
      coordinator(shared_coordinator), monotonic_clock(monotonic), lanes(&monotonic) {

  if (!coordinator) {
    auto made = base::MakeOwned<providers::ProviderExecutionAdapter>([&] {
      return providers::ProviderExecutionAdapter::Create(lanes, monotonic_clock, WakeOwner, this);
    });
    if (made) adapter = made.TakeValue();
  }
}

SpotRuntime::~SpotRuntime() {

  adapter.reset();



}

void SpotRuntime::WakeOwner(void* opaque) {

  auto* self = static_cast<SpotRuntime*>(opaque);
  fortis::base::LockGuard lock(self->mutex);
  self->changed.Signal();
}

size_t SpotRuntime::ProviderLiveJobCount() const {

  return coordinator ? coordinator->live_job_count(coordinator_owner_id)
                     : adapter->live_job_count();
}
uint64_t SpotRuntime::ProviderNextWakeupMs() {

  return coordinator ? UINT64_MAX : adapter->NextWakeupMs();
}

bool SpotRuntime::ProviderCancel(uint64_t job_id) {

  return coordinator ? coordinator->Cancel(coordinator_owner_id, job_id)
                     : adapter->Cancel(job_id);
}
providers::ProviderExecutionSubmission SpotRuntime::ProviderSubmit(const char* lane,
                             const http::OutboundRequest& request, void* context,
                             const providers::ProviderExecutionOptions& options) {

  return coordinator
      ? coordinator->Submit(coordinator_owner_id, lane, request, context, options)
      : adapter->Submit(lane, request, context, options);
}
providers::ProviderExecutionTakeStatus SpotRuntime::ProviderTakeResult(providers::ProviderExecutionResult* result) {

  return coordinator
      ? coordinator->TryTakeResult(coordinator_owner_id, result, response_body,
                                   sizeof(response_body))
      : adapter->TryTakeResult(result, response_body, sizeof(response_body));
}

void SpotRuntime::RecordStorageFailures(bool cache_failed, bool series_failed) {

  fortis::base::LockGuard lock(mutex);
  if (cache_failed && storage_stats.cache_write_failures != UINT64_MAX)
    ++storage_stats.cache_write_failures;
  if (series_failed && storage_stats.series_write_failures != UINT64_MAX)
    ++storage_stats.series_write_failures;
}

void* SpotRuntime::OwnerEntry(void* opaque) {

  static_cast<SpotRuntime*>(opaque)->OwnerLoop();
  return nullptr;
}

bool SpotRuntime::Start() {

  static_assert(sizeof(SpotRuntime) <= 6 * 1024 * 1024,
                "the complete provider owner must remain bounded below 6 MiB");
  fortis::base::UniqueLock lock(mutex);
  const bool already_started = owner_started || stop_requested || workers_joined;
  lock.Unlock();
  if (already_started) return false;
  if ((coordinator ? !coordinator->valid()
                   : (!lanes.valid() || !adapter)) ||
      !config.ca_bundle_configured ||
      !config.ca_bundle_path[0] ||
      strnlen(config.ca_bundle_path, sizeof(config.ca_bundle_path)) >=
          sizeof(config.ca_bundle_path)) return false;
  if (coordinator) {
    coordinator_owner_id = coordinator->RegisterOwner(WakeOwner, this);
    if (!coordinator_owner_id) return false;
  }
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) {
    if (coordinator && coordinator_owner_id) {
      coordinator->UnregisterOwner(coordinator_owner_id); coordinator_owner_id = 0;
    }
    return false;
  }
  // Provider parsing and atomic cache/series commits have bounded large frames.
  // GCC size optimization/LTO exceeds the former 256 KiB owner stack on musl.
  constexpr size_t kOwnerStackBytes = 1024 * 1024;
  if (pthread_attr_setstacksize(&attributes, kOwnerStackBytes) != 0) {
    pthread_attr_destroy(&attributes);
    if (coordinator && coordinator_owner_id) {
      coordinator->UnregisterOwner(coordinator_owner_id); coordinator_owner_id = 0;
    }
    return false;
  }
  const int result = (owner_thread.StartEntry<OwnerEntry>(this, &attributes) ? 0 : -1);
  pthread_attr_destroy(&attributes);
  if (result != 0) {
    if (coordinator && coordinator_owner_id) {
      coordinator->UnregisterOwner(coordinator_owner_id); coordinator_owner_id = 0;
    }
    return false;
  }
  lock.Lock();
  owner_started = true;
  return true;
}

void SpotRuntime::DrainResults(bool) {

  for (;;) {
    providers::ProviderExecutionResult result{};
    const auto taken = ProviderTakeResult(&result);
    if (taken == providers::ProviderExecutionTakeStatus::kNone) return;
    if (taken != providers::ProviderExecutionTakeStatus::kTaken) continue;
    batches.CompleteBatch(result, result.body_length);
  }
}

// At its deadline a planned request stops waiting: every id whose flight
// has not answered is recorded as a failed fetch (no cache write), the
// request leaves its flights (a batch nobody else waits for is cancelled),
// and it completes like a provider failure — stale floor or `missing`.
void SpotRequestProcessor::ExpireToFallback(size_t index, uint32_t generation) {
  auto& flights = runtime_.flight_table.flights;
  Slot& slot = slots[index];
  if (slot.generation != generation || slot.callback_sent) return;
  for (size_t i = 0; i < slot.plan.id_count; ++i) {
    if (!slot.flight_registered[i] || slot.flight_indices[i] >= kMaxSharedFlights) continue;
    const SharedFlight& flight = flights[slot.flight_indices[i]];
    for (size_t w = 0; w < flight.waiter_count; ++w) {
      const FlightWaiter& waiter = flight.waiters[w];
      if (waiter.slot_index == index && waiter.generation == generation &&
          waiter.fetch_index == i) {
        RecordSpotResolutionBatchFailure(&slot.accumulator, i, true);
        break;
      }
    }
  }
  runtime_.flight_table.DetachSlotWaiters(index, generation);
  CompleteRequest(index);
}

void SpotRequestProcessor::CancelOrExpire(uint64_t now, bool stopping) {
  auto& mutex = runtime_.mutex;
  for (size_t i = 0; i < kSpotProviderOwnerMaxRequests; ++i) {
    bool abandon = false;
    bool fallback = false;
    uint32_t generation = 0;
    SpotProviderOwnerResultStatus status = SpotProviderOwnerResultStatus::kCancelled;
    fortis::base::UniqueLock lock(mutex);
    Slot& slot = slots[i];
    if (slot.state != SlotState::kFree) {
      const bool expired = now >= slot.deadline_ms;
      if (!stopping && expired && !slot.cancel_requested && !slot.callback_sent &&
          slot.batch_initialized) {
        slot.deadline_fallback = true;
        fallback = true;
      } else if (stopping || expired) {
        slot.cancel_requested = true;
        status = stopping ? SpotProviderOwnerResultStatus::kShutdown
                          : SpotProviderOwnerResultStatus::kTimeout;
      }
      abandon = slot.cancel_requested && !slot.callback_sent;
      generation = slot.generation;
    }
    lock.Unlock();
    if (fallback) {
      ExpireToFallback(i, generation);
    } else if (abandon) {
      runtime_.flight_table.DetachSlotWaiters(i, generation);
      FinishWithoutBody(i, status, SpotResolutionCompletionStatus::kProviderFailed);
    }
  }
}

void SpotRuntime::BeginShutdown() {
  auto& flights = flight_table.flights;
  auto& shared_batches = batches.shared_batches;
  auto& groups = jupiter.groups;
  fortis::base::UniqueLock lock(mutex);
  const bool begin = stop_requested && !shutdown_started;
  lock.Unlock();
  if (!begin) return;
  requests.CancelOrExpire(MonotonicMs(), true);
  // A group with no page in flight would otherwise wait for a scheduler
  // that no longer runs; a group with an active page completes through
  // that page's (cancelled or final) result.
  for (size_t g = 0; g < kMaxJupiterGroups; ++g)
    if (groups[g].used && groups[g].active_batch == kNoBatch) jupiter.FailGroup(g, true);
  for (auto& flight : flights) {
    if (flight.state != FlightState::kWindow) continue;
    const uint32_t generation = flight.generation;
    flight = SharedFlight{};
    flight.generation = generation;
  }
  // Background flights keep their batch alive with no waiter (the
  // revalidation writes the cache); on shutdown nobody reads it, so every
  // in-flight batch is cancelled and completes through its cancelled
  // result instead of holding Stop for the provider's timeout.
  for (const auto& batch : shared_batches)
    if (batch.used && batch.job_id) ProviderCancel(batch.job_id);
  if (!coordinator) adapter->BeginShutdown();
  lock.Lock();
  shutdown_started = true;
  changed.Broadcast();
}

void SpotRuntime::Wait(bool stopping) {
  auto& slots = requests.slots;
  auto& flights = flight_table.flights;
  auto& groups = jupiter.groups;
  const uint64_t now = MonotonicMs();
  uint64_t next = ProviderNextWakeupMs();
  const size_t live_jobs = ProviderLiveJobCount();
  fortis::base::LockGuard lock(mutex);
  for (const Slot& slot : slots)
    if (slot.state != SlotState::kFree && slot.deadline_ms < next)
      next = slot.deadline_ms;
  for (const SharedFlight& flight : flights)
    if (flight.state == FlightState::kWindow && flight.window_deadline_ms < next)
      next = flight.window_deadline_ms;
  for (const JupiterGroup& group : groups)
    if (group.used && group.active_batch == kNoBatch && group.retry_at_ms &&
        group.retry_at_ms < next)
      next = group.retry_at_ms;
  if (stopping && workers_joined && live_jobs == 0) return;
  // A due flight can remain queued while all provider slots are busy or a
  // background batch is in flight. The scheduler has already tried it this
  // iteration, so retrying immediately spins on the guest's only vCPU and
  // starves HTTP workers. Provider completions signal this condition; the
  // bounded fallback also catches a missed signal or a newly free slot.
  const uint64_t wait_ms = next == UINT64_MAX || next <= now ||
      next - now > kWaitSliceMs ? kWaitSliceMs : next - now;
  const timespec wake = realtime_after(wait_ms);
  changed.WaitUntilLocked(mutex, wake);
}

void SpotRuntime::OwnerLoop() {
  auto& slots = requests.slots;
  auto& flights = flight_table.flights;
  auto& groups = jupiter.groups;
  for (;;) {
    fortis::base::UniqueLock lock(mutex);
    const bool stopping = stop_requested;
    lock.Unlock();
    if (stopping) BeginShutdown();
    else {
      for (size_t i = 0; i < kSpotProviderOwnerMaxRequests; ++i)
        if (requests.IsState(i, SlotState::kQueued)) requests.ProcessQueued(i);
    }
    requests.CancelOrExpire(MonotonicMs(), stopping);
    ProviderPoll();
    DrainResults(stopping);
    if (!stopping) {
      batches.ScheduleSharedBatches();
    }
    const size_t live_jobs = ProviderLiveJobCount();
    lock.Lock();
    bool any = false;
    for (const Slot& slot : slots) any |= slot.state != SlotState::kFree;
    bool any_flight = false;
    for (const SharedFlight& flight : flights)
      any_flight |= flight.state != FlightState::kFree;
    for (const JupiterGroup& group : groups) any_flight |= group.used;
    const bool done = stop_requested && shutdown_started && workers_joined &&
                      live_jobs == 0 && !any && !any_flight;
    lock.Unlock();
    if (done) break;
    Wait(stopping);
  }
  if (coordinator) {
    if (!coordinator->UnregisterOwner(coordinator_owner_id)) abort();
    coordinator_owner_id = 0;
  } else {
    lanes.Close();
  }
  fortis::base::LockGuard exit_lock(mutex);
  owner_exited = true;
  changed.Broadcast();
}

SpotProviderOwner::SpotProviderOwner(fortis::prices::PriceSpotCache& cache,
                                     fortis::prices::PriceSeriesStore& series,
                                     const config::ProviderRuntimeConfig& config,
                                     providers::ProviderExecutionCoordinator* coordinator, base::Clock* monotonic)
    : impl_(fortis::base::New<Impl>(cache, series, config, coordinator,
          monotonic ? *monotonic : base::SystemClock())) {}

SpotProviderOwner::SpotProviderOwner(SpotProviderOwner&&) noexcept = default;

base::Result<SpotProviderOwner> SpotProviderOwner::Create(PriceSpotCache& cache, PriceSeriesStore& series,
    const config::ProviderRuntimeConfig& config, const SpotProviderOwnerOptions& options) try {
  if (options.coordinator && options.monotonic_clock && options.monotonic_clock != &base::SystemClock())
    return base::Status(base::StatusCode::kInvalidArgument, "coordinator monotonic clock");
  SpotProviderOwner owner(cache, series, config, options.coordinator, options.monotonic_clock);
  if (!owner.impl_) return base::Status(base::StatusCode::kResourceExhausted, "spot owner allocation");
  owner.SetClock(options.wall_clock);
  owner.SetErrorReporter(options.errors);
  owner.SetCoinPopularity(options.popularity);
  owner.SetContractPopularity(options.contract_popularity);
  owner.SetRobinhoodCatalog(options.robinhood_catalog);
  const auto initialized = owner.Initialize(options.start_owner);
  if (!initialized.ok()) return initialized;
  return owner;
} catch (const std::bad_alloc&) {
  return base::Status(base::StatusCode::kResourceExhausted, "spot owner allocation");
}

base::Status SpotProviderOwner::Initialize(base::Starter<SpotProviderOwnerStartup>* starter) {
  if (!impl_) return base::Status(base::StatusCode::kResourceExhausted, "spot owner allocation");
  SpotProviderOwnerStartup startup(*this);
  if (!(starter ? starter->Start(startup) : startup.Start()))
    return base::Status(base::StatusCode::kUnavailable, "spot owner initialization");
  return base::Status::Ok();
}

SpotProviderOwner::~SpotProviderOwner() {
  Stop();
  if (impl_) {
    if (!impl_->workers_joined && impl_->adapter) {
      impl_->adapter->BeginShutdown();
      impl_->adapter->JoinWorkers();
      impl_->workers_joined = true;
    }
    impl_.reset();
  }
}

bool SpotProviderOwner::Start() {
  return impl_ && impl_->Start();
}

void SpotProviderOwner::SetCoinPopularity(fortis::discovery::CoinPopularity* popularity) {
  if (impl_) impl_->popularity = popularity;
}

void SpotProviderOwner::SetRobinhoodCatalog(const SpotRobinhoodCatalog* catalog) {
  if (!impl_) return;
  fortis::base::LockGuard robinhood_shared_lock(impl_->robinhood.robinhood_shared_mutex);
  impl_->robinhood.robinhood_shared = catalog;
}

void SpotProviderOwner::SetContractPopularity(fortis::discovery::CoinPopularity* popularity) {
  if (impl_) impl_->contract_popularity = popularity;
}

void SpotProviderOwner::RecordDemand(const std::string_view* raw_ids, size_t count,
                                    const char* device_id) {
  if (!impl_) return;
  impl_->requests.RecordDemand(raw_ids, count, device_id);
}

SpotProviderOwnerSubmission SpotProviderOwner::Submit(
    const std::string_view* raw_ids, size_t raw_count, std::string_view fiat,
    SpotProviderOwnerFormat format, uint64_t deadline_monotonic_ms,
    SpotProviderOwnerCompletionFn completion, void* completion_context,
    SpotSubmitMode mode, const char* device_id, int64_t refresh_lead_ms) {
  if (!impl_) return SpotProviderOwnerSubmitStatus::kNotRunning;
  return impl_->requests.Submit(raw_ids, raw_count, fiat, format, deadline_monotonic_ms, completion, completion_context, mode, device_id, refresh_lead_ms);
}

bool SpotProviderOwner::Cancel(uint64_t request_id) {
  if (!impl_) return false;
  return impl_->requests.Cancel(request_id);
}

void SpotProviderOwner::Stop() {
  if (!impl_) return;
  fortis::base::UniqueLock lock(impl_->mutex);
  if (!impl_->owner_started) return;
  impl_->stop_requested = true;
  impl_->changed.Broadcast();
  while (!impl_->shutdown_started && !impl_->owner_exited)
    impl_->changed.WaitLocked(impl_->mutex);
  lock.Unlock();
  if (impl_->adapter) impl_->adapter->JoinWorkers();
  lock.Lock();
  impl_->workers_joined = true;
  impl_->changed.Broadcast();
  lock.Unlock();
  impl_->owner_thread.Join();
  lock.Lock();
  impl_->owner_started = false;
}

size_t SpotProviderOwner::active_request_count() const {
  if (!impl_) return 0;
  fortis::base::LockGuard lock(impl_->mutex);
  size_t count = 0;
  for (const auto& slot : impl_->requests.slots)
    count += slot.state != Impl::SlotState::kFree;
  return count;
}

SpotProviderOwnerStorageStats SpotProviderOwner::storage_stats() const {
  if (!impl_) return {};
  fortis::base::LockGuard lock(impl_->mutex);
  const auto stats = impl_->storage_stats;
  return stats;
}

void SpotProviderOwner::SetErrorReporter(base::ErrorReporter* reporter) {
  if (impl_ && !impl_->owner_started) impl_->errors = reporter;
}

void SpotProviderOwner::SetClock(fortis::base::Clock* clock) {
  if (impl_ && clock) impl_->clock = clock;
}

bool SpotProviderOwner::StartRedirected(const char* host, const char* port,
    const char* ca_file, bool (*allow_address)(void*, uint32_t), void* context,
    size_t serialized_capacity) {
  if (!impl_ || !impl_->redirect.Accepts(host, port, ca_file, allow_address, true) ||
      (serialized_capacity != static_cast<size_t>(-1) &&
       serialized_capacity > sizeof(impl_->requests.serialized))) return false;
  impl_->redirect.Set(host, port, ca_file, allow_address, context, true);
  if (serialized_capacity != static_cast<size_t>(-1))
    impl_->requests.serialized_capacity = serialized_capacity;
  impl_->config.ca_bundle_configured = true;
  snprintf(impl_->config.ca_bundle_path, sizeof(impl_->config.ca_bundle_path),
           "%s", ca_file);
  return impl_->Start();
}

void SpotRequestProcessor::RecordDemand(const std::string_view* raw_ids, size_t count,
                                    const char* device_id) {

  if (count > kSpotResolutionMaxIds) return;
  if (runtime_.popularity) {
    fortis::discovery::CoinIdView views[kSpotResolutionMaxIds];
    for (size_t i = 0; i < count; ++i) views[i] = {raw_ids[i].data(), raw_ids[i].size()};
    runtime_.popularity->RecordPriceIds(views, count, device_id);
  }
  if (!runtime_.contract_popularity) return;
  // Match the common fast-path batch size and keep the foreground tracker
  // work bounded even when a caller submits the full 100-ID query.
  constexpr size_t kTrackedBatch = 32;
  char keys[kTrackedBatch][providers::kPriceIdMaxSourceIdBytes + 1];
  fortis::discovery::CoinIdView tracked[kTrackedBatch];
  size_t used = 0;
  for (size_t i = 0; i < count && used < kTrackedBatch; ++i) {
    providers::ParsedPriceId parsed{};
    if (!raw_ids[i].data() || !ParsePriceId(raw_ids[i].data(), raw_ids[i].size(), &parsed) ||
        (parsed.kind != providers::PriceIdKind::kContract && parsed.kind != providers::PriceIdKind::kAsa &&
         parsed.kind != providers::PriceIdKind::kSpl && parsed.kind != providers::PriceIdKind::kTrc10)) continue;
    if (parsed.kind == providers::PriceIdKind::kContract && !providers::CoinGeckoPlatformForNetwork(parsed.network) &&
        strcmp(parsed.network, "eip155:4663") != 0 && strcmp(parsed.network, "robinhood") != 0)
      continue;
    bool duplicate = false;
    for (size_t j = 0; j < used && !duplicate; ++j)
      duplicate = strcmp(keys[j], parsed.source_id) == 0;
    if (duplicate) continue;
    std::snprintf(keys[used], sizeof(keys[used]), "%s", parsed.source_id);
    tracked[used] = {keys[used], strlen(keys[used])};
    ++used;
  }
  runtime_.contract_popularity->Record(tracked, used, device_id);
}

SpotProviderOwnerSubmission SpotRequestProcessor::Submit(
    const std::string_view* raw_ids, size_t raw_count, std::string_view fiat,
    SpotProviderOwnerFormat format, uint64_t deadline_monotonic_ms,
    SpotProviderOwnerCompletionFn completion, void* completion_context,
    SpotSubmitMode mode, const char* device_id, int64_t refresh_lead_ms) {

  const bool background = mode == SpotSubmitMode::kBackground;
  const uint64_t now = runtime_.MonotonicMs();
  if ((mode != SpotSubmitMode::kForeground && mode != SpotSubmitMode::kBackground) ||
      !raw_ids || raw_count == 0 || raw_count > kSpotResolutionMaxIds ||
      !valid_fiat(fiat) || !completion ||
      (format != SpotProviderOwnerFormat::kJson &&
       format != SpotProviderOwnerFormat::kProtobuf) ||
      deadline_monotonic_ms <= now ||
      deadline_monotonic_ms - now > kSpotProviderOwnerMaxDeadlineMs)
    return SpotProviderOwnerSubmitStatus::kInvalidRequest;
  for (size_t i = 0; i < raw_count; ++i)
    if ((!raw_ids[i].data() && !raw_ids[i].empty()) ||
        raw_ids[i].size() > providers::kPriceIdMaxRawBytes)
      return SpotProviderOwnerSubmitStatus::kInvalidRequest;
  // PricesFacet.spot: client demand feeds the popularity schedule before any
  // cache read; internal refreshes (background) do not count.
  if (!background) RecordDemand(raw_ids, raw_count, device_id);
  fortis::base::LockGuard lock(runtime_.mutex);
  if (!runtime_.owner_started) return SpotProviderOwnerSubmitStatus::kNotRunning;
  if (runtime_.stop_requested) return SpotProviderOwnerSubmitStatus::kStopping;
  // Cache-only replies can request a refresh without owning a response
  // socket. Coalesce identical queued refreshes before taking an owner slot.
  for (const auto& slot : slots) {
    if (!background || slot.state != SlotState::kQueued || !slot.background ||
        slot.refresh_lead_ms != refresh_lead_ms || slot.raw_count != raw_count || strlen(slot.fiat) != fiat.size() ||
        memcmp(slot.fiat, fiat.data(), fiat.size()) != 0) continue;
    bool same = true;
    for (size_t i = 0; same && i < raw_count; ++i) {
      same = slot.raw_views[i].size() == raw_ids[i].size() &&
          (raw_ids[i].empty() || memcmp(slot.raw_views[i].data(), raw_ids[i].data(), raw_ids[i].size()) == 0);
    }
    if (same && slot.completion == completion && !completion_context &&
        !slot.completion_context) {
      return SpotProviderOwnerSubmitStatus::kBusy;
    }
  }
  size_t index = kSpotProviderOwnerMaxRequests;
  for (size_t i = 0; i < kSpotProviderOwnerMaxRequests; ++i)
    if (slots[i].state == SlotState::kFree) { index = i; break; }
  if (index == kSpotProviderOwnerMaxRequests) return SpotProviderOwnerSubmitStatus::kBusy;
  auto& slot = slots[index];
  uint32_t generation = slot.generation + 1;
  if (!generation) ++generation;
  ResetSlotFields(slot, generation);
  slot.generation = generation;
  slot.state = SlotState::kQueued;
  slot.request_id = next_request_id++;
  if (!slot.request_id) slot.request_id = next_request_id++;
  slot.deadline_ms = deadline_monotonic_ms;
  slot.format = format;
  slot.completion = completion;
  slot.completion_context = completion_context;
  slot.background = background;
  slot.refresh_lead_ms = refresh_lead_ms;
  slot.raw_count = raw_count;
  memcpy(slot.fiat, fiat.data(), fiat.size());
  slot.fiat[fiat.size()] = '\0';
  for (size_t i = 0; i < raw_count; ++i) {
    if (!raw_ids[i].empty()) memcpy(slot.raw_ids[i], raw_ids[i].data(), raw_ids[i].size());
    slot.raw_ids[i][raw_ids[i].size()] = '\0';
    slot.raw_views[i] = {slot.raw_ids[i], raw_ids[i].size()};
  }
  const SpotSubmitTicket ticket{slot.request_id};
  runtime_.changed.Signal();
  return ticket;
}

bool SpotRequestProcessor::Cancel(uint64_t request_id) {

  if (!request_id) return false;
  fortis::base::LockGuard lock(runtime_.mutex);
  for (auto& slot : slots) {
    if (slot.state != SlotState::kFree && slot.request_id == request_id) {
      if (slot.callback_sent) return false;
      slot.cancel_requested = true;
      runtime_.changed.Signal();
      return true;
    }
  }
  return false;
}

}  // namespace fortis::prices
