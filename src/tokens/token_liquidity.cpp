#include "token_liquidity.h"

#include "provider_text.h"

#include <math.h>
#include <string.h>

namespace tj = fortis::codec::tj;

// JavaScript rounds every product and sum separately. Clang contracts
// `sum += a * b` into a fused multiply-add on arm64 by default, which moves
// weighted averages by one ulp (an inverted pool's price change in the R22
// oracle). GCC's ISO modes (the guest) never contract.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace fortis::tokens::token_liquidity {
namespace {

using tj::Arena;
using tj::Value;

constexpr const char* kWindows[4] = {"m5", "h1", "h6", "h24"};
constexpr double kSpreadLiquidityShare = 0.01;

Value* num_or_null(Arena& a, const Value* v) {
  return tj::IsFinite(v) ? tj::Num(a, v->number) : tj::Null(a);
}
Value* int_or_null(Arena& a, const Value* v) {
  return tj::IsFinite(v) ? tj::Num(a, trunc(v->number)) : tj::Null(a);
}
// A finite number field, or NaN for null/absent (`?? null` reads).
bool number_of(const Value* v, double* out) {
  if (!v || v->type != tj::Type::kNumber) return false;
  *out = v->number;
  return true;
}
bool is_null(const Value* v) { return !v || v->type == tj::Type::kNull; }

Value* windowed(Arena& a, const Value* raw, bool trades) {
  Value* out = tj::Obj(a);
  if (!out) return nullptr;
  for (const char* w : kWindows) {
    const Value* item = tj::Get(raw, w);
    Value* mapped = nullptr;
    if (!trades) {
      mapped = num_or_null(a, item);
    } else {
      Value* buys = int_or_null(a, tj::Get(item, "buys"));
      Value* sells = int_or_null(a, tj::Get(item, "sells"));
      if (!buys || !sells) return nullptr;
      if (is_null(buys) && is_null(sells)) {
        mapped = tj::Null(a);
      } else {
        mapped = tj::Obj(a);
        if (!mapped || !tj::Set(a, mapped, "buys", buys) || !tj::Set(a, mapped, "sells", sells))
          return nullptr;
      }
    }
    if (!mapped || !tj::Set(a, out, w, mapped)) return nullptr;
  }
  return out;
}

Value* string_or_null(Arena& a, const Value* v) {
  return tj::IsString(v) ? tj::Str(a, v->text, v->length) : tj::Null(a);
}

// `typeof x === 'string' ? (Number(x) || null) : …`
Value* numeric_string(Arena& a, const Value* v) {
  const double n = codec::provider_text::ToNumber(v->text, v->length);
  return (isnan(n) || n == 0) ? tj::Null(a) : tj::Num(a, n);
}

bool starts_with(const Value* v, const char* prefix) {
  const size_t n = strlen(prefix);
  return tj::IsString(v) && v->length >= n && memcmp(v->text, prefix, n) == 0;
}

// mapPair(p), including the transient chainId.
Value* map_pair(Arena& a, const Value* p) {
  const Value* liquidity = tj::Get(p, "liquidity");
  const Value* volume = tj::Get(p, "volume");
  const Value* base = tj::Get(p, "baseToken");
  const Value* quote = tj::Get(p, "quoteToken");
  const Value* labels = tj::Get(p, "labels");
  const Value* price_native = tj::Get(p, "priceNative");
  Value* out = tj::Obj(a);
  if (!out) return nullptr;
  bool ok = tj::Set(a, out, "chainId", string_or_null(a, tj::Get(p, "chainId"))) &&
            tj::Set(a, out, "dex", string_or_null(a, tj::Get(p, "dexId")));
  const Value* base_symbol = tj::Get(base, "symbol");
  const Value* quote_symbol = tj::Get(quote, "symbol");
  Value* label = nullptr;
  if (Truthy(base_symbol) && Truthy(quote_symbol)) {
    Value* left = TemplateString(a, base_symbol);
    Value* right = TemplateString(a, quote_symbol);
    if (!left || !right) return nullptr;
    char* text = static_cast<char*>(a.Alloc(left->length + right->length + 2, 1));
    if (!text) return nullptr;
    memcpy(text, left->text, left->length);
    text[left->length] = '/';
    memcpy(text + left->length + 1, right->text, right->length);
    label = tj::Str(a, text, left->length + 1 + right->length);
  } else {
    label = tj::Null(a);
  }
  ok = ok && tj::Set(a, out, "pairLabel", label) &&
       tj::Set(a, out, "pairAddress", string_or_null(a, tj::Get(p, "pairAddress"))) &&
       tj::Set(a, out, "baseAddress", string_or_null(a, tj::Get(base, "address"))) &&
       tj::Set(a, out, "quoteAddress", string_or_null(a, tj::Get(quote, "address"))) &&
       tj::Set(a, out, "liquidityUsd", num_or_null(a, tj::Get(liquidity, "usd"))) &&
       tj::Set(a, out, "volume24h", num_or_null(a, tj::Get(volume, "h24"))) &&
       tj::Set(a, out, "volume", windowed(a, tj::Get(p, "volume"), false)) &&
       tj::Set(a, out, "priceChange", windowed(a, tj::Get(p, "priceChange"), false)) &&
       tj::Set(a, out, "txns", windowed(a, tj::Get(p, "txns"), true)) &&
       tj::Set(a, out, "baseAmount", num_or_null(a, tj::Get(liquidity, "base"))) &&
       tj::Set(a, out, "quoteAmount", num_or_null(a, tj::Get(liquidity, "quote")));
  Value* native = tj::IsString(price_native) ? numeric_string(a, price_native)
                                             : num_or_null(a, price_native);
  Value* protocol = tj::Null(a);
  if (tj::IsArray(labels) && labels->count > 0 && tj::IsString(labels->items[0]))
    protocol = codec::provider_text::ToLowerCase(a, labels->items[0]->text, labels->items[0]->length);
  const Value* url = tj::Get(p, "url");
  const Value* price_usd = tj::Get(p, "priceUsd");
  ok = ok && tj::Set(a, out, "priceNative", native) && tj::Set(a, out, "protocolLabel", protocol) &&
       tj::Set(a, out, "url", starts_with(url, "https://") ? tj::Str(a, url->text, url->length)
                                                          : tj::Null(a)) &&
       tj::Set(a, out, "pairCreatedAt", num_or_null(a, tj::Get(p, "pairCreatedAt"))) &&
       tj::Set(a, out, "fdv", num_or_null(a, tj::Get(p, "fdv"))) &&
       tj::Set(a, out, "marketCap", num_or_null(a, tj::Get(p, "marketCap"))) &&
       tj::Set(a, out, "priceUsd", tj::IsString(price_usd) ? numeric_string(a, price_usd)
                                                           : tj::Null(a));
  return ok ? out : nullptr;
}

bool text_equal(const Value* a, const Value* b) {
  return a->length == b->length && memcmp(a->text, b->text, a->length) == 0;
}

// The dedupe key of one pool as a string value.
Value* dedupe_key(Arena& a, const Value* pool) {
  const Value* pair = tj::Get(pool, "pairAddress");
  if (tj::IsString(pair)) return ChainAddressKey(a, pair->text, pair->length);
  const Value* parts[3] = {tj::Get(pool, "dex"), tj::Get(pool, "baseAddress"),
                           tj::Get(pool, "quoteAddress")};
  size_t length = 2;
  for (const Value* part : parts) if (tj::IsString(part)) length += part->length;
  char* text = static_cast<char*>(a.Alloc(length + 1, 1));
  if (!text) return nullptr;
  size_t used = 0;
  for (size_t i = 0; i < 3; ++i) {
    if (i) text[used++] = '\0';
    if (tj::IsString(parts[i])) { memcpy(text + used, parts[i]->text, parts[i]->length); used += parts[i]->length; }
  }
  return tj::Str(a, text, used);
}

double liquidity_or_zero(const Value* pool) {
  double v = 0;
  return number_of(tj::Get(pool, "liquidityUsd"), &v) ? v : 0;
}

// [...pools].sort(byLiquidityDesc): stable insertion sort.
Value** sorted_by_liquidity(Arena& a, Value* const* pools, size_t count) {
  Value** out = static_cast<Value**>(a.Alloc(sizeof(Value*) * (count + 1)));
  if (!out) return nullptr;
  for (size_t i = 0; i < count; ++i) {
    Value* item = pools[i];
    const double key = liquidity_or_zero(item);
    size_t j = i;
    // comparator (b.l ?? 0) - (a.l ?? 0) > 0 means b before a
    while (j > 0 && (key - liquidity_or_zero(out[j - 1])) > 0) { out[j] = out[j - 1]; --j; }
    out[j] = item;
  }
  return out;
}

double invert_percent(double v) {
  const double ratio = 1 + v / 100;
  return ratio > 0 ? (1 / ratio - 1) * 100 : NAN;
}

Value* orient_quote(Arena& a, const Value* pool) {
  Value* out = tj::Clone(a, pool);
  if (!out) return nullptr;
  double native = 0, price = 0;
  const bool has_native = number_of(tj::Get(pool, "priceNative"), &native);
  const bool has_price = number_of(tj::Get(pool, "priceUsd"), &price);
  Value* quote_price = has_price && has_native && native > 0 ? tj::Num(a, price / native) : tj::Null(a);
  Value* inverse = has_native && native > 0 ? tj::Num(a, 1 / native) : tj::Null(a);
  Value* change = tj::Obj(a);
  Value* trades = tj::Obj(a);
  if (!change || !trades) return nullptr;
  const Value* pc = tj::Get(pool, "priceChange");
  const Value* tx = tj::Get(pool, "txns");
  for (const char* w : kWindows) {
    double v = 0;
    Value* mapped = nullptr;
    if (number_of(tj::Get(pc, w), &v)) {
      const double inverted = invert_percent(v);
      mapped = isnan(inverted) ? tj::Null(a) : tj::Num(a, inverted);
    } else {
      mapped = tj::Null(a);
    }
    if (!tj::Set(a, change, w, mapped)) return nullptr;
    const Value* counts = tj::Get(tx, w);
    Value* swapped = nullptr;
    if (is_null(counts)) {
      swapped = tj::Null(a);
    } else {
      swapped = tj::Obj(a);
      const Value* sells = tj::Get(counts, "sells");
      const Value* buys = tj::Get(counts, "buys");
      if (!swapped || !tj::Set(a, swapped, "buys", sells ? tj::Clone(a, sells) : tj::Null(a)) ||
          !tj::Set(a, swapped, "sells", buys ? tj::Clone(a, buys) : tj::Null(a))) return nullptr;
    }
    if (!tj::Set(a, trades, w, swapped)) return nullptr;
  }
  if (!tj::Set(a, out, "fdv", tj::Null(a)) || !tj::Set(a, out, "marketCap", tj::Null(a)) ||
      !tj::Set(a, out, "priceUsd", quote_price) || !tj::Set(a, out, "priceNative", inverse) ||
      !tj::Set(a, out, "priceChange", change) || !tj::Set(a, out, "txns", trades)) return nullptr;
  return out;
}

Value* nullable_sum(Arena& a, Value* const* pools, size_t count, const char* field) {
  bool any = false;
  double sum = 0;
  for (size_t i = 0; i < count; ++i) {
    double v = 0;
    if (number_of(tj::Get(pools[i], field), &v)) {
      sum = any ? sum + v : 0 + v;
      any = true;
    }
  }
  return any ? tj::Num(a, sum) : tj::Null(a);
}

Value* copy_or_null(Arena& a, const Value* v) { return v ? tj::Clone(a, v) : tj::Null(a); }

}  // namespace

