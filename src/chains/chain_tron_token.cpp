#include "chain_tron_token.h"

#include "chain_core.h"
#include "token_text.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::chains::tron_token {
namespace {

constexpr double kMaxSafe = 9007199254740991.0;
// The TypeScript amount helpers' MAX_DECIMALS.
constexpr int kMaxScaleDecimals = 24;

bool nullish(const Value* v) { return !v || v->type == tj::Type::kNull; }

// `a ?? b`.
const Value* coalesce(const Value* a, const Value* b) { return nullish(a) ? b : a; }
const Value* coalesce(const Value* a, const Value* b, const Value* c) { return coalesce(coalesce(a, b), c); }

bool all_digits(const char* text, size_t length) {
  if (!length) return false;
  for (size_t i = 0; i < length; ++i) if (text[i] < '0' || text[i] > '9') return false;
  return true;
}

void append_js_string(const Value* v, tj::OutBuffer& out) {
  char number[40];
  if (!v) { out.Text("undefined"); return; }
  switch (v->type) {
    case tj::Type::kNull: out.Text("null"); return;
    case tj::Type::kTrue: out.Text("true"); return;
    case tj::Type::kFalse: out.Text("false"); return;
    case tj::Type::kNumber: chains::JsNumberText(v->number, number, sizeof(number)); out.Text(number); return;
    case tj::Type::kString: out.Append(v->text, v->length); return;
    case tj::Type::kObject: out.Text("[object Object]"); return;
    case tj::Type::kArray:
      for (uint32_t i = 0; i < v->count; ++i) {
        if (i) out.Byte(',');
        if (!nullish(v->items[i])) append_js_string(v->items[i], out);
      }
      return;
  }
}

// safeInt(value, max): a number, or an all-digit string read with Number();
// a safe integer in [0, max].
bool safe_int(const Value* v, double max, double* out) {
  double parsed = NAN;
  if (tj::IsNumber(v)) parsed = v->number;
  else if (tj::IsString(v) && all_digits(v->text, v->length)) parsed = compat::token_text::ToNumber(v->text, v->length);
  if (!isfinite(parsed) || floor(parsed) != parsed || fabs(parsed) > kMaxSafe) return false;
  if (parsed < 0 || parsed > max) return false;
  *out = parsed;
  return true;
}

// safeSymbol / safeName: a non-blank string, sanitized. *ok false only on
// arena exhaustion.
Value* safe_text(Arena& a, const Value* v, bool symbol, bool* ok) {
  *ok = true;
  if (!tj::IsString(v)) return nullptr;
  size_t begin = 0, end = 0;
  compat::token_text::Trim(v->text, v->length, &begin, &end);
  if (begin == end) return nullptr;
  Value* out = symbol ? compat::token_text::SanitizeSymbol(a, v->text, v->length)
                      : compat::token_text::SanitizeName(a, v->text, v->length);
  *ok = out != nullptr;
  return out;
}

// validTronAddress(value).
Value* valid_address(Arena& a, const Value* v, bool* ok) {
  *ok = true;
  if (!tj::IsString(v) || !IsAddress(v->text, v->length)) return nullptr;
  Value* out = tj::Str(a, v->text, v->length);
  *ok = out != nullptr;
  return out;
}

// typeof row.vip === 'boolean' ? row.vip : typeof row.verified === 'boolean' ? … : null.
int8_t verified_of(const Value* row) {
  const Value* vip = tj::Get(row, "vip");
  if (tj::IsBool(vip)) return vip->type == tj::Type::kTrue ? 1 : 0;
  const Value* verified = tj::Get(row, "verified");
  if (tj::IsBool(verified)) return verified->type == tj::Type::kTrue ? 1 : 0;
  return -1;
}

// The fields both TronScan rows read; the per-standard aliases differ.
struct RowKeys {
  const char* symbol[3];
  const char* name[2];
  const char* decimals[3];
  double max_decimals;
  const char* supply[2];
  const char* transfers[3];
  const char* issuer[3];
};

const Value* first_of(const Value* row, const char* const* keys, size_t count) {
  const Value* value = nullptr;
  for (size_t i = count; i-- > 0;) {
    if (!keys[i]) continue;
    value = value ? coalesce(tj::Get(row, keys[i]), value) : tj::Get(row, keys[i]);
  }
  return value;
}

Read tronscan_row(Arena& a, const Value* row, const RowKeys& keys, bool trc10, Snapshot* out) {
  *out = Snapshot{};
  out->trc10 = trc10;
  out->tronscan = true;
  bool ok = true, step = true;
  out->symbol = safe_text(a, first_of(row, keys.symbol, 3), true, &step);
  ok = ok && step;
  out->name = safe_text(a, first_of(row, keys.name, 2), false, &step);
  ok = ok && step;
  out->has_decimals = safe_int(first_of(row, keys.decimals, 3), keys.max_decimals, &out->decimals);
  const Value* supply = first_of(row, keys.supply, 2);
  out->total_supply_raw = IntegerString(a, supply);
  static const char* const kHolders[] = {"nrOfTokenHolders", "holders", "holder_count"};
  out->has_holders = safe_int(first_of(row, kHolders, 3), kMaxSafe, &out->holders);
  out->has_transfers = safe_int(first_of(row, keys.transfers, 3), kMaxSafe, &out->transfers);
  out->issuer = valid_address(a, first_of(row, keys.issuer, 3), &step);
  ok = ok && step && a.ok;
  out->verified = verified_of(row);
  return ok ? Read::kSnapshot : Read::kThrow;
}

// A digit string scaled by `decimals` (rawToDecimalString over BigInt(raw)):
// exact for any length. JSON null past MAX_DECIMALS.
Value* scale_digits(Arena& a, const Value* raw, double decimals) {
  if (decimals < 0 || decimals > kMaxScaleDecimals) return tj::Null(a);
  const size_t places = static_cast<size_t>(decimals);
  const char* digits = raw->text;
  size_t length = raw->length;
  while (length > 1 && digits[0] == '0') { ++digits; --length; }  // BigInt(raw).toString()
  const size_t padded = length < places + 1 ? places + 1 : length;
  char* text = static_cast<char*>(a.Alloc(padded + 2, 1));
  if (!text) return nullptr;
  size_t used = 0;
  for (size_t i = length; i < padded; ++i) text[used++] = '0';
  memcpy(text + used, digits, length);
  used += length;
  if (places == 0) return tj::Str(a, text, used);
  // whole + '.' + fraction without trailing zeros.
  size_t fraction_end = used;
  while (fraction_end > used - places && text[fraction_end - 1] == '0') --fraction_end;
  const size_t whole = used - places;
  if (fraction_end == whole) return tj::Str(a, text, whole);
  memmove(text + whole + 1, text + whole, fraction_end - whole);
  text[whole] = '.';
  return tj::Str(a, text, fraction_end + 1);
}

Value* number_text(Arena& a, double value) {
  char text[40];
  chains::JsNumberText(value, text, sizeof(text));
  return tj::Str(a, text);
}

Value* row(Arena& a, const char* label, Value* value) {
  Value* o = tj::Obj(a);
  if (!o || !value || !tj::Set(a, o, "label", tj::Str(a, label)) || !tj::Set(a, o, "value", value)) return nullptr;
  return o;
}

Value* section(Arena& a, const char* title, Value* summary, Value* rows) {
  Value* o = tj::Obj(a);
  if (!o || !summary || !tj::Set(a, o, "title", tj::Str(a, title)) || !tj::Set(a, o, "summary", summary) ||
      !tj::Set(a, o, "ok", tj::Bool(a, true)) || !tj::Set(a, o, "rows", rows))
    return nullptr;
  return o;
}

Value* or_null(Arena& a, Value* v) { return v ? v : tj::Null(a); }

}  // namespace

