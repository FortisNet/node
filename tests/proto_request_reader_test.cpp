#include "proto_wire.h"
#include <stdio.h>

namespace pw = fortis::codec::proto_wire;
namespace {
int failures = 0;
void check(bool value, const char* name) {
  if (!value) { fprintf(stderr, "FAIL: %s\n", name); ++failures; }
}
}
int main() {
  const uint8_t message[] = {0x0a, 0x04, 0xef, 0xbb, 0xbf, 'x', 0x10, 0x96, 0x01,
                            0xfa, 0x01, 0x03, 0x01, 0x02, 0x03};
  pw::Reader reader(message, sizeof(message));
  uint32_t field = 0, wire = 0, number = 0;
  const uint8_t* text = nullptr;
  size_t length = 0;
  check(reader.Tag(&field, &wire) && field == 1 && reader.String(&text, &length) &&
        length == 4 && text[0] == 0xef, "strings preserve a leading BOM");
  check(reader.Tag(&field, &wire) && field == 2 && reader.Uint32(&number) && number == 150,
        "canonical scalar request");
  check(reader.Tag(&field, &wire) && field == 31 && reader.Skip(wire, field) &&
        reader.pos() == sizeof(message), "unknown packed repeated scalars stay accepted");

  const uint8_t negative[] = {0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01};
  pw::Reader signed_reader(negative, sizeof(negative));
  int32_t integer = 0;
  check(signed_reader.Tag(&field, &wire) && signed_reader.Int32(&integer) && integer == -1,
        "ten-byte negative int32 is valid");
  pw::Reader unsigned_reader(negative, sizeof(negative));
  check(unsigned_reader.Tag(&field, &wire) && !unsigned_reader.Uint32(&number), "uint32 overflow refused");
  const uint8_t overflow[] = {0x08, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02};
  pw::Reader overflow_reader(overflow, sizeof(overflow));
  bool boolean = false;
  check(overflow_reader.Tag(&field, &wire) && !overflow_reader.Bool(&boolean), "varint overflow past 64 bits refused");
  const uint8_t wrong_string[] = {0x08, 0x01, 'x'};
  pw::Reader wrong(wrong_string, sizeof(wrong_string));
  check(wrong.Tag(&field, &wire) && !wrong.String(&text, &length), "known field wire mismatch refused");
  const uint8_t invalid_utf8[] = {0x0a, 0x03, 0xed, 0xa0, 0x80};
  pw::Reader invalid(invalid_utf8, sizeof(invalid_utf8));
  check(invalid.Tag(&field, &wire) && !invalid.String(&text, &length), "protobuf strings are Unicode scalars");

  // A complete outer buffer must not rescue a truncated embedded message.
  const uint8_t nested[] = {0x0a, 0x02, 0x0a, 0x02, 'x', 'y'};
  pw::Reader bounded(nested, sizeof(nested));
  check(bounded.Tag(&field, &wire) && bounded.MessageLength(&number), "embedded message prefix");
  {
    pw::Reader::Frame frame(bounded, bounded.pos() + number);
    check(frame.ok() && bounded.Tag(&field, &wire) && !bounded.String(&text, &length),
          "embedded field cannot consume bytes beyond its message");
  }
  const uint8_t groups[] = {0x0b, 0x10, 0x01, 0x0c};
  pw::Reader group(groups, sizeof(groups));
  check(group.Tag(&field, &wire) && group.Skip(wire, field) && group.pos() == sizeof(groups),
        "unknown legacy group skipped to matching end");
  const uint8_t mismatched[] = {0x0b, 0x14};
  pw::Reader bad_group(mismatched, sizeof(mismatched));
  check(bad_group.Tag(&field, &wire) && !bad_group.Skip(wire, field), "mismatched group refused");
  const uint8_t long_unknown[] = {0x08, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00};
  pw::Reader long_reader(long_unknown, sizeof(long_unknown));
  check(long_reader.Tag(&field, &wire) && !long_reader.Skip(wire, field), "unbounded unknown varint refused");
  const uint8_t length_overflow[] = {0x0a, 0x80, 0x80, 0x80, 0x80, 0x10};
  pw::Reader long_length(length_overflow, sizeof(length_overflow));
  check(long_length.Tag(&field, &wire) && !long_length.String(&text, &length), "length overflow refused");
  if (failures) return 1;
  puts("proto_request_reader_test: request wire types, Unicode, boundaries, unknowns and signed varints OK");
  return 0;
}
