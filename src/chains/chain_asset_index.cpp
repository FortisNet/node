#include "chain_asset_index.h"

#include "chain_core.h"
#include "token_ref.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::chains::asset_index {
namespace {

using tj::Type;

constexpr double kMaxSafeInteger = 9007199254740991.0;

bool nullish(const Value* v) { return !v || v->type == Type::kNull; }

// `!!value` for a JSON value (nullptr is undefined).
bool truthy(const Value* v) {
  if (!v) return false;
  switch (v->type) {
    case Type::kNull: case Type::kFalse: return false;
    case Type::kTrue: case Type::kArray: case Type::kObject: return true;
    case Type::kNumber: return v->number != 0 && !isnan(v->number);
    case Type::kString: return v->length != 0;
  }
  return false;
}

// `value.key` of a non-nullish value: only objects have the members these
// bodies are read for.
const Value* member(const Value* v, const char* key) { return tj::Get(v, key); }

// `value[0]`: an array's first element, an object's "0" member, a string's
// first character (only its truthiness is ever read: every member read of a
// string is undefined) or undefined.
const Value* first(const Value* v) {
  if (!v) return nullptr;
  if (v->type == Type::kArray) return v->count ? v->items[0] : nullptr;
  if (v->type == Type::kObject) return tj::Get(v, "0");
  if (v->type == Type::kString) return v->length ? v : nullptr;
  return nullptr;
}

// Number.isSafeInteger(value) && value >= 0.
bool non_negative_safe(const Value* v, double* out) {
  if (!tj::IsNumber(v) || !isfinite(v->number) || v->number < 0 || v->number > kMaxSafeInteger ||
      floor(v->number) != v->number)
    return false;
  *out = v->number;
  return true;
}

// `${value}` of a JSON value, as the template literal coerces it.
void append_template(tj::OutBuffer& out, const Value* v, bool in_array) {
  if (!v) { if (!in_array) out.Text("undefined"); return; }
  switch (v->type) {
    case Type::kNull: if (!in_array) out.Text("null"); return;
    case Type::kTrue: out.Text("true"); return;
    case Type::kFalse: out.Text("false"); return;
    case Type::kNumber: {
      char text[40];
      chains::JsNumberText(v->number, text, sizeof(text));
      out.Text(text);
      return;
    }
    case Type::kString: out.Append(v->text, v->length); return;
    case Type::kArray:
      // Array.prototype.join: null elements are empty.
      for (uint32_t i = 0; i < v->count; ++i) {
        if (i) out.Byte(',');
        if (!nullish(v->items[i])) append_template(out, v->items[i], true);
      }
      return;
    case Type::kObject: out.Text("[object Object]"); return;
  }
}

size_t template_bytes(const Value* v) {
  if (!v) return 16;
  switch (v->type) {
    case Type::kString: return v->length + 1;
    case Type::kArray: {
      size_t total = v->count + 1;
      for (uint32_t i = 0; i < v->count; ++i) total += template_bytes(v->items[i]);
      return total;
    }
    default: return 40;
  }
}

// `${value}${suffix}` as an arena string.
Value* template_text(Arena& a, const Value* v, const char* suffix) {
  const size_t capacity = template_bytes(v) + strlen(suffix) + 16;
  uint8_t* buffer = static_cast<uint8_t*>(a.Alloc(capacity, 1));
  if (!buffer) return nullptr;
  tj::OutBuffer out{buffer, capacity, 0, true};
  append_template(out, v, false);
  out.Text(suffix);
  if (!out.ok) return nullptr;
  return tj::Str(a, reinterpret_cast<const char*>(buffer), out.used);
}

Value* count_text(Arena& a, double count, const char* suffix = "") {
  char text[40];
  chains::JsNumberText(count, text, sizeof(text));
  char joined[96];
  snprintf(joined, sizeof(joined), "%s%s", text, suffix);
  return tj::Str(a, joined);
}

// encodeURIComponent over ASCII/UTF-8 bytes.
bool append_component(const char* text, char* out, size_t capacity, size_t* used) {
  static const char kHex[] = "0123456789ABCDEF";
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
    const unsigned char c = *p;
    const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' ||
                      c == '(' || c == ')';
    if (*used + (keep ? 1 : 3) >= capacity) return false;
    if (keep) {
      out[(*used)++] = static_cast<char>(c);
    } else {
      out[(*used)++] = '%';
      out[(*used)++] = kHex[c >> 4];
      out[(*used)++] = kHex[c & 15];
    }
  }
  out[*used] = '\0';
  return true;
}

bool append_text(const char* text, char* out, size_t capacity, size_t* used) {
  const size_t length = strlen(text);
  if (*used + length >= capacity) return false;
  memcpy(out + *used, text, length + 1);
  *used += length;
  return true;
}

Value* row(Arena& a, const char* label, Value* value, const char* sentiment) {
  Value* o = tj::Obj(a);
  if (!o || !value || !tj::Set(a, o, "label", tj::Str(a, label)) || !tj::Set(a, o, "value", value)) return nullptr;
  if (sentiment && !tj::Set(a, o, "sentiment", tj::Str(a, sentiment))) return nullptr;
  return o;
}

Value* section(Arena& a, const char* title, Value* summary, bool ok, Value* rows) {
  Value* o = tj::Obj(a);
  if (!o || !summary || !rows || !tj::Set(a, o, "title", tj::Str(a, title)) || !tj::Set(a, o, "summary", summary) ||
      !tj::Set(a, o, "ok", tj::Bool(a, ok)) || !tj::Set(a, o, "rows", rows))
    return nullptr;
  return o;
}

Value* single(Arena& a, Value* item) {
  Value* list = tj::Arr(a);
  return list && item && tj::Push(a, list, item) ? list : nullptr;
}

Value* holder_count(Arena& a, bool has, double count, const char* source) {
  if (!has) return tj::Null(a);
  Value* o = tj::Obj(a);
  if (!o || !tj::Set(a, o, "count", tj::Num(a, count)) || !tj::Set(a, o, "atLeast", tj::Bool(a, false)) ||
      !tj::Set(a, o, "source", tj::Str(a, source)))
    return nullptr;
  return o;
}

// page._embedded?.records?.[0]
const Value* horizon_record(const Value* page) {
  const Value* embedded = member(page, "_embedded");
  if (nullish(embedded)) return nullptr;
  const Value* records = member(embedded, "records");
  if (nullish(records)) return nullptr;
  return first(records);
}

// horizonHolderCount(record).
bool horizon_holder_count(const Value* record, double* count) {
  if (!truthy(record)) return false;
  if (non_negative_safe(member(record, "num_accounts"), count)) return true;
  const Value* accounts = member(record, "accounts");
  if (!truthy(accounts) || (!tj::IsObject(accounts) && !tj::IsArray(accounts))) return false;
  // Object.values: object members in property order, or array elements.
  double total = 0;
  const uint32_t n = accounts->count;
  for (uint32_t i = 0; i < n; ++i) {
    const Value* value = tj::IsObject(accounts) ? accounts->members[i].value : accounts->items[i];
    double part = 0;
    if (!non_negative_safe(value, &part)) return false;
    total += part;
    if (total > kMaxSafeInteger) return false;
  }
  *count = total;
  return true;
}

// koiosHolderCount(summary).
bool koios_holder_count(const Value* summary, double* count) {
  if (!truthy(summary)) return false;
  double staked = 0, unstaked = 0;
  if (!non_negative_safe(member(summary, "staked_wallets"), &staked) ||
      !non_negative_safe(member(summary, "unstaked_addresses"), &unstaked))
    return false;
  const double total = staked + unstaked;
  if (total > kMaxSafeInteger) return false;
  *count = total;
  return true;
}

// describeFlags(flags): worst first.
Value* describe_flags(Arena& a, const Value* flags) {
  char text[128];
  size_t used = 0;
  text[0] = '\0';
  const char* notes[] = {
      truthy(member(flags, "auth_clawback_enabled")) ? "clawback enabled" : nullptr,
      truthy(member(flags, "auth_revocable")) ? "can be frozen" : nullptr,
      truthy(member(flags, "auth_required")) ? "approval required" : nullptr,
      truthy(member(flags, "auth_immutable")) ? "flags locked" : nullptr,
  };
  for (const char* note : notes) {
    if (!note) continue;
    if (used && !append_text(" \xC2\xB7 ", text, sizeof(text), &used)) return nullptr;
    if (!append_text(note, text, sizeof(text), &used)) return nullptr;
  }
  return tj::Str(a, used ? text : "No restrictions set");
}


