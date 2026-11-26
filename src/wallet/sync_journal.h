#pragma once

#include "token_json.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::storage { class KvStore; }

// The read half of the device sync journal (the TypeScript account sync
// log). The
// transfer writer (address_store.h: UpsertTransfers / AppendSyncEvents)
// appends `sync2/account/{encodeURIComponent(accountId)}/{sequence}` events
// with a seven-day expiry and advances `sync2/meta/head` (a global
// 20-digit sequence) in the same KV frame. Reading never writes.
//
// A page merges only the given accounts' streams in global sequence order
// up to the head read at its start (the watermark): one bounded KV range
// step per account per event, never a scan of other accounts. Guest-safe.
namespace fortis::wallet::sync {

constexpr int64_t kCursorMaxAgeMs = 6LL * 86400000;  // CURSOR_MAX_AGE_MS
constexpr size_t kSequenceDigits = 20;
constexpr size_t kCursorTextBytes = 40;  // "s2_" + 13 + "_" + 20 + NUL

// AccountSyncLog.cursor(sequence, atMs): s2_{atMs padded to 13}_{sequence}.
void FormatCursor(const char* sequence, int64_t at_ms, char output[kCursorTextBytes]);

// One accepted event, parsed into the page's arena (valid only during the
// visit): the stored {model, entityKey, operation, fields, changedAt,
// accountId, sequence, committedAt} object. kFull ends the page before this
// event (the caller's response bound): the page reports has_more with the
// cursor at the last written event.
enum class Visit : uint8_t { kWritten, kFull, kFailed };
using EventVisitor = Visit (*)(void* context, size_t account, const codec::tj::Value* event, codec::tj::Arena& arena);

struct PageResult {
  bool reset = false;
  bool has_more = false;
  char cursor[kCursorTextBytes]{};
  size_t events = 0;
};

class Journal {
 public:
  explicit Journal(const storage::KvStore* kv) : kv_(kv) {}

  // head(): the 20-digit journal head ("000…0" before any event).
  bool Head(int64_t now_ms, char head[kSequenceDigits + 1]) const;

  // read(accountIds, cursor, limit, now, after). `cursor` null is a cold
  // handshake (an empty page at the head). An unparsable, future or
  // over-age cursor, or one past the head, answers an empty page with
  // reset. Otherwise events with sequence in (cursor, watermark] are
  // visited in global order (skipping changedAt <= after when has_after);
  // limit + 1 are read to set has_more. A visitor's kFull ends the page
  // early with has_more (never before the first event). False is a storage
  // failure, a malformed stored event or a failed visit.
  bool Read(const char* const* account_ids, size_t account_count, const char* cursor, size_t cursor_length,
            uint32_t limit, int64_t now_ms, bool has_after, double after_ms, EventVisitor visit, void* context,
            codec::tj::Arena& arena, PageResult* result) const;

 private:
  const storage::KvStore* kv_;
};

}  // namespace fortis::wallet::sync