const char* const kOnChainSelectors[4] = {"totalSupply()", "decimals()", "symbol()", "name()"};

bool IsAddress(const char* text, size_t length) {
  if (length != 34 || text[0] != 'T') return false;
  for (size_t i = 1; i < length; ++i) {
    const char c = text[i];
    const bool ok = (c >= '1' && c <= '9') || (c >= 'A' && c <= 'H') || (c >= 'J' && c <= 'N') ||
                    (c >= 'P' && c <= 'Z') || (c >= 'a' && c <= 'k') || (c >= 'm' && c <= 'z');
    if (!ok) return false;
  }
  return true;
}

bool IsTrc10Id(const char* text, size_t length) { return length >= 1 && length <= 20 && all_digits(text, length); }

bool JsStringEquals(const Value* v, const char* text) {
  uint8_t bytes[1024];
  tj::OutBuffer out{bytes, sizeof(bytes), 0, true};
  append_js_string(v, out);
  return out.ok && out.used == strlen(text) && memcmp(bytes, text, out.used) == 0;
}

Value* IntegerString(Arena& a, const Value* v) {
  if (tj::IsString(v) && all_digits(v->text, v->length)) return tj::Str(a, v->text, v->length);
  if (tj::IsNumber(v) && isfinite(v->number) && floor(v->number) == v->number && v->number >= 0 &&
      v->number <= kMaxSafe)
    return number_text(a, v->number);
  return nullptr;
}