bool Truthy(const Value* v) {
  if (!v) return false;
  switch (v->type) {
    case tj::Type::kNull: case tj::Type::kFalse: return false;
    case tj::Type::kTrue: return true;
    case tj::Type::kNumber: return !(v->number == 0 || isnan(v->number));
    case tj::Type::kString: return v->length > 0;
    default: return true;
  }
}

Value* TemplateString(Arena& a, const Value* v) {
  if (!v) return tj::Str(a, "undefined");
  switch (v->type) {
    case tj::Type::kNull: return tj::Str(a, "null");
    case tj::Type::kTrue: return tj::Str(a, "true");
    case tj::Type::kFalse: return tj::Str(a, "false");
    case tj::Type::kString: return tj::Str(a, v->text, v->length);
    case tj::Type::kNumber: {
      uint8_t buffer[64];
      tj::OutBuffer out{buffer, sizeof(buffer), 0, true};
      codec::history_batch::AppendJsNumber(out, v->number);
      return tj::Str(a, reinterpret_cast<const char*>(buffer), out.used);
    }
    case tj::Type::kObject: return tj::Str(a, "[object Object]");
    case tj::Type::kArray: {
      // Array.prototype.join(','): null/undefined elements are empty.
      size_t total = 0;
      Value** parts = static_cast<Value**>(a.Alloc(sizeof(Value*) * (v->count + 1)));
      if (!parts) return nullptr;
      for (uint32_t i = 0; i < v->count; ++i) {
        parts[i] = v->items[i]->type == tj::Type::kNull ? tj::Str(a, "") : TemplateString(a, v->items[i]);
        if (!parts[i]) return nullptr;
        total += parts[i]->length + 1;
      }
      char* text = static_cast<char*>(a.Alloc(total + 1, 1));
      if (!text) return nullptr;
      size_t used = 0;
      for (uint32_t i = 0; i < v->count; ++i) {
        if (i) text[used++] = ',';
        memcpy(text + used, parts[i]->text, parts[i]->length);
        used += parts[i]->length;
      }
      return tj::Str(a, text, used);
    }
  }
  return nullptr;
}

