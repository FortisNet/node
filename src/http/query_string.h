#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <charconv>
#include <optional>
#include <system_error>

// Allocation-free request query parsing. Components are UTF-8 bytes, with
// '+' decoded as space and %XX decoded exactly once. Every pair is validated,
// including unknown parameters and later occurrences of a repeated key.
namespace fortis::http::query_string {

// Parse an already decoded request value; never interpret percent escapes
// twice. JSON numeric values must be type-checked by their JSON decoder.
inline bool ParseUnsigned(const char* text, size_t length, uint64_t maximum, uint64_t* value) {
  if (!length) return false;
  uint64_t number = 0;
  const auto [end, error] = std::from_chars(text, text + length, number);
  if (error != std::errc{} || end != text + length || number > maximum) return false;
  *value = number;
  return true;
}

struct Component {
  const char* data = nullptr;
  size_t size = 0;

  // Returns the full decoded length, or SIZE_MAX for invalid encoding.
  // A short output buffer receives a prefix; no terminator is written.
  size_t Decode(char* output, size_t capacity) const {
    size_t used = 0;
    unsigned need = 0;
    unsigned char low = 0x80, high = 0xbf;
    for (size_t i = 0; i < size;) {
      const auto byte = NextByte(i);
      if (!byte) return SIZE_MAX;
      const auto c = *byte;
      if (need) {
        if (c < low || c > high) return SIZE_MAX;
        --need;
        low = 0x80; high = 0xbf;
      } else if (c >= 0x80) {
        if (c >= 0xc2 && c <= 0xdf) need = 1;
        else if (c >= 0xe0 && c <= 0xef) {
          need = 2;
          if (c == 0xe0) low = 0xa0;
          if (c == 0xed) high = 0x9f;
        } else if (c >= 0xf0 && c <= 0xf4) {
          need = 3;
          if (c == 0xf0) low = 0x90;
          if (c == 0xf4) high = 0x8f;
        } else return SIZE_MAX;
      }
      if (used < capacity) output[used] = static_cast<char>(c);
      ++used;
    }
    return need ? SIZE_MAX : used;
  }

  using Sink = void (*)(void*, unsigned char);
  // Validate before emitting anything, so an invalid component never feeds
  // a partial value to a streaming consumer.
  size_t DecodeTo(Sink sink, void* context) const {
    const size_t length = Decode(nullptr, 0);
    if (length == SIZE_MAX) return SIZE_MAX;
    for (size_t i = 0; i < size;) {
      const auto byte = NextByte(i);
      if (!byte) return SIZE_MAX;
      sink(context, *byte);
    }
    return length;
  }

  bool Equals(const char* literal) const {
    if (Decode(nullptr, 0) != strlen(literal)) return false;
    size_t matched = 0;
    for (size_t i = 0; i < size;) {
      const auto byte = NextByte(i);
      if (!byte || *byte != static_cast<unsigned char>(literal[matched++])) return false;
    }
    return true;
  }

  // Decimal digits only, with an explicit inclusive bound. Leading zeros
  // are accepted without buffering, overflow, or floating-point rounding.
  bool Unsigned(uint64_t maximum, uint64_t* value) const {
    if (!size) return false;
    uint64_t number = 0;
    for (size_t i = 0; i < size;) {
      const auto byte = NextByte(i);
      if (!byte || *byte < '0' || *byte > '9') return false;
      const unsigned digit = *byte - '0';
      if (digit > maximum || number > (maximum - digit) / 10) return false;
      number = number * 10 + digit;
    }
    *value = number;
    return true;
  }

 private:
  // Called while position < size; consumes one literal or percent-encoded byte.
  std::optional<unsigned char> NextByte(size_t& position) const {
    const auto byte = static_cast<unsigned char>(data[position++]);
    if (byte != '%') return byte == '+' ? ' ' : byte;
    if (size - position < 2) return std::nullopt;
    const auto high = Hex(data[position++]);
    const auto low = Hex(data[position++]);
    if (high < 0 || low < 0) return std::nullopt;
    return static_cast<unsigned char>((high << 4) | low);
  }

  static int Hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }
};

class Query {
 public:
  Query(const char* data, size_t size) : data_(data), size_(size) {}

  static Query FromTarget(const char* target) {
    const char* question = target ? strchr(target, '?') : nullptr;
    return question ? Query(question + 1, strlen(question + 1)) : Query(nullptr, 0);
  }

  bool Valid() const {
    if (!data_) return size_ == 0;
    size_t cursor = 0;
    Component key, value;
    while (Next(&cursor, &key, &value))
      if (key.Decode(nullptr, 0) == SIZE_MAX || value.Decode(nullptr, 0) == SIZE_MAX) return false;
    return true;
  }

  // Call Valid() first. Unknown names are ignored; the first occurrence
  // wins even when it is empty. An absent field returns false.
  bool First(const char* name, Component* result) const {
    size_t cursor = 0;
    Component key, value;
    while (Next(&cursor, &key, &value)) {
      if (key.Equals(name)) { *result = value; return true; }
    }
    return false;
  }

 private:
  bool Next(size_t* cursor, Component* key, Component* value) const {
    if (!data_ || *cursor >= size_) return false;
    const size_t start = *cursor;
    size_t end = start;
    while (end < size_ && data_[end] != '&') ++end;
    size_t equal = start;
    while (equal < end && data_[equal] != '=') ++equal;
    *key = {data_ + start, equal - start};
    const size_t begin = equal < end ? equal + 1 : end;
    *value = {data_ + begin, end - begin};
    *cursor = end < size_ ? end + 1 : end;
    return true;
  }

  const char* data_;
  size_t size_;
};

}  // namespace fortis::http::query_string