Read TronscanTrc20(Arena& a, const Value* body, const char* contract, Snapshot* out) {
  *out = Snapshot{};
  if (nullish(body)) return Read::kThrow;  // body.data on null
  // [body.data, body.trc20_tokens, body.tokens].find(Array.isArray)
  const Value* rows = nullptr;
  static const char* const kLists[] = {"data", "trc20_tokens", "tokens"};
  for (const char* key : kLists) {
    const Value* candidate = tj::Get(body, key);
    if (tj::IsArray(candidate)) { rows = candidate; break; }
  }
  auto identity = [](const Value* r) {
    return coalesce(tj::Get(r, "contract_address"), tj::Get(r, "contractAddress"), tj::Get(r, "tokenId"));
  };
  const Value* match = nullptr;
  if (rows) {
    for (uint32_t i = 0; i < rows->count; ++i) {
      const Value* candidate = rows->items[i];
      if (nullish(candidate)) return Read::kThrow;  // value.contract_address on null
      if (chains::Equals(identity(candidate), contract)) { match = candidate; break; }
    }
  }
  // ?? (isTronscanTokenRow(body) ? body : undefined)
  if (!match && tj::IsObject(body)) match = body;
  if (!match) return Read::kNull;
  const Value* found = identity(match);
  if (found && !chains::Equals(found, contract)) return Read::kNull;  // found !== undefined && !== contract
  const Value* type = tj::Get(match, "tokenType");
  if (tj::IsString(type)) {
    Value* lower = compat::token_text::ToLowerCase(a, type->text, type->length);
    if (!lower) return Read::kThrow;
    if (!chains::Equals(lower, "trc20")) return Read::kNull;
  }
  static const RowKeys kKeys = {
      {"tokenAbbr", "symbol", nullptr}, {"tokenName", "name"}, {"tokenDecimal", "decimals", nullptr}, 255,
      {"total_supply", "totalSupply"}, {"transferCount", "transfer_count", nullptr},
      {"issue_address", "issueAddress", nullptr}};
  return tronscan_row(a, match, kKeys, false, out);
}

Read TronscanTrc10(Arena& a, const Value* body, const char* asset_id, Snapshot* out) {
  *out = Snapshot{};
  if (nullish(body)) return Read::kThrow;  // body.data on null
  const Value* data = tj::Get(body, "data");
  if (!tj::IsArray(data)) return Read::kNull;
  for (uint32_t i = 0; i < data->count; ++i) {
    const Value* candidate = data->items[i];
    if (nullish(candidate)) return Read::kThrow;
    // `${value.tokenID ?? value.id ?? ''}` === assetId
    const Value* id = coalesce(tj::Get(candidate, "tokenID"), tj::Get(candidate, "id"));
    if (!(nullish(id) ? asset_id[0] == '\0' : JsStringEquals(id, asset_id))) continue;
    static const RowKeys kKeys = {
        {"abbr", "tokenAbbr", "symbol"}, {"name", "tokenName"}, {"precision", "tokenDecimal", "decimals"}, 36,
        {"totalSupply", "total_supply"}, {"totalTransactions", "transferCount", "transfer_count"},
        {"ownerAddress", "issue_address", "issueAddress"}};
    return tronscan_row(a, candidate, kKeys, true, out);
  }
  return Read::kNull;
}

