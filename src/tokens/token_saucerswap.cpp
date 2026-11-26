#include <algorithm>
#include "ownership.h"
#include "utf8.h"
#include "token_saucerswap.h"

#include "chain_asset_index.h"
#include "media_registry.h"
#include "price_history_batch_codec.h"
#include "token_fetchers.h"
#include "token_liquidity.h"
#include "provider_text.h"
#include "token_views.h"
#include "whatwg_url.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::tokens::saucerswap {
namespace {

constexpr const char* kMainnetBase = "https://api.saucerswap.finance";
// One extra hour gives the 24-hour percentage a baseline (recentPrices).
constexpr int64_t kPriceWindowSeconds = 25 * 3600;

bool is_null(const Value* v) { return !v || v->type == tj::Type::kNull; }

bool encoded_path(const char* prefix, const char* token_id, const char* suffix, char* out, size_t capacity) {
  char encoded[3 * 1024 + 1];
  if (!token_fetch::EncodeComponent(token_id, encoded, sizeof(encoded))) return false;
  const int n = snprintf(out, capacity, "%s%s%s", prefix, encoded, suffix);
  return n > 0 && static_cast<size_t>(n) < capacity;
}

// finiteNumber(value) / finite(value): a number, or a string through
// Number(), finite.
bool finite_number(const Value* v, double* out) { return token_views::FiniteNumberOrString(v, out); }

Value* number_or_null(Arena& a, bool has, double value) { return has ? tj::Num(a, value) : tj::Null(a); }

// A USVString copy of WTF-8 text: a lone surrogate becomes U+FFFD, as the
// URL constructor's string conversion does.
const char* well_formed(Arena& a, const char* text, size_t length, size_t* out_length) {
  const size_t size = fortis::codec::utf8::Normalize(reinterpret_cast<const uint8_t*>(text), length, nullptr, 0);
  char* out = static_cast<char*>(a.Alloc(size + 1, 1));
  if (!out) return nullptr;
  fortis::codec::utf8::Normalize(reinterpret_cast<const uint8_t*>(text), length, out, size);
  out[size] = '\0';
  if (out_length) *out_length = size;
  return out;
}

// The URL parser's working memory, allocated once per projection.
struct UrlWork {
  std::unique_ptr<compat::url::UrlScratch, base::StorageDeleter> scratch;
  std::unique_ptr<compat::url::Url, base::StorageDeleter> parsed;
  UrlWork()
      : scratch(base::AllocateStorage<compat::url::UrlScratch>(sizeof(compat::url::UrlScratch))),
        parsed(base::AllocateStorage<compat::url::Url>(sizeof(compat::url::Url))) {}
  ~UrlWork() = default;
  UrlWork(const UrlWork&) = delete;
  UrlWork& operator=(const UrlWork&) = delete;
};

// safeHttps(value): `new URL(value)` with the https: protocol, serialized.
// JSON null when the value is not a string, does not parse or is not https
// (and for an input over the parser's native bound); nullptr on exhaustion.
Value* safe_https(Arena& a, UrlWork& work, const Value* value) {
  if (!tj::IsString(value)) return tj::Null(a);
  if (!work.scratch || !work.parsed) return nullptr;
  size_t length = 0;
  const char* clean = well_formed(a, value->text, value->length, &length);
  if (!clean) return nullptr;
  if (!compat::url::Parse(clean, length, nullptr, work.parsed.get(), work.scratch.get())) return tj::Null(a);
  const compat::url::View protocol = compat::url::Protocol(*work.parsed);
  if (protocol.length != 6 || memcmp(protocol.data, "https:", 6) != 0) return tj::Null(a);
  const compat::url::View href = compat::url::Href(*work.parsed);
  return tj::Str(a, href.data, href.length);
}

// String.prototype.trim(value) === ''.
bool blank(const Value* text) {
  size_t begin = 0, end = 0;
  codec::provider_text::Trim(text->text, text->length, &begin, &end);
  return begin == end;
}

// safeSaucerUrl(value).
Value* safe_saucer_url(Arena& a, UrlWork& work, const Value* value) {
  if (!tj::IsString(value) || blank(value)) return tj::Null(a);
  Value* https = safe_https(a, work, value);
  if (!https || https->type != tj::Type::kNull) return https;
  if (!value->length || value->text[0] != '/') return tj::Null(a);
  const size_t base = strlen(kMainnetBase);
  char* text = static_cast<char*>(a.Alloc(base + value->length + 1, 1));
  if (!text) return nullptr;
  std::copy_n(kMainnetBase, base, text);
  memcpy(text + base, value->text, value->length);
  return tj::Str(a, text, base + value->length);
}

// safeHandle(value): `value.trim().replace(/^@/, '')` when it then matches
// /^[A-Za-z0-9_]{1,30}$/.
Value* safe_handle(Arena& a, const Value* value) {
  if (!tj::IsString(value)) return tj::Null(a);
  size_t begin = 0, end = 0;
  codec::provider_text::Trim(value->text, value->length, &begin, &end);
  if (begin < end && value->text[begin] == '@') ++begin;
  const size_t length = end - begin;
  if (!length || length > 30) return tj::Null(a);
  for (size_t i = begin; i < end; ++i) {
    const char c = value->text[i];
    const bool word = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    if (!word) return tj::Null(a);
  }
  return tj::Str(a, value->text + begin, length);
}

// media.register([url])[0] ?? null for one safe URL (or JSON null).
Value* register_media(Arena& a, const Value* url, storage::KvStore* kv, int64_t now_ms) {
  if (!tj::IsString(url)) return tj::Null(a);
  char key[discovery::market_view::kMediaKeyChars + 1];
  const char* text = tj::CString(a, url);
  if (!text) return nullptr;
  if (!codec::history_batch::ValidUtf8(reinterpret_cast<const uint8_t*>(text), url->length) ||
      !discovery::RegisterMediaSource(kv, text, url->length, now_ms, key))
    return tj::Null(a);
  return tj::Str(a, key);
}

// One price point kept by `Number.isFinite(point.startTimestampSeconds)`.
struct Point {
  double start;
  const Value* value;
};

// [...points].sort((l, r) => l.start - r.start): a stable merge sort (V8's
// TimSort is stable, and the comparator is consistent over finite numbers).
bool sort_points(Arena& a, Point* points, size_t count) {
  if (count < 2) return true;
  Point* buffer = static_cast<Point*>(a.Alloc(sizeof(Point) * count));
  if (!buffer) return false;
  Point* from = points;
  Point* to = buffer;
  for (size_t width = 1; width < count; width *= 2) {
    for (size_t left = 0; left < count; left += 2 * width) {
      const size_t middle = left + width < count ? left + width : count;
      const size_t right = left + 2 * width < count ? left + 2 * width : count;
      size_t i = left, j = middle, k = left;
      // Take from the right run only when it sorts strictly first.
      while (i < middle && j < right) to[k++] = from[j].start - from[i].start < 0 ? from[j++] : from[i++];
      while (i < middle) to[k++] = from[i++];
      while (j < right) to[k++] = from[j++];
    }
    Point* swap = from;
    from = to;
    to = swap;
  }
  if (from != points) memcpy(points, from, sizeof(Point) * count);
  return true;
}

// Math.max / Math.min over finite numbers (+0 is greater than -0).
double js_max(double best, double v) {
  return v > best || (v == 0 && best == 0 && !signbit(v) && signbit(best)) ? v : best;
}
double js_min(double best, double v) {
  return v < best || (v == 0 && best == 0 && signbit(v) && !signbit(best)) ? v : best;
}

// decimalAmount(raw, decimals): `raw === undefined` and a decimals value
// that is not a non-negative integer are null; otherwise
// Number(rawToDecimalString(raw, decimals)) when finite. A raw value BigInt
// rejects makes rawToDecimalString null, and Number(null) is 0.
bool decimal_amount(Arena& a, const Value* raw, const Value* decimals, bool* has, double* out) {
  *has = false;
  if (!raw) return true;
  if (!tj::IsNumber(decimals) || !isfinite(decimals->number) || trunc(decimals->number) != decimals->number ||
      decimals->number < 0)
    return true;
  Value* text = chains::asset_index::RawToDecimal(a, raw, decimals->number);
  if (!text) return false;
  const double value = text->type == tj::Type::kString ? codec::provider_text::ToNumber(text->text, text->length) : 0;
  if (!isfinite(value)) return true;
  *has = true;
  *out = value;
  return true;
}

Value* empty_window(Arena& a) {
  Value* out = tj::Obj(a);
  if (!out) return nullptr;
  static const char* const kWindows[] = {"m5", "h1", "h6", "h24"};
  for (const char* w : kWindows)
    if (!tj::Set(a, out, w, tj::Null(a))) return nullptr;
  return out;
}

// `${a.symbol}/${b.symbol}` when both are truthy, else null.
Value* pair_label(Arena& a, const Value* left, const Value* right) {
  if (!token_liquidity::Truthy(left) || !token_liquidity::Truthy(right)) return tj::Null(a);
  Value* l = token_liquidity::TemplateString(a, left);
  Value* r = token_liquidity::TemplateString(a, right);
  if (!l || !r) return nullptr;
  char* text = static_cast<char*>(a.Alloc(l->length + r->length + 2, 1));
  if (!text) return nullptr;
  memcpy(text, l->text, l->length);
  text[l->length] = '/';
  memcpy(text + l->length + 1, r->text, r->length);
  return tj::Str(a, text, l->length + 1 + r->length);
}

bool same_id(const Value* id, const char* token_id) {
  return tj::IsString(id) && id->length == strlen(token_id) && memcmp(id->text, token_id, id->length) == 0;
}

// mapPool(row, tokenId). *pool stays nullptr for a row that maps to [];
// false on arena exhaustion.
bool map_pool(Arena& a, const Value* row, const char* token_id, Value** pool) {
  *pool = nullptr;
  const Value* left = tj::Get(row, "tokenA");
  const Value* right = tj::Get(row, "tokenB");
  const Value* left_id = tj::Get(left, "id");
  const Value* right_id = tj::Get(right, "id");
  if (!token_liquidity::Truthy(left_id) || !token_liquidity::Truthy(right_id) ||
      (!same_id(left_id, token_id) && !same_id(right_id, token_id)))
    return true;
  bool has_amount_a = false, has_amount_b = false;
  double amount_a = 0, amount_b = 0;
  if (!decimal_amount(a, tj::Get(row, "tokenReserveA"), tj::Get(left, "decimals"), &has_amount_a, &amount_a) ||
      !decimal_amount(a, tj::Get(row, "tokenReserveB"), tj::Get(right, "decimals"), &has_amount_b, &amount_b))
    return false;
  double price_a = 0, price_b = 0;
  const bool has_price_a = finite_number(tj::Get(left, "priceUsd"), &price_a);
  const bool has_price_b = finite_number(tj::Get(right, "priceUsd"), &price_b);
  const bool has_side_a = has_amount_a && has_price_a;
  const bool has_side_b = has_amount_b && has_price_b;
  // knownSides.reduce((sum, value) => sum + value, 0).
  double liquidity = 0;
  if (has_side_a) liquidity = liquidity + amount_a * price_a;
  if (has_side_b) liquidity = liquidity + amount_b * price_b;
  // row.contractId ?? (row.id === undefined ? null : String(row.id)).
  const Value* contract = tj::Get(row, "contractId");
  const Value* id = tj::Get(row, "id");
  Value* pair_address = nullptr;
  if (!is_null(contract)) pair_address = tj::Clone(a, contract);
  else pair_address = id ? token_liquidity::TemplateString(a, id) : tj::Null(a);
  Value* out = tj::Obj(a);
  Value* label = pair_label(a, tj::Get(left, "symbol"), tj::Get(right, "symbol"));
  if (!out || !label || !pair_address) return false;
  const bool ok =
      tj::Set(a, out, "dex", tj::Str(a, "saucerswap")) && tj::Set(a, out, "pairLabel", label) &&
      tj::Set(a, out, "pairAddress", pair_address) && tj::Set(a, out, "baseAddress", tj::Clone(a, left_id)) &&
      tj::Set(a, out, "quoteAddress", tj::Clone(a, right_id)) &&
      tj::Set(a, out, "liquidityUsd", number_or_null(a, has_side_a || has_side_b, liquidity)) &&
      tj::Set(a, out, "volume24h", tj::Null(a)) &&
      tj::Set(a, out, "baseAmount", number_or_null(a, has_amount_a, amount_a)) &&
      tj::Set(a, out, "quoteAmount", number_or_null(a, has_amount_b, amount_b)) &&
      tj::Set(a, out, "priceNative",
              number_or_null(a, has_price_a && has_price_b && price_b > 0, has_price_b ? price_a / price_b : 0)) &&
      tj::Set(a, out, "protocolLabel", tj::Str(a, "v1")) && tj::Set(a, out, "url", tj::Null(a)) &&
      tj::Set(a, out, "pairCreatedAt", tj::Null(a)) && tj::Set(a, out, "fdv", tj::Null(a)) &&
      tj::Set(a, out, "marketCap", tj::Null(a)) &&
      tj::Set(a, out, "priceUsd", number_or_null(a, has_price_a, price_a)) &&
      tj::Set(a, out, "volume", empty_window(a)) && tj::Set(a, out, "priceChange", empty_window(a)) &&
      tj::Set(a, out, "txns", empty_window(a));
  if (!ok) return false;
  *pool = out;
  return true;
}

Value* insight_row(Arena& a, const char* label, Value* value, const char* sentiment) {
  Value* row = tj::Obj(a);
  if (!row || !value || !tj::Set(a, row, "label", tj::Str(a, label)) || !tj::Set(a, row, "value", value) ||
      (sentiment && !tj::Set(a, row, "sentiment", tj::Str(a, sentiment))))
    return nullptr;
  return row;
}

}  // namespace

