#include <algorithm>
#include "ownership.h"
#include "robinhood_markets.h"

#include "chain_core.h"
#include "chain_transport.h"
#include "cross_chain_support.h"
#include "js_json.h"
#include "proto_wire.h"
#include "media_registry.h"
#include "robinhood_policy.h"
#include "robinhood_price.h"
#include "threading.h"
#include "token_search_route.h"
#include "token_text.h"
#include "whatwg_url.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace tj = fortis::codec::tj;

namespace fortis::exchange::robinhood {

struct StockClient::Snapshot {
  tj::Arena arena{};
  Value* value = nullptr;
  int64_t until_ms = 0;
  ~Snapshot() { tj::FreeArena(arena); }
};

namespace {

namespace cc = fortis::exchange::cross_chain;
namespace text = fortis::compat::token_text;
namespace jj = fortis::compat::js_json;
namespace pw = fortis::codec::proto_wire;
using tj::OutBuffer;

constexpr const char* kLane = "robinhood-assets";
constexpr const char* kBase = "https://api.robinhood.com";
constexpr const char* kSource = "robinhood-assets";
constexpr int64_t kAssetTtlMs = 15 * 60000;
constexpr int64_t kActionTtlMs = 60 * 60000;
constexpr int64_t kQuoteTtlMs = 15000;
constexpr size_t kFetchArenaBytes = 16 * 1024 * 1024;
constexpr size_t kSnapshotArenaBytes = 16 * 1024 * 1024;

uint64_t monotonic_ms() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1000000;
}

bool is_record(const Value* v) { return tj::IsObject(v); }
Value* str(Arena& arena, const char* t) { return tj::Str(arena, t); }
bool set(Arena& arena, Value* object, const char* key, Value* value) {
  return value && tj::Set(arena, object, key, value);
}
Value* or_null(Arena& arena, const Value* v) { return v ? const_cast<Value*>(v) : tj::Null(arena); }

bool hex_digit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// /^0x[0-9a-fA-F]{n}$/
bool hex_text(const Value* v, size_t digits) {
  if (!tj::IsString(v) || v->length != digits + 2 || v->text[0] != '0' || v->text[1] != 'x') return false;
  for (size_t i = 2; i < v->length; ++i)
    if (!hex_digit(v->text[i])) return false;
  return true;
}

// a.toLowerCase() === b.toLowerCase() for an ASCII `b` (a non-ASCII
// character never lowercases into the address alphabet).
bool same_address(const Value* a, const char* b) {
  if (!tj::IsString(a) || a->length != strlen(b)) return false;
  for (size_t i = 0; i < a->length; ++i) {
    char x = a->text[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x | 0x20);
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y | 0x20);
    if (x != y) return false;
  }
  return true;
}

// text(value, max): trimmed, non-empty, at most `max` UTF-16 units.
Value* text_value(Arena& arena, const Value* v, size_t max) {
  if (!tj::IsString(v)) return nullptr;
  size_t begin = 0, end = 0;
  text::Trim(v->text, v->length, &begin, &end);
  if (begin == end || text::Utf16Length(v->text + begin, end - begin) > max) return nullptr;
  return tj::Str(arena, v->text + begin, end - begin);
}

// positive(value): `typeof value === 'number' ? value : Number(value)`, > 0.
bool positive(const Value* v, double* out) {
  const double number = tj::IsNumber(v) ? v->number : cc::JsNumber(v);
  if (!isfinite(number) || !(number > 0)) return false;
  *out = number;
  return true;
}

// decimal(value): a trimmed /^\d+(?:\.\d{1,18})?$/ string that is positive.
Value* decimal_value(Arena& arena, const Value* v) {
  if (!tj::IsString(v)) return nullptr;
  size_t begin = 0, end = 0;
  text::Trim(v->text, v->length, &begin, &end);
  const char* t = v->text + begin;
  const size_t length = end - begin;
  size_t i = 0, digits = 0, fraction = 0;
  while (i < length && t[i] >= '0' && t[i] <= '9') { ++i; ++digits; }
  if (!digits) return nullptr;
  if (i < length && t[i] == '.') {
    ++i;
    while (i < length && t[i] >= '0' && t[i] <= '9') { ++i; ++fraction; }
    if (!fraction || fraction > 18) return nullptr;
  }
  if (i != length) return nullptr;
  Value* raw = tj::Str(arena, t, length);
  double number = 0;
  return raw && positive(raw, &number) ? raw : nullptr;
}

// tradability(value): text(value, 40), lowercase, without "trading_status_".
Value* tradability(Arena& arena, const Value* v) {
  Value* raw = text_value(arena, v, 40);
  if (!raw) return nullptr;
  Value* lower = text::ToLowerCase(arena, raw->text, raw->length);
  if (!lower) return nullptr;
  constexpr size_t kPrefix = 15;  // "trading_status_"
  if (lower->length >= kPrefix && memcmp(lower->text, "trading_status_", kPrefix) == 0)
    return tj::Str(arena, lower->text + kPrefix, lower->length - kPrefix);
  return lower;
}

// iso(value): new Date(value).toISOString() of a parseable string.
Value* iso_value(Arena& arena, const Value* v) {
  if (!tj::IsString(v)) return nullptr;
  double ms = 0;
  if (!cc::DateParse(v->text, v->length, &ms)) return nullptr;
  const char* iso = cc::IsoText(arena, ms);
  return iso ? str(arena, iso) : nullptr;
}

// httpsUrl(value): new URL(value) with https: and no credentials, as href.
Value* https_url(Arena& arena, const Value* v, compat::url::Url* parsed, compat::url::UrlScratch* scratch) {
  if (!tj::IsString(v)) return nullptr;
  size_t length = 0;
  const char* input = cc::WellFormed(arena, v->text, v->length, &length);
  if (!input || !compat::url::Parse(input, length, nullptr, parsed, scratch)) return nullptr;
  const compat::url::View protocol = compat::url::Protocol(*parsed);
  if (protocol.length != 6 || memcmp(protocol.data, "https:", 6) != 0) return nullptr;
  if (compat::url::Username(*parsed).length || compat::url::Password(*parsed).length) return nullptr;
  const compat::url::View href = compat::url::Href(*parsed);
  return tj::Str(arena, href.data, href.length);
}

// The first deployment with Number(chainId) === 4663 and a valid address.
const Value* chain_deployment(const Value* deployments) {
  if (!tj::IsArray(deployments)) return nullptr;
  for (uint32_t i = 0; i < deployments->count; ++i) {
    const Value* d = deployments->items[i];
    if (is_record(d) && cc::JsNumber(tj::Get(d, "chainId")) == kChainId && hex_text(tj::Get(d, "contractAddress"), 40))
      return d;
  }
  return nullptr;
}