Read OnChainTrc10(Arena& a, const Value* body, const char* asset_id, Snapshot* out) {
  *out = Snapshot{};
  if (nullish(body)) return Read::kNull;  // row.id on null throws; caught as null
  const Value* id = tj::Get(body, "id");
  if (!(nullish(id) ? asset_id[0] == '\0' : JsStringEquals(id, asset_id))) return Read::kNull;
  double decimals = 0;
  if (!safe_int(tj::Get(body, "precision"), 36, &decimals)) return Read::kNull;
  bool ok = true;
  Value* symbol = safe_text(a, tj::Get(body, "abbr"), true, &ok);
  if (!ok) return Read::kThrow;
  if (!symbol) return Read::kNull;
  out->trc10 = true;
  out->symbol = symbol;
  Value* name = safe_text(a, tj::Get(body, "name"), false, &ok);
  if (!ok) return Read::kThrow;
  out->name = name ? name : symbol;  // safeName(row.name) ?? symbol
  out->has_decimals = true;
  out->decimals = decimals;
  out->total_supply_raw = IntegerString(a, tj::Get(body, "total_supply"));
  out->issuer = valid_address(a, tj::Get(body, "owner_address"), &ok);
  return ok && a.ok ? Read::kSnapshot : Read::kThrow;
}

Read ViewResult(Arena& a, const Value* body, const Value** result) {
  *result = nullptr;
  if (nullish(body)) return Read::kThrow;  // body.result on null
  const Value* status = tj::Get(tj::Get(body, "result"), "result");
  if (!status || status->type != tj::Type::kTrue) return Read::kNull;
  const Value* results = tj::Get(body, "constant_result");
  const Value* first = nullptr;
  if (tj::IsArray(results)) {
    first = results->count ? results->items[0] : nullptr;
  } else if (tj::IsString(results) && results->length) {
    // `[0]` of a string: its first character.
    size_t length = 1;
    const unsigned char lead = static_cast<unsigned char>(results->text[0]);
    if (lead >= 0xF0) length = 4;
    else if (lead >= 0xE0) length = 3;
    else if (lead >= 0xC0) length = 2;
    if (length > results->length) length = results->length;
    first = tj::Str(a, results->text, length);
    if (!first) return Read::kThrow;
  } else if (tj::IsObject(results)) {
    first = tj::Get(results, "0");
  }
  if (nullish(first)) return Read::kNull;
  *result = first;
  return Read::kSnapshot;
}

Read OnChainTrc20(Arena& a, const Value* const bodies[4], Snapshot* out) {
  *out = Snapshot{};
  const Value* views[4] = {};
  for (size_t i = 0; i < 4; ++i) {
    if (!bodies[i]) return Read::kNull;  // a failed request rejects Promise.all
    if (ViewResult(a, bodies[i], &views[i]) == Read::kThrow) return a.ok ? Read::kNull : Read::kThrow;
  }
  const Value* supply_hex = views[0];
  const Value* decimals_hex = views[1];
  if (!supply_hex || !decimals_hex) return Read::kNull;
  // decodeUint256 of a non-string or a non-hex word throws.
  if (!tj::IsString(supply_hex) || !tj::IsString(decimals_hex)) return Read::kNull;
  chains::BigInt decimals, supply;
  if (!chains::AbiUint256Checked(decimals_hex->text, decimals_hex->length, &decimals)) return Read::kNull;
  if (decimals.Compare(chains::BigInt::FromU64(255)) > 0) return Read::kNull;
  // decodeAbiSymbol / decodeAbiName run before the supply decode in the
  // object literal, and both are caught; the supply decode is not.
  Value* texts[2] = {};
  for (size_t k = 0; k < 2; ++k) {
    const Value* hex = views[2 + k];
    if (!tj::IsString(hex)) continue;  // null, or strip0x on a non-string: caught as null
    Value* decoded = chains::AbiString(a, hex->text, hex->length);
    if (!decoded) return Read::kThrow;
    bool ok = true;
    texts[k] = safe_text(a, decoded, k == 0, &ok);
    if (!ok) return Read::kThrow;
  }
  if (!chains::AbiUint256Checked(supply_hex->text, supply_hex->length, &supply)) return Read::kNull;
  out->trc10 = false;
  out->symbol = texts[0];
  out->name = texts[1];
  out->has_decimals = true;
  out->decimals = decimals.ToDouble();
  out->total_supply_raw = supply.ToValue(a);
  if (!out->total_supply_raw) return Read::kThrow;
  return Read::kSnapshot;
}

