#include "token_writes.h"

#include "kv_store.h"

namespace tj = fortis::codec::tj;

namespace fortis::tokens::token_writes {
namespace {

using token_data::TokenDataStore;

bool commit(storage::KvBatch& batch, storage::KvStore* kv, FrameStats* stats) {
  const size_t mutations = batch.count();
  const size_t bytes = batch.FrameBytes();
  if (stats && batch.allocated() > stats->max_staging_bytes) stats->max_staging_bytes = batch.allocated();
  if (!mutations) return true;
  if (!batch.Commit(kv)) {
    if (stats) ++stats->failed_frames;
    return false;
  }
  if (stats) {
    ++stats->frames;
    stats->mutations += mutations;
    stats->bytes += bytes;
    if (bytes > stats->max_frame_bytes) stats->max_frame_bytes = bytes;
  }
  return true;
}

class DataLock {
 public:
  explicit DataLock(TokenDataStore& data) : data_(data) { data_.Lock(); }
  ~DataLock() { data_.Unlock(); }
  DataLock(const DataLock&) = delete;
  DataLock& operator=(const DataLock&) = delete;

 private:
  TokenDataStore& data_;
};

// Both store locks for one transaction in the documented order: members are
// constructed in declaration order and destroyed in reverse.
class Locks {
 public:
  explicit Locks(TokenDataStore& data) : data_(data) {}

 private:
  DataLock data_;
  TokenComparisonStore::WriterLock writer_;
};

}  // namespace

bool RecordLiquidity(TokenDataStore& data, const LiquidityOutcome& outcome, int64_t now_ms, FrameStats* stats) {
  if (!data.valid() || !data.kv() || !outcome.network || !outcome.token_id || !outcome.facts ||
      !outcome.comparison)
    return false;
  storage::KvBatch batch(kLiquidityFrameBytes, 8);
  Locks locks(data);
  TokenComparisonStore comparison(data.kv());
  // Staged in the TypeScript order: facts, comparison, observation + pair.
  const bool staged =
      data.PrepareFacts(batch, outcome.network, outcome.token_id, outcome.facts, now_ms) &&
      comparison.PrepareComparison(batch, outcome.network, outcome.token_id, *outcome.comparison, now_ms) &&
      (!outcome.observation ||
       data.PrepareObservation(batch, outcome.network, outcome.token_id, *outcome.observation, now_ms));
  return staged && commit(batch, data.kv(), stats);
}

bool WriteThrough(TokenDataStore& data, const char* network, const char* token_id, const tj::Value* facts,
                  const token_store::ComparisonPatch* patch, int64_t now_ms, FrameStats* stats) {
  if (!data.valid() || !data.kv() || !network || !token_id || !facts) return false;
  storage::KvBatch batch(kLiquidityFrameBytes, 4);
  Locks locks(data);
  if (!data.PrepareFacts(batch, network, token_id, facts, now_ms)) return false;
  bool both = true;
  if (patch) {
    const storage::KvBatch::Mark mark = batch.Position();
    TokenComparisonStore comparison(data.kv());
    if (!comparison.PrepareComparison(batch, network, token_id, *patch, now_ms)) {
      batch.Rollback(mark);
      both = false;
    }
  }
  return commit(batch, data.kv(), stats) && both;
}

size_t RecordMarketRefreshes(TokenDataStore& data, MarketRefresh* items, size_t count, int64_t now_ms,
                             FrameStats* stats) {
  if (!items && count) return 0;
  for (size_t i = 0; i < count; ++i) items[i].committed = false;
  if (!data.valid() || !data.kv() || !items) return 0;
  storage::KvBatch batch(kMarketFrameBytes, kMarketChunk * 3);
  TokenComparisonStore comparison(data.kv());
  size_t committed = 0;
  bool failed = false;
  for (size_t start = 0; start < count && !failed; start += kMarketChunk) {
    const size_t end = count - start > kMarketChunk ? start + kMarketChunk : count;
    Locks locks(data);
    batch.Clear();
    size_t pending_from = start;  // items in [pending_from, i) staged when ok
    // Marks which items of the current frame were staged (vs. skipped).
    bool in_frame[kMarketChunk]{};
    auto flush = [&](size_t upto) {
      const bool ok = commit(batch, data.kv(), stats);
      for (size_t k = pending_from; k < upto; ++k) {
        if (!in_frame[k - start]) continue;
        items[k].committed = ok;
        if (ok) ++committed;
      }
      pending_from = upto;
      return ok;
    };
    for (size_t i = start; i < end; ++i) {
      MarketRefresh& item = items[i];
      auto stage = [&]() {
        return item.network && item.token_id && item.facts && item.comparison && item.demand &&
               data.PrepareFacts(batch, item.network, item.token_id, item.facts, now_ms) &&
               comparison.PrepareComparison(batch, item.network, item.token_id, *item.comparison, now_ms) &&
               data.PrepareDemand(batch, item.network, item.token_id, item.demand, now_ms);
      };
      storage::KvBatch::Mark mark = batch.Position();
      bool ok = stage();
      if (!ok && batch.full() && mark.count > 0) {
        // The frame bound: commit the tokens staged so far, then retry this
        // token's group alone in an empty batch.
        batch.Rollback(mark);
        if (stats) ++stats->splits;
        if (!flush(i)) { failed = true; break; }
        mark = batch.Position();
        ok = stage();
      }
      if (!ok) {
        batch.Rollback(mark);
        continue;
      }
      in_frame[i - start] = true;
    }
    if (!failed && !flush(end)) failed = true;
  }
  return committed;
}

}  // namespace fortis::tokens::token_writes
