#include "ownership.h"
#include "ai_snapshot.h"

#include "ai_proof.h"
#include "price_history_batch_codec.h"

#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::ai {
namespace {

namespace hb = fortis::codec::history_batch;
using tj::OutBuffer;
using tj::Value;

size_t units(const Value* v) { return hb::Utf16Units(v->text, v->length); }

// z.string().min(min).max(max)
bool zod_string(const Value* v, size_t min, size_t max) {
  if (!tj::IsString(v)) return false;
  const size_t n = units(v);
  return n >= min && n <= max;
}

bool digits(const char* text, size_t from, size_t to) {
  if (from >= to) return false;
  for (size_t i = from; i < to; ++i)
    if (text[i] < '0' || text[i] > '9') return false;
  return true;
}

// /^(0|[1-9]\d*)(\.\d{1,30})?$/
bool display_units(const Value* v) {
  if (!tj::IsString(v) || !v->length) return false;
  const char* text = v->text;
  const size_t length = v->length;
  size_t dot = 0;
  while (dot < length && text[dot] != '.') ++dot;
  if (!digits(text, 0, dot) || (text[0] == '0' && dot != 1)) return false;
  if (dot == length) return true;
  const size_t fraction = length - dot - 1;
  return fraction >= 1 && fraction <= 30 && digits(text, dot + 1, length);
}

// .strict(): exactly the listed keys (every one present; none other).
bool exact_keys(const Value* object, const char* const* keys, size_t count) {
  if (!tj::IsObject(object)) return false;
  if (object->count != count) return false;
  for (size_t i = 0; i < count; ++i)
    if (!tj::Get(object, keys[i])) return false;
  return true;
}

Value* wallet(const Value* v, tj::Arena& arena) {
  static const char* const kKeys[] = {"id", "address", "family", "networkIds", "watchOnly"};
  if (!exact_keys(v, kKeys, 5)) return nullptr;
  Value* id = tj::Get(v, "id");
  Value* address = tj::Get(v, "address");
  Value* family = tj::Get(v, "family");
  Value* networks = tj::Get(v, "networkIds");
  Value* watch = tj::Get(v, "watchOnly");
  if (!zod_string(id, 1, 128) || !zod_string(address, 1, 256) || !zod_string(family, 1, 40) ||
      !tj::IsArray(networks) || networks->count > 100 || !tj::IsBool(watch))
    return nullptr;
  for (uint32_t i = 0; i < networks->count; ++i)
    if (!zod_string(networks->items[i], 1, 80)) return nullptr;
  Value* out = tj::Obj(arena);
  if (!out || !tj::Set(arena, out, "id", id) || !tj::Set(arena, out, "address", address) ||
      !tj::Set(arena, out, "family", family) || !tj::Set(arena, out, "networkIds", networks) ||
      !tj::Set(arena, out, "watchOnly", watch))
    return nullptr;
  return out;
}

Value* token(const Value* v, tj::Arena& arena) {
  static const char* const kKeys[] = {"assetId", "networkId", "symbol", "decimals", "source"};
  if (!exact_keys(v, kKeys, 5)) return nullptr;
  Value* asset = tj::Get(v, "assetId");
  Value* network = tj::Get(v, "networkId");
  Value* symbol = tj::Get(v, "symbol");
  Value* decimals = tj::Get(v, "decimals");
  Value* source = tj::Get(v, "source");
  if (!zod_string(asset, 1, 256) || !zod_string(network, 1, 80) || !zod_string(symbol, 1, 64)) return nullptr;
  // z.number().int().min(0).max(30): finite, integral, in range.
  if (!tj::IsNumber(decimals) || !tj::IsFinite(decimals)) return nullptr;
  const double d = decimals->number;
  if (d < 0 || d > 30 || static_cast<double>(static_cast<int>(d)) != d) return nullptr;
  if (!tj::StringEquals(source, "native") && !tj::StringEquals(source, "tracked") &&
      !tj::StringEquals(source, "detected"))
    return nullptr;
  Value* out = tj::Obj(arena);
  if (!out || !tj::Set(arena, out, "assetId", asset) || !tj::Set(arena, out, "networkId", network) ||
      !tj::Set(arena, out, "symbol", symbol) || !tj::Set(arena, out, "decimals", decimals) ||
      !tj::Set(arena, out, "source", source))
    return nullptr;
  return out;
}

Value* holding(const Value* v, tj::Arena& arena) {
  static const char* const kKeys[] = {"walletId", "assetId", "displayUnits"};
  if (!exact_keys(v, kKeys, 3)) return nullptr;
  Value* wallet_id = tj::Get(v, "walletId");
  Value* asset = tj::Get(v, "assetId");
  Value* units_value = tj::Get(v, "displayUnits");
  if (!zod_string(wallet_id, 1, 128) || !zod_string(asset, 1, 256) || !display_units(units_value)) return nullptr;
  Value* out = tj::Obj(arena);
  if (!out || !tj::Set(arena, out, "walletId", wallet_id) || !tj::Set(arena, out, "assetId", asset) ||
      !tj::Set(arena, out, "displayUnits", units_value))
    return nullptr;
  return out;
}

Value* coverage(const Value* v, tj::Arena&) {
  static const char* const keys[] = {"walletId", "networkId", "status", "knownZero", "observedAt", "source", "stale"};
  if (!exact_keys(v, keys, 7) || !zod_string(tj::Get(v, "walletId"), 1, 128) ||
      !zod_string(tj::Get(v, "networkId"), 1, 80) ||
      (!tj::StringEquals(tj::Get(v, "status"), "observed") &&
       !tj::StringEquals(tj::Get(v, "status"), "unknown")) ||
      !tj::IsBool(tj::Get(v, "knownZero")) || !tj::IsBool(tj::Get(v, "stale")) ||
      !zod_string(tj::Get(v, "source"), 1, 40)) return nullptr;
  const Value* observed = tj::Get(v, "observedAt");
  if (observed->type != tj::Type::kNull &&
      (!tj::IsString(observed) || !IsZodDatetime(observed->text, observed->length))) return nullptr;
  if (tj::StringEquals(tj::Get(v, "status"), "unknown") &&
      (tj::Get(v, "knownZero")->type == tj::Type::kTrue || observed->type != tj::Type::kNull)) return nullptr;
  return const_cast<Value*>(v);
}

Value* price(const Value* v, tj::Arena&) {
  static const char* const keys[] = {"assetId", "usd", "lookup", "source", "observedAt", "stale"};
  if (!exact_keys(v, keys, 6) || !zod_string(tj::Get(v, "assetId"), 1, 256) ||
      !display_units(tj::Get(v, "usd")) || !zod_string(tj::Get(v, "lookup"), 1, 512) ||
      !zod_string(tj::Get(v, "source"), 1, 128) || !tj::IsBool(tj::Get(v, "stale"))) return nullptr;
  const Value* observed = tj::Get(v, "observedAt");
  if (observed->type != tj::Type::kNull &&
      (!tj::IsString(observed) || !IsZodDatetime(observed->text, observed->length))) return nullptr;
  return const_cast<Value*>(v);
}

bool decimal_v2(const Value* value) {
  if (!tj::IsString(value) || value->length == 0 || value->length > 128) return false;
  const char* text = value->text;
  const size_t length = value->length;
  size_t i = 0;
  while (i < length && text[i] >= '0' && text[i] <= '9') ++i;
  if (i == 0) return false;
  if (i < length && text[i] == '.') {
    const size_t first = ++i;
    while (i < length && text[i] >= '0' && text[i] <= '9') ++i;
    if (i == first) return false;
  }
  if (i < length && (text[i] == 'e' || text[i] == 'E')) {
    ++i;
    if (i < length && (text[i] == '+' || text[i] == '-')) ++i;
    const size_t first = i;
    while (i < length && text[i] >= '0' && text[i] <= '9') ++i;
    if (i == first || i - first > 3) return false;
  }
  return i == length;
}

Value* holding_v2(const Value* v, tj::Arena&) {
  static const char* const keys[] = {"walletId", "assetId", "rawAmount", "displayUnits", "precision"};
  if (!exact_keys(v, keys, 5) || !zod_string(tj::Get(v, "walletId"), 1, 128) ||
      !zod_string(tj::Get(v, "assetId"), 1, 256) ||
      !decimal_v2(tj::Get(v, "displayUnits")) ||
      (!tj::StringEquals(tj::Get(v, "precision"), "exact") &&
       !tj::StringEquals(tj::Get(v, "precision"), "stated") &&
       !tj::StringEquals(tj::Get(v, "precision"), "estimated"))) return nullptr;
  const Value* raw = tj::Get(v, "rawAmount");
  if (tj::StringEquals(tj::Get(v, "precision"), "estimated")) {
    if (raw->type != tj::Type::kNull) return nullptr;
  } else if (!zod_string(raw, 1, 100) || !digits(raw->text, 0, raw->length)) return nullptr;
  return const_cast<Value*>(v);
}

using ItemFn = Value* (*)(const Value*, tj::Arena&);
Value* array_of(const Value* v, size_t min, size_t max, ItemFn item, tj::Arena& arena) {
  if (!tj::IsArray(v) || v->count < min || v->count > max) return nullptr;
  Value* out = tj::Arr(arena);
  if (!out) return nullptr;
  for (uint32_t i = 0; i < v->count; ++i) {
    Value* parsed = item(v->items[i], arena);
    if (!parsed || !tj::Push(arena, out, parsed)) return nullptr;
  }
  return out;
}

// A string-keyed table over the arena (Map semantics: a later set wins).
struct Table {
  struct Slot { const Value* key; const Value* value; };
  Slot* slots = nullptr;
  size_t mask = 0;
  size_t size = 0;

