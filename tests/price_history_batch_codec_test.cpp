#include "price_history_batch_codec.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

using namespace fortis::codec::history_batch;

namespace {

char scratch[4096];

double number(const char* text) {
  return JsStringToNumber(text, strlen(text), scratch, sizeof(scratch));
}

bool json_string_is(const char* text, const char* expected) {
  uint8_t storage[256]{};
  OutBuffer out{storage, sizeof(storage)};
  AppendJsonString(out, text, strlen(text));
  return out.ok && out.used == strlen(expected) && memcmp(storage, expected, out.used) == 0;
}

bool js_number_is(double value, const char* expected) {
  uint8_t storage[64]{};
  OutBuffer out{storage, sizeof(storage)};
  AppendJsNumber(out, value);
  const bool same = out.ok && out.used == strlen(expected) &&
      memcmp(storage, expected, out.used) == 0;
  if (!same) fprintf(stderr, "AppendJsNumber: got %.*s, want %s\n",
                     static_cast<int>(out.used), storage, expected);
  return same;
}

struct Wire {
  uint8_t bytes[512]{};
  size_t used = 0;
  void byte(uint8_t value) { bytes[used++] = value; }
  void varint(uint64_t value) {
    do {
      uint8_t next = static_cast<uint8_t>(value & 0x7f);
      value >>= 7;
      if (value) next |= 0x80;
      byte(next);
    } while (value);
  }
  void tag(uint32_t field, uint32_t wire) { varint((field << 3) | wire); }
  void string(uint32_t field, const char* text) {
    tag(field, 2); varint(strlen(text));
    memcpy(bytes + used, text, strlen(text)); used += strlen(text);
  }
  bool decode(BatchProtoRequest* request) const {
    return DecodeBatchProtoRequest(bytes, used, request);
  }
};

}  // namespace

