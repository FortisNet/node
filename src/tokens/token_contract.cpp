#include <algorithm>
#include "token_contract.h"

#include "provider_text.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::tokens::token_contract {
namespace {

using tj::Arena;
using tj::Value;

constexpr uint32_t kSupportedEvm[] = {
    1, 10, 25, 56, 100, 130, 137, 146, 169, 177, 185, 196, 204, 321, 324, 480,
    988, 1030, 1514, 1625, 1672, 1868, 2741, 2818, 4200, 4663, 5000, 8453, 9745,
    42161, 42766, 43114, 48900, 59144, 81457, 143, 200901, 201022, 534352,
    810180, 5734951, 688688,
};

bool supported_evm(uint64_t id) {
  for (uint32_t s : kSupportedEvm) if (s == id) return true;
  return false;
}

enum class Dialect { kNone, kEvm, kSolana };

Dialect dialect_for(const char* chain) {
  if (strcmp(chain, "solana") == 0) return Dialect::kSolana;
  if (strcmp(chain, "tron") == 0) return Dialect::kEvm;
  if (!*chain) return Dialect::kNone;
  uint64_t value = 0;
  for (const char* p = chain; *p; ++p) {
    if (*p < '0' || *p > '9') return Dialect::kNone;
    if (value < 100000000000ULL) value = value * 10 + static_cast<uint64_t>(*p - '0');
  }
  return supported_evm(value) ? Dialect::kEvm : Dialect::kNone;
}

void append_component(tj::OutBuffer& out, const char* text) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
    const unsigned char c = *p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' ||
        c == '(' || c == ')') {
      out.Byte(c);
    } else {
      out.Byte('%'); out.Byte(static_cast<uint8_t>(kHex[c >> 4])); out.Byte(static_cast<uint8_t>(kHex[c & 15]));
    }
  }
}

// ---- field parsers -----------------------------------------------------------

// flag(): '1' | 1 | true -> true; '0' | 0 | false -> false.
Value* flag(Arena& a, const Value* v) {
  if (!v) return tj::Null(a);
  if (v->type == tj::Type::kTrue || (v->type == tj::Type::kNumber && v->number == 1) ||
      tj::StringEquals(v, "1")) return tj::Bool(a, true);
  if (v->type == tj::Type::kFalse || (v->type == tj::Type::kNumber && v->number == 0) ||
      tj::StringEquals(v, "0")) return tj::Bool(a, false);
  return tj::Null(a);
}

bool num_value(const Value* v, double* out) {
  if (!v) return false;
  if (v->type == tj::Type::kNumber) { if (!isfinite(v->number)) return false; *out = v->number; return true; }
  if (v->type != tj::Type::kString) return false;
  size_t begin = 0, end = 0;
  codec::provider_text::Trim(v->text, v->length, &begin, &end);
  if (begin == end) return false;
  const double parsed = codec::provider_text::ToNumber(v->text, v->length);
  if (!isfinite(parsed)) return false;
  *out = parsed;
  return true;
}
Value* num(Arena& a, const Value* v) { double n = 0; return num_value(v, &n) ? tj::Num(a, n) : tj::Null(a); }
Value* int_(Arena& a, const Value* v) { double n = 0; return num_value(v, &n) ? tj::Num(a, trunc(n)) : tj::Null(a); }
Value* percent(Arena& a, const Value* v) { double n = 0; return num_value(v, &n) ? tj::Num(a, n * 100) : tj::Null(a); }

Value* str(Arena& a, const Value* v) {
  if (!tj::IsString(v)) return tj::Null(a);
  size_t begin = 0, end = 0;
  codec::provider_text::Trim(v->text, v->length, &begin, &end);
  if (begin == end) return tj::Null(a);
  return tj::Str(a, v->text + begin, end - begin);
}
bool is_null(const Value* v) { return !v || v->type == tj::Type::kNull; }
const Value* record(const Value* v) { return tj::IsObject(v) ? v : nullptr; }

Value* lower(Arena& a, const Value* s) { return codec::provider_text::ToLowerCase(a, s->text, s->length); }
bool same_text(const Value* x, const Value* y) {
  return x && y && x->length == y->length && memcmp(x->text, y->text, x->length) == 0;
}

bool parse_date(const Value* s, double* ms) {
  return tj::IsString(s) && codec::provider_text::DateParse(s->text, s->length, ms);
}

// lockEnd(locked_detail): the latest parseable end_time.
bool lock_end(Arena& a, const Value* raw, double* latest) {
  if (!tj::IsArray(raw)) return false;
  bool any = false;
  for (uint32_t i = 0; i < raw->count; ++i) {
    Value* end = str(a, tj::Get(record(raw->items[i]), "end_time"));
    double parsed = 0;
    if (!end || is_null(end) || !parse_date(end, &parsed)) continue;
    if (!any || parsed > *latest) *latest = parsed;
    any = true;
  }
  return any;
}

Value* iso_or_null(Arena& a, double ms) {
  char text[32];
  if (!codec::provider_text::IsoString(static_cast<int64_t>(ms), text)) return tj::Null(a);
  return tj::Str(a, text);
}

