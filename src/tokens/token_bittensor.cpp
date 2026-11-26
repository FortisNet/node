#include <cerrno>
#include <cstdlib>
#include "token_bittensor.h"
#include "ownership.h"

#include "chain_asset_index.h"
#include "chain_core.h"
#include "provider_runtime_config.h"
#include "price_history_batch_codec.h"
#include "threading.h"
#include "token_views.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::tokens::token_bittensor {
namespace {

using tj::Type;

constexpr double kRaoPerTao = 1000000000.0;
constexpr double kMaxSafeInteger = 9007199254740991.0;

bool nullish(const Value* v) { return !v || v->type == Type::kNull; }

// `value.key`: only objects have the members these bodies are read for.
const Value* member(const Value* v, const char* key) { return tj::Get(v, key); }

// `value[0]`: an array's first element, an object's "0" member, or a
// string's first character (a stand-in: every member read of it is
// undefined); otherwise undefined.
const Value* first(const Value* v) {
  if (!v) return nullptr;
  if (v->type == Type::kArray) return v->count ? v->items[0] : nullptr;
  if (v->type == Type::kObject) return tj::Get(v, "0");
  if (v->type == Type::kString) return v->length ? v : nullptr;
  return nullptr;
}

// finite(value): a number, or Number() of a string; finite or null.
Value* finite(Arena& a, const Value* v, double scale = 1) {
  double number = 0;
  if (!token_views::FiniteNumberOrString(v, &number)) return tj::Null(a);
  return tj::Num(a, scale == 1 ? number : number / scale);
}

// cleanSymbol / cleanName: `typeof value === 'string' && value.trim() !==
// '' ? sanitize(value) : null`; the sanitizer sees the untrimmed text and
// may still answer "".
Value* clean(Arena& a, const Value* v, bool symbol) {
  if (!tj::IsString(v)) return tj::Null(a);
  size_t begin = 0, end = 0;
  codec::history_batch::JsTrim(v->text, v->length, &begin, &end);
  if (end == begin) return tj::Null(a);
  return symbol ? chains::SanitizeSymbol(a, v->text, v->length) : chains::SanitizeName(a, v->text, v->length);
}

// rawToDecimalString(integerString(value), 9) ?? null, where integerString
// keeps a safe integer's `${value}` or a string's trimmed text when it is
// /^\d+$/.
Value* supply(Arena& a, const Value* v) {
  char number[40];
  const char* text = nullptr;
  size_t length = 0;
  if (tj::IsNumber(v) && isfinite(v->number) && fabs(v->number) <= kMaxSafeInteger &&
      floor(v->number) == v->number) {
    chains::JsNumberText(v->number, number, sizeof(number));
    text = number;
    length = strlen(number);
  } else if (tj::IsString(v)) {
    size_t begin = 0, end = 0;
    codec::history_batch::JsTrim(v->text, v->length, &begin, &end);
    text = v->text + begin;
    length = end - begin;
  }
  if (!text || !length) return tj::Null(a);
  for (size_t i = 0; i < length; ++i)
    if (text[i] < '0' || text[i] > '9') return tj::Null(a);
  Value* digits = tj::Str(a, text, length);
  return digits ? chains::asset_index::RawToDecimal(a, digits, 9) : nullptr;
}

Value* row(Arena& a, const char* label, const char* value) {
  Value* o = tj::Obj(a);
  if (!o || !tj::Set(a, o, "label", tj::Str(a, label)) || !tj::Set(a, o, "value", tj::Str(a, value)))
    return nullptr;
  return o;
}

// row(label, value, unit): `${value.toLocaleString('en-US', {
// maximumFractionDigits: 9 })} ${unit}`, skipped for a null value.
bool push_row(Arena& a, Value* rows, const char* label, const Value* value, const char* unit, size_t unit_length) {
  if (!tj::IsNumber(value)) return true;
  char formatted[448];
  if (!FormatEnUs(value->number, formatted, sizeof(formatted))) return false;
  const size_t used = strlen(formatted);
  char* text = static_cast<char*>(a.Alloc(used + 1 + unit_length + 1, 1));
  if (!text) return false;
  memcpy(text, formatted, used);
  text[used] = ' ';
  memcpy(text + used + 1, unit, unit_length);
  text[used + 1 + unit_length] = '\0';
  return tj::Push(a, rows, row(a, label, text));
}

Value* number_or_null(Arena& a, bool has, double value) { return has ? tj::Num(a, value) : tj::Null(a); }

bool get_number(const Value* pool, const char* key, double* out) {
  const Value* v = member(pool, key);
  if (!tj::IsNumber(v)) return false;
  *out = v->number;
  return true;
}

// The pool page's members, in order.
constexpr const char* kPageKeys[] = {"symbol", "name", "price", "marketCap", "liquidity", "totalTao",
                                     "alphaInPool", "alphaStaked", "totalAlpha", "totalSupply",
                                     "change1h", "change24h", "change7d"};

// ---- working-set copies -----------------------------------------------------

// A flat encoding that keeps −0: [0] for a null page, else [1] then per
// member a tag (0 null, 1 number, 2 string) and its payload, in kPageKeys
// order.
bool encode(const Value* page, uint8_t* out, size_t capacity, size_t* length) {
  size_t used = 0;
  auto put = [&](const void* bytes, size_t count) {
    if (used + count > capacity) return false;
    memcpy(out + used, bytes, count);
    used += count;
    return true;
  };
  const uint8_t present = tj::IsObject(page) ? 1 : 0;
  if (!put(&present, 1)) return false;
  if (present) {
    for (const char* key : kPageKeys) {
      const Value* v = member(page, key);
      uint8_t tag = 0;
      if (tj::IsNumber(v)) tag = 1;
      else if (tj::IsString(v)) tag = 2;
      if (!put(&tag, 1)) return false;
      if (tag == 1 && !put(&v->number, sizeof(double))) return false;
      if (tag == 2) {
        const uint32_t n = v->length;
        if (!put(&n, sizeof(n)) || !put(v->text, n)) return false;
      }
    }
  }
  *length = used;
  return true;
}

Value* decode(Arena& a, const uint8_t* blob, size_t length) {
  size_t used = 0;
  auto take = [&](void* bytes, size_t count) {
    if (used + count > length) return false;
    memcpy(bytes, blob + used, count);
    used += count;
    return true;
  };
  uint8_t present = 0;
  if (!take(&present, 1)) return nullptr;
  if (!present) return tj::Null(a);
  Value* page = tj::Obj(a);
  if (!page) return nullptr;
  for (const char* key : kPageKeys) {
    uint8_t tag = 0;
    if (!take(&tag, 1)) return nullptr;
    Value* v = nullptr;
    if (tag == 0) {
      v = tj::Null(a);
    } else if (tag == 1) {
      double number = 0;
      if (!take(&number, sizeof(number))) return nullptr;
      v = tj::Num(a, number);
    } else {
      uint32_t n = 0;
      if (!take(&n, sizeof(n)) || used + n > length) return nullptr;
      v = tj::Str(a, reinterpret_cast<const char*>(blob + used), n);
      used += n;
    }
    if (!tj::Set(a, page, key, v)) return nullptr;
  }
  return page;
}

}  // namespace