// `value ?? fallback` over member reads.
const Value* coalesce(const Value* a, const Value* b) { return cc::Nullish(a) ? b : a; }

Value* session(Arena& arena, const Value* source) {
  Value* out = tj::Obj(arena);
  if (!out || !set(arena, out, "whole", or_null(arena, tradability(arena, tj::Get(source, "whole")))) ||
      !set(arena, out, "fractional", or_null(arena, tradability(arena, tj::Get(source, "fractional")))))
    return nullptr;
  return out;
}

Value* trading_capabilities(Arena& arena, const Value* raw) {
  const Value* capabilities = is_record(raw) ? raw : nullptr;
  const Value* market = is_record(tj::Get(capabilities, "market")) ? tj::Get(capabilities, "market") : nullptr;
  const Value* extended = is_record(tj::Get(capabilities, "extended")) ? tj::Get(capabilities, "extended") : nullptr;
  const Value* overnight = is_record(tj::Get(capabilities, "overnight")) ? tj::Get(capabilities, "overnight") : nullptr;
  Value* out = tj::Obj(arena);
  if (!out) return nullptr;
  const Value* fractional = coalesce(tj::Get(capabilities, "fractionalTradability"), tj::Get(market, "fractional"));
  const Value* all_day = coalesce(coalesce(tj::Get(capabilities, "allDayTradability"), tj::Get(overnight, "fractional")),
                                  tj::Get(overnight, "whole"));
  const Value* extended_flag = tj::Get(capabilities, "extendedHoursFractionalTradability");
  Value* extended_value = nullptr;
  if (tj::IsBool(extended_flag)) {
    // Copied: the snapshot outlives the provider body.
    extended_value = tj::Bool(arena, extended_flag->type == tj::Type::kTrue);
  } else {
    const Value* status = tradability(arena, tj::Get(extended, "fractional"));
    if (cc::Is(status, "tradable")) extended_value = tj::Bool(arena, true);
    else if (cc::Is(status, "untradable")) extended_value = tj::Bool(arena, false);
  }
  if (!set(arena, out, "fractionalTradability", or_null(arena, tradability(arena, fractional))) ||
      !set(arena, out, "allDayTradability", or_null(arena, tradability(arena, all_day))) ||
      !set(arena, out, "extendedHoursFractionalTradability", or_null(arena, extended_value)) ||
      !set(arena, out, "market", session(arena, market)) || !set(arena, out, "extended", session(arena, extended)) ||
      !set(arena, out, "overnight", session(arena, overnight)))
    return nullptr;
  return out;
}

// Date.UTC(year, month - 1, day) in days (month 1..12; day may run past the
// month's end, as Date.UTC lets it).
int64_t utc_days(int64_t year, int64_t month, int64_t day) {
  year -= month <= 2;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t yoe = year - era * 400;
  const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468 + day - 1;
}

// integer(value, min, max): Number(value) is an integer in range.
bool integer_in(const Value* v, double min, double max, double* out) {
  const double number = cc::JsNumber(v);
  if (!isfinite(number) || trunc(number) != number || number < min || number > max) return false;
  *out = number;
  return true;
}

void put_string(OutBuffer& out, uint32_t field, const char* t, size_t length) {
  pw::PutBytes(out, field, t, length);
}

// An implicit proto3 string: written when non-empty.
void implicit_string(OutBuffer& out, uint32_t field, const Value* object, const char* key) {
  const Value* v = tj::Get(object, key);
  if (tj::IsString(v) && v->length) put_string(out, field, v->text, v->length);
}
// `...(x !== null ? {x} : {})`: written when it is a string.
void optional_string(OutBuffer& out, uint32_t field, const Value* object, const char* key) {
  const Value* v = tj::Get(object, key);
  if (tj::IsString(v)) put_string(out, field, v->text, v->length);
}
void implicit_double(OutBuffer& out, uint32_t field, const Value* object, const char* key) {
  const Value* v = tj::Get(object, key);
  if (tj::IsNumber(v)) pw::PutImplicitDouble(out, field, v->number);
}
void implicit_uint32(OutBuffer& out, uint32_t field, const Value* object, const char* key) {
  const Value* v = tj::Get(object, key);
  if (!tj::IsNumber(v) || v->number == 0) return;
  pw::PutTag(out, field, 0);
  pw::PutVarint(out, static_cast<uint32_t>(v->number));
}
void bool_field(OutBuffer& out, uint32_t field, bool value) {
  pw::PutTag(out, field, 0);
  out.Byte(value ? 1 : 0);
}

using Encoder = bool (*)(OutBuffer& out, const Value* v);
bool embed(OutBuffer& out, uint32_t field, Encoder encode, const Value* v) {
  OutBuffer measure{nullptr, 0, 0, true};
  if (!encode(measure, v)) return false;
  pw::PutTag(out, field, 2);
  pw::PutVarint(out, measure.used);
  return encode(out, v);
}

bool encode_session(OutBuffer& out, const Value* s) {
  optional_string(out, 1, s, "whole");
  optional_string(out, 2, s, "fractional");
  return true;
}

bool encode_capabilities(OutBuffer& out, const Value* c) {
  optional_string(out, 1, c, "fractionalTradability");
  optional_string(out, 2, c, "allDayTradability");
  const Value* flag = tj::Get(c, "extendedHoursFractionalTradability");
  if (tj::IsBool(flag)) bool_field(out, 3, flag->type == tj::Type::kTrue);
  return embed(out, 4, encode_session, tj::Get(c, "market")) && embed(out, 5, encode_session, tj::Get(c, "extended")) &&
         embed(out, 6, encode_session, tj::Get(c, "overnight"));
}

bool encode_deployment(OutBuffer& out, const Value* d) {
  implicit_uint32(out, 1, d, "chainId");
  implicit_string(out, 2, d, "contractAddress");
  return true;
}

bool encode_asset(OutBuffer& out, const Value* a) {
  implicit_string(out, 1, a, "id");
  implicit_string(out, 2, a, "tokenSymbol");
  implicit_string(out, 3, a, "tokenName");
  const Value* deployments = tj::Get(a, "deployments");
  for (uint32_t i = 0; tj::IsArray(deployments) && i < deployments->count; ++i)
    if (!embed(out, 4, encode_deployment, deployments->items[i])) return false;
  implicit_string(out, 5, a, "currentMultiplier");
  implicit_string(out, 6, a, "pendingMultiplier");
  optional_string(out, 7, a, "pendingMultiplierEffectiveTime");
  optional_string(out, 8, a, "logoUrl");
  optional_string(out, 9, a, "issuerLogoUrl");
  optional_string(out, 10, a, "logoMediaKey");
  implicit_string(out, 11, a, "status");
  return embed(out, 12, encode_capabilities, tj::Get(a, "tradingCapabilities"));
}