  static uint64_t hash(const Value* key) {
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t i = 0; i < key->length; ++i) h = (h ^ static_cast<uint8_t>(key->text[i])) * 1099511628211ULL;
    return h;
  }
  static bool same(const Value* a, const Value* b) {
    return a->length == b->length && memcmp(a->text, b->text, a->length) == 0;
  }
  bool Init(size_t count, tj::Arena& arena) {
    size_t capacity = 16;
    while (capacity < count * 2) capacity *= 2;
    slots = static_cast<Slot*>(arena.Alloc(sizeof(Slot) * capacity));
    if (!slots) return false;
    memset(slots, 0, sizeof(Slot) * capacity);
    mask = capacity - 1;
    return true;
  }
  // False when the key was already present (its value is replaced).
  bool Set(const Value* key, const Value* value) {
    for (size_t i = hash(key) & mask;; i = (i + 1) & mask) {
      if (!slots[i].key) { slots[i] = {key, value}; ++size; return true; }
      if (same(slots[i].key, key)) { slots[i].value = value; return false; }
    }
  }
  const Value* Get(const Value* key) const {
    for (size_t i = hash(key) & mask;; i = (i + 1) & mask) {
      if (!slots[i].key) return nullptr;
      if (same(slots[i].key, key)) return slots[i].value;
    }
  }
};

