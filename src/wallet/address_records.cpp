#include <algorithm>
#include "address_records.h"

#include "token_text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fortis::wallet::address {
namespace {

using chains::BigInt;
namespace tj = fortis::codec::tj;

Value* get(const Value* row, const char* key) { return tj::Get(row, key); }

bool is_null(const Value* v) { return !v || v->type == tj::Type::kNull; }

Value* copy_or_null(Arena& arena, const Value* v) {
  return is_null(v) ? tj::Null(arena) : tj::Clone(arena, v);
}

// `a ?? b` over stored values (undefined and null are nullish).
const Value* coalesce(const Value* a, const Value* b) { return is_null(a) ? b : a; }

bool same_value(const Value* a, const Value* b) {
  // `old.amountRaw !== incoming.amountRaw` over strings / null.
  if (is_null(a) || is_null(b)) return is_null(a) && is_null(b);
  if (a->type != b->type) return false;
  if (a->type == tj::Type::kString) return a->length == b->length && memcmp(a->text, b->text, a->length) == 0;
  if (a->type == tj::Type::kNumber) return a->number == b->number;
  return a == b;
}

// Date.prototype.toISOString over the whole Date range (|ms| <= 8.64e15):
// years 0..9999 as four digits, others as a signed six-digit year
// ("1969-12-31T23:59:59.000Z", "+275760-09-13T00:00:00.000Z"). False past
// the range (an Invalid Date throws there).
bool js_iso(int64_t ms, char output[40]) {
  if (ms > 8640000000000000LL || ms < -8640000000000000LL) return false;
  int64_t days = ms / 86400000;
  int64_t rem = ms % 86400000;
  if (rem < 0) { rem += 86400000; --days; }
  const int hour = static_cast<int>(rem / 3600000);
  const int minute = static_cast<int>(rem / 60000 % 60);
  const int second = static_cast<int>(rem / 1000 % 60);
  const int milli = static_cast<int>(rem % 1000);
  // civil_from_days (Howard Hinnant), valid for negative day counts.
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t doe = days - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t year = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  if (month <= 2) ++year;
  char formatted[128];
  int written = 0;
  if (year >= 0 && year <= 9999)
    written = snprintf(formatted, sizeof(formatted), "%04lld-%02d-%02dT%02d:%02d:%02d.%03dZ", static_cast<long long>(year), month, day, hour,
             minute, second, milli);
  else
    written = snprintf(formatted, sizeof(formatted), "%c%06lld-%02d-%02dT%02d:%02d:%02d.%03dZ", year < 0 ? '-' : '+',
             static_cast<long long>(year < 0 ? -year : year), month, day, hour, minute, second, milli);
  if (written < 0 || written >= 40) return false;
  memcpy(output, formatted, static_cast<size_t>(written) + 1);
  return true;
}

Value* iso_or_null(Arena& arena, const Value* v) {
  if (!tj::IsNumber(v)) return tj::Null(arena);
  char text[40];
  return js_iso(static_cast<int64_t>(v->number), text) ? tj::Str(arena, text) : tj::Null(arena);
}

Value* amount(Arena& arena, const Value* raw, const Value* decimals) {
  if (is_null(raw) || is_null(decimals)) return tj::Null(arena);
  Value* out = tj::Obj(arena);
  if (!out || !tj::Set(arena, out, "raw", tj::Clone(arena, raw)) ||
      !tj::Set(arena, out, "decimals", tj::Clone(arena, decimals)))
    return nullptr;
  return out;
}

// Number(text) of a stored decimal string.
double stored_number(const Value* v) {
  if (!tj::IsString(v)) return NAN;
  return fortis::compat::token_text::ToNumber(v->text, v->length);
}

constexpr double kPow10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10,
                             1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21,
                             1e22, 1e23, 1e24, 1e25, 1e26, 1e27, 1e28, 1e29, 1e30, 1e31, 1e32,
                             1e33, 1e34, 1e35, 1e36, 1e37, 1e38, 1e39, 1e40};

// `10 ** decimals` (V8 returns the exact literal for these exponents).
double pow10(double decimals) {
  if (decimals >= 0 && decimals <= 40 && trunc(decimals) == decimals) return kPow10[static_cast<int>(decimals)];
  return pow(10, decimals);
}

