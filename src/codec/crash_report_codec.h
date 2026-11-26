#pragma once

#include <stddef.h>
#include <stdint.h>

#include "bounded_json.h"
#include "price_history_batch_codec.h"

// Pure request/record helpers for POST /v1/crash-reports, mirroring the
// earlier TypeScript implementation (Zod body schema, protobuf-es decode,
// stored record). Guest-compiled: no C++ standard library.
namespace fortis::codec::crash_report {

// Retention of a stored report (the TypeScript RETENTION_MS).
constexpr int64_t kRetentionMs = 7LL * 24 * 60 * 60 * 1000;
// TypeScript requestCost for /crash-reports.
constexpr uint32_t kQuotaCost = 5;
constexpr size_t kUlidLength = 26;

// A UTF-8 slice; `data` is not NUL-terminated and may contain NUL.
struct Slice {
  const char* data = nullptr;
  size_t length = 0;
};

// A body field before validation. JSON values arrive as tokens of a parsed
// document; protobuf values are always strings and one boolean.
enum class ValueKind : uint8_t { kMissing, kNull, kBoolean, kNumber, kString, kArray, kObject };
struct FieldValue {
  ValueKind kind = ValueKind::kMissing;
  Slice text;          // kString, valid UTF-8
  bool boolean = false;
};

enum class Field : uint8_t {
  kPlatform, kAppVersion, kBuildMode, kSource, kFatal, kErrorType, kMessage, kStack,
  kOccurredAt, kCount
};

// The body as Zod sees it: a root value kind plus the nine shape fields.
// `unrecognized` (caller storage) lists keys outside the shape in
// first-occurrence order, each once.
struct RawReport {
  ValueKind root = ValueKind::kMissing;
  FieldValue fields[static_cast<size_t>(Field::kCount)];
  Slice* unrecognized = nullptr;
  size_t unrecognized_capacity = 0;
  size_t unrecognized_count = 0;
};

// The validated report: Zod's output (trimmed platform, appVersion and
// errorType; message and stack untouched).
struct ValidReport {
  Slice platform;
  bool has_app_version = false;
  Slice app_version;
  Slice build_mode;
  Slice source;
  bool fatal = false;
  Slice error_type;
  Slice message;
  Slice stack;
  Slice occurred_at;
};

// protobuf-es 2.x fromBinary(CrashReportRequestSchema): known fields are read
// by their declared type whatever the wire type, the last occurrence wins,
// app_version presence follows `optional`, strings must be valid UTF-8.
// Returns false wherever protobuf-es throws.
bool DecodeProtobuf(const uint8_t* body, size_t length, RawReport* report);

// Fills `report` from a parsed JSON document (JSON.parse semantics: the last
// duplicate wins). Decoded strings are written into `arena`, which must hold
// the JSON input length; `report->unrecognized` must hold one slot per
// object member. Returns false only when caller storage is too small.
bool ReadJson(const BoundedJsonDocument& document, char* arena, size_t arena_capacity,
              RawReport* report);

// No body at all (or another scalar root).
void ReadScalarRoot(ValueKind kind, RawReport* report);

// bodySchema.safeParse. On success fills `valid` and returns true; on
// failure appends `{"error":"BAD_REQUEST"}` to `out`.
bool Validate(const RawReport& report, ValidReport* valid,
              history_batch::OutBuffer& out);

// z.iso.datetime({ offset: true }).
bool IsIsoDatetimeWithOffset(const char* text, size_t length);

// ulid(): 48-bit millisecond time and 80 random bits, Crockford base32.
bool NewUlid(int64_t now_ms, const uint8_t random[10], char output[kUlidLength + 1]);

// Date.prototype.toISOString for a millisecond epoch time.
bool FormatIsoTime(int64_t ms, char output[25]);

// sha256(`${errorType}\n${message}\n${first eight stack lines}`) as hex.
bool Fingerprint(Slice error_type, Slice message, Slice stack, char output[65]);

struct StoredReport {
  const char* id = nullptr;
  const char* device_id = nullptr;
  const char* fingerprint = nullptr;
  const ValidReport* report = nullptr;
  Slice error_type;   // redacted
  Slice message;      // redacted
  Slice stack;        // redacted
  int64_t created_at_ms = 0;
};

// JSON.stringify of the facet's StoredCrashReport, in its key order.
bool AppendStoredJson(const StoredReport& stored, history_batch::OutBuffer& out);

// CrashReportResponse { accepted: true, id } as protobuf-es toBinary writes it.
size_t EncodeResponseProtobuf(const char id[kUlidLength + 1], uint8_t* output, size_t capacity);

}  // namespace fortis::codec::crash_report