// ---- Algorand / XRPL issuer reads --------------------------------------------

constexpr uint32_t kLsfRequireAuth = 0x00040000u;
constexpr uint32_t kLsfNoFreeze = 0x00200000u;
constexpr uint32_t kLsfGlobalFreeze = 0x00400000u;
constexpr uint32_t kLsfDisallowIncomingTrustline = 0x20000000u;
constexpr uint32_t kLsfAllowTrustlineClawback = 0x80000000u;

bool all_digits(const char* text) {
  if (!text || !*text) return false;
  for (const char* p = text; *p; ++p)
    if (*p < '0' || *p > '9') return false;
  return true;
}

// /^r[1-9A-HJ-NP-Za-km-z]{24,34}$/
bool xrpl_address(const char* address) {
  const size_t length = strlen(address);
  if (length < 25 || length > 35 || address[0] != 'r') return false;
  for (size_t i = 1; i < length; ++i) {
    const char c = address[i];
    const bool ok = (c >= '1' && c <= '9') || (c >= 'A' && c <= 'H') || (c >= 'J' && c <= 'N') ||
                    (c >= 'P' && c <= 'Z') || (c >= 'a' && c <= 'k') || (c >= 'm' && c <= 'z');
    if (!ok) return false;
  }
  return true;
}

// `typeof value === 'string' && value !== ''`.
bool non_empty_string(const Value* v) { return tj::IsString(v) && v->length > 0; }

// `value === 'text'`.
bool string_is(const Value* v, const char* text) {
  return tj::IsString(v) && v->length == strlen(text) && memcmp(v->text, text, v->length) == 0;
}

// Number.isSafeInteger(value).
bool safe_integer(const Value* v) {
  return tj::IsNumber(v) && isfinite(v->number) && fabs(v->number) <= kMaxSafeInteger &&
         floor(v->number) == v->number;
}

// ToNumber(value) of a JSON value (nullptr is undefined): an array or object
// goes through its string form (Array.prototype.join, "[object Object]").
double js_to_number(Arena& a, const Value* v) {
  if (!v) return NAN;
  switch (v->type) {
    case Type::kNull: case Type::kFalse: return 0;
    case Type::kTrue: return 1;
    case Type::kNumber: return v->number;
    case Type::kObject: return NAN;
    case Type::kString: case Type::kArray: break;
  }
  const Value* text = v->type == Type::kString ? v : template_text(a, v, "");
  if (!text) return NAN;
  char* scratch = static_cast<char*>(a.Alloc(text->length + 8, 1));
  if (!scratch) return NAN;
  return codec::history_batch::JsStringToNumber(text->text, text->length, scratch, text->length + 8);
}

// ToInt32 / ToUint32 bits: NaN and infinities are 0; otherwise the truncated
// value modulo 2^32.
uint32_t js_uint32(double value) {
  if (!isfinite(value)) return 0;
  double m = fmod(trunc(value), 4294967296.0);
  if (m < 0) m += 4294967296.0;
  return static_cast<uint32_t>(m);
}

// transferFeePercent(rate): false for null. `rate` nullptr is undefined.
bool transfer_fee_percent(const Value* rate, char out[32]) {
  // AccountRoot omits TransferRate when the issuer charges the default 0%.
  if (!rate) { snprintf(out, 32, "0"); return true; }
  if (!safe_integer(rate) || rate->number < 1000000000.0 || rate->number > 2000000000.0) return false;
  const double basis_points = (rate->number - 1000000000.0) / 100000.0;
  const double percent = basis_points / 100.0;
  // (percent).toFixed(3): the integer n nearest percent * 1000 on the exact
  // double value, the larger on a tie. percent is in [0, 100]: as
  // mantissa * 2^-shift with a 53-bit mantissa, mantissa * 1000 < 2^63.
  uint64_t n = 0;
  if (percent > 0) {
    int exponent = 0;
    const double fraction = frexp(percent, &exponent);  // [0.5, 1)
    const uint64_t mantissa = static_cast<uint64_t>(ldexp(fraction, 53));
    const int shift = 53 - exponent;  // > 0: percent <= 100 < 2^53
    const uint64_t scaled = mantissa * 1000u;
    if (shift < 64) {
      n = scaled >> shift;
      const uint64_t rest = scaled & ((uint64_t{1} << shift) - 1);
      if (rest >= (uint64_t{1} << (shift - 1))) ++n;
    }
  }
  char text[32];
  snprintf(text, sizeof(text), "%llu.%03llu", static_cast<unsigned long long>(n / 1000),
           static_cast<unsigned long long>(n % 1000));
  // .replace(/\.?0+$/, ''): trailing fraction zeros, then a bare point.
  size_t length = strlen(text);
  while (text[length - 1] == '0') --length;
  if (text[length - 1] == '.') --length;
  text[length] = '\0';
  snprintf(out, 32, "%s", text);
  return true;
}

// rawToDecimalString(raw, decimals) for an all-digit raw string; false for
// null (decimals not an integer in 0..24).
Value* raw_to_decimal_string(Arena& a, const char* raw, size_t raw_length, const Value* decimals) {
  if (!tj::IsNumber(decimals) || !isfinite(decimals->number) || floor(decimals->number) != decimals->number ||
      decimals->number < 0 || decimals->number > 24)
    return tj::Null(a);
  const size_t places = static_cast<size_t>(decimals->number);
  // BigInt(raw).toString(): leading zeros dropped.
  size_t first = 0;
  while (first + 1 < raw_length && raw[first] == '0') ++first;
  const size_t length = raw_length - first;
  const size_t padded = length < places + 1 ? places + 1 : length;
  char* digits = static_cast<char*>(a.Alloc(padded + 2, 1));
  if (!digits) return nullptr;
  memset(digits, '0', padded - length);
  memcpy(digits + padded - length, raw + first, length);
  if (!places) return tj::Str(a, digits, padded);
  const size_t whole = padded - places;
  size_t fraction = places;
  while (fraction && digits[whole + fraction - 1] == '0') --fraction;
  if (!fraction) return tj::Str(a, digits, whole);
  memmove(digits + whole + 1, digits + whole, fraction);
  digits[whole] = '.';
  return tj::Str(a, digits, whole + 1 + fraction);
}

// controlRow / riskRow: Yes with the given sentiment, else No and 'good'.
Value* yes_no_row(Arena& a, const char* label, bool enabled, const char* sentiment) {
  return row(a, label, tj::Str(a, enabled ? "Yes" : "No"), enabled ? sentiment : "good");
}

// The next WTF-8 code point at *position (a lone surrogate keeps its value).
uint32_t next_code_point(const char* text, size_t length, size_t* position, size_t* width) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(text);
  const size_t start = *position;
  const unsigned char c = bytes[(*position)++];
  uint32_t value = c;
  if (c >= 0x80) {
    const unsigned trailing = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : 3;
    value = c & (trailing == 1 ? 0x1f : trailing == 2 ? 0x0f : 0x07);
    for (unsigned i = 0; i < trailing && *position < length; ++i)
      value = (value << 6) | (bytes[(*position)++] & 0x3f);
  }
  *width = *position - start;
  return value;
}

// key.toUpperCase() === target, for an ASCII target (the ref grammar's
// currency code). The only non-ASCII code points whose full upper-case
// mapping is ASCII are U+00DF, U+0131, U+017F and U+FB00..U+FB06 (checked
// against V8's String.prototype.toUpperCase over every code point).
bool upper_equals(const char* key, size_t key_length, const char* target) {
  const size_t target_length = strlen(target);
  size_t at = 0, position = 0;
  while (position < key_length) {
    size_t width = 0;
    const uint32_t cp = next_code_point(key, key_length, &position, &width);
    const char* upper = nullptr;
    char single_char[2] = {0, 0};
    if (cp < 0x80) {
      single_char[0] = static_cast<char>(cp >= 'a' && cp <= 'z' ? cp - 'a' + 'A' : cp);
      upper = single_char;
    } else {
      switch (cp) {
        case 0xdf: upper = "SS"; break;
        case 0x131: upper = "I"; break;
        case 0x17f: upper = "S"; break;
        case 0xfb00: upper = "FF"; break;
        case 0xfb01: upper = "FI"; break;
        case 0xfb02: upper = "FL"; break;
        case 0xfb03: upper = "FFI"; break;
        case 0xfb04: upper = "FFL"; break;
        case 0xfb05: case 0xfb06: upper = "ST"; break;
        default: return false;
      }
    }
    const size_t length = cp < 0x80 ? 1 : strlen(upper);
    if (at + length > target_length || memcmp(target + at, upper, length) != 0) return false;
    at += length;
  }
  return at == target_length;
}

