#pragma once

#include "price_history_batch_codec.h"

#include <stddef.h>
#include <stdint.h>

// Mutable JSON DOM for provider payloads and dynamic responses. Objects keep
// source/insertion order; replacing a duplicate changes its value in place.
// Strings contain valid UTF-8. Lone JSON surrogate escapes become U+FFFD;
// malformed raw UTF-8 uses Unicode replacement decoding. A leading BOM is
// stripped from a parsed JSON document, retained inside string content.
// JSON and protobuf Struct output share this representation.
//
// Every node lives in a caller-owned bump arena; nothing is freed
// individually. Guest-safe: no C++ standard-library headers.
namespace fortis::codec::tj {

using history_batch::OutBuffer;

enum class Type : uint8_t { kNull, kFalse, kTrue, kNumber, kString, kArray, kObject };

struct Value;

struct Member {
  const char* key;
  uint32_t key_length;
  Value* value;
};

struct Value {
  Type type = Type::kNull;
  uint32_t count = 0;
  uint32_t capacity = 0;
  uint32_t length = 0;  // string bytes
  double number = 0;
  const char* text = nullptr;  // string bytes (UTF-8, NUL-terminated after `length`)
  Member* members = nullptr;   // objects
  Value** items = nullptr;     // arrays
  // Protobuf Value body size, filled by the protobuf writer's measure pass.
  mutable uint32_t proto_size = 0;
};

struct Arena {
  uint8_t* base = nullptr;
  size_t capacity = 0;
  size_t used = 0;
  bool ok = true;
  // A growing arena (GrowingArena) chains allocated blocks while fewer than
  // `limit` bytes are held; 0 is a fixed caller-owned region.
  size_t limit = 0;
  size_t held = 0;
  void* Alloc(size_t bytes, size_t align = 8);
  char* CopyText(const char* text, size_t length);
};

// A rewind point: Rewind drops everything allocated after MarkOf (and frees
// the blocks a growing arena chained since), so it is valid only while the
// arena still holds the marked block.
struct Mark {
  uint8_t* base;
  size_t used;
};

// The growing block size: 1 MiB less the guest allocator's 32-byte header,
// so on the Unikraft buddy allocator a block is exactly one 256-page order
// (an allocation one byte larger needs a free 2 MiB order). A larger single
// allocation gets a block of its own size.
constexpr size_t kArenaBlockBytes = 1024 * 1024 - 64;

// An arena that holds at most `limit` bytes of blocks (none until the first
// Alloc); a request past the limit fails like a full fixed arena. Release it
// with FreeArena.
Arena GrowingArena(size_t limit);
void FreeArena(Arena& arena);
// Owns a growing arena's blocks, not the Arena object or a borrowed fixed span.
struct ArenaBlocksDeleter {
  void operator()(Arena* arena) const noexcept;
};
Mark MarkOf(const Arena& arena);
void Rewind(Arena& arena, Mark mark);

// Maximum nesting accepted by Parse (provider bodies are shallow; a deeper
// document is a provider failure, a documented native bound).
constexpr size_t kMaxDepth = 64;

Value* Null(Arena& arena);
Value* Bool(Arena& arena, bool value);
Value* Num(Arena& arena, double value);
// Copies `text` into the arena.
Value* Str(Arena& arena, const char* text, size_t length);
Value* Str(Arena& arena, const char* text);
Value* Obj(Arena& arena);
Value* Arr(Arena& arena);

// Object member value or null when absent (or `object` is not an object).
Value* Get(const Value* object, const char* key);
Value* Get(const Value* object, const char* key, size_t key_length);
// Adds or replaces (keeps the position of an existing key). Copies the key.
bool Set(Arena& arena, Value* object, const char* key, Value* value);
bool Set(Arena& arena, Value* object, const char* key, size_t key_length, Value* value);
// `delete object[key]`.
void Remove(Value* object, const char* key);
bool Push(Arena& arena, Value* array, Value* value);

Value* Parse(Arena& arena, const uint8_t* text, size_t length);
Value* Clone(Arena& arena, const Value* value);

// JSON.stringify(value). Non-finite numbers are written as null.
bool Stringify(const Value* value, OutBuffer& out);
// Exactly the bytes protobuf-es writes for google.protobuf.Struct (the body,
// without an outer tag) built from a JSON object.
bool AppendProtoStruct(const Value* object, OutBuffer& out);
// The StructuredReadResponse message {payload: object}.
bool AppendStructuredReadResponse(const Value* object, OutBuffer& out);

// Typed views (the TypeScript `typeof` guards).
inline bool IsString(const Value* v) { return v && v->type == Type::kString; }
inline bool IsNumber(const Value* v) { return v && v->type == Type::kNumber; }
inline bool IsObject(const Value* v) { return v && v->type == Type::kObject; }
inline bool IsArray(const Value* v) { return v && v->type == Type::kArray; }
inline bool IsBool(const Value* v) { return v && (v->type == Type::kTrue || v->type == Type::kFalse); }
bool IsFinite(const Value* v);
bool StringEquals(const Value* v, const char* text);
// A string value as a NUL-terminated arena copy (null when not a string).
const char* CString(Arena& arena, const Value* v);

}  // namespace fortis::codec::tj