Value* holders(Arena& a, const Value* raw, bool lp) {
  Value* out = tj::Arr(a);
  if (!out || !tj::IsArray(raw)) return out;
  for (uint32_t i = 0; i < raw->count; ++i) {
    const Value* held = record(raw->items[i]);
    if (!held) continue;
    Value* address = str(a, tj::Get(held, "address"));
    if (!address) return nullptr;
    if (is_null(address)) address = str(a, tj::Get(held, "account"));
    if (!address) return nullptr;
    if (is_null(address)) continue;
    Value* row = tj::Obj(a);
    if (!row) return nullptr;
    bool ok = tj::Set(a, row, "address", address) && tj::Set(a, row, "tag", str(a, tj::Get(held, "tag"))) &&
              tj::Set(a, row, "isContract", flag(a, tj::Get(held, "is_contract"))) &&
              tj::Set(a, row, "isLocked", flag(a, tj::Get(held, "is_locked"))) &&
              tj::Set(a, row, "balance", num(a, tj::Get(held, "balance"))) &&
              tj::Set(a, row, "percent", percent(a, tj::Get(held, "percent")));
    if (ok && lp) {
      double end = 0;
      ok = tj::Set(a, row, "lockedUntil", lock_end(a, tj::Get(held, "locked_detail"), &end)
                                              ? iso_or_null(a, end) : tj::Null(a));
    }
    if (!ok || !tj::Push(a, out, row)) return nullptr;
  }
  return out;
}

Value* pools(Arena& a, const Value* raw, bool solana) {
  Value* out = tj::Arr(a);
  if (!out || !tj::IsArray(raw)) return out;
  for (uint32_t i = 0; i < raw->count; ++i) {
    const Value* pool = record(raw->items[i]);
    if (!pool) continue;
    Value* row = tj::Obj(a);
    if (!row ||
        !tj::Set(a, row, "dex", str(a, tj::Get(pool, solana ? "dex_name" : "name"))) ||
        !tj::Set(a, row, "pair", str(a, tj::Get(pool, solana ? "id" : "pair"))) ||
        !tj::Set(a, row, "liquidity", num(a, tj::Get(pool, solana ? "tvl" : "liquidity"))) ||
        !tj::Set(a, row, "liquidityType", str(a, tj::Get(pool, solana ? "type" : "liquidity_type"))) ||
        !tj::Set(a, row, "feePct", percent(a, tj::Get(pool, solana ? "fee_rate" : "pool_fee"))) ||
        !tj::Push(a, out, row)) return nullptr;
  }
  return out;
}

Value* cex_listed(Arena& a, const Value* v) {
  const Value* held = record(v);
  if (!held) return flag(a, v);
  Value* listed = flag(a, tj::Get(held, "listed"));
  if (!listed || !is_null(listed)) return listed;
  const Value* list = tj::Get(held, "cex_list");
  return tj::IsArray(list) ? tj::Bool(a, list->count > 0) : tj::Null(a);
}

Value* cex_names(Arena& a, const Value* v) {
  Value* out = tj::Arr(a);
  const Value* list = tj::Get(record(v), "cex_list");
  if (!out || !tj::IsArray(list)) return out;
  Value** seen = static_cast<Value**>(a.Alloc(sizeof(Value*) * (list->count + 1)));
  if (!seen) return nullptr;
  size_t seen_count = 0;
  for (uint32_t i = 0; i < list->count; ++i) {
    const Value* item = list->items[i];
    if (!tj::IsString(item)) continue;
    size_t begin = 0, end = 0;
    codec::provider_text::Trim(item->text, item->length, &begin, &end);
    if (begin == end) continue;
    Value* name = tj::Str(a, item->text + begin, end - begin);
    Value* key = name ? lower(a, name) : nullptr;
    if (!key) return nullptr;
    bool dup = false;
    for (size_t k = 0; k < seen_count && !dup; ++k) dup = same_text(seen[k], key);
    if (dup) continue;
    seen[seen_count++] = key;
    if (!tj::Push(a, out, name)) return nullptr;
  }
  return out;
}

Value* authority(Arena& a, const Value* v) {
  const Value* held = record(v);
  return held ? flag(a, tj::Get(held, "status")) : tj::Null(a);
}

Value* solana_fee(Arena& a, const Value* raw) {
  const Value* fee = record(raw);
  if (!fee) return tj::Null(a);
  const char* keys[] = {"transfer_fee_percent", "fee_rate", "current_fee_rate", "percent"};
  for (const char* key : keys) {
    double v = 0;
    if (!num_value(tj::Get(fee, key), &v)) continue;
    const size_t n = strlen(key);
    const bool ends_percent = n >= 7 && strcmp(key + n - 7, "percent") == 0;
    return tj::Num(a, ends_percent ? v : v * 100);
  }
  return tj::Null(a);
}

