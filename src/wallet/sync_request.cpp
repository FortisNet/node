#include "sync_request.h"

#include "price_history_batch_codec.h"
#include "proto_wire.h"
#include "provider_text.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace tj = fortis::codec::tj;

namespace fortis::wallet::sync {
namespace {

namespace hb = fortis::codec::history_batch;

bool digit(char c) { return c >= '0' && c <= '9'; }

int two_digits(const char* text) { return (text[0] - '0') * 10 + (text[1] - '0'); }

// One UTF-8 code point at *position; a malformed byte is returned alone.
uint32_t next_code_point(const char* text, size_t length, size_t* position) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(text);
  const unsigned char lead = bytes[(*position)++];
  if (lead < 0x80) return lead;
  const size_t extra = (lead & 0xe0) == 0xc0 ? 1 : (lead & 0xf0) == 0xe0 ? 2 : (lead & 0xf8) == 0xf0 ? 3 : 0;
  if (!extra || *position + extra > length) return 0xfffd;
  uint32_t value = lead & (0x3f >> extra);
  for (size_t i = 0; i < extra; ++i) {
    const unsigned char next = bytes[*position];
    if ((next & 0xc0) != 0x80) return 0xfffd;
    value = value << 6 | (next & 0x3f);
    ++*position;
  }
  return value;
}

// JavaScript \s (WhiteSpace and LineTerminator).
bool js_space(uint32_t c) {
  return c == 0x09 || c == 0x0a || c == 0x0b || c == 0x0c || c == 0x0d || c == 0x20 || c == 0xa0 ||
         c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f ||
         c == 0x205f || c == 0x3000 || c == 0xfeff;
}

bool is_leap(int year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }

// ---- JSON values (after JSON.parse) -----------------------------------------------

enum class Kind : uint8_t { kNull, kFalse, kTrue, kNumber, kString, kArray, kObject };

struct JsValue {
  Kind kind = Kind::kNull;
  double number = 0;
  Text text;
  uint32_t token = codec::kBoundedJsonNoToken;
};

// Decode a string token as valid UTF-8, replacing lone surrogate escapes.
bool decode_string(const codec::BoundedJsonDocument& doc, uint32_t index, tj::Arena& arena, Text* out) {
  const codec::BoundedJsonToken* item = doc.token(index);
  if (!item || item->type != codec::BoundedJsonType::kString) return false;
  // Escapes only shrink: a \uXXXX escape is at most three UTF-8 bytes.
  char* text = static_cast<char*>(arena.Alloc(item->length + 1, 1));
  size_t used = 0;
  if (!text || !doc.DecodeString(index, text, item->length + 1, &used)) return false;
  *out = Text{text, used};
  return true;
}

bool load(const codec::BoundedJsonDocument& doc, uint32_t index, tj::Arena& arena, JsValue* out) {
  const codec::BoundedJsonToken* item = doc.token(index);
  if (!item) return false;
  *out = JsValue{};
  out->token = index;
  switch (item->type) {
    case codec::BoundedJsonType::kNull: out->kind = Kind::kNull; return true;
    case codec::BoundedJsonType::kFalse: out->kind = Kind::kFalse; return true;
    case codec::BoundedJsonType::kTrue: out->kind = Kind::kTrue; return true;
    case codec::BoundedJsonType::kNumber: out->kind = Kind::kNumber; return doc.Number(index, &out->number);
    case codec::BoundedJsonType::kString: out->kind = Kind::kString; return decode_string(doc, index, arena, &out->text);
    case codec::BoundedJsonType::kArray: out->kind = Kind::kArray; return true;
    case codec::BoundedJsonType::kObject: out->kind = Kind::kObject; return true;
  }
  return false;
}

size_t array_length(const codec::BoundedJsonDocument& doc, uint32_t array) {
  size_t count = 0;
  for (uint32_t i = doc.token(array)->first_child; i != codec::kBoundedJsonNoToken; i = doc.token(i)->next_sibling)
    ++count;
  return count;
}

// `data[name]`: JSON.parse keeps the last of duplicate keys.
uint32_t member(const codec::BoundedJsonDocument& doc, uint32_t object, const char* name, tj::Arena& arena) {
  const size_t name_length = strlen(name);
  uint32_t found = codec::kBoundedJsonNoToken;
  for (uint32_t key = doc.token(object)->first_child; key != codec::kBoundedJsonNoToken;) {
    const uint32_t value = doc.token(key)->next_sibling;
    const tj::Mark mark = tj::MarkOf(arena);
    Text decoded;
    if (decode_string(doc, key, arena, &decoded) && decoded.length == name_length &&
        memcmp(decoded.data, name, name_length) == 0)
      found = value;
    tj::Rewind(arena, mark);
    key = doc.token(value)->next_sibling;
  }
  return found;
}

// JSON requests keep their declared types. No scalar wrapping, one-element
// array unwrapping, or JavaScript number/string conversion.
bool string_typed(const JsValue& value, Text* out) {
  if (value.kind != Kind::kString) return false;
  *out = value.text;
  return true;
}

bool integer_typed(const JsValue& value, double* out) {
  if (value.kind != Kind::kNumber || !isfinite(value.number) || fmod(value.number, 1) != 0) return false;
  *out = value.number;
  return true;
}

}  // namespace

