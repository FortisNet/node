#pragma once

#include "clock.h"
#include "kv_store.h"
#include "news_text.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::prices { class PriceSeriesStore; }

// The TypeScript KvNewsStore on the native runtime KV. Every story,
// time-index, update-index, source-index and article-ownership key of one
// persisted batch of 32 stories changes in one durable KV frame, read through
// an in-memory overlay so later stories in a batch see earlier writes exactly
// like the TypeScript write transaction does. Keys (ASCII, bounded):
//   news/item/<id>                              story JSON (below)
//   news/time/<publishedAt ms, 13 digits>/<id>  -> id
//   news/updated/<updatedAt ms, 13 digits>/<id> -> id
//   news/source/<sourceId>/<publishedAt ms>/<sha256(articleKey) hex>
//                                               -> articleKey (UTF-8)
//   news/article_owner/<sha256(articleKey) hex> -> id
// TypeScript URI-encodes the article key into the key itself; the native
// store hashes it instead so a long publisher id cannot exceed the 1 KiB KV
// key bound. Every entry of a story expires at publishedAt + 7 days.
//
// The story value is native JSON (no historic TypeScript data is migrated):
//   {"id","headline","summary","topics":[…],"impact","tickers":[{"symbol",
//    "coinId"}],"publisher","url","imageUrl","imageMediaKey","publishedAt":ms,
//    "updatedAt":ms,"articles":[{"sourceId","publisher","externalId","title",
//    "summary","url","imageUrl","publishedAt":ms}]}
// Strings are JSON.stringify-escaped UTF-16 (lone surrogates survive).
// Reading applies KvNewsStore.decode: members sorted by publishedAt
// ascending (stable) with trust 0 and no categories or declared topics.
namespace fortis::news {

constexpr int64_t kNewsTtlMs = 7LL * 86400000;
constexpr size_t kNewsPageSize = 200;
constexpr size_t kNewsFeedDefaultLimit = 40;
constexpr size_t kNewsFeedMaxLimit = 100;

enum class StoreStatus : uint8_t { kOk, kIoError, kNoMemory, kCorrupt };

struct NewsStore {
  storage::KvStore* kv = nullptr;
  // The durable spot rows read for change24hPct; null joins nothing.
  const prices::PriceSeriesStore* prices = nullptr;
};

// Borrowed domain persistence capability. Every operation preserves the
// original store's transaction/snapshot boundary and caller-owned arena.
class NewsRepository {
 public:
  virtual StoreStatus KnownArticleKeys(const char* const* source_ids,
                             size_t source_count, int64_t since_ms, int64_t now_ms,
                             Arena* arena, U16** keys, size_t* key_count) const = 0;
  virtual StoreStatus RecentClusters(int64_t since_ms, int64_t now_ms,
                           Arena* arena, Cluster** clusters, size_t* count) const = 0;
  virtual StoreStatus Persist(Cluster* const* clusters, size_t count,
                    base::Clock& clock, Arena* arena) const = 0;
  virtual StoreStatus Prune(int64_t before_ms, Arena* arena, size_t* removed) const = 0;
  virtual StoreStatus Feed(const Topic* topics, size_t topic_count, size_t limit,
                 int64_t now_ms, Arena* arena, Cluster** clusters, size_t* count) const = 0;
  virtual StoreStatus Item(const char* id, int64_t now_ms, Arena* arena,
                 Cluster* cluster, bool* found) const = 0;
  virtual StoreStatus LastIngestAt(int64_t now_ms, bool* found, int64_t* at_ms) const = 0;
  virtual StoreStatus FeedSnapshot(const Topic* topics, size_t topic_count,
                         size_t limit, bool has_from, int64_t from_ms, const char* before_id,
                         int64_t now_ms, Arena* arena, Cluster** clusters,
                         size_t* count, bool* found, int64_t* last_ingest_ms) const = 0;
 protected:
  ~NewsRepository() = default;
};
class StoredNewsRepository final : public NewsRepository {
 public:
  explicit StoredNewsRepository(NewsStore store) : store_(store) {}
  StoreStatus KnownArticleKeys(const char* const* source_ids,
                             size_t source_count, int64_t since_ms, int64_t now_ms,
                             Arena* arena, U16** keys, size_t* key_count) const override;
  StoreStatus RecentClusters(int64_t since_ms, int64_t now_ms,
                           Arena* arena, Cluster** clusters, size_t* count) const override;
  StoreStatus Persist(Cluster* const* clusters, size_t count,
                    base::Clock& clock, Arena* arena) const override;
  StoreStatus Prune(int64_t before_ms, Arena* arena, size_t* removed) const override;
  StoreStatus Feed(const Topic* topics, size_t topic_count, size_t limit,
                 int64_t now_ms, Arena* arena, Cluster** clusters, size_t* count) const override;
  StoreStatus Item(const char* id, int64_t now_ms, Arena* arena,
                 Cluster* cluster, bool* found) const override;
  StoreStatus LastIngestAt(int64_t now_ms, bool* found, int64_t* at_ms) const override;
  StoreStatus FeedSnapshot(const Topic* topics, size_t topic_count,
                         size_t limit, bool has_from, int64_t from_ms, const char* before_id,
                         int64_t now_ms, Arena* arena, Cluster** clusters,
                         size_t* count, bool* found, int64_t* last_ingest_ms) const override;
 private:
  NewsStore store_;
};

// knownExternalIds(sourceIds, since): article keys of the named sources
// published at or after `since_ms`.
StoreStatus KnownArticleKeys(const NewsStore& store, const char* const* source_ids,
                             size_t source_count, int64_t since_ms, int64_t now_ms,
                             Arena* arena, U16** keys, size_t* key_count);

// recentClusters(since): unexpired stories newest first (no price join).
StoreStatus RecentClusters(const NewsStore& store, int64_t since_ms, int64_t now_ms,
                           Arena* arena, Cluster** clusters, size_t* count);

// persist(clusters), in batches of 32 per KV frame. `clock` (NowMs) is read
// once per story, as TypeScript's this.now() is.
StoreStatus Persist(const NewsStore& store, Cluster* const* clusters, size_t count,
                    base::Clock& clock, Arena* arena);

// prune(before): removes stories whose time-index entry is older than
// `before_ms`, ignoring expiry like TypeScript's page(…, -1, …).
StoreStatus Prune(const NewsStore& store, int64_t before_ms, Arena* arena, size_t* removed);

// feed({topics, limit}) with the change24hPct join. `limit` is clamped to
// 1..100 like KvNewsStore.feed.
StoreStatus Feed(const NewsStore& store, const Topic* topics, size_t topic_count, size_t limit,
                 int64_t now_ms, Arena* arena, Cluster** clusters, size_t* count);

// item(id) with the change24hPct join; *found false for unknown/expired.
StoreStatus Item(const NewsStore& store, const char* id, int64_t now_ms, Arena* arena,
                 Cluster* cluster, bool* found);

// lastIngestAt(): the newest unexpired update-index timestamp.
StoreStatus LastIngestAt(const NewsStore& store, int64_t now_ms, bool* found, int64_t* at_ms);

// NewsFacet.computeFeed's two reads (feed, lastIngestAt) as one snapshot:
// no Persist or Prune commits between them (see StoryLock in the .cpp).
StoreStatus FeedSnapshot(const NewsStore& store, const Topic* topics, size_t topic_count,
                         size_t limit, bool has_from, int64_t from_ms, const char* before_id,
                         int64_t now_ms, Arena* arena, Cluster** clusters,
                         size_t* count, bool* found, int64_t* last_ingest_ms);

// The story JSON codec, exposed for tests.
bool EncodeCluster(const Cluster& cluster, Arena* arena, const uint8_t** data, size_t* length);
bool DecodeCluster(const uint8_t* data, size_t length, Arena* arena, Cluster* cluster);

}  // namespace fortis::news