// The `empty(chain, address)` literal: every key in TypeScript order.
constexpr const char* kKeys[] = {
    "source", "chain", "address", "name", "symbol", "isHoneypot", "cannotBuy", "cannotSellAll",
    "buyTaxPct", "sellTaxPct", "transferTaxPct", "slippageModifiable", "personalSlippageModifiable",
    "tradingCooldown", "transferPausable", "isBlacklisted", "isWhitelisted", "isAntiWhale",
    "antiWhaleModifiable", "honeypotWithSameCreator", "isOpenSource", "isProxy", "isMintable",
    "selfdestruct", "externalCall", "canTakeBackOwnership", "hiddenOwner", "ownerAddress",
    "ownerPercent", "ownerBalance", "ownerChangeBalance", "ownerRenounced", "creatorAddress",
    "creatorPercent", "creatorBalance", "creatorExited", "holderCount", "totalSupply", "topHolders",
    "topHoldersPercent", "topHoldersPercentExcludingLocked", "burnedPercent", "isInDex", "isInCex",
    "cexList", "pools", "lpHolderCount", "lpTotalSupply", "lpHolders", "lpLockedPercent",
    "lpLockEndsAt", "lpLockExpired", "isFreezable", "isClosable", "balanceMutable",
    "metadataMutable", "hasTransferHook", "isNonTransferable", "trustedListed", "verdict", "flags",
};

Value* empty(Arena& a, const char* chain, const char* address) {
  Value* out = tj::Obj(a);
  if (!out) return nullptr;
  for (const char* key : kKeys) {
    Value* v = nullptr;
    if (strcmp(key, "source") == 0) v = tj::Str(a, "goplus");
    else if (strcmp(key, "chain") == 0) v = tj::Str(a, chain);
    else if (strcmp(key, "address") == 0) v = tj::Str(a, address);
    else if (strcmp(key, "topHolders") == 0 || strcmp(key, "cexList") == 0 ||
             strcmp(key, "pools") == 0 || strcmp(key, "lpHolders") == 0 ||
             strcmp(key, "flags") == 0) v = tj::Arr(a);
    else if (strcmp(key, "verdict") == 0) v = tj::Str(a, "unknown");
    else v = tj::Null(a);
    if (!v || !tj::Set(a, out, key, v)) return nullptr;
  }
  return out;
}

bool set(Arena& a, Value* o, const char* key, Value* v) { return v && tj::Set(a, o, key, v); }

bool from_evm(Arena& a, Value* o, const Value* raw) {
  Value* owner = str(a, tj::Get(raw, "owner_address"));
  if (!owner) return false;
  Value* renounced = tj::Null(a);
  if (!is_null(owner)) renounced = tj::Bool(a, IsBurnAddress(a, owner->text, owner->length));
  return set(a, o, "name", str(a, tj::Get(raw, "token_name"))) &&
         set(a, o, "symbol", str(a, tj::Get(raw, "token_symbol"))) &&
         set(a, o, "isHoneypot", flag(a, tj::Get(raw, "is_honeypot"))) &&
         set(a, o, "cannotBuy", flag(a, tj::Get(raw, "cannot_buy"))) &&
         set(a, o, "cannotSellAll", flag(a, tj::Get(raw, "cannot_sell_all"))) &&
         set(a, o, "buyTaxPct", percent(a, tj::Get(raw, "buy_tax"))) &&
         set(a, o, "sellTaxPct", percent(a, tj::Get(raw, "sell_tax"))) &&
         set(a, o, "transferTaxPct", percent(a, tj::Get(raw, "transfer_tax"))) &&
         set(a, o, "slippageModifiable", flag(a, tj::Get(raw, "slippage_modifiable"))) &&
         set(a, o, "personalSlippageModifiable", flag(a, tj::Get(raw, "personal_slippage_modifiable"))) &&
         set(a, o, "tradingCooldown", flag(a, tj::Get(raw, "trading_cooldown"))) &&
         set(a, o, "transferPausable", flag(a, tj::Get(raw, "transfer_pausable"))) &&
         set(a, o, "isBlacklisted", flag(a, tj::Get(raw, "is_blacklisted"))) &&
         set(a, o, "isWhitelisted", flag(a, tj::Get(raw, "is_whitelisted"))) &&
         set(a, o, "isAntiWhale", flag(a, tj::Get(raw, "is_anti_whale"))) &&
         set(a, o, "antiWhaleModifiable", flag(a, tj::Get(raw, "anti_whale_modifiable"))) &&
         set(a, o, "honeypotWithSameCreator", flag(a, tj::Get(raw, "honeypot_with_same_creator"))) &&
         set(a, o, "isOpenSource", flag(a, tj::Get(raw, "is_open_source"))) &&
         set(a, o, "isProxy", flag(a, tj::Get(raw, "is_proxy"))) &&
         set(a, o, "isMintable", flag(a, tj::Get(raw, "is_mintable"))) &&
         set(a, o, "selfdestruct", flag(a, tj::Get(raw, "selfdestruct"))) &&
         set(a, o, "externalCall", flag(a, tj::Get(raw, "external_call"))) &&
         set(a, o, "canTakeBackOwnership", flag(a, tj::Get(raw, "can_take_back_ownership"))) &&
         set(a, o, "hiddenOwner", flag(a, tj::Get(raw, "hidden_owner"))) &&
         set(a, o, "ownerAddress", owner) &&
         set(a, o, "ownerPercent", percent(a, tj::Get(raw, "owner_percent"))) &&
         set(a, o, "ownerBalance", num(a, tj::Get(raw, "owner_balance"))) &&
         set(a, o, "ownerChangeBalance", flag(a, tj::Get(raw, "owner_change_balance"))) &&
         set(a, o, "ownerRenounced", renounced) &&
         set(a, o, "creatorAddress", str(a, tj::Get(raw, "creator_address"))) &&
         set(a, o, "creatorPercent", percent(a, tj::Get(raw, "creator_percent"))) &&
         set(a, o, "creatorBalance", num(a, tj::Get(raw, "creator_balance"))) &&
         set(a, o, "holderCount", int_(a, tj::Get(raw, "holder_count"))) &&
         set(a, o, "totalSupply", num(a, tj::Get(raw, "total_supply"))) &&
         set(a, o, "topHolders", holders(a, tj::Get(raw, "holders"), false)) &&
         set(a, o, "isInDex", flag(a, tj::Get(raw, "is_in_dex"))) &&
         set(a, o, "isInCex", cex_listed(a, tj::Get(raw, "is_in_cex"))) &&
         set(a, o, "cexList", cex_names(a, tj::Get(raw, "is_in_cex"))) &&
         set(a, o, "pools", pools(a, tj::Get(raw, "dex"), false)) &&
         set(a, o, "lpHolderCount", int_(a, tj::Get(raw, "lp_holder_count"))) &&
         set(a, o, "lpTotalSupply", num(a, tj::Get(raw, "lp_total_supply"))) &&
         set(a, o, "lpHolders", holders(a, tj::Get(raw, "lp_holders"), true)) &&
         set(a, o, "trustedListed", flag(a, tj::Get(raw, "trust_list")));
}

