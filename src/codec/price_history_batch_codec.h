#pragma once

#include <stddef.h>
#include <stdint.h>

// Pure, allocation-free helpers for the POST /v1/prices/history route.
// Each mirrors one JavaScript or protobuf-es behaviour the TypeScript route
// depends on. They are compiled into the guest, so they use no C++ standard
// library headers.
namespace fortis::codec::history_batch {

// Appends bytes to caller-owned storage. A null `data` only counts bytes, so
// nested protobuf lengths can be measured with the same emitter.
struct OutBuffer {
  uint8_t* data = nullptr;
  size_t capacity = 0;
  size_t used = 0;
  bool ok = true;

  void Append(const void* bytes, size_t length);
  void Byte(uint8_t value);
  void Text(const char* text);
};

// Strict UTF-8 (TextDecoder fatal): no overlongs, surrogates, or > U+10FFFF.
bool ValidUtf8(const uint8_t* bytes, size_t length);
// String.prototype.trim on valid UTF-8: ECMAScript WhiteSpace and
// LineTerminator code points are removed from both ends.
void JsTrim(const char* text, size_t length, size_t* begin, size_t* end);
// String.prototype.length (UTF-16 code units) of valid UTF-8.
size_t Utf16Units(const char* text, size_t length);
// ECMAScript StringToNumber (Number("…")) of valid UTF-8. Its use on request
// values is parity-only, see compat-decisions.md#request-coercion
// (provider payloads keep it). `scratch` holds a
// NUL-terminated copy for strtod; it must have room for length + 3 bytes,
// otherwise NaN is returned.
double JsStringToNumber(const char* text, size_t length, char* scratch,
                        size_t scratch_capacity);
// ECMAScript array-index property keys ("0".."4294967294", canonical form)
// enumerate before all other string keys, in ascending numeric order.
// parity-only, see compat-decisions.md#js-property-order
bool JsArrayIndexKey(const char* text, size_t length, uint32_t* index);
// JSON.stringify string escaping, without the surrounding quotes.
void AppendJsonStringBody(OutBuffer& out, const char* text, size_t length);
void AppendJsonString(OutBuffer& out, const char* text, size_t length);
// Number.prototype.toString as JSON.stringify writes it (finite values).
void AppendJsNumber(OutBuffer& out, double value);
void AppendInteger(OutBuffer& out, int64_t value);

// protobuf-es 2.x fromBinary(PriceHistoryBatchRequestSchema, body). Known
// fields are read by their declared type whatever the wire type, unknown
// fields are skipped by wire type, and strings must be valid UTF-8. Returns
// false wherever protobuf-es throws. Built on the proto_wire reader
// (proto_wire.h), which other request decoders use directly.
struct BatchProtoRequest {
  size_t id_count = 0;
  int64_t from = 0;
  int64_t to = 0;
  bool has_interval = false;
  const uint8_t* interval = nullptr;
  size_t interval_length = 0;
};
bool DecodeBatchProtoRequest(const uint8_t* body, size_t length,
                             BatchProtoRequest* request);
// Returns the next `ids` entry of a body DecodeBatchProtoRequest accepted.
// `cursor` is a byte offset that starts at zero; each call resumes after the
// previous entry, so a full iteration is one pass over the body.
bool NextBatchProtoId(const uint8_t* body, size_t length, size_t* cursor,
                      const uint8_t** id, size_t* id_length);

// Response-size policy: an upper bound on a batch reply of `ids` entries of
// up to `candles` candles each. Per candle a JSON point and volume pair is at
// most 90 bytes ([16-digit seconds, 24-character number] twice, with
// separators); a protobuf pair is at most 40 (two PricePoint entries: tag,
// length, a varint of at most 8 bytes for seconds ≤ 2^53 and a double). Each
// entry adds at most 2,400 bytes (escaped key, canonical id, provenance,
// window and availability), the envelope 512. So one full ten-year daily ID
// (3,662 candles) fits three times in 1 MiB of JSON and seven of protobuf,
// and 50 IDs fit the app's widest comparison windows (14 days hourly, one
// year daily) as protobuf.
constexpr size_t kBatchWorstJsonBytesPerCandle = 90;
constexpr size_t kBatchWorstProtoBytesPerCandle = 40;
constexpr size_t kBatchWorstBytesPerEntry = 2400;
constexpr size_t kBatchWorstEnvelopeBytes = 512;
size_t BatchWorstResponseBytes(size_t ids, size_t candles, bool protobuf);

}  // namespace fortis::codec::history_batch