// The UTF-16 code unit at `index` of a WTF-8 string, as a WTF-8 string (half
// of a surrogate pair becomes a lone surrogate). nullptr past the end.
Value* code_unit_at(Arena& a, const Value* text, uint32_t index) {
  uint32_t unit = 0;
  size_t position = 0;
  while (position < text->length) {
    const size_t start = position;
    size_t width = 0;
    const uint32_t cp = next_code_point(text->text, text->length, &position, &width);
    if (cp <= 0xffff) {
      if (unit == index) return tj::Str(a, text->text + start, width);
      ++unit;
      continue;
    }
    const uint32_t v = cp - 0x10000;
    const uint32_t halves[2] = {0xd800 + (v >> 10), 0xdc00 + (v & 0x3ff)};
    for (uint32_t half : halves) {
      if (unit++ != index) continue;
      const char bytes[3] = {static_cast<char>(0xe0 | (half >> 12)), static_cast<char>(0x80 | ((half >> 6) & 0x3f)),
                             static_cast<char>(0x80 | (half & 0x3f))};
      return tj::Str(a, bytes, 3);
    }
  }
  return nullptr;
}

// issuedObligation(obligations, currency): the first Object.entries pair
// whose key, or its displayCurrency, upper-cases to the currency; its value
// when a non-empty string. *out is nullptr for null (the caller's `?? '0'`).
// False on arena exhaustion.
bool issued_obligation(Arena& a, const Value* obligations, const char* currency, const Value** out) {
  *out = nullptr;
  if (!truthy(obligations)) return true;
  char target[64];
  const size_t length = strlen(currency);
  if (length >= sizeof(target)) return true;
  for (size_t i = 0; i <= length; ++i) {
    const char c = currency[i];
    target[i] = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
  }
  auto matches = [&](const char* key, size_t key_length, bool* match) {
    *match = upper_equals(key, key_length, target);
    if (*match) return true;
    const char* display = XrplDisplayCurrency(a, key, key_length);
    if (!display) return a.ok;
    *match = upper_equals(display, strlen(display), target);
    return true;
  };
  auto answer = [&](const Value* value) {
    *out = non_empty_string(value) ? value : nullptr;
    return true;
  };
  if (obligations->type == Type::kObject) {
    // Members are already in JavaScript property order.
    for (uint32_t i = 0; i < obligations->count; ++i) {
      bool match = false;
      if (!matches(obligations->members[i].key, obligations->members[i].key_length, &match)) return false;
      if (match) return answer(obligations->members[i].value);
    }
    return true;
  }
  if (obligations->type != Type::kArray && obligations->type != Type::kString) return true;
  // Arrays and strings enumerate their indices "0", "1", ...: only the
  // canonical index equal to the currency can match (an index key has no
  // other upper-case or display spelling).
  uint32_t index = 0;
  if (!codec::history_batch::JsArrayIndexKey(target, length, &index)) return true;
  if (obligations->type == Type::kArray) return index < obligations->count ? answer(obligations->items[index]) : true;
  const Value* unit = code_unit_at(a, obligations, index);
  if (!unit) return a.ok;
  return answer(unit);
}

// `${a}${b}${c}` of three strings.
Value* concat(Arena& a, const Value* first_text, const char* middle, const Value* last, const char* suffix) {
  const size_t capacity = first_text->length + strlen(middle) + last->length + strlen(suffix) + 1;
  char* buffer = static_cast<char*>(a.Alloc(capacity, 1));
  if (!buffer) return nullptr;
  tj::OutBuffer out{reinterpret_cast<uint8_t*>(buffer), capacity, 0, true};
  out.Append(first_text->text, first_text->length);
  out.Text(middle);
  out.Append(last->text, last->length);
  out.Text(suffix);
  return out.ok ? tj::Str(a, buffer, out.used) : nullptr;
}

// ---- Hedera Mirror token reads ---------------------------------------------------

constexpr const char* kMirrorSource = "hedera-mirror";
// The longest canonical entity id: 10 + 10 + 12 digits and two dots.
constexpr size_t kEntityIdBytes = 40;
// Significant digits a hexadecimal, octal or binary BigInt string may have
// (2^4096 at most): beyond that the conversion to decimal is refused (a
// documented native bound; Mirror reports decimal strings).
constexpr size_t kMaxRadixDigits = 4096;

enum class Big : uint8_t { kValue, kThrows, kTooLarge };

// Decimal digits of a non-negative integral double (exact).
bool double_digits(Arena& a, double magnitude, const char** digits, size_t* length) {
  if (magnitude < 18446744073709551616.0) {
    char text[24];
    snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(magnitude));
    *length = strlen(text);
    *digits = a.CopyText(text, *length);
    return *digits != nullptr;
  }
  int exponent = 0;
  const double fraction = frexp(magnitude, &exponent);  // [0.5, 1)
  uint64_t mantissa = static_cast<uint64_t>(ldexp(fraction, 53));
  int doublings = exponent - 53;  // >= 11: magnitude >= 2^64
  // Little-endian decimal digits; a double has at most 309 integer digits.
  uint8_t work[320];
  size_t count = 0;
  while (mantissa) { work[count++] = static_cast<uint8_t>(mantissa % 10); mantissa /= 10; }
  for (; doublings > 0; --doublings) {
    unsigned carry = 0;
    for (size_t i = 0; i < count; ++i) {
      const unsigned v = work[i] * 2u + carry;
      work[i] = static_cast<uint8_t>(v % 10);
      carry = v / 10;
    }
    if (carry) {
      if (count == sizeof(work)) return false;
      work[count++] = static_cast<uint8_t>(carry);
    }
  }
  char* out = static_cast<char*>(a.Alloc(count + 1, 1));
  if (!out) return false;
  for (size_t i = 0; i < count; ++i) out[i] = static_cast<char>('0' + work[count - 1 - i]);
  out[count] = '\0';
  *digits = out;
  *length = count;
  return true;
}

int radix_digit(char c, int radix) {
  int d = -1;
  if (c >= '0' && c <= '9') d = c - '0';
  else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
  else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
  return d >= 0 && d < radix ? d : -1;
}

// StringToBigInt (BigInt("…")): StrWhiteSpace around a decimal integer with
// an optional sign, or a sign-less 0x / 0o / 0b literal; empty is 0n.
Big string_to_bigint(Arena& a, const char* text, size_t length, bool* negative, const char** digits,
                     size_t* digit_count) {
  size_t begin = 0, end = 0;
  codec::history_batch::JsTrim(text, length, &begin, &end);
  const char* s = text + begin;
  size_t n = end - begin;
  *negative = false;
  if (!n) { *digits = "0"; *digit_count = 1; return Big::kValue; }
  int radix = 10;
  if (n >= 2 && s[0] == '0') {
    const char p = s[1];
    radix = p == 'x' || p == 'X' ? 16 : p == 'o' || p == 'O' ? 8 : p == 'b' || p == 'B' ? 2 : 10;
  }
  if (radix != 10) {
    s += 2;
    n -= 2;
    if (!n) return Big::kThrows;
    for (size_t i = 0; i < n; ++i) if (radix_digit(s[i], radix) < 0) return Big::kThrows;
    while (n > 1 && s[0] == '0') { ++s; --n; }
    if (n > kMaxRadixDigits) return Big::kTooLarge;
    // Base 1e9 limbs, little-endian.
    const size_t limb_capacity = n * 4 / 29 + 2;
    uint32_t* limbs = static_cast<uint32_t*>(a.Alloc(sizeof(uint32_t) * limb_capacity, 4));
    if (!limbs) return Big::kThrows;
    size_t limb_count = 0;
    for (size_t i = 0; i < n; ++i) {
      uint64_t carry = static_cast<uint64_t>(radix_digit(s[i], radix));
      for (size_t k = 0; k < limb_count; ++k) {
        const uint64_t t = static_cast<uint64_t>(limbs[k]) * static_cast<uint64_t>(radix) + carry;
        limbs[k] = static_cast<uint32_t>(t % 1000000000u);
        carry = t / 1000000000u;
      }
      while (carry) {
        if (limb_count == limb_capacity) return Big::kThrows;
        limbs[limb_count++] = static_cast<uint32_t>(carry % 1000000000u);
        carry /= 1000000000u;
      }
    }
    char* out = static_cast<char*>(a.Alloc(limb_count * 9 + 2, 1));
    if (!out) return Big::kThrows;
    if (!limb_count) { out[0] = '0'; out[1] = '\0'; *digits = out; *digit_count = 1; return Big::kValue; }
    int used = snprintf(out, 12, "%u", limbs[limb_count - 1]);
    for (size_t k = limb_count - 1; k-- > 0;) used += snprintf(out + used, 10, "%09u", limbs[k]);
    *digits = out;
    *digit_count = static_cast<size_t>(used);
    return Big::kValue;
  }
  bool minus = false;
  if (s[0] == '+' || s[0] == '-') { minus = s[0] == '-'; ++s; --n; }
  if (!n) return Big::kThrows;
  for (size_t i = 0; i < n; ++i) if (s[i] < '0' || s[i] > '9') return Big::kThrows;
  while (n > 1 && s[0] == '0') { ++s; --n; }
  *negative = minus && !(n == 1 && s[0] == '0');
  *digits = s;
  *digit_count = n;
  return Big::kValue;
}