// `Number(amountRaw) / 10 ** decimals`.
double units_of(const Value* raw, const Value* decimals) {
  const double number = tj::IsString(raw) ? fortis::compat::token_text::ToNumber(raw->text, raw->length)
                                          : chains::JsNumber(raw);
  const double scale = pow10(chains::JsNumber(decimals));
  return number / scale;
}

Value* fixed24(Arena& arena, double value) {
  char text[160];
  JsToFixed(value, 24, text, sizeof(text));
  return tj::Str(arena, text);
}


}  // namespace

void JsToFixed(double value, int digits, char* output, size_t capacity) {
  if (!capacity) return;
  if (!isfinite(value) || fabs(value) >= 1e21) {
    chains::JsNumberText(value, output, capacity);
    return;
  }
  if (digits < 0) digits = 0;
  if (digits > 100) digits = 100;
  const bool negative = value < 0;
  const double magnitude = fabs(value);
  // magnitude * 10^digits = m * 2^e * 10^digits, rounded to the nearest
  // integer n with ties to the larger n.
  int exponent = 0;
  const double fraction = frexp(magnitude, &exponent);
  const uint64_t m = static_cast<uint64_t>(ldexp(fraction, 53));
  const int e = magnitude == 0 ? 0 : exponent - 53;
  BigInt n = BigInt::FromU64(magnitude == 0 ? 0 : m);
  for (int i = 0; i < digits; ++i) n.MulSmall(10);
  if (e >= 0) {
    for (int i = 0; i < e; ++i) n.MulSmall(2);
  } else if (-e > 500) {
    n = BigInt();  // below 2^-447: always rounds to zero
  } else {
    BigInt divisor = BigInt::FromU64(1);
    for (int i = 0; i < -e; ++i) divisor.MulSmall(2);
    BigInt remainder;
    n.Div(divisor, &remainder);
    BigInt twice = remainder;
    twice.MulSmall(2);
    if (twice.Compare(divisor) >= 0) n.AddSmall(1);
  }
  char digits_text[BigInt::kLimbs * 10 + 4];
  n.ToDecimal(digits_text, sizeof(digits_text));
  size_t length = strlen(digits_text);
  char padded[BigInt::kLimbs * 10 + 120];
  size_t used = 0;
  for (size_t i = length; i < static_cast<size_t>(digits) + 1; ++i) padded[used++] = '0';
  std::copy_n(digits_text, length, padded + used);
  used += length;
  size_t o = 0;
  // (-x).toFixed() is "-" + x.toFixed() only when x < 0 (never for -0).
  if (negative && o + 1 < capacity) output[o++] = '-';
  const size_t whole = used - static_cast<size_t>(digits);
  for (size_t i = 0; i < whole && o + 1 < capacity; ++i) output[o++] = padded[i];
  if (digits > 0) {
    if (o + 1 < capacity) output[o++] = '.';
    for (size_t i = whole; i < used && o + 1 < capacity; ++i) output[o++] = padded[i];
  }
  output[o] = '\0';
}

const char* RowString(const Value* row, const char* key) {
  const Value* v = get(row, key);
  return tj::IsString(v) ? v->text : nullptr;
}

bool RowNumber(const Value* row, const char* key, double* out) {
  const Value* v = get(row, key);
  if (!tj::IsNumber(v)) return false;
  *out = v->number;
  return true;
}

int64_t RowMs(const Value* row, const char* key) {
  const Value* v = get(row, key);
  return tj::IsNumber(v) ? static_cast<int64_t>(v->number) : 0;
}

