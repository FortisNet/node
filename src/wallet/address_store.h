#include "ownership.h"
#include "threading.h"
#pragma once

#include "status.h"
#include <optional>

#include "function_ref.h"

#include "address_records.h"
#include "chain_core.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

namespace fortis::storage { class KvStore; }

// The durable repositories of the address routes on the runtime KV, with the
// TypeScript stores' key grammar and JSON values (Date fields as epoch-ms
// numbers; no historic data migration is required):
//
//   account/id/{id} · account/pair/{network}/{address} ·
//   account/recent/{lastReadAt pad13}/{id}                (AccountStore)
//   balance/account/{accountId}                           (BalanceStore)
//   transfer/sync/{accountId}                             (TransferSyncStateStore)
//   transfer/row|cursor|time|token-time|age|retention|unpriced|
//            token-unpriced-time/…                        (TransferStore)
//   sync2/meta/head · sync2/account/{accountId}/{seq}     (AccountSyncLog append;
//                                                          sync_journal.h reads)
//   watch/device/{device}/{topic} · watch/account/{acct}/{device}/{topic}
//                                                         (WatchStore, 15 min expiry)
//
// Every path component is encodeURIComponent()'d. Each TypeScript
// `disk.write(tx => …)` transaction is one read-modify-write under the
// store's writer mutex committed as ONE KV frame (KvStore::Write), so a
// crash leaves either all of a transaction's rows and indexes or none.
// Readers do not take the mutex. Guest-safe.
namespace fortis::wallet::address {

constexpr size_t kAccountIdBytes = 26;  // ulid()

struct Account {
  char id[kAccountIdBytes + 1]{};
  char network[128]{};
  char address[512]{};
  int64_t first_read_at = 0;
  int64_t last_read_at = 0;
};

enum class ReadStatus : uint8_t { kFound, kMissing, kError };

struct PageOptions {
  const char* token_id = nullptr;  // time order: the token-time index
  bool has_from = false;
  double from_ms = 0;
  bool has_to = false;
  double to_ms = 0;
};

// Existing transfer high-water wire representation, bounded to 63 bytes.
struct TransferHighWater { char cursor[64]{}; };

using RowMatcher = base::FunctionRef<bool(const Value* row)>;

struct ValuationTarget {
  const char* account_id = nullptr;
  const char* record_id = nullptr;
  // Present when a candle priced the row.
  bool has_valuation = false;
  const char* fiat_at_time = nullptr;  // toFixed(24)
  const char* interval = nullptr;      // "1h" | "1d"
  const char* source_id = nullptr;
  int64_t source_ts = 0;
};

class Store {
 public:
  explicit Store(storage::KvStore* kv);
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  storage::KvStore* kv() const { return kv_; }

  // ---- generic JSON documents (facets with their own key grammar) --------
  // Reads `key` (expiry judged at now_ms) and parses it into `arena`.
  ReadStatus GetJson(const char* key, int64_t now_ms, Arena& arena, Value** value) const;
  // The writer lock for multi-key read-modify-write sequences of facets.
  void Lock() { mutex_.Lock(); }
  void Unlock() { mutex_.Unlock(); }

  // ---- accounts ----------------------------------------------------------
  bool Touch(const char* network, const char* address, int64_t now_ms, Account* out);
  ReadStatus Find(const char* network, const char* address, int64_t now_ms, Account* out) const;
  ReadStatus ById(const char* id, int64_t now_ms, Account* out) const;
  // recentlyRead(since, limit): newest-first. `out` holds `capacity` rows.
  bool RecentlyRead(int64_t since_ms, size_t limit, int64_t now_ms, Account* out, size_t capacity,
                    size_t* count) const;

  // ---- balance snapshots -------------------------------------------------
  ReadStatus GetBalance(const char* account_id, int64_t now_ms, Arena& arena, Value** payload,
                        int64_t* as_of_ms) const;
  bool PutBalance(const char* account_id, const Value* incoming, const char* const* requested,
                  size_t requested_count, int64_t as_of_ms, Arena& arena);

  // ---- transfer sync state -----------------------------------------------
  ReadStatus GetSyncState(const char* account_id, int64_t now_ms, Arena& arena, Value** state) const;
  // patch(accountId, fields): `fields` is an object whose members replace.
  bool PatchSyncState(const char* account_id, const Value* fields, int64_t now_ms, Arena& arena);