// BigInt(value) of a JSON value (nullptr is undefined): sign and decimal
// digits without leading zeros.
Big js_bigint(Arena& a, const Value* v, bool* negative, const char** digits, size_t* length) {
  *negative = false;
  if (nullish(v) || v->type == Type::kObject) return Big::kThrows;  // TypeError / SyntaxError
  switch (v->type) {
    case Type::kTrue: *digits = "1"; *length = 1; return Big::kValue;
    case Type::kFalse: *digits = "0"; *length = 1; return Big::kValue;
    case Type::kNumber:
      // NumberToBigInt: a RangeError unless integral.
      if (!isfinite(v->number) || trunc(v->number) != v->number) return Big::kThrows;
      *negative = v->number < 0;
      return double_digits(a, fabs(v->number), digits, length) ? Big::kValue : Big::kThrows;
    default: break;
  }
  const Value* text = v->type == Type::kString ? v : template_text(a, v, "");
  if (!text) return Big::kThrows;
  return string_to_bigint(a, text->text, text->length, negative, digits, length);
}

// Number.isInteger(n) && n >= 0.
bool non_negative_integer(double n) { return isfinite(n) && trunc(n) == n && n >= 0; }

// rawToDecimalString(raw, decimals) over any JSON raw value: null for
// decimals outside the integers 0..24 or a raw value BigInt rejects.
// nullptr on arena exhaustion.
Value* raw_to_decimal(Arena& a, const Value* raw, double decimals) {
  if (!non_negative_integer(decimals) || decimals > 24) return tj::Null(a);
  bool negative = false;
  const char* digits = nullptr;
  size_t length = 0;
  if (js_bigint(a, raw, &negative, &digits, &length) != Big::kValue) return tj::Null(a);
  const size_t places = static_cast<size_t>(decimals);
  const size_t padded = length < places + 1 ? places + 1 : length;
  char* out = static_cast<char*>(a.Alloc(padded + 3, 1));
  if (!out) return nullptr;
  size_t used = 0;
  if (negative) out[used++] = '-';
  char* body = out + used;
  memset(body, '0', padded - length);
  memcpy(body + padded - length, digits, length);
  if (!places) return tj::Str(a, out, used + padded);
  const size_t whole = padded - places;
  size_t fraction = places;
  while (fraction && body[whole + fraction - 1] == '0') --fraction;
  if (!fraction) return tj::Str(a, out, used + whole);
  memmove(body + whole + 1, body + whole, fraction);
  body[whole] = '.';
  return tj::Str(a, out, used + whole + 1 + fraction);
}

// new Date(ms).toISOString() of a valid time value (|ms| <= 8.64e15, already
// truncated), extended years included.
void iso_time(int64_t t, char out[40]) {
  int64_t days = t / 86400000;
  int64_t rem = t % 86400000;
  if (rem < 0) { rem += 86400000; --days; }
  // civil_from_days (Howard Hinnant).
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
  const int hour = static_cast<int>(rem / 3600000);
  const int minute = static_cast<int>(rem / 60000 % 60);
  const int second = static_cast<int>(rem / 1000 % 60);
  const int milli = static_cast<int>(rem % 1000);
  if (year >= 0 && year <= 9999)
    snprintf(out, 40, "%04lld-%02d-%02dT%02d:%02d:%02d.%03dZ", static_cast<long long>(year), month, day, hour,
             minute, second, milli);
  else
    snprintf(out, 40, "%c%06lld-%02d-%02dT%02d:%02d:%02d.%03dZ", year < 0 ? '-' : '+',
             static_cast<long long>(year < 0 ? -year : year), month, day, hour, minute, second, milli);
}

double js_string_number(Arena& a, const Value* text) {
  char* scratch = static_cast<char*>(a.Alloc(text->length + 8, 1));
  if (!scratch) return NAN;
  return codec::history_batch::JsStringToNumber(text->text, text->length, scratch, text->length + 8);
}

// timestampDisplay(value): seconds.nanos or integer nanoseconds to an ISO
// time, else the raw String(value). False where it throws (BigInt of a
// malformed string longer than 12 UTF-16 units). *out nullptr: no arena.
bool timestamp_display(Arena& a, const Value* value, Value** out) {
  *out = nullptr;
  Value* raw = template_text(a, value, "");
  if (!raw) return true;
  double seconds = NAN;
  if (memchr(raw->text, '.', raw->length)) {
    seconds = js_string_number(a, raw);
  } else if (codec::history_batch::Utf16Units(raw->text, raw->length) > 12) {
    bool negative = false;
    const char* digits = nullptr;
    size_t length = 0;
    const Big big = string_to_bigint(a, raw->text, raw->length, &negative, &digits, &length);
    if (big == Big::kThrows) return a.ok ? false : true;
    // Number(BigInt(raw) / 1_000_000_000n): division truncates toward zero.
    // A quotient past 13 digits is no valid time, which keeps the raw text.
    if (big == Big::kValue && length <= 9 + 13) {
      int64_t quotient = 0;
      for (size_t i = 0; i + 9 < length; ++i) quotient = quotient * 10 + (digits[i] - '0');
      seconds = negative ? -static_cast<double>(quotient) : static_cast<double>(quotient);
      if (seconds == 0) seconds = 0;  // 0n has no sign
    } else {
      seconds = INFINITY;
    }
  } else {
    seconds = js_string_number(a, raw);
  }
  if (!isfinite(seconds)) { *out = raw; return true; }
  const double ms = seconds * 1000;
  if (!isfinite(ms) || fabs(ms) > 8.64e15) { *out = raw; return true; }
  char iso[40];
  iso_time(static_cast<int64_t>(trunc(ms)), iso);
  *out = tj::Str(a, iso);
  return true;
}

// hasKey(value): any non-nullish primitive, or an object/array with an own
// enumerable key.
bool has_key(const Value* v) {
  if (nullish(v)) return false;
  if (v->type == Type::kArray || v->type == Type::kObject) return v->count > 0;
  return true;
}

// `info.custom_fees?.{key}?.length ?? 0` (any JSON value; nullptr never).
Value* fee_rule_count(Arena& a, const Value* info, const char* key) {
  const Value* fees = member(info, "custom_fees");
  const Value* list = nullish(fees) ? nullptr : member(fees, key);
  const Value* count = nullptr;
  if (!nullish(list)) {
    if (list->type == Type::kArray) return tj::Num(a, list->count);
    if (list->type == Type::kString) return tj::Num(a, static_cast<double>(codec::history_batch::Utf16Units(list->text, list->length)));
    if (list->type == Type::kObject) count = member(list, "length");
  }
  return nullish(count) ? tj::Num(a, 0) : const_cast<Value*>(count);
}

