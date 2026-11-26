#include "ohlc_cache_policy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace fortis::discovery;

int main() {
  // jitteredTtl parity vectors evaluated with the TypeScript expression in Node:
  // Math.max(1, Math.round(ms * (1 + pct * (2 * rand - 1)))).
  struct Vector { int64_t ms; double rand; double pct; int64_t expected; };
  const Vector vectors[] = {
    {300000, 0, 0.1, 270000}, {300000, 1, 0.1, 330000}, {300000, 0.5, 0.1, 300000},
    {300000, 0.25, 0.1, 285000}, {300000, 0.123456789, 0.1, 277407},
    {300000, 0.987654321, 0.1, 329259}, {300000, 0.3333333333333333, 0.1, 290000},
    {5, 0, 0.1, 5}, {5, 1, 0.1, 6}, {1, 0, 0.1, 1}, {1, 1, 0.1, 1},
    {3600000, 0.7071067811865476, 0.1, 3749117},
    {300000, 0.5, 0, 300000}, {300000, 0, 0.5, 150000}, {300000, 1, 0.5, 450000},
    {300000, 0, 0.9, 150000},   // clampPct caps the percentage at 0.5
    {300000, 1, -1, 300000},    // non-positive percentage disables jitter
    {300000, 2, 0.1, 330000},   // out-of-range samples clamp to [0, 1]
  };
  for (const Vector& vector : vectors)
    assert(OhlcJitteredFreshMs(vector.ms, vector.rand, vector.pct) == vector.expected);
  assert(OhlcJitteredFreshMs(kOhlcCacheFreshMs, 0) == 270000);
  assert(OhlcJitteredFreshMs(kOhlcCacheFreshMs, 1) == 330000);

  // Revalidation backoff: 30 s after a failure, 5 s when the lane shed it.
  OhlcCachePolicy policy;
  const char* key = "ohlc:v1:backoff-coin:usd:1";
  const char* other = "ohlc:v1:other-coin:usd:1";
  assert(policy.AllowRevalidate(key, 1000));
  assert(policy.entry_count() == 0);
  policy.RecordRevalidateFailure(key, 1000, false);
  assert(policy.entry_count() == 1);
  assert(!policy.AllowRevalidate(key, 1000));
  assert(!policy.AllowRevalidate(key, 30999));
  assert(policy.AllowRevalidate(other, 1000));
  assert(policy.entry_count() == 1);
  assert(policy.AllowRevalidate(key, 31000));
  assert(policy.entry_count() == 0);
  policy.RecordRevalidateFailure(key, 31000, true);
  assert(!policy.AllowRevalidate(key, 35999));
  assert(policy.AllowRevalidate(key, 36000));

  // Miss replay: 10 s after a failure, 5 s when shed; the status is replayed.
  int status = 0;
  assert(!policy.MissReplay(key, 50000, &status));
  policy.RecordMissFailure(key, 50000, false, 502);
  assert(policy.MissReplay(key, 50000, &status) && status == 502);
  status = 0;
  assert(policy.MissReplay(key, 59999, &status) && status == 502);
  assert(policy.AllowRevalidate(key, 59999));  // the windows are independent
  assert(!policy.MissReplay(key, 60000, &status));
  assert(policy.entry_count() == 0);
  policy.RecordMissFailure(key, 60000, true, 503);
  assert(policy.MissReplay(key, 64999, &status) && status == 503);
  assert(!policy.MissReplay(key, 65000, &status));
  assert(policy.entry_count() == 0);

  // Both windows on one key share an entry; the entry lives while either holds.
  policy.RecordMissFailure(key, 70000, false, 502);
  policy.RecordRevalidateFailure(key, 70000, false);
  assert(policy.entry_count() == 1);
  assert(!policy.AllowRevalidate(key, 70000));
  assert(policy.MissReplay(key, 70000, &status) && status == 502);
  assert(!policy.MissReplay(key, 80000, &status));
  assert(policy.entry_count() == 1);
  assert(!policy.AllowRevalidate(key, 80000));
  policy.Invalidate(key);
  assert(policy.entry_count() == 0);
  assert(policy.AllowRevalidate(key, 80000));

  // Invalid keys never hold state and never block.
  char long_key[kOhlcCachePolicyKeyBytes + 8];
  memset(long_key, 'a', sizeof(long_key) - 1);
  long_key[sizeof(long_key) - 1] = 0;
  policy.RecordMissFailure(long_key, 1, false, 502);
  policy.RecordRevalidateFailure("", 1, false);
  policy.RecordRevalidateFailure(nullptr, 1, false);
  assert(policy.entry_count() == 0);
  assert(!policy.MissReplay(long_key, 1, &status));
  assert(policy.AllowRevalidate(nullptr, 1));
  char max_key[kOhlcCachePolicyKeyBytes];
  memset(max_key, 'b', sizeof(max_key) - 1);
  max_key[sizeof(max_key) - 1] = 0;
  policy.RecordMissFailure(max_key, 1, false, 502);
  assert(policy.entry_count() == 1 && policy.MissReplay(max_key, 1, &status));
  policy.Invalidate(max_key);

  // Bound: a full table evicts the oldest write; updating a key makes it newest.
  static char keys[kOhlcCachePolicyEntries + 1][40];
  for (size_t i = 0; i <= kOhlcCachePolicyEntries; ++i)
    snprintf(keys[i], sizeof(keys[i]), "ohlc:v1:coin%zu:usd:1", i);
  static OhlcCachePolicy bounded;
  for (size_t i = 0; i < kOhlcCachePolicyEntries; ++i)
    bounded.RecordRevalidateFailure(keys[i], 100000, false);
  assert(bounded.entry_count() == kOhlcCachePolicyEntries);
  bounded.RecordRevalidateFailure(keys[0], 100001, false);
  bounded.RecordMissFailure(keys[kOhlcCachePolicyEntries], 100001, false, 502);
  assert(bounded.entry_count() == kOhlcCachePolicyEntries);
  assert(!bounded.AllowRevalidate(keys[0], 100001));
  assert(bounded.AllowRevalidate(keys[1], 100001));  // evicted: oldest write
  assert(!bounded.AllowRevalidate(keys[2], 100001));
  assert(bounded.MissReplay(keys[kOhlcCachePolicyEntries], 100001, &status));

  // A fully expired entry is reclaimed before a live one is evicted.
  static OhlcCachePolicy reclaim;
  for (size_t i = 0; i + 1 < kOhlcCachePolicyEntries; ++i)
    reclaim.RecordRevalidateFailure(keys[i], 0, false);  // held until 30 s
  reclaim.RecordRevalidateFailure(keys[kOhlcCachePolicyEntries - 1], 40000, false);
  reclaim.RecordMissFailure(keys[kOhlcCachePolicyEntries], 50000, false, 502);
  assert(reclaim.entry_count() == kOhlcCachePolicyEntries);
  assert(!reclaim.AllowRevalidate(keys[kOhlcCachePolicyEntries - 1], 50000));
  assert(reclaim.MissReplay(keys[kOhlcCachePolicyEntries], 50000, &status));
  size_t live = 0;
  for (size_t i = 0; i + 1 < kOhlcCachePolicyEntries; ++i)
    if (!reclaim.AllowRevalidate(keys[i], 29999)) ++live;
  assert(live == kOhlcCachePolicyEntries - 2);  // exactly one expired entry was reclaimed

  puts("ohlc cache policy: jitter vectors, backoff, replay, isolation, and bounds passed");
  return 0;
}
