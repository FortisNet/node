#include "js_case.h"

#include <string.h>

namespace fortis::compat {
namespace {

// One strict UTF-8 scalar (no overlongs, surrogates or > U+10FFFF).
bool decode(const unsigned char* input, size_t length, size_t* position,
            uint32_t* code_point) {
  const unsigned char lead = input[*position];
  if (lead < 0x80) { *code_point = lead; ++*position; return true; }
  const size_t trailing = (lead & 0xe0) == 0xc0 ? 1 : (lead & 0xf0) == 0xe0 ? 2 :
                          (lead & 0xf8) == 0xf0 ? 3 : 0;
  if (!trailing || length - *position <= trailing) return false;
  uint32_t value = lead & (trailing == 1 ? 0x1f : trailing == 2 ? 0x0f : 0x07);
  for (size_t i = 1; i <= trailing; ++i) {
    const unsigned char next = input[*position + i];
    if ((next & 0xc0) != 0x80) return false;
    value = (value << 6) | (next & 0x3f);
  }
  if ((trailing == 1 && value < 0x80) || (trailing == 2 && value < 0x800) ||
      (trailing == 3 && value < 0x10000) || value > 0x10ffff ||
      (value >= 0xd800 && value <= 0xdfff)) return false;
  *position += trailing + 1;
  *code_point = value;
  return true;
}

// The scalar ending just before `end`, which the caller knows is valid UTF-8.
uint32_t decode_before(const unsigned char* input, size_t* end) {
  size_t start = *end - 1;
  while (start > 0 && (input[start] & 0xc0) == 0x80) --start;
  size_t position = start;
  uint32_t value = 0;
  decode(input, *end, &position, &value);
  *end = start;
  return value;
}

bool append(uint32_t value, char* output, size_t capacity, size_t* used) {
  unsigned char bytes[4];
  size_t count = 0;
  if (value < 0x80) { bytes[0] = static_cast<unsigned char>(value); count = 1; }
  else if (value < 0x800) {
    bytes[0] = static_cast<unsigned char>(0xc0 | (value >> 6));
    bytes[1] = static_cast<unsigned char>(0x80 | (value & 0x3f)); count = 2;
  } else if (value < 0x10000) {
    bytes[0] = static_cast<unsigned char>(0xe0 | (value >> 12));
    bytes[1] = static_cast<unsigned char>(0x80 | ((value >> 6) & 0x3f));
    bytes[2] = static_cast<unsigned char>(0x80 | (value & 0x3f)); count = 3;
  } else {
    bytes[0] = static_cast<unsigned char>(0xf0 | (value >> 18));
    bytes[1] = static_cast<unsigned char>(0x80 | ((value >> 12) & 0x3f));
    bytes[2] = static_cast<unsigned char>(0x80 | ((value >> 6) & 0x3f));
    bytes[3] = static_cast<unsigned char>(0x80 | (value & 0x3f)); count = 4;
  }
  if (capacity - *used <= count) return false;  // keep room for the NUL
  memcpy(output + *used, bytes, count);
  *used += count;
  return true;
}

const JsCaseMapping* find(const JsCaseMapping* table, size_t count, uint32_t value) {
  size_t low = 0, high = count;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    if (table[middle].code_point < value) low = middle + 1;
    else high = middle;
  }
  return low < count && table[low].code_point == value ? &table[low] : nullptr;
}

bool in_ranges(const JsCodePointRange* table, size_t count, uint32_t value) {
  size_t low = 0, high = count;
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    if (table[middle].last < value) low = middle + 1;
    else high = middle;
  }
  return low < count && table[low].first <= value;
}

bool ignorable(uint32_t value) {
  return in_ranges(js_case_tables::kCaseIgnorable, js_case_tables::kCaseIgnorableCount, value);
}
bool cased(uint32_t value) {
  return in_ranges(js_case_tables::kCased, js_case_tables::kCasedCount, value);
}

// Unicode Final_Sigma as ICU applies it: a case-ignorable character is
// skipped before the Cased test, in both directions.
bool final_sigma(const unsigned char* input, size_t length, size_t start, size_t end) {
  bool preceded = false;
  for (size_t cursor = start; cursor > 0;) {
    const uint32_t value = decode_before(input, &cursor);
    if (ignorable(value)) continue;
    preceded = cased(value);
    break;
  }
  if (!preceded) return false;
  for (size_t cursor = end; cursor < length;) {
    uint32_t value = 0;
    decode(input, length, &cursor, &value);
    if (ignorable(value)) continue;
    return !cased(value);
  }
  return true;
}

bool map(const char* raw, size_t length, char* output, size_t capacity,
         size_t* output_length, bool upper) {
  if (!output || !capacity || (!raw && length)) return false;
  const auto* input = reinterpret_cast<const unsigned char*>(raw);
  const JsCaseMapping* table = upper ? js_case_tables::kUpper : js_case_tables::kLower;
  const size_t count = upper ? js_case_tables::kUpperCount : js_case_tables::kLowerCount;
  size_t position = 0, used = 0;
  while (position < length) {
    const size_t start = position;
    uint32_t value = 0;
    if (!decode(input, length, &position, &value)) return false;
    if (!upper && value == 0x03a3 && final_sigma(input, length, start, position)) {
      if (!append(0x03c2, output, capacity, &used)) return false;
      continue;
    }
    const JsCaseMapping* mapping = find(table, count, value);
    if (!mapping) {
      if (!append(value, output, capacity, &used)) return false;
      continue;
    }
    for (uint8_t i = 0; i < mapping->count; ++i)
      if (!append(mapping->output[i], output, capacity, &used)) return false;
  }
  output[used] = '\0';
  if (output_length) *output_length = used;
  return true;
}

}  // namespace

bool JsToUpperCase(const char* input, size_t length, char* output,
                   size_t capacity, size_t* output_length) {
  return map(input, length, output, capacity, output_length, true);
}

bool JsToLowerCase(const char* input, size_t length, char* output,
                   size_t capacity, size_t* output_length) {
  return map(input, length, output, capacity, output_length, false);
}

namespace {

bool ascii_case(const char* input, size_t length, char* output, size_t capacity,
                size_t* output_length, bool upper) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(input);
  for (size_t position = 0; position < length;) {
    uint32_t ignored = 0;
    if (!decode(bytes, length, &position, &ignored)) return false;
  }
  if (capacity <= length) return false;  // keep room for the NUL
  for (size_t i = 0; i < length; ++i) {
    const char c = input[i];
    output[i] = upper ? (c >= 'a' && c <= 'z' ? static_cast<char>(c - 0x20) : c)
                      : (c >= 'A' && c <= 'Z' ? static_cast<char>(c + 0x20) : c);
  }
  output[length] = '\0';
  if (output_length) *output_length = length;
  return true;
}

}  // namespace

bool AsciiToUpperCase(const char* input, size_t length, char* output, size_t capacity,
                      size_t* output_length) {
  return ascii_case(input, length, output, capacity, output_length, true);
}

bool AsciiToLowerCase(const char* input, size_t length, char* output, size_t capacity,
                      size_t* output_length) {
  return ascii_case(input, length, output, capacity, output_length, false);
}

}  // namespace fortis::compat