size_t json_units(const Value* value, tj::Arena& arena, bool* ok) {
  OutBuffer counter{};
  if (!tj::Stringify(value, counter) || !counter.ok) { *ok = false; return 0; }
  // Measure the bytes, then count their UTF-16 units.
  uint8_t* bytes = static_cast<uint8_t*>(arena.Alloc(counter.used + 1, 1));
  if (!bytes) { *ok = false; return 0; }
  OutBuffer out{bytes, counter.used + 1};
  if (!tj::Stringify(value, out) || !out.ok) { *ok = false; return 0; }
  return hb::Utf16Units(reinterpret_cast<const char*>(bytes), out.used);
}

// Decimal multiplication in base 10. No floating point enters valuation.
// `cents` is little-endian and can represent every bounded v2 holding.
bool decimal_digits_le(const Value* value, uint8_t out[256], size_t* count, size_t* scale) {
  if (!tj::IsString(value) || !value->length || value->length > 128) return false;
  size_t dot = value->length, digits_count = 0;
  for (size_t i = 0; i < value->length; ++i) {
    const char c = value->text[i];
    if (c == '.') {
      if (dot != value->length || i == 0 || i + 1 == value->length) return false;
      dot = i;
    } else if (c < '0' || c > '9') return false;
    else ++digits_count;
  }
  if (!digits_count || digits_count > 128) return false;
  *scale = dot == value->length ? 0 : value->length - dot - 1;
  *count = 0;
  for (size_t i = value->length; i > 0; --i)
    if (value->text[i - 1] != '.') out[(*count)++] = static_cast<uint8_t>(value->text[i - 1] - '0');
  return true;
}

