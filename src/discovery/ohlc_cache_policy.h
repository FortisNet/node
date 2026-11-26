#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fortis::discovery {

// In-process failure policy for the gated CoinGecko `/v1/prices/ohlc` cache.
// It mirrors the TypeScript stale-while-revalidate cache:
//  - freshness is jittered by the coordinator's ±10 % rule at store time;
//  - a failed background revalidation holds further attempts for 30 s, or 5 s
//    when the provider lane shed the work (REVALIDATE_*_BACKOFF_MS);
//  - a failed cold miss replays its failure for 10 s, or 5 s when shed, so
//    later misses do not queue for the provider again (MISS_*_HOLD_MS).
// Like the TypeScript maps the state is never persisted: a process restart
// forgets every hold. The table is fixed-size; TypeScript caps each map at
// 10,000 keys and evicts its oldest insertion, this table holds 256 keys and
// reclaims a fully expired entry before it evicts the oldest write. The
// caller serializes access; the route holds its cache mutex around every call.
constexpr size_t kOhlcCachePolicyEntries = 256;
constexpr size_t kOhlcCachePolicyKeyBytes = 160;
constexpr int64_t kOhlcCacheFreshMs = 300000;     // TTL.chart.freshMs
constexpr int64_t kOhlcCacheStaleMs = 3600000;    // TTL.chart.staleMs
constexpr double kOhlcCacheTtlJitterPct = 0.1;    // COORD_TTL_JITTER_PCT default
constexpr int64_t kOhlcRevalidateFailureBackoffMs = 30000;
constexpr int64_t kOhlcRevalidateBusyBackoffMs = 5000;
constexpr int64_t kOhlcMissFailureHoldMs = 10000;
constexpr int64_t kOhlcMissBusyHoldMs = 5000;

// LocalCoordinator.jitteredTtl: `Math.max(1, Math.round(ms * (1 + pct * (2 * rand - 1))))`
// with clampPct (non-positive disables, capped at 0.5). `rand` is clamped to [0, 1].
int64_t OhlcJitteredFreshMs(int64_t fresh_ms, double rand,
                            double jitter_pct = kOhlcCacheTtlJitterPct);

class OhlcCachePolicy {
 public:
  OhlcCachePolicy();
  OhlcCachePolicy(const OhlcCachePolicy&) = delete;
  OhlcCachePolicy& operator=(const OhlcCachePolicy&) = delete;

  // True when a stale read may start a background revalidation now. An
  // expired backoff is cleared here, exactly like SwrCache.revalidate().
  bool AllowRevalidate(const char* key, int64_t now_ms);
  void RecordRevalidateFailure(const char* key, int64_t now_ms, bool lane_busy);
  // True while a recent cold-miss failure is replayed; `status` receives the
  // HTTP status that failure produced. An expired hold is cleared here.
  bool MissReplay(const char* key, int64_t now_ms, int* status);
  void RecordMissFailure(const char* key, int64_t now_ms, bool lane_busy, int status);
  // SwrCache.invalidate(): drops both windows for the key.
  void Invalidate(const char* key);
  size_t entry_count() const;

 private:
  struct Entry {
    bool used = false;
    uint64_t sequence = 0;
    int64_t backoff_until_ms = 0;  // zero: no revalidation backoff
    int64_t miss_until_ms = 0;     // zero: no miss replay
    int miss_status = 0;
    char key[kOhlcCachePolicyKeyBytes] = {};
  };
  static bool ValidKey(const char* key);
  Entry* Find(const char* key);
  Entry* Upsert(const char* key, int64_t now_ms);
  void ReleaseIfIdle(Entry* entry);
  Entry entries_[kOhlcCachePolicyEntries];
  uint64_t next_sequence_ = 1;
};

}  // namespace fortis::discovery