bool Keyed(const config::ProviderRuntimeConfig* config) { return config && config->taostats_api_key_configured; }

bool SubnetNetuid(const char* address, uint32_t* netuid) {
  if (!address) return false;
  const size_t length = strlen(address);
  if (!length || length > 5) return false;
  uint32_t value = 0;
  for (size_t i = 0; i < length; ++i) {
    if (address[i] < '0' || address[i] > '9') return false;
    value = value * 10 + static_cast<uint32_t>(address[i] - '0');
  }
  if (value == 0 || value > 65535) return false;
  *netuid = value;
  return true;
}

bool PoolPath(uint32_t netuid, char* out, size_t capacity) {
  const int n = snprintf(out, capacity, "/api/dtao/pool/latest/v1?network=finney&netuid=%u&limit=1", netuid);
  return n > 0 && static_cast<size_t>(n) < capacity;
}

Value* PoolPage(Arena& a, const Value* body, uint32_t netuid) {
  if (nullish(body)) return nullptr;  // `null.data`
  const Value* pool = first(member(body, "data"));
  if (!pool) return tj::Null(a);
  if (pool->type == Type::kNull) return nullptr;  // `null.netuid`
  // `Number(pool.netuid) === netuid`.
  if (!(chains::JsNumber(member(pool, "netuid")) == static_cast<double>(netuid))) return tj::Null(a);
  Value* page = tj::Obj(a);
  if (!page) return nullptr;
  Value* values[] = {
      clean(a, member(pool, "symbol"), true),
      clean(a, member(pool, "name"), false),
      finite(a, member(pool, "price")),
      finite(a, member(pool, "market_cap"), kRaoPerTao),
      finite(a, member(pool, "liquidity"), kRaoPerTao),
      finite(a, member(pool, "total_tao"), kRaoPerTao),
      finite(a, member(pool, "alpha_in_pool"), kRaoPerTao),
      finite(a, member(pool, "alpha_staked"), kRaoPerTao),
      finite(a, member(pool, "total_alpha"), kRaoPerTao),
      supply(a, member(pool, "total_alpha")),
      finite(a, member(pool, "price_change_1_hour")),
      finite(a, member(pool, "price_change_1_day")),
      finite(a, member(pool, "price_change_1_week")),
  };
  static_assert(sizeof(values) / sizeof(values[0]) == sizeof(kPageKeys) / sizeof(kPageKeys[0]), "page keys");
  for (size_t i = 0; i < sizeof(kPageKeys) / sizeof(kPageKeys[0]); ++i)
    if (!tj::Set(a, page, kPageKeys[i], values[i])) return nullptr;
  return page;
}

