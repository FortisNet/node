#include "exception_boundary.h"
// The address plane's service: its workers, queues, completion paths, the
// background revalidation and ingest threads, the job scheduler and the
// lifecycle. The route handlers live in the address_*_route(s).cpp units.
#include "address_service_internal.h"
#include "ownership.h"
#include "threading.h"

namespace tj = fortis::codec::tj;

namespace fortis::wallet {

namespace {

// Task arenas grow in kArenaBlockBytes blocks up to these caps (the guest's
// buddy allocator cannot be relied on for a large contiguous block once the
// heap is fragmented); the response and overview news buffers are allocated
// once at start.
constexpr size_t kForegroundArenaBytes = 8 * 1024 * 1024;
constexpr size_t kBackgroundArenaBytes = 8 * 1024 * 1024;
constexpr size_t kJobArenaBytes = 16 * 1024 * 1024;
constexpr size_t kResponseBytes = 4 * 1024 * 1024;
constexpr size_t kOverviewNewsArenaBytes = 4 * 1024 * 1024;
constexpr size_t kOverviewNewsOutputBytes = 1024 * 1024;

const char* const kOverviewCacheControl = "private, max-age=60";

constexpr int64_t kPendingPollMs = 5000;
constexpr int64_t kChangeDetectionMs = 60000;
constexpr int64_t kCandleRepairMs = 600000;
constexpr int64_t kRetentionMs = 86400000;

timespec wake_after(uint64_t ms) {
  timespec out{};
  clock_gettime(CLOCK_REALTIME, &out);
  out.tv_sec += static_cast<time_t>(ms / 1000);
  out.tv_nsec += static_cast<long>((ms % 1000) * 1000000);
  if (out.tv_nsec >= 1000000000L) { ++out.tv_sec; out.tv_nsec -= 1000000000L; }
  return out;
}

uint64_t monotonic_ms() { return fortis::chains::ChainMonotonicMs(); }

// The chain transport's slice of the configuration (its CA bundle).
fortis::chains::TransportConfig transport_config(const config::ProviderRuntimeConfig& config) {
  fortis::chains::TransportConfig transport{};
  transport.ca_bundle_configured = config.ca_bundle_configured;
  snprintf(transport.ca_bundle_path, sizeof(transport.ca_bundle_path), "%s", config.ca_bundle_path);
  return transport;
}

}  // namespace

void free_task(AddressTask* task) {
  if (!task) return;
  fortis::base::Delete(task);
}

void release_ingest(IngestJob* job) {


  fortis::base::Delete(job);
}

AddressService::Impl::Impl(const config::ProviderRuntimeConfig& cfg, fortis::storage::KvStore* store_kv,
                           providers::ProviderExecutionCoordinator* coordinator, prices::SpotProviderOwner* spot_owner,
                           prices::PriceHistoryService* history_service, const AddressServiceSeams* test_seams)
    : config(cfg), kv(store_kv), transport(transport_config(cfg), coordinator), series(*store_kv), price_access(spot_owner, series), store(store_kv),
      swr(store_kv, &swr_seams), spot(spot_owner), history(history_service) {
  if (test_seams) seams = *test_seams;
  swr_seams.clock = seams.clock;
  swr_seams.jitter = seams.jitter;
  news_seams.clock = seams.clock;
  news_seams.jitter = seams.jitter;
  attestation_required = cfg.attestation_required;
}
AddressService::Impl::~Impl() {
  facets.reset();
  news_cache.reset();
  news_scratch.reset();
  news_arena.reset();
  news_output.reset();
  for (fortis::base::Buffer& response : responses) response.reset();




}

bool AddressService::Impl::Init() {
  if (!catalog.Init(config)) return false;
  news_scratch = fortis::base::Buffer::Allocate(1024 * 1024);
  news_arena = fortis::base::Buffer::Allocate(kOverviewNewsArenaBytes);
  news_output = fortis::base::Buffer::Allocate(kOverviewNewsOutputBytes);
  for (fortis::base::Buffer& response : responses) {
    response = fortis::base::Buffer::Allocate(kResponseBytes);
    if (!response) return false;
  }
  if (!news_scratch || !news_arena || !news_output) return false;
  news_cache.reset(fortis::base::New<discovery::DiscoveryCache>(kv, news_scratch.data(), 1024 * 1024, &news_seams));
  if (!news_cache) return false;
  deps.config = &config;
  deps.catalog = &catalog;
  deps.transport = &transport;
  deps.store = &store;
  deps.swr = &swr;
  deps.kv = kv;
  deps.series = &price_access;
  deps.spot = spot ? &price_access : nullptr;
  deps.changes = &changes;
  deps.news_feed = &Impl::NewsFeed;
  deps.news_context = this;
  deps.enqueue = &Impl::EnqueueRevalidation;
  deps.enqueue_context = this;
  deps.ingest_async = &Impl::IngestAsync;
  deps.ingest_context = this;
  deps.clock = seams.clock;
  facets.reset(fortis::base::New<fortis::wallet::address::Facets>(deps));
  return facets != nullptr;
}

bool AddressService::Impl::NewsFeed(void* context, int64_t now_ms, tj::Arena& arena, Value** payload) {
  auto* self = static_cast<Impl*>(context);
  fortis::news::NewsStore news{self->kv, &self->series};
  size_t length = 0;
  fortis::base::UniqueLock news_lock(self->news_mutex);
  const bool ok = NewsOverviewFeed(news, self->news_cache.get(), now_ms, self->news_arena.data(), kOverviewNewsArenaBytes,
                                   self->news_output.data(), kOverviewNewsOutputBytes, &length);
  Value* parsed = ok ? tj::Parse(arena, self->news_output.data(), length) : nullptr;
  news_lock.Unlock();
  if (!parsed) return false;
  *payload = parsed;
  return true;
}

// ---- revalidation queue -----------------------------------------------------
bool AddressService::Impl::EnqueueRevalidation(void* context, Revalidation* job) {
  auto* self = static_cast<Impl*>(context);
  fortis::base::LockGuard lock(self->mutex);
  if (self->stopping || self->revalidation_count == kRevalidationSlots) return false;
  self->revalidations[(self->revalidation_head + self->revalidation_count) % kRevalidationSlots] = job;
  ++self->revalidation_count;
  self->changed.Broadcast();
  return true;
}

void* AddressService::Impl::BackgroundMain(void* opaque) {
  auto* self = static_cast<Impl*>(opaque);
  for (;;) {
    fortis::base::UniqueLock lock(self->mutex);
    while (!self->stopping && self->revalidation_count == 0) self->changed.WaitLocked(self->mutex);
    if (self->stopping) {
      while (self->revalidation_count) {
        Revalidation* job = self->revalidations[self->revalidation_head];
        self->revalidation_head = (self->revalidation_head + 1) % kRevalidationSlots;
        --self->revalidation_count;
        self->swr.EndRevalidate(job->key.data(), self->Now(), false, false);
        fortis::wallet::address::FreeRevalidation(job);
      }
      self->changed.Broadcast();
      break;
    }
    Revalidation* job = self->revalidations[self->revalidation_head];
    self->revalidation_head = (self->revalidation_head + 1) % kRevalidationSlots;
    --self->revalidation_count;
    self->revalidating = true;
    lock.Unlock();
    tj::Arena arena = tj::GrowingArena(kBackgroundArenaBytes);
    std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
    self->facets->RunRevalidation(job, arena);
    tj::FreeArena(arena);
    fortis::wallet::address::FreeRevalidation(job);
    lock.Lock();
    self->revalidating = false;
    self->changed.Broadcast();
  }
  return nullptr;
}

// ---- background ingests (the delta poll's race) --------------------------------
bool AddressService::Impl::IngestAsync(void* context, const char* account_id, const fortis::chains::Network& network,
                                       const char* address, int64_t max_age, uint64_t wait_ms) {
  auto* self = static_cast<Impl*>(context);
  auto* job = fortis::base::New<IngestJob>();
  if (!job) return false;
  snprintf(job->account_id, sizeof(job->account_id), "%s", account_id);
  job->network = network;
  snprintf(job->address, sizeof(job->address), "%s", address);
  job->max_age = max_age;
  fortis::base::UniqueLock lock(self->mutex);
  if (self->stopping || self->ingest_count == kIngestSlots) {
    lock.Unlock();
    release_ingest(job);
    return false;
  }
  self->ingests[(self->ingest_head + self->ingest_count) % kIngestSlots] = job;
  ++self->ingest_count;
  self->changed.Broadcast();
  lock.Unlock();
  fortis::base::UniqueLock job_lock(job->mutex);
  const timespec deadline = wake_after(wait_ms);
  while (!job->finished)
    if (job->done.WaitUntilLocked(job->mutex, deadline) != 0) break;
  const bool finished = job->finished;
  if (!finished) job->abandoned = true;
  job_lock.Unlock();
  if (finished) release_ingest(job);
  return true;
}

void AddressService::Impl::FinishIngest(IngestJob* job) {
  fortis::base::UniqueLock lock(job->mutex);
  job->finished = true;
  const bool abandoned = job->abandoned;
  job->done.Signal();
  lock.Unlock();
  if (abandoned) release_ingest(job);
}

void* AddressService::Impl::IngestMain(void* opaque) {
  auto* self = static_cast<Impl*>(opaque);
  for (;;) {
    fortis::base::UniqueLock lock(self->mutex);
    while (!self->stopping && self->ingest_count == 0) self->changed.WaitLocked(self->mutex);
    if (self->stopping) {
      while (self->ingest_count) {
        IngestJob* job = self->ingests[self->ingest_head];
        self->ingest_head = (self->ingest_head + 1) % kIngestSlots;
        --self->ingest_count;
        lock.Unlock();
        FinishIngest(job);
        lock.Lock();
      }
      self->changed.Broadcast();
      break;
    }
    IngestJob* job = self->ingests[self->ingest_head];
    self->ingest_head = (self->ingest_head + 1) % kIngestSlots;
    --self->ingest_count;
    self->ingesting = true;
    lock.Unlock();
    self->facets->EnsureFresh(job->account_id, job->network, job->address, job->max_age);
    FinishIngest(job);
    lock.Lock();
    self->ingesting = false;
    self->changed.Broadcast();
  }
  return nullptr;
}

// ---- foreground workers ----------------------------------------------------------
bool AddressService::Impl::Submit(AddressTask* task) {
  fortis::base::LockGuard lock(mutex);
  if (stopping || task_count == kQueueSlots) return false;
  tasks[(task_head + task_count) % kQueueSlots] = task;
  ++task_count;
  changed.Broadcast();
  return true;
}

void* AddressService::Impl::WorkerMain(void* opaque) {
  auto* self = static_cast<Impl*>(opaque);
  for (;;) {
    fortis::base::UniqueLock lock(self->mutex);
    while (!self->stopping && self->task_count == 0) self->changed.WaitLocked(self->mutex);
    if (self->task_count == 0) break;
    AddressTask* task = self->tasks[self->task_head];
    self->task_head = (self->task_head + 1) % kQueueSlots;
    --self->task_count;
    lock.Unlock();
    self->Run(task);
    free_task(task);
  }
  return nullptr;
}

void AddressService::Impl::Complete(const AddressTask& task, const http::HttpResponse& response) {
  http::HttpResponse copy = response;
  copy.rate_limit_cost = task.cost;
  copy.rate_limit_remaining = task.remaining;
  handoff->Complete(task.token, copy);
}

void AddressService::Impl::CompleteText(const AddressTask& task, int status, const char* reason, const char* body) {
  http::HttpResponse response{status, reason, kJsonType, reinterpret_cast<const uint8_t*>(body), strlen(body)};
  Complete(task, response);
}

// runFacet's error contract.
void AddressService::Impl::CompleteError(const AddressTask& task, const FacetReply& result) {
  char body[4096];
  switch (result.outcome) {
    case Outcome::kBadRequest: {
      const size_t length = error_body("BAD_REQUEST", result.detail, body, sizeof(body));
      http::HttpResponse response{400, "Bad Request", kJsonType, reinterpret_cast<const uint8_t*>(body), length};
      Complete(task, response);
      return;
    }
    case Outcome::kUnsupported: {
      const size_t length = error_body("UNSUPPORTED", result.detail, body, sizeof(body));
      http::HttpResponse response{400, "Bad Request", kJsonType, reinterpret_cast<const uint8_t*>(body), length};
      Complete(task, response);
      return;
    }
    case Outcome::kNotFound:
      CompleteText(task, 404, "Not Found", "{\"error\":\"NOT_FOUND\"}");
      return;
    case Outcome::kLaneBusy: {
      http::HttpResponse response{429, "Too Many Requests", kJsonType, reinterpret_cast<const uint8_t*>(kBusy), strlen(kBusy)};
      const uint32_t ms = result.retry_after_ms ? result.retry_after_ms : 2000;
      response.retry_after = static_cast<unsigned>((ms + 999) / 1000 < 1 ? 1 : (ms + 999) / 1000);
      Complete(task, response);
      return;
    }
    default:
      CompleteText(task, 502, "Bad Gateway", kUpstreamUnavailable);
      return;
  }
}

// sendStructuredOrJson: JSON or the StructuredReadResponse protobuf, Vary:
// Accept, no validator.
// At most kForegroundWorkers tasks complete at once, so a buffer is free.
uint8_t* AddressService::Impl::AcquireResponse() {
  fortis::base::LockGuard lock(mutex);
  uint8_t* body = nullptr;
  for (size_t i = 0; !body && i < kForegroundWorkers; ++i)
    if (!response_busy[i]) { response_busy[i] = true; body = responses[i].data(); }
  return body;
}
void AddressService::Impl::ReleaseResponse(uint8_t* body) {
  fortis::base::LockGuard lock(mutex);
  for (size_t i = 0; i < kForegroundWorkers; ++i)
    if (responses[i].data() == body) response_busy[i] = false;
}

void AddressService::Impl::CompleteStructured(const AddressTask& task, const Value* payload) {
  uint8_t* body = AcquireResponse();
  if (!body) { CompleteText(task, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}"); return; }
  tj::OutBuffer out{body, kResponseBytes, 0, true};
  const bool ok = task.protobuf ? tj::AppendStructuredReadResponse(payload, out) && out.ok
                                : tj::Stringify(payload, out) && out.ok;
  if (!ok) {
    ReleaseResponse(body);
    CompleteText(task, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}");
    return;
  }
  http::HttpResponse response{200, "OK", task.protobuf ? "application/protobuf" : kJsonType, body, out.used};
  response.vary_accept = true;
  Complete(task, response);
  ReleaseResponse(body);
}

void AddressService::Impl::CompleteOverview(const AddressTask& task, const Value* payload) {
  uint8_t* body = AcquireResponse();
  if (!body) { CompleteText(task, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}"); return; }
  tj::OutBuffer out{body, kResponseBytes, 0, true};
  bool refused = false;
  const bool ok = task.protobuf ? fortis::wallet::address::OverviewProtobuf(payload, out, &refused) && out.ok
                                : tj::Stringify(payload, out) && out.ok;
  if (!ok) {
    ReleaseResponse(body);
    static const char kInternal[] = "{\"error\":\"INTERNAL\"}";
    http::HttpResponse response{500, "Internal Server Error", kJsonType, reinterpret_cast<const uint8_t*>(kInternal),
                          sizeof(kInternal) - 1};
    // The route set its cache-control before protobuf-es threw.
    if (refused) response.cache_control = kOverviewCacheControl;
    Complete(task, response);
    return;
  }
  http::HttpResponse selected{200, "OK", task.protobuf ? "application/protobuf" : kJsonType, body, out.used};
  http::HttpRequest request{};
  char accept[] = "application/protobuf";
  request.accept = task.protobuf ? accept : nullptr;
  request.if_none_match = task.has_if_none_match ? task.if_none_match : nullptr;
  http::HttpResponse response{};
  if (!ConditionalStructuredResponse(request, "network-overview", selected, selected, &response)) {
    ReleaseResponse(body);
    CompleteText(task, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}");
    return;
  }
  response.cache_control = kOverviewCacheControl;
  Complete(task, response);
  ReleaseResponse(body);
}

void AddressService::Impl::Run(AddressTask* task) {
  tj::Arena arena = tj::GrowingArena(kForegroundArenaBytes);
  std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
  fortis::wallet::address::Facets& f = *facets;
  const char* device = task->device_id[0] ? task->device_id : nullptr;
  FacetReply result;
  switch (task->kind) {
    case AddressTask::kBalances:
      f.RememberAddressTopic(arena, device, task->network, task->subject);
      result = f.Balances(arena, task->network, task->subject, task->tokens.get(), task->token_count);
      break;
    case AddressTask::kInsight:
      result = f.Insight(arena, task->network, task->subject);
      break;
    case AddressTask::kNfts:
      result = f.Nfts(arena, task->network, task->subject);
      break;
    case AddressTask::kHoldings:
      result = f.TronHoldings(arena, task->subject);
      break;
    case AddressTask::kTransfers:
      f.RememberAddressTopic(arena, device, task->network, task->subject);
      result = f.Transfers(arena, task->network, task->subject, task->query, device);
      break;
    case AddressTask::kTx:
      f.RememberTxTopic(arena, device, task->network, task->subject);
      result = f.TxDetail(arena, task->network, task->subject);
      break;
    case AddressTask::kView:
      f.RememberTxTopic(arena, device, task->network, task->subject);
      result = f.TxView(arena, task->network, task->subject);
      break;
    case AddressTask::kOverview:
      result = f.Overview(arena, task->network);
      break;
  }
  if (result.outcome != Outcome::kOk || !result.payload) CompleteError(*task, result);
  else if (task->kind == AddressTask::kOverview) CompleteOverview(*task, result.payload);
  else CompleteStructured(*task, result.payload);
  tj::FreeArena(arena);
}

// ---- jobs ------------------------------------------------------------------------
bool AddressService::Impl::History(void* context, const char* price_id, int64_t from, int64_t to, bool hourly) {
  auto* self = static_cast<Impl*>(context);
  return self->history && self->history->BackgroundHistory(price_id, from, to, hourly);
}

bool AddressService::Impl::RunJob(Job job, char* summary, size_t capacity) {
  fortis::base::UniqueLock job_lock(job_mutex);
  tj::Arena arena = tj::GrowingArena(kJobArenaBytes);
  std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
  char text[256] = "{}";
  switch (job) {
    case Job::kPendingPoll:
      snprintf(text, sizeof(text), "{\"transitions\":%zu}", facets->PollPendingOnce(arena));
      break;
    case Job::kChangeDetection:
      snprintf(text, sizeof(text), "{\"observed\":%zu}", detector.Run(*facets, deps, arena));
      break;
    case Job::kCandleRepair: {
      const auto repaired = facets->RepairOperationCandles(arena,
          [this](const char* id, int64_t from, int64_t to, bool hourly) {
            return History(this, id, from, to, hourly);
          });
      snprintf(text, sizeof(text), "{\"groups\":%zu,\"historiesReady\":%zu,\"transfersRepaired\":%zu}",
               repaired.groups, repaired.histories_ready, repaired.transfers_repaired);
      break;
    }
    case Job::kRetention:
      snprintf(text, sizeof(text), "{\"pruned\":%zu}", facets->PruneTransfers(arena));
      break;
  }
  tj::FreeArena(arena);
  job_lock.Unlock();
  if (summary && capacity) snprintf(summary, capacity, "%s", text);
  return true;
}

double AddressService::Impl::JitterSample() const {
  double sample = 0.5;
  if (seams.jitter && seams.jitter->Sample(&sample)) return sample;
  uint32_t bits = 0;
  if (!fortis::auth::SecureRandomBytes(reinterpret_cast<uint8_t*>(&bits), sizeof(bits))) return 0.5;
  return static_cast<double>(bits) / 4294967295.0;
}

void* AddressService::Impl::SchedulerMain(void* opaque) {
  auto* self = static_cast<Impl*>(opaque);
  JobSlot slots[] = {
      {Job::kPendingPoll, kPendingPollMs, 0, 5000, 0},
      {Job::kChangeDetection, kChangeDetectionMs, 0.1, -1, 0},
      {Job::kCandleRepair, kCandleRepairMs, 0.1, -1, 0},
      {Job::kRetention, kRetentionMs, 0.1, -1, 0},
  };
  const uint64_t start = monotonic_ms();
  for (JobSlot& slot : slots)
    slot.due_ms = start + static_cast<uint64_t>(slot.initial_ms >= 0 ? static_cast<double>(slot.initial_ms)
                                                                      : self->JitterSample() * slot.interval_ms);
  for (;;) {
    fortis::base::UniqueLock lock(self->mutex);
    if (self->stopping) break;
    uint64_t next = UINT64_MAX;
    for (const JobSlot& slot : slots) if (slot.due_ms < next) next = slot.due_ms;
    const uint64_t now = monotonic_ms();
    if (next > now) {
      const timespec wake = wake_after(next - now > 1000 ? 1000 : next - now);
      self->changed.WaitUntilLocked(self->mutex, wake);
      continue;
    }
    lock.Unlock();
    for (JobSlot& slot : slots) {
      if (slot.due_ms > monotonic_ms()) continue;
      self->RunJob(slot.job, nullptr, 0);
      const double unit = self->JitterSample();
      const double delay =
          floor(static_cast<double>(slot.interval_ms) * (1 + slot.jitter_pct * (2 * unit - 1)) + 0.5);
      slot.due_ms = monotonic_ms() + static_cast<uint64_t>(delay > 1 ? delay : 1);
      lock.Lock();
      const bool stop = self->stopping;
      lock.Unlock();
      if (stop) break;
    }
  }
  return nullptr;
}

bool AddressService::Impl::StartThreads() {
  pthread_attr_t attr;
  if (pthread_attr_init(&attr) != 0) return false;
  bool ok = pthread_attr_setstacksize(&attr, 256 * 1024) == 0;
  for (size_t i = 0; ok && i < kForegroundWorkers; ++i) {
    ok = (workers[i].StartEntry<&Impl::WorkerMain>(this, &attr) ? 0 : -1) == 0;
    if (ok) ++started_workers;
  }
  if (ok) ok = background_started = (background.StartEntry<&Impl::BackgroundMain>(this, &attr) ? 0 : -1) == 0;
  // Revalidations run at background lane priority, as TypeScript's do.
  if (ok) transport.SetBackgroundThread(background.native());
  if (ok) ok = ingest_started = (ingest_thread.StartEntry<&Impl::IngestMain>(this, &attr) ? 0 : -1) == 0;
  if (ok && !seams.manual_jobs)
    ok = scheduler_started = (scheduler.StartEntry<&Impl::SchedulerMain>(this, &attr) ? 0 : -1) == 0;
  pthread_attr_destroy(&attr);
  return ok;
}

void AddressService::Impl::StopThreads() {
  fortis::base::UniqueLock lock(mutex);
  stopping = true;
  changed.Broadcast();
  lock.Unlock();
  if (scheduler_started) { scheduler.Join(); scheduler_started = false; }
  for (size_t i = 0; i < started_workers; ++i) workers[i].Join();
  started_workers = 0;
  if (ingest_started) { ingest_thread.Join(); ingest_started = false; }
  if (background_started) { background.Join(); background_started = false; }
}

// ---- public API ----------------------------------------------------------------------

void AddressService::ImplDeleter::operator()(Impl* impl) const { fortis::base::Delete(impl); }

AddressService::~AddressService() { Stop(); }

AddressService::AddressService(Impl* impl) : impl_(impl) {}
AddressService::AddressService(AddressService&&) noexcept = default;

base::Result<AddressService> AddressService::Create(
    const config::ProviderRuntimeConfig& config, AddressServiceDeps deps, const AddressServiceOptions& options) try {
  auto& kv = deps.kv;
  auto& coordinator = deps.coordinator;
  auto* start_transport = options.start_transport;
  auto* seams = options.seams;
  auto* spot = deps.spot;
  auto* history = deps.history;
  Impl* impl = fortis::base::New<Impl>(config, &kv, &coordinator, spot, history, seams);
  if (!impl) return base::Status(base::StatusCode::kResourceExhausted, "address allocation");
  AddressService service(impl);
  const bool transport_started =
      start_transport ? start_transport->Start(impl->transport) : impl->transport.Start();
  if (!impl->Init() || !transport_started ||
      !impl->sync.Start(impl->kv, &impl->store, impl->attestation_required, &impl->swr_clock) ||
      !impl->StartThreads()) {
    return base::Status(base::StatusCode::kUnavailable, "address initialization");
  }
  return service;
} catch (const std::bad_alloc&) {
  return base::Status(base::StatusCode::kResourceExhausted, "address allocation");
}

bool AddressService::Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff) {
  if (impl_->handoff) return false;
  impl_->handoff = &handoff;
  return router.Add<&Impl::HandleBalances>("GET", "/v1/networks/:network/address/:address/balances", *impl_) &&
         router.Add<&Impl::HandleInsight>("GET", "/v1/networks/:network/address/:address/insight", *impl_) &&
         router.Add<&Impl::HandleNfts>("GET", "/v1/networks/:network/address/:address/nfts", *impl_) &&
         router.Add<&Impl::HandleHoldings>("GET", "/v1/networks/:network/address/:address/token-holdings", *impl_) &&
         router.Add<&Impl::HandleTransfers>("GET", "/v1/networks/:network/address/:address/transfers", *impl_) &&
         router.Add<&Impl::HandleTx>("GET", "/v1/networks/:network/tx/:hash", *impl_) &&
         router.Add<&Impl::HandleView>("GET", "/v1/networks/:network/tx/:hash/view", *impl_) &&
         router.Add<&Impl::HandleOverview>("GET", "/v1/networks/:network/overview", *impl_) &&
         impl_->sync.Register(router);
}

fortis::wallet::address::Store* AddressService::store() { return &impl_->store; }

int64_t AddressService::NowMs() const { return impl_->Now(); }

void AddressService::Stop() {
  if (!impl_) return;
  impl_->StopThreads();
  // Queued foreground tasks answered nothing: their handoff slots time out.
  for (size_t i = 0; i < impl_->task_count; ++i)
    free_task(impl_->tasks[(impl_->task_head + i) % kQueueSlots]);
  impl_->task_count = 0;
  impl_->transport.Stop();
  impl_.reset();
}

bool AddressService::RunJobNow(Job job, char* summary, size_t capacity) {
  return impl_->RunJob(job, summary, capacity);
}

bool AddressService::WaitIdle(uint32_t timeout_ms) {
  const uint64_t deadline = monotonic_ms() + timeout_ms;
  fortis::base::LockGuard lock(impl_->mutex);
  while (impl_->revalidation_count || impl_->revalidating || impl_->ingest_count || impl_->ingesting) {
    const uint64_t now = monotonic_ms();
    if (now >= deadline) break;
    const timespec wake = wake_after(deadline - now > 100 ? 100 : deadline - now);
    impl_->changed.WaitUntilLocked(impl_->mutex, wake);
  }
  const bool idle = !impl_->revalidation_count && !impl_->revalidating && !impl_->ingest_count && !impl_->ingesting;
  return idle;
}

}  // namespace fortis::wallet
