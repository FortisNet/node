#include "utf8.h"
#include "cross_chain_support.h"

#include "chain_core.h"
#include "provider_text.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::exchange::cross_chain {
namespace {

namespace tt = fortis::codec::provider_text;

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

bool ascii_only(const Value* v) {
  if (!v || v->type != tj::Type::kString) return false;
  for (uint32_t i = 0; i < v->length; ++i)
    if (static_cast<uint8_t>(v->text[i]) >= 0x80 || v->text[i] == '\0') return false;
  return true;
}

void append_js_string(Arena& arena, OutBuffer& out, const Value* v, bool in_array);

// Array.prototype.join(',') as String(array) runs it.
void append_join(Arena& arena, OutBuffer& out, const Value* array) {
  for (uint32_t i = 0; i < array->count; ++i) {
    if (i) out.Byte(',');
    const Value* item = array->items[i];
    if (Nullish(item)) continue;
    append_js_string(arena, out, item, true);
  }
}

void append_js_string(Arena& arena, OutBuffer& out, const Value* v, bool in_array) {
  if (!v) { if (!in_array) out.Text("undefined"); return; }
  switch (v->type) {
    case tj::Type::kNull: if (!in_array) out.Text("null"); return;
    case tj::Type::kTrue: out.Text("true"); return;
    case tj::Type::kFalse: out.Text("false"); return;
    case tj::Type::kNumber: {
      char text[40];
      NumberText(v->number, text);
      out.Text(text);
      return;
    }
    case tj::Type::kString: out.Append(v->text, v->length); return;
    case tj::Type::kArray: append_join(arena, out, v); return;
    case tj::Type::kObject: out.Text("[object Object]"); return;
  }
}

size_t js_string_bytes(const Value* v) {
  if (!v) return 16;
  switch (v->type) {
    case tj::Type::kString: return v->length + 1;
    case tj::Type::kNumber: return 40;
    case tj::Type::kArray: {
      size_t total = v->count + 1;
      for (uint32_t i = 0; i < v->count; ++i) total += js_string_bytes(v->items[i]);
      return total;
    }
    default: return 24;
  }
}

bool regex_digits(const char* text, size_t length, size_t min, size_t max, bool nonzero_first) {
  if (length < min || length > max) return false;
  if (nonzero_first && text[0] == '0') return false;
  for (size_t i = 0; i < length; ++i)
    if (!is_digit(text[i])) return false;
  return true;
}

// civil_from_days (Howard Hinnant), valid for every day the ECMAScript time
// range reaches.
void civil(int64_t days, int64_t* year, int* month, int* day) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t doe = days - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  *day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  *month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  if (*month <= 2) ++y;
  *year = y;
}

const char* kBridgeWebsites[][2] = {
    {"bridge-vet", "https://bridge.vet/"},
    {"bridge-vet-swap", "https://bridge.vet/"},
    {"across", "https://across.to/"},
    {"across-v4", "https://across.to/"},
    {"acrossv4", "https://across.to/"},
    {"axelar", "https://www.axelar.network/"},
    {"gaszip", "https://www.gas.zip/"},
    {"gaszip-bridge", "https://www.gas.zip/"},
    {"gaszipbridge", "https://www.gas.zip/"},
    {"maya", "https://www.mayaprotocol.com/"},
    {"mayan", "https://mayan.finance/"},
    {"mayan-swift", "https://mayan.finance/"},
    {"mayanwh", "https://mayan.finance/"},
    {"mayan-wormhole", "https://mayan.finance/"},
    {"mayanmctp", "https://mayan.finance/"},
    {"cctp-mayan", "https://mayan.finance/"},
    {"mayanfastmctp", "https://mayan.finance/"},
    {"cctpv2-mayan", "https://mayan.finance/"},
    {"simpleswap", "https://simpleswap.io/"},
    {"mayachain", "https://www.mayaprotocol.com/"},
    {"near-intents", "https://near-intents.org/"},
    {"near-intents-1click", "https://near-intents.org/"},
    {"1click", "https://near-intents.org/"},
    {"relay", "https://relay.link/"},
    {"relaydepository", "https://relay.link/"},
    {"stargate", "https://stargate.finance/bridge"},
    {"stargate-v2", "https://stargate.finance/bridge"},
    {"stargatev2", "https://stargate.finance/bridge"},
    {"stargatev2bus", "https://stargate.finance/bridge"},
    {"symbiosis", "https://symbiosis.finance/"},
    {"symbiosis-finance", "https://symbiosis.finance/"},
};

const char kBech32[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

int bech32_value(char c) {
  const char* found = c ? strchr(kBech32, c) : nullptr;
  return found ? static_cast<int>(found - kBech32) : -1;
}

uint32_t bech32_polymod(const uint8_t* values, size_t count) {
  static const uint32_t kGenerators[] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3};
  uint32_t checksum = 1;
  for (size_t i = 0; i < count; ++i) {
    const uint32_t top = checksum >> 25;
    checksum = ((checksum & 0x1ffffff) << 5) ^ values[i];
    for (int g = 0; g < 5; ++g)
      if ((top >> g) & 1) checksum ^= kGenerators[g];
  }
  return checksum;
}

// The TypeScript NEAR Intents convertBits(values, 5, 8) (masked accumulator).
bool convert_bits_masked(const uint8_t* values, size_t count, uint8_t* out, size_t capacity, size_t* written) {
  uint32_t accumulator = 0;
  int bits = 0;
  size_t used = 0;
  const uint32_t max_value = (1u << 8) - 1;
  const uint32_t max_accumulator = (1u << (5 + 8 - 1)) - 1;
  for (size_t i = 0; i < count; ++i) {
    if (values[i] >> 5) return false;
    accumulator = ((accumulator << 5) | values[i]) & max_accumulator;
    bits += 5;
    while (bits >= 8) {
      bits -= 8;
      if (used == capacity) return false;
      out[used++] = static_cast<uint8_t>((accumulator >> bits) & max_value);
    }
  }
  if (bits >= 5 || ((accumulator << (8 - bits)) & max_value) != 0) return false;
  *written = used;
  return true;
}