// Keep 160 fractional decimal places across all holdings, then round once.
// v2 bounds each displayed amount at 128 characters and USD quotes at 30
// fractional places, so no supported plain decimal is truncated here.
bool add_priced_total(const Value* amount, const Value* price, uint8_t total[512]) {
  uint8_t a[256]{}, b[256]{};
  uint16_t product[512]{};
  size_t an = 0, bn = 0, as = 0, bs = 0;
  if (!decimal_digits_le(amount, a, &an, &as) || !decimal_digits_le(price, b, &bn, &bs) ||
      as + bs > 160) return false;
  for (size_t i = 0; i < an; ++i)
    for (size_t j = 0; j < bn; ++j) product[i + j] += a[i] * b[j];
  for (size_t i = 0; i < 511; ++i) {
    product[i + 1] += product[i] / 10;
    product[i] %= 10;
  }
  const size_t append = 160 - as - bs;
  for (size_t i = 0; i + append < 512; ++i) {
    unsigned carry = product[i];
    for (size_t place = i + append; carry; ++place) {
      if (place >= 512) return false;
      carry += total[place];
      total[place] = static_cast<uint8_t>(carry % 10);
      carry /= 10;
    }
  }
  return true;
}

Value* total_text(tj::Arena& arena, const uint8_t total[512]) {
  uint8_t rounded[512];
  memcpy(rounded, total, sizeof(rounded));
  if (rounded[157] >= 5) {
    size_t place = 158;
    unsigned carry = 1;
    while (carry && place < 512) {
      carry += rounded[place];
      rounded[place++] = static_cast<uint8_t>(carry % 10);
      carry /= 10;
    }
    if (carry) return nullptr;
  }
  size_t high = 511;
  while (high > 160 && rounded[high] == 0) --high;
  char* text = static_cast<char*>(arena.Alloc(high - 154, 1));
  if (!text) return nullptr;
  size_t used = 0;
  for (size_t i = high + 1; i > 158; --i) {
    if (i == 160) text[used++] = '.';
    text[used++] = static_cast<char>('0' + rounded[i - 1]);
  }
  text[used] = '\0';
  return tj::Str(arena, text, used);
}

size_t decimal_digits(size_t value) {
  size_t n = 1;
  while (value >= 10) { value /= 10; ++n; }
  return n;
}

}  // namespace

static const Value* ValidateSnapshotV1(const Value* v, tj::Arena& arena) {
  static const char* const kKeys[] = {"version", "capturedAt", "balancesComplete", "wallets", "tokens", "holdings"};
  if (!exact_keys(v, kKeys, 6)) return nullptr;
  Value* version = tj::Get(v, "version");
  Value* captured = tj::Get(v, "capturedAt");
  Value* complete = tj::Get(v, "balancesComplete");
  if (!tj::IsNumber(version) || version->number != 1 || !tj::IsString(captured) ||
      !IsZodDatetime(captured->text, captured->length) || !tj::IsBool(complete))
    return nullptr;
  Value* wallets = array_of(tj::Get(v, "wallets"), 1, 100, wallet, arena);
  Value* tokens = wallets ? array_of(tj::Get(v, "tokens"), 0, 1000, token, arena) : nullptr;
  Value* holdings = tokens ? array_of(tj::Get(v, "holdings"), 0, 2000, holding, arena) : nullptr;
  Value* out = holdings ? tj::Obj(arena) : nullptr;
  // z.literal(1) outputs the literal itself (so -0 or 1.0 inputs read 1).
  if (!out || !tj::Set(arena, out, "version", tj::Num(arena, 1)) ||
      !tj::Set(arena, out, "capturedAt", captured) || !tj::Set(arena, out, "balancesComplete", complete) ||
      !tj::Set(arena, out, "wallets", wallets) || !tj::Set(arena, out, "tokens", tokens) ||
      !tj::Set(arena, out, "holdings", holdings))
    return nullptr;
  return out;
}

