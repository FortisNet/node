#include "js_json.h"

#include "json_lex.h"
#include "proto_wire.h"

#include <string.h>

namespace fortis::compat::js_json {
namespace {

namespace pw = fortis::codec::proto_wire;

// Iterates the scalar values of a string token the parser has validated.
struct Scalars {
  const unsigned char* input;
  size_t position;
  size_t end;

  static uint32_t hex4(const unsigned char* text) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const unsigned char c = text[i];
      value = value << 4 | static_cast<uint32_t>(
          c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return value;
  }

  bool Next(uint32_t* value) {
    if (position >= end) return false;
    const unsigned char c = input[position];
    if (c == '\\') {
      const unsigned char escaped = input[position + 1];
      position += 2;
      switch (escaped) {
        case 'b': *value = '\b'; return true;
        case 'f': *value = '\f'; return true;
        case 'n': *value = '\n'; return true;
        case 'r': *value = '\r'; return true;
        case 't': *value = '\t'; return true;
        case 'u': {
          const uint32_t first = hex4(input + position);
          position += 4;
          if (first >= 0xd800 && first <= 0xdbff) {
            if (end - position >= 6 && input[position] == '\\' && input[position + 1] == 'u') {
              const uint32_t second = hex4(input + position + 2);
              if (second >= 0xdc00 && second <= 0xdfff) {
                position += 6;
                *value = 0x10000 + ((first - 0xd800) << 10) + (second - 0xdc00);
                return true;
              }
            }
            *value = 0xfffd;
          } else {
            *value = first >= 0xdc00 && first <= 0xdfff ? 0xfffd : first;
          }
          return true;
        }
        default: *value = escaped; return true;  // " \ /
      }
    }
    if (c < 0x80) { *value = c; ++position; return true; }
    const size_t trailing = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : 3;
    uint32_t result = c & (trailing == 1 ? 0x1f : trailing == 2 ? 0x0f : 0x07);
    for (size_t i = 1; i <= trailing; ++i) result = result << 6 | (input[position + i] & 0x3f);
    position += trailing + 1;
    *value = result;
    return true;
  }
};

Scalars scalars_of(const codec::BoundedJsonDocument& document, uint32_t index) {
  const codec::BoundedJsonToken* item = document.token(index);
  return Scalars{document.input(), item->start, static_cast<size_t>(item->start) + item->length};
}

bool has_escape(const codec::BoundedJsonDocument& document, uint32_t index) {
  const codec::BoundedJsonToken* item = document.token(index);
  return memchr(document.input() + item->start, '\\', item->length) != nullptr;
}

void append_utf8(OutBuffer& out, uint32_t value) {
  uint8_t bytes[4];
  out.Append(bytes, codec::json_lex::EncodeUtf8(value, bytes));
}

// The decoded UTF-8 bytes of a string token. An unescaped span is already the
// decoded text (the parser rejected raw control characters and bad UTF-8).
void append_decoded(OutBuffer& out, const codec::BoundedJsonDocument& document, uint32_t index) {
  const codec::BoundedJsonToken* item = document.token(index);
  if (!has_escape(document, index)) {
    out.Append(document.input() + item->start, item->length);
    return;
  }
  Scalars text = scalars_of(document, index);
  uint32_t value = 0;
  while (text.Next(&value)) append_utf8(out, value);
}

size_t decoded_length(const codec::BoundedJsonDocument& document, uint32_t index) {
  OutBuffer counter{};
  append_decoded(counter, document, index);
  return counter.used;
}

// JSON.stringify of one string value: the escaped decoded text, quoted.
void append_json_string(OutBuffer& out, const codec::BoundedJsonDocument& document,
                        uint32_t index) {
  const codec::BoundedJsonToken* item = document.token(index);
  out.Byte('"');
  if (!has_escape(document, index)) {
    codec::history_batch::AppendJsonStringBody(out,
        reinterpret_cast<const char*>(document.input() + item->start), item->length);
    out.Byte('"');
    return;
  }
  Scalars text = scalars_of(document, index);
  uint32_t value = 0;
  while (text.Next(&value)) {
    if (value >= 0x20 && value != '"' && value != '\\') { append_utf8(out, value); continue; }
    char escaped[8];
    const char* literal = value == '"' ? "\\\"" : value == '\\' ? "\\\\" :
        value == '\b' ? "\\b" : value == '\f' ? "\\f" : value == '\n' ? "\\n" :
        value == '\r' ? "\\r" : value == '\t' ? "\\t" : nullptr;
    if (!literal) {
      static const char kHex[] = "0123456789abcdef";
      escaped[0] = '\\'; escaped[1] = 'u'; escaped[2] = '0'; escaped[3] = '0';
      escaped[4] = kHex[value >> 4]; escaped[5] = kHex[value & 15]; escaped[6] = 0;
      literal = escaped;
    }
    out.Text(literal);
  }
  out.Byte('"');
}

uint32_t key_hash(const codec::BoundedJsonDocument& document, uint32_t index) {
  Scalars text = scalars_of(document, index);
  uint32_t hash = 2166136261u, value = 0;
  while (text.Next(&value)) { hash ^= value; hash *= 16777619u; }
  return hash;
}

bool keys_equal(const codec::BoundedJsonDocument& document, uint32_t left, uint32_t right) {
  if (!has_escape(document, left) && !has_escape(document, right)) {
    const codec::BoundedJsonToken* a = document.token(left);
    const codec::BoundedJsonToken* b = document.token(right);
    return a->length == b->length &&
        memcmp(document.input() + a->start, document.input() + b->start, a->length) == 0;
  }
  Scalars a = scalars_of(document, left), b = scalars_of(document, right);
  for (;;) {
    uint32_t x = 0, y = 0;
    const bool more_a = a.Next(&x), more_b = b.Next(&y);
    if (more_a != more_b) return false;
    if (!more_a) return true;
    if (x != y) return false;
  }
}

// ECMAScript array index: canonical decimal 0..2^32-2.
// The members of one object in JavaScript property order. `order` receives
// key token indices, `values` the value token index each key finally holds.
// Scratch is taken from `*top` and must be released by the caller.
bool order_members(const codec::BoundedJsonDocument& document, uint32_t object,
                   const Scratch& scratch, size_t* top, uint32_t** order,
                   uint32_t** values, size_t* count) {
  size_t members = 0;
  for (uint32_t key = document.token(object)->first_child; key != codec::kBoundedJsonNoToken;) {
    const uint32_t value = document.token(key)->next_sibling;
    ++members;
    key = document.token(value)->next_sibling;
  }
  size_t table_size = 1;
  while (table_size < members * 2) table_size <<= 1;
  const size_t need = members * 2 + table_size;
  if (need > scratch.capacity - *top) return false;
  uint32_t* keys = scratch.words + *top;
  uint32_t* finals = keys + members;
  uint32_t* table = finals + members;
  *top += need;
  for (size_t i = 0; i < table_size; ++i) table[i] = UINT32_MAX;
  size_t unique = 0;
  for (uint32_t key = document.token(object)->first_child; key != codec::kBoundedJsonNoToken;) {
    const uint32_t value = document.token(key)->next_sibling;
    size_t slot = key_hash(document, key) & (table_size - 1);
    for (;;) {
      if (table[slot] == UINT32_MAX) {
        table[slot] = static_cast<uint32_t>(unique);
        keys[unique] = key;
        finals[unique] = value;
        ++unique;
        break;
      }
      if (keys_equal(document, keys[table[slot]], key)) {
        finals[table[slot]] = value;  // later duplicate wins, first position kept
        break;
      }
      slot = (slot + 1) & (table_size - 1);
    }
    key = document.token(value)->next_sibling;
  }
  // First insertion/source position, with each duplicate's final value.
  *order = keys;
  *values = finals;
  *count = unique;
  return true;
}

bool stringify(const codec::BoundedJsonDocument& document, uint32_t index, OutBuffer& out,
               const Scratch& scratch, size_t* top) {
  const codec::BoundedJsonToken* item = document.token(index);
  if (!item) return false;
  switch (item->type) {
    case codec::BoundedJsonType::kNull: out.Text("null"); return true;
    case codec::BoundedJsonType::kTrue: out.Text("true"); return true;
    case codec::BoundedJsonType::kFalse: out.Text("false"); return true;
    case codec::BoundedJsonType::kNumber: {
      double value = 0;
      if (!document.Number(index, &value)) return false;
      codec::history_batch::AppendJsNumber(out, value);
      return true;
    }
    case codec::BoundedJsonType::kString: append_json_string(out, document, index); return true;
    case codec::BoundedJsonType::kArray: {
      out.Byte('[');
      bool first = true;
      for (uint32_t child = item->first_child; child != codec::kBoundedJsonNoToken;
           child = document.token(child)->next_sibling) {
        if (!first) out.Byte(',');
        first = false;
        if (!stringify(document, child, out, scratch, top)) return false;
      }
      out.Byte(']');
      return true;
    }
    case codec::BoundedJsonType::kObject: {
      const size_t mark = *top;
      uint32_t* keys = nullptr; uint32_t* values = nullptr; size_t count = 0;
      if (!order_members(document, index, scratch, top, &keys, &values, &count)) return false;
      out.Byte('{');
      for (size_t i = 0; i < count; ++i) {
        if (i) out.Byte(',');
        append_json_string(out, document, keys[i]);
        out.Byte(':');
        if (!stringify(document, values[i], out, scratch, top)) return false;
      }
      out.Byte('}');
      *top = mark;
      return true;
    }
  }
  return false;
}

// The google.protobuf.Value / Struct emitter over a bounded document, on the
// shared proto_wire writer (the DOM counterpart is tj::AppendProtoStruct).
// Protobuf sizes are measured once, bottom-up, into one word per token so the
// write pass never re-measures a subtree: `content[i]` is the length of the
// Value's oneof payload (string bytes, ListValue or Struct body).
struct ProtoWriter {
  const codec::BoundedJsonDocument& document;
  const Scratch& scratch;
  uint32_t* content;
  size_t* top;