bool encode_quote(OutBuffer& out, const Value* q) {
  implicit_string(out, 1, q, "symbol");
  implicit_string(out, 2, q, "contractAddress");
  implicit_double(out, 3, q, "bidUsd");
  implicit_double(out, 4, q, "askUsd");
  implicit_double(out, 5, q, "midpointUsd");
  implicit_double(out, 6, q, "rawBidUsd");
  implicit_double(out, 7, q, "rawAskUsd");
  implicit_string(out, 8, q, "multiplier");
  const Value* volume = tj::Get(q, "dailyTradingVolume");
  if (tj::IsNumber(volume)) pw::PutDouble(out, 9, volume->number);
  if (tj::Get(q, "tradingHalted") && tj::Get(q, "tradingHalted")->type == tj::Type::kTrue) bool_field(out, 10, true);
  implicit_string(out, 11, q, "generatedAt");
  implicit_string(out, 12, q, "currency");
  return true;
}

bool encode_date(OutBuffer& out, const Value* d) {
  implicit_uint32(out, 1, d, "year");
  implicit_uint32(out, 2, d, "month");
  implicit_uint32(out, 3, d, "day");
  return true;
}

bool encode_struct(OutBuffer& out, const Value* object) { return tj::AppendProtoStruct(object, out); }

bool encode_action(OutBuffer& out, const Value* a) {
  implicit_string(out, 1, a, "id");
  implicit_string(out, 2, a, "type");
  implicit_string(out, 3, a, "status");
  implicit_string(out, 4, a, "tokenSymbol");
  implicit_string(out, 5, a, "contractAddress");
  const Value* date = tj::Get(a, "processDate");
  if (is_record(date) && !embed(out, 6, encode_date, date)) return false;
  return embed(out, 7, encode_struct, tj::Get(a, "details"));
}

// JSON.parse(JSON.stringify(details)) over an already parsed value: a
// number JSON cannot write (±Infinity from an out-of-range literal) becomes
// null and -0 becomes 0. Depth is bounded by the parser's.
void json_round_trip(Value* v) {
  if (!v) return;
  if (v->type == tj::Type::kNumber) {
    if (!isfinite(v->number)) v->type = tj::Type::kNull;
    else if (v->number == 0) v->number = 0;
  } else if (v->type == tj::Type::kArray) {
    for (uint32_t i = 0; i < v->count; ++i) json_round_trip(v->items[i]);
  } else if (v->type == tj::Type::kObject) {
    for (uint32_t i = 0; i < v->count; ++i) json_round_trip(v->members[i].value);
  }
}

bool encode_map_entry(OutBuffer& out, const Value* member_pair) {
  // member_pair is a two-element array [key, value].
  put_string(out, 1, member_pair->items[0]->text, member_pair->items[0]->length);
  put_string(out, 2, member_pair->items[1]->text, member_pair->items[1]->length);
  return true;
}

}  // namespace

// ---- parsing ---------------------------------------------------------------------------

Value* ParseAssets(Arena& arena, const Value* body) {
  const Value* assets = tj::Get(body, "assets");
  if (!is_record(body) || !tj::IsArray(assets)) return nullptr;
  auto parsed_storage = base::AllocateStorage<compat::url::Url>(sizeof(compat::url::Url));
  auto* parsed = parsed_storage.get();
  auto scratch_storage = base::AllocateStorage<compat::url::UrlScratch>(sizeof(compat::url::UrlScratch));
  auto* scratch = scratch_storage.get();
  Value* out = tj::Arr(arena);
  bool ok = parsed && scratch && out;
  for (uint32_t n = 0; ok && n < assets->count; ++n) {
    const Value* raw = assets->items[n];
    if (!is_record(raw)) continue;
    const tj::Mark mark = tj::MarkOf(arena);
    Value* id = text_value(arena, tj::Get(raw, "id"), 68);
    Value* symbol = text_value(arena, tj::Get(raw, "tokenSymbol"), 32);
    Value* name = text_value(arena, tj::Get(raw, "tokenName"), 160);
    Value* multiplier = decimal_value(arena, tj::Get(raw, "currentMultiplier"));
    const Value* deployment = chain_deployment(tj::Get(raw, "deployments"));
    if (!id || !hex_text(id, 64) || !symbol || !name || !multiplier || !deployment) {
      tj::Rewind(arena, mark);
      if (!arena.ok) ok = false;
      continue;
    }
    // `!seen.add(...)` is never true: a repeated contract is kept.
    for (uint32_t i = 0; i < id->length; ++i) {
      char* c = const_cast<char*>(id->text) + i;
      if (*c >= 'A' && *c <= 'Z') *c = static_cast<char>(*c | 0x20);
    }
    const Value* pending_raw = tj::Get(raw, "pendingMultiplier");
    Value* pending = cc::Is(pending_raw, "") ? nullptr : decimal_value(arena, pending_raw);
    const Value* status = tj::Get(raw, "status");
    Value* asset = tj::Obj(arena);
    ok = asset && set(arena, asset, "id", id) && set(arena, asset, "tokenSymbol", symbol) &&
         set(arena, asset, "tokenName", name) &&
         set(arena, asset, "contractAddress", tj::Clone(arena, tj::Get(deployment, "contractAddress"))) &&
         set(arena, asset, "chainId", tj::Num(arena, kChainId)) && set(arena, asset, "currentMultiplier", multiplier) &&
         set(arena, asset, "pendingMultiplier", or_null(arena, pending)) &&
         set(arena, asset, "pendingMultiplierEffectiveTime",
             or_null(arena, pending ? iso_value(arena, tj::Get(raw, "pendingMultiplierEffectiveTime")) : nullptr)) &&
         set(arena, asset, "logoUrl", or_null(arena, https_url(arena, tj::Get(raw, "logoUrl"), parsed, scratch))) &&
         set(arena, asset, "status",
             str(arena, cc::Is(status, "ASSET_STATUS_ACTIVE")     ? "active"
                        : cc::Is(status, "ASSET_STATUS_INACTIVE") ? "inactive"
                                                                  : "unknown")) &&
         set(arena, asset, "tradingCapabilities", trading_capabilities(arena, tj::Get(raw, "tradingCapabilities"))) &&
         tj::Push(arena, out, asset);
  }
  parsed_storage.reset();
  scratch_storage.reset();
  if (!ok) return nullptr;
  // Stable sort by ticker (RobinhoodSymbolCompare).
  for (uint32_t i = 1; i < out->count; ++i) {
    Value* moving = out->items[i];
    const Value* key = tj::Get(moving, "tokenSymbol");
    uint32_t j = i;
    while (j > 0) {
      const Value* prior = tj::Get(out->items[j - 1], "tokenSymbol");
      if (discovery::RobinhoodSymbolCompare(prior->text, prior->length, key->text, key->length) <= 0) break;
      out->items[j] = out->items[j - 1];
      --j;
    }
    out->items[j] = moving;
  }
  return out;
}