bool from_solana(Arena& a, Value* o, const Value* raw) {
  const Value* metadata = record(tj::Get(raw, "metadata"));
  const Value* creators = tj::Get(raw, "creators");
  const Value* first = tj::IsArray(creators) && creators->count ? record(creators->items[0]) : nullptr;
  const Value* hook = tj::Get(raw, "transfer_hook");
  return set(a, o, "name", str(a, tj::Get(metadata, "name"))) &&
         set(a, o, "symbol", str(a, tj::Get(metadata, "symbol"))) &&
         set(a, o, "isNonTransferable", flag(a, tj::Get(raw, "non_transferable"))) &&
         set(a, o, "transferTaxPct", solana_fee(a, tj::Get(raw, "transfer_fee"))) &&
         set(a, o, "isMintable", authority(a, tj::Get(raw, "mintable"))) &&
         set(a, o, "isFreezable", authority(a, tj::Get(raw, "freezable"))) &&
         set(a, o, "isClosable", authority(a, tj::Get(raw, "closable"))) &&
         set(a, o, "balanceMutable", authority(a, tj::Get(raw, "balance_mutable_authority"))) &&
         set(a, o, "metadataMutable", authority(a, tj::Get(raw, "metadata_mutable"))) &&
         set(a, o, "hasTransferHook", tj::IsArray(hook) ? tj::Bool(a, hook->count > 0) : tj::Null(a)) &&
         set(a, o, "creatorAddress", str(a, tj::Get(first, "address"))) &&
         set(a, o, "holderCount", int_(a, tj::Get(raw, "holder_count"))) &&
         set(a, o, "totalSupply", num(a, tj::Get(raw, "total_supply"))) &&
         set(a, o, "topHolders", holders(a, tj::Get(raw, "holders"), false)) &&
         set(a, o, "pools", pools(a, tj::Get(raw, "dex"), true)) &&
         set(a, o, "lpHolders", holders(a, tj::Get(raw, "lp_holders"), true)) &&
         set(a, o, "trustedListed", flag(a, tj::Get(raw, "trusted_token")));
}

// sumPercent: null when any share is unknown.
bool sum_percent(Value* const* rows, size_t count, double* total) {
  double sum = 0;
  for (size_t i = 0; i < count; ++i) {
    const Value* p = tj::Get(rows[i], "percent");
    if (!tj::IsNumber(p)) return false;
    sum += p->number;
  }
  *total = sum;
  return true;
}

bool burn(Arena& a, const Value* row) {
  const Value* address = tj::Get(row, "address");
  return tj::IsString(address) && IsBurnAddress(a, address->text, address->length);
}

