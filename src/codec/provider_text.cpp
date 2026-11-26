#include "provider_text.h"
#include "utf8.h"

#include "js_case.h"
#include "js_date_parse.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::codec::provider_text {
namespace {

using codec::history_batch::OutBuffer;

bool removed(uint32_t cp) {
  return cp <= 0x08 || (cp >= 0x0b && cp <= 0x1f) || (cp >= 0x7f && cp <= 0x9f) ||
         (cp >= 0x200b && cp <= 0x200f) || (cp >= 0x202a && cp <= 0x202e) ||
         (cp >= 0x2060 && cp <= 0x2064) || cp == 0xfeff || cp == '`';
}

size_t units_of(uint32_t cp) { return cp > 0xffff ? 2 : 1; }

enum class Mode { kSymbol, kName, kText };

// Code points of the input, cleaned by baseClean and transformed per mode.
tj::Value* sanitize(tj::Arena& arena, const char* text, size_t length, Mode mode) {
  const size_t max_units = mode == Mode::kSymbol ? 16 : mode == Mode::kName ? 64 : 2000;
  // Pass 1: removal. Collect code points into the arena.
  uint32_t* cps = static_cast<uint32_t*>(arena.Alloc(sizeof(uint32_t) * (length + 1)));
  if (!cps) return nullptr;
  size_t count = 0;
  for (size_t position = 0; position < length;) {
    const uint32_t cp = DecodeAt(text, length, &position);
    if (!removed(cp)) cps[count++] = cp;
  }
  // trim
  size_t first = 0, last = count;
  while (first < last && IsJsWhitespace(cps[first])) ++first;
  while (last > first && IsJsWhitespace(cps[last - 1])) --last;
  // transform into a second sequence
  uint32_t* out = static_cast<uint32_t*>(arena.Alloc(sizeof(uint32_t) * (count + 1)));
  if (!out) return nullptr;
  size_t n = 0;
  for (size_t i = first; i < last;) {
    if (mode == Mode::kText) {
      if (cps[i] == '\n') {
        size_t run = 0;
        while (i + run < last && cps[i + run] == '\n') ++run;
        const size_t keep = run >= 3 ? 2 : run;
        for (size_t k = 0; k < keep; ++k) out[n++] = '\n';
        i += run;
        continue;
      }
      out[n++] = cps[i++];
    } else {
      if (IsJsWhitespace(cps[i])) {
        while (i < last && IsJsWhitespace(cps[i])) ++i;
        out[n++] = ' ';
        continue;
      }
      out[n++] = cps[i++];
    }
  }
  size_t units = 0;
  for (size_t i = 0; i < n; ++i) units += units_of(out[i]);
  const bool truncate = units > max_units;
  const size_t keep = truncate ? max_units - 1 : units;
  char* bytes = static_cast<char*>(arena.Alloc(n * 4 + 4, 1));
  if (!bytes) return nullptr;
  OutBuffer buffer{reinterpret_cast<uint8_t*>(bytes), n * 4 + 4, 0, true};
  size_t written = 0;
  for (size_t i = 0; i < n; ++i) {
    const size_t width = units_of(out[i]);
    if (written + width > keep) {
      break;
    }
    AppendCodePoint(buffer, out[i]);
    written += width;
  }
  if (truncate) AppendCodePoint(buffer, 0x2026);
  if (!buffer.ok) return nullptr;
  tj::Value* v = tj::Str(arena, "");
  if (!v) return nullptr;
  v->text = bytes;
  v->length = static_cast<uint32_t>(buffer.used);
  return v;
}

tj::Value* map_case(tj::Arena& arena, const char* text, size_t length, bool upper) {
  if (!codec::utf8::Valid(reinterpret_cast<const uint8_t*>(text), length)) {
    const tj::Value* normalized = tj::Str(arena, text, length);
    if (!normalized) return nullptr;
    text = normalized->text;
    length = normalized->length;
  }
  const size_t capacity = length * 3 + 4;
  char* out = static_cast<char*>(arena.Alloc(capacity, 1));
  if (!out) return nullptr;
  size_t used = 0;
  const bool ok = upper ? compat::JsToUpperCase(text, length, out, capacity, &used)
                        : compat::JsToLowerCase(text, length, out, capacity, &used);
  if (!ok) return nullptr;
  return tj::Str(arena, out, used);
}

}  // namespace

uint32_t DecodeAt(const char* text, size_t length, size_t* position) {
  return codec::utf8::Next(reinterpret_cast<const uint8_t*>(text), length, position);
}