int main() {
  // Response-size policy, with the route's candle capacity (window buckets
  // + 1): the exact bounds the route test relies on.
  const size_t mib = 1024 * 1024;
  assert(BatchWorstResponseBytes(5, 2160 + 1, false) <= mib);
  assert(BatchWorstResponseBytes(6, 2160 + 1, false) > mib);
  assert(BatchWorstResponseBytes(11, 2160 + 1, true) <= mib);
  assert(BatchWorstResponseBytes(12, 2160 + 1, true) > mib);
  assert(BatchWorstResponseBytes(3, 3660 + 1, false) <= mib);
  assert(BatchWorstResponseBytes(4, 3660 + 1, false) > mib);
  assert(BatchWorstResponseBytes(7, 3660 + 1, true) <= mib);
  assert(BatchWorstResponseBytes(8, 3660 + 1, true) > mib);
  // The app's comparison windows with 50 IDs: 14 days hourly, one year daily.
  assert(BatchWorstResponseBytes(50, 14 * 24 + 1, true) <= mib);
  assert(BatchWorstResponseBytes(50, 365 + 1, true) <= mib);
  assert(BatchWorstResponseBytes(50, 14 * 24 + 1, false) > mib);

  // Number("…") (ECMAScript StringToNumber), as z.coerce.number() applies it.
  assert(number("") == 0 && number("   ") == 0);
  assert(number("  12 ") == 12);
  assert(number("\xc2\xa0" "12" "\xe2\x80\xa8") == 12);  // NBSP and U+2028 trim
  assert(number("1767225600") == 1767225600);
  assert(number("1.7672256e9") == 1767225600);
  assert(number("+.5") == 0.5 && number("5.") == 5 && number("-2.5e1") == -25);
  assert(number("0x10") == 16 && number("0X1f") == 31);
  assert(number("0o17") == 15 && number("0b101") == 5);
  assert(number("0x1fffffffffffff") == 9007199254740991.0);
  // 2^53 + 1 rounds once, to the even neighbour 2^53.
  assert(number("0x20000000000001") == 9007199254740992.0);
  assert(number("Infinity") == INFINITY && number("+Infinity") == INFINITY);
  assert(number("-Infinity") == -INFINITY);
  const char* not_numbers[] = {".", "1e", "e5", "-0x10", "0x", "0b2", "12abc", "1_000",
                               "infinity", "NaN", "--1", "1.2.3"};
  for (const char* text : not_numbers) assert(isnan(number(text)));

  // String.prototype.trim and .length.
  size_t begin = 0, end = 0;
  const char* padded = " \t" "ab" "\xef\xbb\xbf";  // trailing U+FEFF
  JsTrim(padded, strlen(padded), &begin, &end);
  assert(begin == 2 && end == 4);
  const char* blank = "\xe2\x80\x80\xe3\x80\x80";  // U+2000 U+3000
  JsTrim(blank, strlen(blank), &begin, &end);
  assert(begin == end);
  const char* inner = "a b";
  JsTrim(inner, strlen(inner), &begin, &end);
  assert(begin == 0 && end == 3);
  assert(Utf16Units("\xc3\xa9", 2) == 1);              // é
  assert(Utf16Units("\xf0\x9f\x98\x80", 4) == 2);      // U+1F600
  assert(ValidUtf8(reinterpret_cast<const uint8_t*>("\xf0\x9f\x98\x80"), 4));
  assert(!ValidUtf8(reinterpret_cast<const uint8_t*>("\xed\xa0\x80"), 3));  // surrogate
  assert(!ValidUtf8(reinterpret_cast<const uint8_t*>("\xc0\xaf"), 2));      // overlong
  assert(!ValidUtf8(reinterpret_cast<const uint8_t*>("\xe2\x80"), 2));      // truncated

  // Array-index keys enumerate first in Object.entries/JSON.stringify.
  uint32_t index = 0;
  assert(JsArrayIndexKey("0", 1, &index) && index == 0);
  assert(JsArrayIndexKey("42", 2, &index) && index == 42);
  assert(JsArrayIndexKey("4294967294", 10, &index) && index == 4294967294u);
  assert(!JsArrayIndexKey("4294967295", 10, nullptr));
  assert(!JsArrayIndexKey("042", 3, nullptr) && !JsArrayIndexKey("-1", 2, nullptr));
  assert(!JsArrayIndexKey("", 0, nullptr) && !JsArrayIndexKey("4a", 2, nullptr));

  // JSON.stringify escaping and Number::toString.
  assert(json_string_is("a\"b\\c\n\x01\x7f", "\"a\\\"b\\\\c\\n\\u0001\x7f\""));
  assert(json_string_is("caf\xc3\xa9 </script> \xe2\x80\xa8",
                        "\"caf\xc3\xa9 </script> \xe2\x80\xa8\""));
  assert(js_number_is(0, "0") && js_number_is(-0.0, "0"));
  assert(js_number_is(101, "101") && js_number_is(-2.5, "-2.5"));
  assert(js_number_is(0.5, "0.5") && js_number_is(0.000001, "0.000001"));
  assert(js_number_is(1.5e-7, "1.5e-7") && js_number_is(5e-324, "5e-324"));
  assert(js_number_is(1e21, "1e+21") && js_number_is(123456789012345680000.0,
                                                     "123456789012345680000"));
  assert(js_number_is(1767225600, "1767225600"));
  assert(js_number_is(1.7976931348623157e308, "1.7976931348623157e+308"));
  assert(js_number_is(0.1 + 0.2, "0.30000000000000004"));

  // protobuf-es fromBinary(PriceHistoryBatchRequest).
  BatchProtoRequest request{};
  Wire empty;
  assert(empty.decode(&request) && request.id_count == 0 && request.from == 0 &&
         !request.has_interval);

  Wire full;
  full.string(1, "ethereum"); full.string(1, " bitcoin ");
  full.tag(2, 0); full.varint(1767225600);
  full.tag(3, 0); full.varint(1767247200);
  full.string(4, "1d"); full.string(4, "1h");    // last occurrence wins
  full.tag(9, 0); full.varint(300);               // unknown varint
  full.string(10, "skipped");                     // unknown length-delimited
  full.tag(11, 3); full.tag(1, 0); full.varint(7); full.tag(11, 4);  // unknown group
  full.tag(12, 1); for (int i = 0; i < 8; ++i) full.byte(0);        // unknown fixed64
  full.tag(13, 5); for (int i = 0; i < 4; ++i) full.byte(0);        // unknown fixed32
  full.string(1, "e\xc3\xa9");
  assert(full.decode(&request));
  assert(request.id_count == 3 && request.from == 1767225600 && request.to == 1767247200);
  assert(request.has_interval && request.interval_length == 2 &&
         memcmp(request.interval, "1h", 2) == 0);
  size_t cursor = 0; const uint8_t* id = nullptr; size_t id_length = 0;
  assert(NextBatchProtoId(full.bytes, full.used, &cursor, &id, &id_length) &&
         id_length == 8 && memcmp(id, "ethereum", 8) == 0);
  assert(NextBatchProtoId(full.bytes, full.used, &cursor, &id, &id_length) &&
         id_length == 9 && memcmp(id, " bitcoin ", 9) == 0);
  assert(NextBatchProtoId(full.bytes, full.used, &cursor, &id, &id_length) &&
         id_length == 3 && memcmp(id, "e\xc3\xa9", 3) == 0);
  assert(!NextBatchProtoId(full.bytes, full.used, &cursor, &id, &id_length));

  // Known fields ignore the wire type and read their declared scalar type.
  Wire lenient;
  lenient.tag(2, 2); lenient.varint(5);
  lenient.tag(1, 0); lenient.varint(2); lenient.byte('h'); lenient.byte('i');
  assert(!lenient.decode(&request));

  // Negative int64 is a ten-byte varint.
  Wire negative;
  negative.tag(2, 0); negative.varint(static_cast<uint64_t>(-5LL));
  assert(negative.decode(&request) && request.from == -5);

  // Everything protobuf-es rejects.
  const uint8_t truncated_length[] = {0x0a, 0xff};
  assert(!DecodeBatchProtoRequest(truncated_length, sizeof(truncated_length), &request));
  const uint8_t short_string[] = {0x0a, 0x05, 'a', 'b'};
  assert(!DecodeBatchProtoRequest(short_string, sizeof(short_string), &request));
  const uint8_t bad_utf8[] = {0x0a, 0x02, 0xc0, 0xaf};
  assert(!DecodeBatchProtoRequest(bad_utf8, sizeof(bad_utf8), &request));
  const uint8_t field_zero[] = {0x00, 0x01};
  assert(!DecodeBatchProtoRequest(field_zero, sizeof(field_zero), &request));
  const uint8_t wire_six[] = {0x4e};
  assert(!DecodeBatchProtoRequest(wire_six, sizeof(wire_six), &request));
  const uint8_t stray_end_group[] = {0x64};  // field 12, wire 4
  assert(!DecodeBatchProtoRequest(stray_end_group, sizeof(stray_end_group), &request));
  const uint8_t wrong_end_group[] = {0x5b, 0x64};  // start 11, end 12
  assert(!DecodeBatchProtoRequest(wrong_end_group, sizeof(wrong_end_group), &request));
  const uint8_t long_tag[] = {0x88, 0x80, 0x80, 0x80, 0x80, 0x00};
  assert(!DecodeBatchProtoRequest(long_tag, sizeof(long_tag), &request));
  const uint8_t long_varint[] = {0x10, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                 0xff, 0xff, 0x01};
  assert(!DecodeBatchProtoRequest(long_varint, sizeof(long_varint), &request));
  const uint8_t short_fixed64[] = {0x61, 0x00, 0x00};
  assert(!DecodeBatchProtoRequest(short_fixed64, sizeof(short_fixed64), &request));

  puts("price history batch codec passed");
  return 0;
}
