#include "utf8.h"
#include "price_history_batch_codec.h"

#include "proto_wire.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fortis::codec::history_batch {
namespace {

// Decodes one code point from valid UTF-8.
uint32_t next_code_point(const unsigned char* text, size_t length, size_t* position) {
  const unsigned char c = text[(*position)++];
  if (c < 0x80) return c;
  const unsigned trailing = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : 3;
  uint32_t value = c & (trailing == 1 ? 0x1f : trailing == 2 ? 0x0f : 0x07);
  for (unsigned i = 0; i < trailing && *position < length; ++i)
    value = (value << 6) | (text[(*position)++] & 0x3f);
  return value;
}

bool js_whitespace(uint32_t cp) {
  switch (cp) {
    case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x20:
    case 0xa0: case 0x1680: case 0x2028: case 0x2029: case 0x202f:
    case 0x205f: case 0x3000: case 0xfeff:
      return true;
    default:
      return cp >= 0x2000 && cp <= 0x200a;
  }
}

int digit_value(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 99;
}

// Number("0x…"), Number("0o…") and Number("0b…"): the exact integer, rounded
// once to the nearest double. Digits are regrouped as hexadecimal so strtod
// (correctly rounded for hex input) performs that single rounding.
double radix_integer(const char* digits, size_t count, unsigned bits_per_digit,
                     char* scratch, size_t capacity) {
  size_t first = 0;
  while (first < count && digits[first] == '0') ++first;
  if (first == count) return 0;
  const size_t bits = (count - first) * bits_per_digit;
  // Anything above 2^1024 is Infinity in JavaScript as well.
  if (bits > 4 * 1100) return INFINITY;
  const size_t hex_digits = (bits + 3) / 4;
  if (hex_digits + 3 > capacity) return NAN;
  scratch[0] = '0'; scratch[1] = 'x';
  // Fill hexadecimal digits from the least significant end.
  unsigned accumulator = 0, accumulated = 0;
  size_t out = hex_digits;
  for (size_t i = count; i > first; --i) {
    accumulator |= static_cast<unsigned>(digit_value(static_cast<unsigned char>(digits[i - 1])))
        << accumulated;
    accumulated += bits_per_digit;
    while (accumulated >= 4) {
      scratch[2 + --out] = "0123456789abcdef"[accumulator & 15];
      accumulator >>= 4; accumulated -= 4;
    }
  }
  while (out > 0) {
    scratch[2 + --out] = "0123456789abcdef"[accumulator & 15];
    accumulator >>= 4;
  }
  scratch[2 + hex_digits] = '\0';
  return strtod(scratch, nullptr);
}

bool decimal_literal(const char* text, size_t length) {
  size_t i = 0;
  if (i < length && (text[i] == '+' || text[i] == '-')) ++i;
  size_t integer_digits = 0, fraction_digits = 0;
  while (i < length && text[i] >= '0' && text[i] <= '9') { ++i; ++integer_digits; }
  if (i < length && text[i] == '.') {
    ++i;
    while (i < length && text[i] >= '0' && text[i] <= '9') { ++i; ++fraction_digits; }
  }
  if (!integer_digits && !fraction_digits) return false;
  if (i < length && (text[i] == 'e' || text[i] == 'E')) {
    ++i;
    if (i < length && (text[i] == '+' || text[i] == '-')) ++i;
    size_t exponent_digits = 0;
    while (i < length && text[i] >= '0' && text[i] <= '9') { ++i; ++exponent_digits; }
    if (!exponent_digits) return false;
  }
  return i == length;
}

// Reads one wire record at *position. Strings (fields 1 and 4) are returned
// through data/data_length; integers (fields 2 and 3) through integer.
bool read_record(const uint8_t* body, size_t length, size_t* position,
                 uint32_t* field, const uint8_t** data, size_t* data_length,
                 int64_t* integer) {
  uint32_t wire = 0;
  if (!proto_wire::ReadTag(body, length, position, field, &wire)) return false;
  if (*field == 1 || *field == 4) {
    return wire == 2 && proto_wire::ReadBytes(body, length, position, data, data_length) &&
        ValidUtf8(*data, *data_length);
  }
  if (*field == 2 || *field == 3) {
    if (wire != 0) return false;
    uint64_t value = 0;
    if (!proto_wire::ReadVarint64(body, length, position, &value)) return false;
    *integer = static_cast<int64_t>(value);
    return true;
  }
  return proto_wire::SkipField(body, length, position, wire, *field);
}

}  // namespace