// The TypeScript Bitcoin Cash convertBits (32-bit accumulator; only low bits matter).
bool convert_bits_plain(const uint8_t* values, size_t count, uint8_t* out, size_t capacity, size_t* written) {
  uint32_t accumulator = 0;
  int bits = 0;
  size_t used = 0;
  for (size_t i = 0; i < count; ++i) {
    if (values[i] >= 32) return false;
    accumulator = (accumulator << 5) | values[i];
    bits += 5;
    while (bits >= 8) {
      bits -= 8;
      if (used == capacity) return false;
      out[used++] = static_cast<uint8_t>((accumulator >> bits) & 0xff);
    }
  }
  if (bits >= 5 || ((accumulator << (8 - bits)) & 0xff) != 0) return false;
  *written = used;
  return true;
}

bool base58_check(const Value* v, uint8_t* payload, size_t expected_length) {
  uint8_t decoded[64];
  size_t written = 0;
  if (!chains::Base58Decode(v->text, v->length, decoded, sizeof(decoded), &written) ||
      written != expected_length)
    return false;
  uint8_t first[32], second[32];
  chains::Sha256(decoded, expected_length - 4, first);
  chains::Sha256(first, sizeof(first), second);
  if (memcmp(second, decoded + expected_length - 4, 4) != 0) return false;
  memcpy(payload, decoded, expected_length - 4);
  return true;
}

Value* bound(Arena& arena, const Value* value, int depth, int* nodes);

const Value kUndefinedMarker{};

}  // namespace

// ---- JavaScript values -------------------------------------------------------

const Value* AsRecord(const Value* v) { return v && v != &kUndefinedMarker && v->type == tj::Type::kObject ? v : nullptr; }

const Value* Field(const Value* record, const char* key) {
  const Value* object = AsRecord(record);
  return object ? tj::Get(object, key) : nullptr;
}

bool Is(const Value* v, const char* text) {
  if (!v || v->type != tj::Type::kString) return false;
  const size_t length = strlen(text);
  return v->length == length && memcmp(v->text, text, length) == 0;
}

bool SameText(const Value* a, const Value* b) {
  return a && b && a->type == tj::Type::kString && b->type == tj::Type::kString && a->length == b->length &&
         memcmp(a->text, b->text, a->length) == 0;
}

size_t Units(const char* text, size_t length) { return tt::Utf16Length(text, length); }
size_t Units(const Value* v) { return v && v->type == tj::Type::kString ? Units(v->text, v->length) : 0; }

bool Blank(const char* text, size_t length) {
  size_t begin = 0, end = 0;
  tt::Trim(text, length, &begin, &end);
  return begin == end;
}

const Value* StringValue(const Value* v, size_t max_units) {
  if (!v || v == &kUndefinedMarker || v->type != tj::Type::kString) return nullptr;
  if (Blank(v->text, v->length) || Units(v) > max_units) return nullptr;
  return v;
}

Value* JsString(Arena& arena, const Value* v) {
  if (v == &kUndefinedMarker) v = nullptr;
  if (v && v->type == tj::Type::kString) return const_cast<Value*>(v);
  const size_t capacity = js_string_bytes(v) + 16;
  uint8_t* buffer = static_cast<uint8_t*>(arena.Alloc(capacity, 1));
  if (!buffer) return nullptr;
  OutBuffer out{buffer, capacity, 0, true};
  append_js_string(arena, out, v, false);
  if (!out.ok) return nullptr;
  return tj::Str(arena, reinterpret_cast<const char*>(buffer), out.used);
}

double JsNumber(const Value* v) {
  if (!v || v == &kUndefinedMarker) return NAN;
  switch (v->type) {
    case tj::Type::kNull: return 0;
    case tj::Type::kFalse: return 0;
    case tj::Type::kTrue: return 1;
    case tj::Type::kNumber: return v->number;
    case tj::Type::kString: return tt::ToNumber(v->text, v->length);
    case tj::Type::kArray: {
      // Number(array) is Number(String(array)). A joined text longer than
      // the buffer is never a numeric literal a provider would send.
      uint8_t local[4096];
      OutBuffer out{local, sizeof(local), 0, true};
      Arena unused{};
      append_join(unused, out, v);
      return out.ok ? tt::ToNumber(reinterpret_cast<const char*>(local), out.used) : NAN;
    }
    case tj::Type::kObject: return NAN;
  }
  return NAN;
}

bool FiniteNumber(const Value* v, double* out) {
  const double number = JsNumber(v);
  if (!isfinite(number)) return false;
  *out = number;
  return true;
}

bool IntegerNumber(const Value* v, double* out) {
  double number = 0;
  if (!FiniteNumber(v, &number) || trunc(number) != number) return false;
  *out = number;
  return true;
}

const Value* ChainValue(Arena& arena, const Value* v) {
  if (!v || v == &kUndefinedMarker) return nullptr;
  if (v->type == tj::Type::kString) return Blank(v->text, v->length) ? nullptr : v;
  if (v->type == tj::Type::kNumber) return JsString(arena, v);
  return nullptr;
}

bool SameIdentifier(Arena& arena, const Value* reported, const Value* expected) {
  const Value* value = ChainValue(arena, reported);
  if (!value) return true;
  if (StartsWith(value, "0x") && StartsWith(expected, "0x")) {
    const Value* a = Lower(arena, value);
    const Value* b = Lower(arena, expected);
    return SameText(a, b);
  }
  return SameText(value, expected);
}

bool PositiveIntegerText(const char* text, size_t length) { return regex_digits(text, length, 1, 80, true); }
bool NonNegativeIntegerText(const char* text, size_t length) { return regex_digits(text, length, 1, 80, false); }