Value* ParseReferenceQuote(Arena& arena, const Value* body, const Value* asset) {
  const Value* quotes = tj::Get(body, "quotes");
  if (!is_record(body) || !tj::IsArray(quotes)) return nullptr;
  const Value* symbol = tj::Get(asset, "tokenSymbol");
  const Value* contract = tj::Get(asset, "contractAddress");
  char contract_lower[43];
  if (!tj::IsString(contract) || contract->length != 42) return nullptr;
  for (size_t i = 0; i < 42; ++i) {
    const char c = contract->text[i];
    contract_lower[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c | 0x20) : c;
  }
  contract_lower[42] = '\0';
  const Value* raw = nullptr;
  for (uint32_t i = 0; i < quotes->count && !raw; ++i) {
    const Value* value = quotes->items[i];
    if (!is_record(value) || !cc::SameText(tj::Get(value, "tokenSymbol"), symbol)) continue;
    const Value* deployments = tj::Get(value, "deployments");
    if (!tj::IsArray(deployments)) continue;
    for (uint32_t k = 0; k < deployments->count; ++k) {
      const Value* d = deployments->items[k];
      if (is_record(d) && cc::JsNumber(tj::Get(d, "chainId")) == kChainId &&
          same_address(tj::Get(d, "contractAddress"), contract_lower)) {
        raw = value;
        break;
      }
    }
  }
  if (!raw || !cc::Is(tj::Get(raw, "currency"), "USD")) return nullptr;
  double raw_bid = 0, raw_ask = 0, multiplier = 0;
  Value* generated = iso_value(arena, tj::Get(raw, "generatedAt"));
  if (!positive(tj::Get(raw, "bid"), &raw_bid) || !positive(tj::Get(raw, "ask"), &raw_ask) ||
      !positive(tj::Get(asset, "currentMultiplier"), &multiplier) || !generated)
    return nullptr;
  const double bid = raw_bid * multiplier, ask = raw_ask * multiplier;
  if (!isfinite(bid) || !isfinite(ask) || ask < bid) return nullptr;
  // nonNegative(dailyTradingVolume).
  const Value* volume = tj::Get(raw, "dailyTradingVolume");
  bool has_volume = false;
  double volume_value = 0;
  if (tj::IsNumber(volume) || (tj::IsString(volume) && !cc::Blank(volume->text, volume->length))) {
    const double number = tj::IsNumber(volume) ? volume->number : cc::JsNumber(volume);
    if (isfinite(number) && number >= 0) { has_volume = true; volume_value = number; }
  }
  const Value* halted = tj::Get(raw, "isTradingHalt");
  Value* quote = tj::Obj(arena);
  if (!quote || !set(arena, quote, "symbol", const_cast<Value*>(symbol)) ||
      !set(arena, quote, "contractAddress", const_cast<Value*>(contract)) || !set(arena, quote, "bidUsd", tj::Num(arena, bid)) ||
      !set(arena, quote, "askUsd", tj::Num(arena, ask)) ||
      !set(arena, quote, "midpointUsd", tj::Num(arena, (bid + ask) / 2)) ||
      !set(arena, quote, "rawBidUsd", tj::Num(arena, raw_bid)) || !set(arena, quote, "rawAskUsd", tj::Num(arena, raw_ask)) ||
      !set(arena, quote, "multiplier", tj::Get(asset, "currentMultiplier")) ||
      !set(arena, quote, "dailyTradingVolume", has_volume ? tj::Num(arena, volume_value) : tj::Null(arena)) ||
      !set(arena, quote, "tradingHalted", tj::Bool(arena, halted && halted->type == tj::Type::kTrue)) ||
      !set(arena, quote, "generatedAt", generated) || !set(arena, quote, "currency", str(arena, "USD")))
    return nullptr;
  return quote;
}

Value* ParseCorporateActions(Arena& arena, const Value* body) {
  Value* out = tj::Arr(arena);
  const Value* actions = tj::Get(body, "corpActions");
  if (!out || !is_record(body) || !tj::IsArray(actions)) return out;
  for (uint32_t n = 0; n < actions->count; ++n) {
    const Value* raw = actions->items[n];
    if (!is_record(raw)) continue;
    const tj::Mark mark = tj::MarkOf(arena);
    Value* id = text_value(arena, tj::Get(raw, "id"), 68);
    Value* type = text_value(arena, tj::Get(raw, "type"), 80);
    Value* status = text_value(arena, tj::Get(raw, "status"), 80);
    Value* symbol = text_value(arena, tj::Get(raw, "tokenSymbol"), 32);
    const Value* deployment = chain_deployment(tj::Get(raw, "deployments"));
    if (!id || !type || !status || !symbol || !deployment) {
      tj::Rewind(arena, mark);
      if (!arena.ok) return nullptr;
      continue;
    }
    const Value* date = tj::Get(raw, "processDate");
    double year = 0, month = 0, day = 0;
    Value* process_date = nullptr;
    if (is_record(date) && integer_in(tj::Get(date, "year"), 1900, 9999, &year) &&
        integer_in(tj::Get(date, "month"), 1, 12, &month) && integer_in(tj::Get(date, "day"), 1, 31, &day)) {
      process_date = tj::Obj(arena);
      if (!process_date || !set(arena, process_date, "year", tj::Num(arena, year)) ||
          !set(arena, process_date, "month", tj::Num(arena, month)) || !set(arena, process_date, "day", tj::Num(arena, day)))
        return nullptr;
    }
    // boundedRecord(details): {} when JSON.stringify(details) is longer than
    // 8,000 UTF-16 units.
    const Value* details = tj::Get(raw, "details");
    Value* bounded = nullptr;
    if (is_record(details)) {
      OutBuffer measure{nullptr, 0, 0, true};
      tj::Stringify(details, measure);
      bool small = measure.used <= 8000 * 3;
      if (small && measure.used > 8000) {
        uint8_t* buffer = static_cast<uint8_t*>(arena.Alloc(measure.used, 1));
        OutBuffer copy{buffer, measure.used, 0, buffer != nullptr};
        small = buffer && tj::Stringify(details, copy) &&
                text::Utf16Length(reinterpret_cast<const char*>(buffer), copy.used) <= 8000;
      }
      bounded = small ? tj::Clone(arena, details) : tj::Obj(arena);
      json_round_trip(bounded);
    } else {
      bounded = tj::Obj(arena);
    }
    Value* action = tj::Obj(arena);
    if (!action || !set(arena, action, "id", id) || !set(arena, action, "type", type) ||
        !set(arena, action, "status", status) || !set(arena, action, "tokenSymbol", symbol) ||
        !set(arena, action, "contractAddress", tj::Clone(arena, tj::Get(deployment, "contractAddress"))) ||
        !set(arena, action, "processDate", or_null(arena, process_date)) || !set(arena, action, "details", bounded) ||
        !tj::Push(arena, out, action))
      return nullptr;
  }
  return out;
}

