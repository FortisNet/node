#pragma once

#include "news_text.h"

#include <stddef.h>
#include <stdint.h>

// A small JSON DOM over arena memory for the JSON the news routes cache
// themselves (feed, story and reaction payloads, CoinGecko price samples
// re-serialized by codec::json_document_writer::Stringify). Strings decode to UTF-16 code units,
// lone-surrogate escapes included; every value keeps its source span so an
// element can be re-emitted byte for byte. Depth is bounded at 32.
namespace fortis::news::json {

enum class Type : uint8_t { kNull, kFalse, kTrue, kNumber, kString, kArray, kObject };

struct Value {
  Type type = Type::kNull;
  double number = 0;
  U16 string;
  Value* items = nullptr;  // array elements, or object member values
  U16* keys = nullptr;     // object member names (count entries)
  size_t count = 0;
  const uint8_t* raw = nullptr;
  size_t raw_length = 0;
};

// Null on a syntax error, trailing bytes, depth overflow or arena overflow.
const Value* Parse(const uint8_t* text, size_t length, Arena* arena);
// The last member named `key` (JSON.parse semantics), or null.
const Value* Find(const Value& object, const char* key);

}  // namespace fortis::news::json
