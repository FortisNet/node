#include <cmath>
#include <memory>
#include <new>
#include "news_service.h"
#include "log.h"
#include "ownership.h"

#include "device_auth.h"
#include "network_registry.h"
#include "news_routes.h"
#include "outbound_https.h"
#include "price_series_store.h"
#include "provider_execution_coordinator.h"
#include "threading.h"
#include "whatwg_url.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

namespace fortis::news {

namespace {

using fortis::news::FeedRegistration;
using fortis::news::IngestResult;

constexpr int64_t kIngestIntervalMs = 300000;     // TypeScript news ingest
constexpr int64_t kPruneIntervalMs = 86400000;    // daily retention
constexpr double kJitterPct = 0.1;                // RecurringJob default
constexpr uint32_t kFeedTimeoutMs = 15000;        // TypeScript FETCH_TIMEOUT_MS
constexpr size_t kFeedAttempts = 3;               // HttpClient.json GET: 1 + 2 retries
constexpr uint32_t kFeedRetryDelaysMs[2] = {250, 1000};
constexpr uint32_t kMaxRetrySleepMs = 5000;
// fetch() follows up to 20 redirects; the native transport follows five,
// each re-parsed (WHATWG) and dialled under the public-address policy.
constexpr size_t kFeedMaxRedirects = 5;
constexpr size_t kDictionaryArenaBytes = 256 * 1024;
const char kFeedUserAgent[] =
    "FortisNetIntelligence/1.0 (+https://fortisnet.app; wallet news aggregator)";
const char kFeedAccept[] =
    "application/rss+xml, application/atom+xml, application/xml;q=0.9, text/xml;q=0.8";

int64_t wall_ms() {
  timeval now{};
  gettimeofday(&now, nullptr);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
}

void sleep_for(uint32_t ms) {
  timespec delay{static_cast<time_t>(ms / 1000), static_cast<long>(ms % 1000) * 1000000L};
  while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

// href[pathname_start, hash): the request target fetch() sends.
bool request_target(const fortis::compat::url::Url& url, char* output, size_t capacity) {
  const size_t end = url.hash_start == fortis::compat::url::kOmitted ? url.href_length : url.hash_start;
  if (url.pathname_start >= end || end - url.pathname_start + 1 > capacity) return false;
  memcpy(output, url.href + url.pathname_start, end - url.pathname_start);
  output[end - url.pathname_start] = '\0';
  return true;
}

}  // namespace

struct NewsService::Impl {
  Impl(const config::ProviderRuntimeConfig& cfg, fortis::storage::KvStore& store, providers::ProviderExecutionCoordinator& coordinator,
       NewsWorkspace* buffers, const NewsServiceSeams* service_seams)
      : config(cfg), kv(&store), owner(cfg, &coordinator), workspace(buffers),
        cache_seams{service_seams ? service_seams->clock : nullptr,
                    service_seams ? service_seams->jitter : nullptr},
        cache(&store, buffers->kv_scratch, sizeof(buffers->kv_scratch), &cache_seams),
        prices(store) {
    if (service_seams) { seams = *service_seams; have_seams = true; }
    new (workspace) NewsWorkspace;
    workspace->cache = &cache;
    route.store.kv = &store;
    route.store.prices = &prices;
    route.workspace = workspace;
    route.owner = &owner;
    route.attestation_required = config.attestation_required;
    if (have_seams && seams.clock) route.clock = seams.clock;
    media_config.ca_bundle_configured = config.ca_bundle_configured;
    snprintf(media_config.ca_bundle_path, sizeof(media_config.ca_bundle_path), "%s", config.ca_bundle_path);
  }
  ~Impl() {
    DestroyNewsReactionsState(route.reactions);

    run_memory.reset();
    feed_scratch.reset();
    feed_body.reset();
    dictionary_memory.reset();
    url_scratch.reset();
    urls.reset();
  }