bool TokenPath(const char* token_id, char* out, size_t capacity) {
  return encoded_path("/tokens/", token_id, "", out, capacity);
}

bool PricesPath(const char* token_id, int64_t now_ms, char* out, size_t capacity) {
  // Math.floor(now.getTime() / 1000).
  const int64_t to = now_ms >= 0 ? now_ms / 1000 : -((-now_ms + 999) / 1000);
  char query[96];
  snprintf(query, sizeof(query), "?from=%lld&to=%lld&interval=HOUR",
           static_cast<long long>(to - kPriceWindowSeconds), static_cast<long long>(to));
  return encoded_path("/tokens/prices/", token_id, query, out, capacity);
}

bool PoolsPath(const char* token_id, char* out, size_t capacity) {
  return encoded_path("/tokens/associated-pools/", token_id, "", out, capacity);
}

Value* MarketRow(Arena& a, const Value* token, const Value* prices) {
  if (is_null(token)) return tj::Null(a);
  // [...rawPrices]: an array spreads its elements and a string its
  // characters (none has a finite startTimestampSeconds); any other
  // non-null value is not iterable.
  size_t count = 0;
  Point* points = nullptr;
  if (!is_null(prices) && prices->type != tj::Type::kString) {
    if (!tj::IsArray(prices)) return nullptr;
    points = static_cast<Point*>(a.Alloc(sizeof(Point) * (prices->count + 1)));
    if (!points) return nullptr;
    for (uint32_t i = 0; i < prices->count; ++i) {
      const Value* point = prices->items[i];
      if (point->type == tj::Type::kNull) return nullptr;  // null.startTimestampSeconds
      const Value* start = tj::Get(point, "startTimestampSeconds");
      if (!tj::IsNumber(start) || !isfinite(start->number)) continue;
      points[count++] = Point{start->number, point};
    }
    if (!sort_points(a, points, count)) return nullptr;
  }
  double current = 0, baseline = 0;
  bool has_current = finite_number(tj::Get(token, "priceUsd"), &current);
  if (!has_current && count) has_current = finite_number(tj::Get(points[count - 1].value, "closeUsd"), &current);
  const bool has_baseline = count && finite_number(tj::Get(points[0].value, "openUsd"), &baseline);
  bool has_high = false, has_low = false, has_volume = false;
  double high = -INFINITY, low = INFINITY, volume = 0;
  for (size_t i = 0; i < count; ++i) {
    double v = 0;
    if (finite_number(tj::Get(points[i].value, "highUsd"), &v)) { high = js_max(high, v); has_high = true; }
    if (finite_number(tj::Get(points[i].value, "lowUsd"), &v)) { low = js_min(low, v); has_low = true; }
    if (finite_number(tj::Get(points[i].value, "volumeUsd"), &v)) { volume = volume + v; has_volume = true; }
  }
  const Value* symbol = tj::Get(token, "symbol");
  const Value* name = tj::Get(token, "name");
  Value* out = tj::Obj(a);
  Value* symbol_value = tj::IsString(symbol) ? codec::provider_text::SanitizeSymbol(a, symbol->text, symbol->length)
                                             : tj::Null(a);
  Value* name_value = tj::IsString(name) ? codec::provider_text::SanitizeName(a, name->text, name->length) : tj::Null(a);
  const bool has_change = has_current && has_baseline && baseline > 0;
  if (!out || !symbol_value || !name_value || !tj::Set(a, out, "symbol", symbol_value) ||
      !tj::Set(a, out, "name", name_value) ||
      !tj::Set(a, out, "current_price", number_or_null(a, has_current, current)) ||
      !tj::Set(a, out, "price_change_percentage_24h",
               number_or_null(a, has_change, has_change ? ((current / baseline) - 1) * 100 : 0)) ||
      !tj::Set(a, out, "high_24h", number_or_null(a, has_high, high)) ||
      !tj::Set(a, out, "low_24h", number_or_null(a, has_low, low)) ||
      !tj::Set(a, out, "total_volume", number_or_null(a, has_volume, volume)))
    return nullptr;
  return out;
}