Value* TransferAppJson(Arena& arena, const Value* row, const Network& network, bool has_tip, double tip) {
  Value* out = tj::Obj(arena);
  if (!out) return nullptr;
  const Value* status = get(row, "status");
  const Value* block = get(row, "blockNumber");
  double confirmations = 0;
  if (chains::Equals(status, "confirmed")) {
    confirmations = tj::IsNumber(block) && has_tip && tip >= block->number ? tip - block->number + 1 : 1;
  }
  const Value* metadata = get(row, "metadata");
  const Value* label = tj::IsObject(metadata) ? tj::Get(metadata, "activityLabel") : nullptr;
  const Value* fiat = get(row, "fiatAtTime");
  bool ok = tj::Set(arena, out, "id", copy_or_null(arena, get(row, "recordId"))) &&
            tj::Set(arena, out, "type", copy_or_null(arena, get(row, "txType"))) &&
            tj::Set(arena, out, "status", copy_or_null(arena, status)) &&
            tj::Set(arena, out, "direction", copy_or_null(arena, get(row, "direction"))) &&
            tj::Set(arena, out, "timestamp", iso_or_null(arena, get(row, "ts"))) &&
            tj::Set(arena, out, "chainId", tj::Str(arena, network.id)) &&
            tj::Set(arena, out, "family", tj::Str(arena, chains::FamilyName(network.family))) &&
            tj::Set(arena, out, "hash", copy_or_null(arena, get(row, "hash"))) &&
            tj::Set(arena, out, "blockNumber", copy_or_null(arena, block)) &&
            tj::Set(arena, out, "nonce", tj::Null(arena)) &&
            tj::Set(arena, out, "confirmations", tj::Num(arena, confirmations)) &&
            tj::Set(arena, out, "fromAddress", copy_or_null(arena, get(row, "fromAddress"))) &&
            tj::Set(arena, out, "toAddress", copy_or_null(arena, get(row, "toAddress"))) &&
            tj::Set(arena, out, "asset", copy_or_null(arena, get(row, "asset"))) &&
            tj::Set(arena, out, "amount", amount(arena, get(row, "amountRaw"), get(row, "amountDecimals"))) &&
            tj::Set(arena, out, "toAsset", copy_or_null(arena, get(row, "toAsset"))) &&
            tj::Set(arena, out, "toAmount", amount(arena, get(row, "toAmountRaw"), get(row, "toAmountDecimals"))) &&
            tj::Set(arena, out, "destinationChainId", copy_or_null(arena, get(row, "destChainId"))) &&
            tj::Set(arena, out, "destinationHash", copy_or_null(arena, get(row, "destHash"))) &&
            tj::Set(arena, out, "fee", amount(arena, get(row, "feeRaw"), get(row, "feeDecimals"))) &&
            tj::Set(arena, out, "feeSymbol", copy_or_null(arena, get(row, "feeSymbol"))) &&
            tj::Set(arena, out, "fiatValue", is_null(fiat) ? tj::Null(arena) : tj::Num(arena, stored_number(fiat))) &&
            tj::Set(arena, out, "fiatCurrency", is_null(fiat) ? tj::Null(arena)
                                                              : copy_or_null(arena, get(row, "fiatCurrency"))) &&
            tj::Set(arena, out, "fiatValueSource", copy_or_null(arena, get(row, "fiatSourceInterval"))) &&
            tj::Set(arena, out, "fiatValueSourceId", copy_or_null(arena, get(row, "fiatSourceId"))) &&
            tj::Set(arena, out, "fiatValueAsOf", iso_or_null(arena, get(row, "fiatSourceTs"))) &&
            tj::Set(arena, out, "memo", copy_or_null(arena, get(row, "memo")));
  if (ok && tj::IsString(label)) ok = tj::Set(arena, out, "note", tj::Clone(arena, label));
  ok = ok && tj::Set(arena, out, "error", copy_or_null(arena, get(row, "error"))) &&
       tj::Set(arena, out, "metadata", is_null(metadata) ? tj::Obj(arena) : tj::Clone(arena, metadata));
  return ok ? out : nullptr;
}

