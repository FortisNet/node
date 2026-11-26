#pragma once

#include "token_json.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::storage { class KvStore; }

// The Hedera SaucerSwap token reads (the TypeScript SaucerSwapClient)
// and the Token360 projections over them (the TypeScript
// saucerMarketRow, hederaProfile, saucerInsight, and the client's pool
// mapping). Mirror Node stays the authority for on-ledger identity and
// controls; SaucerSwap contributes the DEX price, description, logo, pools
// and listing context.
//
// The three GETs run on the 'saucerswap' lane against
// https://api.saucerswap.finance or, for a testnet network,
// https://test-api.saucerswap.finance (token_provider: kSaucerSwap /
// kSaucerSwapTestnet). getOrNull: a 404 is the durable null answer (an
// unlisted token); every other failure (a non-2xx, a transport failure, an
// unparseable 2xx after HttpClient's retries, a busy lane) is thrown.
//
// Provider bodies are read with JavaScript property semantics: `x.key` of a
// non-object is undefined and `!x` is JavaScript truthiness. Every function
// is pure apart from Profile's media registration; documents live in the
// caller's arena. Guest-safe.
namespace fortis::tokens::saucerswap {

using codec::tj::Arena;
using codec::tj::Value;

// `/tokens/{id}`, `/tokens/prices/{id}?from=&to=&interval=HOUR` (the 25 hours
// before `now_ms`: `to = Math.floor(now / 1000)`, `from = to - 90000`) and
// `/tokens/associated-pools/{id}`, each id encodeURIComponent'd. False when
// the path does not fit.
bool TokenPath(const char* token_id, char* out, size_t capacity);
bool PricesPath(const char* token_id, int64_t now_ms, char* out, size_t capacity);
bool PoolsPath(const char* token_id, char* out, size_t capacity);

// Token360Facet.marketData's SaucerSwap branch: `token === null ? null :
// saucerMarketRow(token, prices)`. `token` is the token() answer (a JSON
// value, JSON null for the 404 / null answer). `prices` is recentPrices'
// parsed body, or nullptr when the read failed or answered null (both []).
// nullptr where saucerMarketRow throws: a prices body that is not iterable
// (a number, boolean or object; a string iterates to nothing usable) or a
// null price point; and on arena exhaustion.
Value* MarketRow(Arena& arena, const Value* token, const Value* prices);

// Token360Facet.hederaProfile over the token() answer: JSON null for a null
// token, else {description, categories, links: {homepage, twitter,
// discord, telegram}, logoMediaKey, marketsCount: null, sentimentUpPct:
// null}. The icon passes safeSaucerUrl (an https URL, or a root-relative
// path on https://api.saucerswap.finance) before it is registered as media
// (`kv`, `now_ms`); links pass safeHttps / safeHandle. nullptr only on arena
// exhaustion.
Value* Profile(Arena& arena, const Value* token, storage::KvStore* kv, int64_t now_ms);

// saucerInsight(token) appended to `sections`: nothing for a null token,
// else the "SaucerSwap listing" section. False on arena exhaustion.
bool AppendInsight(Arena& arena, const Value* token, Value* sections);

// SaucerSwapClient.pools(tokenId) over the parsed associated-pools body:
// `(rows ?? []).flatMap((row) => mapPool(row, tokenId))` as provider-neutral
// LiquidityPool rows (nullptr / JSON null rows are []) in *pools.
//  - kFailed: pools() throws (a non-array body: `flatMap` is not a
//    function; a null row);
//  - kSnapshotFailed: the rows map, but a row's baseAddress is not a string,
//    so the calculateLiquiditySnapshot that always follows this read throws
//    (tokenAddressesEqual calls startsWith on every base address);
//  - kArena: arena exhaustion.
enum class PoolsStatus : uint8_t { kOk, kFailed, kSnapshotFailed, kArena };
PoolsStatus Pools(Arena& arena, const Value* rows, const char* token_id, Value** pools);

}  // namespace fortis::tokens::saucerswap