void OutBuffer::Append(const void* bytes, size_t length) {
  if (!ok) return;
  if (!data) { used += length; return; }
  if (length > capacity - used) { ok = false; return; }
  if (length) memcpy(data + used, bytes, length);
  used += length;
}

void OutBuffer::Byte(uint8_t value) { Append(&value, 1); }

void OutBuffer::Text(const char* text) { Append(text, strlen(text)); }

bool ValidUtf8(const uint8_t* bytes, size_t length) {
  return utf8::Valid(bytes, length);
}

void JsTrim(const char* text, size_t length, size_t* begin, size_t* end) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(text);
  size_t start = 0;
  while (start < length) {
    size_t next = start;
    if (!js_whitespace(next_code_point(bytes, length, &next))) break;
    start = next;
  }
  size_t stop = length;
  while (stop > start) {
    size_t lead = stop - 1;
    while (lead > start && (bytes[lead] & 0xc0) == 0x80) --lead;
    size_t probe = lead;
    if (!js_whitespace(next_code_point(bytes, length, &probe))) break;
    stop = lead;
  }
  *begin = start; *end = stop;
}

size_t Utf16Units(const char* text, size_t length) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(text);
  size_t units = 0, position = 0;
  while (position < length)
    units += next_code_point(bytes, length, &position) > 0xffff ? 2 : 1;
  return units;
}

double JsStringToNumber(const char* text, size_t length, char* scratch,
                        size_t scratch_capacity) {
  size_t begin = 0, end = 0;
  JsTrim(text, length, &begin, &end);
  const char* value = text + begin;
  const size_t size = end - begin;
  if (!size) return 0;
  auto equals = [&](const char* literal) {
    return strlen(literal) == size && memcmp(value, literal, size) == 0;
  };
  if (equals("Infinity") || equals("+Infinity")) return INFINITY;
  if (equals("-Infinity")) return -INFINITY;
  if (size > 2 && value[0] == '0') {
    const char marker = value[1];
    unsigned bits = 0, radix = 0;
    if (marker == 'x' || marker == 'X') { bits = 4; radix = 16; }
    else if (marker == 'o' || marker == 'O') { bits = 3; radix = 8; }
    else if (marker == 'b' || marker == 'B') { bits = 1; radix = 2; }
    if (radix) {
      for (size_t i = 2; i < size; ++i)
        if (digit_value(static_cast<unsigned char>(value[i])) >= static_cast<int>(radix))
          return NAN;
      return radix_integer(value + 2, size - 2, bits, scratch, scratch_capacity);
    }
  }
  if (!decimal_literal(value, size)) return NAN;
  if (size + 1 > scratch_capacity) return NAN;
  memcpy(scratch, value, size);
  scratch[size] = '\0';
  return strtod(scratch, nullptr);
}

bool JsArrayIndexKey(const char* text, size_t length, uint32_t* index) {
  if (!length || length > 10 || (length > 1 && text[0] == '0')) return false;
  uint64_t value = 0;
  for (size_t i = 0; i < length; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    value = value * 10 + static_cast<uint64_t>(text[i] - '0');
  }
  if (value > 4294967294ULL) return false;
  if (index) *index = static_cast<uint32_t>(value);
  return true;
}

void AppendJsonStringBody(OutBuffer& out, const char* text, size_t length) {
  size_t run = 0;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    const char* escaped = nullptr; char unicode[7];
    switch (c) {
      case '"': escaped = "\\\""; break;
      case '\\': escaped = "\\\\"; break;
      case '\b': escaped = "\\b"; break;
      case '\f': escaped = "\\f"; break;
      case '\n': escaped = "\\n"; break;
      case '\r': escaped = "\\r"; break;
      case '\t': escaped = "\\t"; break;
      default:
        if (c < 0x20) {
          snprintf(unicode, sizeof(unicode), "\\u%04x", c);
          escaped = unicode;
        }
    }
    if (!escaped) continue;
    out.Append(text + run, i - run);
    out.Text(escaped);
    run = i + 1;
  }
  out.Append(text + run, length - run);
}

void AppendJsonString(OutBuffer& out, const char* text, size_t length) {
  out.Byte('"');
  AppendJsonStringBody(out, text, length);
  out.Byte('"');
}

