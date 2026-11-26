#pragma once

#include "token_json.h"

#include <stddef.h>
#include <stdint.h>

// The asset-index reads of the TypeScript Stellar and Cardano drivers
// (Stellar assetPage / tokenHolders / tokenInsight and Cardano
// assetSummary / tokenHolders / tokenInsight): the request paths and the
// projections over one parsed index body, shared by
// the address routes (chain_cardano.cpp: the overview's tokenHolders) and the
// token routes (token_ledger.cpp: the page's holders and insight). Also the
// issuer reads of the Algorand and XRPL drivers' tokenInsight, which have
// no holder count; XRPL displayCurrency is shared with the address routes
// (chain_xrpl.cpp). Also the Hedera driver's Mirror Node token metadata,
// holder count and insight, and the Sui driver's GraphQL coin metadata.
// Every function is pure; documents live in the
// caller's arena. Guest-safe.
//
// Provider bodies are read with JavaScript property semantics: `x.key` of a
// non-object is undefined (a string or array has no such member), `x[0]`
// reads an array's first element, an object's "0" member or a string's
// first character, and `!x` is JavaScript truthiness.
namespace fortis::chains::asset_index {

using codec::tj::Arena;
using codec::tj::Value;

// Longest request path either index builds (a 12-character code, a 56-
// character issuer, or a 120-hex unit, plus the fixed query text).
constexpr size_t kMaxPathBytes = 256;

// splitAsset(address, symbol) then `/assets?asset_code=&asset_issuer=
// &limit=1` with encodeURIComponent halves. False when the ref names no code
// (a bare issuer without a symbol): the driver answers without a read.
bool StellarAssetPath(const char* address, const char* symbol, char* out, size_t capacity);

// cardanoAssetParts(unit) then `/asset_summary?_asset_policy=&_asset_name=`
// (URLSearchParams of lower-case hex needs no escaping). False unless the
// unit is 56..120 hex digits.
bool CardanoSummaryPath(const char* unit, char* out, size_t capacity);

// Projections over the parsed body. nullptr is the TypeError the driver
// throws on a JSON null body (`null._embedded`, `null[0]`) or arena
// exhaustion. Holders answer a JSON null for "no count" and otherwise
// {count, atLeast: false, source}; insight answers the section array.
Value* StellarHolders(Arena& arena, const Value* page);
Value* StellarInsight(Arena& arena, const Value* page);
Value* CardanoHolders(Arena& arena, const Value* rows);
Value* CardanoInsight(Arena& arena, const Value* rows);

// AlgorandDriver.tokenInsight: `/v2/assets/{id}` for an all-digit id (kept
// as written: the ref grammar does not strip leading zeros). False when the
// driver answers [] without a read.
bool AlgorandAssetPath(const char* address, char* out, size_t capacity);
// The sections over the parsed algod asset body. nullptr is the TypeError on
// a JSON null body (`null.params`) or arena exhaustion; a falsy `params`
// answers [].
Value* AlgorandInsight(Arena& arena, const Value* asset);

// Longest XRPL JSON-RPC request body (a 35-character issuer plus the fixed
// text).
constexpr size_t kMaxXrplBodyBytes = 192;
// XrplDriver.tokenInsight's splitIssuedToken(address, symbol): `CODE.ISSUER`
// at the first '.', else the symbol as currency and the address as issuer.
// False when the driver answers [] without a read (no currency, or an issuer
// that is not /^r[1-9A-HJ-NP-Za-km-z]{24,34}$/).
bool XrplIssuedToken(const char* address, const char* symbol, char* currency, size_t currency_capacity,
                     char* issuer, size_t issuer_capacity);
// JSON.stringify of DriverTransport.jsonRpc's body for gateway_balances
// ({account, strict: true, ledger_index: 'validated'}) and account_info
// ({account, ledger_index: 'validated'}).
bool XrplGatewayBalancesBody(const char* issuer, char* out, size_t capacity, size_t* length);
bool XrplAccountInfoBody(const char* issuer, char* out, size_t capacity, size_t* length);
// displayCurrency(currency): a 3-character code passes through; a 40-hex
// code decodes to ASCII when printable (trailing zero bytes trimmed), else
// keeps its hex form. nullptr where TypeScript returns null.
const char* XrplDisplayCurrency(Arena& arena, const char* currency, size_t length);
// The sections over the two `result` values (each any JSON value, possibly
// null). nullptr where the driver throws: a TypeError on a null result, a
// gateway_balances or account_info error result, or arena exhaustion.
Value* XrplInsight(Arena& arena, const char* currency, const Value* gateway, const Value* info);

// The Mirror Node token reads of the TypeScript Hedera driver:
// canonicalHederaEntityId(address), then `/api/v1/tokens/{id}` (tokenInfo,
// shared by tokenMetadata and tokenInsight) and the positive-balance holder
// page `/api/v1/tokens/{id}/balances?account.balance=gt:0&limit=100&order=asc`
// (tokenHolders). False when the driver answers without a read (null / []).
bool HederaTokenPath(const char* address, char* out, size_t capacity);
bool HederaBalancesPath(const char* address, char* out, size_t capacity);
// tokenHolders over the parsed balances page: {count, atLeast, source:
// 'hedera-mirror'}, or a JSON null when `balances` is not an array. nullptr
// is the TypeError on a JSON null page (`null.balances`) or arena exhaustion.
Value* HederaHolders(Arena& arena, const Value* page);
// tokenMetadata / tokenInsight over the parsed token-info body (any JSON
// value). nullptr where the driver throws: a TypeError on a JSON null body,
// sanitizeSymbol of a non-string symbol, BigInt of a malformed timestamp
// longer than 12 UTF-16 units, or arena exhaustion. `token_id` is the
// canonical entity id the read used.
Value* HederaMetadata(Arena& arena, const Value* info);
Value* HederaInsight(Arena& arena, const char* token_id, const Value* info);

// The TypeScript Sui driver's tokenMetadata: the GraphQL
// `FortisCoinMetadata` query for a canonical Coin<T> type
// (canonicalSuiCoinType, which the token-ref grammar already applied).
// JSON.stringify({query}) of the driver's template, byte for byte. False for
// a type outside the canonical grammar's characters or an undersized buffer.
bool SuiCoinMetadataBody(const char* coin_type, char* out, size_t capacity, size_t* length);
// coinMetadata's checks and tokenMetadata's projection over the parsed
// GraphQL body (any JSON value): {symbol, name, decimals, verified: null,
// holderCount: null, logoMediaKey: null}, or a JSON null for a null row (an
// unregistered type) and for a row whose decimals is not a non-negative
// integer. nullptr where the driver throws: the TypeError on a JSON null
// body (`null.errors`), a non-empty in-band `errors` array (a transient
// failure, never a negative answer), or arena exhaustion.
Value* SuiMetadata(Arena& arena, const Value* body);

// The TypeScript rawToDecimalString(raw, decimals) over any JSON raw
// value (nullptr is undefined), with BigInt(raw)'s coercions (a decimal or
// 0x/0o/0b string with StrWhiteSpace, an integral number, a boolean, an
// array through join): JSON null for decimals outside the integers 0..24 or
// a raw value BigInt rejects (and, a native bound, a radix string over 4,096
// significant digits). nullptr on arena exhaustion. Shared with the
// SaucerSwap pool reserves (token_saucerswap.cpp).
Value* RawToDecimal(Arena& arena, const Value* raw, double decimals);

}  // namespace fortis::chains::asset_index