bool Trc20Path(const char* contract, char* out, size_t capacity) {
  uint8_t encoded[256];
  tj::OutBuffer buffer{encoded, sizeof(encoded) - 1, 0, true};
  if (!chains::EncodeUriComponent(contract, strlen(contract), buffer)) return false;
  const int n = snprintf(out, capacity, "/api/token_trc20?contract=%.*s", static_cast<int>(buffer.used),
                         reinterpret_cast<const char*>(encoded));
  return n > 0 && static_cast<size_t>(n) < capacity;
}

bool Trc10Path(const char* asset_id, char* out, size_t capacity) {
  uint8_t encoded[256];
  tj::OutBuffer buffer{encoded, sizeof(encoded) - 1, 0, true};
  if (!chains::EncodeUriComponent(asset_id, strlen(asset_id), buffer)) return false;
  const int n = snprintf(out, capacity, "/api/token?id=%.*s&showAll=1&start=0&limit=1",
                         static_cast<int>(buffer.used), reinterpret_cast<const char*>(encoded));
  return n > 0 && static_cast<size_t>(n) < capacity;
}

// The identifiers are validated base58/digits and the selectors constant:
// none of them needs a JSON escape.
bool AssetIssueBody(const char* asset_id, char* out, size_t capacity, size_t* length) {
  const int n = snprintf(out, capacity, "{\"value\":\"%s\",\"visible\":true}", asset_id);
  if (n <= 0 || static_cast<size_t>(n) >= capacity) return false;
  *length = static_cast<size_t>(n);
  return true;
}

bool ViewBody(const char* owner, const char* contract, const char* selector, const char* parameter, char* out,
              size_t capacity, size_t* length) {
  const int n = snprintf(out, capacity,
                         "{\"owner_address\":\"%s\",\"contract_address\":\"%s\",\"function_selector\":\"%s\","
                         "\"parameter\":\"%s\",\"visible\":true}",
                         owner, contract, selector, parameter);
  if (n <= 0 || static_cast<size_t>(n) >= capacity) return false;
  *length = static_cast<size_t>(n);
  return true;
}

Value* Metadata(Arena& a, const Snapshot* s) {
  if (!s) return tj::Null(a);
  Value* o = tj::Obj(a);
  if (!o || !tj::Set(a, o, "symbol", or_null(a, s->symbol)) || !tj::Set(a, o, "name", or_null(a, s->name)) ||
      !tj::Set(a, o, "decimals", s->has_decimals ? tj::Num(a, s->decimals) : tj::Null(a)) ||
      !tj::Set(a, o, "verified", s->verified < 0 ? tj::Null(a) : tj::Bool(a, s->verified == 1)) ||
      // Holder counts have their own capability/provenance path.
      !tj::Set(a, o, "holderCount", tj::Null(a)) || !tj::Set(a, o, "logoMediaKey", tj::Null(a)))
    return nullptr;
  return o;
}

Value* Insight(Arena& a, const Snapshot* s) {
  Value* sections = tj::Arr(a);
  if (!sections || !s) return sections;
  const char* standard = s->trc10 ? "TRC-10" : "TRC-20";
  Value* supply = nullptr;
  if (s->total_supply_raw && s->has_decimals) {
    supply = scale_digits(a, s->total_supply_raw, s->decimals);
    if (!supply) return nullptr;
    if (supply->type == tj::Type::kNull) supply = nullptr;
  }
  Value* supply_rows = tj::Arr(a);
  if (!supply_rows) return nullptr;
  if (supply && !tj::Push(a, supply_rows, row(a, "Total supply", supply))) return nullptr;
  if (s->has_decimals && !tj::Push(a, supply_rows, row(a, "Decimals", number_text(a, s->decimals)))) return nullptr;
  Value* activity = tj::Arr(a);
  if (!activity) return nullptr;
  if (s->has_holders && !tj::Push(a, activity, row(a, "Holders", number_text(a, s->holders)))) return nullptr;
  if (s->has_transfers && !tj::Push(a, activity, row(a, "Transfers", number_text(a, s->transfers))))
    return nullptr;
  if (s->issuer && !tj::Push(a, activity, row(a, "Issuer", s->issuer))) return nullptr;
  if (s->verified >= 0) {
    Value* verified = row(a, "Verified by TronScan", tj::Str(a, s->verified ? "Yes" : "No"));
    if (!verified || !tj::Set(a, verified, "sentiment", tj::Str(a, s->verified ? "good" : "caution")) ||
        !tj::Push(a, activity, verified))
      return nullptr;
  }
  if (supply_rows->count) {
    Value* summary = tj::Str(a, s->trc10 ? "TRC-10 ledger asset" : "TRC-20 contract state");
    if (!tj::Push(a, sections, section(a, "Supply", summary, supply_rows))) return nullptr;
  }
  if (activity->count) {
    char text[96];
    if (s->has_holders) {
      char count[40];
      chains::JsNumberText(s->holders, count, sizeof(count));
      snprintf(text, sizeof(text), "%s accounts hold this token", count);
    } else {
      snprintf(text, sizeof(text), "%s token", standard);
    }
    if (!tj::Push(a, sections, section(a, "Chain activity", tj::Str(a, text), activity))) return nullptr;
  }
  return sections;
}

