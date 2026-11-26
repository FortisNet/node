#include <algorithm>
#include "ownership.h"
#include "token_json.h"
#include "utf8.h"

#include "json_lex.h"
#include "proto_wire.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace fortis::codec::tj {
namespace {

using history_batch::AppendJsNumber;
namespace pw = fortis::codec::proto_wire;
namespace lex = fortis::codec::json_lex;

Value* make(Arena& arena, Type type) {
  auto* v = static_cast<Value*>(arena.Alloc(sizeof(Value)));
  if (!v) return nullptr;
  *v = Value{};
  v->type = type;
  return v;
}

bool grow(Arena& arena, Value* v, size_t element) {
  if (v->count < v->capacity) return true;
  const uint32_t next = v->capacity ? v->capacity * 2 : 4;
  void* storage = arena.Alloc(element * next);
  if (!storage) return false;
  if (v->type == Type::kObject) {
    if (v->count) memcpy(storage, v->members, element * v->count);
    v->members = static_cast<Member*>(storage);
  } else {
    if (v->count) std::copy_n(v->items, v->count, static_cast<Value**>(storage));
    v->items = static_cast<Value**>(storage);
  }
  v->capacity = next;
  return true;
}

// ---- Unicode helpers ----------------------------------------------------------

// Append a Unicode scalar, replacing surrogate code points.
void put_code_point(char* out, size_t* used, uint32_t cp) {
  if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
  *used += lex::EncodeUtf8(cp, reinterpret_cast<uint8_t*>(out) + *used);
}

// WHATWG UTF-8 decode with replacement (maximal subparts), BOM stripped.
// Returns the original text when it is already valid and has no BOM.
const uint8_t* whatwg_decode(Arena& arena, const uint8_t* text, size_t length, size_t* out_length, bool strip_bom = true) {
  size_t start = 0;
  if (strip_bom && length >= 3 && text[0] == 0xEF && text[1] == 0xBB && text[2] == 0xBF) start = 3;
  if (start == 0 && history_batch::ValidUtf8(text, length)) { *out_length = length; return text; }
  const size_t size = codec::utf8::Normalize(text + start, length - start, nullptr, 0);
  char* out = static_cast<char*>(arena.Alloc(size + 1, 1));
  if (!out) return nullptr;
  codec::utf8::Normalize(text + start, length - start, out, size);
  out[size] = '\0';
  *out_length = size;
  return reinterpret_cast<const uint8_t*>(out);
}

// ---- parser -----------------------------------------------------------------

struct Parser {
  Arena& arena;
  const uint8_t* p;
  const uint8_t* end;
  size_t depth = 0;

  void ws() { while (p < end && lex::IsSpace(*p)) ++p; }

  bool hex4(const uint8_t* at, uint32_t* value) { return end - at >= 4 && lex::Hex4(at, value); }

