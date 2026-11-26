#pragma once

#include <memory>
#include "clock.h"
#include "ownership.h"
#include "status.h"

#include <stddef.h>
#include <stdint.h>

// TypeScript coin price popularity:
//  - the domain rules: two exponentially decayed counters per coin
//    (distinct devices, requests) with a 24-hour half-life, the ranking
//    score 4·log1p(users) + log1p(min(hits, 500)), the geometric bucket
//    tree ⌊log₂(rank/64 + 1)⌋ and its 1–64 minute refresh intervals, and the
//    seven-day expiry;
//  - the TypeScript CoinPopularity: the in-process tracker. A
//    device counts once per coin per UTC day through a set of 32-bit FNV-1a
//    hashes of `${device}\0${coin}` (UTF-16 code units, as charCodeAt), capped
//    at 300,000 pairs per day; at most 20,000 coins are kept (pruned in
//    batches once the table passes 24,000).
//
// Native bounds and representation (no per-coin allocation: the guest allocator
// spends a 4 KiB page per allocation):
//  - coin rows live in one growable array in insertion order (the order a
//    JavaScript Map iterates) with ids in one growable byte arena; a prune
//    compacts both. An id longer than kCoinPopularityMaxIdBytes after
//    normalization is not tracked (a native bound; route ids are at most
//    100 UTF-16 units);
//  - the daily pair set is a fixed open-addressing table of 2^19 slots
//    (2 MiB) allocated on the first device pair of a UTC day and freed when
//    the day changes, as TypeScript replaces its Set;
//  - restore stops growing at the hard row capacity and prunes to the coin
//    cap first (TypeScript keeps every restored row until the next record).
// Thread-safe: one mutex guards every call. Ids handed out by Due/TakeDirty/
// Snapshot are copies owned by the returned list.
namespace fortis::storage { class KvStore; }

namespace fortis::discovery {

constexpr int64_t kCoinPopularityHalfLifeMs = 24LL * 3600000;
constexpr int64_t kCoinPopularityExpireAfterMs = 7LL * 86400000;
constexpr size_t kCoinPopularityBucketBaseSize = 64;
constexpr int64_t kCoinPopularityBaseIntervalMs = 60000;
constexpr int64_t kCoinPopularityMaxIntervalMs = 64LL * 60000;
constexpr size_t kCoinPopularityDefaultMaxCoins = 20000;
constexpr size_t kCoinPopularityDefaultMaxSeenPerDay = 300000;
constexpr size_t kCoinPopularityMaxIdBytes = 1024;
// One record call names at most this many ids (both routes accept 100).
constexpr size_t kCoinPopularityMaxIdsPerRecord = 100;
constexpr size_t kCoinPopularitySeenSlots = 1u << 19;

struct CoinPopularityState {
  double users = 0;   // decayed distinct-device count
  double hits = 0;    // decayed request count
  int64_t updated_at_ms = 0;
  int64_t last_requested_at_ms = 0;
};

struct CoinIdView {
  const char* data;
  size_t size;
};

namespace coin_popularity {
// decayed(value, from, to).
double Decayed(double value, int64_t from_ms, int64_t to_ms);
// recordRequest(state, now, newUser); `state` may be null.
CoinPopularityState RecordRequest(const CoinPopularityState* state, int64_t now_ms, bool new_user);
double Score(const CoinPopularityState& state, int64_t now_ms);
bool IsExpired(const CoinPopularityState& state, int64_t now_ms);
// bucketForRank(rank) with the default base: ⌊log₂(max(0, rank)/64 + 1)⌋.
uint32_t BucketForRank(int64_t rank);
// intervalForBucket(bucket): 60 s · 2^bucket, capped at 64 minutes.
int64_t IntervalForBucket(int64_t bucket);
// fnv1a(`${device}\u0000${id}`) over UTF-16 code units of the UTF-8 inputs.
uint32_t PairHash(const char* device, size_t device_length, const char* id, size_t id_length);
// The tie-break order of ids: byte order (unsigned), a prefix first.
int IdCompare(const char* a, size_t a_length, const char* b, size_t b_length);
}  // namespace coin_popularity

struct CoinPopularityOptions {
  bool preserve_case = false;
  size_t max_coins = kCoinPopularityDefaultMaxCoins;
  size_t max_seen_per_day = kCoinPopularityDefaultMaxSeenPerDay;
  // Wall clock when null or when its NowMs is negative.
  base::Clock* clock = nullptr;
};

struct RankedCoin {
  const char* id;
  size_t id_length;
  size_t rank;
  uint32_t bucket;
  int64_t interval_ms;
};

struct PopularityRow {
  const char* id;
  size_t id_length;
  CoinPopularityState state;
  // Snapshot only: the in-memory refresh mark.
  bool has_refreshed_at;
  int64_t refreshed_at_ms;
};

// A allocated result: `items` and the ids they point to. Free with Release.
template <typename T>
struct CoinPopularityList {
  std::unique_ptr<T[], base::StorageDeleter> items;
  size_t count = 0;
  std::unique_ptr<char[], base::StorageDeleter> text;
  void Release();
};
using RankedCoinList = CoinPopularityList<RankedCoin>;
using PopularityRowList = CoinPopularityList<PopularityRow>;

struct CoinPopularityStats {
  size_t coins = 0;
  size_t row_capacity = 0;
  size_t id_bytes = 0;
  size_t id_capacity = 0;
  size_t index_slots = 0;
  size_t seen_pairs = 0;
  bool seen_allocated = false;
  size_t dropped = 0;  // ids not tracked (allocation failure or over-long)
  size_t allocated_bytes = 0;
  size_t peak_allocated_bytes = 0;
};

class CoinPopularity;

// Deleter for the std::unique_ptr returned by CoinPopularity::Create.
struct CoinPopularityDeleter {
  void operator()(CoinPopularity* popularity) const;
};

class CoinPopularity {
 public:
  // A tracker in its own allocation, or kResourceExhausted when there is no
  // memory for it.
  static base::Result<std::unique_ptr<CoinPopularity, CoinPopularityDeleter>> Create(const CoinPopularityOptions& options);
  static void Destroy(CoinPopularity* popularity);
  CoinPopularity(const CoinPopularity&) = delete;
  CoinPopularity& operator=(const CoinPopularity&) = delete;

