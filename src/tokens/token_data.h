#include "threading.h"
#include "ownership.h"
#pragma once

#include "token_json.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

namespace fortis::storage { class KvStore; class KvBatch; }

// Durable token data for the token routes and jobs, on the runtime KV.
// TypeScript keeps one LMDB row per sample (its token, liquidity
// observation, liquidity candle, holder history and fetch statistics
// stores). The native KV keeps every key in memory and the guest allocator
// charges at least one page per key, so time series are grouped into
// bounded blocks instead:
//
//   token/facts/{net}/{id}                JSON facts document (TokenStore 'facts')
//   token/demand/{net}/{id}               JSON demand row (TokenStore 'demand')
//   token/fetch/{hour13}/{shard}          JSON fetch-stat rows of one hour (8 shards)
//   token/liqraw/{net}/{id}/{start13}     6-hour block of raw observations
//   token/liqpair/{net}/{id}              last observation time (recentPairs)
//   token/liqcandle/{net}/{id}/{iv}/{start13}  1h: 4-day, 1d: 32-day, 1w: 182-day blocks
//   token/holder/{net}/{id}/{source}/{start13} 32-day block of daily snapshots
//
// {net}/{id}/{source} are encodeURIComponent'd; numbers are zero-padded to 13
// digits so key order is time order, as in TypeScript. Dates are epoch ms.
// Every public write is one KV frame (an observation's block and pair row
// share it); read-modify-writes serialize on one store mutex. Prepare*
// stage the same documents into a caller's KvBatch so token_writes.h can
// commit facts, comparison and observation rows of one logical update as a
// single frame. Retention matches TypeScript at row granularity on read (an aged
// row inside a live block is filtered) and at block granularity on disk
// (a block expires when its newest possible row would).
namespace fortis::tokens::token_data {

constexpr size_t kMaxDocumentBytes = 256 * 1024;
constexpr int64_t kRawRetentionMs = 7LL * 86400000;
constexpr int64_t kRawBlockMs = 6LL * 3600000;
constexpr int64_t kHolderRetainMs = 400LL * 86400000;
constexpr int64_t kHolderBlockMs = 32LL * 86400000;
constexpr int64_t kFetchRetentionMs = 7LL * 86400000;
constexpr size_t kFetchShards = 8;
constexpr size_t kFetchRowsPerShard = 512;
// KvBatch bounds for the single-update writers: one document plus its key,
// and an observation block plus its pair row.
constexpr size_t kSingleDocumentFrameBytes = kMaxDocumentBytes + 2048;
constexpr size_t kObservationFrameBytes = 2 * kMaxDocumentBytes + 4096;

enum class DataOutcome : uint8_t { kOk, kMissing, kIoError, kCorrupt, kTooLarge };

// One liquidity observation (LiquidityObservation, numbers already Number()).
struct Observation {
  int64_t ts = 0;
  bool ok = true;  // status 'ok' | 'not_found'
  char source[24]{};
  bool has_sampling = false;
  double sampling_seconds = 0;
  bool has_liquidity = false, has_price = false, has_spread = false, has_spread_pools = false,
       has_top_share = false, has_fdv = false, has_market_cap = false, has_volume_h24 = false,
       has_buys_h24 = false, has_sells_h24 = false;
  double liquidity = 0, price = 0, spread = 0, spread_pools = 0, top_share = 0, fdv = 0,
         market_cap = 0, volume_h24 = 0, buys_h24 = 0, sells_h24 = 0;
  double pool_count = 0;
  // Epoch ms the row expires (max(arrival, ts) + 7 days); set by PutObservation.
  int64_t expires_at = 0;
};

enum class Interval : uint8_t { k1h, k1d, k1w };
const char* IntervalName(Interval interval);

struct Candle {
  int64_t ts = 0;
  // OHLC and gauges; has[] marks the non-null values in field order:
  // liquidityOpen, High, Low, Close, priceOpen, High, Low, Close,
  // spreadClosePct, spreadMaxPct, topPoolShareClose, topPoolShareMax,
  // poolCountOpen, poolCountClose, volume24hClose, buys24hClose, sells24hClose.
  static constexpr size_t kFields = 17;
  bool has[kFields]{};
  double value[kFields]{};
  double sample_count = 0;
  double expected_samples = 0;
  char source[24]{};
};

// Caller-owned bounded output for range reads.
template <typename T>
struct Rows {
  T* items = nullptr;
  size_t capacity = 0;
  size_t count = 0;
  bool truncated = false;
};

class TokenDataStore {
 public:
  explicit TokenDataStore(storage::KvStore* kv);
  ~TokenDataStore();
  TokenDataStore(const TokenDataStore&) = delete;
  TokenDataStore& operator=(const TokenDataStore&) = delete;
  bool valid() const { return scratch_ != nullptr; }
  storage::KvStore* kv() const { return kv_; }