// ---- the client --------------------------------------------------------------------------

StockClient::~StockClient() = default;

void StockClient::Configure(chains::Transport* transport, fortis::base::Clock* clock) {
  transport_ = transport;
  clock_ = clock;
  if (!quotes_) { try { quotes_ = std::make_unique<QuoteEntry[]>(kQuoteEntries); } catch (const std::bad_alloc&) { quotes_.reset(); } }
}

void StockClient::Configure(Fetcher* fetcher, fortis::base::Clock* clock) {
  fetcher_ = fetcher;
  transport_ = nullptr;
  clock_ = clock;
  if (!quotes_) { try { quotes_ = std::make_unique<QuoteEntry[]>(kQuoteEntries); } catch (const std::bad_alloc&) { quotes_.reset(); } }
}

int64_t StockClient::Now() const { return clock_->NowMs(); }

const Value* StockClient::Document(const Snapshot* snapshot) { return snapshot ? snapshot->value : nullptr; }

bool StockClient::WaitLocked(uint64_t deadline_ms, const uint64_t* generation, uint64_t seen) {
  while (*generation == seen) {
    const uint64_t now = monotonic_ms();
    if (now >= deadline_ms) return false;
    timespec wake{};
    clock_gettime(CLOCK_REALTIME, &wake);
    const uint64_t wait = deadline_ms - now < 100 ? deadline_ms - now : 100;
    wake.tv_nsec += static_cast<long>(wait * 1000000);
    while (wake.tv_nsec >= 1000000000L) { ++wake.tv_sec; wake.tv_nsec -= 1000000000L; }
    changed_.WaitUntilLocked(mutex_, wake);
  }
  return true;
}

namespace {

// One GET on the robinhood-assets lane; the body parses into `arena`.
Outcome fetch(chains::Transport* transport, Arena& arena, uint64_t deadline_ms, const char* path, bool background,
              Value** body) {
  static const char* const kUrls[] = {kBase};
  chains::Call call;
  call.kind = chains::CallKind::kGet;
  call.urls = kUrls;
  call.url_count = 1;
  call.path = path;
  call.lane = kLane;
  call.background = background;
  Outcome outcome;
  if (!transport) {
    outcome.failure = Failure::kError;
    return outcome;
  }
  transport->Run(&call, 1, arena, deadline_ms);
  if (call.failure == chains::CallFailure::kNone && call.value) {
    *body = call.value;
  } else if (call.failure == chains::CallFailure::kLaneBusy) {
    outcome.failure = Failure::kLaneBusy;
    outcome.retry_after_ms = call.retry_after_ms;
  } else {
    outcome.failure = Failure::kError;
  }
  return outcome;
}

}  // namespace

Outcome StockClient::Fetcher::Fetch(Arena&, uint64_t, const char*, bool, Value**) {
  Outcome outcome;
  outcome.failure = Failure::kError;
  return outcome;
}

Outcome StockClient::Fetch(Arena& arena, uint64_t deadline_ms, const char* path, bool background, Value** body) {
  if (fetcher_) return fetcher_->Fetch(arena, deadline_ms, path, background, body);
  return fetch(transport_, arena, deadline_ms, path, background, body);
}

Outcome StockClient::Document(uint64_t deadline_ms, std::shared_ptr<const Snapshot>* cache, Flight* flight, const char* path, int64_t ttl_ms,
                              bool assets, bool background, uint64_t issued_generation, std::shared_ptr<const Snapshot>* out) {
  Outcome outcome;
  fortis::base::UniqueLock lock(mutex_);
  for (;;) {
    if (*cache && (*cache)->until_ms > Now()) {
      *out = *cache;
      return outcome;
    }
    if (!flight->running) {
      // A read issued before the last request finished, which failed: the
      // TypeScript caller awaited that request's promise.
      if (issued_generation < flight->generation && !flight->result.ok()) {
        outcome = flight->result;
        return outcome;
      }
      break;
    }
    // Join the request in flight: its result is this read's result.
    const uint64_t seen = flight->generation;
    if (!WaitLocked(deadline_ms, &flight->generation, seen)) {
      lock.Unlock();
      outcome.failure = Failure::kError;
      return outcome;
    }
    if (!flight->result.ok()) {
      outcome = flight->result;
      return outcome;
    }
    if (*cache) {
      *out = *cache;
      return outcome;
    }
  }
  flight->running = true;
  lock.Unlock();

  tj::Arena scratch = tj::GrowingArena(kFetchArenaBytes);
    std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> scratch_blocks(&scratch);
  Value* body = nullptr;
  outcome = Fetch(scratch, deadline_ms, path, background, &body);
  std::shared_ptr<Snapshot> snapshot;
  if (outcome.ok()) {
    try { snapshot = std::make_shared<Snapshot>(); } catch (const std::bad_alloc&) { snapshot.reset(); }
    if (snapshot) {
      snapshot->arena = tj::GrowingArena(kSnapshotArenaBytes);
      snapshot->value = assets ? ParseAssets(snapshot->arena, body) : ParseCorporateActions(snapshot->arena, body);
      if (!snapshot->value || !snapshot->arena.ok) {
        snapshot.reset();
      }
    }
    if (!snapshot) outcome.failure = Failure::kError;
  }
  tj::FreeArena(scratch);

  lock.Lock();
  if (snapshot) {
    snapshot->until_ms = Now() + ttl_ms;
    *cache = snapshot;
    *out = snapshot;
  }
  flight->running = false;
  flight->result = outcome;
  ++flight->generation;
  changed_.Broadcast();
  return outcome;
}

uint64_t StockClient::AssetsGeneration() {
  fortis::base::LockGuard lock(mutex_);
  const uint64_t generation = assets_flight_.generation;
  return generation;
}

Outcome StockClient::Assets(uint64_t deadline_ms, std::shared_ptr<const Snapshot>* out, bool background, uint64_t issued_generation) {
  return Document(deadline_ms, &assets_, &assets_flight_, "/rhj/assets", kAssetTtlMs, true, background,
                  issued_generation, out);
}

uint64_t StockClient::ActionsGeneration() {
  fortis::base::LockGuard lock(mutex_);
  const uint64_t generation = actions_flight_.generation;
  return generation;
}

Outcome StockClient::CorporateActions(uint64_t deadline_ms, std::shared_ptr<const Snapshot>* out, uint64_t issued_generation) {
  return Document(deadline_ms, &actions_, &actions_flight_, "/rhj/corporate-actions", kActionTtlMs, false, false,
                  issued_generation, out);
}