Value* Holders(Arena& a, const Snapshot* s) {
  if (!s || !s->has_holders) return tj::Null(a);
  Value* o = tj::Obj(a);
  if (!o || !tj::Set(a, o, "count", tj::Num(a, s->holders)) || !tj::Set(a, o, "atLeast", tj::Bool(a, false)) ||
      !tj::Set(a, o, "source", tj::Str(a, "tronscan")))
    return nullptr;
  return o;
}

Value* ToJson(Arena& a, const Snapshot* s) {
  if (!s) return tj::Null(a);
  Value* o = tj::Obj(a);
  if (!o || !tj::Set(a, o, "standard", tj::Str(a, s->trc10 ? "TRC-10" : "TRC-20")) ||
      !tj::Set(a, o, "symbol", or_null(a, s->symbol)) || !tj::Set(a, o, "name", or_null(a, s->name)) ||
      !tj::Set(a, o, "decimals", s->has_decimals ? tj::Num(a, s->decimals) : tj::Null(a)) ||
      !tj::Set(a, o, "totalSupplyRaw", or_null(a, s->total_supply_raw)) ||
      !tj::Set(a, o, "holders", s->has_holders ? tj::Num(a, s->holders) : tj::Null(a)) ||
      !tj::Set(a, o, "transfers", s->has_transfers ? tj::Num(a, s->transfers) : tj::Null(a)) ||
      !tj::Set(a, o, "issuer", or_null(a, s->issuer)) ||
      !tj::Set(a, o, "verified", s->verified < 0 ? tj::Null(a) : tj::Bool(a, s->verified == 1)) ||
      !tj::Set(a, o, "source", tj::Str(a, s->tronscan ? "tronscan" : "trongrid")))
    return nullptr;
  return o;
}

bool FromJson(Arena& a, const Value* json, Snapshot* out, bool* present) {
  (void)a;
  *out = Snapshot{};
  *present = false;
  if (json && json->type == tj::Type::kNull) return true;
  if (!tj::IsObject(json)) return false;
  auto text = [&](const char* key, Value** field) {
    Value* v = tj::Get(json, key);
    if (tj::IsString(v)) { *field = v; return true; }
    *field = nullptr;
    return v && v->type == tj::Type::kNull;
  };
  auto number = [&](const char* key, bool* has, double* field) {
    const Value* v = tj::Get(json, key);
    *has = tj::IsNumber(v);
    if (*has) *field = v->number;
    return *has || (v && v->type == tj::Type::kNull);
  };
  const Value* verified = tj::Get(json, "verified");
  if (!text("symbol", &out->symbol) || !text("name", &out->name) ||
      !number("decimals", &out->has_decimals, &out->decimals) ||
      !text("totalSupplyRaw", &out->total_supply_raw) ||
      !number("holders", &out->has_holders, &out->holders) ||
      !number("transfers", &out->has_transfers, &out->transfers) || !text("issuer", &out->issuer) ||
      !(tj::IsBool(verified) || (verified && verified->type == tj::Type::kNull)))
    return false;
  out->verified = tj::IsBool(verified) ? (verified->type == tj::Type::kTrue ? 1 : 0) : -1;
  out->trc10 = tj::StringEquals(tj::Get(json, "standard"), "TRC-10");
  out->tronscan = tj::StringEquals(tj::Get(json, "source"), "tronscan");
  *present = true;
  return true;
}

}  // namespace fortis::chains::tron_token