// ---- date-time grammars -------------------------------------------------------------

bool AjvDateTime(const char* text, size_t length) {
  // str.split(/t|\s/i) must give exactly two parts.
  size_t separator = 0, after = 0, separators = 0;
  for (size_t position = 0; position < length;) {
    const size_t start = position;
    const uint32_t c = next_code_point(text, length, &position);
    if (c == 't' || c == 'T' || js_space(c)) {
      if (++separators > 1) return false;
      separator = start;
      after = position;
    }
  }
  if (separators != 1) return false;
  // full-date: /^(\d\d\d\d)-(\d\d)-(\d\d)$/ with real month lengths.
  static const int kDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const char* date = text;
  if (separator != 10 || !digit(date[0]) || !digit(date[1]) || !digit(date[2]) || !digit(date[3]) ||
      date[4] != '-' || !digit(date[5]) || !digit(date[6]) || date[7] != '-' || !digit(date[8]) || !digit(date[9]))
    return false;
  const int year = two_digits(date) * 100 + two_digits(date + 2);
  const int month = two_digits(date + 5);
  const int day = two_digits(date + 8);
  if (month < 1 || month > 12 || day < 1 || day > (month == 2 && is_leap(year) ? 29 : kDays[month])) return false;
  // time: /^(\d\d):(\d\d):(\d\d(?:\.\d+)?)(z|([+-])(\d\d)(?::?(\d\d))?)?$/i,
  // a time zone required.
  const char* time = text + after;
  const size_t n = length - after;
  if (n < 8 || !digit(time[0]) || !digit(time[1]) || time[2] != ':' || !digit(time[3]) || !digit(time[4]) ||
      time[5] != ':' || !digit(time[6]) || !digit(time[7]))
    return false;
  size_t i = 8;
  if (i < n && time[i] == '.') {
    size_t digits = 0;
    while (i + 1 + digits < n && digit(time[i + 1 + digits])) ++digits;
    if (!digits) return false;
    i += 1 + digits;
  }
  char seconds[64];
  const size_t seconds_length = i - 6;
  if (seconds_length >= sizeof(seconds)) {
    // Any fraction of a two-digit second below 60 keeps it below 60.
    memcpy(seconds, time + 6, 2);
    seconds[2] = '\0';
  } else {
    memcpy(seconds, time + 6, seconds_length);
    seconds[seconds_length] = '\0';
  }
  const double sec = strtod(seconds, nullptr);
  if (i >= n) return false;  // strict time zone
  int sign = 1, tz_hours = 0, tz_minutes = 0;
  if (time[i] == 'z' || time[i] == 'Z') {
    if (i + 1 != n) return false;
  } else if (time[i] == '+' || time[i] == '-') {
    sign = time[i] == '-' ? -1 : 1;
    if (i + 3 > n || !digit(time[i + 1]) || !digit(time[i + 2])) return false;
    tz_hours = two_digits(time + i + 1);
    i += 3;
    if (i < n) {
      if (time[i] == ':') ++i;
      if (i + 2 != n || !digit(time[i]) || !digit(time[i + 1])) return false;
      tz_minutes = two_digits(time + i);
    }
  } else {
    return false;
  }
  const int hours = two_digits(time), minutes = two_digits(time + 3);
  if (tz_hours > 23 || tz_minutes > 59) return false;
  if (hours <= 23 && minutes <= 59 && sec < 60) return true;
  // A leap second at 23:59 UTC.
  const int utc_minutes = minutes - tz_minutes * sign;
  const int utc_hours = hours - tz_hours * sign - (utc_minutes < 0 ? 1 : 0);
  return (utc_hours == 23 || utc_hours == -1) && (utc_minutes == 59 || utc_minutes == -1) && sec < 61;
}