Value* ChainAddressKey(Arena& a, const char* text, size_t length) {
  if (length >= 2 && text[0] == '0' && text[1] == 'x') return codec::provider_text::ToLowerCase(a, text, length);
  return tj::Str(a, text, length);
}

bool TokenAddressesEqual(Arena& a, const Value* left, const char* right) {
  if (!tj::IsString(left)) return false;
  Value* l = ChainAddressKey(a, left->text, left->length);
  Value* r = ChainAddressKey(a, right, strlen(right));
  return l && r && text_equal(l, r);
}

PoolsStatus DexScreenerPools(Arena& a, const Value* body, const char* address,
                             const char* chain_slug, Value** pools) {
  *pools = nullptr;
  if (!body || body->type == tj::Type::kNull) return PoolsStatus::kFailed;
  const Value* pairs = tj::Get(body, "pairs");
  Value* out = tj::Arr(a);
  Value** keys = nullptr;
  if (!out) return PoolsStatus::kArena;
  const size_t count = tj::IsArray(pairs) ? pairs->count : 0;
  keys = static_cast<Value**>(a.Alloc(sizeof(Value*) * (count + 1)));
  if (!keys) return PoolsStatus::kArena;
  // Every pair is mapped first (a null element fails the whole read).
  Value** mapped = static_cast<Value**>(a.Alloc(sizeof(Value*) * (count + 1)));
  if (!mapped) return PoolsStatus::kArena;
  for (size_t i = 0; i < count; ++i) {
    if (pairs->items[i]->type == tj::Type::kNull) return PoolsStatus::kFailed;
    mapped[i] = map_pair(a, pairs->items[i]);
    if (!mapped[i]) return PoolsStatus::kArena;
  }
  size_t kept = 0;
  for (size_t i = 0; i < count; ++i) {
    Value* pool = mapped[i];
    if (!tj::StringEquals(tj::Get(pool, "chainId"), chain_slug)) continue;
    if (!TokenAddressesEqual(a, tj::Get(pool, "baseAddress"), address) &&
        !TokenAddressesEqual(a, tj::Get(pool, "quoteAddress"), address)) continue;
    tj::Remove(pool, "chainId");
    Value* key = dedupe_key(a, pool);
    if (!key) return PoolsStatus::kArena;
    bool seen = false;
    for (size_t k = 0; k < kept && !seen; ++k) seen = text_equal(keys[k], key);
    if (seen) continue;
    keys[kept++] = key;
    if (!tj::Push(a, out, pool)) return PoolsStatus::kArena;
  }
  *pools = out;
  return a.ok ? PoolsStatus::kOk : PoolsStatus::kArena;
}

