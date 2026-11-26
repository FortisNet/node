#include "threading.h"
#pragma once

#include "clock.h"
#include "ohlc_cache_policy.h"
#include "seams.h"
#include "token_json.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

namespace fortis::storage { class KvStore; }

// The TypeScript stale-while-revalidate cache for the token facets, shared
// by every token worker:
//  - one KV value per key: a fixed binary header {freshUntil, staleUntil,
//    asOf} and the cached value `d` as JSON.stringify writes it;
//  - freshness jittered by the coordinator's +-10 % rule at store time and a
//    KV expiry of min(fresh + stale, 7 days);
//  - the in-process revalidation backoff, cold-miss failure hold and the
//    "revalidating" set (the cross-process lock of a single instance), all
//    under one mutex. A restart forgets them, exactly like the TS maps.
// The L1 memory tier is not ported: it only skips a KV read.
namespace fortis::tokens::token_swr {

struct Ttl {
  int64_t fresh_ms;
  int64_t stale_ms;
};

// The TypeScript cache TTL entries used by the token facets.
constexpr Ttl kTokenMarket{60000, 900000};
constexpr Ttl kTokenMarketActiveWarm{90000, 900000};
constexpr Ttl kTokenProfile{21600000, 604800000};
constexpr Ttl kTokenIdentity{21600000, 604800000};
constexpr Ttl kTokenLiquidity{360000, 3600000};
constexpr Ttl kTokenLiquidityDetail{60000, 0};
constexpr Ttl kTokenLiquidityTier1{1200000, 7200000};
constexpr Ttl kTokenHolders{100800000, 604800000};
constexpr Ttl kTokenChainMeta{3600000, 86400000};
constexpr Ttl kTokenHolderTrends{21600000, 604800000};
constexpr Ttl kTokenContract{21600000, 604800000};
constexpr Ttl kTokenNativeInsight{1800000, 21600000};
constexpr int64_t kKvMaxTtlMs = 7LL * 24 * 3600000;

enum class State : uint8_t { kFresh, kStale, kMissing };

// One stored envelope as written, without judging its freshness (test
// probes and diagnostics). `body` points into the caller's scratch.
struct EnvelopeInfo {
  int64_t as_of_ms = 0;
  int64_t fresh_until_ms = 0;
  int64_t stale_until_ms = 0;
  const uint8_t* body = nullptr;
  size_t body_length = 0;
};
// False when `key` is absent at `now_ms` (KV expiry) or unreadable.
bool PeekEnvelope(storage::KvStore* kv, const char* key, int64_t now_ms, uint8_t* scratch, size_t capacity,
                  EnvelopeInfo* out);

// Null members (or a negative NowMs) use the wall clock and CSPRNG jitter.
struct Seams {
  base::Clock* clock = nullptr;
  base::JitterSource* jitter = nullptr;
};

class Cache {
 public:
  Cache(storage::KvStore* kv, const Seams* seams);
  ~Cache() = default;
  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;

  int64_t Now() const;
  // Reads `key` into `scratch` and parses the value into `arena`. An absent,
  // past-stale or unreadable entry is kMissing (readEnvelope's catch).
  State Read(const char* key, int64_t now_ms, uint8_t* scratch, size_t capacity, codec::tj::Arena& arena,
             codec::tj::Value** value, int64_t* as_of_ms);
  // SwrCache.store(). False on a KV failure or an oversized value (the
  // caller still answers the fetched value, as TypeScript logs and continues).
  bool Write(const char* key, const Ttl& ttl, int64_t now_ms, const codec::tj::Value* value,
             uint8_t* scratch, size_t capacity, int64_t* as_of_ms);
  // SwrCache.invalidate(): the KV entry and both failure windows.
  bool Invalidate(const char* key);

  // revalidate(): true when this caller now owns the key's background
  // revalidation (not already running, not inside a failure backoff).
  bool BeginRevalidate(const char* key, int64_t now_ms);
  void EndRevalidate(const char* key, int64_t now_ms, bool failed, bool lane_busy);
  // missFailures: true while a recent cold-miss failure is replayed.
  bool MissHeld(const char* key, int64_t now_ms);
  void HoldMiss(const char* key, int64_t now_ms, bool lane_busy);
  // The same hold carrying the HTTP status the replayed error answers with
  // (TypeScript rethrows the stored error: 429 for a LaneBusyError).
  bool MissHeld(const char* key, int64_t now_ms, int* status);
  void HoldMiss(const char* key, int64_t now_ms, bool lane_busy, int status);

 private:
  void PolicyKey(const char* key, char output[discovery::kOhlcCachePolicyKeyBytes]) const;
  double JitterSample() const;
  storage::KvStore* kv_;
  const Seams* seams_;
  fortis::base::Mutex mutex_;
  discovery::OhlcCachePolicy policy_;
  static constexpr size_t kRevalidating = 64;
  char revalidating_[kRevalidating][discovery::kOhlcCachePolicyKeyBytes]{};
};

// A cache's Now() as a Clock (the wall clock its owner hands to components).
class CacheClock final : public base::Clock {
 public:
  explicit CacheClock(const Cache* cache) : cache_(cache) {}
  int64_t NowMs() override { return cache_->Now(); }

 private:
  const Cache* cache_;
};

}  // namespace fortis::tokens::token_swr