  std::unique_ptr<NewsWorkspace, base::StorageDeleter> workspace_storage;
  config::ProviderRuntimeConfig config;
  fortis::storage::KvStore* kv;
  discovery::DiscoveryProviderOwner owner;
  NewsWorkspace* workspace;
  NewsServiceSeams seams{};
  bool have_seams = false;
  discovery::DiscoveryCacheSeams cache_seams;
  discovery::DiscoveryCache cache;
  fortis::prices::PriceSeriesStore prices;
  NewsRouteContext route{};
  MediaServiceConfig media_config{};
  MediaService media;
  // The guest's buddy allocator may not find another contiguous 8 MiB block
  // after several runs. Reserve the three job buffers once and reuse them.
  fortis::base::Buffer run_memory;
  fortis::base::Buffer feed_scratch;
  fortis::base::Buffer feed_body;
  fortis::base::Buffer dictionary_memory;
  fortis::news::TickerDictionary dictionary{};
  std::unique_ptr<fortis::compat::url::UrlScratch, fortis::base::StorageDeleter> url_scratch;  // feed transport (job thread)
  std::unique_ptr<fortis::compat::url::Url, fortis::base::StorageDeleter> urls;                // current and next hop

  fortis::base::Mutex job_mutex;  // one job at a time
  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable wake;
  fortis::base::Thread scheduler;
  bool scheduler_started = false;
  bool stopping = false;
  uint64_t ingest_count = 0;
  uint64_t prune_count = 0;

  int64_t Now() const { return have_seams && seams.clock ? seams.clock->NowMs() : wall_ms(); }

  // The ingest clock: Impl::Now.
  class IngestClock final : public fortis::base::Clock {
   public:
    explicit IngestClock(const Impl* impl) : impl_(impl) {}
    int64_t NowMs() override { return impl_->Now(); }

   private:
    const Impl* impl_;
  };

  double Random() const {
    double sample = 0.5;
    if (have_seams && seams.jitter && seams.jitter->Sample(&sample)) return sample;
    uint32_t bits = 0;
    if (!fortis::auth::SecureRandomBytes(reinterpret_cast<uint8_t*>(&bits), sizeof(bits))) return 0.5;
    return bits / 4294967296.0;
  }

  // jitteredDelay(base, 0.1, random)
  int64_t Jittered(int64_t base) const {
    const double unit = Random();
    const double value = static_cast<double>(base) * (1 + kJitterPct * (2 * unit - 1));
    const int64_t rounded = static_cast<int64_t>(std::floor(value + 0.5));
    return rounded < 1 ? 1 : rounded;
  }

  void Sleep(uint32_t ms) {
    if (have_seams && seams.clock) seams.clock->SleepMs(ms);
    else sleep_for(ms);
  }

  // ---- the feed transport (HttpClient.json with parseText) -----------------

  enum class Outcome { kOk, kRetry, kFail };