// Caller holds mutex_.
bool StockClient::FlattenLocked(const Snapshot* snapshot, providers::RobinhoodAssetCatalog* out) const {
  *out = providers::RobinhoodAssetCatalog{};
  const Value* assets = snapshot ? snapshot->value : nullptr;
  if (!tj::IsArray(assets)) return false;
  for (uint32_t i = 0; i < assets->count; ++i) {
    const Value* asset = assets->items[i];
    const Value* contract = tj::Get(asset, "contractAddress");
    const Value* symbol = tj::Get(asset, "tokenSymbol");
    double multiplier = 0;
    if (!tj::IsString(contract) || contract->length != 42 || !tj::IsString(symbol) ||
        !positive(tj::Get(asset, "currentMultiplier"), &multiplier))
      return false;
    if (symbol->length > providers::kRobinhoodMaxSymbolBytes) continue;
    providers::RobinhoodAsset flat{};
    for (size_t k = 0; k < 42; ++k) {
      const char c = contract->text[k];
      flat.address[k] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c | 0x20) : c;
    }
    memcpy(flat.symbol, symbol->text, symbol->length);
    flat.multiplier = multiplier;
    // A repeated contract: the later row replaces the earlier one.
    size_t slot = out->count;
    for (size_t k = 0; k < out->count; ++k)
      if (strcmp(out->assets[k].address, flat.address) == 0) slot = k;
    if (slot == out->count) {
      if (out->count == providers::kRobinhoodMaxAssets) return false;
      ++out->count;
    }
    out->assets[slot] = flat;
  }
  return true;
}

bool StockClient::FreshCatalog(providers::RobinhoodAssetCatalog* out) {
  fortis::base::LockGuard lock(mutex_);
  const bool fresh = clock_ && assets_ && assets_->until_ms > Now() && FlattenLocked(assets_.get(), out);
  return fresh;
}

bool StockClient::AdoptCatalog(const uint8_t* body, size_t length, providers::RobinhoodAssetCatalog* out) {
  if (!clock_ || !body) return false;
  tj::Arena scratch = tj::GrowingArena(kFetchArenaBytes);
    std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> scratch_blocks(&scratch);
  const Value* parsed = tj::Parse(scratch, body, length);
  std::shared_ptr<Snapshot> snapshot;
  if (parsed) { try { snapshot = std::make_shared<Snapshot>(); } catch (const std::bad_alloc&) { snapshot.reset(); } }
  if (snapshot) {
    snapshot->arena = tj::GrowingArena(kSnapshotArenaBytes);
    snapshot->value = ParseAssets(snapshot->arena, parsed);
    if (!snapshot->value || !snapshot->arena.ok) {
      snapshot.reset();
    }
  }
  tj::FreeArena(scratch);
  if (!snapshot) return false;
  fortis::base::LockGuard lock(mutex_);
  snapshot->until_ms = Now() + kAssetTtlMs;
  assets_ = snapshot;
  const bool flat = FlattenLocked(snapshot.get(), out);
  return flat;
}

uint64_t StockClient::QuoteGeneration() {
  fortis::base::LockGuard lock(mutex_);
  const uint64_t generation = quote_generation_;
  return generation;
}

Outcome StockClient::ReferenceQuote(Arena& arena, uint64_t deadline_ms, const Value* asset, Value** quote,
                                    bool background, uint64_t issued_generation) {
  *quote = nullptr;
  Outcome outcome;
  const Value* contract = tj::Get(asset, "contractAddress");
  const Value* symbol = tj::Get(asset, "tokenSymbol");
  if (!quotes_ || !tj::IsString(contract) || contract->length != 42 || !tj::IsString(symbol)) {
    outcome.failure = Failure::kError;
    return outcome;
  }
  char key[43];
  for (size_t i = 0; i < 42; ++i) {
    const char c = contract->text[i];
    key[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c | 0x20) : c;
  }
  key[42] = '\0';
  // The quote object again: its JSON text, and the sign of a -0 volume.
  auto restore = [](Value* parsed, bool negative_zero) {
    Value* volume = negative_zero ? tj::Get(parsed, "dailyTradingVolume") : nullptr;
    if (tj::IsNumber(volume)) volume->number = -0.0;
  };
  auto read_cached = [&](const QuoteEntry& entry) {
    *quote = entry.json ? tj::Parse(arena, reinterpret_cast<const uint8_t*>(entry.json.get()), entry.length) : nullptr;
    if (entry.json && !*quote) outcome.failure = Failure::kError;
    restore(*quote, entry.negative_zero_volume);
  };
  fortis::base::UniqueLock lock(mutex_);
  QuoteEntry* entry = nullptr;
  for (size_t i = 0; i < kQuoteEntries && !entry; ++i)
    if (quotes_[i].used && strcmp(quotes_[i].key, key) == 0) entry = &quotes_[i];
  for (;;) {
    if (entry && entry->json && entry->until_ms > Now()) {
      read_cached(*entry);
      return outcome;
    }
    if (entry && !entry->running && entry->finished > issued_generation) {
      // A request for this contract finished after this read was issued:
      // a TypeScript caller would have awaited that request's promise, so
      // its result (a failure, a null quote or its quote) is this read's.
      if (!entry->result.ok()) outcome = entry->result;
      else if (entry->has_result) read_cached(*entry);
      return outcome;
    }
    if (!entry || !entry->running) break;
    const uint64_t seen = entry->generation;
    ++entry->waiters;
    const bool joined = WaitLocked(deadline_ms, &entry->generation, seen);
    --entry->waiters;
    if (!joined) {
      lock.Unlock();
      outcome.failure = Failure::kError;
      return outcome;
    }
    // The joined request's result: a failure, a null quote or its quote.
    if (!entry->result.ok()) {
      outcome = entry->result;
    } else if (entry->has_result) {
      read_cached(*entry);
    }
    return outcome;
  }
  if (!entry) {
    // A free slot, else the stalest idle one (the TypeScript map is
    // unbounded; this cache keeps at most kQuoteEntries contracts).
    for (size_t i = 0; i < kQuoteEntries && !entry; ++i)
      if (!quotes_[i].used) entry = &quotes_[i];
    // An entry a joiner has still to read is never reused.
    for (size_t i = 0; i < kQuoteEntries; ++i)
      if (!quotes_[i].running && !quotes_[i].waiters &&
          (!entry || (entry->used && quotes_[i].until_ms < entry->until_ms)))
        entry = &quotes_[i];
    if (entry) {
      entry->json.reset();
      const uint64_t generation = entry->generation;
      *entry = QuoteEntry{};
      entry->generation = generation;
      entry->used = true;
      memcpy(entry->key, key, 43);
    }
  }
  if (entry) {
    entry->running = true;
    entry->has_result = false;
  }
  lock.Unlock();

  // `${BASE}/prices/${encodeURIComponent(asset.tokenSymbol)}`.
  char path[512];
  OutBuffer encoded{reinterpret_cast<uint8_t*>(path), sizeof(path) - 1, 0, true};
  encoded.Text("/rhj/prices/");
  Value* parsed = nullptr;
  std::unique_ptr<char[], base::StorageDeleter> json;
  size_t json_length = 0;
  bool negative_zero = false;
  if (!cc::EncodeComponent(symbol, encoded) || !encoded.ok) {
    outcome.failure = Failure::kError;
  } else {
    path[encoded.used] = '\0';
    tj::Arena scratch = tj::GrowingArena(kFetchArenaBytes);
    std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> scratch_blocks(&scratch);
    Value* body = nullptr;
    outcome = Fetch(scratch, deadline_ms, path, background, &body);
    if (outcome.ok()) {
      parsed = ParseReferenceQuote(scratch, body, asset);
      if (parsed) {
        const Value* volume = tj::Get(parsed, "dailyTradingVolume");
        negative_zero = tj::IsNumber(volume) && volume->number == 0 && signbit(volume->number);
        OutBuffer measure{nullptr, 0, 0, true};
        tj::Stringify(parsed, measure);
        json = base::AllocateStorage<char[]>(measure.used ? measure.used : 1);
        OutBuffer write{reinterpret_cast<uint8_t*>(json.get()), measure.used, 0, json != nullptr};
        if (!json || !tj::Stringify(parsed, write) || !write.ok) {
          json.reset();
          json = nullptr;
          outcome.failure = Failure::kError;
        } else {
          json_length = write.used;
        }
      }
    }
    tj::FreeArena(scratch);
  }
  if (outcome.ok() && json) {
    *quote = tj::Parse(arena, reinterpret_cast<const uint8_t*>(json.get()), json_length);
    if (!*quote) outcome.failure = Failure::kError;
    restore(*quote, negative_zero);
  }
  lock.Lock();
  if (entry) {
    if (outcome.ok() && json) {
      entry->json.reset();
      entry->json = std::move(json);
      entry->length = json_length;
      entry->until_ms = Now() + kQuoteTtlMs;
      entry->negative_zero_volume = negative_zero;
      json = nullptr;
    }
    entry->running = false;
    entry->result = outcome;
    // A joiner reads entry->json only when this request produced a quote.
    entry->has_result = outcome.ok() && *quote != nullptr;
    ++entry->generation;
    entry->finished = ++quote_generation_;
    changed_.Broadcast();
  }
  lock.Unlock();
  json.reset();
  return outcome;
}

