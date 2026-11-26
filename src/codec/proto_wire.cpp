#include "proto_wire.h"
#include "utf8.h"

namespace fortis::codec::proto_wire {
namespace {
// fromBinary enters the message at depth 1 with recursionLimit 100.
constexpr int kSkipRecursion = 99;

bool skip(const uint8_t* body, size_t length, size_t* position, uint32_t wire,
          uint32_t field, int recursion_limit) {
  if (wire == 0) {
    uint64_t ignored = 0;
    return ReadVarint64(body, length, position, &ignored);
  }
  if (wire == 1 || wire == 5) {
    const size_t size = wire == 1 ? 8 : 4;
    if (length - *position < size) return false;
    *position += size;
    return true;
  }
  if (wire == 2) {
    const uint8_t* ignored = nullptr;
    size_t ignored_length = 0;
    return ReadBytes(body, length, position, &ignored, &ignored_length);
  }
  if (wire != 3 || recursion_limit <= 0) return false;
  for (;;) {
    uint32_t nested_field = 0, nested_wire = 0;
    if (!ReadTag(body, length, position, &nested_field, &nested_wire)) return false;
    if (nested_wire == 4) return nested_field == field;
    if (!skip(body, length, position, nested_wire, nested_field, recursion_limit - 1))
      return false;
  }
}
}  // namespace

bool ReadVarint32(const uint8_t* body, size_t length, size_t* position, uint32_t* value) {
  uint64_t result = 0;
  if (!ReadVarint64(body, length, position, &result) || result > UINT32_MAX) return false;
  *value = static_cast<uint32_t>(result);
  return true;
}

bool ReadVarint64(const uint8_t* body, size_t length, size_t* position, uint64_t* value) {
  if (*position > length || (!body && length)) return false;
  uint64_t result = 0;
  for (unsigned i = 0; i < 10; ++i) {
    if (*position >= length) return false;
    const uint8_t b = body[(*position)++];
    if (i == 9 && b > 1) return false;
    result |= static_cast<uint64_t>(b & 0x7f) << (7 * i);
    if (!(b & 0x80)) { *value = result; return true; }
  }
  return false;
}

bool ReadTag(const uint8_t* body, size_t length, size_t* position, uint32_t* field,
             uint32_t* wire) {
  const size_t start = *position;
  uint32_t tag = 0;
  if (!ReadVarint32(body, length, position, &tag)) return false;
  const size_t read = *position - start;
  if (!body || read > 5 || (read == 5 && body[*position - 1] > 0x0f)) return false;
  *field = tag >> 3;
  *wire = tag & 7;
  return *field > 0 && *wire <= 5;
}

bool ReadBytes(const uint8_t* body, size_t length, size_t* position, const uint8_t** data,
               size_t* data_length) {
  if (*position > length) return false;
  uint32_t size = 0;
  if (!ReadVarint32(body, length, position, &size) || size > length - *position) return false;
  *data = body + *position;
  *data_length = size;
  *position += size;
  return true;
}

bool ReadFixed64(const uint8_t* body, size_t length, size_t* position, uint64_t* value) {
  if (*position > length || length - *position < 8) return false;
  uint64_t result = 0;
  for (int i = 7; i >= 0; --i) result = result << 8 | body[*position + static_cast<size_t>(i)];
  *position += 8;
  *value = result;
  return true;
}

bool SkipField(const uint8_t* body, size_t length, size_t* position, uint32_t wire,
               uint32_t field, int recursion_limit) {
  if (*position > length || (!body && length)) return false;
  return skip(body, length, position, wire, field,
              recursion_limit < kSkipRecursion ? recursion_limit : kSkipRecursion);
}

bool Reader::Tag(uint32_t* field, uint32_t* wire) {
  if (!ReadTag(data_, limit_, &position_, field, wire)) return false;
  wire_ = *wire;
  return true;
}
bool Reader::Skip(uint32_t wire, uint32_t field, int limit) {
  return SkipField(data_, limit_, &position_, wire, field, limit);
}
bool Reader::Uint32(uint32_t* value) {
  return wire_ == kWireVarint && ReadVarint32(data_, limit_, &position_, value);
}
bool Reader::Int32(int32_t* value) {
  uint64_t raw = 0;
  if (wire_ != kWireVarint || !ReadVarint64(data_, limit_, &position_, &raw) ||
      (raw > INT32_MAX && raw < UINT64_C(0xffffffff80000000))) return false;
  if (value) *value = static_cast<int32_t>(raw);
  return true;
}
bool Reader::Bool(bool* value) {
  uint64_t raw = 0;
  if (wire_ != kWireVarint || !ReadVarint64(data_, limit_, &position_, &raw)) return false;
  *value = raw != 0;
  return true;
}
bool Reader::Double(double* value) {
  uint64_t raw = 0;
  if (wire_ != kWireFixed64 || !ReadFixed64(data_, limit_, &position_, &raw)) return false;
  memcpy(value, &raw, sizeof(raw));
  return true;
}
bool Reader::String(const uint8_t** text, size_t* length) {
  return wire_ == kWireLengthDelimited && ReadBytes(data_, limit_, &position_, text, length) &&
      utf8::Valid(*text, *length);
}
bool Reader::MessageLength(uint32_t* length) {
  return wire_ == kWireLengthDelimited && ReadVarint32(data_, limit_, &position_, length) &&
      *length <= limit_ - position_;
}

}  // namespace fortis::codec::proto_wire