// A JavaScript primitive: the operands and results of `+`.
struct Primitive {
  enum Kind : uint8_t { kNumber, kBoolean, kString } kind = kNumber;
  double number = 0;  // kNumber, kBoolean (0 / 1)
  const Value* text = nullptr;  // kString
};

// ToPrimitive(value) (default hint) of a JSON value.
bool to_primitive(Arena& a, const Value* v, Primitive* out) {
  *out = Primitive{};
  switch (v->type) {
    case Type::kNumber: out->number = v->number; return true;
    case Type::kTrue: out->kind = Primitive::kBoolean; out->number = 1; return true;
    case Type::kFalse: out->kind = Primitive::kBoolean; return true;
    case Type::kNull: return true;  // never reached: nullish counts are 0
    case Type::kString: out->kind = Primitive::kString; out->text = v; return true;
    case Type::kArray: case Type::kObject:
      out->kind = Primitive::kString;
      out->text = template_text(a, v, "");
      return out->text != nullptr;
  }
  return false;
}

// ToString of a primitive.
const Value* primitive_text(Arena& a, const Primitive& p) {
  if (p.kind == Primitive::kString) return p.text;
  if (p.kind == Primitive::kBoolean) return tj::Str(a, p.number ? "true" : "false");
  return count_text(a, p.number);
}

// The `+` operator: string concatenation when either side is a string,
// otherwise numeric addition (booleans as 0 / 1).
bool js_add(Arena& a, const Primitive& left, const Primitive& right, Primitive* out) {
  *out = Primitive{};
  if (left.kind != Primitive::kString && right.kind != Primitive::kString) {
    out->number = left.number + right.number;
    return true;
  }
  const Value* l = primitive_text(a, left);
  const Value* r = primitive_text(a, right);
  if (!l || !r) return false;
  char* joined = static_cast<char*>(a.Alloc(l->length + r->length + 1, 1));
  if (!joined) return false;
  memcpy(joined, l->text, l->length);
  memcpy(joined + l->length, r->text, r->length);
  out->kind = Primitive::kString;
  out->text = tj::Str(a, joined, l->length + r->length);
  return out->text != nullptr;
}

// sanitizeSymbol(value): a TypeError (nullptr) for anything but a string.
Value* sanitize_symbol(Arena& a, const Value* v) {
  if (!tj::IsString(v)) return nullptr;
  return chains::SanitizeSymbol(a, v->text, v->length);
}

// Number(info.decimals), then Number.isInteger && >= 0 (validDecimals).
bool valid_decimals(Arena& a, const Value* info, double* out) {
  const double decimals = js_to_number(a, member(info, "decimals"));
  if (!non_negative_integer(decimals)) return false;
  *out = decimals;
  return true;
}

// `validDecimals === null || info[key] === undefined ? null :
// rawToDecimalString(info[key], validDecimals)`.
Value* mirror_supply(Arena& a, const Value* info, const char* key, bool has_decimals, double decimals) {
  const Value* raw = member(info, key);
  if (!has_decimals || !raw) return tj::Null(a);
  return raw_to_decimal(a, raw, decimals);
}

bool push_row(Arena& a, Value* rows, const char* label, Value* value, const char* sentiment = nullptr) {
  Value* r = row(a, label, value, sentiment);
  return r && tj::Push(a, rows, r);
}

}  // namespace

bool StellarAssetPath(const char* address, const char* symbol, char* out, size_t capacity) {
  if (!address || !capacity) return false;
  // splitAsset: `CODE-ISSUER`, or a bare issuer named by the symbol.
  char code[128];
  const char* issuer = address;
  const char* dash = strchr(address, '-');
  if (dash) {
    const size_t length = static_cast<size_t>(dash - address);
    if (length >= sizeof(code)) return false;
    memcpy(code, address, length);
    code[length] = '\0';
    issuer = dash + 1;
  } else {
    if (!symbol || strlen(symbol) >= sizeof(code)) return false;
    snprintf(code, sizeof(code), "%s", symbol);
  }
  size_t used = 0;
  out[0] = '\0';
  return append_text("/assets?asset_code=", out, capacity, &used) &&
         append_component(code, out, capacity, &used) &&
         append_text("&asset_issuer=", out, capacity, &used) &&
         append_component(issuer, out, capacity, &used) && append_text("&limit=1", out, capacity, &used);
}

bool CardanoSummaryPath(const char* unit, char* out, size_t capacity) {
  if (!unit) return false;
  const size_t length = strlen(unit);
  if (length < 56 || length > 120 || !chains::IsHex(unit, length)) return false;
  char policy[57], name[65];
  for (size_t i = 0; i < length; ++i) {
    const char c = unit[i];
    const char lower = c >= 'A' && c <= 'F' ? static_cast<char>(c - 'A' + 'a') : c;
    if (i < 56) policy[i] = lower;
    else name[i - 56] = lower;
  }
  policy[56] = '\0';
  name[length - 56] = '\0';
  const int n = snprintf(out, capacity, "/asset_summary?_asset_policy=%s&_asset_name=%s", policy, name);
  return n > 0 && static_cast<size_t>(n) < capacity;
}

Value* StellarHolders(Arena& a, const Value* page) {
  if (nullish(page)) return nullptr;  // `null._embedded`
  double count = 0;
  return holder_count(a, horizon_holder_count(horizon_record(page), &count), count, "horizon");
}

Value* StellarInsight(Arena& a, const Value* page) {
  if (nullish(page)) return nullptr;
  const Value* record = horizon_record(page);
  if (!truthy(record))
    return single(a, section(a, "Ledger", tj::Str(a, "Horizon has no record of this asset."), false, tj::Arr(a)));

  const Value* flags = member(record, "flags");  // `?? {}`: every member of {} is undefined
  double holders = 0;
  const bool has_holders = horizon_holder_count(record, &holders);
  const Value* supply = member(record, "amount");
  if (nullish(supply)) supply = nullptr;
  const Value* claimable = member(record, "num_claimable_balances");

  Value* supply_rows = tj::Arr(a);
  if (!supply_rows) return nullptr;
  if (supply && !tj::Push(a, supply_rows, row(a, "Issued", const_cast<Value*>(supply), nullptr))) return nullptr;
  if (has_holders && !tj::Push(a, supply_rows, row(a, "Accounts holding", count_text(a, holders), nullptr)))
    return nullptr;
  if (truthy(claimable) &&
      !tj::Push(a, supply_rows, row(a, "Claimable balances", template_text(a, claimable, ""), nullptr)))
    return nullptr;
  Value* supply_summary = supply ? template_text(a, supply, " issued") : tj::Str(a, "Not reported");

  const bool required = truthy(member(flags, "auth_required"));
  const bool revocable = truthy(member(flags, "auth_revocable"));
  const bool clawback = truthy(member(flags, "auth_clawback_enabled"));
  const bool immutable = truthy(member(flags, "auth_immutable"));
  Value* control_rows = tj::Arr(a);
  if (!control_rows ||
      !tj::Push(a, control_rows, row(a, "Trustline approval", tj::Str(a, required ? "Required" : "Open"),
                                     required ? "caution" : nullptr)) ||
      !tj::Push(a, control_rows, row(a, "Can freeze holdings", tj::Str(a, revocable ? "Yes" : "No"),
                                     revocable ? "caution" : nullptr)) ||
      !tj::Push(a, control_rows, row(a, "Can claw back", tj::Str(a, clawback ? "Yes" : "No"),
                                     clawback ? "bad" : nullptr)) ||
      !tj::Push(a, control_rows, row(a, "Flags locked", tj::Str(a, immutable ? "Yes (cannot change)" : "No"),
                                     immutable ? "good" : nullptr)))
    return nullptr;

  Value* sections = tj::Arr(a);
  if (!sections || !tj::Push(a, sections, section(a, "Supply", supply_summary, supply != nullptr, supply_rows)) ||
      !tj::Push(a, sections, section(a, "Issuer controls", describe_flags(a, flags), true, control_rows)))
    return nullptr;
  return sections;
}

Value* CardanoHolders(Arena& a, const Value* rows) {
  if (nullish(rows)) return nullptr;  // `null[0]`
  double count = 0;
  return holder_count(a, koios_holder_count(first(rows), &count), count, "koios");
}

