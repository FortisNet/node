#pragma once

#include "clock.h"
#include "kv_store.h"
#include "ohlc_cache_policy.h"
#include "seams.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::discovery {

// The TypeScript stale-while-revalidate envelope for the discovery routes,
// on the durable runtime KV:
//  - one KV value per cache key: a fixed header {freshUntil, staleUntil,
//    asOf} and the cached JSON value `d`, serialized as JSON.stringify would;
//  - freshness is jittered by the coordinator's ±10 % rule at store time and
//    the KV entry expires after min(fresh + stale, 7 days), the TypeScript
//    volatile-ttl bound, so short-lived discovery data never outlives a week;
//  - the in-process revalidation backoff and cold-miss replay holds of
//    SwrCache, which a restart forgets exactly like the TypeScript maps.
// The cache is not thread-safe; each route serializes it with its own mutex,
// which also owns the scratch buffer reads copy into.
struct DiscoveryCacheTtl {
  int64_t fresh_ms;
  int64_t stale_ms;  // serve-stale window after freshness ends
};

// The TypeScript TTL table entries used by the discovery routes.
constexpr DiscoveryCacheTtl kDiscoveryTtlTokenMarket{60000, 900000};
constexpr DiscoveryCacheTtl kDiscoveryTtlTokenIdentity{21600000, 604800000};
constexpr DiscoveryCacheTtl kDiscoveryTtlSearch{300000, 3600000};
constexpr DiscoveryCacheTtl kDiscoveryTtlTaostats{15000, 300000};
constexpr int64_t kDiscoveryKvMaxTtlMs = 7LL * 24 * 3600000;

enum class DiscoveryCacheState : uint8_t { kFresh, kStale, kMissing, kError };

struct DiscoveryCacheEntry {
  DiscoveryCacheState state = DiscoveryCacheState::kMissing;
  int64_t as_of_ms = 0;
  int64_t fresh_until_ms = 0;
  int64_t stale_until_ms = 0;
  const unsigned char* body = nullptr;  // points into the cache scratch
  size_t body_length = 0;
};

// Clock and jitter seams. A normal composition leaves both null: the wall
// clock and CSPRNG jitter. Only a test main fills them. A clock whose NowMs
// is negative also means the wall clock.
struct DiscoveryCacheSeams {
  fortis::base::Clock* clock = nullptr;
  fortis::base::JitterSource* jitter = nullptr;
};

class DiscoveryCache {
 public:
  DiscoveryCache(fortis::storage::KvStore* kv, unsigned char* scratch, size_t scratch_capacity,
                 const DiscoveryCacheSeams* seams);
  int64_t Now() const;
  // Reads `key`. kMissing covers an absent, expired or past-stale entry;
  // kError a KV I/O failure or an unreadable value (treated as a miss by the
  // routes, as TypeScript's readEnvelope catch does).
  DiscoveryCacheEntry Read(const char* key, int64_t now_ms);
  // SwrCache.store(): jittered freshness, KV TTL bound. On success *as_of_ms
  // is the stored asOf. A KV write failure returns false; TypeScript logs it
  // and still answers the fetched value, which the routes replicate.
  bool Write(const char* key, const DiscoveryCacheTtl& ttl, int64_t now_ms,
             const void* body, size_t body_length, int64_t* as_of_ms);
  // SwrCache maps: revalidation backoff and cold-miss replay holds.
  bool AllowRevalidate(const char* key, int64_t now_ms);
  void RecordRevalidateFailure(const char* key, int64_t now_ms, bool lane_busy);
  bool MissReplay(const char* key, int64_t now_ms, int* status);
  void RecordMissFailure(const char* key, int64_t now_ms, bool lane_busy, int status);
  size_t scratch_capacity() const { return capacity_; }
  fortis::storage::KvStore* kv() const { return kv_; }

 private:
  void PolicyKey(const char* key, char output[kOhlcCachePolicyKeyBytes]) const;
  double JitterSample() const;
  fortis::storage::KvStore* kv_;
  unsigned char* scratch_;
  size_t capacity_;
  const DiscoveryCacheSeams* seams_;
  OhlcCachePolicy policy_;
};

// "2026-01-02T03:04:05.000Z" (Date.prototype.toISOString) for epoch ms.
bool DiscoveryIsoTimestamp(int64_t epoch_ms, char output[32]);

}  // namespace fortis::discovery
