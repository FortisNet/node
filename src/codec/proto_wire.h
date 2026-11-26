#pragma once

#include <bit>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// The one hand-written protobuf wire codec. Every encoder and decoder in src/
// that does not go through nanopb builds on these primitives; see
// docs/codec-semantics.md for the full codec map and for which behaviours
// remain for provider compatibility. Requests use bounded typed readers;
// see compat-decisions.md#protobuf-es-reader
//
// When to use what:
//  - nanopb (pb_encode / pb_decode with the generated *_fields): fixed-shape
//    messages whose generated struct fits in bounded storage (device
//    challenge / enrollment, /v1/networks, DashPay name response).
//  - proto_wire writer (below): responses whose shape or size is dynamic
//    (google.protobuf.Struct, repeated messages streamed into a caller-owned
//    buffer) or whose bytes must equal protobuf-es toBinary exactly.
//  - proto_wire reader (below): request decoders with declared wire types, bounded
//    messages, strict UTF-8 and standard unknown-field skipping.
//
// Writer model. The writer is a set of free functions over any byte sink that
// has `void Append(const void* bytes, size_t length)`: history_batch::OutBuffer
// (which only counts when its data pointer is null, giving the measure pass of
// a two-pass length prefix), proto_wire::Span for a fixed stack buffer, and the
// few route-local growable buffers. The writer is deliberately policy-free:
//  - It never omits a field. proto3 implicit-presence omission (zero numbers,
//    empty strings, false) is the CALLER's decision, because protobuf-es
//    distinguishes implicit fields, explicit `optional` fields and oneof
//    members, and the three differ per field.
//  - It never reorders. Callers emit fields in ascending field-number order,
//    which is the order protobuf-es toBinary writes, and map entries in the
//    order documented at the call site.
//  - Length prefixes are never back-patched: a caller either encodes the
//    nested message into a Span first and copies it (small bounded messages)
//    or measures it with a counting OutBuffer and then writes it (two-pass).
//  - Repeated scalars are not packed by any current message; there is no
//    packed helper on purpose.
// Guest-safe: allocation-free; std::bit_cast is supported by the pinned libc++ 14.
namespace fortis::codec::proto_wire {

// ---- writer -------------------------------------------------------------------

constexpr uint32_t kWireVarint = 0;
constexpr uint32_t kWireFixed64 = 1;
constexpr uint32_t kWireLengthDelimited = 2;
constexpr uint32_t kWireFixed32 = 5;
constexpr size_t kMaxVarintBytes = 10;

// Bytes EncodeVarint writes for `value` (1..10).
inline size_t VarintSize(uint64_t value) {
  size_t size = 1;
  while (value >= 0x80) { value >>= 7; ++size; }
  return size;
}

// Base-128 little-endian varint into `out` (room for kMaxVarintBytes).
// A negative int32/int64 is passed sign-extended to 64 bits (ten bytes), as
// protobuf requires. sint (zigzag) fields are not used by any schema here.
inline size_t EncodeVarint(uint8_t* out, uint64_t value) {
  size_t used = 0;
  while (value >= 0x80) { out[used++] = static_cast<uint8_t>(value | 0x80); value >>= 7; }
  out[used++] = static_cast<uint8_t>(value);
  return used;
}

inline uint64_t MakeTag(uint32_t field, uint32_t wire_type) {
  return static_cast<uint64_t>(field) << 3 | wire_type;
}

// A fixed caller-owned region as a sink, for small nested messages built on
// the stack. An append that does not fit writes nothing and clears `ok`.
struct Span {
  uint8_t* data = nullptr;
  size_t capacity = 0;
  size_t used = 0;
  bool ok = true;