Value* CardanoInsight(Arena& a, const Value* rows) {
  if (nullish(rows)) return nullptr;
  const Value* summary = first(rows);
  if (!truthy(summary))
    return single(a, section(a, "Chain activity", tj::Str(a, "Koios has no asset summary"), false, tj::Arr(a)));
  double holders = 0, transactions = 0, staked = 0, unstaked = 0;
  const bool has_holders = koios_holder_count(summary, &holders);
  const bool has_transactions = non_negative_safe(member(summary, "total_transactions"), &transactions);
  const bool has_staked = non_negative_safe(member(summary, "staked_wallets"), &staked);
  const bool has_unstaked = non_negative_safe(member(summary, "unstaked_addresses"), &unstaked);
  Value* list = tj::Arr(a);
  if (!list) return nullptr;
  if (has_holders && !tj::Push(a, list, row(a, "Wallets holding", count_text(a, holders), nullptr))) return nullptr;
  if (has_transactions && !tj::Push(a, list, row(a, "Transactions", count_text(a, transactions), nullptr)))
    return nullptr;
  if (has_staked && !tj::Push(a, list, row(a, "Staked wallets", count_text(a, staked), nullptr))) return nullptr;
  if (has_unstaked && !tj::Push(a, list, row(a, "Unstaked addresses", count_text(a, unstaked), nullptr)))
    return nullptr;
  Value* summary_text = has_holders ? count_text(a, holders, " wallets hold this asset")
                                    : tj::Str(a, "Cardano native asset");
  return single(a, section(a, "Chain activity", summary_text, has_holders || has_transactions, list));
}

bool AlgorandAssetPath(const char* address, char* out, size_t capacity) {
  if (!all_digits(address)) return false;  // /^\d+$/
  const int n = snprintf(out, capacity, "/v2/assets/%s", address);
  return n > 0 && static_cast<size_t>(n) < capacity;
}

Value* AlgorandInsight(Arena& a, const Value* asset) {
  if (nullish(asset)) return nullptr;  // `null.params`
  const Value* params = member(asset, "params");
  if (!truthy(params)) return tj::Arr(a);
  // params.total: a digit string, or a safe non-negative integer.
  const Value* total = member(params, "total");
  const char* raw = nullptr;
  size_t raw_length = 0;
  char number[40];
  if (tj::IsString(total)) {
    bool digits = total->length > 0;
    for (uint32_t i = 0; i < total->length && digits; ++i) digits = total->text[i] >= '0' && total->text[i] <= '9';
    if (digits) { raw = total->text; raw_length = total->length; }
  } else if (double ignored = 0; non_negative_safe(total, &ignored)) {
    chains::JsNumberText(total->number, number, sizeof(number));
    raw = number;
    raw_length = strlen(number);
  }
  // `params.decimals === undefined`: only an absent member.
  const Value* decimals = member(params, "decimals");
  Value* supply = !raw || !decimals ? tj::Null(a) : raw_to_decimal_string(a, raw, raw_length, decimals);
  if (!supply) return nullptr;
  const bool has_supply = supply->type == Type::kString;

  Value* asset_rows = tj::Arr(a);
  if (!asset_rows) return nullptr;
  if (has_supply && !tj::Push(a, asset_rows, row(a, "Total supply", supply, nullptr))) return nullptr;
  if (decimals && !tj::Push(a, asset_rows, row(a, "Decimals", template_text(a, decimals, ""), nullptr)))
    return nullptr;
  Value* asset_summary = has_supply ? template_text(a, supply, " total supply") : tj::Str(a, "Algorand Standard Asset");

  const bool can_manage = non_empty_string(member(params, "manager"));
  const bool can_freeze = non_empty_string(member(params, "freeze"));
  const bool can_clawback = non_empty_string(member(params, "clawback"));
  const Value* frozen = member(params, "default-frozen");
  Value* control_rows = tj::Arr(a);
  if (!control_rows || !tj::Push(a, control_rows, yes_no_row(a, "Can reconfigure", can_manage, "caution")) ||
      !tj::Push(a, control_rows, yes_no_row(a, "Can freeze holdings", can_freeze, "caution")) ||
      !tj::Push(a, control_rows, yes_no_row(a, "Can claw back", can_clawback, "bad")) ||
      !tj::Push(a, control_rows,
                yes_no_row(a, "Frozen by default", frozen && frozen->type == Type::kTrue, "caution")))
    return nullptr;
  Value* control_summary = tj::Str(a, can_clawback || can_freeze || can_manage
                                          ? "Issuer controls remain enabled"
                                          : "Control addresses are permanently cleared");
  Value* sections = tj::Arr(a);
  if (!sections || !tj::Push(a, sections, section(a, "Asset", asset_summary, true, asset_rows)) ||
      !tj::Push(a, sections, section(a, "Asset controls", control_summary, true, control_rows)))
    return nullptr;
  return sections;
}

bool XrplIssuedToken(const char* address, const char* symbol, char* currency, size_t currency_capacity,
                     char* issuer, size_t issuer_capacity) {
  if (!address) return false;
  const char* dot = strchr(address, '.');
  const char* code = symbol;
  size_t code_length = symbol ? strlen(symbol) : 0;
  const char* account = address;
  if (dot) {
    code = address;
    code_length = static_cast<size_t>(dot - address);
    account = dot + 1;
  }
  // `currency === null`: a bare issuer without a symbol.
  if (!code || code_length >= currency_capacity || !xrpl_address(account) ||
      strlen(account) >= issuer_capacity)
    return false;
  memcpy(currency, code, code_length);
  currency[code_length] = '\0';
  snprintf(issuer, issuer_capacity, "%s", account);
  return true;
}

bool XrplGatewayBalancesBody(const char* issuer, char* out, size_t capacity, size_t* length) {
  const int n = snprintf(out, capacity,
                         "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"gateway_balances\",\"params\":"
                         "[{\"account\":\"%s\",\"strict\":true,\"ledger_index\":\"validated\"}]}",
                         issuer);
  if (n <= 0 || static_cast<size_t>(n) >= capacity) return false;
  *length = static_cast<size_t>(n);
  return true;
}

bool XrplAccountInfoBody(const char* issuer, char* out, size_t capacity, size_t* length) {
  const int n = snprintf(out, capacity,
                         "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"account_info\",\"params\":"
                         "[{\"account\":\"%s\",\"ledger_index\":\"validated\"}]}",
                         issuer);
  if (n <= 0 || static_cast<size_t>(n) >= capacity) return false;
  *length = static_cast<size_t>(n);
  return true;
}

const char* XrplDisplayCurrency(Arena& arena, const char* currency, size_t length) {
  static const char kAllowed[] = "?!@#$%^&*<>(){}[]|";
  if (length == 3) {
    bool ok = true;
    for (size_t i = 0; i < 3 && ok; ++i) {
      const char c = currency[i];
      ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           (c != '\0' && strchr(kAllowed, c) != nullptr);
    }
    if (ok) return arena.CopyText(currency, length);
  }
  if (length != 40 || !chains::IsHex(currency, length)) return nullptr;
  uint8_t bytes[20];
  size_t written = 0;
  if (!chains::HexDecode(currency, length, bytes, sizeof(bytes), &written) || written != 20) return nullptr;
  size_t end = 20;
  while (end > 0 && bytes[end - 1] == 0) --end;
  if (end == 0) return nullptr;
  for (size_t i = 0; i < end; ++i)
    if (bytes[i] < 0x20 || bytes[i] > 0x7e) return arena.CopyText(currency, length);
  return arena.CopyText(reinterpret_cast<const char*>(bytes), end);
}