bool with_derived(Arena& a, Value* v, int64_t now_ms) {
  Value* reported = tj::Get(v, "topHolders");
  Value* lp = tj::Get(v, "lpHolders");
  const Value* pool_list = tj::Get(v, "pools");
  const size_t n = reported->count;
  Value** burned_rows = static_cast<Value**>(a.Alloc(sizeof(Value*) * (n + 1)));
  Value** live_rows = static_cast<Value**>(a.Alloc(sizeof(Value*) * (n + 1)));
  Value** locked_rows = static_cast<Value**>(a.Alloc(sizeof(Value*) * (lp->count + 1)));
  Value** pool_keys = static_cast<Value**>(a.Alloc(sizeof(Value*) * (pool_list->count + 1)));
  if (!burned_rows || !live_rows || !locked_rows || !pool_keys) return false;
  size_t pool_count = 0;
  for (uint32_t i = 0; i < pool_list->count; ++i) {
    const Value* pair = tj::Get(pool_list->items[i], "pair");
    if (!tj::IsString(pair)) continue;
    pool_keys[pool_count] = lower(a, pair);
    if (!pool_keys[pool_count]) return false;
    ++pool_count;
  }
  Value* kept = tj::Arr(a);
  if (!kept) return false;
  size_t burned_count = 0, live_count = 0;
  for (uint32_t i = 0; i < n; ++i) {
    Value* row = reported->items[i];
    if (burn(a, row)) { burned_rows[burned_count++] = row; continue; }
    if (!tj::Push(a, kept, row)) return false;
    const Value* locked = tj::Get(row, "isLocked");
    if (locked && locked->type == tj::Type::kTrue) continue;
    Value* key = lower(a, tj::Get(row, "address"));
    if (!key) return false;
    bool in_pool = false;
    for (size_t k = 0; k < pool_count && !in_pool; ++k) in_pool = same_text(pool_keys[k], key);
    if (!in_pool) live_rows[live_count++] = row;
  }
  size_t locked_count = 0;
  for (uint32_t i = 0; i < lp->count; ++i) {
    const Value* locked = tj::Get(lp->items[i], "isLocked");
    if ((locked && locked->type == tj::Type::kTrue) || burn(a, lp->items[i]))
      locked_rows[locked_count++] = lp->items[i];
  }
  double burned = 0, total = 0, live = 0, locked_lp = 0;
  const bool has_burned = sum_percent(burned_rows, burned_count, &burned);
  const bool has_total = sum_percent(reported->items, n, &total);
  const bool has_live = sum_percent(live_rows, live_count, &live);
  const bool has_locked = sum_percent(locked_rows, locked_count, &locked_lp);
  // latestLockEnd over lockedUntil strings; datedLocks = rows with one.
  bool has_end = false;
  double latest = 0;
  size_t dated = 0;
  for (uint32_t i = 0; i < lp->count; ++i) {
    const Value* until = tj::Get(lp->items[i], "lockedUntil");
    if (!tj::IsString(until)) continue;
    ++dated;
    double parsed = 0;
    if (!parse_date(until, &parsed)) continue;
    if (!has_end || parsed > latest) latest = parsed;
    has_end = true;
  }
  const Value* creator_balance = tj::Get(v, "creatorBalance");
  Value* exited = tj::IsNumber(creator_balance) ? tj::Bool(a, creator_balance->number == 0) : tj::Null(a);
  auto nullable = [&](bool empty_list, bool has, double value) -> Value* {
    if (empty_list) return tj::Null(a);
    return has ? tj::Num(a, value) : tj::Null(a);
  };
  return set(a, v, "topHolders", kept) && set(a, v, "creatorExited", exited) &&
         set(a, v, "burnedPercent", nullable(n == 0, has_burned, burned)) &&
         set(a, v, "topHoldersPercent", nullable(n == 0, has_total, total)) &&
         set(a, v, "topHoldersPercentExcludingLocked", nullable(n == 0, has_live, live)) &&
         set(a, v, "lpLockedPercent", nullable(lp->count == 0, has_locked, locked_lp)) &&
         set(a, v, "lpLockEndsAt", has_end ? iso_or_null(a, latest) : tj::Null(a)) &&
         set(a, v, "lpLockExpired", !has_end || dated == 0 ? tj::Null(a)
                                                           : tj::Bool(a, latest < static_cast<double>(now_ms)));
}

// pct(): coarse when large, precise when small, never a trailing ".0".
void pct(double value, char* out, size_t capacity) {
  const int digits = value >= 10 ? 0 : value >= 1 ? 1 : 2;
  char text[64];
  codec::provider_text::ToFixed(value, digits, text, sizeof(text));
  if (strchr(text, '.')) {
    size_t n = strlen(text);
    size_t zeros = n;
    while (zeros > 0 && text[zeros - 1] == '0') --zeros;
    if (zeros < n) {
      size_t cut = zeros;
      if (cut > 0 && text[cut - 1] == '.') --cut;
      text[cut] = '\0';
    }
  }
  snprintf(out, capacity, "%s%%", text);
}

struct Verdict {
  Arena& a;
  Value* flags;
  bool danger = false, caution = false, ok = true;
  int observed = 0;

  void push(const char* key, const char* label, const char* detail, const char* sentiment) {
    Value* row = tj::Obj(a);
    ok = ok && row && tj::Set(a, row, "key", tj::Str(a, key)) && tj::Set(a, row, "label", tj::Str(a, label)) &&
         tj::Set(a, row, "detail", tj::Str(a, detail)) && tj::Set(a, row, "sentiment", tj::Str(a, sentiment)) &&
         tj::Push(a, flags, row);
  }
  void check(const Value* value, const char* key, const char* label, const char* detail, bool danger_severity,
             const char* false_label = nullptr, const char* false_detail = nullptr) {
    if (!value || value->type == tj::Type::kNull) return;
    ++observed;
    if (value->type == tj::Type::kTrue) {
      push(key, label, detail, danger_severity ? "bad" : "caution");
      if (danger_severity) danger = true; else caution = true;
    } else if (false_label) {
      push(key, false_label, false_detail, "good");
    }
  }
};

double number_or(const Value* v, bool* has) {
  *has = tj::IsNumber(v);
  return *has ? v->number : 0;
}

void tax_detail(Arena&, const Value* v, char* out, size_t capacity) {
  const char* keys[3] = {"buyTaxPct", "sellTaxPct", "transferTaxPct"};
  const char* names[3] = {"buy", "sell", "transfer"};
  out[0] = '\0';
  size_t used = 0;
  bool any = false;
  for (int i = 0; i < 3; ++i) {
    bool has = false;
    const double value = number_or(tj::Get(v, keys[i]), &has);
    if (!has) continue;
    char p[64];
    pct(value, p, sizeof(p));
    const int n = snprintf(out + used, capacity - used, "%s%s %s", any ? " · " : "", p, names[i]);
    if (n > 0 && static_cast<size_t>(n) < capacity - used) used += static_cast<size_t>(n);
    any = true;
  }
  if (!any) snprintf(out, capacity, "Unknown");
}

