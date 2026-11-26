#pragma once

#include "discovery_route_support.h"

namespace fortis::discovery {

// GET /v1/networks/{network}/tokens/search: the port of the TypeScript
// TokenDiscoveryFacet.search behind its route, frozen in
// tests/corpus/token_search_route.json.
//
// The registry resolves {network} (aliases included); the robinhood network
// searches the Robinhood stock-token catalog, the solana family Jupiter's
// token search, and every other network CoinGecko's /search plus a per-coin
// `cgdetail:` identity entry (6 h fresh, 7 d stale, a 404 cached as null).
// The answer (deduplicated by address, at most 20 items) is cached under
// `search:<network>:<query>` for 5 min fresh and 1 h stale unless a coin
// detail failed, in which case the partial answer is served but not stored.
//
// Native differences:
//  - the cached answer is a native binary record (it carries the sanitizers'
//    lone surrogates exactly), not the TypeScript JSON envelope, under a key
//    whose term has NUL and '%' escaped (SearchCacheKey);
//  - a fetched coin detail is kept as the subset the search reads (symbol,
//    name, image.small, platforms, detail_platforms decimals);
//  - the Robinhood catalog symbols are sorted in byte order after ASCII
//    lower-casing (docs/compat-decisions.md#locale-compare); at most
//    512 assets / 256 KiB are kept for 15 min;
//  - a provider body is parsed within the native bounds (1 MiB, 32,768
//    tokens, depth 16, no lone-surrogate escapes); beyond them it is a
//    provider failure.
struct TokenSearchState;
TokenSearchState* CreateTokenSearchState();
void DestroyTokenSearchState(TokenSearchState* state);
bool RegisterTokenSearchRoute(http::HttpRouter& router, DiscoveryRouteContext* context);
// keys.tokenSearch(network.id, term) as a KV key: `search:<id>:<term>` with
// a NUL of the term written %00 and a '%' %25 (a KV key is a C string; the
// escape keeps distinct terms distinct). False when it exceeds `capacity`.
bool SearchCacheKey(const char* network_id, const char* term, size_t term_length, char* out, size_t capacity,
                    size_t* length);
// Test hooks (tests/token_search_test.cpp): the handler's q parse and term
// over a raw target (false: the bare 400), and the answer the route writes
// for a stored record (JSON text or TokenSearchResponse wire; false when
// protobuf-es would refuse a value — the 500 — or `capacity` is short).
bool TokenSearchTerm(const char* target, char* term, size_t capacity, size_t* length);
bool TokenSearchRender(const unsigned char* record, size_t length, bool hit, int64_t as_of_ms, bool protobuf,
                       unsigned char* out, size_t capacity, size_t* used);
// Test probes: a stored `search:` answer (the native record) as the JSON text
// TypeScript's envelope `d` holds (JSON.stringify(items)). False when the
// record is unreadable or `capacity` is too small.
bool TokenSearchRecordJson(const unsigned char* record, size_t length, unsigned char* out, size_t capacity,
                           size_t* used);
// The Robinhood catalog order of ticker symbols, shared with the stock
// catalog route: byte order after ASCII lower-casing, a prefix first, then
// the symbols' own bytes as the tie-break.
int RobinhoodSymbolCompare(const char* a, size_t a_length, const char* b, size_t b_length);

}  // namespace fortis::discovery
