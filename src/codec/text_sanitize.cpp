#include "text_sanitize.h"

#include <stdio.h>
#include <string.h>

namespace fortis::codec {

namespace {

using fortis::codec::history_batch::OutBuffer;

// Decodes one code point from valid UTF-8.
uint32_t decode(const unsigned char* text, size_t length, size_t* position) {
  const unsigned char c = text[(*position)++];
  if (c < 0x80) return c;
  const unsigned trailing = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : 3;
  uint32_t value = c & (trailing == 1 ? 0x1f : trailing == 2 ? 0x0f : 0x07);
  for (unsigned i = 0; i < trailing && *position < length; ++i)
    value = (value << 6) | (text[(*position)++] & 0x3f);
  return value;
}

// Controls U+0000..0008, U+000B..001F, U+007F..009F, U+200B..200F,
// U+202A..202E, U+2060..2064, U+FEFF and backticks.
bool removed(uint32_t cp) {
  return cp <= 0x08 || (cp >= 0x0b && cp <= 0x1f) || (cp >= 0x7f && cp <= 0x9f) ||
         (cp >= 0x200b && cp <= 0x200f) || (cp >= 0x202a && cp <= 0x202e) ||
         (cp >= 0x2060 && cp <= 0x2064) || cp == 0xfeff || cp == '`';
}

// ECMAScript WhiteSpace + LineTerminator, which is also the set of \s.
bool whitespace(uint32_t cp) {
  switch (cp) {
    case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x20:
    case 0xa0: case 0x1680: case 0x2028: case 0x2029: case 0x202f:
    case 0x205f: case 0x3000: case 0xfeff:
      return true;
    default:
      return cp >= 0x2000 && cp <= 0x200a;
  }
}

// Generalized UTF-8 (a surrogate code point encodes like any BMP value).
size_t encode(uint32_t cp, unsigned char out[4]) {
  if (cp < 0x80) { out[0] = static_cast<unsigned char>(cp); return 1; }
  if (cp < 0x800) {
    out[0] = static_cast<unsigned char>(0xc0 | (cp >> 6));
    out[1] = static_cast<unsigned char>(0x80 | (cp & 63));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<unsigned char>(0xe0 | (cp >> 12));
    out[1] = static_cast<unsigned char>(0x80 | ((cp >> 6) & 63));
    out[2] = static_cast<unsigned char>(0x80 | (cp & 63));
    return 3;
  }
  out[0] = static_cast<unsigned char>(0xf0 | (cp >> 18));
  out[1] = static_cast<unsigned char>(0x80 | ((cp >> 12) & 63));
  out[2] = static_cast<unsigned char>(0x80 | ((cp >> 6) & 63));
  out[3] = static_cast<unsigned char>(0x80 | (cp & 63));
  return 4;
}

size_t sanitize(const char* input, size_t length, char* output, size_t capacity,
                size_t max_units) {
  if ((!input && length) || !output || !capacity ||
      !fortis::codec::history_batch::ValidUtf8(reinterpret_cast<const uint8_t*>(input), length))
    return kSanitizeFailed;
  const auto* bytes = reinterpret_cast<const unsigned char*>(input);
  // Pass 1: the UTF-16 length after removal, trim and whitespace collapse.
  // Pass 2 writes the same sequence, truncated when it is too long. Both
  // passes walk the code points of the cleaned string in order:
  //   kept[i] = not removed; trimmed between the first and last non-space;
  //   a whitespace run inside becomes one ' '.
  size_t first = 0, last = 0;  // byte offsets of the first/last kept non-space
  bool any = false;
  for (size_t position = 0; position < length;) {
    const size_t start = position;
    const uint32_t cp = decode(bytes, length, &position);
    if (removed(cp) || whitespace(cp)) continue;
    if (!any) first = start;
    any = true;
    last = position;
  }
  if (!any) { output[0] = '\0'; return 0; }
  auto walk = [&](auto&& emit) {
    bool pending_space = false;
    for (size_t position = first; position < last;) {
      const uint32_t cp = decode(bytes, length, &position);
      if (removed(cp)) continue;
      if (whitespace(cp)) { pending_space = true; continue; }
      if (pending_space) { if (!emit(0x20u)) return; pending_space = false; }
      if (!emit(cp)) return;
    }
  };
  size_t units = 0;
  walk([&](uint32_t cp) { units += cp > 0xffff ? 2 : 1; return true; });
  const bool truncate = units > max_units;
  const size_t keep = truncate ? max_units - 1 : units;
  size_t used = 0, written_units = 0;
  bool ok = true;
  auto put = [&](uint32_t cp) {
    unsigned char encoded[4];
    const size_t n = encode(cp, encoded);
    if (used + n >= capacity) { ok = false; return false; }
    memcpy(output + used, encoded, n);
    used += n;
    return true;
  };
  walk([&](uint32_t cp) {
    const size_t width = cp > 0xffff ? 2 : 1;
    if (written_units + width > keep) {
      return false;
    }
    if (!put(cp)) return false;
    written_units += width;
    return true;
  });
  if (ok && truncate) put(0x2026);
  if (!ok) return kSanitizeFailed;
  output[used] = '\0';
  return used;
}

}  // namespace

size_t SanitizeSymbol(const char* input, size_t length, char* output, size_t capacity) {
  return sanitize(input, length, output, capacity, kSanitizeSymbolMaxUnits);
}

size_t SanitizeName(const char* input, size_t length, char* output, size_t capacity) {
  return sanitize(input, length, output, capacity, kSanitizeNameMaxUnits);
}

void AppendSanitizedJsonString(OutBuffer& out, const char* text, size_t length) {
  if (!codec::history_batch::ValidUtf8(reinterpret_cast<const uint8_t*>(text), length)) { out.ok = false; return; }
  codec::history_batch::AppendJsonString(out, text, length);
}

size_t SanitizedToProtobufUtf8(const char* text, size_t length, char* output, size_t capacity) {
  if (length > capacity || !codec::history_batch::ValidUtf8(reinterpret_cast<const uint8_t*>(text), length))
    return kSanitizeFailed;
  if (length) memcpy(output, text, length);
  return length;
}

}  // namespace fortis::codec