void AppendCodePoint(OutBuffer& out, uint32_t cp) {
  if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
  if (cp < 0x80) { out.Byte(static_cast<uint8_t>(cp)); return; }
  if (cp < 0x800) {
    out.Byte(static_cast<uint8_t>(0xc0 | (cp >> 6)));
    out.Byte(static_cast<uint8_t>(0x80 | (cp & 63)));
    return;
  }
  if (cp < 0x10000) {
    out.Byte(static_cast<uint8_t>(0xe0 | (cp >> 12)));
    out.Byte(static_cast<uint8_t>(0x80 | ((cp >> 6) & 63)));
    out.Byte(static_cast<uint8_t>(0x80 | (cp & 63)));
    return;
  }
  out.Byte(static_cast<uint8_t>(0xf0 | (cp >> 18)));
  out.Byte(static_cast<uint8_t>(0x80 | ((cp >> 12) & 63)));
  out.Byte(static_cast<uint8_t>(0x80 | ((cp >> 6) & 63)));
  out.Byte(static_cast<uint8_t>(0x80 | (cp & 63)));
}

size_t Utf16Length(const char* text, size_t length) {
  size_t units = 0;
  for (size_t position = 0; position < length;) units += units_of(DecodeAt(text, length, &position));
  return units;
}

tj::Value* Slice(tj::Arena& arena, const char* text, size_t length, size_t begin, size_t end) {
  char* out = static_cast<char*>(arena.Alloc(length + 4, 1));
  if (!out) return nullptr;
  OutBuffer buffer{reinterpret_cast<uint8_t*>(out), length + 4, 0, true};
  size_t unit = 0;
  for (size_t position = 0; position < length && unit < end;) {
    const uint32_t cp = DecodeAt(text, length, &position);
    if (cp > 0xffff) {
      if (unit >= begin && unit + 1 < end) AppendCodePoint(buffer, cp);
      unit += 2;
    } else {
      if (unit >= begin) AppendCodePoint(buffer, cp);
      ++unit;
    }
  }
  if (!buffer.ok) return nullptr;
  tj::Value* v = tj::Str(arena, "");
  if (!v) return nullptr;
  v->text = out;
  v->length = static_cast<uint32_t>(buffer.used);
  return v;
}

bool IsJsWhitespace(uint32_t cp) {
  switch (cp) {
    case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x20:
    case 0xa0: case 0x1680: case 0x2028: case 0x2029: case 0x202f:
    case 0x205f: case 0x3000: case 0xfeff:
      return true;
    default:
      return cp >= 0x2000 && cp <= 0x200a;
  }
}

void Trim(const char* text, size_t length, size_t* begin, size_t* end) {
  size_t first = 0;
  size_t last_end = 0;
  bool any = false;
  for (size_t position = 0; position < length;) {
    const size_t start = position;
    const uint32_t cp = DecodeAt(text, length, &position);
    if (IsJsWhitespace(cp)) continue;
    if (!any) first = start;
    any = true;
    last_end = position;
  }
  *begin = any ? first : 0;
  *end = any ? last_end : 0;
}

tj::Value* SanitizeSymbol(tj::Arena& arena, const char* text, size_t length) {
  return sanitize(arena, text, length, Mode::kSymbol);
}
tj::Value* SanitizeName(tj::Arena& arena, const char* text, size_t length) {
  return sanitize(arena, text, length, Mode::kName);
}
tj::Value* SanitizeText(tj::Arena& arena, const char* text, size_t length) {
  return sanitize(arena, text, length, Mode::kText);
}