  // One fetch() attempt: follows redirects, 2xx answers the body.
  Outcome Attempt(const FeedRegistration& feed, uint8_t* body, size_t capacity, size_t* length,
                  uint32_t* retry_after_ms) {
    using namespace fortis::compat::url;
    *retry_after_ms = 0;
    Url* current = &urls.get()[0];
    Url* next = &urls.get()[1];
    if (!Parse(feed.url, strlen(feed.url), nullptr, current, url_scratch.get())) return Outcome::kFail;
    timespec started{};
    clock_gettime(CLOCK_MONOTONIC, &started);
    const int64_t deadline = static_cast<int64_t>(started.tv_sec) * 1000 +
                             started.tv_nsec / 1000000 + kFeedTimeoutMs;
    for (size_t hop = 0; hop <= kFeedMaxRedirects; ++hop) {
      const View protocol = Protocol(*current);
      const bool https = protocol.length == 6 && memcmp(protocol.data, "https:", 6) == 0;
      const bool http = protocol.length == 5 && memcmp(protocol.data, "http:", 5) == 0;
      if ((!https && !http) || current->host_type == HostType::kIpv6) return Outcome::kFail;
      const View hostname = Hostname(*current);
      const View port = Port(*current);
      char host[256];
      char port_text[8];
      char path[8192];
      if (!hostname.length || hostname.length >= sizeof(host) || port.length >= sizeof(port_text) ||
          !request_target(*current, path, sizeof(path))) return Outcome::kFail;
      memcpy(host, hostname.data, hostname.length);
      host[hostname.length] = '\0';
      if (port.length) { memcpy(port_text, port.data, port.length); port_text[port.length] = '\0'; }
      else snprintf(port_text, sizeof(port_text), "%s", https ? "443" : "80");

      timespec now{};
      clock_gettime(CLOCK_MONOTONIC, &now);
      const int64_t remaining = deadline - (static_cast<int64_t>(now.tv_sec) * 1000 +
                                            now.tv_nsec / 1000000);
      if (remaining <= 0) return Outcome::kRetry;  // the attempt timed out
      http::OutboundRequest request{};
      request.method = http::OutboundMethod::kGet;
      request.path = path;
      request.plaintext = http;
      request.timeout_ms = static_cast<uint32_t>(remaining);
      request.max_response_bytes = capacity;
      request.user_agent = kFeedUserAgent;
      snprintf(request.headers[0].name, sizeof(request.headers[0].name), "accept");
      snprintf(request.headers[0].value, sizeof(request.headers[0].value), "%s", kFeedAccept);
      request.header_count = 1;
      if (have_seams && seams.feed_host) {
        request.host = seams.feed_host;
        request.port = http ? seams.feed_plain_port : seams.feed_port;
        request.ca_file = http ? nullptr : seams.feed_ca;
        request.allow_address = seams.allow_feed_address;
        request.policy_context = seams.context;
        if (seams.feed_host_header && *seams.feed_host_header) {
          snprintf(request.headers[1].name, sizeof(request.headers[1].name), "%s",
                   seams.feed_host_header);
          snprintf(request.headers[1].value, sizeof(request.headers[1].value), "%.240s:%.7s",
                   host, port_text);
          request.header_count = 2;
        }
      } else {
        if (https && !config.ca_bundle_configured) return Outcome::kFail;
        request.host = host;
        request.port = port_text;
        request.ca_file = https ? config.ca_bundle_path : nullptr;
      }
      const http::OutboundResponse response = OutboundHttpsRequest(request, body, capacity);
      if (response.status >= 300 && response.status < 400 && response.status != 304) {
        if (!response.has_location) return Outcome::kFail;
        // new URL(location, current): the header's bytes as Latin-1.
        char converted[3072];
        size_t used = 0;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(response.location);
             *p; ++p) {
          if (*p < 0x80) converted[used++] = static_cast<char>(*p);
          else {
            converted[used++] = static_cast<char>(0xc0 | (*p >> 6));
            converted[used++] = static_cast<char>(0x80 | (*p & 0x3f));
          }
        }
        if (!Parse(converted, used, current, next, url_scratch.get())) return Outcome::kFail;
        Url* swap = current;
        current = next;
        next = swap;
        continue;
      }
      if (response.status >= 200 && response.status < 300 &&
          response.error == http::OutboundError::kNone) {
        *length = response.body_length;
        return Outcome::kOk;
      }
      if (response.status != 0) {
        // Retry only 5xx and 408 (a 429 or other 4xx is thrown at once).
        if (response.has_retry_after) {
          char* end = nullptr;
          const double seconds = strtod(response.retry_after, &end);
          if (end && end != response.retry_after && seconds >= 0) {
            const double ms = seconds * 1000 < 60000 ? seconds * 1000 : 60000;
            *retry_after_ms = static_cast<uint32_t>(ms);
          }
        }
        return response.status >= 500 || response.status == 408 ? Outcome::kRetry : Outcome::kFail;
      }
      // A transport failure. An oversized document is not retried: the
      // TypeScript client would have read it whole and parseFeed refused it.
      if (response.error == http::OutboundError::kTooLarge ||
          response.error == http::OutboundError::kPrivateAddress ||
          response.error == http::OutboundError::kInvalidRequest) return Outcome::kFail;
      return Outcome::kRetry;
    }
    return Outcome::kFail;  // too many redirects (fetch's TypeError)
  }

  static bool fetch_feed(void* opaque, const FeedRegistration& feed, uint8_t* body,
                         size_t capacity, size_t* length) {
    Impl* impl = static_cast<Impl*>(opaque);
    uint32_t retry_after = 0;
    for (size_t attempt = 0; attempt < kFeedAttempts; ++attempt) {
      if (attempt) {
        uint32_t delay = kFeedRetryDelaysMs[attempt - 1];
        if (retry_after > delay) delay = retry_after;
        if (delay > kMaxRetrySleepMs) delay = kMaxRetrySleepMs;
        impl->Sleep(delay);
      }
      const Outcome outcome = impl->Attempt(feed, body, capacity, length, &retry_after);
      if (outcome == Outcome::kOk) return true;
      if (outcome == Outcome::kFail) return false;
    }
    return false;
  }

  static void log_line(void*, const char* message) {
    fortis::base::LogError("news", "%s", message);
  }