  // ---- TokenStore 'facts' / 'demand' -------------------------------------
  // The stored document (an object) parsed into `arena`, or kMissing.
  DataOutcome ReadFacts(const char* network, const char* token_id, int64_t now_ms, codec::tj::Arena& arena,
                   codec::tj::Value** document);
  // {...prior, ...fields, network, tokenId}. False on a store failure.
  bool PatchFacts(const char* network, const char* token_id, const codec::tj::Value* fields, int64_t now_ms);
  DataOutcome ReadDemand(const char* network, const char* token_id, int64_t now_ms, codec::tj::Arena& arena,
                    codec::tj::Value** document);
  bool PatchDemand(const char* network, const char* token_id, const codec::tj::Value* fields, int64_t now_ms);
  // Visits every demand row in key order (forEach('demand')); the visitor
  // gets a document parsed into its own arena slice. Stops on false.
  using DemandVisitor = bool (*)(void* context, const codec::tj::Value* row);
  bool ForEachDemand(int64_t now_ms, codec::tj::Arena& arena, DemandVisitor visitor, void* context);
  // Up to kDemandPatchBatch patchInTransaction('demand', ...) calls in one
  // KV frame ({...prior, ...fields, network, tokenId}); a batch whose
  // serialized rows exceed kMaxDocumentBytes is split into further frames.
  // `frames` (optional) counts the frames written. False on a store failure;
  // rows of frames already written stay written.
  static constexpr size_t kDemandPatchBatch = 128;
  bool PatchDemandMany(const char* const* networks, const char* const* token_ids, size_t count,
                       const codec::tj::Value* fields, int64_t now_ms, size_t* frames = nullptr);
  // replaceDemand(rows, at): writes rows (keeping each prior lastRefreshedAt)
  // and deletes rows whose computedAt is not `at`.
  bool ReplaceDemand(codec::tj::Value* const* rows, size_t count, int64_t at_ms, int64_t now_ms);

  // ---- liquidity observations -------------------------------------------
  // putInTransaction for one observation: its block and the liqpair row in
  // one frame; samplingIntervalSeconds keeps a prior value when absent (the
  // TypeScript merge).
  bool PutObservation(const char* network, const char* token_id, const Observation& row, int64_t now_ms);
  // range(network, tokenId, from, to): rows with from <= ts <= to in order
  // (every block of the range, paged; `rows->truncated` past capacity).
  DataOutcome Observations(const char* network, const char* token_id, int64_t from_ms, int64_t to_ms,
                      int64_t now_ms, Rows<Observation>* rows);
  // latest(network, tokenId, before, match): the newest live row with
  // ts <= before that `match` accepts, scanning blocks newest first.
  DataOutcome LatestObservation(const char* network, const char* token_id, int64_t before_ms, int64_t now_ms,
                           bool (*match)(const Observation&), Observation* out, bool* found);
  // recentPairs(since) into caller storage ("{net}\0{id}" pairs, NUL-split).
  struct Pair { char network[128]; char token_id[512]; };
  DataOutcome RecentPairs(int64_t since_ms, int64_t now_ms, Rows<Pair>* pairs);

  // ---- liquidity candles ---------------------------------------------------
  // putMany for one token/interval: rewrites only blocks with a changed
  // candle. *changed counts candles that differed.
  bool PutCandles(const char* network, const char* token_id, Interval interval, const Candle* candles,
                  size_t count, int64_t now_ms, size_t* changed);
  DataOutcome Candles(const char* network, const char* token_id, Interval interval, int64_t from_ms,
                 int64_t to_ms, int64_t now_ms, Rows<Candle>* rows);

