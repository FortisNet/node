#include "ownership.h"
#include "market_view.h"

#include "js_case.h"
#include "threading.h"
#include "whatwg_url.h"

#include <math.h>
#include <mbedtls/sha256.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

namespace fortis::discovery::market_view {
namespace {

using codec::history_batch::AppendJsNumber;
using codec::history_batch::AppendJsonString;

const codec::BoundedJsonToken* field(const codec::BoundedJsonDocument& document, uint32_t row,
                              const char* name, uint32_t* index) {
  *index = document.FindObjectValue(row, name);
  return *index == codec::kBoundedJsonNoToken ? nullptr : document.token(*index);
}

// num(): a finite JSON number, else null.
bool number(const codec::BoundedJsonDocument& document, uint32_t row, const char* name,
            double* value) {
  uint32_t index = 0;
  const codec::BoundedJsonToken* item = field(document, row, name, &index);
  return item && item->type == codec::BoundedJsonType::kNumber && document.Number(index, value);
}

void append_number(OutBuffer& out, const codec::BoundedJsonDocument& document, uint32_t row,
                   const char* name) {
  double value = 0;
  if (number(document, row, name, &value)) AppendJsNumber(out, value);
  else out.Text("null");
}

// A decoded string field into the caller's scratch; false when absent, not a
// string or empty (str()).
bool string_field(const codec::BoundedJsonDocument& document, uint32_t row, const char* name,
                  char* scratch, size_t capacity, size_t* length) {
  uint32_t index = 0;
  const codec::BoundedJsonToken* item = field(document, row, name, &index);
  if (!item || item->type != codec::BoundedJsonType::kString) return false;
  if (!document.DecodeString(index, scratch, capacity, length)) return false;
  return *length > 0;
}

}  // namespace

bool AppendMarketEntry(const codec::BoundedJsonDocument& document, uint32_t row, const char* id,
                       size_t id_length, const char* logo_key, OutBuffer& out,
                       MarketViewScratch* buffers) {
  const codec::BoundedJsonToken* object = document.token(row);
  if (!object || object->type != codec::BoundedJsonType::kObject || !buffers) return false;
  char* scratch = buffers->text;
  char* upper = buffers->upper;
  size_t length = 0;
  out.Text("{\"id\":");
  AppendJsonString(out, id, id_length);
  out.Text(",\"symbol\":");
  if (string_field(document, row, "symbol", scratch, sizeof(buffers->text), &length)) {
    size_t mapped = 0;
    // Display text, not an identifier: the full Unicode upper-casing stays
    // (compat-decisions.md#js-case).
    if (!compat::JsToUpperCase(scratch, length, upper, sizeof(buffers->upper), &mapped)) return false;
    AppendJsonString(out, upper, mapped);
  } else {
    out.Text("null");
  }
  out.Text(",\"name\":");
  if (string_field(document, row, "name", scratch, sizeof(buffers->text), &length))
    AppendJsonString(out, scratch, length);
  else out.Text("null");
  struct Numeric { const char* key; const char* source; };
  static constexpr Numeric kLeading[] = {
    {"priceUsd", "current_price"},
    {"change1hPct", "price_change_percentage_1h_in_currency"},
    {"change24hPct", "price_change_percentage_24h"},
    {"change7dPct", "price_change_percentage_7d_in_currency"},
    {"change14dPct", "price_change_percentage_14d_in_currency"},
    {"change30dPct", "price_change_percentage_30d_in_currency"},
    {"change200dPct", "price_change_percentage_200d_in_currency"},
    {"change1yPct", "price_change_percentage_1y_in_currency"},
    {"marketCapUsd", "market_cap"},
  };
  for (const Numeric& item : kLeading) {
    out.Byte(','); out.Byte('"'); out.Text(item.key); out.Text("\":");
    append_number(out, document, row, item.source);
  }
  // int(): Math.trunc of a finite number (-0.5 truncates to -0, written 0).
  out.Text(",\"marketCapRank\":");
  double rank = 0;
  if (number(document, row, "market_cap_rank", &rank)) AppendJsNumber(out, trunc(rank));
  else out.Text("null");
  static constexpr Numeric kTrailing[] = {
    {"fdvUsd", "fully_diluted_valuation"},
    {"volume24hUsd", "total_volume"},
    {"high24hUsd", "high_24h"},
    {"low24hUsd", "low_24h"},
    {"circulatingSupply", "circulating_supply"},
    {"totalSupply", "total_supply"},
    {"maxSupply", "max_supply"},
    {"athUsd", "ath"},
  };
  for (const Numeric& item : kTrailing) {
    out.Byte(','); out.Byte('"'); out.Text(item.key); out.Text("\":");
    append_number(out, document, row, item.source);
  }
  auto iso = [&](const char* key, const char* source) {
    out.Byte(','); out.Byte('"'); out.Text(key); out.Text("\":");
    if (string_field(document, row, source, scratch, sizeof(buffers->text), &length) &&
        DateParseValid(scratch, length))
      AppendJsonString(out, scratch, length);
    else
      out.Text("null");
  };
  iso("athAt", "ath_date");
  out.Text(",\"athChangePct\":");
  append_number(out, document, row, "ath_change_percentage");
  out.Text(",\"atlUsd\":");
  append_number(out, document, row, "atl");
  iso("atlAt", "atl_date");
  out.Text(",\"atlChangePct\":");
  append_number(out, document, row, "atl_change_percentage");
  // sparkline(): an object's `price` member, or the value itself; finite
  // numbers only, the newest kSparklineCap kept.
  out.Text(",\"sparkline7d\":[");
  uint32_t index = 0;
  const codec::BoundedJsonToken* raw = field(document, row, "sparkline_in_7d", &index);
  if (raw && raw->type == codec::BoundedJsonType::kObject) {
    index = document.FindObjectValue(index, "price");
    raw = index == codec::kBoundedJsonNoToken ? nullptr : document.token(index);
  }
  if (raw && raw->type == codec::BoundedJsonType::kArray) {
    size_t finite = 0;
    for (uint32_t child = raw->first_child; child != codec::kBoundedJsonNoToken;
         child = document.token(child)->next_sibling)
      if (document.token(child)->type == codec::BoundedJsonType::kNumber) ++finite;
    size_t skip = finite > kSparklineCap ? finite - kSparklineCap : 0;
    bool first = true;
    for (uint32_t child = raw->first_child; child != codec::kBoundedJsonNoToken;
         child = document.token(child)->next_sibling) {
      double value = 0;
      if (document.token(child)->type != codec::BoundedJsonType::kNumber ||
          !document.Number(child, &value)) continue;
      if (skip) { --skip; continue; }
      if (!first) out.Byte(',');
      first = false;
      AppendJsNumber(out, value);
    }
  }
  out.Text("],\"logoMediaKey\":");
  if (logo_key) { out.Byte('"'); out.Text(logo_key); out.Byte('"'); }
  else out.Text("null");
  out.Byte('}');
  return out.ok;
}

bool RowImage(const codec::BoundedJsonDocument& document, uint32_t row, char* output, size_t capacity,
              size_t* length) {
  uint32_t index = 0;
  const codec::BoundedJsonToken* item = field(document, row, "image", &index);
  return item && item->type == codec::BoundedJsonType::kString &&
         document.DecodeString(index, output, capacity, length);
}

namespace {

bool digits(const char* text, size_t length, size_t* position, size_t count, int* value) {
  if (length - *position < count) return false;
  int result = 0;
  for (size_t i = 0; i < count; ++i) {
    const char c = text[*position + i];
    if (c < '0' || c > '9') return false;
    result = result * 10 + (c - '0');
  }
  *position += count;
  *value = result;
  return true;
}

bool time_part(const char* text, size_t length, size_t* position, bool allow_zone) {
  int hour = 0, minute = 0, second = 0;
  bool seconds = false, fraction_nonzero = false;
  if (!digits(text, length, position, 2, &hour) || *position >= length || text[*position] != ':')
    return false;
  ++*position;
  if (!digits(text, length, position, 2, &minute)) return false;
  if (*position < length && text[*position] == ':') {
    ++*position;
    if (!digits(text, length, position, 2, &second)) return false;
    seconds = true;
    if (*position < length && text[*position] == '.') {
      ++*position;
      size_t count = 0;
      while (*position < length && text[*position] >= '0' && text[*position] <= '9') {
        if (text[*position] != '0') fraction_nonzero = true;
        ++*position; ++count;
      }
      if (!count) return false;
    }
  }
  static_cast<void>(seconds);
  if (minute > 59 || second > 59 || hour > 24) return false;
  if (hour == 24 && (minute || second || fraction_nonzero)) return false;
  if (!allow_zone || *position == length) return true;
  const char zone = text[*position];
  if (zone == 'Z' || zone == 'z') return ++*position == length;
  if (zone != '+' && zone != '-') return false;
  ++*position;
  int offset_hour = 0, offset_minute = 0;
  if (!digits(text, length, position, 2, &offset_hour) || *position >= length || text[*position] != ':')
    return false;
  ++*position;
  if (!digits(text, length, position, 2, &offset_minute)) return false;
  return offset_hour <= 23 && offset_minute <= 59 && *position == length;
}

}  // namespace

bool DateParseValid(const char* text, size_t length) {
  size_t position = 0;
  int year = 0, month = 1, day = 1;
  if (length && (text[0] == '+' || text[0] == '-')) {
    const bool negative = text[0] == '-';
    position = 1;
    if (!digits(text, length, &position, 6, &year)) return false;
    if (negative && year == 0) return false;  // -000000
  } else if (!digits(text, length, &position, 4, &year)) {
    return false;
  }
  if (position < length && text[position] == '-') {
    ++position;
    if (!digits(text, length, &position, 2, &month)) return false;
    if (position < length && text[position] == '-') {
      ++position;
      if (!digits(text, length, &position, 2, &day)) return false;
    }
  }
  if (month < 1 || month > 12 || day < 1 || day > 31) return false;
  if (position == length) return true;
  const char separator = text[position];
  if (separator == 'T' || separator == 't') {
    ++position;
    return time_part(text, length, &position, true);
  }
  if (separator == ' ' && position == 10) {
    // V8's legacy parser: "YYYY-MM-DD HH:mm[:ss[.fff]]" in local time.
    ++position;
    return time_part(text, length, &position, false) && position == length;
  }
  return false;
}

namespace {

// One parser workspace (about 220 KiB) shared by every IsHttpUrl caller and
// allocated on first use, so callers on different threads serialize here
// instead of sharing unguarded static buffers.
fortis::base::Mutex g_url_mutex;
struct UrlWorkspace {
  fortis::compat::url::Url url;
  fortis::compat::url::UrlScratch scratch;
};
std::unique_ptr<UrlWorkspace, base::StorageDeleter> g_url_workspace;

}  // namespace

bool IsHttpUrl(const char* raw, size_t raw_length) {
  fortis::base::LockGuard g_url_lock(g_url_mutex);
  if (!g_url_workspace) g_url_workspace = base::AllocateStorage<UrlWorkspace>(sizeof(UrlWorkspace));
  bool http = false;
  if (g_url_workspace && fortis::compat::url::Parse(raw, raw_length, nullptr, &g_url_workspace->url,
                                            &g_url_workspace->scratch)) {
    const fortis::compat::url::View protocol = fortis::compat::url::Protocol(g_url_workspace->url);
    http = (protocol.length == 5 && memcmp(protocol.data, "http:", 5) == 0) ||
           (protocol.length == 6 && memcmp(protocol.data, "https:", 6) == 0);
  }
  return http;
}

void MediaKeyFor(const char* url, size_t length, char output[kMediaKeyChars + 1]) {
  unsigned char digest[32];
  mbedtls_sha256(reinterpret_cast<const unsigned char*>(url), length, digest, 0);
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < kMediaKeyChars / 2; ++i) {
    output[i * 2] = kHex[digest[i] >> 4];
    output[i * 2 + 1] = kHex[digest[i] & 15];
  }
  output[kMediaKeyChars] = '\0';
}

}  // namespace fortis::discovery::market_view