const Value* ValidateSnapshot(const Value* v, tj::Arena& arena) {
  const Value* version = tj::Get(v, "version");
  if (!tj::IsNumber(version)) return nullptr;
  if (version->number == 1) return ValidateSnapshotV1(v, arena);
  static const char* const keys[] = {"version", "capturedAt", "balancesComplete", "wallets", "tokens", "holdings", "coverage", "prices"};
  if (version->number != 2 || !exact_keys(v, keys, 8)) return nullptr;
  Value* captured = tj::Get(v, "capturedAt");
  Value* complete = tj::Get(v, "balancesComplete");
  if (!tj::IsString(captured) || !IsZodDatetime(captured->text, captured->length) || !tj::IsBool(complete)) return nullptr;
  Value* wallets = array_of(tj::Get(v, "wallets"), 1, 100, wallet, arena);
  Value* tokens = wallets ? array_of(tj::Get(v, "tokens"), 0, 1000, token, arena) : nullptr;
  Value* holdings = tokens ? array_of(tj::Get(v, "holdings"), 0, 2000, holding_v2, arena) : nullptr;
  Value* rows = holdings ? array_of(tj::Get(v, "coverage"), 0, 10000, coverage, arena) : nullptr;
  Value* prices = rows ? array_of(tj::Get(v, "prices"), 0, 1000, price, arena) : nullptr;
  Value* out = prices ? tj::Obj(arena) : nullptr;
  if (!out || !tj::Set(arena, out, "version", tj::Num(arena, 2)) ||
      !tj::Set(arena, out, "capturedAt", captured) || !tj::Set(arena, out, "balancesComplete", complete) ||
      !tj::Set(arena, out, "wallets", wallets) || !tj::Set(arena, out, "tokens", tokens) ||
      !tj::Set(arena, out, "holdings", holdings) || !tj::Set(arena, out, "coverage", rows) ||
      !tj::Set(arena, out, "prices", prices)) return nullptr;
  return out;
}

const Value* ParseSnapshot(const char* text, size_t length, tj::Arena& arena) {
  // JSON.parse does not skip a byte order mark (Response.json would).
  if (!text || (length >= 3 && memcmp(text, "\xEF\xBB\xBF", 3) == 0)) return nullptr;
  const Value* decoded = tj::Parse(arena, reinterpret_cast<const uint8_t*>(text), length);
  return decoded ? ValidateSnapshot(decoded, arena) : nullptr;
}

