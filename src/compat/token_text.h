#pragma once

#include "token_json.h"

#include <stddef.h>
#include <stdint.h>

// String helpers for the token, address, exchange and AI routes. Lengths and
// slices are in UTF-16 code units: that is the unit this server's text
// limits are defined in (docs/compat-decisions.md#bounds). Guest-safe.
//
// Strings contain Unicode scalar values in UTF-8; malformed input becomes
// U+FFFD. Slices exclude code points crossing either unit boundary.
namespace fortis::compat::token_text {

// Decodes one Unicode scalar, replacing malformed UTF-8.
uint32_t DecodeAt(const char* text, size_t length, size_t* position);
// Appends one scalar in UTF-8; surrogate values become U+FFFD.
void AppendCodePoint(codec::tj::OutBuffer& out, uint32_t cp);
// String.prototype.length.
size_t Utf16Length(const char* text, size_t length);
// String.prototype.slice(begin, end) with non-negative unit offsets
// (end clamps to the length), excluding any partial code point.
codec::tj::Value* Slice(codec::tj::Arena& arena, const char* text, size_t length, size_t begin, size_t end);
// String.prototype.trim (ECMAScript WhiteSpace and LineTerminator).
void Trim(const char* text, size_t length, size_t* begin, size_t* end);
bool IsJsWhitespace(uint32_t cp);

// The display sanitizers for third-party text
// (docs/compat-decisions.md#text-sanitizers): symbol (16 units), name (64)
// and text (2000, newlines kept, \n{3,} -> \n\n). Null on arena failure.
codec::tj::Value* SanitizeSymbol(codec::tj::Arena& arena, const char* text, size_t length);
codec::tj::Value* SanitizeName(codec::tj::Arena& arena, const char* text, size_t length);
codec::tj::Value* SanitizeText(codec::tj::Arena& arena, const char* text, size_t length);

// Number.prototype.toFixed(digits) for 0 <= digits <= 20, exact (the
// mathematical value of the double, ties to the larger magnitude). Values
// with |x| >= 1e21 use Number.prototype.toString, as JavaScript does.
void ToFixed(double value, int digits, char* output, size_t capacity);

// Unicode upper/lower case over UTF-8. Null on arena failure.
codec::tj::Value* ToUpperCase(codec::tj::Arena& arena, const char* text, size_t length);
codec::tj::Value* ToLowerCase(codec::tj::Arena& arena, const char* text, size_t length);

// Provider-only ECMAScript Number(string); malformed UTF-8 makes it NaN.
double ToNumber(const char* text, size_t length);

// "2026-01-02T03:04:05.000Z" (Date.prototype.toISOString), false when out of
// the ISO range.
bool IsoString(int64_t epoch_ms, char output[32]);
// The epoch milliseconds of a date string (JsDateParse); false when it does
// not parse. parity-only, see compat-decisions.md#date-parse
bool DateParse(const char* text, size_t length, double* epoch_ms);

}  // namespace fortis::compat::token_text