  // At the opening quote. Decodes into the arena.
  bool string(const char** text, uint32_t* length) {
    ++p;
    const uint8_t* start = p;
    bool escapes = false;
    while (p < end && *p != '"') {
      if (*p < 0x20) return false;
      if (*p == '\\') { escapes = true; p += 2; continue; }
      ++p;
    }
    if (p >= end) return false;
    const size_t raw = static_cast<size_t>(p - start);
    if (!escapes) {
      char* copy = arena.CopyText(reinterpret_cast<const char*>(start), raw);
      if (!copy) return false;
      *text = copy; *length = static_cast<uint32_t>(raw); ++p;
      return true;
    }
    char* out = static_cast<char*>(arena.Alloc(raw + 1, 1));
    if (!out) return false;
    size_t used = 0;
    for (const uint8_t* q = start; q < p;) {
      if (*q != '\\') { out[used++] = static_cast<char>(*q++); continue; }
      if (q + 1 >= p) return false;
      const uint8_t e = q[1];
      q += 2;
      switch (e) {
        case '"': out[used++] = '"'; break;
        case '\\': out[used++] = '\\'; break;
        case '/': out[used++] = '/'; break;
        case 'b': out[used++] = '\b'; break;
        case 'f': out[used++] = '\f'; break;
        case 'n': out[used++] = '\n'; break;
        case 'r': out[used++] = '\r'; break;
        case 't': out[used++] = '\t'; break;
        case 'u': {
          uint32_t unit = 0;
          if (q + 4 > p || !hex4(q, &unit)) return false;
          q += 4;
          if (unit >= 0xD800 && unit <= 0xDBFF && q + 6 <= p && q[0] == '\\' && q[1] == 'u') {
            uint32_t low = 0;
            if (hex4(q + 2, &low) && low >= 0xDC00 && low <= 0xDFFF) {
              q += 6;
              put_code_point(out, &used, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
              break;
            }
          }
          put_code_point(out, &used, unit);  // BMP or replacement for a lone surrogate
          break;
        }
        default: return false;
      }
    }
    out[used] = '\0';  // every string value is NUL-terminated (raw + 1 bytes)
    *text = out; *length = static_cast<uint32_t>(used); ++p;
    return true;
  }

  Value* number() {
    // JSON.parse number semantics: the literal becomes the nearest double
    // (strtod), with no length bound; an out-of-range literal is +-Infinity.
    const uint8_t* start = p;
    const uint8_t* after = lex::ScanNumber(p, end);
    if (!after) return nullptr;
    p = after;
    const size_t length = static_cast<size_t>(p - start);
    char* copy = arena.CopyText(reinterpret_cast<const char*>(start), length);
    if (!copy) return nullptr;
    Value* v = make(arena, Type::kNumber);
    if (!v) return nullptr;
    v->number = strtod(copy, nullptr);
    return v;
  }

  bool literal(const char* word) {
    const size_t n = strlen(word);
    if (static_cast<size_t>(end - p) < n || memcmp(p, word, n) != 0) return false;
    p += n;
    return true;
  }

  Value* value() {
    ws();
    if (p >= end) return nullptr;
    switch (*p) {
      case '{': {
        if (++depth > kMaxDepth) return nullptr;
        ++p;
        Value* object = make(arena, Type::kObject);
        if (!object) return nullptr;
        ws();
        if (p < end && *p == '}') { ++p; --depth; return object; }
        for (;;) {
          ws();
          if (p >= end || *p != '"') return nullptr;
          const char* key = nullptr; uint32_t key_length = 0;
          if (!string(&key, &key_length)) return nullptr;
          ws();
          if (p >= end || *p != ':') return nullptr;
          ++p;
          Value* member = value();
          if (!member) return nullptr;
          if (!Set(arena, object, key, key_length, member)) return nullptr;
          ws();
          if (p < end && *p == ',') { ++p; continue; }
          if (p < end && *p == '}') { ++p; break; }
          return nullptr;
        }
        --depth;
        return object;
      }
      case '[': {
        if (++depth > kMaxDepth) return nullptr;
        ++p;
        Value* array = make(arena, Type::kArray);
        if (!array) return nullptr;
        ws();
        if (p < end && *p == ']') { ++p; --depth; return array; }
        for (;;) {
          Value* item = value();
          if (!item || !Push(arena, array, item)) return nullptr;
          ws();
          if (p < end && *p == ',') { ++p; continue; }
          if (p < end && *p == ']') { ++p; break; }
          return nullptr;
        }
        --depth;
        return array;
      }
      case '"': {
        Value* v = make(arena, Type::kString);
        if (!v || !string(&v->text, &v->length)) return nullptr;
        return v;
      }
      case 't': return literal("true") ? make(arena, Type::kTrue) : nullptr;
      case 'f': return literal("false") ? make(arena, Type::kFalse) : nullptr;
      case 'n': return literal("null") ? make(arena, Type::kNull) : nullptr;
      default: return number();
    }
  }
};

// ---- stringify --------------------------------------------------------------

constexpr char kHex[] = "0123456789abcdef";

void append_wtf8_json(OutBuffer& out, const char* text, size_t length) {
  out.Byte('"');
  for (size_t i = 0; i < length;) {
    const uint8_t c = static_cast<uint8_t>(text[i]);
    if (c == '"') { out.Text("\\\""); ++i; continue; }
    if (c == '\\') { out.Text("\\\\"); ++i; continue; }
    if (c < 0x20) {
      const char* named = c == '\b' ? "\\b" : c == '\f' ? "\\f" : c == '\n' ? "\\n"
          : c == '\r' ? "\\r" : c == '\t' ? "\\t" : nullptr;
      if (named) out.Text(named);
      else { out.Text("\\u00"); out.Byte(static_cast<uint8_t>(kHex[c >> 4])); out.Byte(static_cast<uint8_t>(kHex[c & 15])); }
      ++i;
      continue;
    }
    if (c >= 0x80) {
      const uint32_t cp = fortis::codec::utf8::Next(reinterpret_cast<const uint8_t*>(text), length, &i);
      uint8_t bytes[4];
      out.Append(bytes, lex::EncodeUtf8(cp, bytes));
      continue;
    }
    out.Byte(c);
    ++i;
  }
  out.Byte('"');
}

bool stringify(const Value* v, OutBuffer& out, size_t depth) {
  if (!v || depth > 256) return false;
  switch (v->type) {
    case Type::kNull: out.Text("null"); return true;
    case Type::kTrue: out.Text("true"); return true;
    case Type::kFalse: out.Text("false"); return true;
    case Type::kNumber: AppendJsNumber(out, v->number); return true;
    case Type::kString: append_wtf8_json(out, v->text, v->length); return true;
    case Type::kArray:
      out.Byte('[');
      for (uint32_t i = 0; i < v->count; ++i) {
        if (i) out.Byte(',');
        if (!stringify(v->items[i], out, depth + 1)) return false;
      }
      out.Byte(']');
      return true;
    case Type::kObject:
      out.Byte('{');
      for (uint32_t i = 0; i < v->count; ++i) {
        if (i) out.Byte(',');
        append_wtf8_json(out, v->members[i].key, v->members[i].key_length);
        out.Byte(':');
        if (!stringify(v->members[i].value, out, depth + 1)) return false;
      }
      out.Byte('}');
      return true;
  }
  return false;
}

// ---- protobuf ---------------------------------------------------------------
//
// google.protobuf.Struct over the DOM, on the shared proto_wire writer (the
// bounded-document counterpart is codec::json_document_writer::AppendProtoStruct).

void utf8(OutBuffer& out, const char* text, size_t length) {
  for (size_t i = 0; i < length;) {
    const uint32_t cp = fortis::codec::utf8::Next(reinterpret_cast<const uint8_t*>(text), length, &i);
    uint8_t bytes[4];
    out.Append(bytes, lex::EncodeUtf8(cp, bytes));
  }
}
size_t utf8_size(const char* text, size_t length) {
  size_t size = 0;
  for (size_t i = 0; i < length;) {
    const uint32_t cp = fortis::codec::utf8::Next(reinterpret_cast<const uint8_t*>(text), length, &i);
    size += cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
  }
  return size;
}

size_t entry_size(const Member& m) {
  return pw::StructEntrySize(utf8_size(m.key, m.key_length), m.value->proto_size);
}

// Bottom-up sizes of every Value body; false past the depth bound or 4 GiB.
bool measure(const Value* v, size_t depth) {
  if (!v || depth > 256) return false;
  uint64_t size = 0;
  switch (v->type) {
    case Type::kNull: case Type::kTrue: case Type::kFalse: size = pw::kNullOrBoolValueSize; break;
    case Type::kNumber: size = pw::kNumberValueSize; break;
    case Type::kString: size = pw::DelimitedSize(utf8_size(v->text, v->length)); break;
    case Type::kArray: {
      uint64_t body = 0;
      for (uint32_t i = 0; i < v->count; ++i) {
        if (!measure(v->items[i], depth + 1)) return false;
        body += pw::DelimitedSize(v->items[i]->proto_size);
      }
      size = pw::DelimitedSize(body);
      break;
    }
    case Type::kObject: {
      uint64_t body = 0;
      for (uint32_t i = 0; i < v->count; ++i) {
        if (!measure(v->members[i].value, depth + 1)) return false;
        body += pw::DelimitedSize(entry_size(v->members[i]));
      }
      size = pw::DelimitedSize(body);
      break;
    }
  }
  if (size > 0xFFFFFFFFULL) return false;
  v->proto_size = static_cast<uint32_t>(size);
  return true;
}

void write_value(const Value* v, OutBuffer& out);

void write_struct_body(const Value* object, OutBuffer& out) {
  for (uint32_t i = 0; i < object->count; ++i) {
    const Member& m = object->members[i];
    pw::PutStructEntryKeyPrefix(out, utf8_size(m.key, m.key_length), m.value->proto_size);
    utf8(out, m.key, m.key_length);
    pw::PutStructEntryValuePrefix(out, m.value->proto_size);
    write_value(m.value, out);
  }
}

size_t struct_body_size(const Value* object) {
  size_t body = 0;
  for (uint32_t i = 0; i < object->count; ++i) body += pw::DelimitedSize(entry_size(object->members[i]));
  return body;
}

void write_value(const Value* v, OutBuffer& out) {
  switch (v->type) {
    case Type::kNull: pw::PutNullValue(out); return;
    case Type::kTrue: case Type::kFalse: pw::PutBoolValue(out, v->type == Type::kTrue); return;
    case Type::kNumber: pw::PutNumberValue(out, v->number); return;
    case Type::kString:
      pw::PutLengthPrefix(out, pw::kValueString, utf8_size(v->text, v->length));
      utf8(out, v->text, v->length);
      return;
    case Type::kArray: {
      size_t body = 0;
      for (uint32_t i = 0; i < v->count; ++i) body += pw::DelimitedSize(v->items[i]->proto_size);
      pw::PutLengthPrefix(out, pw::kValueList, body);
      for (uint32_t i = 0; i < v->count; ++i) {
        pw::PutLengthPrefix(out, pw::kListValues, v->items[i]->proto_size);
        write_value(v->items[i], out);
      }
      return;
    }
    case Type::kObject:
      pw::PutLengthPrefix(out, pw::kValueStruct, struct_body_size(v));
      write_struct_body(v, out);
      return;
  }
}

}  // namespace

namespace {

// The head of each block of a growing arena: the block it was chained after.
struct ArenaBlock {
  uint8_t* prev_base;
  size_t prev_capacity;
  size_t prev_used;
};
constexpr size_t kArenaBlockHead = (sizeof(ArenaBlock) + 15) & ~static_cast<size_t>(15);

bool chain_block(Arena& arena, size_t bytes, size_t align) {
  if (align > 16 || bytes > SIZE_MAX - kArenaBlockHead - align) return false;
  size_t size = kArenaBlockHead + bytes + align;
  if (size < kArenaBlockBytes) size = kArenaBlockBytes;
  if (arena.held > arena.limit || size > arena.limit - arena.held) return false;
  auto storage = base::AllocateStorage<uint8_t[]>(size);
  auto* block = storage.get();
  if (!block) return false;
  ArenaBlock* head = reinterpret_cast<ArenaBlock*>(block);
  head->prev_base = arena.base;
  head->prev_capacity = arena.capacity;
  head->prev_used = arena.used;
  arena.base = storage.release();
  arena.capacity = size;
  arena.used = kArenaBlockHead;
  arena.held += size;
  return true;
}

// Frees the current block of a growing arena and returns to the previous one.
void pop_block(Arena& arena) {
  const ArenaBlock* head = reinterpret_cast<const ArenaBlock*>(arena.base);
  uint8_t* block = arena.base;
  arena.held -= arena.capacity;
  arena.base = head->prev_base;
  arena.capacity = head->prev_capacity;
  arena.used = head->prev_used;
  std::unique_ptr<uint8_t[], base::StorageDeleter> storage(block);
}

}  // namespace

void* Arena::Alloc(size_t bytes, size_t align) {
  if (!ok) return nullptr;
  size_t start = (used + align - 1) & ~(align - 1);
  if (start > capacity || bytes > capacity - start) {
    if (!limit || !chain_block(*this, bytes, align)) { ok = false; return nullptr; }
    start = (used + align - 1) & ~(align - 1);
  }
  used = start + bytes;
  return base + start;
}

Arena GrowingArena(size_t limit) {
  Arena arena;
  arena.limit = limit;
  return arena;
}

void FreeArena(Arena& arena) {
  if (arena.limit)
    while (arena.base) pop_block(arena);
  arena.used = 0;
}

void ArenaBlocksDeleter::operator()(Arena* arena) const noexcept { FreeArena(*arena); }

Mark MarkOf(const Arena& arena) { return Mark{arena.base, arena.used}; }

void Rewind(Arena& arena, Mark mark) {
  if (arena.limit)
    while (arena.base && arena.base != mark.base) pop_block(arena);
  if (arena.base == mark.base) arena.used = mark.used;
}

char* Arena::CopyText(const char* text, size_t length) {
  char* copy = static_cast<char*>(Alloc(length + 1, 1));
  if (!copy) return nullptr;
  if (length) memcpy(copy, text, length);
  copy[length] = '\0';
  return copy;
}

Value* Null(Arena& arena) { return make(arena, Type::kNull); }
Value* Bool(Arena& arena, bool value) { return make(arena, value ? Type::kTrue : Type::kFalse); }
Value* Num(Arena& arena, double value) {
  Value* v = make(arena, Type::kNumber);
  if (v) v->number = value;
  return v;
}
Value* Str(Arena& arena, const char* text, size_t length) {
  Value* v = make(arena, Type::kString);
  if (!v) return nullptr;
  size_t normalized_length = 0;
  const auto* normalized = whatwg_decode(arena, reinterpret_cast<const uint8_t*>(text ? text : ""),
                                         text ? length : 0, &normalized_length, false);
  if (!normalized) return nullptr;
  char* copy = arena.CopyText(reinterpret_cast<const char*>(normalized), normalized_length);
  if (!copy) return nullptr;
  v->text = copy;
  v->length = static_cast<uint32_t>(normalized_length);
  return v;
}
Value* Str(Arena& arena, const char* text) { return Str(arena, text, text ? strlen(text) : 0); }
Value* Obj(Arena& arena) { return make(arena, Type::kObject); }
Value* Arr(Arena& arena) { return make(arena, Type::kArray); }

Value* Get(const Value* object, const char* key, size_t key_length) {
  if (!object || object->type != Type::kObject) return nullptr;
  for (uint32_t i = 0; i < object->count; ++i)
    if (object->members[i].key_length == key_length &&
        memcmp(object->members[i].key, key, key_length) == 0) return object->members[i].value;
  return nullptr;
}
Value* Get(const Value* object, const char* key) { return Get(object, key, strlen(key)); }

bool Set(Arena& arena, Value* object, const char* key, size_t key_length, Value* value) {
  if (!object || object->type != Type::kObject || !value) return false;
  size_t normalized_length = 0;
  const auto* normalized = whatwg_decode(arena, reinterpret_cast<const uint8_t*>(key), key_length,
                                         &normalized_length, false);
  if (!normalized) return false;
  key = reinterpret_cast<const char*>(normalized);
  key_length = normalized_length;
  for (uint32_t i = 0; i < object->count; ++i)
    if (object->members[i].key_length == key_length &&
        memcmp(object->members[i].key, key, key_length) == 0) {
      object->members[i].value = value;
      return true;
    }
  if (!grow(arena, object, sizeof(Member))) return false;
  char* copy = arena.CopyText(key, key_length);
  if (!copy) return false;
  uint32_t position = object->count;
  object->members[position] = Member{copy, static_cast<uint32_t>(key_length), value};
  ++object->count;
  return true;
}
bool Set(Arena& arena, Value* object, const char* key, Value* value) {
  return Set(arena, object, key, strlen(key), value);
}

void Remove(Value* object, const char* key) {
  if (!object || object->type != Type::kObject) return;
  const size_t key_length = strlen(key);
  for (uint32_t i = 0; i < object->count; ++i)
    if (object->members[i].key_length == key_length &&
        memcmp(object->members[i].key, key, key_length) == 0) {
      memmove(object->members + i, object->members + i + 1,
              sizeof(Member) * (object->count - i - 1));
      --object->count;
      return;
    }
}

bool Push(Arena& arena, Value* array, Value* value) {
  if (!array || array->type != Type::kArray || !value) return false;
  if (!grow(arena, array, sizeof(Value*))) return false;
  array->items[array->count++] = value;
  return true;
}

Value* Parse(Arena& arena, const uint8_t* text, size_t length) {
  if (!text) return nullptr;
  size_t decoded_length = 0;
  const uint8_t* decoded = whatwg_decode(arena, text, length, &decoded_length);
  if (!decoded) return nullptr;
  Parser parser{arena, decoded, decoded + decoded_length};
  Value* v = parser.value();
  if (!v) return nullptr;
  parser.ws();
  if (parser.p != parser.end) return nullptr;
  return arena.ok ? v : nullptr;
}

Value* Clone(Arena& arena, const Value* value) {
  if (!value) return nullptr;
  switch (value->type) {
    case Type::kNull: case Type::kTrue: case Type::kFalse: return make(arena, value->type);
    case Type::kNumber: return Num(arena, value->number);
    case Type::kString: return Str(arena, value->text, value->length);
    case Type::kArray: {
      Value* out = Arr(arena);
      if (!out) return nullptr;
      for (uint32_t i = 0; i < value->count; ++i) {
        Value* item = Clone(arena, value->items[i]);
        if (!item || !Push(arena, out, item)) return nullptr;
      }
      return out;
    }
    case Type::kObject: {
      Value* out = Obj(arena);
      if (!out) return nullptr;
      for (uint32_t i = 0; i < value->count; ++i) {
        Value* member = Clone(arena, value->members[i].value);
        if (!member || !Set(arena, out, value->members[i].key, value->members[i].key_length, member))
          return nullptr;
      }
      return out;
    }
  }
  return nullptr;
}

bool Stringify(const Value* value, OutBuffer& out) { return stringify(value, out, 0) && out.ok; }

bool AppendProtoStruct(const Value* object, OutBuffer& out) {
  if (!object || object->type != Type::kObject || !measure(object, 0)) return false;
  write_struct_body(object, out);
  return out.ok;
}

bool AppendStructuredReadResponse(const Value* object, OutBuffer& out) {
  if (!object || object->type != Type::kObject || !measure(object, 0)) return false;
  pw::PutLengthPrefix(out, 1, struct_body_size(object));  // StructuredReadResponse.payload
  write_struct_body(object, out);
  return out.ok;
}

bool IsFinite(const Value* v) { return v && v->type == Type::kNumber && isfinite(v->number); }

bool StringEquals(const Value* v, const char* text) {
  if (!IsString(v)) return false;
  const size_t length = strlen(text);
  return v->length == length && memcmp(v->text, text, length) == 0;
}

const char* CString(Arena& arena, const Value* v) {
  if (!IsString(v)) return nullptr;
  return arena.CopyText(v->text, v->length);
}

}  // namespace fortis::codec::tj
