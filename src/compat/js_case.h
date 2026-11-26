#pragma once

#include <stddef.h>
#include <stdint.h>

// This server's locale-independent Unicode case mapping over valid UTF-8 (no
// surrogate code points): the Unicode default full case mapping, as
// String.prototype.toUpperCase / toLowerCase define it, driven by tables
// generated from ICU data (scripts/generate_js_case_tables.mjs).
// Multi-code-point mappings (ß → SS, İ → i̇) and the Final_Sigma rule of
// lower-casing are applied; there is no other context or locale rule.
// Allocation-free and guest-safe.
//
// Used for user search text, news text and the display casing of
// third-party symbols. Identifiers (coin ids, network ids, addresses, news
// ids) use the ASCII mapping below instead.
namespace fortis::compat {

struct JsCaseMapping {
  uint32_t code_point;
  uint8_t count;
  uint32_t output[3];
};

struct JsCodePointRange {
  uint32_t first;
  uint32_t last;
};

namespace js_case_tables {
extern const char kUnicodeVersion[];
extern const JsCaseMapping kUpper[];
extern const size_t kUpperCount;
extern const JsCaseMapping kLower[];
extern const size_t kLowerCount;
extern const JsCodePointRange kCased[];
extern const size_t kCasedCount;
extern const JsCodePointRange kCaseIgnorable[];
extern const size_t kCaseIgnorableCount;
}  // namespace js_case_tables

// Writes the mapped UTF-8 and a terminating NUL. False when the input is not
// valid UTF-8 or the output (plus NUL) does not fit in `capacity`.
bool JsToUpperCase(const char* input, size_t length, char* output,
                   size_t capacity, size_t* output_length);
bool JsToLowerCase(const char* input, size_t length, char* output,
                   size_t capacity, size_t* output_length);

// The identifier case mapping: only A-Z / a-z change, every other byte is
// copied. Same contract as above: false when the input is not valid UTF-8 or
// the output (plus NUL) does not fit; the output has the input's length.
bool AsciiToUpperCase(const char* input, size_t length, char* output,
                      size_t capacity, size_t* output_length);
bool AsciiToLowerCase(const char* input, size_t length, char* output,
                      size_t capacity, size_t* output_length);

}  // namespace fortis::compat