  int64_t Now() const;
  // record(coinIds, deviceId): each id is trimmed and ASCII lower-cased;
  // empty ids are skipped. `device_id` null = unknown.
  void Record(const CoinIdView* ids, size_t count, const char* device_id);
  // PricesFacet.spot's foreground record: the CoinGecko ids among raw price
  // ids (parsePriceId kind 'coingecko'), de-duplicated by source id in
  // first-seen order. Invalid and other kinds are ignored.
  void RecordPriceIds(const CoinIdView* raw_ids, size_t count, const char* device_id);
  // due(budget): coins whose bucket interval has elapsed since their last
  // refresh, most popular first. False on allocation failure.
  bool Due(size_t budget, RankedCoinList* out, unsigned interval_divisor = 1, int64_t demand_max_age_ms = 0);
  void MarkRefreshed(const CoinIdView* ids, size_t count);
  // takeDirty(): states changed since the last call; clears the marks.
  bool TakeDirty(PopularityRowList* out);
  // Requeue a failed persistence snapshot without overwriting newer records.
  void RequeueDirty(const PopularityRow* rows, size_t count);
  // restore(rows): existing entries win; expired rows are skipped.
  void Restore(const PopularityRow* rows, size_t count);
  // prune(max): expired coins, then the least popular beyond `max`.
  void Prune(size_t max);
  void Prune();
  size_t Size() const;
  CoinPopularityStats Stats() const;
  // Every row in insertion order (tests and the fixture replay).
  bool Snapshot(PopularityRowList* out) const;

  struct Impl;

 private:
  CoinPopularity() noexcept;
  ~CoinPopularity();
  std::unique_ptr<Impl, base::ObjectDeleter<Impl>> impl_;
};

inline void CoinPopularityDeleter::operator()(CoinPopularity* popularity) const {
  CoinPopularity::Destroy(popularity);
}

// The seven-day durable ranking (as the TypeScript store kept it): one KV
// row per coin at `popularity/<encodeURIComponent(id)>`, expiring at
// lastRequestedAt + 7 days + 1 ms, written in transactions of 100 rows.
// Values are a fixed 34-byte record (magic, version, the four state fields
// as IEEE doubles / int64) instead of the TypeScript msgpack object.
namespace coin_popularity_store {
constexpr char kPrefix[] = "popularity/";
constexpr size_t kPageSize = 500;
constexpr size_t kSaveBatch = 100;
constexpr size_t kValueBytes = 34;

struct SaveResult {
  size_t rows = 0;
  size_t frames = 0;
  size_t bytes = 0;
  size_t skipped = 0;  // keys over the KV bound
  bool failed = false;
};
SaveResult Save(storage::KvStore& kv, const PopularityRow* rows, size_t count);
// load(now) + restore: pages of 500 rows; rows older than seven days are
// skipped. Returns the rows read (live ones) or SIZE_MAX on a read failure.
size_t LoadInto(storage::KvStore& kv, int64_t now_ms, CoinPopularity& popularity);
bool Encode(const CoinPopularityState& state, uint8_t output[kValueBytes]);
bool Decode(const void* value, size_t size, CoinPopularityState* state);
// "popularity/" + encodeURIComponent(id); false when over the KV key bound.
bool Key(const char* id, size_t id_length, char* output, size_t capacity);
}  // namespace coin_popularity_store

}  // namespace fortis::discovery
