#pragma once

#include "token_json.h"

#include <stddef.h>
#include <stdint.h>

// The Tron driver's token snapshot (the TypeScript TronTokenSnapshot):
// the response interpreters, request builders and projections shared by the
// address routes (chain_tron.cpp: TRC-10 balances, the overview's
// tokenHolders) and the token routes (token_tron.cpp: tokenMetadata,
// tokenInsight, tokenHolders). Every function is pure; documents live in the
// caller's arena. Guest-safe.
namespace fortis::chains::tron_token {

using codec::tj::Arena;
using codec::tj::Value;

// TRON_ADDRESS_RE /^T[1-9A-HJ-NP-Za-km-z]{33}$/.
bool IsAddress(const char* text, size_t length);
// TRC10_ID_RE /^\d{1,20}$/.
bool IsTrc10Id(const char* text, size_t length);
// `${value}` === text (template-literal coercion of a JSON value; absent
// reads as "undefined").
bool JsStringEquals(const Value* value, const char* text);
// The Tron driver's integerString(value): an all-digit string as given, or a
// safe non-negative integer number as String(value). nullptr when neither.
Value* IntegerString(Arena& arena, const Value* value);

// TronTokenSnapshot. String members are arena strings, nullptr for null.
struct Snapshot {
  bool trc10 = false;  // standard: 'TRC-10' when true, else 'TRC-20'
  Value* symbol = nullptr;
  Value* name = nullptr;
  bool has_decimals = false;
  double decimals = 0;
  Value* total_supply_raw = nullptr;
  bool has_holders = false;
  double holders = 0;
  bool has_transfers = false;
  double transfers = 0;
  Value* issuer = nullptr;
  int8_t verified = -1;  // -1 null, 0 false, 1 true
  bool tronscan = false;  // source: 'tronscan', else 'trongrid'
};

// The outcome of one TypeScript snapshot read: a snapshot, null, or a throw
// (which tokenSnapshot's catch turns into the next source).
enum class Read : uint8_t { kSnapshot, kNull, kThrow };

// tronscanSnapshot(contract) over the parsed /api/token_trc20 body.
Read TronscanTrc20(Arena& arena, const Value* body, const char* contract, Snapshot* out);
// tronscanTrc10Snapshot(assetId) over the parsed /api/token body.
Read TronscanTrc10(Arena& arena, const Value* body, const char* asset_id, Snapshot* out);
// onChainTrc10Snapshot over the parsed /wallet/getassetissuebyid body (its
// throws are caught there: kThrow never returned).
Read OnChainTrc10(Arena& arena, const Value* body, const char* asset_id, Snapshot* out);

// view(): `body.result?.result !== true` is null, else
// `body.constant_result?.[0] ?? null`. kThrow for a JSON null body.
Read ViewResult(Arena& arena, const Value* body, const Value** result);
// onChainSnapshot over the four view bodies (totalSupply, decimals, symbol,
// name); a failed request is nullptr. Every throw is null, as there.
Read OnChainTrc20(Arena& arena, const Value* const bodies[4], Snapshot* out);

// Request builders. False when the result does not fit `capacity`.
// TronScan GET paths (encodeURIComponent of the id).
bool Trc20Path(const char* contract, char* out, size_t capacity);
bool Trc10Path(const char* asset_id, char* out, size_t capacity);
// JSON.stringify request bodies of the TronGrid POSTs.
bool AssetIssueBody(const char* asset_id, char* out, size_t capacity, size_t* length);
bool ViewBody(const char* owner, const char* contract, const char* selector, const char* parameter,
              char* out, size_t capacity, size_t* length);
extern const char* const kOnChainSelectors[4];  // totalSupply(), decimals(), symbol(), name()

// Projections of a snapshot (`snapshot` null = the null snapshot). nullptr
// only on arena exhaustion.
// tokenMetadata: {symbol, name, decimals, verified, holderCount:null,
// logoMediaKey:null}, or JSON null.
Value* Metadata(Arena& arena, const Snapshot* snapshot);
// tokenInsight: the Supply / Chain activity sections (maybe []).
Value* Insight(Arena& arena, const Snapshot* snapshot);
// readTokenHolders: {count, atLeast:false, source:'tronscan'}, or JSON null.
Value* Holders(Arena& arena, const Snapshot* snapshot);

// The snapshot as a JSON document (JSON null for the null snapshot) and back,
// for handing one provider read to every waiter of its flight.
Value* ToJson(Arena& arena, const Snapshot* snapshot);
// False on a malformed document; *present false for JSON null.
bool FromJson(Arena& arena, const Value* json, Snapshot* out, bool* present);

}  // namespace fortis::chains::tron_token