  void Append(const void* bytes, size_t length) {
    if (!length) return;
    if (!ok || length > capacity - used) { ok = false; return; }
    memcpy(data + used, bytes, length);
    used += length;
  }
};

template <typename Sink>
inline void PutVarint(Sink& out, uint64_t value) {
  uint8_t encoded[kMaxVarintBytes];
  out.Append(encoded, EncodeVarint(encoded, value));
}

template <typename Sink>
inline void PutTag(Sink& out, uint32_t field, uint32_t wire_type) {
  PutVarint(out, MakeTag(field, wire_type));
}

// Tag and length of a length-delimited field; the caller appends the body.
template <typename Sink>
inline void PutLengthPrefix(Sink& out, uint32_t field, size_t length) {
  PutTag(out, field, kWireLengthDelimited);
  PutVarint(out, length);
}

// A string, bytes or already-encoded nested message field. Always written,
// also when empty (explicit presence, oneof members, always-present
// sub-messages); implicit-presence callers skip the call for length 0.
template <typename Sink>
inline void PutBytes(Sink& out, uint32_t field, const void* bytes, size_t length) {
  PutLengthPrefix(out, field, length);
  out.Append(bytes, length);
}

// A varint field (int32/int64/uint32/uint64/bool/enum). Always written.
template <typename Sink>
inline void PutVarintField(Sink& out, uint32_t field, uint64_t value) {
  PutTag(out, field, kWireVarint);
  PutVarint(out, value);
}

// The eight little-endian bytes of a fixed64 / double payload, without a tag.
template <typename Sink>
inline void PutFixed64(Sink& out, uint64_t bits) {
  uint8_t bytes[8];
  for (int i = 0; i < 8; ++i) bytes[i] = static_cast<uint8_t>(bits >> (8 * i));
  out.Append(bytes, 8);
}

// A double field, always written bit for bit (including +0, -0 and NaN).
template <typename Sink>
inline void PutDouble(Sink& out, uint32_t field, double value) {
  const auto bits = std::bit_cast<uint64_t>(value);
  PutTag(out, field, kWireFixed64);
  PutFixed64(out, bits);
}

/** Write a proto3 implicit double unless it is the positive-zero default.
 * Negative zero is distinct under protobuf's presence rules and keeps its sign.
 */
template <typename Sink>
inline void PutImplicitDouble(Sink& out, uint32_t field, double value) {
  if (std::bit_cast<uint64_t>(value) != 0) PutDouble(out, field, value);
}

// Encoded size of a length-delimited field of `length` body bytes.
inline size_t BytesFieldSize(uint32_t field, size_t length) {
  return VarintSize(MakeTag(field, kWireLengthDelimited)) + VarintSize(length) + length;
}

// ---- google.protobuf.Struct / Value / ListValue framing ------------------------
//
// Shared by the two emitters that turn a JSON document into the bytes
// protobuf-es toBinary(fromJson(StructSchema | ValueSchema, json)) writes:
// codec::json_document_writer::AppendProtoValue/AppendProtoStruct (bounded token document) and
// tj::AppendProtoStruct (mutable DOM). Both measure bottom-up and then write;
// both preserve source/insertion member order and duplicate-key positions.
//   Struct    { map<string, Value> fields = 1; }   entry { key = 1; value = 2; }
//   Value     oneof { null_value = 1; number_value = 2; string_value = 3;
//                     bool_value = 4; struct_value = 5; list_value = 6; }
//   ListValue { repeated Value values = 1; }
// A oneof member is always written, so null (enum 0), false, 0 and "" are
// all present on the wire, and map entries always carry both key and value.
constexpr uint32_t kStructFields = 1;
constexpr uint32_t kMapEntryKey = 1;
constexpr uint32_t kMapEntryValue = 2;
constexpr uint32_t kValueNull = 1;
constexpr uint32_t kValueNumber = 2;
constexpr uint32_t kValueString = 3;
constexpr uint32_t kValueBool = 4;
constexpr uint32_t kValueStruct = 5;
constexpr uint32_t kValueList = 6;
constexpr uint32_t kListValues = 1;
// Encoded Value body sizes of the scalar members.
constexpr size_t kNullOrBoolValueSize = 2;  // tag + one-byte varint
constexpr size_t kNumberValueSize = 9;      // tag + fixed64

// Size of a length-delimited field with a one-byte tag (every field above).
inline size_t DelimitedSize(size_t body) { return 1 + VarintSize(body) + body; }
// Body size of one Struct.fields map entry.
inline size_t StructEntrySize(size_t key_length, size_t value_size) {
  return DelimitedSize(key_length) + DelimitedSize(value_size);
}

template <typename Sink>
inline void PutNullValue(Sink& out) { PutVarintField(out, kValueNull, 0); }
template <typename Sink>
inline void PutBoolValue(Sink& out, bool value) { PutVarintField(out, kValueBool, value ? 1 : 0); }
// number_value keeps every bit: -0 and (from a DOM built in code) NaN and
// the infinities are written as they are.
template <typename Sink>
inline void PutNumberValue(Sink& out, double value) { PutDouble(out, kValueNumber, value); }
// One Struct.fields entry up to and including the key's length prefix; the
// caller appends the `key_length` key bytes, then PutStructEntryValuePrefix
// and the Value body of `value_size` bytes.
template <typename Sink>
inline void PutStructEntryKeyPrefix(Sink& out, size_t key_length, size_t value_size) {
  PutLengthPrefix(out, kStructFields, StructEntrySize(key_length, value_size));
  PutLengthPrefix(out, kMapEntryKey, key_length);
}
template <typename Sink>
inline void PutStructEntryValuePrefix(Sink& out, size_t value_size) {
  PutLengthPrefix(out, kMapEntryValue, value_size);
}

// ---- reader -------------------------------------------------------------------
//
// Bounded protobuf wire primitives. Unknown fields (including groups) are
// skipped by wire type. uint32/lengths reject overflow; int32 accepts the
// protobuf ten-byte sign-extended negative representation. Strings retain BOM.
bool ReadVarint32(const uint8_t* body, size_t length, size_t* position, uint32_t* value);
bool ReadVarint64(const uint8_t* body, size_t length, size_t* position, uint64_t* value);
bool ReadTag(const uint8_t* body, size_t length, size_t* position, uint32_t* field,
             uint32_t* wire);
bool ReadBytes(const uint8_t* body, size_t length, size_t* position, const uint8_t** data,
               size_t* data_length);
bool ReadFixed64(const uint8_t* body, size_t length, size_t* position, uint64_t* value);
// Skips one field of `wire` type (groups recurse up to protobuf-es' limit).
bool SkipField(const uint8_t* body, size_t length, size_t* position, uint32_t wire,
               uint32_t field, int recursion_limit = 99);

class Reader {
 public:
  Reader(const uint8_t* data, size_t length) : data_(data), limit_(length) {}
  size_t pos() const { return position_; }
  bool Tag(uint32_t* field, uint32_t* wire);
  bool Skip(uint32_t wire, uint32_t field, int limit = 99);
  bool Uint32(uint32_t* value);
  bool Int32(int32_t* value = nullptr);
  bool Bool(bool* value);
  bool Double(double* value);
  bool String(const uint8_t** text, size_t* length);
  bool MessageLength(uint32_t* length);

  // Enforce an embedded message's own bounds, not just the outer buffer.
  // A frame does not move the cursor. Its enclosing loop must consume to end.
  class Frame {
   public:
    Frame(Reader& reader, size_t end) : reader_(reader), previous_(reader.limit_),
        ok_(end >= reader.position_ && end <= reader.limit_) {
      if (ok_) reader_.limit_ = end;
    }
    ~Frame() { reader_.limit_ = previous_; }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    bool ok() const { return ok_; }
   private:
    Reader& reader_;
    size_t previous_;
    bool ok_;
  };
 private:
  const uint8_t* data_;
  size_t limit_;
  size_t position_ = 0;
  uint32_t wire_ = 0;
};

}  // namespace fortis::codec::proto_wire
