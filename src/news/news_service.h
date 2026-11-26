#pragma once

#include <memory>
#include "status.h"

#include "clock.h"
#include "ownership.h"
#include "discovery_cache.h"
#include "discovery_provider_owner.h"
#include "http_server.h"
#include "media_service.h"
#include "provider_runtime_config.h"
#include "news_ingest.h"
#include "seams.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::storage { class KvStore; }
namespace fortis::providers { class ProviderExecutionCoordinator; }

namespace fortis::news {

// Normal startup passes none and uses DiscoveryProviderOwner::Start() for the
// CoinGecko reaction samples. Only a test main supplies a substituting start.
using NewsOwnerStarter = fortis::base::Starter<discovery::DiscoveryProviderOwner>;

// Test seams. A normal composition passes none: the wall clock, CSPRNG
// jitter and ULIDs, the publisher hosts with the configured CA bundle and the
// public-address policy, and the recurring jobs.
struct NewsServiceSeams {
  // NowMs: the news, cache and ingest clock (also the media clock unless
  // media.clock is set); SleepMs: the feed retry backoff sleeps.
  fortis::base::Clock* clock = nullptr;
  fortis::base::JitterSource* jitter = nullptr;
  fortis::base::IdSource* new_id = nullptr;
  // Every feed request (and redirect hop) dials feed_host with feed_port +
  // feed_ca (https:) or feed_plain_port (http:) under `allow_feed_address`,
  // carrying the URL's host in the request header `feed_host_header`
  // (none when unset); the path is unchanged.
  const char* feed_host = nullptr;
  const char* feed_host_header = nullptr;
  const char* feed_port = nullptr;
  const char* feed_plain_port = nullptr;
  const char* feed_ca = nullptr;
  bool (*allow_feed_address)(void* context, uint32_t ipv4) = nullptr;
  // True: no scheduler thread; ingest and media prune run only through
  // RunIngestNow / RunMediaPruneNow.
  bool manual_jobs = false;
  MediaSeams media{};
  void* context = nullptr;  // allow_feed_address's context
};

// News and media behind FORTIS_NATIVE_NEWS_CANARY (which requires a
// validated FORTIS_CA_BUNDLE_PATH):
//   GET /v1/news, /v1/news/:id, /v1/news/:id/reactions (news_routes.h)
//   GET /v1/media/:key (media_service.h, public like the TypeScript route)
// and their two recurring jobs, scheduled like the TypeScript RecurringJob:
//   news ingest     every 300 s +-10 %, first run immediately (initialDelayMs 0)
//   media prune     every 24 h +-10 %, first run at a random point of the first
//                   day (the daily-maintenance media step; the other daily
//                   steps belong to their own batches)
// Both run on one scheduler thread, so neither overlaps itself or the other
// (TypeScript completion-schedules each job). The 8 MiB run arena, 6 MiB
// per-feed scratch and 1 MiB body buffer are reserved at startup and reused
// across runs. This avoids repeated large contiguous allocations in the guest
// buddy allocator as other services use memory.
struct NewsServiceDeps {
  storage::KvStore& kv;
  providers::ProviderExecutionCoordinator& coordinator;
};
struct NewsServiceOptions {
  NewsOwnerStarter* start_owner = nullptr;
  const NewsServiceSeams* seams = nullptr;
};

// Factory-created resource handle. Moved-from handles are destruction-only.
class NewsService {
 public:
  NewsService(NewsService&&) noexcept;
  NewsService& operator=(NewsService&&) = delete;
  ~NewsService();
  NewsService(const NewsService&) = delete;
  NewsService& operator=(const NewsService&) = delete;

  static base::Result<NewsService> Create(const config::ProviderRuntimeConfig& config, NewsServiceDeps deps,
                                      const NewsServiceOptions& options = {});
  // Registers the four routes, starts the media worker and the scheduler.
  bool Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff);
  void Stop();

  // One ingest run on a dedicated thread, joined (serialized with the scheduler).
  bool RunIngestNow(fortis::news::IngestResult* result);
  // One media prune pass on the caller's thread.
  size_t RunMediaPruneNow(int64_t now_ms, size_t* orphans_removed);
  // Completed scheduled runs (diagnostics/tests).
  uint64_t ingest_runs() const;
  uint64_t prune_runs() const;

  struct Impl;

 private:
  explicit NewsService(Impl* impl);
  // Defined beside Impl: only that unit can destroy one.
  struct ImplDeleter { void operator()(Impl* impl) const; };
  std::unique_ptr<Impl, ImplDeleter> impl_;
};

}  // namespace fortis::news
