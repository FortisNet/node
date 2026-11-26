#pragma once

#include <stddef.h>
#include <stdint.h>

#include "price_history_batch_codec.h"

namespace fortis::compat {

// This server's display sanitizers for token search: the symbol rule
// (<= 16 UTF-16 units) and the name rule (<= 64), pinned by
// tests/corpus/token_ref_corpus.json
// (docs/compat-decisions.md#text-sanitizers).
//
// Input and output are valid UTF-8. Limits count UTF-16 units, but truncation
// stops before a partial code point, then appends an ellipsis. Both wire
// formats use these same Unicode scalar values.
constexpr size_t kSanitizeFailed = static_cast<size_t>(-1);
constexpr size_t kSanitizeSymbolMaxUnits = 16;
constexpr size_t kSanitizeNameMaxUnits = 64;
// Worst-case output bytes (every unit a 3-byte BMP character plus "…").
constexpr size_t kSanitizeSymbolMaxBytes = 3 * kSanitizeSymbolMaxUnits;
constexpr size_t kSanitizeNameMaxBytes = 3 * kSanitizeNameMaxUnits;

// Returns the bytes written (no terminator is counted; one is written when
// capacity allows), or kSanitizeFailed for invalid UTF-8 or a too-small
// output buffer. A capacity of k...MaxBytes + 1 never fails for valid input.
size_t SanitizeSymbol(const char* input, size_t length, char* output, size_t capacity);
size_t SanitizeName(const char* input, size_t length, char* output, size_t capacity);

// The JSON string literal of a sanitized UTF-8 string, quotes included.
void AppendSanitizedJsonString(fortis::codec::history_batch::OutBuffer& out,
                               const char* text, size_t length);
// Its protobuf string bytes: valid UTF-8.
// Returns bytes written or kSanitizeFailed when the output is too small.
size_t SanitizedToProtobufUtf8(const char* text, size_t length, char* output,
                               size_t capacity);

}  // namespace fortis::compat
