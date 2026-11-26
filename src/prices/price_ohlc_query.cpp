#include "price_ohlc_query.h"

#include "price_history_query.h"
#include "price_spot_query.h"
#include "query_string.h"

#include <string.h>

namespace fortis::prices {
namespace ohlc_query {
namespace {

// zod's .trim() (String.prototype.trim) and min/max over the decoded id,
// streamed: leading whitespace is dropped, everything after it is kept up to
// the buffer, and the trimmed end is the end of the last non-whitespace code
// point. Whitespace past the buffer is fine (it is trailing until another
// character arrives); a non-whitespace character past it means the trimmed
// id has over 600 bytes, so over 200 units.
struct TrimmedId {
  char* out;
  size_t used = 0;          // bytes kept since the first non-whitespace
  size_t units = 0;         // their UTF-16 units
  size_t end = 0;           // bytes up to the last non-whitespace code point
  size_t end_units = 0;
  bool started = false;
  bool too_long = false;
  unsigned char pending[4]{};
  unsigned need = 0, have = 0;

  void byte(unsigned char value) {
    if (need) {
      pending[have++] = value;
      if (--need == 0) code_point();
      return;
    }
    pending[0] = value;
    have = 1;
    need = value < 0x80 ? 0 : value >= 0xf0 ? 3 : value >= 0xe0 ? 2 : 1;
    if (!need) code_point();
  }
  void code_point() {
    uint32_t c = have == 1 ? pending[0]
        : have == 2 ? pending[0] & 0x1fu : have == 3 ? pending[0] & 0x0fu : pending[0] & 0x07u;
    for (unsigned i = 1; i < have; ++i) c = (c << 6) | (pending[i] & 0x3fu);
    const bool space = history_query::IsJsWhitespace(c);
    if (!started && space) return;
    started = true;
    if (used + have <= kIdBufferBytes) memcpy(out + used, pending, have);
    used += have;
    units += have == 4 ? 2 : 1;
    if (!space) {
      if (used > kIdBufferBytes) too_long = true;
      else { end = used; end_units = units; }
    }
  }
};

void feed_id(void* context, unsigned char byte) { static_cast<TrimmedId*>(context)->byte(byte); }
}  // namespace

bool IsCoinGeckoSlug(const char* id, size_t length) {
  if (!id || !length || length > kMaxSlugBytes) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(id[i]);
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-' && i))) return false;
  }
  return true;
}

void ParseQuery(const char* query, size_t length, Query* out) {
  out->ok = false;
  out->id_length = 0;
  out->id[0] = '\0';
  out->days = 0;
  out->slug = false;
  const http::query_string::Query parsed(query, length);
  http::query_string::Component value;
  if (!parsed.Valid() || !parsed.First("id", &value)) return;
  TrimmedId id{out->id};
  value.DecodeTo(feed_id, &id);
  if (id.too_long || id.end_units < 1 || id.end_units > kMaxIdUnits) return;
  if (parsed.First("fiat", &value) && !value.Equals("usd")) return;
  uint64_t days = 0;
  if (!parsed.First("days", &value) || !value.Unsigned(365, &days)) return;
  const int supported[] = {1, 7, 14, 30, 90, 180, 365};
  for (const int candidate : supported) if (days == static_cast<uint64_t>(candidate)) out->days = candidate;
  if (!out->days) return;
  out->id_length = id.end;
  out->id[id.end] = '\0';
  out->slug = IsCoinGeckoSlug(out->id, out->id_length);
  out->ok = true;
}

}  // namespace ohlc_query
}  // namespace fortis::prices