void ToFixed(double value, int digits, char* output, size_t capacity) {
  if (!capacity) return;
  output[0] = '\0';
  if (!isfinite(value)) {
    snprintf(output, capacity, "%s", isnan(value) ? "NaN" : value > 0 ? "Infinity" : "-Infinity");
    return;
  }
  if (digits < 0) digits = 0;
  if (digits > 20) digits = 20;
  if (fabs(value) >= 1e21) {
    uint8_t buffer[64];
    OutBuffer out{buffer, sizeof(buffer) - 1, 0, true};
    codec::history_batch::AppendJsNumber(out, value);
    buffer[out.used] = 0;
    snprintf(output, capacity, "%s", reinterpret_cast<char*>(buffer));
    return;
  }
  const bool negative = value < 0;
  const double magnitude = negative ? -value : value;
  // The exact decimal expansion of the double (at most 1074 fraction digits).
  static constexpr int kPrecision = 1100;
  char exact[kPrecision + 64];
  snprintf(exact, sizeof(exact), "%.*f", kPrecision, magnitude);
  char* dot = strchr(exact, '.');
  if (!dot) dot = exact + strlen(exact);
  // n = integer part followed by `digits` fraction digits, rounded on the
  // first dropped digit (ties go to the larger n).
  char n[64];
  size_t k = 0;
  for (const char* p = exact; p < dot && k < sizeof(n) - 2; ++p) n[k++] = *p;
  for (int i = 0; i < digits && k < sizeof(n) - 2; ++i) n[k++] = dot[0] && dot[1 + i] ? dot[1 + i] : '0';
  n[k] = '\0';
  const char dropped = dot[0] && dot[1 + digits] ? dot[1 + digits] : '0';
  if (dropped >= '5') {
    size_t i = k;
    while (i > 0) {
      --i;
      if (n[i] == '9') { n[i] = '0'; continue; }
      ++n[i];
      break;
    }
    if (i == 0 && n[0] == '0') {
      // carried out of the leading digit
      bool all_zero = true;
      for (size_t j = 0; j < k; ++j) if (n[j] != '0') { all_zero = false; break; }
      if (all_zero) { memmove(n + 1, n, k + 1); n[0] = '1'; ++k; }
    }
  }
  // strip leading zeros of the integer part, keeping at least digits+1 chars
  size_t start = 0;
  while (start + 1 < k - static_cast<size_t>(digits) && n[start] == '0') ++start;
  const char* m = n + start;
  const size_t m_length = k - start;
  OutBuffer out{reinterpret_cast<uint8_t*>(output), capacity - 1, 0, true};
  bool zero = true;
  for (size_t i = 0; i < m_length; ++i) if (m[i] != '0') { zero = false; break; }
  if (negative && !zero) out.Byte('-');
  else if (negative && zero) out.Byte('-');  // (-0.001).toFixed(2) === "-0.00"
  if (digits == 0) {
    out.Append(m, m_length);
  } else {
    const size_t integer = m_length - static_cast<size_t>(digits);
    out.Append(m, integer);
    out.Byte('.');
    out.Append(m + integer, static_cast<size_t>(digits));
  }
  output[out.used] = '\0';
}

tj::Value* ToUpperCase(tj::Arena& arena, const char* text, size_t length) {
  return map_case(arena, text, length, true);
}
tj::Value* ToLowerCase(tj::Arena& arena, const char* text, size_t length) {
  return map_case(arena, text, length, false);
}

double ToNumber(const char* text, size_t length) {
  if (!codec::utf8::Valid(reinterpret_cast<const uint8_t*>(text), length)) return NAN;
  char scratch[512];
  if (length + 3 > sizeof(scratch)) {
    // Longer numeric literals: only whitespace padding can make them valid
    // in practice; trim and retry within the bound.
    size_t begin = 0, end = 0;
    Trim(text, length, &begin, &end);
    if (end - begin + 3 > sizeof(scratch)) return NAN;
    return codec::history_batch::JsStringToNumber(text + begin, end - begin, scratch, sizeof(scratch));
  }
  return codec::history_batch::JsStringToNumber(text, length, scratch, sizeof(scratch));
}

bool IsoString(int64_t ms, char output[32]) {
  if (ms < 0 || ms > 253402300799999LL) return false;
  int64_t days = ms / 86400000;
  int64_t rem = ms % 86400000;
  const int hour = static_cast<int>(rem / 3600000);
  const int minute = static_cast<int>(rem / 60000 % 60);
  const int second = static_cast<int>(rem / 1000 % 60);
  const int milli = static_cast<int>(rem % 1000);
  // civil_from_days (Howard Hinnant)
  days += 719468;
  const int64_t era = days / 146097;
  const int64_t doe = days - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t year = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  const int month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  if (month <= 2) ++year;
  char formatted[128];
  const int written = snprintf(formatted, sizeof(formatted), "%04lld-%02d-%02dT%02d:%02d:%02d.%03dZ", static_cast<long long>(year), month,
           day, hour, minute, second, milli);
  if (written < 0 || written >= 32) return false;
  memcpy(output, formatted, static_cast<size_t>(written) + 1);
  return true;
}

bool DateParse(const char* text, size_t length, double* epoch_ms) {
  uint16_t units[1024];
  size_t count = 0;
  for (size_t position = 0; position < length;) {
    const uint32_t cp = DecodeAt(text, length, &position);
    if (cp > 0xffff) {
      if (count + 2 > sizeof(units) / sizeof(units[0])) return false;
      units[count++] = static_cast<uint16_t>(0xd800 + ((cp - 0x10000) >> 10));
      units[count++] = static_cast<uint16_t>(0xdc00 + ((cp - 0x10000) & 0x3ff));
    } else {
      if (count + 1 > sizeof(units) / sizeof(units[0])) return false;
      units[count++] = static_cast<uint16_t>(cp);
    }
  }
  return compat::JsDateParse(units, count, epoch_ms);
}

}  // namespace fortis::codec::provider_text