  bool Ingest(IngestResult* result) {
    fortis::news::IngestDeps deps;
    deps.store = route.store;
    deps.media_kv = kv;
    deps.dictionary = &dictionary;
    deps.fetch = fetch_feed;
    deps.fetch_context = this;
    IngestClock clock(this);
    deps.clock = &clock;
    if (have_seams && seams.new_id) deps.new_id = seams.new_id;
    deps.log = log_line;
    fortis::base::LockGuard job_lock(job_mutex);
    const bool ok = fortis::news::NewsIngestor(deps).Run(run_memory.data(), feed_scratch.data(), feed_body.data(), result);
    return ok;
  }

  size_t Prune(int64_t now, size_t* orphans) {
    fortis::base::LockGuard job_lock(job_mutex);
    const size_t freed = media.PruneBytes(now, orphans);
    return freed;
  }

  static void SchedulerMain(Impl* self) { self->SchedulerLoop(); }

  // Waits until `due` (monotonic ms) or stop; false when stopping.
  bool WaitUntil(int64_t due) {
    fortis::base::LockGuard lock(mutex);
    while (!stopping) {
      timespec now{};
      clock_gettime(CLOCK_MONOTONIC, &now);
      const int64_t now_ms = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
      if (now_ms >= due) break;
      timespec wall{};
      clock_gettime(CLOCK_REALTIME, &wall);
      int64_t wait = due - now_ms;
      if (wait > 1000) wait = 1000;  // re-check against the monotonic clock
      wall.tv_sec += static_cast<time_t>(wait / 1000);
      wall.tv_nsec += static_cast<long>(wait % 1000) * 1000000L;
      if (wall.tv_nsec >= 1000000000L) { ++wall.tv_sec; wall.tv_nsec -= 1000000000L; }
      wake.WaitUntilLocked(mutex, wall);
    }
    const bool running = !stopping;
    return running;
  }

  static int64_t monotonic() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
  }

  void SchedulerLoop() {
    int64_t next_ingest = monotonic();  // initialDelayMs: 0
    int64_t next_prune = monotonic() + static_cast<int64_t>(Random() * kPruneIntervalMs);
    for (;;) {
      const int64_t due = next_ingest < next_prune ? next_ingest : next_prune;
      if (!WaitUntil(due)) return;
      const int64_t now = monotonic();
      if (now >= next_ingest) {
        IngestResult result;
        if (Ingest(&result)) {
          fortis::base::LogInfo("news", "news ingest fetched=%zu fresh=%zu stored=%zu pruned=%zu feeds_failed=%zu "
                  "arena=%zu scratch=%zu",
                  result.fetched, result.fresh, result.stored, result.pruned, result.feeds_failed,
                  result.run_arena_used, result.scratch_peak);
        } else {
          fortis::base::LogError("news", "news ingest failed");
        }
        fortis::base::UniqueLock lock(mutex);
        ++ingest_count;
        lock.Unlock();
        next_ingest = monotonic() + Jittered(kIngestIntervalMs);
      }
      if (monotonic() >= next_prune) {
        size_t orphans = 0;
        const size_t freed = Prune(Now(), &orphans);
        fortis::base::LogInfo("news", "media prune freed=%zu orphans=%zu", freed, orphans);
        fortis::base::UniqueLock lock(mutex);
        ++prune_count;
        lock.Unlock();
        next_prune = monotonic() + Jittered(kPruneIntervalMs);
      }
    }
  }
};

void NewsService::ImplDeleter::operator()(Impl* impl) const { fortis::base::Delete(impl); }

NewsService::~NewsService() {
  if (!impl_) return;
  Stop();
  impl_.reset();
}

NewsService::NewsService(Impl* impl) : impl_(impl) {}
NewsService::NewsService(NewsService&&) noexcept = default;