bool ZodDateTime(const char* text, size_t length) {
  const char* s = text;
  if (length < 17) return false;
  static const size_t kDigits[] = {0, 1, 2, 3, 5, 6, 8, 9};
  for (size_t i : kDigits)
    if (!digit(s[i])) return false;
  if (s[4] != '-' || s[7] != '-' || s[10] != 'T') return false;
  const int month = two_digits(s + 5), day = two_digits(s + 8);
  if (month == 2 && day == 29) {
    // \d\d[2468][048]|\d\d[13579][26]|\d\d0[48]|[02468][048]00|[13579][26]00
    const char a = s[0], b = s[1], c = s[2], d = s[3];
    auto in = [](char x, const char* set) { return strchr(set, x) != nullptr; };
    const bool leap = (in(c, "2468") && in(d, "048")) || (in(c, "13579") && in(d, "26")) ||
                      (c == '0' && in(d, "48")) || (in(a, "02468") && in(b, "048") && c == '0' && d == '0') ||
                      (in(a, "13579") && in(b, "26") && c == '0' && d == '0');
    if (!leap) return false;
  } else {
    int last = 0;
    if (month == 1 || month == 3 || month == 5 || month == 7 || month == 8 || month == 10 || month == 12) last = 31;
    else if (month == 4 || month == 6 || month == 9 || month == 11) last = 30;
    else if (month == 2) last = 28;
    if (!last || day < 1 || day > last) return false;
  }
  // (?:[01]\d|2[0-3]):[0-5]\d(?::[0-5]\d(?:\.\d+)?)?
  const char* t = s + 11;
  const size_t n = length - 11;
  if (n < 5 || !digit(t[0]) || !digit(t[1]) || two_digits(t) > 23 || t[2] != ':' || t[3] < '0' || t[3] > '5' ||
      !digit(t[4]))
    return false;
  size_t i = 5;
  if (i < n && t[i] == ':') {
    if (i + 3 > n || t[i + 1] < '0' || t[i + 1] > '5' || !digit(t[i + 2])) return false;
    i += 3;
    if (i < n && t[i] == '.') {
      size_t digits = 0;
      while (i + 1 + digits < n && digit(t[i + 1 + digits])) ++digits;
      if (!digits) return false;
      i += 1 + digits;
    }
  }
  // (?:Z|[+-](?:[01]\d|2[0-3]):[0-5]\d)$
  if (i < n && t[i] == 'Z') return i + 1 == n;
  if (i + 6 != n || (t[i] != '+' && t[i] != '-') || !digit(t[i + 1]) || !digit(t[i + 2]) ||
      two_digits(t + i + 1) > 23 || t[i + 3] != ':' || t[i + 4] < '0' || t[i + 4] > '5' || !digit(t[i + 5]))
    return false;
  return true;
}

// ---- step 1: the body schema ---------------------------------------------------------

bool CheckSchema(const codec::BoundedJsonDocument* json, tj::Arena& arena, Input* input) {
  *input = Input{};
  const uint32_t root = json ? json->root() : codec::kBoundedJsonNoToken;
  if (root == codec::kBoundedJsonNoToken || json->token(root)->type != codec::BoundedJsonType::kObject) return false;
  const codec::BoundedJsonDocument& doc = *json;
  // additionalProperties: false with removeAdditional drops every other key;
  // the declared properties are checked in schema order.
  const uint32_t cursor = member(doc, root, "cursor", arena);
  if (cursor != codec::kBoundedJsonNoToken) {
    JsValue value;
    if (!load(doc, cursor, arena, &value)) return false;
    if (!string_typed(value, &input->cursor)) return false;
    input->has_cursor = true;
  }
  const uint32_t after = member(doc, root, "after", arena);
  if (after != codec::kBoundedJsonNoToken) {
    JsValue value;
    if (!load(doc, after, arena, &value)) return false;
    if (!string_typed(value, &input->after)) return false;
    if (!AjvDateTime(input->after.data, input->after.length)) return false;
    input->has_after = true;
  }
  const uint32_t models = member(doc, root, "models", arena);
  if (models != codec::kBoundedJsonNoToken) {
    JsValue value;
    if (!load(doc, models, arena, &value)) return false;
    if (value.kind != Kind::kArray) return false;
    const size_t count = array_length(doc, value.token);
    input->models = static_cast<Text*>(arena.Alloc(sizeof(Text) * (count ? count : 1)));
    if (!input->models) return false;
    uint32_t item_token = value.kind == Kind::kArray ? doc.token(value.token)->first_child : codec::kBoundedJsonNoToken;
    for (size_t i = 0; i < count; ++i) {
      JsValue item = value;
      if (value.kind == Kind::kArray) {
        if (!load(doc, item_token, arena, &item)) return false;
        item_token = doc.token(item_token)->next_sibling;
      }
      if (!string_typed(item, &input->models[i])) return false;
      if (input->models[i].length != 8 || memcmp(input->models[i].data, "transfer", 8) != 0) return false;
    }
    input->has_models = true;
    input->model_count = count;
  }
  const uint32_t limit = member(doc, root, "limit", arena);
  if (limit != codec::kBoundedJsonNoToken) {
    JsValue value;
    if (!load(doc, limit, arena, &value)) return false;
    if (!integer_typed(value, &input->limit)) return false;
    // The number keywords apply only to a finite number.
    if (isfinite(input->limit)) {
      if (input->limit > 500) return false;
      if (input->limit < 1) return false;
    }
    input->has_limit = true;
  }
  return true;
}