namespace {

const char* operation_kind(const char* type) {
  if (!type) return "unknown";
  if (!strcmp(type, "send") || !strcmp(type, "receive")) return "transfer";
  if (!strcmp(type, "approve") || !strcmp(type, "associate") || !strcmp(type, "contractCall")) return "unknown";
  return type;
}

const char* string_meta(Arena& arena, const Value* metadata, const char* key) {
  const Value* v = tj::IsObject(metadata) ? tj::Get(metadata, key) : nullptr;
  if (!tj::IsString(v)) return nullptr;
  size_t begin = 0, end = 0;
  fortis::compat::token_text::Trim(v->text, v->length, &begin, &end);
  if (begin == end) return nullptr;
  return arena.CopyText(v->text + begin, end - begin);
}

struct LegInput {
  double index;
  const char* role;
  const Value* token_id;
  const Value* asset;
  const Value* amount_raw;
  const Value* amount_decimals;
  const char* direction;
};

Value* leg_json(Arena& arena, const Value* row, const LegInput& in) {
  const Value* fiat = get(row, "fiatAtTime");
  const bool has_total = !is_null(fiat);
  const double total = has_total ? stored_number(fiat) : 0;
  // unitPrice(total, amountRaw, decimals) then Number(toFixed(24)).
  Value* unit = tj::Null(arena);
  if (has_total && total >= 0 && !is_null(in.amount_raw) && !is_null(in.amount_decimals)) {
    const double units = units_of(in.amount_raw, in.amount_decimals);
    if (isfinite(units) && units != 0) {
      const double price = total / fabs(units);
      if (isfinite(price) && price >= 0) {
        char text[160];
        JsToFixed(price, 24, text, sizeof(text));
        unit = tj::Num(arena, fortis::compat::token_text::ToNumber(text, strlen(text)));
      }
    }
  }
  Value* total_value = tj::Null(arena);
  if (has_total) {
    char text[160];
    JsToFixed(total, 24, text, sizeof(text));
    total_value = tj::Num(arena, fortis::compat::token_text::ToNumber(text, strlen(text)));
  }
  const Value* interval = get(row, "fiatSourceInterval");
  Value* source = !has_total ? tj::Null(arena)
                             : is_null(interval) ? tj::Str(arena, "unknownEstimate") : tj::Clone(arena, interval);
  const Value* source_ts = get(row, "fiatSourceTs");
  Value* as_of = !is_null(source_ts) ? iso_or_null(arena, source_ts)
                                     : has_total ? iso_or_null(arena, get(row, "ts")) : tj::Null(arena);
  Value* out = tj::Obj(arena);
  if (!out || !tj::Set(arena, out, "index", tj::Num(arena, in.index)) ||
      !tj::Set(arena, out, "role", tj::Str(arena, in.role)) ||
      !tj::Set(arena, out, "tokenId", copy_or_null(arena, in.token_id)) ||
      !tj::Set(arena, out, "asset", copy_or_null(arena, in.asset)) ||
      !tj::Set(arena, out, "amount", amount(arena, in.amount_raw, in.amount_decimals)) ||
      !tj::Set(arena, out, "direction", tj::Str(arena, in.direction)) ||
      !tj::Set(arena, out, "unitPriceUsd", unit) || !tj::Set(arena, out, "totalUsd", total_value) ||
      !tj::Set(arena, out, "valuationSource", source) ||
      !tj::Set(arena, out, "valuationSourceId", copy_or_null(arena, get(row, "fiatSourceId"))) ||
      !tj::Set(arena, out, "valuationAsOf", as_of))
    return nullptr;
  return out;
}

Value* fee_leg_json(Arena& arena, const Value* row) {
  Value* out = tj::Obj(arena);
  if (!out || !tj::Set(arena, out, "index", tj::Num(arena, 2)) ||
      !tj::Set(arena, out, "role", tj::Str(arena, "fee")) ||
      !tj::Set(arena, out, "tokenId", tj::Str(arena, "native")) ||
      !tj::Set(arena, out, "asset", tj::Null(arena)) ||
      !tj::Set(arena, out, "amount", amount(arena, get(row, "feeRaw"), get(row, "feeDecimals"))) ||
      !tj::Set(arena, out, "direction", tj::Str(arena, "out")) ||
      !tj::Set(arena, out, "unitPriceUsd", tj::Null(arena)) || !tj::Set(arena, out, "totalUsd", tj::Null(arena)) ||
      !tj::Set(arena, out, "valuationSource", tj::Null(arena)) ||
      !tj::Set(arena, out, "valuationSourceId", tj::Null(arena)) ||
      !tj::Set(arena, out, "valuationAsOf", tj::Null(arena)))
    return nullptr;
  return out;
}

}  // namespace