// shortAddress (UTF-16 slicing).
Value* short_address(Arena& a, const Value* address) {
  if (!tj::IsString(address)) return tj::Str(a, "an address");
  const size_t units = codec::provider_text::Utf16Length(address->text, address->length);
  if (units <= 12) return tj::Str(a, address->text, address->length);
  Value* head = codec::provider_text::Slice(a, address->text, address->length, 0, 6);
  Value* tail = codec::provider_text::Slice(a, address->text, address->length, units - 4, units);
  if (!head || !tail) return nullptr;
  char* text = static_cast<char*>(a.Alloc(head->length + tail->length + 4, 1));
  if (!text) return nullptr;
  memcpy(text, head->text, head->length);
  std::copy_n("\xE2\x80\xA6", 3, text + head->length);
  memcpy(text + head->length + 3, tail->text, tail->length);
  return tj::Str(a, text, head->length + 3 + tail->length);
}

bool verdict_for(Arena& a, Value* v) {
  Value* flags = tj::Arr(a);
  if (!flags) return false;
  Verdict r{a, flags};
  auto g = [&](const char* key) { return tj::Get(v, key); };
  r.check(g("isHoneypot"), "honeypot", "Honeypot", "Simulated sells fail \xE2\x80\x94 you may not be able to sell", true,
          "Sell check", "A simulated sell succeeds");
  r.check(g("cannotSellAll"), "cannot-sell-all", "Partial sells only",
          "The contract blocks selling the whole balance", true);
  r.check(g("cannotBuy"), "cannot-buy", "Cannot buy", "The contract blocks buys", true);
  r.check(g("honeypotWithSameCreator"), "creator-honeypot", "Deployer history",
          "This deployer has shipped a honeypot before", true, "Deployer history",
          "No known honeypot from this deployer");
  r.check(g("hiddenOwner"), "hidden-owner", "Hidden owner", "Ownership is concealed in the contract", true);
  r.check(g("canTakeBackOwnership"), "reclaim-ownership", "Ownership reclaimable",
          "Renounced ownership can be taken back", true);
  r.check(g("selfdestruct"), "selfdestruct", "Self-destruct", "The contract can destroy itself", true);

  bool has_buy = false, has_sell = false, has_transfer = false;
  const double buy = number_or(g("buyTaxPct"), &has_buy);
  const double sell = number_or(g("sellTaxPct"), &has_sell);
  const double transfer = number_or(g("transferTaxPct"), &has_transfer);
  if (has_buy || has_sell || has_transfer) {
    double tax = -INFINITY;
    if (has_buy) tax = fmax(tax, buy);
    if (has_sell) tax = has_buy ? fmax(tax, sell) : sell;
    if (has_transfer) tax = (has_buy || has_sell) ? fmax(tax, transfer) : transfer;
    ++r.observed;
    char detail[256];
    tax_detail(a, v, detail, sizeof(detail));
    if (tax >= 50) {
      r.danger = true;
      r.push("tax", "Transfer tax", detail, "bad");
    } else if (tax > 0) {
      r.caution = true;
      r.push("tax", "Transfer tax", detail, "caution");
    } else if (has_buy && has_sell && has_transfer) {
      r.push("tax", "Transfer tax", "None", "good");
    } else {
      char neutral[320];
      snprintf(neutral, sizeof(neutral), "%s \xC2\xB7 other legs not reported", detail);
      r.push("tax", "Transfer tax", neutral, "neutral");
    }
  }
  r.check(g("slippageModifiable"), "slippage-modifiable", "Tax can change", "The owner can raise the trading tax", false);
  r.check(g("personalSlippageModifiable"), "personal-slippage", "Per-wallet tax",
          "The owner can set a different tax for one wallet", false);
  r.check(g("transferPausable"), "pausable", "Transfers pausable", "The owner can halt all transfers", false);
  r.check(g("tradingCooldown"), "cooldown", "Trading cooldown", "The contract enforces a delay between trades", false);
  r.check(g("isBlacklisted"), "blacklist", "Blacklist", "Addresses can be blocked from trading", false);
  r.check(g("isWhitelisted"), "whitelist", "Whitelist", "Only approved addresses trade freely", false);
  r.check(g("antiWhaleModifiable"), "anti-whale-modifiable", "Max trade size can change",
          "The owner can change the per-trade cap", false);
  r.check(g("isMintable"), "mintable", "Mintable", "New supply can still be created", false, "Supply",
          "No new tokens can be minted");
  r.check(g("isProxy"), "proxy", "Proxy contract", "The code behind this address can be replaced", false);
  r.check(g("externalCall"), "external-call", "External calls", "Transfers depend on another contract", false);
  r.check(g("isFreezable"), "freezable", "Freeze authority", "An authority can freeze your token account", false,
          "Freeze authority", "Revoked");
  r.check(g("isClosable"), "closable", "Close authority", "An authority can close token accounts", false);
  r.check(g("balanceMutable"), "balance-mutable", "Balance mutable", "An authority can change balances directly", true);
  r.check(g("isNonTransferable"), "non-transferable", "Non-transferable", "The token cannot be transferred at all", true);
  r.check(g("hasTransferHook"), "transfer-hook", "Transfer hook", "Custom code runs on every transfer", false);
  r.check(g("metadataMutable"), "metadata-mutable", "Metadata mutable", "Name, symbol and image can still be changed",
          false);

  const Value* open = g("isOpenSource");
  if (open && open->type != tj::Type::kNull) {
    ++r.observed;
    if (open->type == tj::Type::kTrue) {
      r.push("source", "Source code", "Verified and public", "good");
    } else {
      r.caution = true;
      r.push("source", "Source code", "Not verified \xE2\x80\x94 the checks below cannot see what it does", "bad");
    }
  }
  const Value* in_cex = g("isInCex");
  if (in_cex && in_cex->type == tj::Type::kTrue) {
    const Value* list = g("cexList");
    if (!list->count) {
      r.push("cex", "Exchange listing", "Listed on a centralised exchange", "good");
    } else {
      size_t total = 16;
      for (uint32_t i = 0; i < list->count; ++i) total += list->items[i]->length + 4;
      char* text = static_cast<char*>(a.Alloc(total, 1));
      if (!text) return false;
      size_t used = static_cast<size_t>(snprintf(text, total, "Listed on "));
      for (uint32_t i = 0; i < list->count; ++i) {
        if (i) { memcpy(text + used, " \xC2\xB7 ", 4); used += 4; }
        memcpy(text + used, list->items[i]->text, list->items[i]->length);
        used += list->items[i]->length;
      }
      text[used] = '\0';
      r.push("cex", "Exchange listing", text, "good");
    }
  }
  const Value* renounced = g("ownerRenounced");
  bool has_owner_pct = false;
  const double owner_pct = number_or(g("ownerPercent"), &has_owner_pct);
  if (renounced && renounced->type != tj::Type::kNull) {
    ++r.observed;
    if (renounced->type == tj::Type::kTrue) {
      r.push("owner", "Ownership", "Renounced", "good");
    } else {
      Value* who = short_address(a, g("ownerAddress"));
      if (!who) return false;
      const char* who_text = tj::CString(a, who);
      if (!who_text) return false;
      char detail[512];
      if (has_owner_pct && owner_pct > 0) {
        char p[64];
        pct(owner_pct, p, sizeof(p));
        snprintf(detail, sizeof(detail), "Held by %s \xC2\xB7 %s of supply", who_text, p);
      } else {
        snprintf(detail, sizeof(detail), "Held by %s", who_text);
      }
      r.push("owner", "Ownership", detail, "neutral");
    }
  }
  if (has_owner_pct && owner_pct >= 5) {
    r.caution = true;
    char p[64], detail[128];
    pct(owner_pct, p, sizeof(p));
    snprintf(detail, sizeof(detail), "%s of supply", p);
    r.push("owner-stake", "Owner holdings", detail, "caution");
  }
  const Value* exited = g("creatorExited");
  if (exited && exited->type != tj::Type::kNull) {
    ++r.observed;
    bool has_creator_pct = false;
    const double creator_pct = number_or(g("creatorPercent"), &has_creator_pct);
    if (exited->type == tj::Type::kTrue) {
      r.push("creator", "Deployer", "Holds none of the supply", "good");
    } else {
      char detail[128];
      if (!has_creator_pct) {
        snprintf(detail, sizeof(detail), "Still holds a balance");
      } else {
        char p[64];
        pct(creator_pct, p, sizeof(p));
        snprintf(detail, sizeof(detail), "Holds %s of supply", p);
      }
      r.push("creator", "Deployer", detail, has_creator_pct && creator_pct >= 5 ? "caution" : "neutral");
    }
    if (has_creator_pct && creator_pct >= 5) r.caution = true;
  }
  bool has_live = false;
  const double live = number_or(g("topHoldersPercentExcludingLocked"), &has_live);
  if (has_live) {
    ++r.observed;
    char p[64], detail[128];
    pct(live, p, sizeof(p));
    snprintf(detail, sizeof(detail), "%s in live wallets", p);
    if (live >= 50) { r.danger = true; r.push("concentration", "Top holders", detail, "bad"); }
    else r.push("concentration", "Top holders", detail, "good");
  }
  bool has_burned = false;
  const double burned = number_or(g("burnedPercent"), &has_burned);
  if (has_burned && burned > 0) {
    char p[64];
    pct(burned, p, sizeof(p));
    r.push("burned", "Burned supply", p, "good");
  }
  bool has_lp = false;
  const double lp = number_or(g("lpLockedPercent"), &has_lp);
  if (has_lp) {
    ++r.observed;
    char p[64], detail[160];
    pct(lp, p, sizeof(p));
    const Value* expired = g("lpLockExpired");
    if (expired && expired->type == tj::Type::kTrue) {
      r.caution = true;
      snprintf(detail, sizeof(detail), "%s of LP locked \xE2\x80\x94 the lock expired", p);
      r.push("lp-lock", "Liquidity lock", detail, "caution");
    } else if (lp >= 50) {
      snprintf(detail, sizeof(detail), "%s of LP locked or burned", p);
      r.push("lp-lock", "Liquidity lock", detail, "good");
    } else {
      r.caution = true;
      snprintf(detail, sizeof(detail), "%s of LP locked \xE2\x80\x94 the rest can be withdrawn", p);
      r.push("lp-lock", "Liquidity lock", detail, "caution");
    }
  }
  if (!r.ok) return false;
  const char* verdict = r.observed == 0 ? "unknown" : r.danger ? "danger" : r.caution ? "caution" : "ok";
  return set(a, v, "verdict", tj::Str(a, verdict)) && set(a, v, "flags", flags);
}

}  // namespace

