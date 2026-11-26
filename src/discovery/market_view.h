#pragma once

#include "bounded_json.h"
#include "price_history_batch_codec.h"

#include <stddef.h>
#include <stdint.h>

// The TypeScript market row translation (`marketView`) and the per-coin
// market entry TokenDiscoveryFacet caches:
//   {"id":<coin id>, ...marketView(row), "logoMediaKey":<media key | null>}
// written as JSON.stringify writes it. Unknown is null, never 0.
// Guest-safe and allocation-free.
namespace fortis::discovery::market_view {

using codec::history_batch::OutBuffer;

constexpr size_t kSparklineCap = 400;
// sha256(url) as 32 lowercase hex characters (the TypeScript mediaKeyFor).
constexpr size_t kMediaKeyChars = 32;

// Caller-owned scratch for one entry (decoded strings and the upper-cased
// symbol). A string longer than `text` reads as absent: a native bound.
struct MarketViewScratch {
  char text[16 * 1024];
  char upper[48 * 1024];
};

// Appends the market entry for one CoinGecko /coins/markets row object.
// `logo_key` is null or a 32-character media key. False on a document error
// or when `out` overflows.
bool AppendMarketEntry(const codec::BoundedJsonDocument& document, uint32_t row, const char* id,
                       size_t id_length, const char* logo_key, OutBuffer& out,
                       MarketViewScratch* buffers);

// `typeof row.image === 'string'` and its decoded value, for media
// registration. False when absent or not a string; `length` may be 0.
bool RowImage(const codec::BoundedJsonDocument& document, uint32_t row, char* output,
              size_t capacity, size_t* length);

// parity-only, see compat-decisions.md#date-parse
// Date.parse(text) is not NaN, for the ECMAScript date-time string format
// (and V8's "YYYY-MM-DD HH:mm[:ss]" spelling). V8's other legacy formats
// ("Jan 1 2021", "2021/01/01") are reported invalid: a native difference.
bool DateParseValid(const char* text, size_t length);

// The TypeScript isHttpUrl: `new URL(value)` succeeds with an http: or
// https: protocol, decided by src/compat/whatwg_url.cpp (the production Node.js 22
// parser, Ada 2.9.2, including its IDNA/punycode checks). Inputs over
// compat::url::kMaxInputBytes are refused. Thread-safe: callers serialize on one
// lazily allocated parser workspace; false if it cannot be allocated.
bool IsHttpUrl(const char* raw, size_t raw_length);

// mediaKeyFor(url): the first 32 hex characters of sha256(url).
void MediaKeyFor(const char* url, size_t length, char output[kMediaKeyChars + 1]);

}  // namespace fortis::discovery::market_view