Value* TaoPricePage(Arena& a, const Value* body) {
  if (nullish(body)) return nullptr;  // `null.data`
  // `Array.isArray(data) ? data[0] : data`, then `finite(row?.price)`.
  const Value* data = member(body, "data");
  const Value* price_row = tj::IsArray(data) ? (data->count ? data->items[0] : nullptr) : data;
  return finite(a, member(price_row, "price"));
}

Value* Metadata(Arena& a, const Value* pool) {
  if (!tj::IsObject(pool)) return tj::Null(a);
  Value* o = tj::Obj(a);
  if (!o || !tj::Set(a, o, "symbol", tj::Clone(a, member(pool, "symbol"))) ||
      !tj::Set(a, o, "name", tj::Clone(a, member(pool, "name"))) || !tj::Set(a, o, "decimals", tj::Num(a, 9)) ||
      !tj::Set(a, o, "verified", tj::Null(a)) || !tj::Set(a, o, "holderCount", tj::Null(a)) ||
      !tj::Set(a, o, "logoMediaKey", tj::Null(a)) ||
      !tj::Set(a, o, "totalSupply", tj::Clone(a, member(pool, "totalSupply"))) ||
      !tj::Set(a, o, "maxSupply", tj::Null(a)))
    return nullptr;
  return o;
}

Value* Insight(Arena& a, uint32_t netuid, const Value* pool) {
  Value* sections = tj::Arr(a);
  if (!sections || !tj::IsObject(pool)) return sections;
  Value* rows = tj::Arr(a);
  char id[16];
  snprintf(id, sizeof(id), "%u", netuid);
  if (!rows || !tj::Push(a, rows, row(a, "Netuid", id))) return nullptr;
  // cleanSymbol(pool.symbol) ?? 'alpha' (a sanitized "" stays "").
  const Value* symbol = member(pool, "symbol");
  const char* unit = tj::IsString(symbol) ? symbol->text : "alpha";
  const size_t unit_length = tj::IsString(symbol) ? symbol->length : 5;
  if (!push_row(a, rows, "Price", member(pool, "price"), "TAO", 3) ||
      !push_row(a, rows, "Market cap", member(pool, "marketCap"), "TAO", 3) ||
      !push_row(a, rows, "Liquidity", member(pool, "liquidity"), "TAO", 3) ||
      !push_row(a, rows, "TAO in pool", member(pool, "totalTao"), "TAO", 3) ||
      !push_row(a, rows, "Alpha in pool", member(pool, "alphaInPool"), unit, unit_length) ||
      !push_row(a, rows, "Alpha staked", member(pool, "alphaStaked"), unit, unit_length))
    return nullptr;
  Value* section = tj::Obj(a);
  if (!section || !tj::Set(a, section, "title", tj::Str(a, "Bittensor subnet pool")) ||
      !tj::Set(a, section, "summary", tj::Str(a, "Current dTAO pool data from Taostats")) ||
      !tj::Set(a, section, "ok", tj::Bool(a, true)) || !tj::Set(a, section, "rows", rows) ||
      !tj::Push(a, sections, section))
    return nullptr;
  return sections;
}