Value* XrplInsight(Arena& a, const char* currency, const Value* gateway, const Value* info) {
  // gateway.error === 'actNotFound' || info.error === 'actNotFound' (a null
  // result is a TypeError where it is read).
  if (nullish(gateway)) return nullptr;
  bool not_found = string_is(member(gateway, "error"), "actNotFound");
  if (!not_found) {
    if (nullish(info)) return nullptr;
    not_found = string_is(member(info, "error"), "actNotFound");
  }
  if (not_found)
    return single(a, section(a, "Ledger", tj::Str(a, "Issuer account not found"), false, tj::Arr(a)));
  if (string_is(member(gateway, "status"), "error") || truthy(member(gateway, "error"))) return nullptr;
  if (string_is(member(info, "status"), "error") || truthy(member(info, "error"))) return nullptr;

  const Value* found = nullptr;
  if (!issued_obligation(a, member(gateway, "obligations"), currency, &found)) return nullptr;
  const Value* obligation = found ? found : tj::Str(a, "0");
  // info.account_data?.Flags ?? 0, then (flags & flag) !== 0 on ToInt32.
  const Value* account = member(info, "account_data");
  const Value* flags_value = member(account, "Flags");
  const uint32_t flags = nullish(flags_value) ? 0 : js_uint32(js_to_number(a, flags_value));
  const bool requires_auth = (flags & kLsfRequireAuth) != 0;
  const bool global_freeze = (flags & kLsfGlobalFreeze) != 0;
  const bool no_freeze = (flags & kLsfNoFreeze) != 0;
  const bool clawback = (flags & kLsfAllowTrustlineClawback) != 0;
  const bool blocks_trustlines = (flags & kLsfDisallowIncomingTrustline) != 0;
  char fee[32];
  const bool has_fee = transfer_fee_percent(member(account, "TransferRate"), fee);

  Value* symbol = chains::SanitizeSymbol(a, currency, strlen(currency));
  if (!obligation || !symbol) return nullptr;
  Value* supply_rows = single(a, row(a, "Outstanding obligation", const_cast<Value*>(obligation), nullptr));
  Value* supply_summary = concat(a, obligation, " ", symbol, " outstanding");

  Value* fee_value = nullptr;
  if (has_fee) {
    char text[40];
    snprintf(text, sizeof(text), "%s%%", fee);
    fee_value = tj::Str(a, text);
  } else {
    fee_value = tj::Str(a, "None reported");
  }
  Value* control_rows = tj::Arr(a);
  if (!control_rows ||
      !tj::Push(a, control_rows, yes_no_row(a, "Authorization required", requires_auth, "caution")) ||
      !tj::Push(a, control_rows, yes_no_row(a, "Global freeze active", global_freeze, "bad")) ||
      !tj::Push(a, control_rows, row(a, "Freeze authority renounced", tj::Str(a, no_freeze ? "Yes" : "No"),
                                     no_freeze ? "good" : "caution")) ||
      !tj::Push(a, control_rows, yes_no_row(a, "Clawback enabled", clawback, "bad")) ||
      !tj::Push(a, control_rows, yes_no_row(a, "New trustlines blocked", blocks_trustlines, "caution")) ||
      !tj::Push(a, control_rows,
                row(a, "Transfer fee", fee_value, has_fee && strcmp(fee, "0") != 0 ? "caution" : nullptr)))
    return nullptr;
  Value* control_summary = tj::Str(a, global_freeze ? "All issued assets are globally frozen"
                                      : clawback || requires_auth || blocks_trustlines
                                          ? "Issuer restrictions are enabled"
                                          : "No restrictive issuer flags reported");
  Value* sections = tj::Arr(a);
  if (!sections || !tj::Push(a, sections, section(a, "Issued supply", supply_summary, true, supply_rows)) ||
      !tj::Push(a, sections, section(a, "Issuer controls", control_summary, true, control_rows)))
    return nullptr;
  return sections;
}

bool HederaTokenPath(const char* address, char* out, size_t capacity) {
  char id[kEntityIdBytes];
  if (!address || !networks::CanonicalHederaEntityId(address, strlen(address), id, sizeof(id))) return false;
  // encodeURIComponent of digits and dots is the id itself.
  const int n = snprintf(out, capacity, "/api/v1/tokens/%s", id);
  return n > 0 && static_cast<size_t>(n) < capacity;
}

bool HederaBalancesPath(const char* address, char* out, size_t capacity) {
  char id[kEntityIdBytes];
  if (!address || !networks::CanonicalHederaEntityId(address, strlen(address), id, sizeof(id))) return false;
  const int n = snprintf(out, capacity, "/api/v1/tokens/%s/balances?account.balance=gt:0&limit=100&order=asc", id);
  return n > 0 && static_cast<size_t>(n) < capacity;
}

Value* HederaHolders(Arena& a, const Value* page) {
  if (nullish(page)) return nullptr;  // `null.balances`
  const Value* balances = member(page, "balances");
  if (!tj::IsArray(balances)) return tj::Null(a);
  // typeof page.links?.next === 'string' && page.links.next !== ''
  const bool at_least = non_empty_string(member(member(page, "links"), "next"));
  Value* o = tj::Obj(a);
  if (!o || !tj::Set(a, o, "count", tj::Num(a, balances->count)) || !tj::Set(a, o, "atLeast", tj::Bool(a, at_least)) ||
      !tj::Set(a, o, "source", tj::Str(a, kMirrorSource)))
    return nullptr;
  return o;
}

Value* HederaMetadata(Arena& a, const Value* info) {
  if (nullish(info)) return nullptr;  // `null.decimals`
  double decimals = 0;
  const bool has_decimals = valid_decimals(a, info, &decimals);
  const Value* symbol_value = member(info, "symbol");
  Value* symbol = truthy(symbol_value) ? sanitize_symbol(a, symbol_value) : tj::Null(a);
  if (!symbol) return nullptr;
  // typeof info.name === 'string' && info.name.trim() !== '' ? info.name.trim() : null
  const Value* name_value = member(info, "name");
  Value* name = tj::Null(a);
  if (tj::IsString(name_value)) {
    size_t begin = 0, end = 0;
    codec::history_batch::JsTrim(name_value->text, name_value->length, &begin, &end);
    if (end > begin) name = tj::Str(a, name_value->text + begin, end - begin);
  }
  Value* total = mirror_supply(a, info, "total_supply", has_decimals, decimals);
  Value* max = mirror_supply(a, info, "max_supply", has_decimals, decimals);
  Value* o = tj::Obj(a);
  if (!o || !name || !total || !max || !tj::Set(a, o, "symbol", symbol) || !tj::Set(a, o, "name", name) ||
      !tj::Set(a, o, "decimals", has_decimals ? tj::Num(a, decimals) : tj::Null(a)) ||
      !tj::Set(a, o, "verified", tj::Null(a)) || !tj::Set(a, o, "holderCount", tj::Null(a)) ||
      !tj::Set(a, o, "logoMediaKey", tj::Null(a)) || !tj::Set(a, o, "totalSupply", total) ||
      !tj::Set(a, o, "maxSupply", max))
    return nullptr;
  return o;
}