// The shortest round-trip significant digits of a finite nonzero double, in
// printf's d[.ddd]e±xx form. The guest has no <charconv>, so this searches
// the correctly rounded %.*e precisions instead of calling std::to_chars.
// Round-tripping is monotonic in precision (the p-digit rounding is also a
// (p+1)-digit decimal), so a binary search over 1..17 digits finds the
// shortest; 17 digits always round-trip. For the same digit count this is the
// closest decimal, as std::to_chars chooses. The two can differ only when the
// significand is an exact power of two, whose rounding interval is narrower
// below: std::to_chars may then find a shorter value above, which this search
// skips in favour of one more digit.
static bool shortest_scientific(double value, char* out, size_t capacity) {
  int low = 1, high = 17;
  while (low < high) {
    const int middle = (low + high) / 2;
    const int count = snprintf(out, capacity, "%.*e", middle - 1, value);
    if (count <= 0 || static_cast<size_t>(count) >= capacity) return false;
    char* end = nullptr;
    const double parsed = strtod(out, &end);
    if (end == out + count && parsed == value) high = middle;
    else low = middle + 1;
  }
  const int count = snprintf(out, capacity, "%.*e", low - 1, value);
  return count > 0 && static_cast<size_t>(count) < capacity;
}

void AppendJsNumber(OutBuffer& out, double value) {
  if (!isfinite(value)) { out.Text("null"); return; }
  if (value == 0) { out.Byte('0'); return; }
  char scientific[64];
  if (!shortest_scientific(value, scientific, sizeof(scientific))) { out.ok = false; return; }
  // Shortest round-trip digits d[.ddd]e±x, reformatted by Number::toString.
  // Only digits are kept, whatever the locale's decimal separator.
  const bool negative = scientific[0] == '-';
  const char* cursor = scientific + (negative ? 1 : 0);
  char digits[32]{}; size_t k = 0;
  while (*cursor && *cursor != 'e') {
    if (*cursor >= '0' && *cursor <= '9' && k < sizeof(digits)) digits[k++] = *cursor;
    ++cursor;
  }
  const long exponent = *cursor == 'e' ? strtol(cursor + 1, nullptr, 10) : 0;
  const long n = exponent + 1;
  if (negative) out.Byte('-');
  if (static_cast<long>(k) <= n && n <= 21) {
    out.Append(digits, k);
    for (long i = static_cast<long>(k); i < n; ++i) out.Byte('0');
  } else if (0 < n && n <= 21) {
    out.Append(digits, static_cast<size_t>(n));
    out.Byte('.');
    out.Append(digits + n, k - static_cast<size_t>(n));
  } else if (-6 < n && n <= 0) {
    out.Text("0.");
    for (long i = 0; i < -n; ++i) out.Byte('0');
    out.Append(digits, k);
  } else {
    out.Byte(static_cast<uint8_t>(digits[0]));
    if (k > 1) { out.Byte('.'); out.Append(digits + 1, k - 1); }
    char tail[32];
    snprintf(tail, sizeof(tail), "e%c%ld", n - 1 >= 0 ? '+' : '-', labs(n - 1));
    out.Text(tail);
  }
}

void AppendInteger(OutBuffer& out, int64_t value) {
  char text[32];
  snprintf(text, sizeof(text), "%lld", static_cast<long long>(value));
  out.Text(text);
}

bool DecodeBatchProtoRequest(const uint8_t* body, size_t length,
                             BatchProtoRequest* request) {
  if (!request || (!body && length)) return false;
  *request = BatchProtoRequest{};
  size_t position = 0;
  while (position < length) {
    uint32_t field = 0; const uint8_t* data = nullptr; size_t data_length = 0;
    int64_t integer = 0;
    if (!read_record(body, length, &position, &field, &data, &data_length, &integer))
      return false;
    if (field == 1) {
      ++request->id_count;
    } else if (field == 4) {
      // Optional scalar: the last occurrence wins.
      request->has_interval = true;
      request->interval = data; request->interval_length = data_length;
    } else if (field == 2) {
      request->from = integer;
    } else if (field == 3) {
      request->to = integer;
    }
  }
  return true;
}

bool NextBatchProtoId(const uint8_t* body, size_t length, size_t* cursor,
                      const uint8_t** id, size_t* id_length) {
  if (!cursor || !id || !id_length || (!body && length)) return false;
  // `cursor` is a byte offset at a record boundary of an accepted body.
  while (*cursor < length) {
    uint32_t field = 0; const uint8_t* data = nullptr; size_t data_length = 0;
    int64_t integer = 0;
    if (!read_record(body, length, cursor, &field, &data, &data_length, &integer))
      return false;
    if (field == 1) { *id = data; *id_length = data_length; return true; }
  }
  return false;
}

size_t BatchWorstResponseBytes(size_t ids, size_t candles, bool protobuf) {
  return kBatchWorstEnvelopeBytes + ids * (kBatchWorstBytesPerEntry + candles *
      (protobuf ? kBatchWorstProtoBytesPerCandle : kBatchWorstJsonBytesPerCandle));
}

}  // namespace fortis::codec::history_batch