Value* MarketRow(Arena& a, const Value* pool, bool has_usd, double usd) {
  if (!tj::IsObject(pool)) return tj::Null(a);
  double price = 0, cap = 0, total = 0;
  const bool has_price = get_number(pool, "price", &price);
  const bool has_cap = get_number(pool, "marketCap", &cap);
  const bool has_total = get_number(pool, "totalAlpha", &total);
  const bool has_price_usd = has_price && has_usd;
  const double price_usd = has_price_usd ? price * usd : 0;
  Value* o = tj::Obj(a);
  if (!o || !tj::Set(a, o, "symbol", tj::Clone(a, member(pool, "symbol"))) ||
      !tj::Set(a, o, "name", tj::Clone(a, member(pool, "name"))) ||
      !tj::Set(a, o, "current_price", number_or_null(a, has_price_usd, price_usd)) ||
      !tj::Set(a, o, "market_cap", number_or_null(a, has_cap && has_usd, has_cap && has_usd ? cap * usd : 0)) ||
      !tj::Set(a, o, "fully_diluted_valuation",
               number_or_null(a, has_price_usd && has_total, has_price_usd && has_total ? price_usd * total : 0)) ||
      !tj::Set(a, o, "total_supply", tj::Clone(a, member(pool, "totalAlpha"))) ||
      !tj::Set(a, o, "price_change_percentage_1h_in_currency", tj::Clone(a, member(pool, "change1h"))) ||
      !tj::Set(a, o, "price_change_percentage_24h", tj::Clone(a, member(pool, "change24h"))) ||
      !tj::Set(a, o, "price_change_percentage_7d_in_currency", tj::Clone(a, member(pool, "change7d"))) ||
      !tj::Set(a, o, "source", tj::Str(a, "taostats")))
    return nullptr;
  return o;
}

bool FormatEnUs(double value, char* out, size_t capacity) {
  if (!isfinite(value) || capacity < 2) return false;
  const bool negative = signbit(value);
  // The shortest round-trip digits (Number.prototype.toString): value =
  // 0.d0d1…dn-1 × 10^point.
  char text[64];
  chains::JsNumberText(fabs(value), text, sizeof(text));
  char digits[40];
  size_t n = 0;
  int before_point = -1;
  int exponent = 0;
  for (const char* p = text; *p; ++p) {
    if (*p == '.') {
      before_point = static_cast<int>(n);
    } else if (*p == 'e') {
      char* end = nullptr;
      errno = 0;
      const long parsed = std::strtol(p + 1, &end, 10);
      if (end == p + 1 || *end || errno == ERANGE || parsed < -400 || parsed > 400) return false;
      exponent = static_cast<int>(parsed);
      break;
    } else if (*p >= '0' && *p <= '9') {
      if (n + 1 >= sizeof(digits)) return false;
      digits[n++] = *p;
    }
  }
  if (before_point < 0) before_point = static_cast<int>(n);
  int point = before_point + exponent;
  size_t lead = 0;
  while (lead < n && digits[lead] == '0') ++lead;
  memmove(digits, digits + lead, n - lead);
  n -= lead;
  point -= static_cast<int>(lead);
  while (n && digits[n - 1] == '0') --n;
  // Round half away from zero at nine fraction digits.
  const int keep = point + 9;
  if (n && static_cast<int>(n) > keep) {
    if (keep < 0) {
      n = 0;
    } else {
      const bool up = digits[keep] >= '5';
      n = static_cast<size_t>(keep);
      if (up) {
        size_t i = n;
        while (i > 0 && digits[i - 1] == '9') --i;
        if (i == 0) {
          // Every kept digit carries (or none was kept): 10^point.
          digits[0] = '1';
          n = 1;
          ++point;
        } else {
          ++digits[i - 1];
          n = i;
        }
      }
      while (n && digits[n - 1] == '0') --n;
    }
  }
  size_t used = 0;
  auto put = [&](char c) {
    if (used + 1 >= capacity) return false;
    out[used++] = c;
    return true;
  };
  if (negative && !put('-')) return false;
  if (!n || point <= 0) {
    if (!put('0')) return false;
  } else {
    for (int i = 0; i < point; ++i) {
      if (i > 0 && (point - i) % 3 == 0 && !put(',')) return false;
      if (!put(static_cast<size_t>(i) < n ? digits[i] : '0')) return false;
    }
  }
  if (n && static_cast<int>(n) > point) {
    if (!put('.')) return false;
    for (int i = point; i < static_cast<int>(n); ++i)
      if (!put(i < 0 ? '0' : digits[i])) return false;
  }
  out[used] = '\0';
  return true;
}