Value* Profile(Arena& a, const Value* token, storage::KvStore* kv, int64_t now_ms) {
  if (is_null(token)) return tj::Null(a);
  UrlWork work;
  Value* icon = safe_saucer_url(a, work, tj::Get(token, "icon"));
  if (!icon) return nullptr;
  Value* logo = register_media(a, icon, kv, now_ms);
  const Value* description = tj::Get(token, "description");
  const Value* category = tj::Get(token, "category");
  Value* description_value = tj::IsString(description)
      ? codec::provider_text::SanitizeText(a, description->text, description->length)
      : tj::Null(a);
  Value* categories = tj::Arr(a);
  if (!categories || !logo || !description_value) return nullptr;
  if (tj::IsString(category) && !blank(category) &&
      !tj::Push(a, categories, codec::provider_text::SanitizeName(a, category->text, category->length)))
    return nullptr;
  Value* links = tj::Obj(a);
  Value* out = tj::Obj(a);
  if (!links || !out || !tj::Set(a, links, "homepage", safe_https(a, work, tj::Get(token, "website"))) ||
      !tj::Set(a, links, "twitter", safe_handle(a, tj::Get(token, "twitterHandle"))) ||
      !tj::Set(a, links, "discord", safe_https(a, work, tj::Get(token, "discord"))) ||
      !tj::Set(a, links, "telegram", safe_https(a, work, tj::Get(token, "telegram"))) ||
      !tj::Set(a, out, "description", description_value) || !tj::Set(a, out, "categories", categories) ||
      !tj::Set(a, out, "links", links) || !tj::Set(a, out, "logoMediaKey", logo) ||
      !tj::Set(a, out, "marketsCount", tj::Null(a)) || !tj::Set(a, out, "sentimentUpPct", tj::Null(a)))
    return nullptr;
  return out;
}