Value* OperationAppJson(Arena& arena, const Value* row) {
  const char* type = RowString(row, "txType");
  const char* kind = operation_kind(type);
  const char* direction = RowString(row, "direction");
  const bool semantic = type && (!strcmp(type, "swap") || !strcmp(type, "bridge"));
  const char* primary_direction = semantic ? "out"
                                  : direction && !strcmp(direction, "incoming") ? "in"
                                  : direction && !strcmp(direction, "outgoing") ? "out"
                                                                                : "neutral";
  const bool fee_type = type && !strcmp(type, "fee");
  const char* primary_role = fee_type ? "fee" : !strcmp(primary_direction, "in") ? "output" : "input";
  Value* legs = tj::Arr(arena);
  if (!legs) return nullptr;
  bool has_output = !strcmp(primary_role, "output");
  const bool kind_semantic = !strcmp(kind, "swap") || !strcmp(kind, "bridge");
  if (!tj::Push(arena, legs, leg_json(arena, row, {0, primary_role, get(row, "tokenId"), get(row, "asset"),
                                                   get(row, "amountRaw"), get(row, "amountDecimals"),
                                                   primary_direction})))
    return nullptr;
  const Value* to_token = get(row, "toTokenId");
  const bool to_token_truthy = tj::IsString(to_token) && to_token->length > 0;
  if (kind_semantic && to_token_truthy && !is_null(get(row, "toAmountRaw"))) {
    has_output = true;
    if (!tj::Push(arena, legs, leg_json(arena, row, {static_cast<double>(legs->count), "output", to_token,
                                                     get(row, "toAsset"), get(row, "toAmountRaw"),
                                                     get(row, "toAmountDecimals"), "in"})))
      return nullptr;
  }
  if (!fee_type && !is_null(get(row, "feeRaw"))) {
    if (!tj::Push(arena, legs, fee_leg_json(arena, row))) return nullptr;
  }
  const Value* metadata = get(row, "metadata");
  const char* venue = string_meta(arena, metadata, "venue");
  if (!venue) venue = string_meta(arena, metadata, "protocol");
  const char* confidence = kind_semantic && has_output ? "source" : !strcmp(kind, "unknown") ? "estimated" : "derived";
  const Value* status = get(row, "status");
  Value* out = tj::Obj(arena);
  if (!out || !tj::Set(arena, out, "id", copy_or_null(arena, get(row, "recordId"))) ||
      !tj::Set(arena, out, "recordId", copy_or_null(arena, get(row, "recordId"))) ||
      !tj::Set(arena, out, "kind", tj::Str(arena, kind)) ||
      !tj::Set(arena, out, "triggeredAt", iso_or_null(arena, get(row, "ts"))) ||
      !tj::Set(arena, out, "settledAt", chains::Equals(status, "confirmed") ? iso_or_null(arena, get(row, "ts"))
                                                                          : tj::Null(arena)) ||
      !tj::Set(arena, out, "status", copy_or_null(arena, status)) ||
      !tj::Set(arena, out, "hash", copy_or_null(arena, get(row, "hash"))) ||
      !tj::Set(arena, out, "venue", venue ? tj::Str(arena, venue) : tj::Null(arena)) ||
      !tj::Set(arena, out, "confidence", tj::Str(arena, confidence)) || !tj::Set(arena, out, "legs", legs))
    return nullptr;
  return out;
}

Value* InitialRow(Arena& arena, const char* account_id, const Value* record, int64_t now_ms) {
  Value* row = tj::Clone(arena, record);
  if (!tj::IsObject(row)) return nullptr;
  const Value* fiat = get(record, "fiatAtTime");
  bool ok = tj::Set(arena, row, "accountId", tj::Str(arena, account_id)) &&
            tj::Set(arena, row, "cursor", tj::Str(arena, "")) &&
            tj::Set(arena, row, "toTokenId", copy_or_null(arena, get(record, "toTokenId"))) &&
            tj::Set(arena, row, "toAsset", copy_or_null(arena, get(record, "toAsset"))) &&
            tj::Set(arena, row, "toAmountRaw", copy_or_null(arena, get(record, "toAmountRaw"))) &&
            tj::Set(arena, row, "toAmountDecimals", copy_or_null(arena, get(record, "toAmountDecimals"))) &&
            tj::Set(arena, row, "destChainId", copy_or_null(arena, get(record, "destChainId"))) &&
            tj::Set(arena, row, "destHash", copy_or_null(arena, get(record, "destHash"))) &&
            tj::Set(arena, row, "fiatAtTime", tj::IsNumber(fiat) ? fixed24(arena, fiat->number) : tj::Null(arena)) &&
            tj::Set(arena, row, "fiatCurrency", tj::Str(arena, "usd")) &&
            tj::Set(arena, row, "fiatSourceInterval", copy_or_null(arena, get(record, "fiatSourceInterval"))) &&
            tj::Set(arena, row, "fiatSourceId", copy_or_null(arena, get(record, "fiatSourceId"))) &&
            tj::Set(arena, row, "fiatSourceTs", copy_or_null(arena, get(record, "fiatSourceTs"))) &&
            tj::Set(arena, row, "fiatAttemptedAt", tj::Null(arena)) &&
            tj::Set(arena, row, "createdAt", tj::Num(arena, static_cast<double>(now_ms))) &&
            tj::Set(arena, row, "updatedAt", tj::Num(arena, static_cast<double>(now_ms)));
  return ok ? row : nullptr;
}

