#pragma once

#include "chain_core.h"

#include <stddef.h>
#include <stdint.h>

// Pure transfer-row views of the transfers route (no I/O), ported from the
// earlier TypeScript implementation:
//  - transferRowToAppJson (the app's
//    TransactionHistory.toJson wire format);
//  - economicOperationFromStoredTransfer +
//    economicOperationToAppJson (no basis annotations);
//  - the transfer store's initial()/merge() (a stored row from a source
//    record, and the re-ingest merge);
//  - the TransfersFacet display cursors and transferMatches;
//  - priceIdForTransfer.
//
// A stored row (StoredTransfer) is an object with the TypeScript fields;
// Date fields (ts, fiatSourceTs, fiatAttemptedAt, createdAt, updatedAt) are
// epoch-ms numbers or JSON null. Guest-safe.
namespace fortis::wallet::address {

using chains::Arena;
using chains::Network;
using chains::Value;

// Number.prototype.toFixed(digits) for 0 <= digits <= 100, exact (ties away
// from zero, as V8 picks the larger n); |x| >= 1e21 uses String(x).
void JsToFixed(double value, int digits, char* output, size_t capacity);

// transferRowToAppJson(row, network, tipHeight); `has_tip` false = null.
Value* TransferAppJson(Arena& arena, const Value* row, const Network& network, bool has_tip, double tip);
// economicOperationToAppJson(economicOperationFromStoredTransfer(row), legs).
Value* OperationAppJson(Arena& arena, const Value* row);

// initial(accountId, record, now) of TransferStore.
Value* InitialRow(Arena& arena, const char* account_id, const Value* record, int64_t now_ms);
// merge(old, incoming, restatable, now).
Value* MergeRow(Arena& arena, const Value* old, const Value* incoming, bool restatable, int64_t now_ms);

// encodeDisplayCursor({ts, recordId}): base64url(JSON {ts: ISO, id}).
bool EncodeDisplayCursor(int64_t ts_ms, const char* record_id, char* output, size_t capacity);
// decodeDisplayCursor over `length` bytes of the raw cursor (Buffer.from(raw,
// 'base64url'): both alphabets, other characters skipped, decoding stops at
// '='; then JSON.parse and Date.parse(ts)): false = RangeError('invalid
// display cursor'). The id may hold an escaped NUL, so it comes with its
// byte length.
bool DecodeDisplayCursor(Arena& arena, const char* raw, size_t length, int64_t* ts_ms, const char** id,
                         size_t* id_length);

struct TransferQuery {
  // Present (non-null) even when empty: `since=` selects the time order
  // (an empty string is falsy) but is still the page position ('' ?? …).
  // since_length counts bytes past an escaped NUL (truthiness).
  const char* since = nullptr;
  size_t since_length = 0;
  const char* before = nullptr;  // null when absent or empty
  size_t before_length = 0;
  bool refresh = false;
  double limit = 100;
  const char* token_id = nullptr;
  const char* price_source_id = nullptr;
  bool has_from = false;
  double from_sec = 0;
  bool has_to = false;
  double to_sec = 0;
  const char* flow = nullptr;  // "in" | "out" | "neutral" | null
};
// transferMatches(row, query).
bool TransferMatches(const Value* row, const TransferQuery& query);

// priceIdForTransfer(network, tokenId, canonicalId); null when unpriceable.
const char* PriceIdForTransfer(Arena& arena, const Network& network, const char* token_id,
                               const char* canonical_id);

// Field accessors over a stored row.
const char* RowString(const Value* row, const char* key);  // null unless a string
bool RowNumber(const Value* row, const char* key, double* out);
int64_t RowMs(const Value* row, const char* key);  // 0 when null/absent

}  // namespace fortis::wallet::address