const char* GoPlusChainId(const networks::TokenNetwork& network, uint32_t evm_chain_id, char buffer[16]) {
  if (strcmp(network.family, "solana") == 0) return strcmp(network.id, "solana") == 0 ? "solana" : nullptr;
  if (strcmp(network.family, "tron") == 0) return "tron";
  if (strcmp(network.family, "evm") != 0 || evm_chain_id == 0 || !supported_evm(evm_chain_id)) return nullptr;
  snprintf(buffer, 16, "%u", evm_chain_id);
  return buffer;
}

bool GoPlusPath(const char* chain, const char* address, char* output, size_t capacity) {
  const Dialect dialect = dialect_for(chain);
  if (dialect == Dialect::kNone || !*address) return false;
  tj::OutBuffer out{reinterpret_cast<uint8_t*>(output), capacity - 1, 0, true};
  if (dialect == Dialect::kSolana) {
    out.Text("/api/v1/solana/token_security?contract_addresses=");
  } else {
    out.Text("/api/v1/token_security/");
    append_component(out, chain);
    out.Text("?contract_addresses=");
  }
  append_component(out, address);
  if (!out.ok) return false;
  output[out.used] = '\0';
  return true;
}

GoPlusStatus ClassifyGoPlusBody(const Value* body, const char* address, const Value** token) {
  *token = nullptr;
  if (!body || body->type == tj::Type::kNull) return GoPlusStatus::kFailed;
  const Value* code = tj::Get(body, "code");
  if (!(tj::IsNumber(code) && code->number == 1)) {
    if (tj::IsNumber(code) && code->number == 2020) return GoPlusStatus::kAbsent;
    return GoPlusStatus::kFailed;
  }
  const Value* result = tj::Get(body, "result");
  if (!result || result->type == tj::Type::kNull) return GoPlusStatus::kAbsent;
  if (tj::IsObject(result)) {
    const Value* exact = tj::Get(result, address);
    if (!exact || exact->type == tj::Type::kNull) {
      // result[address.toLowerCase()] (ASCII for every address GoPlus echoes)
      char lowered[256];
      const size_t n = strnlen(address, sizeof(lowered) - 1);
      for (size_t i = 0; i < n; ++i)
        lowered[i] = (address[i] >= 'A' && address[i] <= 'Z') ? static_cast<char>(address[i] + 32) : address[i];
      lowered[n] = '\0';
      exact = tj::Get(result, lowered);
    }
    if (tj::IsObject(exact)) { *token = exact; return GoPlusStatus::kRow; }
    for (uint32_t i = 0; i < result->count; ++i) {
      const tj::Member& m = result->members[i];
      const size_t n = strlen(address);
      if (m.key_length != n) continue;
      bool same = true;
      for (size_t k = 0; k < n && same; ++k) {
        char x = m.key[k], y = address[k];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
        same = x == y;
      }
      if (same && tj::IsObject(m.value)) { *token = m.value; return GoPlusStatus::kRow; }
    }
    if (result->count == 1 && tj::IsObject(result->members[0].value)) {
      *token = result->members[0].value;
      return GoPlusStatus::kRow;
    }
    return GoPlusStatus::kAbsent;
  }
  if (tj::IsArray(result) && result->count == 1 && tj::IsObject(result->items[0])) {
    *token = result->items[0];
    return GoPlusStatus::kRow;
  }
  return GoPlusStatus::kAbsent;
}