  size_t ValueSize(uint32_t index) const {
    switch (document.token(index)->type) {
      case codec::BoundedJsonType::kNull:
      case codec::BoundedJsonType::kTrue:
      case codec::BoundedJsonType::kFalse: return pw::kNullOrBoolValueSize;
      case codec::BoundedJsonType::kNumber: return pw::kNumberValueSize;
      default: return pw::DelimitedSize(content[index]);
    }
  }

  bool Measure(uint32_t index) {
    const codec::BoundedJsonToken* item = document.token(index);
    uint64_t length = 0;
    if (item->type == codec::BoundedJsonType::kString) {
      length = decoded_length(document, index);
    } else if (item->type == codec::BoundedJsonType::kArray) {
      for (uint32_t child = item->first_child; child != codec::kBoundedJsonNoToken;
           child = document.token(child)->next_sibling) {
        if (!Measure(child)) return false;
        length += pw::DelimitedSize(ValueSize(child));
      }
    } else if (item->type == codec::BoundedJsonType::kObject) {
      const size_t mark = *top;
      uint32_t* keys = nullptr; uint32_t* values = nullptr; size_t count = 0;
      if (!order_members(document, index, scratch, top, &keys, &values, &count)) return false;
      for (size_t i = 0; i < count; ++i) {
        if (!Measure(values[i])) return false;
        length += pw::DelimitedSize(pw::StructEntrySize(decoded_length(document, keys[i]),
                                                        ValueSize(values[i])));
      }
      *top = mark;
    } else if (item->type == codec::BoundedJsonType::kNumber) {
      double value = 0;
      if (!document.Number(index, &value)) return false;
    }
    if (length > UINT32_MAX) return false;
    content[index] = static_cast<uint32_t>(length);
    return true;
  }

