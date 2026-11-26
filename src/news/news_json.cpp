#include "news_json.h"

#include "json_lex.h"

#include <stdlib.h>
#include <string.h>

namespace fortis::news::json {
namespace {

namespace lex = fortis::codec::json_lex;

constexpr size_t kMaxDepth = 32;

struct Parser {
  const uint8_t* p;
  const uint8_t* end;
  Arena* arena;

  void ws() { while (p < end && lex::IsSpace(*p)) ++p; }

  bool string(U16* output) {
    if (p >= end || *p != '"') return false;
    ++p;
    const uint8_t* start = p;
    size_t capacity = 0;
    while (p < end && *p != '"') {
      if (*p == '\\') ++p;
      ++p;
      ++capacity;
    }
    if (p >= end) return false;
    p = start;
    uint16_t* units = static_cast<uint16_t*>(arena->Alloc((capacity + 1) * 2, 2));
    if (!units) return false;
    size_t used = 0;
    while (*p != '"') {
      const uint8_t c = *p++;
      if (c < 0x20) return false;
      if (c == '\\') {
        const uint8_t e = *p++;
        uint16_t unit = 0;
        switch (e) {
          case '"': unit = '"'; break;
          case '\\': unit = '\\'; break;
          case '/': unit = '/'; break;
          case 'b': unit = 8; break;
          case 'f': unit = 12; break;
          case 'n': unit = 10; break;
          case 'r': unit = 13; break;
          case 't': unit = 9; break;
          case 'u': {
            uint32_t v = 0;
            if (end - p < 4 || !lex::Hex4(p, &v)) return false;
            p += 4;
            unit = static_cast<uint16_t>(v);
            break;
          }
          default: return false;
        }
        units[used++] = unit;
        continue;
      }
      uint32_t cp = c;
      size_t extra = 0;
      if (c >= 0xf0 && c <= 0xf4) { cp = c & 7; extra = 3; }
      else if (c >= 0xe0) { cp = c & 15; extra = 2; }
      else if (c >= 0xc2 && c < 0xe0) { cp = c & 31; extra = 1; }
      else if (c >= 0x80) return false;
      if (static_cast<size_t>(end - p) < extra) return false;
      for (size_t i = 0; i < extra; ++i) {
        if ((p[i] & 0xc0) != 0x80) return false;
        cp = cp << 6 | (p[i] & 0x3f);
      }
      p += extra;
      if (cp >= 0x10000) {
        cp -= 0x10000;
        units[used++] = static_cast<uint16_t>(0xd800 + (cp >> 10));
        units[used++] = static_cast<uint16_t>(0xdc00 + (cp & 0x3ff));
      } else {
        units[used++] = static_cast<uint16_t>(cp);
      }
    }
    ++p;
    output->data = units;
    output->length = used;
    return true;
  }

  bool number(double* value) {
    const uint8_t* start = p;
    const uint8_t* after = lex::ScanNumber(p, end);
    if (!after) return false;
    p = after;
    char text[64];
    const size_t n = static_cast<size_t>(p - start);
    if (n >= sizeof(text)) return false;
    memcpy(text, start, n);
    text[n] = '\0';
    *value = strtod(text, nullptr);
    return true;
  }

  bool literal(const char* text) {
    const size_t n = strlen(text);
    if (static_cast<size_t>(end - p) < n || memcmp(p, text, n) != 0) return false;
    p += n;
    return true;
  }

  // Collects children in a growing arena array.
  template <typename T>
  bool push(T** items, size_t* count, size_t* capacity, const T& value) {
    if (*count == *capacity) {
      const size_t next = *capacity ? *capacity * 2 : 4;
      T* grown = static_cast<T*>(arena->Alloc(next * sizeof(T), alignof(T)));
      if (!grown) return false;
      if (*count) memcpy(static_cast<void*>(grown), *items, *count * sizeof(T));
      *items = grown;
      *capacity = next;
    }
    (*items)[(*count)++] = value;
    return true;
  }

  bool value(Value* out, size_t depth) {
    ws();
    if (p >= end || depth > kMaxDepth) return false;
    *out = Value{};
    out->raw = p;
    const uint8_t c = *p;
    bool ok = false;
    if (c == '"') {
      out->type = Type::kString;
      ok = string(&out->string);
    } else if (c == '{' || c == '[') {
      const bool object = c == '{';
      const auto closing = static_cast<uint8_t>(object ? '}' : ']');
      out->type = object ? Type::kObject : Type::kArray;
      ++p;
      size_t capacity = 0, key_capacity = 0, key_count = 0;
      ws();
      if (p < end && *p == closing) {
        ++p;
        ok = true;
      } else {
        for (;;) {
          if (object) {
            ws();
            U16 key;
            if (!string(&key)) return false;
            ws();
            if (p >= end || *p != ':') return false;
            ++p;
            if (!push(&out->keys, &key_count, &key_capacity, key)) return false;
          }
          Value child;
          if (!value(&child, depth + 1) || !push(&out->items, &out->count, &capacity, child))
            return false;
          ws();
          if (p < end && *p == ',') { ++p; continue; }
          if (p < end && *p == closing) { ++p; ok = true; break; }
          return false;
        }
      }
    } else if (c == 't') {
      out->type = Type::kTrue;
      ok = literal("true");
    } else if (c == 'f') {
      out->type = Type::kFalse;
      ok = literal("false");
    } else if (c == 'n') {
      out->type = Type::kNull;
      ok = literal("null");
    } else {
      out->type = Type::kNumber;
      ok = number(&out->number);
    }
    if (!ok) return false;
    out->raw_length = static_cast<size_t>(p - out->raw);
    return true;
  }
};

}  // namespace

const Value* Parse(const uint8_t* text, size_t length, Arena* arena) {
  Parser parser{text, text + length, arena};
  Value* root = static_cast<Value*>(arena->Alloc(sizeof(Value), alignof(Value)));
  if (!root || !parser.value(root, 0)) return nullptr;
  parser.ws();
  if (parser.p != parser.end || !arena->ok) return nullptr;
  return root;
}

const Value* Find(const Value& object, const char* key) {
  if (object.type != Type::kObject) return nullptr;
  for (size_t i = object.count; i-- > 0;)
    if (EqualAscii(object.keys[i], key)) return &object.items[i];
  return nullptr;
}

}  // namespace fortis::news::json