base::Result<NewsService> NewsService::Create(
    const config::ProviderRuntimeConfig& config, NewsServiceDeps deps, const NewsServiceOptions& options) try {
  auto& kv = deps.kv;
  auto& coordinator = deps.coordinator;
  auto* start_owner = options.start_owner;
  auto* seams = options.seams;
  if (!coordinator.valid() || !fortis::news::NewsTextInitialize()) return base::Status(base::StatusCode::kUnavailable, "news initialization");
  auto workspace_storage = base::AllocateStorage<NewsWorkspace>(sizeof(NewsWorkspace));
  auto* workspace = workspace_storage.get();
  Impl* impl = workspace ? fortis::base::New<Impl>(config, kv, coordinator, workspace, seams) : nullptr;
  if (!impl) { return base::Status(base::StatusCode::kResourceExhausted, "news allocation"); }
  struct InitGuard {
    Impl* impl;
    ~InitGuard() {
      if (!impl) return;
      impl->media.Stop();
      impl->owner.Stop();
      base::Delete(impl);
        }
  } guard{impl};
  impl->workspace_storage = std::move(workspace_storage);
  impl->route.reactions = CreateNewsReactionsState();
  impl->run_memory = fortis::base::Buffer::Allocate(fortis::news::kIngestArenaBytes);
  impl->feed_scratch = fortis::base::Buffer::Allocate(fortis::news::kFeedScratchBytes);
  impl->feed_body = fortis::base::Buffer::Allocate(fortis::news::kMaxFeedBodyBytes);
  impl->dictionary_memory = fortis::base::Buffer::Allocate(kDictionaryArenaBytes);
  impl->url_scratch = base::AllocateStorage<fortis::compat::url::UrlScratch>(sizeof(fortis::compat::url::UrlScratch));
  impl->urls = base::AllocateStorage<fortis::compat::url::Url>(2 * sizeof(fortis::compat::url::Url));
  bool ok = impl->route.reactions && impl->run_memory && impl->feed_scratch &&
            impl->feed_body && impl->dictionary_memory && impl->url_scratch && impl->urls;
  if (ok) {
    fortis::news::Arena arena{impl->dictionary_memory.data(), kDictionaryArenaBytes};
    networks::NativeNetworkRegistry registry;
    ok = fortis::news::BuildTickerDictionary(registry, &arena, &impl->dictionary) && arena.ok;
  }
  MediaSeams media_seams{};
  if (seams) {
    media_seams = seams->media;
    if (!media_seams.clock) media_seams.clock = seams->clock;
  }
  ok = ok && impl->media.Start(impl->media_config, kv, seams ? &media_seams : nullptr) &&
       (start_owner ? start_owner->Start(impl->owner) : impl->owner.Start());
  if (!ok) {
    return base::Status(base::StatusCode::kUnavailable, "news initialization");
  }
  guard.impl = nullptr;
  return NewsService(impl);
} catch (const std::bad_alloc&) {
  return base::Status(base::StatusCode::kResourceExhausted, "news allocation");
}

bool NewsService::Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff) {
  if (impl_->route.handoff) return false;
  impl_->route.handoff = &handoff;
  if (!RegisterNewsRoutes(router, &impl_->route) || !impl_->media.Register(router, handoff))
    return false;
  if (impl_->have_seams && impl_->seams.manual_jobs) return true;
  // RunIngest keeps a FeedEntry[100] array and regex frames on the stack.
  impl_->scheduler_started = impl_->scheduler.Start(Impl::SchedulerMain, impl_.get(), 512 * 1024);
  return impl_->scheduler_started;
}

void NewsService::Stop() {
  fortis::base::UniqueLock lock(impl_->mutex);
  impl_->stopping = true;
  impl_->wake.Broadcast();
  lock.Unlock();
  if (impl_->scheduler_started) impl_->scheduler.Join();
  impl_->scheduler_started = false;
  impl_->owner.Stop();
  impl_->media.Stop();
}

namespace {
struct IngestCall {
  NewsService::Impl* impl;
  IngestResult* result;
  bool ok;
};
}  // namespace

bool NewsService::RunIngestNow(IngestResult* result) {
  // On its own thread: the pipeline wants more stack than an HTTP worker has.
  struct Runner {
    static void Main(IngestCall* call) { call->ok = call->impl->Ingest(call->result); }
  };
  IngestCall call{impl_.get(), result, false};
  fortis::base::Thread thread;
  if (!thread.Start(Runner::Main, &call, 512 * 1024)) return false;
  thread.Join();
  return call.ok;
}

size_t NewsService::RunMediaPruneNow(int64_t now_ms, size_t* orphans_removed) {
  return impl_->Prune(now_ms, orphans_removed);
}

uint64_t NewsService::ingest_runs() const {
  fortis::base::LockGuard lock(impl_->mutex);
  const uint64_t count = impl_->ingest_count;
  return count;
}

uint64_t NewsService::prune_runs() const {
  fortis::base::LockGuard lock(impl_->mutex);
  const uint64_t count = impl_->prune_count;
  return count;
}

}  // namespace fortis::news