bool AppendInsight(Arena& a, const Value* token, Value* sections) {
  if (is_null(token)) return true;
  const Value* reviewed = tj::Get(token, "dueDiligenceComplete");
  const Value* listing = tj::Get(token, "listingType");
  const Value* fee = tj::Get(token, "isFeeOnTransferToken");
  const bool done = reviewed && reviewed->type == tj::Type::kTrue;
  const bool refused = reviewed && reviewed->type == tj::Type::kFalse;
  Value* rows = tj::Arr(a);
  if (!rows) return false;
  if (tj::IsString(listing) &&
      !tj::Push(a, rows, insight_row(a, "Listing type", tj::Str(a, listing->text, listing->length), nullptr)))
    return false;
  if (reviewed) {
    const bool yes = token_liquidity::Truthy(reviewed);
    if (!tj::Push(a, rows, insight_row(a, "Due diligence complete", tj::Str(a, yes ? "Yes" : "No"),
                                       yes ? "good" : "caution")))
      return false;
  }
  if (fee) {
    const bool yes = token_liquidity::Truthy(fee);
    if (!tj::Push(a, rows, insight_row(a, "Fee-on-transfer token", tj::Str(a, yes ? "Yes" : "No"),
                                       yes ? "caution" : "good")))
      return false;
  }
  UrlWork work;
  Value* report = safe_https(a, work, tj::Get(token, "sentinelReport"));
  if (!report) return false;
  if (report->type == tj::Type::kString && !tj::Push(a, rows, insight_row(a, "Sentinel report", report, nullptr)))
    return false;
  Value* section = tj::Obj(a);
  return section && tj::Set(a, section, "title", tj::Str(a, "SaucerSwap listing")) &&
         tj::Set(a, section, "summary",
                 tj::Str(a, done      ? "SaucerSwap due diligence completed"
                            : refused ? "SaucerSwap due diligence not completed"
                                      : "SaucerSwap listing found")) &&
         tj::Set(a, section, "ok", tj::Bool(a, done)) && tj::Set(a, section, "rows", rows) &&
         tj::Push(a, sections, section);
}

PoolsStatus Pools(Arena& a, const Value* rows, const char* token_id, Value** pools) {
  *pools = nullptr;
  Value* out = tj::Arr(a);
  if (!out) return PoolsStatus::kArena;
  if (is_null(rows)) { *pools = out; return PoolsStatus::kOk; }
  if (!tj::IsArray(rows)) return PoolsStatus::kFailed;
  bool base_not_string = false;
  for (uint32_t i = 0; i < rows->count; ++i) {
    const Value* row = rows->items[i];
    if (row->type == tj::Type::kNull) return PoolsStatus::kFailed;  // null.tokenA
    Value* pool = nullptr;
    if (!map_pool(a, row, token_id, &pool)) return PoolsStatus::kArena;
    if (!pool) continue;
    base_not_string = base_not_string || !tj::IsString(tj::Get(pool, "baseAddress"));
    if (!tj::Push(a, out, pool)) return PoolsStatus::kArena;
  }
  if (!a.ok) return PoolsStatus::kArena;
  *pools = out;
  return base_not_string ? PoolsStatus::kSnapshotFailed : PoolsStatus::kOk;
}

}  // namespace fortis::tokens::saucerswap