// ---- step 2: protobuf --------------------------------------------------------------

bool DecodeProto(const uint8_t* body, size_t length, tj::Arena& arena, Input* input) {
  *input = Input{};
  if (!body && length) return false;
  size_t model_count = 0;
  for (int pass = 0; pass < 2; ++pass) {
    if (pass == 1 && model_count) {
      input->models = static_cast<Text*>(arena.Alloc(sizeof(Text) * model_count));
      if (!input->models) return false;
    }
    size_t position = 0;
    while (position < length) {
      uint32_t field = 0, wire = 0;
      if (!fortis::codec::proto_wire::ReadTag(body, length, &position, &field, &wire) || wire == 4) return false;
      if (field >= 1 && field <= 3) {
        if (wire != 2) return false;
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (!fortis::codec::proto_wire::ReadBytes(body, length, &position, &data, &size) || !hb::ValidUtf8(data, size))
          return false;
        const Text text{reinterpret_cast<const char*>(data), size};
        if (field == 1) { input->has_cursor = true; input->cursor = text; }
        else if (field == 2) { input->has_after = true; input->after = text; }
        else if (pass == 0) ++model_count;
        else {
          if (!input->models) return false;
          input->models[input->model_count++] = text;
        }
      } else if (field == 4) {
        if (wire != 0) return false;
        uint32_t value = 0;
        if (!fortis::codec::proto_wire::ReadVarint32(body, length, &position, &value)) return false;
        input->has_limit = true;
        input->limit = value;
      } else if (!fortis::codec::proto_wire::SkipField(body, length, &position, wire, field)) {
        return false;
      }
    }
  }
  // `models.length > 0 ? { models } : {}`
  input->has_models = input->model_count > 0;
  return true;
}

// ---- step 3: schema --------------------------------------------------------------------

bool Validate(const Input& input, Request* request) {
  *request = Request{};
  bool valid = true;
  size_t cursor_begin = 0, cursor_end = 0;
  if (input.has_cursor) {
    // .trim().max(64): length in UTF-16 units after trimming.
    hb::JsTrim(input.cursor.data, input.cursor.length, &cursor_begin, &cursor_end);
    if (hb::Utf16Units(input.cursor.data + cursor_begin, cursor_end - cursor_begin) > 64)
      valid = false;
  }
  if (input.has_after && !ZodDateTime(input.after.data, input.after.length)) valid = false;
  bool include_transfer = !input.has_models;
  if (input.has_models) {
    // .min(1).max(1) of "transfer".
    if (input.model_count != 1) valid = false;
    for (size_t i = 0; i < input.model_count; ++i) {
      const Text& model = input.models[i];
      if (model.length == 8 && memcmp(model.data, "transfer", 8) == 0) include_transfer = true;
      else valid = false;
    }
  }
  // .int() cannot fail: ajv admits only integers and protobuf a uint32.
  if (input.has_limit && !(isfinite(input.limit) && input.limit >= 1 && input.limit <= 500))
    valid = false;
  if (input.has_cursor && input.has_after) valid = false;  // mutually exclusive
  if (!valid) return false;
  if (input.has_cursor) {
    const size_t length = cursor_end - cursor_begin;
    if (length >= sizeof(request->cursor)) return false;  // unreachable: <= 64 units
    memcpy(request->cursor, input.cursor.data + cursor_begin, length);
    request->cursor[length] = '\0';
    request->cursor_length = length;
    request->has_cursor = true;
  }
  if (input.has_after) {
    request->has_after = true;
    double ms = 0;
    request->after_ms = fortis::codec::provider_text::DateParse(input.after.data, input.after.length, &ms) ? ms : NAN;
  }
  request->include_transfer = include_transfer;
  request->limit = input.has_limit ? static_cast<uint32_t>(input.limit) : 200;
  return true;
}

}  // namespace fortis::wallet::sync