  // ---- holder snapshots ------------------------------------------------------
  // The rows of one token (every source) parsed into `arena`, in key order
  // (source, then day): an array of SnapshotRow objects with ms dates.
  DataOutcome HolderRows(const char* network, const char* token_id, int64_t now_ms, codec::tj::Arena& arena,
                    codec::tj::Value** rows);
  // Replaces (or inserts) the row of `source` and `day_ms` in its block.
  bool PutHolderRow(const char* network, const char* token_id, const char* source, int64_t day_ms,
                    const codec::tj::Value* row, int64_t now_ms);
  // Reads one row (null when absent).
  DataOutcome HolderRow(const char* network, const char* token_id, const char* source, int64_t day_ms,
                   int64_t now_ms, codec::tj::Arena& arena, codec::tj::Value** row);

  // ---- token fetch stats -----------------------------------------------------
  // TokenFetchStore.upsert: merge rows (objects with bucket/network/tokenId/
  // facet and counters) into their hour/shard blocks. Rows beyond the
  // per-shard bound are dropped and counted.
  bool UpsertFetchRows(codec::tj::Value* const* rows, size_t count, int64_t now_ms, size_t* dropped);
  using FetchVisitor = bool (*)(void* context, const codec::tj::Value* row);
  // forEach(from, to): rows with from <= bucket <= to, hour-ordered.
  bool ForEachFetchRow(int64_t from_ms, int64_t to_ms, int64_t now_ms, codec::tj::Arena& arena,
                       FetchVisitor visitor, void* context);

  // Deletes one plain KV key (SWR invalidation).
  bool Erase(const char* key);

  // ---- transactions (token_writes.h) ----------------------------------------
  // Lock()/Unlock() take the mutex every read-modify-write above holds.
  // Lock order: this store, then TokenComparisonStore::WriterLock, then the
  // KvStore's own mutex; never the reverse. Prepare* require Lock() held and
  // stage the document(s) the matching public writer would commit into
  // `batch` instead, reading prior values through the batch so staged rows
  // of the same frame are seen. False on invalid input, a store read
  // failure, or batch.full() (nothing is staged for a false return only
  // when the caller rolls back to its own KvBatch::Mark).
  void Lock();
  void Unlock();
  bool PrepareFacts(storage::KvBatch& batch, const char* network, const char* token_id, const codec::tj::Value* fields,
                    int64_t now_ms);
  bool PrepareDemand(storage::KvBatch& batch, const char* network, const char* token_id, const codec::tj::Value* fields,
                     int64_t now_ms);
  // The observation block and the liqpair row (two mutations).
  bool PrepareObservation(storage::KvBatch& batch, const char* network, const char* token_id, const Observation& row,
                          int64_t now_ms);

 private:
  DataOutcome ReadDocument(const char* key, int64_t now_ms, codec::tj::Arena& arena, codec::tj::Value** document,
                      const storage::KvBatch* batch = nullptr);
  bool WriteDocument(const char* key, const codec::tj::Value* document, int64_t expires_at_ms);
  bool StageDocument(storage::KvBatch& batch, const char* key, const codec::tj::Value* document, int64_t expires_at_ms);
  bool PreparePatchLocked(storage::KvBatch& batch, const char* bucket, const char* network, const char* token_id,
                          const codec::tj::Value* fields, int64_t now_ms);
  bool PatchDocument(const char* bucket, const char* network, const char* token_id,
                     const codec::tj::Value* fields, int64_t now_ms);

  storage::KvStore* kv_;
  fortis::base::Mutex mutex_;
  std::unique_ptr<uint8_t[], base::StorageDeleter> scratch_;       // KV reads under the mutex
  std::unique_ptr<uint8_t[], base::StorageDeleter> output_;        // serialized writes
  std::unique_ptr<uint8_t[], base::StorageDeleter> arena_memory_;  // read-modify-write arena
};

// encodeURIComponent of UTF-8 text into `out`; false when it does not fit.
bool AppendComponent(char* out, size_t capacity, size_t* used, const char* text);

}  // namespace fortis::tokens::token_data