bool CalculateLiquiditySnapshot(Arena& a, const Value* pools, const char* address,
                                double displayed_pool_limit, Value** snapshot) {
  *snapshot = nullptr;
  if (!tj::IsArray(pools)) return false;
  Value** all = static_cast<Value**>(a.Alloc(sizeof(Value*) * (pools->count + 1)));
  if (!all) return false;
  size_t count = 0;
  for (uint32_t i = 0; i < pools->count; ++i) {
    Value* pool = pools->items[i];
    if (!address) { all[count++] = pool; continue; }
    if (TokenAddressesEqual(a, tj::Get(pool, "baseAddress"), address)) { all[count++] = pool; continue; }
    if (!TokenAddressesEqual(a, tj::Get(pool, "quoteAddress"), address)) continue;
    Value* oriented = orient_quote(a, pool);
    if (!oriented) return false;
    all[count++] = oriented;
  }
  if (count == 0) return a.ok;
  Value** sorted = sorted_by_liquidity(a, all, count);
  if (!sorted) return false;
  Value* total = nullable_sum(a, all, count, "liquidityUsd");
  if (!total) return false;
  const double total_usd = total->type == tj::Type::kNumber ? total->number : 0;
  const Value* base = sorted[0];

  Value* out = tj::Obj(a);
  if (!out) return false;
  // oldestPairCreatedAt reduce
  bool any_created = false;
  double oldest = 0;
  for (size_t i = 0; i < count; ++i) {
    double created = 0;
    if (!number_of(tj::Get(all[i], "pairCreatedAt"), &created)) continue;
    oldest = any_created ? fmin(oldest, created) : created;
    if (any_created && (isnan(oldest) || isnan(created))) oldest = NAN;
    any_created = true;
  }
  bool ok = tj::Set(a, out, "totalUsd", total) &&
            tj::Set(a, out, "poolCount", tj::Num(a, static_cast<double>(count))) &&
            tj::Set(a, out, "volume24hUsd", nullable_sum(a, all, count, "volume24h")) &&
            tj::Set(a, out, "oldestPairCreatedAt", any_created ? tj::Num(a, oldest) : tj::Null(a)) &&
            tj::Set(a, out, "fdv", copy_or_null(a, tj::Get(base, "fdv"))) &&
            tj::Set(a, out, "priceUsd", copy_or_null(a, tj::Get(base, "priceUsd")));
  if (!ok) return false;

  // aggregateActivity
  Value* volume_usd = tj::Obj(a);
  Value* txns = tj::Obj(a);
  Value* change_pct = tj::Obj(a);
  if (!volume_usd || !txns || !change_pct) return false;
  for (const char* w : kWindows) {
    bool has_volume = false, has_buys = false, has_sells = false;
    double volume = 0, buys = 0, sells = 0;
    double weighted = 0, weight = 0, sum = 0;
    size_t changes = 0;
    for (size_t i = 0; i < count; ++i) {
      const Value* pool = all[i];
      double v = 0;
      if (number_of(tj::Get(tj::Get(pool, "volume"), w), &v)) { volume = (has_volume ? volume : 0) + v; has_volume = true; }
      const Value* counts = tj::Get(tj::Get(pool, "txns"), w);
      if (number_of(tj::Get(counts, "buys"), &v)) { buys = (has_buys ? buys : 0) + v; has_buys = true; }
      if (number_of(tj::Get(counts, "sells"), &v)) { sells = (has_sells ? sells : 0) + v; has_sells = true; }
      double change = 0;
      if (number_of(tj::Get(tj::Get(pool, "priceChange"), w), &change)) {
        const double liquidity = liquidity_or_zero(pool);
        weighted += change * liquidity;
        weight += liquidity;
        sum += change;
        ++changes;
      }
    }
    Value* trade = nullptr;
    if (!has_buys && !has_sells) {
      trade = tj::Null(a);
    } else {
      trade = tj::Obj(a);
      if (!trade || !tj::Set(a, trade, "buys", has_buys ? tj::Num(a, buys) : tj::Null(a)) ||
          !tj::Set(a, trade, "sells", has_sells ? tj::Num(a, sells) : tj::Null(a))) return false;
    }
    Value* change = changes == 0 ? tj::Null(a)
        : tj::Num(a, weight > 0 ? weighted / weight : sum / static_cast<double>(changes));
    if (!tj::Set(a, volume_usd, w, has_volume ? tj::Num(a, volume) : tj::Null(a)) ||
        !tj::Set(a, txns, w, trade) || !tj::Set(a, change_pct, w, change)) return false;
  }
  // priced = pools with (priceUsd ?? 0) > 0
  double price_weight = 0, weighted_price = 0, price_sum = 0;
  size_t priced = 0;
  for (size_t i = 0; i < count; ++i) {
    double price = 0;
    if (!number_of(tj::Get(all[i], "priceUsd"), &price) || !(price > 0)) continue;
    ++priced;
    price_weight += liquidity_or_zero(all[i]);
  }
  bool has_weighted = priced > 0;
  if (has_weighted) {
    if (price_weight > 0) {
      for (size_t i = 0; i < count; ++i) {
        double price = 0;
        if (!number_of(tj::Get(all[i], "priceUsd"), &price) || !(price > 0)) continue;
        weighted_price += price * liquidity_or_zero(all[i]);
      }
      weighted_price /= price_weight;
    } else {
      for (size_t i = 0; i < count; ++i) {
        double price = 0;
        if (!number_of(tj::Get(all[i], "priceUsd"), &price) || !(price > 0)) continue;
        price_sum += price;
      }
      weighted_price = price_sum / static_cast<double>(priced);
    }
  }
  // tradeable: priced pools with (liquidityUsd ?? 0) >= totalUsd * 0.01
  double max_price = -INFINITY, min_price = INFINITY;
  size_t tradeable = 0;
  for (size_t i = 0; i < count; ++i) {
    double price = 0;
    if (!number_of(tj::Get(all[i], "priceUsd"), &price) || !(price > 0)) continue;
    if (!(liquidity_or_zero(all[i]) >= total_usd * kSpreadLiquidityShare)) continue;
    ++tradeable;
    if (price > max_price) max_price = price;
    if (price < min_price) min_price = price;
  }
  const bool has_spread = tradeable > 1 && has_weighted && weighted_price != 0 && !isnan(weighted_price);
  const double spread = has_spread ? ((max_price - min_price) / weighted_price) * 100 : 0;
  const Value* deepest = sorted[0];
  double deepest_liquidity = 0;
  const bool deepest_truthy = number_of(tj::Get(deepest, "liquidityUsd"), &deepest_liquidity) &&
                              deepest_liquidity != 0 && !isnan(deepest_liquidity);
  ok = tj::Set(a, out, "volumeUsd", volume_usd) && tj::Set(a, out, "txns", txns) &&
       tj::Set(a, out, "priceChangePct", change_pct) &&
       tj::Set(a, out, "weightedPriceUsd", has_weighted ? tj::Num(a, weighted_price) : tj::Null(a)) &&
       tj::Set(a, out, "priceSpreadPct", has_spread ? tj::Num(a, spread) : tj::Null(a)) &&
       tj::Set(a, out, "priceSpreadPools",
               has_spread ? tj::Num(a, static_cast<double>(tradeable)) : tj::Null(a)) &&
       tj::Set(a, out, "marketCap", copy_or_null(a, tj::Get(deepest, "marketCap"))) &&
       tj::Set(a, out, "topPoolShare", total_usd > 0 && deepest_truthy
                                           ? tj::Num(a, deepest_liquidity / total_usd)
                                           : tj::Null(a));
  if (!ok) return false;
  Value* shown = tj::Arr(a);
  if (!shown) return false;
  const double limit = displayed_pool_limit > 0 ? displayed_pool_limit : 0;
  for (size_t i = 0; i < count && static_cast<double>(i) < limit; ++i)
    if (!tj::Push(a, shown, sorted[i])) return false;
  if (!tj::Set(a, out, "pools", shown)) return false;
  *snapshot = out;
  return a.ok;
}

Value* CompactSnapshot(Arena& a, const Value* snapshot, double limit) {
  if (!tj::IsObject(snapshot)) return nullptr;
  Value* out = tj::Obj(a);
  if (!out) return nullptr;
  for (uint32_t i = 0; i < snapshot->count; ++i)
    if (!tj::Set(a, out, snapshot->members[i].key, snapshot->members[i].key_length,
                 snapshot->members[i].value)) return nullptr;
  const Value* pools = tj::Get(snapshot, "pools");
  Value* compact = tj::Arr(a);
  if (!compact) return nullptr;
  for (uint32_t i = 0; tj::IsArray(pools) && i < pools->count && static_cast<double>(i) < limit; ++i)
    if (!tj::Push(a, compact, pools->items[i])) return nullptr;
  return tj::Set(a, out, "pools", compact) ? out : nullptr;
}

}  // namespace fortis::tokens::token_liquidity