  // ---- transfers ---------------------------------------------------------
  // upsert(accountId, network, records): chunks of 200 per commit; `changed`
  // counts rows written (an unchanged app JSON is skipped).
  // upsert(accountId, network, records) in commits of 200 records. A new
  // record whose `ts` is not a valid Date (transferRowToAppJson's
  // toISOString throws RangeError) aborts its commit: earlier commits stay,
  // and *invalid_time is set.
  bool UpsertTransfers(const char* account_id, const Network& network, const Value* records,
                       int64_t now_ms, Arena& arena, size_t* changed, bool* invalid_time = nullptr);
  ReadStatus GetTransfer(const char* account_id, const char* record_id, int64_t now_ms, Arena& arena,
                         Value** row) const;
  // page(accountId, order, limit, after, matches, options) -> rows array.
  bool PageTransfers(const char* account_id, bool by_cursor, size_t limit, const char* after,
                     const PageOptions& options, RowMatcher matches, int64_t now_ms,
                     Arena& arena, Value** rows) const;
  // highWater(accountId): last cursor, empty optional when absent, status on failure.
  base::Result<std::optional<TransferHighWater>> HighWater(const char* account_id, int64_t now_ms) const;
  // recent(accountId, limit): newest first by time.
  bool RecentTransfers(const char* account_id, size_t limit, int64_t now_ms, Arena& arena, Value** rows) const;
  // unpricedForToken(accountId, tokenId, limit, dueBefore, matches).
  bool UnpricedForToken(const char* account_id, const char* token_id, size_t limit, int64_t due_before_ms,
                        RowMatcher matches, int64_t now_ms, Arena& arena, Value** rows) const;
  // forEachUnpriced(visit, dueBefore). The visitor gets a row in `arena`
  // (reset by the caller as it likes) and returns false to stop. It may
  // call this store (the KV scan is not held).
  using RowVisitor = base::FunctionRef<bool(const Value* row, Arena& arena)>;
  bool ForEachUnpriced(RowVisitor visit, bool has_due_before, int64_t due_before_ms,
                       int64_t now_ms, Arena& arena) const;
  bool RecordValuations(const ValuationTarget* targets, size_t count, int64_t at_ms, Arena& arena,
                        size_t* changed);
  // pruneTerminalBefore(before, limit, now): rows deleted.
  size_t PruneTerminalBefore(int64_t before_ms, size_t limit, int64_t now_ms, Arena& arena);

  // ---- sync journal (the TypeScript appendInTransaction) -----------------
  // The one journal writer is UpsertTransfers: each changed row and its
  // event share a KV frame. AppendSyncEvents commits `events` (an array of
  // {entityKey, operation, fields, changedAt (epoch ms)}) for `account_id` in
  // one frame with the same append path; commit=false builds the frame and
  // drops it (a rolled-back transaction: no sequence is consumed). `head`
  // receives the journal head after the call (20 digits).
  bool AppendSyncEvents(const char* account_id, const Value* events, int64_t now_ms, bool commit, Arena& arena,
                        char head[32]);

  // ---- watches (the TypeScript watch store and portfolio watches) --------
  bool RememberTopic(const char* device_id, const char* topic, int64_t now_ms, Arena& arena);
  bool Remember(const char* device_id, const char* account_id, const char* network, const char* address,
                const char* const* tracked, size_t tracked_count, int64_t now_ms, Arena& arena);
  // tokensForAccount(accountId): sorted unique token ids (array value).
  bool TokensForAccount(const char* account_id, int64_t now_ms, Arena& arena, Value** tokens) const;
  // listDevice(deviceId, now): the device's live watch records in key order.
  bool ListDeviceWatches(const char* device_id, int64_t now_ms, Arena& arena, Value** rows) const;
  // renew(deviceId, topics, now, WATCH_RENEW_BEFORE_MS, WATCH_TTL_MS): one
  // frame rewriting every live watch due within the renewal margin.
  bool RenewTopics(const char* device_id, const char* const* topics, size_t count, int64_t now_ms, Arena& arena);
  // WatchStore.put of one record with no tracked tokens (`account_id` may be
  // null). Not a topic write: no listener is told.
  bool WriteWatch(const char* device_id, const char* topic, const char* account_id, int64_t expires_at,
                  int64_t now_ms, Arena& arena);
  // PortfolioWatchService.onTopicWritten: told after rememberTopic() creates
  // a topic and after remember() changes one. Set once before serving; the
  // listener runs on the writer's thread and must not call this store.
  using TopicListener = void (*)(void* context, const char* device_id);
  void SetTopicListener(TopicListener listener, void* context) {
    topic_listener_ = listener;
    topic_listener_context_ = context;
  }

  // ---- shared helpers ----------------------------------------------------
  // encodeURIComponent of one key component (false when it cannot encode).
  static bool Component(const char* text, char* output, size_t capacity);
  // String(ms).padStart(13, '0').
  static void Pad13(int64_t ms, char output[24]);

 private:
  struct Mutations;
  bool UpsertChunk(const char* account_id, const Network& network, const Value* records, size_t begin,
                   size_t end, int64_t now_ms, Arena& arena, size_t* changed, bool* invalid_time);
  bool PutWatch(const char* device_id, const char* topic, const char* account_id, const Value* tokens,
                int64_t expires_at, int64_t now_ms, Arena& arena);
  // appendInTransaction: advances `head`, queues the event row (expiring
  // with RETAIN_MS) and writes its 20-digit sequence.
  bool QueueSyncEvent(Mutations& batch, Arena& arena, chains::BigInt& head, const char* account_id,
                      const Value* entity_key, const char* operation, Value* fields, double changed_at_ms,
                      int64_t now_ms, char sequence[32]);
  void TopicWritten(const char* device_id) const {
    if (topic_listener_) topic_listener_(topic_listener_context_, device_id);
  }

  storage::KvStore* kv_;
  mutable fortis::base::Mutex mutex_;
  // The TypeScript recentTopics: (device, topic) -> last renewal.
  // 512 sets of 8 ways, indexed by a mixed hash; a full set forgets its
  // oldest renewal (TypeScript: an LRU of 50,000 pairs).
  static constexpr size_t kRecentTopics = 4096;
  static constexpr size_t kRecentTopicWays = 8;
  struct RecentTopic {
    uint64_t hash = 0;
    int64_t renewed_at = 0;
  };
  std::unique_ptr<RecentTopic[], base::StorageDeleter> recent_topics_;
  TopicListener topic_listener_ = nullptr;
  void* topic_listener_context_ = nullptr;
};

}  // namespace fortis::wallet::address