namespace {
const char* integer_text(Arena& arena, const Value* v, bool positive) {
  if (!v || v == &kUndefinedMarker) return nullptr;
  if (v->type != tj::Type::kString && v->type != tj::Type::kNumber) return nullptr;
  const Value* text = JsString(arena, v);
  if (!text) return nullptr;
  const bool ok = positive ? PositiveIntegerText(text->text, text->length)
                           : NonNegativeIntegerText(text->text, text->length);
  return ok ? arena.CopyText(text->text, text->length) : nullptr;
}
}  // namespace

const char* SafePositiveInteger(Arena& arena, const Value* v) { return integer_text(arena, v, true); }
const char* SafeNonNegativeInteger(Arena& arena, const Value* v) { return integer_text(arena, v, false); }

bool SafeEta(const Value* const* values, size_t count, double* out) {
  for (size_t i = 0; i < count; ++i) {
    double number = 0;
    if (IntegerNumber(values[i], &number) && number >= 0) {
      *out = number == 0 ? 0 : number;
      return true;
    }
  }
  return false;
}

Value* Lower(Arena& arena, const Value* v) {
  if (!v || v->type != tj::Type::kString) return nullptr;
  return tt::ToLowerCase(arena, v->text, v->length);
}

Value* Upper(Arena& arena, const Value* v) {
  if (!v || v->type != tj::Type::kString) return nullptr;
  return tt::ToUpperCase(arena, v->text, v->length);
}

bool StartsWith(const Value* v, const char* prefix) {
  if (!v || v->type != tj::Type::kString) return false;
  const size_t length = strlen(prefix);
  return v->length >= length && memcmp(v->text, prefix, length) == 0;
}

bool Contains(const Value* v, const char* needle) {
  if (!v || v->type != tj::Type::kString) return false;
  const size_t length = strlen(needle);
  if (!length) return true;
  for (size_t i = 0; i + length <= v->length; ++i)
    if (memcmp(v->text + i, needle, length) == 0) return true;
  return false;
}

const char* WellFormed(Arena& arena, const char* text, size_t length, size_t* out_length) {
  const size_t size = fortis::codec::utf8::Normalize(reinterpret_cast<const uint8_t*>(text), length, nullptr, 0);
  char* out = static_cast<char*>(arena.Alloc(size + 1, 1));
  if (!out) return nullptr;
  fortis::codec::utf8::Normalize(reinterpret_cast<const uint8_t*>(text), length, out, size);
  out[size] = '\0';
  if (out_length) *out_length = size;
  return out;
}

const char* CText(Arena& arena, const Value* v) {
  if (!v || v->type != tj::Type::kString) return nullptr;
  return arena.CopyText(v->text, v->length);
}

// ---- time ----------------------------------------------------------------------

bool IsoTime(double ms, char output[40]) {
  if (!isfinite(ms) || fabs(ms) > 8.64e15) return false;
  const int64_t t = static_cast<int64_t>(trunc(ms));
  int64_t days = t / 86400000;
  int64_t rem = t % 86400000;
  if (rem < 0) { rem += 86400000; --days; }
  int64_t year = 0;
  int month = 0, day = 0;
  civil(days, &year, &month, &day);
  const int hour = static_cast<int>(rem / 3600000);
  const int minute = static_cast<int>(rem / 60000 % 60);
  const int second = static_cast<int>(rem / 1000 % 60);
  const int milli = static_cast<int>(rem % 1000);
  if (year >= 0 && year <= 9999) {
    snprintf(output, 40, "%04lld-%02d-%02dT%02d:%02d:%02d.%03dZ", static_cast<long long>(year), month, day, hour,
             minute, second, milli);
  } else {
    snprintf(output, 40, "%c%06lld-%02d-%02dT%02d:%02d:%02d.%03dZ", year < 0 ? '-' : '+',
             static_cast<long long>(year < 0 ? -year : year), month, day, hour, minute, second, milli);
  }
  return true;
}

const char* IsoText(Arena& arena, double ms) {
  char text[40];
  return IsoTime(ms, text) ? arena.CopyText(text, strlen(text)) : nullptr;
}

bool DateParse(const char* text, size_t length, double* ms) { return tt::DateParse(text, length, ms); }

const char* SafeExpiry(Arena& arena, const Value* v) {
  const Value* text = StringValue(v);
  if (!text) return nullptr;
  double ms = 0;
  if (!DateParse(text->text, text->length, &ms)) return nullptr;
  return IsoText(arena, ms);
}

const char* SafeTimestamp(Arena& arena, const Value* v) {
  if (!v || v == &kUndefinedMarker) return nullptr;
  double millis = 0;
  bool have = false;
  if (v->type == tj::Type::kNumber && isfinite(v->number)) {
    millis = v->number < 10000000000.0 ? v->number * 1000 : v->number;
    have = true;
  } else if (v->type == tj::Type::kString && !Blank(v->text, v->length)) {
    if (regex_digits(v->text, v->length, 1, SIZE_MAX, false)) {
      const double numeric = tt::ToNumber(v->text, v->length);
      if (isfinite(numeric) && trunc(numeric) == numeric && fabs(numeric) <= 9007199254740991.0) {
        millis = numeric < 10000000000.0 ? numeric * 1000 : numeric;
        have = true;
      }
    } else {
      double parsed = 0;
      if (DateParse(v->text, v->length, &parsed)) { millis = parsed; have = true; }
    }
  }
  if (!have || millis < 946684800000.0 || millis > 4102444800000.0) return nullptr;
  return IsoText(arena, millis);
}

bool SecondsBetween(const char* started, const char* completed, double* seconds) {
  if (!started || !completed) return false;
  double a = 0, b = 0;
  const bool parsed_a = DateParse(started, strlen(started), &a);
  const bool parsed_b = DateParse(completed, strlen(completed), &b);
  if (!parsed_a || !parsed_b) return false;  // NaN >= 0 is false
  const double value = floor((b - a) / 1000);
  if (!(value >= 0)) return false;
  *seconds = value == 0 ? 0 : value;
  return true;
}