Value* MergeRow(Arena& arena, const Value* old, const Value* incoming, bool restatable, int64_t now_ms) {
  Value* row = tj::Clone(arena, old);
  if (!tj::IsObject(row)) return nullptr;
  bool ok = true;
  if (restatable) {
    static const char* const kRestated[] = {"txType", "direction", "fromAddress", "toAddress", "counterparty",
                                            "amountRaw", "amountDecimals", "feeRaw", "feeDecimals", "feeSymbol"};
    for (const char* key : kRestated) {
      const Value* v = get(incoming, key);
      ok = ok && tj::Set(arena, row, key, v ? tj::Clone(arena, v) : tj::Null(arena));
    }
  }
  auto set_from = [&](const char* key, const Value* v) {
    ok = ok && tj::Set(arena, row, key, v ? tj::Clone(arena, v) : tj::Null(arena));
  };
  set_from("status", get(incoming, "status"));
  set_from("blockNumber", get(incoming, "blockNumber"));
  set_from("error", get(incoming, "error"));
  // metadata: {...old.metadata, ...incoming.metadata} (objects only).
  Value* metadata = tj::Obj(arena);
  if (!metadata) return nullptr;
  const Value* old_meta = get(old, "metadata");
  const Value* new_meta = get(incoming, "metadata");
  const Value* const sources[] = {old_meta, new_meta};
  for (const Value* source : sources) {
    if (!tj::IsObject(source)) continue;
    for (uint32_t i = 0; i < source->count; ++i)
      ok = ok && tj::Set(arena, metadata, source->members[i].key, source->members[i].key_length,
                         tj::Clone(arena, source->members[i].value));
  }
  ok = ok && tj::Set(arena, row, "metadata", metadata);
  static const char* const kCoalesced[] = {"toTokenId", "toAsset", "toAmountRaw", "toAmountDecimals",
                                           "destChainId", "destHash"};
  for (const char* key : kCoalesced) set_from(key, coalesce(get(incoming, key), get(old, key)));
  const bool restated_amount = restatable && !same_value(get(old, "amountRaw"), get(incoming, "amountRaw"));
  set_from("fiatAtTime", restated_amount ? get(incoming, "fiatAtTime")
                                         : coalesce(get(incoming, "fiatAtTime"), get(old, "fiatAtTime")));
  static const char* const kSources[] = {"fiatSourceInterval", "fiatSourceId", "fiatSourceTs"};
  for (const char* key : kSources) set_from(key, coalesce(get(incoming, key), get(old, key)));
  set_from("fiatAttemptedAt", restated_amount ? nullptr : get(old, "fiatAttemptedAt"));
  ok = ok && tj::Set(arena, row, "updatedAt", tj::Num(arena, static_cast<double>(now_ms)));
  return ok ? row : nullptr;
}

bool EncodeDisplayCursor(int64_t ts_ms, const char* record_id, char* output, size_t capacity) {
  uint8_t arena_bytes[4096];
  Arena arena{arena_bytes, sizeof(arena_bytes), 0, true};
  Value* object = tj::Obj(arena);
  char iso[40];
  if (!js_iso(ts_ms, iso)) return false;
  if (!object || !tj::Set(arena, object, "ts", tj::Str(arena, iso)) ||
      !tj::Set(arena, object, "id", tj::Str(arena, record_id)))
    return false;
  uint8_t json[2048];
  chains::OutBuffer out{json, sizeof(json), 0, true};
  if (!tj::Stringify(object, out) || !out.ok) return false;
  return chains::Base64Encode(json, out.used, true, false, output, capacity);
}

