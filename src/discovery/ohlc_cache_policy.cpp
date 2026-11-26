#include "ohlc_cache_policy.h"

#include <math.h>
#include <string.h>

namespace fortis::discovery {

int64_t OhlcJitteredFreshMs(int64_t fresh_ms, double rand, double jitter_pct) {
  // clampPct: a non-finite or non-positive percentage disables jitter and the
  // TypeScript method returns `ms` untouched (no Math.max floor on that path).
  if (!isfinite(jitter_pct) || jitter_pct <= 0) return fresh_ms;
  if (jitter_pct > 0.5) jitter_pct = 0.5;
  if (!isfinite(rand)) rand = 0.5;
  if (rand < 0) rand = 0;
  if (rand > 1) rand = 1;
  // Keep the JavaScript evaluation order so binary64 rounding matches.
  const double scaled = static_cast<double>(fresh_ms) * (1.0 + jitter_pct * (2.0 * rand - 1.0));
  // Math.round for a finite positive argument is floor(x + 0.5).
  const double rounded = floor(scaled + 0.5);
  if (!(rounded >= 1.0)) return 1;
  return static_cast<int64_t>(rounded);
}

OhlcCachePolicy::OhlcCachePolicy() = default;

bool OhlcCachePolicy::ValidKey(const char* key) {
  if (!key) return false;
  const size_t length = strnlen(key, kOhlcCachePolicyKeyBytes);
  return length > 0 && length < kOhlcCachePolicyKeyBytes;
}

OhlcCachePolicy::Entry* OhlcCachePolicy::Find(const char* key) {
  if (!ValidKey(key)) return nullptr;
  for (Entry& entry : entries_)
    if (entry.used && strcmp(entry.key, key) == 0) return &entry;
  return nullptr;
}

OhlcCachePolicy::Entry* OhlcCachePolicy::Upsert(const char* key, int64_t now_ms) {
  if (!ValidKey(key)) return nullptr;
  Entry* target = Find(key);
  if (!target) {
    Entry* oldest_expired = nullptr;
    Entry* oldest = nullptr;
    for (Entry& entry : entries_) {
      if (!entry.used) { target = &entry; break; }
      const bool expired = entry.backoff_until_ms <= now_ms && entry.miss_until_ms <= now_ms;
      if (expired && (!oldest_expired || entry.sequence < oldest_expired->sequence))
        oldest_expired = &entry;
      if (!oldest || entry.sequence < oldest->sequence) oldest = &entry;
    }
    // A full table reclaims an entry whose windows have both passed before it
    // evicts the oldest write (the TypeScript maps only do the latter).
    if (!target) target = oldest_expired ? oldest_expired : oldest;
    *target = Entry{};
    target->used = true;
    memcpy(target->key, key, strlen(key) + 1);
  }
  // Map delete-then-set semantics: a refreshed key becomes the newest entry.
  target->sequence = next_sequence_++;
  if (!next_sequence_) next_sequence_ = 1;
  return target;
}

void OhlcCachePolicy::ReleaseIfIdle(Entry* entry) {
  if (entry && entry->used && !entry->backoff_until_ms && !entry->miss_until_ms)
    *entry = Entry{};
}

bool OhlcCachePolicy::AllowRevalidate(const char* key, int64_t now_ms) {
  Entry* entry = Find(key);
  if (!entry || !entry->backoff_until_ms) return true;
  if (now_ms < entry->backoff_until_ms) return false;
  entry->backoff_until_ms = 0;
  ReleaseIfIdle(entry);
  return true;
}

void OhlcCachePolicy::RecordRevalidateFailure(const char* key, int64_t now_ms,
                                              bool lane_busy) {
  Entry* entry = Upsert(key, now_ms);
  if (!entry) return;
  entry->backoff_until_ms = now_ms +
      (lane_busy ? kOhlcRevalidateBusyBackoffMs : kOhlcRevalidateFailureBackoffMs);
}

bool OhlcCachePolicy::MissReplay(const char* key, int64_t now_ms, int* status) {
  Entry* entry = Find(key);
  if (!entry || !entry->miss_until_ms) return false;
  if (now_ms < entry->miss_until_ms) {
    if (status) *status = entry->miss_status;
    return true;
  }
  entry->miss_until_ms = 0;
  entry->miss_status = 0;
  ReleaseIfIdle(entry);
  return false;
}

void OhlcCachePolicy::RecordMissFailure(const char* key, int64_t now_ms, bool lane_busy,
                                        int status) {
  Entry* entry = Upsert(key, now_ms);
  if (!entry) return;
  entry->miss_until_ms = now_ms + (lane_busy ? kOhlcMissBusyHoldMs : kOhlcMissFailureHoldMs);
  entry->miss_status = status;
}

void OhlcCachePolicy::Invalidate(const char* key) {
  Entry* entry = Find(key);
  if (entry) *entry = Entry{};
}

size_t OhlcCachePolicy::entry_count() const {
  size_t count = 0;
  for (const Entry& entry : entries_) count += entry.used ? 1 : 0;
  return count;
}

}  // namespace fortis::discovery