  bool StructBody(uint32_t object, OutBuffer& out) {
    const size_t mark = *top;
    uint32_t* keys = nullptr; uint32_t* values = nullptr; size_t count = 0;
    if (!order_members(document, object, scratch, top, &keys, &values, &count)) return false;
    for (size_t i = 0; i < count; ++i) {
      const size_t value_size = ValueSize(values[i]);
      pw::PutStructEntryKeyPrefix(out, decoded_length(document, keys[i]), value_size);
      append_decoded(out, document, keys[i]);
      pw::PutStructEntryValuePrefix(out, value_size);
      if (!Value(values[i], out)) return false;
    }
    *top = mark;
    return true;
  }

  bool Value(uint32_t index, OutBuffer& out) {
    const codec::BoundedJsonToken* item = document.token(index);
    switch (item->type) {
      case codec::BoundedJsonType::kNull: pw::PutNullValue(out); return true;
      case codec::BoundedJsonType::kTrue:
      case codec::BoundedJsonType::kFalse:
        pw::PutBoolValue(out, item->type == codec::BoundedJsonType::kTrue);
        return true;
      case codec::BoundedJsonType::kNumber: {
        double value = 0;
        if (!document.Number(index, &value)) return false;
        pw::PutNumberValue(out, value);
        return true;
      }
      case codec::BoundedJsonType::kString:
        pw::PutLengthPrefix(out, pw::kValueString, content[index]);
        append_decoded(out, document, index);
        return true;
      case codec::BoundedJsonType::kArray:
        pw::PutLengthPrefix(out, pw::kValueList, content[index]);
        for (uint32_t child = item->first_child; child != codec::kBoundedJsonNoToken;
             child = document.token(child)->next_sibling) {
          pw::PutLengthPrefix(out, pw::kListValues, ValueSize(child));
          if (!Value(child, out)) return false;
        }
        return true;
      case codec::BoundedJsonType::kObject:
        pw::PutLengthPrefix(out, pw::kValueStruct, content[index]);
        return StructBody(index, out);
    }
    return false;
  }
};

// Reserves the per-token size words at the bottom of the scratch.
bool proto_writer_scratch(const codec::BoundedJsonDocument& document, const Scratch& scratch,
                          uint32_t** content, size_t* top) {
  const size_t tokens = document.token_count();
  if (!document.input() || !scratch.words || tokens > scratch.capacity) return false;
  *content = scratch.words;
  *top = tokens;
  return true;
}

}  // namespace

bool Stringify(const codec::BoundedJsonDocument& document, uint32_t value, OutBuffer& out,
               const Scratch& scratch) {
  if (!document.input()) return false;
  size_t top = 0;
  return stringify(document, value, out, scratch, &top) && out.ok;
}

bool AppendProtoValue(const codec::BoundedJsonDocument& document, uint32_t value, OutBuffer& out,
                      const Scratch& scratch) {
  uint32_t* content = nullptr; size_t top = 0;
  if (!document.token(value) || !proto_writer_scratch(document, scratch, &content, &top))
    return false;
  ProtoWriter writer{document, scratch, content, &top};
  return writer.Measure(value) && writer.Value(value, out) && out.ok;
}

bool AppendProtoStruct(const codec::BoundedJsonDocument& document, uint32_t object, OutBuffer& out,
                       const Scratch& scratch) {
  const codec::BoundedJsonToken* item = document.input() ? document.token(object) : nullptr;
  if (!item || item->type != codec::BoundedJsonType::kObject) return false;
  uint32_t* content = nullptr; size_t top = 0;
  if (!proto_writer_scratch(document, scratch, &content, &top)) return false;
  ProtoWriter writer{document, scratch, content, &top};
  return writer.Measure(object) && writer.StructBody(object, out) && out.ok;
}

bool ForEachTopLevelArrayElement(const unsigned char* text, size_t length, bool* is_array,
                                 ArrayElementFn element, void* context) {
  if (!text || !is_array || !element) return false;
  size_t position = 0;
  while (position < length && codec::json_lex::IsSpace(text[position])) ++position;
  *is_array = position < length && text[position] == '[';
  if (!*is_array) return true;
  ++position;
  while (position < length && codec::json_lex::IsSpace(text[position])) ++position;
  if (position < length && text[position] == ']') {
    ++position;
  } else {
    for (;;) {
      const size_t start = position;
      size_t depth = 0;
      bool in_string = false;
      while (position < length) {
        const unsigned char c = text[position];
        if (in_string) {
          if (c == '\\') position += 2;
          else { if (c == '"') in_string = false; ++position; }
          continue;
        }
        if (c == '"') in_string = true;
        else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') {
          if (!depth) break;
          --depth;
        } else if (c == ',' && !depth) break;
        ++position;
      }
      if (position >= length || in_string || depth ||
          (text[position] != ',' && text[position] != ']')) return false;
      size_t end = position;
      while (end > start && codec::json_lex::IsSpace(text[end - 1])) --end;
      if (end == start || !element(context, text + start, end - start)) return false;
      if (text[position] == ']') { ++position; break; }
      ++position;  // ','
      while (position < length && codec::json_lex::IsSpace(text[position])) ++position;
    }
  }
  while (position < length && codec::json_lex::IsSpace(text[position])) ++position;
  return position == length;
}

}  // namespace fortis::compat::js_json