bool DecodeDisplayCursor(Arena& arena, const char* raw, size_t length, int64_t* ts_ms, const char** id,
                         size_t* id_length) {
  uint8_t* bytes = static_cast<uint8_t*>(arena.Alloc(length + 4, 1));
  if (!bytes) return false;
  size_t size = 0;
  if (!chains::Base64Decode(raw, length, bytes, length + 4, &size)) return false;
  // JSON.parse (unlike Response.json) rejects a leading BOM.
  if (size >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) return false;
  Value* value = tj::Parse(arena, bytes, size);
  if (!tj::IsObject(value)) return false;
  const Value* ts = tj::Get(value, "ts");
  const Value* record = tj::Get(value, "id");
  double parsed = 0;
  if (!tj::IsString(ts) || !chains::ParseTime(ts->text, ts->length, &parsed)) return false;
  if (!tj::IsString(record) || record->length == 0) return false;
  *ts_ms = static_cast<int64_t>(parsed);
  *id = record->text;
  *id_length = record->length;  // an escaped \u0000 is part of the id
  return true;
}

bool TransferMatches(const Value* row, const TransferQuery& query) {
  double ts = 0;
  RowNumber(row, "ts", &ts);
  if (query.has_from && ts < query.from_sec * 1000) return false;
  if (query.has_to && ts > query.to_sec * 1000) return false;
  const char* token = RowString(row, "tokenId");
  const char* to_token = RowString(row, "toTokenId");
  const char* direction = RowString(row, "direction");
  const char* type = RowString(row, "txType");
  auto eq = [](const char* a, const char* b) { return a && b && strcmp(a, b) == 0; };
  if (query.token_id && *query.token_id && !eq(token, query.token_id) && !eq(to_token, query.token_id))
    return false;
  if (!query.flow) return true;
  if (!query.token_id || !*query.token_id) {
    const char* wanted = !strcmp(query.flow, "in") ? "incoming" : !strcmp(query.flow, "out") ? "outgoing" : "selfTransfer";
    return eq(direction, wanted);
  }
  auto one_of = [&](const char* const* list, size_t n) {
    for (size_t i = 0; i < n; ++i) if (eq(type, list[i])) return true;
    return false;
  };
  if (!strcmp(query.flow, "in")) {
    static const char* const kIn[] = {"receive", "mint", "reward", "unstake"};
    return eq(to_token, query.token_id) ||
           (eq(token, query.token_id) && (one_of(kIn, 4) || (eq(type, "unknown") && eq(direction, "incoming"))));
  }
  if (!strcmp(query.flow, "out")) {
    static const char* const kOut[] = {"send", "swap", "bridge", "burn", "stake", "fee"};
    return eq(token, query.token_id) &&
           (one_of(kOut, 6) || (eq(type, "unknown") && eq(direction, "outgoing")));
  }
  static const char* const kNeutral[] = {"send", "receive"};
  return eq(token, query.token_id) && eq(direction, "selfTransfer") && one_of(kNeutral, 2);
}

const char* PriceIdForTransfer(Arena& arena, const Network& network, const char* token_id, const char* canonical_id) {
  if (canonical_id) {
    size_t begin = 0, end = 0;
    fortis::compat::token_text::Trim(canonical_id, strlen(canonical_id), &begin, &end);
    if (end > begin) {
      Value* lowered = fortis::compat::token_text::ToLowerCase(arena, canonical_id + begin, end - begin);
      return lowered ? lowered->text : nullptr;
    }
  }
  if (!strcmp(token_id, "native")) return network.native_coingecko_id;
  if (chains::StartsWith(token_id, "erc20:") || chains::StartsWith(token_id, "trc20:") ||
      chains::StartsWith(token_id, "trc10:")) {
    Value* id = chains::Format(arena, "%s:%s", network.id, token_id);
    return id ? id->text : nullptr;
  }
  if (chains::StartsWith(token_id, "spl:") || chains::StartsWith(token_id, "asa:")) return arena.CopyText(token_id, strlen(token_id));
  return nullptr;
}

}  // namespace fortis::wallet::address