// ---- the read model -----------------------------------------------------------------------

namespace {

// The display logo (stockLogoUrl(symbol) ?? asset.logoUrl) and its media key.
bool display_logo(Arena& arena, const MarketsRequest& request, const Value* asset, Value** logo, Value** media_key) {
  const Value* symbol = tj::Get(asset, "tokenSymbol");
  const char* stock = StockLogoUrl(arena, symbol->text, symbol->length);
  const Value* issuer = tj::Get(asset, "logoUrl");
  *logo = stock ? str(arena, stock) : tj::IsString(issuer) ? const_cast<Value*>(issuer) : tj::Null(arena);
  *media_key = tj::Null(arena);
  if (!*logo || !*media_key) return false;
  if (tj::IsString(*logo)) {
    char key[fortis::discovery::market_view::kMediaKeyChars + 1];
    if (discovery::RegisterMediaSource(request.kv, (*logo)->text, (*logo)->length, request.now_ms, key)) *media_key = str(arena, key);
  }
  return *media_key != nullptr;
}

Value* asset_wire(Arena& arena, const Value* asset, Value* logo, Value* media_key) {
  Value* deployment = tj::Obj(arena);
  Value* deployments = tj::Arr(arena);
  Value* out = tj::Obj(arena);
  const Value* pending = tj::Get(asset, "pendingMultiplier");
  const Value* status = tj::Get(asset, "status");
  if (!deployment || !deployments || !out || !set(arena, deployment, "chainId", tj::Num(arena, kChainId)) ||
      !set(arena, deployment, "contractAddress", tj::Get(asset, "contractAddress")) ||
      !tj::Push(arena, deployments, deployment))
    return nullptr;
  const bool ok =
      set(arena, out, "id", tj::Get(asset, "id")) && set(arena, out, "tokenSymbol", tj::Get(asset, "tokenSymbol")) &&
      set(arena, out, "tokenName", tj::Get(asset, "tokenName")) && set(arena, out, "deployments", deployments) &&
      set(arena, out, "currentMultiplier", tj::Get(asset, "currentMultiplier")) &&
      set(arena, out, "pendingMultiplier", tj::IsString(pending) ? const_cast<Value*>(pending) : str(arena, "")) &&
      set(arena, out, "pendingMultiplierEffectiveTime", tj::Get(asset, "pendingMultiplierEffectiveTime")) &&
      set(arena, out, "logoUrl", logo) && set(arena, out, "issuerLogoUrl", tj::Get(asset, "logoUrl")) &&
      set(arena, out, "logoMediaKey", media_key) &&
      set(arena, out, "status",
          str(arena, cc::Is(status, "active")     ? "ASSET_STATUS_ACTIVE"
                     : cc::Is(status, "inactive") ? "ASSET_STATUS_INACTIVE"
                                                  : "ASSET_STATUS_UNKNOWN")) &&
      set(arena, out, "tradingCapabilities", tj::Get(asset, "tradingCapabilities"));
  return ok ? out : nullptr;
}

double action_date(const Value* action) {
  const Value* date = tj::Get(action, "processDate");
  if (!is_record(date)) return -INFINITY;
  const double year = tj::Get(date, "year")->number, month = tj::Get(date, "month")->number,
               day = tj::Get(date, "day")->number;
  return static_cast<double>(utc_days(static_cast<int64_t>(year), static_cast<int64_t>(month), static_cast<int64_t>(day))) *
         86400000.0;
}

}  // namespace

Outcome Catalog(Arena& arena, const MarketsRequest& request, Value** result, std::shared_ptr<const StockClient::Snapshot>* held) {
  *result = nullptr;
  *held = nullptr;
  Outcome outcome = request.client->Assets(request.deadline_ms, held, false, request.assets_issued);
  if (!outcome.ok()) return outcome;
  const Value* assets = StockClient::Document(held->get());
  Value* list = tj::Arr(arena);
  Value* out = tj::Obj(arena);
  bool ok = list && out;
  for (uint32_t i = 0; ok && i < assets->count; ++i) {
    Value* logo = nullptr;
    Value* key = nullptr;
    ok = display_logo(arena, request, assets->items[i], &logo, &key);
    Value* wire = ok ? asset_wire(arena, assets->items[i], logo, key) : nullptr;
    ok = wire && tj::Push(arena, list, wire);
  }
  ok = ok && set(arena, out, "assets", list) && set(arena, out, "source", str(arena, kSource));
  if (!ok) outcome.failure = Failure::kError;
  else *result = out;
  return outcome;
}