Value* NormalizeContractSecurity(Arena& a, const Value* token, const char* chain, const char* address,
                                 int64_t now_ms) {
  Value* v = empty(a, chain, address);
  if (!v) return nullptr;
  const bool ok = dialect_for(chain) == Dialect::kSolana ? from_solana(a, v, token) : from_evm(a, v, token);
  if (!ok || !with_derived(a, v, now_ms) || !verdict_for(a, v)) return nullptr;
  return a.ok ? v : nullptr;
}

bool IsBurnAddress(Arena& a, const char* address, size_t length) {
  size_t begin = 0, end = 0;
  codec::provider_text::Trim(address, length, &begin, &end);
  const char* t = address + begin;
  const size_t n = end - begin;
  static const char* kBurn[] = {
      "0x0000000000000000000000000000000000000000",
      "0x000000000000000000000000000000000000dead",
      "0x00000000000000000000000000000000000000dead",
      "1nc1nerator11111111111111111111111111111111",
  };
  const Value* key = nullptr;
  if (n >= 2 && t[0] == '0' && t[1] == 'x') key = codec::provider_text::ToLowerCase(a, t, n);
  else key = tj::Str(a, t, n);
  if (!key) return false;
  for (const char* b : kBurn)
    if (strlen(b) == key->length && memcmp(b, key->text, key->length) == 0) return true;
  return false;
}

}  // namespace fortis::tokens::token_contract