bool SnapshotLinksValid(const Value* snapshot) {
  const Value* wallets = tj::Get(snapshot, "wallets");
  const Value* tokens = tj::Get(snapshot, "tokens");
  const Value* holdings = tj::Get(snapshot, "holdings");
  if (!tj::IsArray(wallets) || !tj::IsArray(tokens) || !tj::IsArray(holdings)) return false;
  tj::Arena arena = tj::GrowingArena(4 * 1024 * 1024);
  std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
  Table wallet_ids, asset_ids;
  bool ok = wallet_ids.Init(wallets->count, arena) && asset_ids.Init(tokens->count, arena);
  for (uint32_t i = 0; ok && i < wallets->count; ++i)
    ok = wallet_ids.Set(tj::Get(wallets->items[i], "id"), wallets->items[i]);
  for (uint32_t i = 0; ok && i < tokens->count; ++i)
    ok = asset_ids.Set(tj::Get(tokens->items[i], "assetId"), tokens->items[i]);
  for (uint32_t i = 0; ok && i < holdings->count; ++i)
    ok = wallet_ids.Get(tj::Get(holdings->items[i], "walletId")) &&
         asset_ids.Get(tj::Get(holdings->items[i], "assetId"));
  if (ok && tj::Get(snapshot, "version")->number == 2) {
    const Value* coverage_rows = tj::Get(snapshot, "coverage");
    const Value* price_rows = tj::Get(snapshot, "prices");
    for (uint32_t i = 0; ok && i < coverage_rows->count; ++i) {
      const Value* row = coverage_rows->items[i];
      const Value* owner = wallet_ids.Get(tj::Get(row, "walletId"));
      ok = owner != nullptr;
      if (!ok) break;
      const Value* networks = tj::Get(owner, "networkIds");
      bool listed = false;
      for (uint32_t n = 0; n < networks->count; ++n)
        listed |= Table::same(networks->items[n], tj::Get(row, "networkId"));
      if (!listed) { ok = false; break; }
      if (tj::Get(row, "knownZero")->type == tj::Type::kTrue) {
        for (uint32_t h = 0; h < holdings->count; ++h) {
          const Value* held = holdings->items[h];
          const Value* token = asset_ids.Get(tj::Get(held, "assetId"));
          if (Table::same(tj::Get(held, "walletId"), tj::Get(row, "walletId")) &&
              Table::same(tj::Get(token, "networkId"), tj::Get(row, "networkId"))) ok = false;
        }
      }
      for (uint32_t j = 0; j < i; ++j)
        if (Table::same(tj::Get(coverage_rows->items[j], "walletId"), tj::Get(row, "walletId")) &&
            Table::same(tj::Get(coverage_rows->items[j], "networkId"), tj::Get(row, "networkId"))) ok = false;
    }
    for (uint32_t i = 0; ok && i < wallets->count; ++i) {
      const Value* owner = wallets->items[i];
      const Value* networks = tj::Get(owner, "networkIds");
      for (uint32_t n = 0; ok && n < networks->count; ++n) {
        size_t matching = 0;
        for (uint32_t c = 0; c < coverage_rows->count; ++c)
          if (Table::same(tj::Get(coverage_rows->items[c], "walletId"), tj::Get(owner, "id")) &&
              Table::same(tj::Get(coverage_rows->items[c], "networkId"), networks->items[n])) ++matching;
        ok = matching == 1;
      }
    }
    for (uint32_t i = 0; ok && i < holdings->count; ++i) {
      const Value* holding = holdings->items[i];
      const Value* token = asset_ids.Get(tj::Get(holding, "assetId"));
      bool observed = false, covered = false;
      for (uint32_t c = 0; c < coverage_rows->count; ++c) {
        const Value* row = coverage_rows->items[c];
        if (Table::same(tj::Get(row, "walletId"), tj::Get(holding, "walletId")) &&
            Table::same(tj::Get(row, "networkId"), tj::Get(token, "networkId"))) {
          covered = true;
          if (tj::StringEquals(tj::Get(row, "status"), "observed")) observed = true;
        }
      }
      ok = covered && (observed || tj::StringEquals(tj::Get(holding, "precision"), "estimated"));
    }
    for (uint32_t i = 0; ok && i < price_rows->count; ++i) {
      ok = asset_ids.Get(tj::Get(price_rows->items[i], "assetId")) != nullptr;
      for (uint32_t j = 0; ok && j < i; ++j)
        if (Table::same(tj::Get(price_rows->items[j], "assetId"),
                        tj::Get(price_rows->items[i], "assetId"))) ok = false;
    }
  }
  tj::FreeArena(arena);
  return ok;
}