// ---- WorkingSet --------------------------------------------------------------

WorkingSet::WorkingSet() = default;

WorkingSet::~WorkingSet() {
  for (Entry& entry : entries_) entry.blob.reset();

}

bool WorkingSet::Pool(uint32_t netuid, int64_t now_ms, Arena& arena, Value** page) {
  fortis::base::LockGuard lock(mutex_);
  bool found = false;
  for (Entry& entry : entries_) {
    if (!entry.used || entry.netuid != netuid || entry.expires <= now_ms) continue;
    *page = decode(arena, entry.blob.get(), entry.blob_length);
    found = *page != nullptr;
    break;
  }
  return found;
}

void WorkingSet::RememberPool(uint32_t netuid, int64_t now_ms, const Value* page) {
  uint8_t scratch[kMaxEntryBytes];
  size_t length = 0;
  const bool fits = encode(page, scratch, sizeof(scratch), &length);
  auto blob = fits ? base::AllocateStorage<uint8_t[]>(length) : nullptr;
  if (blob) memcpy(blob.get(), scratch, length);
  fortis::base::UniqueLock lock(mutex_);
  // rememberSubnet: drop expired entries, then (for a new netuid at the
  // bound) the oldest; the entry is re-inserted last.
  Entry* slot = nullptr;
  size_t used = 0;
  for (Entry& entry : entries_) {
    if (!entry.used) continue;
    if (entry.expires <= now_ms || entry.netuid == netuid) {
      entry.blob.reset();
      entry = Entry{};
      continue;
    }
    ++used;
  }
  if (used >= kMaxEntries) {
    Entry* oldest = nullptr;
    for (Entry& entry : entries_)
      if (entry.used && (!oldest || entry.order < oldest->order)) oldest = &entry;
    if (oldest) { oldest->blob.reset(); *oldest = Entry{}; }
  }
  for (Entry& entry : entries_) if (!entry.used) { slot = &entry; break; }
  // A page too large to copy (or out of memory) is not kept: the next read
  // goes to the provider.
  if (slot && blob) {
    slot->used = true;
    slot->netuid = netuid;
    slot->expires = now_ms + kTtlMs;
    slot->order = next_order_++;
    slot->blob = std::move(blob);
    slot->blob_length = length;
    blob = nullptr;
  }
  lock.Unlock();
  blob.reset();
}

bool WorkingSet::Price(int64_t now_ms, Arena& arena, Value** page) {
  fortis::base::UniqueLock lock(mutex_);
  const bool hit = price_set_ && price_expires_ > now_ms;
  const bool has = price_has_;
  const double price = price_;
  lock.Unlock();
  if (!hit) return false;
  *page = has ? tj::Num(arena, price) : tj::Null(arena);
  return *page != nullptr;
}

void WorkingSet::RememberPrice(int64_t now_ms, const Value* page) {
  fortis::base::LockGuard lock(mutex_);
  price_set_ = true;
  price_expires_ = now_ms + kTtlMs;
  price_has_ = tj::IsNumber(page);
  price_ = price_has_ ? page->number : 0;
}

size_t WorkingSet::pool_count() {
  fortis::base::LockGuard lock(mutex_);
  size_t count = 0;
  for (const Entry& entry : entries_) count += entry.used ? 1 : 0;
  return count;
}

size_t WorkingSet::retained_bytes() {
  fortis::base::LockGuard lock(mutex_);
  size_t bytes = 0;
  for (const Entry& entry : entries_) bytes += entry.used ? entry.blob_length : 0;
  return bytes;
}

WorkingSet* NewWorkingSet() {
  return fortis::base::New<WorkingSet>();
}

void DeleteWorkingSet(WorkingSet* set) {
  fortis::base::Delete(set);
}

}  // namespace fortis::tokens::token_bittensor