Outcome FindInstrument(const MarketsRequest& request, const char* address, const Value** asset,
                       std::shared_ptr<const StockClient::Snapshot>* held_assets) {
  *asset = nullptr;
  *held_assets = nullptr;
  Outcome outcome = request.client->Assets(request.deadline_ms, held_assets, false, request.assets_issued);
  if (!outcome.ok()) return outcome;
  // assetByAddress: the first asset whose contract matches, case-insensitively.
  const Value* assets = StockClient::Document(held_assets->get());
  for (uint32_t i = 0; i < assets->count && !*asset; ++i)
    if (same_address(tj::Get(assets->items[i], "contractAddress"), address)) *asset = assets->items[i];
  return outcome;
}

bool BuildInstrument(Arena& arena, const MarketsRequest& request, const Value* asset, Value* quote,
                     const StockClient::Snapshot* held_actions, Value** result) {
  *result = nullptr;
  // corporateActionsFor: this contract's actions, newest process date first
  // (stable; an undated action sorts last).
  const Value* actions = StockClient::Document(held_actions);
  const Value* contract = tj::Get(asset, "contractAddress");
  if (!tj::IsArray(actions) || !tj::IsString(contract) || contract->length != 42) return false;
  char lower[43];
  for (size_t i = 0; i < 42; ++i) {
    const char c = contract->text[i];
    lower[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c | 0x20) : c;
  }
  lower[42] = '\0';
  Value* selected = tj::Arr(arena);
  bool ok = selected != nullptr;
  for (uint32_t i = 0; ok && i < actions->count; ++i)
    if (same_address(tj::Get(actions->items[i], "contractAddress"), lower)) ok = tj::Push(arena, selected, actions->items[i]);
  // .sort((left, right) => date(right) - date(left)): newest first, stable
  // (V8's TimSort); an undated action (-Infinity) sorts last and two undated
  // ones compare NaN, i.e. equal. The comparator is a consistent preorder, so
  // any stable sort gives TimSort's order: a bottom-up merge sort over the
  // precomputed dates (n log n; a 512 KiB document holds ~3,700 actions).
  const uint32_t n = ok ? selected->count : 0;
  if (n > 1) {
    auto* dates = static_cast<double*>(arena.Alloc(sizeof(double) * n * 2, alignof(double)));
    auto* rows = static_cast<Value**>(arena.Alloc(sizeof(Value*) * n, alignof(Value*)));
    ok = dates && rows;
    for (uint32_t i = 0; ok && i < n; ++i) dates[i] = action_date(selected->items[i]);
    double* from_dates = dates;
    double* to_dates = dates + n;
    Value** from_rows = selected->items;
    Value** to_rows = rows;
    for (uint32_t width = 1; ok && width < n; width *= 2) {
      for (uint32_t low = 0; low < n; low += 2 * width) {
        const uint32_t middle = low + width < n ? low + width : n;
        const uint32_t high = low + 2 * width < n ? low + 2 * width : n;
        uint32_t left = low, right = middle, out = low;
        while (left < middle && right < high) {
          // The right row goes first only when it is strictly newer.
          if (from_dates[right] - from_dates[left] > 0) {
            to_dates[out] = from_dates[right];
            to_rows[out++] = from_rows[right++];
          } else {
            to_dates[out] = from_dates[left];
            to_rows[out++] = from_rows[left++];
          }
        }
        while (left < middle) { to_dates[out] = from_dates[left]; to_rows[out++] = from_rows[left++]; }
        while (right < high) { to_dates[out] = from_dates[right]; to_rows[out++] = from_rows[right++]; }
      }
      double* swap_dates = from_dates;
      from_dates = to_dates;
      to_dates = swap_dates;
      Value** swap_rows = from_rows;
      from_rows = to_rows;
      to_rows = swap_rows;
    }
    if (ok && from_rows != selected->items) std::copy_n(from_rows, n, selected->items);
  }
  Value* logo = nullptr;
  Value* media_key = nullptr;
  Value* disclosures = tj::Obj(arena);
  Value* out = tj::Obj(arena);
  ok = ok && display_logo(arena, request, asset, &logo, &media_key) && disclosures && out &&
       set(arena, disclosures, "instrument", str(arena, "Tokenized debt security")) &&
       set(arena, disclosures, "ownership", str(arena, "Economic exposure; not direct share ownership or voting rights")) &&
       set(arena, disclosures, "history",
           str(arena, "Chart history is built prospectively from observed issuer reference quotes")) &&
       set(arena, out, "asset", asset_wire(arena, asset, logo, media_key)) &&
       set(arena, out, "quote", or_null(arena, quote)) && set(arena, out, "corporateActions", selected) &&
       set(arena, out, "source", str(arena, kSource)) && set(arena, out, "disclosures", disclosures);
  if (ok) *result = out;
  return ok;
}

// ---- protobuf -------------------------------------------------------------------------------

bool EncodeCatalog(OutBuffer& out, const Value* catalog) {
  const Value* assets = tj::Get(catalog, "assets");
  for (uint32_t i = 0; tj::IsArray(assets) && i < assets->count; ++i)
    if (!embed(out, 1, encode_asset, assets->items[i])) return false;
  implicit_string(out, 2, catalog, "source");
  return out.ok;
}

bool EncodeInstrument(OutBuffer& out, const Value* instrument) {
  if (!embed(out, 1, encode_asset, tj::Get(instrument, "asset"))) return false;
  const Value* quote = tj::Get(instrument, "quote");
  if (is_record(quote) && !embed(out, 2, encode_quote, quote)) return false;
  const Value* actions = tj::Get(instrument, "corporateActions");
  for (uint32_t i = 0; tj::IsArray(actions) && i < actions->count; ++i)
    if (!embed(out, 3, encode_action, actions->items[i])) return false;
  implicit_string(out, 4, instrument, "source");
  const Value* disclosures = tj::Get(instrument, "disclosures");
  for (uint32_t i = 0; is_record(disclosures) && i < disclosures->count; ++i) {
    const tj::Member& member = disclosures->members[i];
    if (!tj::IsString(member.value)) continue;
    Value key{};
    key.type = tj::Type::kString;
    key.text = member.key;
    key.length = member.key_length;
    Value* pair_items[2] = {&key, member.value};
    Value pair{};
    pair.type = tj::Type::kArray;
    pair.items = pair_items;
    pair.count = 2;
    if (!embed(out, 5, encode_map_entry, &pair)) return false;
  }
  return out.ok;
}

}  // namespace fortis::exchange::robinhood
