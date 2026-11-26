#pragma once

#include <stddef.h>
#include <stdint.h>

// GET OHLC: first repeated value, valid UTF-8, trimmed ID <=200 UTF-16
// units, literal USD fiat, decimal-only days in 1/7/14/30/90/180/365.
// Invalid requests return the bare BAD_REQUEST response.
namespace fortis::prices {
namespace ohlc_query {

constexpr size_t kMaxIdUnits = 200;  // zod max(200) after trim, in UTF-16 code units
// A UTF-16 code unit is at most three UTF-8 bytes, so a trimmed id longer
// than this is already over the zod maximum and is never needed verbatim.
constexpr size_t kIdBufferBytes = kMaxIdUnits * 3;
constexpr size_t kMaxSlugBytes = 100;

struct Query {
  bool ok = false;  // false: 400 {"error":"BAD_REQUEST"}
  // The trimmed `id` (valid UTF-8, may contain NUL), NUL-terminated.
  char id[kIdBufferBytes + 1]{};
  size_t id_length = 0;
  int days = 0;      // one of 1, 7, 14, 30, 90, 180, 365
  bool slug = false; // the id is a CoinGecko slug
};

// Parses the query string (the text after the first '?', not including it;
// null or empty when the target has none).
void ParseQuery(const char* query, size_t length, Query* out);

// /^[a-z0-9][a-z0-9-]{0,99}$/ over the trimmed id.
bool IsCoinGeckoSlug(const char* id, size_t length);

}  // namespace ohlc_query
}  // namespace fortis::prices
