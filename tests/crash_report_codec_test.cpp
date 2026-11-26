// Unit vectors for the POST /v1/crash-reports codec. The route fixture
// (tests/crash_reports_route_test.py) covers the HTTP contract; these pin the
// pieces it cannot isolate: ulid encoding, ISO time, Object.keys ordering of
// unknown keys, protobuf-es wire edge cases and the fingerprint input.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "crash_report_codec.h"

using namespace fortis::codec;

namespace cr = fortis::codec::crash_report;
using fortis::codec::history_batch::OutBuffer;

namespace {

void check_ulid() {
  char id[cr::kUlidLength + 1];
  const uint8_t zero[10] = {};
  const uint8_t ones[10] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  // The ulid specification's time vector.
  assert(cr::NewUlid(1469918176385LL, zero, id) && strcmp(id, "01ARYZ6S410000000000000000") == 0);
  assert(cr::NewUlid(0, ones, id) && strcmp(id, "0000000000ZZZZZZZZZZZZZZZZ") == 0);
  const uint8_t pattern[10] = {0x84, 0x21, 0x08, 0x42, 0x10, 0x84, 0x21, 0x08, 0x42, 0x10};
  // 10000100 00100001 00001000 ...: every five-bit group is 0b10000 (G).
  assert(cr::NewUlid(0, pattern, id) && strcmp(id + 10, "GGGGGGGGGGGGGGGG") == 0);
  assert(cr::NewUlid(0xFFFFFFFFFFFFLL, zero, id) && strncmp(id, "7ZZZZZZZZZ", 10) == 0);
  assert(!cr::NewUlid(0x1000000000000LL, zero, id));
  assert(!cr::NewUlid(-1, zero, id));
}

void check_time() {
  char text[25];
  assert(cr::FormatIsoTime(1767323045000LL, text) && strcmp(text, "2026-01-02T03:04:05.000Z") == 0);
  assert(cr::FormatIsoTime(1767323045000LL + cr::kRetentionMs + 7, text) &&
         strcmp(text, "2026-01-09T03:04:05.007Z") == 0);
  assert(!cr::FormatIsoTime(-1, text));
}

void check_datetime() {
  struct Case { const char* text; bool valid; };
  const Case cases[] = {
      {"2026-01-02T03:04Z", true}, {"2026-01-02T03:04:05Z", true},
      {"2026-01-02T03:04:05.5-03:30", true}, {"2026-01-02T03:04:05+23:59", true},
      {"2026-01-02T03:04:05.123456789+14:00", true}, {"2000-02-29T00:00:00Z", true},
      {"0000-02-29T00:00:00Z", true}, {"2024-02-29T23:59:59Z", true},
      {"2026-01-02T03:04:05+24:00", false}, {"2026-01-02T24:00:00Z", false},
      {"2026-01-02T03:04:05z", false}, {"2026-01-02 03:04:05Z", false},
      {"2026-02-29T00:00:00Z", false}, {"1900-02-29T00:00:00Z", false},
      {"2026-04-31T00:00:00Z", false}, {"2026-01-02T03:04:05.Z", false},
      {"2026-01-02T03:04:05", false}, {"2026-01-02T03:04:05Z\n", false},
      {"2026-13-01T00:00:00Z", false}, {"2026-00-01T00:00:00Z", false},
      {"2026-01-00T00:00:00Z", false}, {"2026-01-02T03:60Z", false},
      {"2026-01-02T03:04:60Z", false}, {"2026-01-02T03:04+01:00", true},
      {"2026-01-02T03:04:05+0100", false}, {"2026-01-02T03:04:05+01:60", false},
      {"", false}, {"+12026-01-02T03:04:05Z", false},
  };
  for (const Case& item : cases) {
    if (cr::IsIsoDatetimeWithOffset(item.text, strlen(item.text)) != item.valid) {
      fprintf(stderr, "datetime %s: expected %d\n", item.text, item.valid);
      assert(false);
    }
  }
}

void check_unrecognized_keys() {
  // Keys outside the shape are collected once each, in first-occurrence
  // order, and refuse the report with the plain BAD_REQUEST body.
  const char body[] =
      "{\"b\":1,\"2\":1,\"a\\\"q\":1,\"1\":1,\"01\":1,\"4294967295\":1,\"4294967294\":1,"
      "\"b\":2,\"platform\":\"ios\",\"buildMode\":\"debug\",\"source\":\"flutter\","
      "\"fatal\":false,\"errorType\":\"E\",\"message\":\"\",\"stack\":\"\","
      "\"occurredAt\":\"2026-01-02T03:04:05Z\"}";
  static BoundedJsonToken tokens[256];
  BoundedJsonDocument json;
  BoundedJsonError error = BoundedJsonError::kNone;
  assert(json.Parse(reinterpret_cast<const unsigned char*>(body), strlen(body), tokens, 256, &error));
  static char arena[1024];
  cr::Slice keys[64];
  cr::RawReport raw{};
  raw.unrecognized = keys;
  raw.unrecognized_capacity = 64;
  assert(cr::ReadJson(json, arena, sizeof(arena), &raw));
  uint8_t storage[2048];
  OutBuffer out{storage, sizeof(storage)};
  cr::ValidReport valid{};
  assert(!cr::Validate(raw, &valid, out));
  storage[out.used] = 0;
  assert(raw.unrecognized_count == 7);
  const char* first[] = {"b", "2", "a\"q", "1", "01", "4294967295", "4294967294"};
  for (size_t i = 0; i < 7; ++i)
    assert(raw.unrecognized[i].length == strlen(first[i]) &&
           memcmp(raw.unrecognized[i].data, first[i], strlen(first[i])) == 0);
  const char* expected = "{\"error\":\"BAD_REQUEST\"}";
  if (strcmp(reinterpret_cast<char*>(storage), expected) != 0) {
    fprintf(stderr, "wanted %s\nactual %s\n", expected, storage);
    assert(false);
  }
  // Too little caller storage fails instead of truncating the list.
  raw.unrecognized_capacity = 3;
  assert(!cr::ReadJson(json, arena, sizeof(arena), &raw));
}

bool decodes(const uint8_t* wire, size_t length) {
  cr::RawReport raw{};
  return cr::DecodeProtobuf(wire, length, &raw);
}

void check_protobuf() {
  const uint8_t empty[] = {0};
  assert(decodes(empty, 0));
  // Unknown fields of every wire type, including a nested group, are skipped.
  const uint8_t unknown[] = {0x50, 0x01,                          // 10: varint
                             0x59, 1, 2, 3, 4, 5, 6, 7, 8,        // 11: fixed64
                             0x65, 1, 2, 3, 4,                    // 12: fixed32
                             0x6a, 0x01, 0x41,                    // 13: bytes
                             0x73, 0x08, 0x01, 0x74};             // 14: group {1: 1}
  assert(decodes(unknown, sizeof(unknown)));
  const uint8_t end_group[] = {0x74};
  assert(!decodes(end_group, sizeof(end_group)));
  const uint8_t field_zero[] = {0x00, 0x01};
  assert(!decodes(field_zero, sizeof(field_zero)));
  const uint8_t wire_six[] = {0x0e};
  assert(!decodes(wire_six, sizeof(wire_six)));
  const uint8_t truncated[] = {0x0a, 0x05, 'i', 'o'};
  assert(!decodes(truncated, sizeof(truncated)));
  const uint8_t bad_utf8[] = {0x3a, 0x01, 0xff};
  assert(!decodes(bad_utf8, sizeof(bad_utf8)));
  // Strings are read by declared type whatever the wire type; last wins.
  const uint8_t repeated[] = {0x0a, 0x03, 'i', 'o', 's', 0x0a, 0x03, 'w', 'i', 'n',
                              0x28, 0x80, 0x01, 0x12, 0x00};
  cr::RawReport raw{};
  assert(cr::DecodeProtobuf(repeated, sizeof(repeated), &raw));
  const cr::FieldValue& platform = raw.fields[static_cast<size_t>(cr::Field::kPlatform)];
  assert(platform.text.length == 3 && memcmp(platform.text.data, "win", 3) == 0);
  assert(raw.fields[static_cast<size_t>(cr::Field::kFatal)].boolean);
  // optional app_version: present (even empty) only when the field occurs.
  assert(raw.fields[static_cast<size_t>(cr::Field::kAppVersion)].kind == cr::ValueKind::kString);
  assert(decodes(repeated, sizeof(repeated) - 2));
  cr::RawReport without{};
  assert(cr::DecodeProtobuf(repeated, sizeof(repeated) - 2, &without));
  assert(without.fields[static_cast<size_t>(cr::Field::kAppVersion)].kind ==
         cr::ValueKind::kMissing);
}

void check_fingerprint_and_response() {
  // The stored fingerprint of the fixture's "json valid" case.
  const char stack[] = "#0 /Users/[REDACTED]/project/main.dart:1\n#1 two\n#2 three\n#3 four\n"
                       "#4 five\n#5 six\n#6 seven\n#7 eight\n#8 nine";
  const char message[] = "failed for [REDACTED_TOKEN] and [REDACTED_HEX]";
  char fingerprint[65];
  assert(cr::Fingerprint({"StateError", 10}, {message, strlen(message)}, {stack, strlen(stack)},
                         fingerprint));
  assert(strcmp(fingerprint,
                "a10a34fec056dcf04c1b4be11b93c62acf72f344f6e4b31fefec46360f4a24a8") == 0);
  uint8_t wire[64];
  const char id[] = "01ARZ3NDEKTSV4RRFFQ69G5FAV";
  assert(cr::EncodeResponseProtobuf(id, wire, sizeof(wire)) == 30);
  assert(wire[0] == 0x08 && wire[1] == 0x01 && wire[2] == 0x12 && wire[3] == 26);
  assert(memcmp(wire + 4, id, 26) == 0);
  assert(cr::EncodeResponseProtobuf(id, wire, 29) == 0);
}

}  // namespace

int main() {
  check_ulid();
  check_time();
  check_datetime();
  check_unrecognized_keys();
  check_protobuf();
  check_fingerprint_and_response();
  printf("crash report codec vectors passed\n");
  return 0;
}