bool PortfolioContext(const Value* snapshot, tj::Arena& arena, const char** text, size_t* length) {
  const Value* wallets = tj::Get(snapshot, "wallets");
  const Value* tokens = tj::Get(snapshot, "tokens");
  const Value* holdings = tj::Get(snapshot, "holdings");
  if (!tj::IsArray(wallets) || !tj::IsArray(tokens) || !tj::IsArray(holdings)) return false;
  Table wallet_map, token_map;
  if (!wallet_map.Init(wallets->count, arena) || !token_map.Init(tokens->count, arena)) return false;
  for (uint32_t i = 0; i < wallets->count; ++i) wallet_map.Set(tj::Get(wallets->items[i], "id"), wallets->items[i]);
  for (uint32_t i = 0; i < tokens->count; ++i) token_map.Set(tj::Get(tokens->items[i], "assetId"), tokens->items[i]);

  // holdings.slice(0, 300): only the selected entries are ever written.
  const size_t total = holdings->count;
  const size_t take = total < kContextHoldings ? total : kContextHoldings;
  Value* selected = tj::Arr(arena);
  if (!selected) return false;
  for (size_t i = 0; i < take; ++i) {
    const Value* row = holdings->items[i];
    const Value* wallet_id = tj::Get(row, "walletId");
    const Value* asset = tj::Get(row, "assetId");
    const Value* owner = wallet_map.Get(wallet_id);
    const Value* listed = token_map.Get(asset);
    Value* entry = tj::Obj(arena);
    Value* address = owner ? tj::Get(owner, "address") : nullptr;
    // `?.networkId` / `?.symbol` of a missing token are undefined: omitted.
    if (!entry || !tj::Set(arena, entry, "wallet", address ? address : const_cast<Value*>(wallet_id)) ||
        !tj::Set(arena, entry, "assetId", const_cast<Value*>(asset)) ||
        (listed && !tj::Set(arena, entry, "networkId", tj::Get(listed, "networkId"))) ||
        (listed && !tj::Set(arena, entry, "symbol", tj::Get(listed, "symbol"))) ||
        !tj::Set(arena, entry, "units", tj::Get(row, "displayUnits")) ||
        (tj::Get(row, "rawAmount") && !tj::Set(arena, entry, "rawAmount", tj::Get(row, "rawAmount"))) || !tj::Push(arena, selected, entry))
      return false;
  }
  Value* tracked = tj::Arr(arena);
  if (!tracked) return false;
  for (uint32_t i = 0; i < tokens->count && tracked->count < kContextTrackedTokens; ++i) {
    const Value* row = tokens->items[i];
    if (!tj::StringEquals(tj::Get(row, "source"), "tracked")) continue;
    Value* entry = tj::Obj(arena);
    if (!entry || !tj::Set(arena, entry, "networkId", tj::Get(row, "networkId")) ||
        !tj::Set(arena, entry, "symbol", tj::Get(row, "symbol")) || !tj::Push(arena, tracked, entry))
      return false;
  }
  Value* wallet_rows = tj::Arr(arena);
  if (!wallet_rows) return false;
  for (uint32_t i = 0; i < wallets->count; ++i) {
    const Value* row = wallets->items[i];
    Value* entry = tj::Obj(arena);
    if (!entry || !tj::Set(arena, entry, "address", tj::Get(row, "address")) ||
        !tj::Set(arena, entry, "family", tj::Get(row, "family")) ||
        !tj::Set(arena, entry, "watchOnly", tj::Get(row, "watchOnly")) || !tj::Push(arena, wallet_rows, entry))
      return false;
  }
  Value* coverage_rows = nullptr;
  Value* price_rows = nullptr;
  if (tj::Get(snapshot, "version")->number == 2) {
    coverage_rows = tj::Arr(arena);
    price_rows = tj::Arr(arena);
    if (!coverage_rows || !price_rows) return false;
    const Value* source_coverage = tj::Get(snapshot, "coverage");
    const Value* source_prices = tj::Get(snapshot, "prices");
    for (uint32_t i = 0; i < source_coverage->count && i < 300; ++i)
      if (!tj::Push(arena, coverage_rows, source_coverage->items[i])) return false;
    for (uint32_t i = 0; i < source_prices->count && i < 300; ++i)
      if (!tj::Push(arena, price_rows, source_prices->items[i])) return false;
  }
  Value* omitted = tj::Num(arena, static_cast<double>(total - take));
  Value* context = tj::Obj(arena);
  if (!omitted || !context || !tj::Set(arena, context, "capturedAt", tj::Get(snapshot, "capturedAt")) ||
      !tj::Set(arena, context, "balancesComplete", tj::Get(snapshot, "balancesComplete")) ||
      !tj::Set(arena, context, "holdings", selected) || !tj::Set(arena, context, "omittedHoldings", omitted) ||
      !tj::Set(arena, context, "wallets", wallet_rows) || !tj::Set(arena, context, "trackedTokens", tracked) ||
      (coverage_rows && (!tj::Set(arena, context, "coverage", coverage_rows) ||
                         !tj::Set(arena, context, "prices", price_rows))))
    return false;

  if (tj::Get(snapshot, "version")->number == 2) {
    const Value* source_prices = tj::Get(snapshot, "prices");
    const Value* source_coverage = tj::Get(snapshot, "coverage");
    Table price_map;
    if (!price_map.Init(source_prices->count, arena)) return false;
    for (uint32_t i = 0; i < source_prices->count; ++i)
      price_map.Set(tj::Get(source_prices->items[i], "assetId"), source_prices->items[i]);
    uint8_t cents[512]{};
    bool partial = tj::Get(snapshot, "balancesComplete")->type != tj::Type::kTrue;
    for (uint32_t i = 0; i < source_coverage->count; ++i) {
      const Value* row = source_coverage->items[i];
      if (!tj::StringEquals(tj::Get(row, "status"), "observed") ||
          tj::Get(row, "stale")->type == tj::Type::kTrue) partial = true;
    }
    for (uint32_t i = 0; i < holdings->count; ++i) {
      const Value* row = holdings->items[i];
      if (tj::StringEquals(tj::Get(row, "precision"), "estimated")) partial = true;
      const Value* quote = price_map.Get(tj::Get(row, "assetId"));
      if (!quote || !add_priced_total(tj::Get(row, "displayUnits"),
                                      tj::Get(quote, "usd"), cents)) partial = true;
      if (quote && tj::Get(quote, "stale")->type == tj::Type::kTrue) partial = true;
    }
    Value* total_usd = total_text(arena, cents);
    if (!total_usd || !tj::Set(arena, context, "cachedPricedTotalUsd", total_usd) ||
        !tj::Set(arena, context, "totalPartial", tj::Bool(arena, partial))) return false;
  }

  // The source pops one entry and re-serializes until the text fits; the
  // same lengths follow arithmetically from each entry's own length.
  bool ok = true;
  size_t text_units = json_units(context, arena, &ok);
  while (ok && text_units > kContextMaxUnits && price_rows && price_rows->count > 0) {
    const size_t last = json_units(price_rows->items[price_rows->count - 1], arena, &ok);
    text_units -= last + (price_rows->count > 1 ? 1 : 0);
    --price_rows->count;
  }
  while (ok && text_units > kContextMaxUnits && coverage_rows && coverage_rows->count > 0) {
    const size_t last = json_units(coverage_rows->items[coverage_rows->count - 1], arena, &ok);
    text_units -= last + (coverage_rows->count > 1 ? 1 : 0);
    --coverage_rows->count;
  }
  while (ok && text_units > kContextMaxUnits && tracked->count > 0) {
    const size_t last = json_units(tracked->items[tracked->count - 1], arena, &ok);
    text_units -= last + (tracked->count > 1 ? 1 : 0);
    --tracked->count;
  }
  while (ok && text_units > kContextMaxUnits && selected->count > 0) {
    const size_t last = json_units(selected->items[selected->count - 1], arena, &ok);
    const size_t before = total - selected->count;
    text_units -= last + (selected->count > 1 ? 1 : 0);
    --selected->count;
    const size_t after = total - selected->count;
    text_units = text_units - decimal_digits(before) + decimal_digits(after);
    omitted->number = static_cast<double>(after);
  }
  if (!ok) return false;
  OutBuffer counter{};
  if (!tj::Stringify(context, counter) || !counter.ok) return false;
  uint8_t* bytes = static_cast<uint8_t*>(arena.Alloc(counter.used + 1, 1));
  if (!bytes) return false;
  OutBuffer out{bytes, counter.used + 1};
  if (!tj::Stringify(context, out) || !out.ok) return false;
  bytes[out.used] = '\0';
  *text = reinterpret_cast<const char*>(bytes);
  *length = out.used;
  return true;
}

size_t Utf16Prefix(const char* text, size_t length, size_t limit) {
  size_t used = 0, i = 0;
  while (i < length && used < limit) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    const size_t width = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (width > length - i) return i;
    if (width == 4) {
      if (used + 2 > limit) {
        return i;
      }
      used += 2;
    } else {
      used += 1;
    }
    i += width;
  }
  return i;
}

bool Utf16Slice(const char* text, size_t length, size_t limit, tj::Arena& arena, const char** output,
                size_t* output_length) {
  const size_t prefix = Utf16Prefix(text, length, limit);
  char* copy = static_cast<char*>(arena.Alloc(prefix + 4, 1));
  if (!copy) return false;
  memcpy(copy, text, prefix);
  size_t used = prefix;
  copy[used] = '\0';
  *output = copy;
  *output_length = used;
  return true;
}

bool ZodTrimmedBody(const char* text, size_t length, size_t* begin, size_t* end) {
  hb::JsTrim(text, length, begin, end);
  const size_t n = hb::Utf16Units(text + *begin, *end - *begin);
  return n >= 1 && n <= kBodyMaxUnits;
}

}  // namespace fortis::ai