bool QuoteValidity(Arena& arena, const Value* provider_expiry, double now_ms, double local_validity_ms,
                   Validity* out) {
  const double fallback = now_ms + local_validity_ms;
  const char* reported = SafeExpiry(arena, provider_expiry);
  double reported_ms = INFINITY;
  if (reported && !DateParse(reported, strlen(reported), &reported_ms)) reported_ms = NAN;
  // Math.min with NaN is NaN, and toISOString of NaN throws.
  const double expires = reported_ms != reported_ms ? NAN : (reported_ms < fallback ? reported_ms : fallback);
  out->quoted_at = IsoText(arena, now_ms);
  out->expires_at = IsoText(arena, expires);
  return out->quoted_at && out->expires_at;
}

// ---- route helpers ---------------------------------------------------------------

const char* NormalizedRiskKey(Arena& arena, const Value* value) {
  if (!value || value->type != tj::Type::kString) return "unknown";
  size_t begin = 0, end = 0;
  tt::Trim(value->text, value->length, &begin, &end);
  Value* lower = tt::ToLowerCase(arena, value->text + begin, end - begin);
  if (!lower) return nullptr;
  char* out = static_cast<char*>(arena.Alloc(lower->length + 1, 1));
  if (!out) return nullptr;
  size_t used = 0;
  bool in_run = false;
  for (size_t position = 0; position < lower->length;) {
    const uint32_t cp = tt::DecodeAt(lower->text, lower->length, &position);
    const bool keep = (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9');
    if (keep) {
      out[used++] = static_cast<char>(cp);
      in_run = false;
    } else if (!in_run) {
      out[used++] = '-';
      in_run = true;
    }
  }
  size_t start = 0;
  if (used && out[0] == '-') start = 1;
  if (used > start && out[used - 1] == '-') --used;
  out[used] = '\0';
  if (used <= start) return "unknown";
  return out + start;
}

const char* BridgeWebsiteUrl(Arena& arena, const Value* bridge) {
  if (!bridge || bridge->type != tj::Type::kString || Blank(bridge->text, bridge->length)) return nullptr;
  const char* key = NormalizedRiskKey(arena, bridge);
  if (!key) return nullptr;
  for (const auto& entry : kBridgeWebsites)
    if (strcmp(entry[0], key) == 0) return entry[1];
  return nullptr;
}

Value* RouteId(Arena& arena, const char* provider_id, const Value* quote_id, const Value* bridge,
               const char* amount_out_min, const Value* execution_to, const Value* execution_data) {
  if (quote_id && quote_id->type == tj::Type::kString && quote_id->length) {
    const size_t length = strlen(provider_id) + 1 + quote_id->length;
    char* text = static_cast<char*>(arena.Alloc(length + 1, 1));
    if (!text) return nullptr;
    const size_t prefix = strlen(provider_id);
    memcpy(text, provider_id, prefix);
    text[prefix] = ':';
    memcpy(text + prefix + 1, quote_id->text, quote_id->length);
    text[length] = '\0';
    return tj::Str(arena, text, length);
  }
  const Value* parts[] = {bridge, execution_to, execution_data};
  size_t total = strlen(provider_id) + strlen(amount_out_min) + 8;
  for (const Value* part : parts) total += part && part->type == tj::Type::kString ? part->length : 0;
  uint8_t* joined = static_cast<uint8_t*>(arena.Alloc(total, 1));
  if (!joined) return nullptr;
  OutBuffer out{joined, total, 0, true};
  out.Text(provider_id);
  out.Byte('|');
  if (bridge && bridge->type == tj::Type::kString) out.Append(bridge->text, bridge->length);
  out.Byte('|');
  out.Text(amount_out_min);
  out.Byte('|');
  if (execution_to && execution_to->type == tj::Type::kString) out.Append(execution_to->text, execution_to->length);
  out.Byte('|');
  if (execution_data && execution_data->type == tj::Type::kString)
    out.Append(execution_data->text, execution_data->length);
  if (!out.ok) return nullptr;
  size_t clean_length = 0;
  const char* clean = WellFormed(arena, reinterpret_cast<const char*>(joined), out.used, &clean_length);
  if (!clean) return nullptr;
  uint8_t digest[32];
  chains::Sha256(reinterpret_cast<const uint8_t*>(clean), clean_length, digest);
  char hex[65];
  chains::HexEncode(digest, sizeof(digest), hex);
  char id[160];
  snprintf(id, sizeof(id), "%s:%.24s", provider_id, hex);
  return tj::Str(arena, id);
}

const char* ExecutionKind(const Value* data, const Value* target) {
  if (data && data->type == tj::Type::kString) {
    if (data->length >= 10) {
      bool psbt = true;
      static const char kMagic[] = "70736274ff";
      for (size_t i = 0; i < 10 && psbt; ++i) {
        char c = data->text[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        psbt = c == kMagic[i];
      }
      if (psbt) return "utxo-psbt";
    }
    if (StartsWith(data, "0x") && data->length > 2) {
      bool hex = true;
      for (size_t i = 2; i < data->length && hex; ++i) hex = is_hex(data->text[i]);
      if (hex && StartsWith(target, "0x")) return "evm-calldata";
    }
    if (data->length) return "serialized-transaction";
  }
  if (target && target->type == tj::Type::kString && target->length) return "deposit";
  return "unknown";
}

bool SumUsdCosts(const Value* estimate, double* out) {
  double total = 0;
  bool found = false;
  static const char* kFields[] = {"feeCosts", "gasCosts"};
  for (const char* name : kFields) {
    const Value* entries = Field(estimate, name);
    if (!entries || entries->type != tj::Type::kArray) continue;
    for (uint32_t i = 0; i < entries->count && i < 64; ++i) {
      const Value* cost = AsRecord(entries->items[i]);
      if (!cost) continue;
      const Value* raw = Coalesce(Coalesce(tj::Get(cost, "amountUSD"), tj::Get(cost, "amountUsd")),
                                  tj::Get(cost, "usdAmount"));
      double usd = 0;
      if (FiniteNumber(raw, &usd) && usd >= 0) {
        total += usd;
        found = true;
      }
    }
  }
  if (found) *out = total;
  return found;
}

const char* SumRawNetworkCosts(Arena& arena, const Value* estimate) {
  const Value* entries = Field(estimate, "gasCosts");
  if (!entries || entries->type != tj::Type::kArray) return nullptr;
  chains::BigInt total;
  bool found = false;
  for (uint32_t i = 0; i < entries->count && i < 64; ++i) {
    const char* amount = SafeNonNegativeInteger(arena, Field(entries->items[i], "amount"));
    if (!amount) continue;
    chains::BigInt value;
    if (!chains::BigInt::ParseDecimal(amount, strlen(amount), &value) || !total.Add(value)) return nullptr;
    found = true;
  }
  if (!found) return nullptr;
  char text[256];
  if (!total.ToDecimal(text, sizeof(text))) return nullptr;
  return arena.CopyText(text, strlen(text));
}

namespace {

const char* step_kind(Arena& arena, const Value* type, bool squid) {
  const Value* text = StringValue(type);
  const Value* lower = text ? Lower(arena, text) : nullptr;
  if (!lower) return "other";
  if (Is(lower, "swap")) return "swap";
  if (Is(lower, "cross") || Is(lower, "bridge")) return "bridge";
  if (Is(lower, "fee")) return "fee";
  if (!squid && Is(lower, "protocol")) return "protocol";
  if (squid && (Is(lower, "wrap") || Is(lower, "unwrap"))) return "protocol";
  return "other";
}

Value* token_symbol(Arena& arena, const Value* token) {
  return OrNull(arena, StringValue(Field(token, "symbol")));
}

bool push_step(Arena& arena, Value* steps, const char* kind, const Value* tool, const Value* from_chain,
               const Value* to_chain, const Value* from_token, const Value* to_token) {
  Value* step = tj::Obj(arena);
  return step && tj::Set(arena, step, "kind", tj::Str(arena, kind)) &&
         tj::Set(arena, step, "tool", const_cast<Value*>(tool)) &&
         tj::Set(arena, step, "fromChainId", OrNull(arena, ChainValue(arena, from_chain))) &&
         tj::Set(arena, step, "toChainId", OrNull(arena, ChainValue(arena, to_chain))) &&
         tj::Set(arena, step, "fromToken", token_symbol(arena, from_token)) &&
         tj::Set(arena, step, "toToken", token_symbol(arena, to_token)) && tj::Push(arena, steps, step);
}

}  // namespace

Value* ParseLifiSteps(Arena& arena, const Value* value) {
  Value* steps = tj::Arr(arena);
  if (!steps || !value || value->type != tj::Type::kArray) return steps;
  for (uint32_t i = 0; i < value->count && i < 32; ++i) {
    const Value* step = AsRecord(value->items[i]);
    if (!step) continue;
    const Value* action = AsRecord(tj::Get(step, "action"));
    const Value* tool = StringValue(Field(tj::Get(step, "toolDetails"), "name"));
    if (!tool) tool = StringValue(tj::Get(step, "tool"));
    if (!tool) tool = tj::Str(arena, "Unknown");
    if (!push_step(arena, steps, step_kind(arena, tj::Get(step, "type"), false), tool, Field(action, "fromChainId"),
                   Field(action, "toChainId"), Field(action, "fromToken"), Field(action, "toToken")))
      return nullptr;
  }
  return steps;
}

Value* ParseSquidSteps(Arena& arena, const Value* value) {
  Value* steps = tj::Arr(arena);
  if (!steps || !value || value->type != tj::Type::kArray) return steps;
  for (uint32_t i = 0; i < value->count && i < 32; ++i) {
    const Value* action = AsRecord(value->items[i]);
    if (!action) continue;
    const Value* tool = StringValue(tj::Get(action, "provider"));
    if (!tool) tool = StringValue(tj::Get(action, "description"));
    if (!tool) tool = tj::Str(arena, "Unknown");
    if (!push_step(arena, steps, step_kind(arena, tj::Get(action, "type"), true), tool,
                   Coalesce(tj::Get(action, "fromChainId"), tj::Get(action, "fromChain")),
                   Coalesce(tj::Get(action, "toChainId"), tj::Get(action, "toChain")), tj::Get(action, "fromToken"),
                   tj::Get(action, "toToken")))
      return nullptr;
  }
  return steps;
}

const Value* Undefined() { return &kUndefinedMarker; }

namespace {

// /api[-_]?key|authorization|client[-_]?secret/i over UTF-16 code units
// (non-unicode case folding never maps a non-ASCII unit to ASCII).
bool redacted_key(const char* key, size_t length) {
  auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
  auto at = [&](size_t i, const char* word) {
    const size_t n = strlen(word);
    if (i + n > length) return static_cast<size_t>(0);
    for (size_t k = 0; k < n; ++k)
      if (lower(key[i + k]) != word[k]) return static_cast<size_t>(0);
    return n;
  };
  for (size_t i = 0; i < length; ++i) {
    if (at(i, "authorization")) return true;
    size_t n = at(i, "api");
    if (n) {
      size_t j = i + n;
      if (at(j, "key")) return true;
      if (j < length && (key[j] == '-' || key[j] == '_') && at(j + 1, "key")) return true;
    }
    n = at(i, "client");
    if (n) {
      size_t j = i + n;
      if (at(j, "secret")) return true;
      if (j < length && (key[j] == '-' || key[j] == '_') && at(j + 1, "secret")) return true;
    }
  }
  return false;
}

Value* bound(Arena& arena, const Value* value, int depth, int* nodes) {
  *nodes += 1;
  if (*nodes > 4096 || depth > 10) return tj::Str(arena, "[truncated]");
  if (!value || value == &kUndefinedMarker) return tj::Str(arena, "undefined");
  switch (value->type) {
    case tj::Type::kNull: return tj::Null(arena);
    case tj::Type::kTrue: return tj::Bool(arena, true);
    case tj::Type::kFalse: return tj::Bool(arena, false);
    case tj::Type::kNumber:
      if (isfinite(value->number)) return tj::Num(arena, value->number);
      return tj::Str(arena, value->number != value->number ? "NaN" : value->number > 0 ? "Infinity" : "-Infinity");
    case tj::Type::kString: {
      if (Units(value) <= 8192) return const_cast<Value*>(value);
      Value* head = tt::Slice(arena, value->text, value->length, 0, 8192);
      if (!head) return nullptr;
      static const char kSuffix[] = "\xE2\x80\xA6[truncated]";
      const size_t length = head->length + sizeof(kSuffix) - 1;
      char* text = static_cast<char*>(arena.Alloc(length + 1, 1));
      if (!text) return nullptr;
      memcpy(text, head->text, head->length);
      memcpy(text + head->length, kSuffix, sizeof(kSuffix) - 1);
      text[length] = '\0';
      head->text = text;
      head->length = static_cast<uint32_t>(length);
      return head;
    }
    case tj::Type::kArray: {
      Value* out = tj::Arr(arena);
      if (!out) return nullptr;
      for (uint32_t i = 0; i < value->count && i < 128; ++i) {
        Value* item = bound(arena, value->items[i], depth + 1, nodes);
        if (!item || !tj::Push(arena, out, item)) return nullptr;
      }
      return out;
    }
    case tj::Type::kObject: {
      Value* out = tj::Obj(arena);
      if (!out) return nullptr;
      for (uint32_t i = 0; i < value->count && i < 128; ++i) {
        const tj::Member& member = value->members[i];
        Value* key = Units(member.key, member.key_length) <= 128
                         ? tj::Str(arena, member.key, member.key_length)
                         : tt::Slice(arena, member.key, member.key_length, 0, 128);
        if (!key) return nullptr;
        Value* child = redacted_key(key->text, key->length) ? tj::Str(arena, "[redacted]")
                                                            : bound(arena, member.value, depth + 1, nodes);
        if (!child) return nullptr;
        // result['__proto__'] = v sets the prototype (or does nothing): no
        // own property appears.
        if (key->length == 9 && memcmp(key->text, "__proto__", 9) == 0) continue;
        if (!tj::Set(arena, out, key->text, key->length, child)) return nullptr;
      }
      return out;
    }
  }
  return nullptr;
}

}  // namespace

Value* BoundedProviderSnapshot(Arena& arena, const Value* value) {
  int nodes = 0;
  Value* bounded = bound(arena, value, 0, &nodes);
  if (bounded && bounded->type == tj::Type::kObject) return bounded;
  return tj::Obj(arena);
}

Value* StrOrNull(Arena& arena, const char* text) { return text ? tj::Str(arena, text) : tj::Null(arena); }
Value* OrNull(Arena& arena, const Value* v) { return v ? const_cast<Value*>(v) : tj::Null(arena); }

// ---- addresses ---------------------------------------------------------------------

bool ValidEvmAddress(const Value* v) {
  if (!v || v->type != tj::Type::kString || v->length != 42 || !StartsWith(v, "0x")) return false;
  for (size_t i = 2; i < 42; ++i)
    if (!is_hex(v->text[i])) return false;
  return true;
}

bool ValidAptosAddress(const Value* v) {
  if (!v || v->type != tj::Type::kString || v->length != 66 || !StartsWith(v, "0x")) return false;
  for (size_t i = 2; i < 66; ++i)
    if (!is_hex(v->text[i])) return false;
  return true;
}

// /^(?=.{2,64}$)[a-z0-9]+(?:[-_.][a-z0-9]+)*$/
bool ValidNearAccount(const Value* v) {
  if (!ascii_only(v) || v->length < 2 || v->length > 64) return false;
  bool previous_separator = true;
  for (size_t i = 0; i < v->length; ++i) {
    const char c = v->text[i];
    const bool word = (c >= 'a' && c <= 'z') || is_digit(c);
    const bool separator = c == '-' || c == '_' || c == '.';
    if (word) { previous_separator = false; continue; }
    if (!separator || previous_separator) return false;
    previous_separator = true;
  }
  return !previous_separator;
}

bool ValidBitcoinAddress(const Value* v) {
  if (!ascii_only(v) || v->length < 14 || v->length > 74) return false;
  char lower[80];
  bool has_lower = false, has_upper = false;
  for (size_t i = 0; i < v->length; ++i) {
    char c = v->text[i];
    if (c >= 'a' && c <= 'z') has_lower = true;
    if (c >= 'A' && c <= 'Z') { has_upper = true; c = static_cast<char>(c - 'A' + 'a'); }
    lower[i] = c;
  }
  if (has_lower && has_upper) return false;
  const size_t length = v->length;
  if (length < 3 || memcmp(lower, "bc1", 3) != 0) return false;
  size_t separator = 0;
  for (size_t i = 0; i < length; ++i)
    if (lower[i] == '1') separator = i;
  if (separator != 2 || separator + 7 > length) return false;
  uint8_t values[80];
  size_t count = 0;
  for (size_t i = separator + 1; i < length; ++i) {
    const int value = bech32_value(lower[i]);
    if (value < 0) return false;
    values[count++] = static_cast<uint8_t>(value);
  }
  uint8_t expanded[96];
  size_t n = 0;
  expanded[n++] = 'b' >> 5;
  expanded[n++] = 'c' >> 5;
  expanded[n++] = 0;
  expanded[n++] = 'b' & 31;
  expanded[n++] = 'c' & 31;
  memcpy(expanded + n, values, count);
  const uint32_t polymod = bech32_polymod(expanded, n + count);
  const uint8_t version = values[0];
  if (version > 16) return false;
  uint8_t program[80];
  size_t program_length = 0;
  if (count < 7 || !convert_bits_masked(values + 1, count - 7, program, sizeof(program), &program_length))
    return false;
  if (program_length < 2 || program_length > 40) return false;
  if (version == 0) return polymod == 1 && (program_length == 20 || program_length == 32);
  return polymod == 0x2bc830a3;
}

bool ValidBitcoinCashAddress(const Value* v) {
  static const char kPrefix[] = "bitcoincash";
  static const uint64_t kGenerators[] = {0x98f2bc8e61ULL, 0x79b76d99e2ULL, 0xf33e5fb3c4ULL, 0xae2eabe2a8ULL,
                                         0x1e4f43e470ULL};
  static const size_t kHashSizes[] = {20, 24, 28, 32, 40, 48, 56, 64};
  if (!ascii_only(v) || v->length < 22 || v->length > 125) return false;
  char lower[128];
  bool has_lower = false, has_upper = false;
  size_t colons = 0, colon = 0;
  for (size_t i = 0; i < v->length; ++i) {
    char c = v->text[i];
    if (c >= 'a' && c <= 'z') has_lower = true;
    if (c >= 'A' && c <= 'Z') { has_upper = true; c = static_cast<char>(c - 'A' + 'a'); }
    if (c == ':') { if (!colons) colon = i; ++colons; }
    lower[i] = c;
  }
  if (has_lower && has_upper) return false;
  if (colons != 1 || colon != sizeof(kPrefix) - 1 || memcmp(lower, kPrefix, colon) != 0) return false;
  const size_t body_length = v->length - colon - 1;
  if (body_length < 9) return false;
  uint8_t values[128];
  for (size_t i = 0; i < body_length; ++i) {
    const int value = bech32_value(lower[colon + 1 + i]);
    if (value < 0) return false;
    values[i] = static_cast<uint8_t>(value);
  }
  uint64_t checksum = 1;
  auto feed = [&](uint8_t value) {
    const uint64_t top = checksum >> 35;
    checksum = ((checksum & 0x07ffffffffULL) << 5) ^ value;
    for (int bit = 0; bit < 5; ++bit)
      if ((top >> bit) & 1) checksum ^= kGenerators[bit];
  };
  for (size_t i = 0; i < colon; ++i) feed(static_cast<uint8_t>(lower[i] & 31));
  feed(0);
  for (size_t i = 0; i < body_length; ++i) feed(values[i]);
  if ((checksum ^ 1) != 0) return false;
  uint8_t decoded[128];
  size_t decoded_length = 0;
  if (!convert_bits_plain(values, body_length - 8, decoded, sizeof(decoded), &decoded_length) || !decoded_length)
    return false;
  const uint8_t version = decoded[0];
  if ((version & 0x80) != 0 || ((version >> 3) & 0x0f) > 1) return false;
  return decoded_length - 1 == kHashSizes[version & 0x07];
}

bool ValidDashAddress(const Value* v) {
  if (!ascii_only(v) || v->length < 25 || v->length > 35) return false;
  uint8_t payload[21];
  if (!base58_check(v, payload, 25)) return false;
  return payload[0] == 0x4c || payload[0] == 0x10;
}

bool ValidZcashTransparentAddress(const Value* v) {
  if (!ascii_only(v) || v->length != 35 || v->text[0] != 't' || (v->text[1] != '1' && v->text[1] != '3'))
    return false;
  uint8_t payload[22];
  if (!base58_check(v, payload, 26)) return false;
  const uint8_t second = payload[1] == 0xb8 ? 0xb8 : 0xbd;
  return payload[0] == 0x1c && payload[1] == second;
}

bool ValidSolanaAddress(const Value* v) {
  if (!ascii_only(v) || v->length < 32 || v->length > 44) return false;
  uint8_t decoded[64];
  size_t written = 0;
  return chains::Base58Decode(v->text, v->length, decoded, sizeof(decoded), &written) && written == 32;
}

bool ValidTonFriendlyAddress(const Value* v) {
  if (!ascii_only(v) || v->length != 48) return false;
  bool standard = true, url_safe = true;
  for (size_t i = 0; i < 48; ++i) {
    const char c = v->text[i];
    const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || is_digit(c);
    standard = standard && (alnum || c == '+' || c == '/');
    url_safe = url_safe && (alnum || c == '_' || c == '-');
  }
  if (!standard && !url_safe) return false;
  char normalized[49];
  for (size_t i = 0; i < 48; ++i) {
    const char c = v->text[i];
    normalized[i] = c == '-' ? '+' : c == '_' ? '/' : c;
  }
  uint8_t decoded[40];
  size_t written = 0;
  if (!chains::Base64Decode(normalized, 48, decoded, sizeof(decoded), &written) || written != 36) return false;
  if (decoded[0] != 0x11 && decoded[0] != 0x51) return false;
  if (decoded[1] != 0x00 && decoded[1] != 0xff) return false;
  uint32_t crc = 0;
  for (size_t i = 0; i < 34; ++i) {
    crc ^= static_cast<uint32_t>(decoded[i]) << 8;
    for (int bit = 0; bit < 8; ++bit) crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  }
  return decoded[34] == (crc >> 8) && decoded[35] == (crc & 0xff);
}

bool ValidMoneroAddress(const Value* v) {
  static const char kAlphabet[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  static const size_t kBlockSizes[] = {0, 2, 3, 5, 6, 7, 9, 10, 11};
  if (!ascii_only(v) || (v->length != 95 && v->length != 106)) return false;
  uint8_t data[96];
  size_t used = 0;
  for (size_t offset = 0; offset < v->length; offset += 11) {
    const size_t block = v->length - offset < 11 ? v->length - offset : 11;
    int size = -1;
    for (int i = 0; i < 9; ++i)
      if (kBlockSizes[i] == block) { size = i; break; }
    if (size < 1) return false;
    // Big-endian value in `size` bytes; overflow means value >= 2^(8*size).
    uint8_t bytes[8]{};
    for (size_t k = 0; k < block; ++k) {
      const char c = v->text[offset + k];
      const char* found = c ? strchr(kAlphabet, c) : nullptr;
      if (!found) return false;
      uint32_t carry = static_cast<uint32_t>(found - kAlphabet);
      for (int b = size - 1; b >= 0; --b) {
        carry += static_cast<uint32_t>(bytes[b]) * 58;
        bytes[b] = static_cast<uint8_t>(carry & 0xff);
        carry >>= 8;
      }
      if (carry) return false;
    }
    memcpy(data + used, bytes, static_cast<size_t>(size));
    used += static_cast<size_t>(size);
  }
  if (used == 69) {
    if (data[0] != 18 && data[0] != 42) return false;
  } else if (used != 77 || data[0] != 19) {
    return false;
  }
  uint8_t digest[32];
  chains::Keccak256(data, used - 4, digest);
  return memcmp(digest, data + used - 4, 4) == 0;
}

// ---- request encodings ---------------------------------------------------------------

bool FormEncode(const QueryPair* pairs, size_t count, OutBuffer& out) {
  static const char kHex[] = "0123456789ABCDEF";
  auto encode = [&](const char* text, size_t length) {
    for (size_t i = 0; i < length; ++i) {
      uint8_t c = static_cast<uint8_t>(text[i]);
      if (c == 0xED && i + 2 < length && static_cast<uint8_t>(text[i + 1]) >= 0xA0) {
        out.Text("%EF%BF%BD");
        i += 2;
        continue;
      }
      const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || is_digit(static_cast<char>(c)) ||
                        c == '*' || c == '-' || c == '.' || c == '_';
      if (safe) out.Byte(c);
      else if (c == ' ') out.Byte('+');
      else { out.Byte('%'); out.Byte(static_cast<uint8_t>(kHex[c >> 4])); out.Byte(static_cast<uint8_t>(kHex[c & 15])); }
    }
  };
  for (size_t i = 0; i < count; ++i) {
    if (i) out.Byte('&');
    encode(pairs[i].name, strlen(pairs[i].name));
    out.Byte('=');
    const Value* value = pairs[i].value;
    if (value && value->type == tj::Type::kString) encode(value->text, value->length);
  }
  return out.ok;
}

bool EncodeComponent(const Value* v, OutBuffer& out) {
  if (!v || v->type != tj::Type::kString) return false;
  return chains::EncodeUriComponent(v->text, v->length, out);
}

void NumberText(double value, char output[40]) { chains::JsNumberText(value, output, 40); }

// ---- exact decimal integers ------------------------------------------------------------

namespace {
bool parse_big(const char* text, chains::BigInt* out) {
  return text && chains::BigInt::ParseDecimal(text, strlen(text), out);
}
const char* big_text(Arena& arena, const chains::BigInt& value) {
  char text[256];
  if (value.negative() || !value.ToDecimal(text, sizeof(text))) return nullptr;
  return arena.CopyText(text, strlen(text));
}
}  // namespace

int CompareDecimal(const char* a, const char* b) {
  chains::BigInt x, y;
  if (!parse_big(a, &x) || !parse_big(b, &y)) return 0;
  return x.Compare(y);
}

const char* AddDecimal(Arena& arena, const char* a, const char* b) {
  chains::BigInt x, y;
  if (!parse_big(a, &x) || !parse_big(b, &y) || !x.Add(y)) return nullptr;
  return big_text(arena, x);
}

const char* SubDecimal(Arena& arena, const char* a, const char* b) {
  chains::BigInt x, y;
  if (!parse_big(a, &x) || !parse_big(b, &y) || !x.Sub(y)) return nullptr;
  return big_text(arena, x);
}

const char* MulDecimal(Arena& arena, const char* a, const char* b) {
  chains::BigInt x, y;
  if (!parse_big(a, &x) || !parse_big(b, &y) || !x.Mul(y)) return nullptr;
  return big_text(arena, x);
}

const char* MulSmallDecimal(Arena& arena, const char* a, uint32_t factor) {
  chains::BigInt x;
  if (!parse_big(a, &x) || !x.MulSmall(factor)) return nullptr;
  return big_text(arena, x);
}

const char* DivSmallDecimal(Arena& arena, const char* a, uint32_t divisor) {
  chains::BigInt x;
  uint32_t remainder = 0;
  if (!divisor || !parse_big(a, &x) || !x.DivSmall(divisor, &remainder)) return nullptr;
  return big_text(arena, x);
}

const char* ScaleUp(Arena& arena, const char* a, uint32_t exponent) {
  chains::BigInt x;
  if (!parse_big(a, &x)) return nullptr;
  for (uint32_t i = 0; i < exponent; ++i)
    if (!x.MulSmall(10)) return nullptr;
  return big_text(arena, x);
}

const char* ScaleDown(Arena& arena, const char* a, uint32_t exponent) {
  chains::BigInt x;
  if (!parse_big(a, &x)) return nullptr;
  for (uint32_t i = 0; i < exponent; ++i) {
    uint32_t remainder = 0;
    if (!x.DivSmall(10, &remainder)) return nullptr;
  }
  return big_text(arena, x);
}

bool IsZeroDecimal(const char* a) {
  chains::BigInt x;
  return parse_big(a, &x) && x.IsZero();
}

}  // namespace fortis::exchange::cross_chain
