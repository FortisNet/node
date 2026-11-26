#include "threading.h"
#pragma once

#include "clock.h"
#include "discovery_cache.h"
#include "discovery_provider_owner.h"
#include "http_server.h"
#include "news_store.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

namespace fortis::news {

// The TypeScript news routes over NewsFacet and NewsReactions:
//   GET /v1/news                 the cached feed (TTL.news, key news:<variant>)
//   GET /v1/news/:id             one story with its sources (TTL.newsItem,
//                                key newsitem:<id>; an unknown id caches null)
//   GET /v1/news/:id/reactions   price reactions around the publish time
//                                (60 s fresh / 120 s stale), measured from
//                                CoinGecko market_chart/range samples cached
//                                per asset and UTC day (news-samples:v1:…)
// Each answers JSON or protobuf (fortisnet.intelligence.v1 news.proto) with
// the TypeScript weak ETag and 304, after the bearer/quota policy (cost 1).
constexpr size_t kNewsArenaBytes = 4 * 1024 * 1024;
constexpr size_t kNewsOutputBytes = 1024 * 1024;
constexpr size_t kNewsKvScratchBytes = 1024 * 1024;

struct NewsWorkspace {
  fortis::base::Mutex mutex;
  discovery::DiscoveryCache* cache;
  unsigned char kv_scratch[kNewsKvScratchBytes];
  uint8_t arena[kNewsArenaBytes];
  uint8_t json_out[kNewsOutputBytes];
  uint8_t proto_out[kNewsOutputBytes];
};

class ReactionService;
using NewsReactionsState = ReactionService;

struct NewsRouteContext {
  fortis::news::NewsStore store{};
  NewsWorkspace* workspace = nullptr;
  http::HttpResponseHandoff* handoff = nullptr;
  discovery::DiscoveryProviderOwner* owner = nullptr;  // CoinGecko for reaction samples
  NewsReactionsState* reactions = nullptr;
  bool attestation_required = false;
  // Wall clock (NowMs in epoch milliseconds); null uses the cache clock.
  fortis::base::Clock* clock = nullptr;
  NewsRepository* repository = nullptr;  // borrowed; null adapts store
};

NewsReactionsState* CreateNewsReactionsState();
void DestroyNewsReactionsState(NewsReactionsState* state);

bool RegisterNewsRoutes(http::HttpRouter& router, NewsRouteContext* context);

// The TypeScript NewsFacet.feed({topics: ['chains','protocol','security'],
// limit: 100}) for the network overview: the cached
// `news:chains+protocol+security|100` envelope GET /v1/news serves (same
// TTL), computed from `store` on a miss and revalidated after a stale read.
// `output` receives the JSON payload {items, asOf, stale}. Thread-safe for
// distinct `arena_memory` buffers; the cache must not be shared with a
// caller holding its scratch.
bool NewsOverviewFeed(const fortis::news::NewsStore& store, discovery::DiscoveryCache* cache, int64_t now_ms,
                      uint8_t* arena_memory, size_t arena_bytes, uint8_t* output, size_t capacity,
                      size_t* length);

// The handlers' pure rules as entry points (exercised by tests/news_reads_test.cpp).
struct NewsReactionWindow {
  int status = 0;  // 0 pending, 1 unavailable, 2 measured
  double return_pct = 0;
  double baseline[2] = {0, 0};  // [at, price] when measured
  double endpoint[2] = {0, 0};
};
namespace news_read_rules {
// GET /v1/news/{id}'s id rule over the decoded parameter bytes.
bool NormalizeId(const char* bytes, size_t length, char output[27]);
// GET /v1/news's query rules over a raw request target: false when the
// native parse scratch is unavailable; otherwise *valid and either the SWR
// key (news:<variant>) or the exact 400 body.
bool FeedQueryOf(const char* raw_target, bool* valid, char* key, size_t key_capacity, char* body,
                 size_t body_capacity);
// The cached `prices` value of one market_chart/range body (false: the
// fetcher's failure).
bool PricesValue(const uint8_t* body, size_t length, uint8_t* output, size_t capacity, size_t* used);
// cleanSamples + measureReaction over raw samples; returns the cleaned count
// (cleaned_* hold count entries).
size_t MeasureReaction(const double* at, const double* price, size_t count, int64_t published_ms,
                       int64_t now_ms, int seconds, double* cleaned_at, double* cleaned_price,
                       NewsReactionWindow* window);
}  // namespace news_read_rules

}  // namespace fortis::news
