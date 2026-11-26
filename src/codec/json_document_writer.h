#pragma once

#include "bounded_json.h"
#include "price_history_batch_codec.h"

#include <stddef.h>
#include <stdint.h>

// Allocation-free JSON and google.protobuf.Struct output for a bounded
// document. Duplicate keys retain their first source position and last value;
// other keys retain source order. Strings use valid UTF-8 with lone JSON
// surrogate escapes replaced by U+FFFD. Numbers are written with the shortest
// digits that round-trip (history_batch::AppendJsNumber).
// The bounded parser rejects non-finite or >=64-byte numeric literals and
// nesting deeper than 16. Callers document any provider refusal bounds.
namespace fortis::codec::json_document_writer {

using codec::history_batch::OutBuffer;

// Words of caller-owned scratch the object-member ordering needs: at most
// eight per token of the parsed document.
constexpr size_t kScratchWordsPerToken = 8;

struct Scratch {
  uint32_t* words = nullptr;
  size_t capacity = 0;
};

bool Stringify(const codec::BoundedJsonDocument& document, uint32_t value,
               OutBuffer& out, const Scratch& scratch);
// The body of one google.protobuf.Value message (no outer tag or length).
bool AppendProtoValue(const codec::BoundedJsonDocument& document, uint32_t value,
                      OutBuffer& out, const Scratch& scratch);
// The body of one google.protobuf.Struct message for an object value.
bool AppendProtoStruct(const codec::BoundedJsonDocument& document, uint32_t object,
                       OutBuffer& out, const Scratch& scratch);

// Walks a top-level JSON array without tokenizing it, calling `element` with
// each element's byte span (whitespace trimmed). Only the array framing is
// validated here; each element must still be parsed by the caller. Returns
// false for a framing error or when `element` returns false. `*is_array` is
// false (and true is returned) when the text is not an array at all.
using ArrayElementFn = bool (*)(void* context, const unsigned char* element,
                                size_t length);
bool ForEachTopLevelArrayElement(const unsigned char* text, size_t length,
                                 bool* is_array, ArrayElementFn element,
                                 void* context);

}  // namespace fortis::codec::json_document_writer
