#include "crash_report_codec.h"

#include "proto_wire.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <mbedtls/sha256.h>

namespace fortis::codec::crash_report {
namespace {

using history_batch::AppendJsonString;
using history_batch::OutBuffer;

constexpr size_t kFieldCount = static_cast<size_t>(Field::kCount);
const char* const kFieldNames[kFieldCount] = {"platform", "appVersion", "buildMode",
    "source", "fatal", "errorType", "message", "stack", "occurredAt"};

// ── JSON helpers ──

ValueKind kind_of(const BoundedJsonToken* token) {
  switch (token->type) {
    case BoundedJsonType::kObject: return ValueKind::kObject;
    case BoundedJsonType::kArray: return ValueKind::kArray;
    case BoundedJsonType::kString: return ValueKind::kString;
    case BoundedJsonType::kNumber: return ValueKind::kNumber;
    case BoundedJsonType::kTrue:
    case BoundedJsonType::kFalse: return ValueKind::kBoolean;
    case BoundedJsonType::kNull: return ValueKind::kNull;
  }
  return ValueKind::kNull;
}

bool slice_equals(Slice slice, const char* literal) {
  const size_t length = strlen(literal);
  return slice.length == length && memcmp(slice.data, literal, length) == 0;
}

bool same_slice(Slice a, Slice b) {
  return a.length == b.length && (!a.length || memcmp(a.data, b.data, a.length) == 0);
}

const FieldValue& field_value(const RawReport& report, Field field) {
  return report.fields[static_cast<size_t>(field)];
}

// z.string().trim() with optional min/max.
bool check_string(const RawReport& report, Field field, bool trim, size_t minimum,
                  size_t maximum, Slice* output) {
  const FieldValue& value = field_value(report, field);
  if (value.kind != ValueKind::kString) return false;
  Slice text = value.text;
  if (trim) {
    size_t begin = 0, end = 0;
    history_batch::JsTrim(text.data, text.length, &begin, &end);
    text = {text.data + begin, end - begin};
  }
  const size_t units = history_batch::Utf16Units(text.data, text.length);
  if (units < minimum || units > maximum) return false;
  *output = text;
  return true;
}

bool check_enum(const RawReport& report, Field field, const char* const* values,
                size_t count, Slice* output) {
  const FieldValue& value = field_value(report, field);
  if (value.kind == ValueKind::kString) {
    for (size_t i = 0; i < count; ++i) {
      if (slice_equals(value.text, values[i])) { *output = value.text; return true; }
    }
  }
  return false;
}

bool two_digits(const char* text, int* value) {
  if (text[0] < '0' || text[0] > '9' || text[1] < '0' || text[1] > '9') return false;
  *value = (text[0] - '0') * 10 + (text[1] - '0');
  return true;
}

}  // namespace

bool IsIsoDatetimeWithOffset(const char* text, size_t length) {
  // YYYY-MM-DDTHH:MM is the shortest accepted prefix.
  if (!text || length < 17) return false;
  int century = 0, year = 0, month = 0, day = 0, hour = 0, minute = 0;
  if (!two_digits(text, &century) || !two_digits(text + 2, &year) || text[4] != '-' ||
      !two_digits(text + 5, &month) || text[7] != '-' || !two_digits(text + 8, &day) ||
      text[10] != 'T' || !two_digits(text + 11, &hour) || text[13] != ':' ||
      !two_digits(text + 14, &minute)) return false;
  int days = 0;
  switch (month) {
    case 1: case 3: case 5: case 7: case 8: case 10: case 12: days = 31; break;
    case 4: case 6: case 9: case 11: days = 30; break;
    case 2: {
      const bool leap = year ? year % 4 == 0 : century % 4 == 0;
      days = leap ? 29 : 28;
      break;
    }
    default: return false;
  }
  if (day < 1 || day > days || hour > 23 || minute > 59) return false;
  size_t i = 16;
  if (i < length && text[i] == ':') {
    int second = 0;
    if (length - i < 3 || !two_digits(text + i + 1, &second) || second > 59) return false;
    i += 3;
    if (i < length && text[i] == '.') {
      size_t digits = 0;
      while (i + 1 + digits < length && text[i + 1 + digits] >= '0' &&
             text[i + 1 + digits] <= '9') ++digits;
      if (!digits) return false;
      i += 1 + digits;
    }
  }
  if (i < length && text[i] == 'Z') return i + 1 == length;
  if (i < length && (text[i] == '+' || text[i] == '-')) {
    int offset_hour = 0, offset_minute = 0;
    return length - i == 6 && two_digits(text + i + 1, &offset_hour) && offset_hour <= 23 &&
           text[i + 3] == ':' && two_digits(text + i + 4, &offset_minute) && offset_minute <= 59;
  }
  return false;
}

bool DecodeProtobuf(const uint8_t* body, size_t length, RawReport* report) {
  if (!report || (!body && length)) return false;
  report->root = ValueKind::kObject;
  for (FieldValue& value : report->fields) {
    value.kind = ValueKind::kString;
    value.text = {"", 0};
  }
  report->fields[static_cast<size_t>(Field::kAppVersion)].kind = ValueKind::kMissing;
  FieldValue& fatal = report->fields[static_cast<size_t>(Field::kFatal)];
  fatal.kind = ValueKind::kBoolean;
  fatal.boolean = false;
  report->unrecognized_count = 0;
  static const Field kByNumber[10] = {Field::kCount, Field::kPlatform, Field::kAppVersion,
      Field::kBuildMode, Field::kSource, Field::kFatal, Field::kErrorType, Field::kMessage,
      Field::kStack, Field::kOccurredAt};
  size_t position = 0;
  while (position < length) {
    uint32_t number = 0, wire = 0;
    if (!proto_wire::ReadTag(body, length, &position, &number, &wire)) return false;
    if (number >= 10) {
      if (!proto_wire::SkipField(body, length, &position, wire, number)) return false;
      continue;
    }
    const Field field = kByNumber[number];
    if (field == Field::kFatal) {
      if (wire != 0) return false;
      uint64_t value = 0;
      if (!proto_wire::ReadVarint64(body, length, &position, &value)) return false;
      fatal.boolean = value != 0;
      continue;
    }
    const uint8_t* data = nullptr; size_t data_length = 0;
    if (wire != 2 || !proto_wire::ReadBytes(body, length, &position, &data, &data_length) ||
        !history_batch::ValidUtf8(data, data_length)) return false;
    FieldValue& value = report->fields[static_cast<size_t>(field)];
    value.kind = ValueKind::kString;
    value.text = {reinterpret_cast<const char*>(data), data_length};
  }
  return true;
}

bool ReadJson(const BoundedJsonDocument& document, char* arena, size_t arena_capacity,
              RawReport* report) {
  if (!report) return false;
  for (FieldValue& value : report->fields) value = FieldValue{};
  report->unrecognized_count = 0;
  const BoundedJsonToken* root = document.token(document.root());
  if (!root) return false;
  report->root = kind_of(root);
  if (root->type != BoundedJsonType::kObject) return true;
  size_t used = 0;
  auto decode = [&](uint32_t index, Slice* output) {
    size_t length = 0;
    if (used >= arena_capacity ||
        !document.DecodeString(index, arena + used, arena_capacity - used, &length))
      return false;
    *output = {arena + used, length};
    used += length;
    return true;
  };
  // First-occurrence order of the keys outside the shape.
  size_t first_count = 0;
  for (uint32_t key = root->first_child; key != kBoundedJsonNoToken;) {
    const BoundedJsonToken* key_token = document.token(key);
    const uint32_t value_index = key_token ? key_token->next_sibling : kBoundedJsonNoToken;
    const BoundedJsonToken* value_token = document.token(value_index);
    if (!value_token) return false;
    Slice name;
    if (!decode(key, &name)) return false;
    size_t field = 0;
    while (field < kFieldCount && !slice_equals(name, kFieldNames[field])) ++field;
    if (field < kFieldCount) {
      FieldValue& value = report->fields[field];
      value.kind = kind_of(value_token);
      value.boolean = value_token->type == BoundedJsonType::kTrue;
      value.text = {};
      if (value.kind == ValueKind::kString && !decode(value_index, &value.text)) return false;
    } else {
      bool seen = false;
      for (size_t i = 0; i < first_count && !seen; ++i)
        seen = same_slice(report->unrecognized[i], name);
      if (!seen) {
        if (first_count == report->unrecognized_capacity) return false;
        report->unrecognized[first_count++] = name;
      }
    }
    key = value_token->next_sibling;
  }
  report->unrecognized_count = first_count;
  return true;
}

void ReadScalarRoot(ValueKind kind, RawReport* report) {
  if (!report) return;
  for (FieldValue& value : report->fields) value = FieldValue{};
  report->root = kind;
  report->unrecognized_count = 0;
}

bool Validate(const RawReport& report, ValidReport* valid, OutBuffer& out) {
  ValidReport result{};
  static const char* const kBuildModes[] = {"debug", "profile", "release"};
  static const char* const kSources[] = {"flutter", "platform"};
  bool ok = report.root == ValueKind::kObject;
  if (ok) {
    ok &= check_string(report, Field::kPlatform, true, 1, 32, &result.platform);
    if (field_value(report, Field::kAppVersion).kind != ValueKind::kMissing) {
      result.has_app_version =
          check_string(report, Field::kAppVersion, true, 0, 64, &result.app_version);
      ok &= result.has_app_version;
    }
    ok &= check_enum(report, Field::kBuildMode, kBuildModes, 3, &result.build_mode);
    ok &= check_enum(report, Field::kSource, kSources, 2, &result.source);
    const FieldValue& fatal = field_value(report, Field::kFatal);
    if (fatal.kind == ValueKind::kBoolean) result.fatal = fatal.boolean;
    else ok = false;
    ok &= check_string(report, Field::kErrorType, true, 1, 160, &result.error_type);
    ok &= check_string(report, Field::kMessage, false, 0, 4000, &result.message);
    ok &= check_string(report, Field::kStack, false, 0, 24000, &result.stack);
    const FieldValue& occurred = field_value(report, Field::kOccurredAt);
    if (occurred.kind == ValueKind::kString &&
        IsIsoDatetimeWithOffset(occurred.text.data, occurred.text.length))
      result.occurred_at = occurred.text;
    else ok = false;
    if (report.unrecognized_count) ok = false;
  }
  if (!ok) {
    out.Text("{\"error\":\"BAD_REQUEST\"}");
    return false;
  }
  *valid = result;
  return true;
}

bool NewUlid(int64_t now_ms, const uint8_t random[10], char output[kUlidLength + 1]) {
  static const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
  if (!random || now_ms < 0 || static_cast<uint64_t>(now_ms) > 0xFFFFFFFFFFFFULL) return false;
  uint64_t time = static_cast<uint64_t>(now_ms);
  for (int i = 9; i >= 0; --i) {
    output[i] = kAlphabet[time & 31];
    time >>= 5;
  }
  // 80 random bits, five per character, most significant first.
  for (size_t i = 0; i < 16; ++i) {
    const size_t bit = i * 5;
    const unsigned pair = (static_cast<unsigned>(random[bit / 8]) << 8) |
                          (bit / 8 + 1 < 10 ? random[bit / 8 + 1] : 0);
    output[10 + i] = kAlphabet[(pair >> (11 - bit % 8)) & 31];
  }
  output[kUlidLength] = '\0';
  return true;
}

bool FormatIsoTime(int64_t ms, char output[25]) {
  if (ms < 0) return false;
  const time_t seconds = static_cast<time_t>(ms / 1000);
  struct tm parts{};
  if (!gmtime_r(&seconds, &parts) || parts.tm_year + 1900 > 9999) return false;
  return snprintf(output, 25, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", parts.tm_year + 1900,
                  parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min,
                  parts.tm_sec, static_cast<int>(ms % 1000)) == 24;
}

bool Fingerprint(Slice error_type, Slice message, Slice stack, char output[65]) {
  // stack.split('\n').slice(0, 8).join('\n')
  size_t head = stack.length, newlines = 0;
  for (size_t i = 0; i < stack.length; ++i) {
    if (stack.data[i] == '\n' && ++newlines == 8) { head = i; break; }
  }
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  unsigned char digest[32];
  const bool ok = mbedtls_sha256_starts(&context, 0) == 0 &&
      mbedtls_sha256_update(&context, reinterpret_cast<const unsigned char*>(error_type.data),
                            error_type.length) == 0 &&
      mbedtls_sha256_update(&context, reinterpret_cast<const unsigned char*>("\n"), 1) == 0 &&
      mbedtls_sha256_update(&context, reinterpret_cast<const unsigned char*>(message.data),
                            message.length) == 0 &&
      mbedtls_sha256_update(&context, reinterpret_cast<const unsigned char*>("\n"), 1) == 0 &&
      mbedtls_sha256_update(&context, reinterpret_cast<const unsigned char*>(stack.data),
                            head) == 0 &&
      mbedtls_sha256_finish(&context, digest) == 0;
  mbedtls_sha256_free(&context);
  if (!ok) return false;
  for (size_t i = 0; i < 32; ++i) snprintf(output + i * 2, 3, "%02x", digest[i]);
  return true;
}

bool AppendStoredJson(const StoredReport& stored, OutBuffer& out) {
  if (!stored.id || !stored.device_id || !stored.fingerprint || !stored.report) return false;
  char created[25], expires[25];
  if (!FormatIsoTime(stored.created_at_ms, created) ||
      !FormatIsoTime(stored.created_at_ms + kRetentionMs, expires)) return false;
  const ValidReport& report = *stored.report;
  auto field = [&out](const char* name, Slice value) {
    out.Text(",\""); out.Text(name); out.Text("\":");
    AppendJsonString(out, value.data, value.length);
  };
  out.Text("{\"id\":");
  AppendJsonString(out, stored.id, strlen(stored.id));
  field("deviceId", {stored.device_id, strlen(stored.device_id)});
  field("fingerprint", {stored.fingerprint, strlen(stored.fingerprint)});
  field("platform", report.platform);
  if (report.has_app_version) field("appVersion", report.app_version);
  field("buildMode", report.build_mode);
  field("source", report.source);
  out.Text(report.fatal ? ",\"fatal\":true" : ",\"fatal\":false");
  field("errorType", stored.error_type);
  field("message", stored.message);
  field("stack", stored.stack);
  field("occurredAt", report.occurred_at);
  field("createdAt", {created, 24});
  field("expiresAt", {expires, 24});
  out.Byte('}');
  return out.ok;
}

size_t EncodeResponseProtobuf(const char id[kUlidLength + 1], uint8_t* output, size_t capacity) {
  if (!id || !output || capacity < 4 + kUlidLength) return 0;
  output[0] = 0x08; output[1] = 0x01;                     // accepted = true
  output[2] = 0x12; output[3] = static_cast<uint8_t>(kUlidLength);  // id
  memcpy(output + 4, id, kUlidLength);
  return 4 + kUlidLength;
}

}  // namespace fortis::codec::crash_report