Value* HederaInsight(Arena& a, const char* token_id, const Value* info) {
  if (nullish(info)) return nullptr;  // `null.decimals`
  double decimals = 0;
  const bool has_decimals = valid_decimals(a, info, &decimals);
  Value* supply = mirror_supply(a, info, "total_supply", has_decimals, decimals);
  Value* max_supply = mirror_supply(a, info, "max_supply", has_decimals, decimals);
  Value* initial_supply = mirror_supply(a, info, "initial_supply", has_decimals, decimals);
  if (!supply || !max_supply || !initial_supply) return nullptr;
  const bool has_supply = supply->type == Type::kString;

  // Fee rule counts and their sum, with JavaScript `+` (a `length` member
  // of a non-array object can be any value).
  Value* fixed = fee_rule_count(a, info, "fixed_fees");
  Value* fractional = fee_rule_count(a, info, "fractional_fees");
  Value* royalty = fee_rule_count(a, info, "royalty_fees");
  if (!fixed || !fractional || !royalty) return nullptr;
  Primitive p_fixed, p_fractional, p_royalty, partial, fee_count;
  if (!to_primitive(a, fixed, &p_fixed) || !to_primitive(a, fractional, &p_fractional) ||
      !to_primitive(a, royalty, &p_royalty) || !js_add(a, p_fixed, p_fractional, &partial) ||
      !js_add(a, partial, p_royalty, &fee_count))
    return nullptr;
  const bool no_fees = fee_count.kind == Primitive::kNumber && fee_count.number == 0;
  const bool one_fee = fee_count.kind == Primitive::kNumber && fee_count.number == 1;

  // Token: info.name && info.symbol ? `${name} (${sanitizeSymbol(symbol)})`
  // : info.name ?? sanitizeSymbol(info.symbol ?? tokenId).
  const Value* name = member(info, "name");
  const Value* symbol = member(info, "symbol");
  Value* summary = nullptr;
  if (truthy(name) && truthy(symbol)) {
    Value* clean = sanitize_symbol(a, symbol);
    Value* head = clean ? template_text(a, name, " (") : nullptr;
    summary = head ? concat(a, head, "", clean, ")") : nullptr;
  } else if (!nullish(name)) {
    summary = const_cast<Value*>(name);
  } else {
    summary = sanitize_symbol(a, nullish(symbol) ? tj::Str(a, token_id) : symbol);
  }
  if (!summary) return nullptr;
  const Value* deleted = member(info, "deleted");
  const bool is_deleted = deleted && deleted->type == Type::kTrue;
  Value* token_rows = tj::Arr(a);
  if (!token_rows) return nullptr;
  const Value* id_value = member(info, "token_id");
  if (!push_row(a, token_rows, "Token ID", nullish(id_value) ? tj::Str(a, token_id) : const_cast<Value*>(id_value)))
    return nullptr;
  const Value* type = member(info, "type");
  if (truthy(type) && !push_row(a, token_rows, "Type", const_cast<Value*>(type))) return nullptr;
  const Value* treasury = member(info, "treasury_account_id");
  if (truthy(treasury) && !push_row(a, token_rows, "Treasury", const_cast<Value*>(treasury))) return nullptr;
  const Value* created = member(info, "created_timestamp");
  if (truthy(created)) {
    Value* shown = nullptr;
    if (!timestamp_display(a, created, &shown) || !shown || !push_row(a, token_rows, "Created", shown)) return nullptr;
  }
  const Value* expiry = member(info, "expiry_timestamp");
  if (expiry) {  // `!== undefined`: a JSON null is displayed too
    Value* shown = nullptr;
    if (!timestamp_display(a, expiry, &shown) || !shown || !push_row(a, token_rows, "Expires", shown)) return nullptr;
  }
  if (!push_row(a, token_rows, "Deleted", tj::Str(a, is_deleted ? "Yes" : "No"), is_deleted ? "bad" : "good"))
    return nullptr;

  Value* supply_rows = tj::Arr(a);
  if (!supply_rows) return nullptr;
  if (has_supply && !push_row(a, supply_rows, "Current supply", supply)) return nullptr;
  if (initial_supply->type == Type::kString && !push_row(a, supply_rows, "Initial supply", initial_supply))
    return nullptr;
  if (max_supply->type == Type::kString && !push_row(a, supply_rows, "Maximum supply", max_supply)) return nullptr;
  const Value* supply_type = member(info, "supply_type");
  if (truthy(supply_type) && !push_row(a, supply_rows, "Supply type", const_cast<Value*>(supply_type)))
    return nullptr;
  if (has_decimals && !push_row(a, supply_rows, "Decimals", count_text(a, decimals))) return nullptr;
  const Value* freeze_default = member(info, "freeze_default");
  if (freeze_default) {  // `=== undefined ? [] : …`
    const bool frozen = truthy(freeze_default);
    if (!push_row(a, supply_rows, "Accounts frozen by default", tj::Str(a, frozen ? "Yes" : "No"),
                  frozen ? "bad" : "good"))
      return nullptr;
  }
  Value* supply_summary = has_supply ? template_text(a, supply, " current supply")
                                     : tj::Str(a, "Mirror did not report supply");

  Value* fee_rows = tj::Arr(a);
  if (!fee_rows || !push_row(a, fee_rows, "Fixed fee rules", template_text(a, fixed, "")) ||
      !push_row(a, fee_rows, "Fractional fee rules", template_text(a, fractional, "")) ||
      !push_row(a, fee_rows, "Royalty fee rules", template_text(a, royalty, "")))
    return nullptr;
  Value* fee_summary = nullptr;
  if (no_fees) {
    fee_summary = tj::Str(a, "No custom transfer fees reported");
  } else {
    const Value* count = primitive_text(a, fee_count);
    fee_summary = count ? template_text(a, count, one_fee ? " custom fee rule" : " custom fee rules") : nullptr;
  }

  struct Control { const char* label; const char* key; const char* sentiment; };
  static constexpr Control kControls[] = {
      {"Admin key", "admin_key", "caution"},       {"Freeze key", "freeze_key", "caution"},
      {"Wipe key", "wipe_key", "bad"},             {"Pause key", "pause_key", "caution"},
      {"KYC key", "kyc_key", "caution"},           {"Fee schedule key", "fee_schedule_key", "caution"},
      {"Supply key", "supply_key", "caution"},     {"Metadata key", "metadata_key", "caution"},
  };
  Value* control_rows = tj::Arr(a);
  if (!control_rows) return nullptr;
  bool any_key = false;
  for (const Control& control : kControls) {
    const bool present = has_key(member(info, control.key));
    any_key = any_key || present;
    if (!push_row(a, control_rows, control.label, tj::Str(a, present ? "Present" : "Absent"),
                  present ? control.sentiment : "good"))
      return nullptr;
  }
  Value* control_summary =
      tj::Str(a, any_key ? "Administrative keys remain enabled" : "No administrative keys reported");

  Value* sections = tj::Arr(a);
  if (!sections || !tj::Push(a, sections, section(a, "Token", summary, !is_deleted, token_rows)) ||
      !tj::Push(a, sections, section(a, "Supply", supply_summary, has_supply, supply_rows)) ||
      !tj::Push(a, sections, section(a, "Custom transfer fees", fee_summary, no_fees, fee_rows)) ||
      !tj::Push(a, sections, section(a, "Token controls", control_summary, true, control_rows)))
    return nullptr;
  return sections;
}

bool SuiCoinMetadataBody(const char* coin_type, char* out, size_t capacity, size_t* length) {
  // JSON.stringify(coinType) inside the query, then JSON.stringify of the
  // query: the canonical grammar's characters need no escaping at either
  // level, so anything else is refused rather than escaped.
  if (!coin_type || !*coin_type) return false;
  for (const char* p = coin_type; *p; ++p) {
    const char c = *p;
    const bool safe = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
                      c == ':';
    if (!safe) return false;
  }
  const int n = snprintf(out, capacity,
                         "{\"query\":\"query FortisCoinMetadata {\\n      coinMetadata(coinType: \\\"%s\\\") "
                         "{ decimals symbol name }\\n    }\"}",
                         coin_type);
  if (n <= 0 || static_cast<size_t>(n) >= capacity) return false;
  *length = static_cast<size_t>(n);
  return true;
}

Value* SuiMetadata(Arena& a, const Value* body) {
  if (nullish(body)) return nullptr;  // `null.errors`
  // An in-band GraphQL error is a transient failure, not proof the type is
  // unregistered: it must never become a cached negative answer.
  const Value* errors = member(body, "errors");
  if (tj::IsArray(errors) && errors->count > 0) return nullptr;
  // `body.data?.coinMetadata ?? null`, then `meta === null ||
  // !Number.isInteger(meta.decimals) || meta.decimals < 0` answers null.
  const Value* meta = member(member(body, "data"), "coinMetadata");
  if (nullish(meta)) return tj::Null(a);
  const Value* decimals = member(meta, "decimals");
  if (!tj::IsNumber(decimals) || !isfinite(decimals->number) || floor(decimals->number) != decimals->number ||
      decimals->number < 0)
    return tj::Null(a);
  // `typeof x === 'string' && x.trim() !== '' ? sanitize(x) : null`; the
  // sanitizer sees the untrimmed text and may still answer "".
  auto text = [&a](const Value* v, bool symbol) -> Value* {
    if (!tj::IsString(v)) return tj::Null(a);
    size_t begin = 0, end = 0;
    codec::history_batch::JsTrim(v->text, v->length, &begin, &end);
    if (end == begin) return tj::Null(a);
    return symbol ? chains::SanitizeSymbol(a, v->text, v->length) : chains::SanitizeName(a, v->text, v->length);
  };
  Value* symbol = text(member(meta, "symbol"), true);
  Value* name = text(member(meta, "name"), false);
  Value* o = tj::Obj(a);
  if (!o || !symbol || !name || !tj::Set(a, o, "symbol", symbol) || !tj::Set(a, o, "name", name) ||
      !tj::Set(a, o, "decimals", tj::Num(a, decimals->number)) || !tj::Set(a, o, "verified", tj::Null(a)) ||
      !tj::Set(a, o, "holderCount", tj::Null(a)) || !tj::Set(a, o, "logoMediaKey", tj::Null(a)))
    return nullptr;
  return o;
}

Value* RawToDecimal(Arena& a, const Value* raw, double decimals) { return raw_to_decimal(a, raw, decimals); }

}  // namespace fortis::chains::asset_index
