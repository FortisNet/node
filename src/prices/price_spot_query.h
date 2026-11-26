#pragma once

#include <stddef.h>
#include <stdint.h>

// The GET /v1/prices query contract: the query string parser
// (fast-querystring 1.1 with fast-decode-uri-component semantics).
// The grammar itself ('&' pairs, first '=', '+' is a space, %XX is a UTF-8
// byte) is what the app sends (docs/compat-decisions.md#query-encoding).
// Only the leniency (repeated keys as arrays, the raw fallback for a
// malformed escape) is
// parity-only, see compat-decisions.md#query-string
// The schema is
//   { ids: z.string().min(1).max(4000), fiat: z.string().regex(/^[a-z]{3,5}$/).default('usd') }
// and the route's `ids.split(',').map(trim).filter(nonEmpty)` with its 1–100
// count check. Pure, allocation-free and guest-safe; the route handler and
// tests/price_spot_query_test.cpp share it.
namespace fortis::prices {
namespace spot_query {

constexpr size_t kMaxIds = 100;          // the TypeScript MAX_IDS
constexpr size_t kMaxIdsUnits = 4000;    // zod max(4000), in UTF-16 code units
// A UTF-16 code unit is at most three UTF-8 bytes, so any value longer than
// this is already over the zod maximum and is never needed verbatim.
constexpr size_t kIdsBufferBytes = kMaxIdsUnits * 3;
// Native bound: price ids, cache keys and the owner workspace hold at most
// 512 bytes per id (kPriceIdMaxRawBytes). TypeScript has no such bound.
constexpr size_t kMaxIdBytes = 512;
constexpr size_t kErrorBodyBytes = 640;

enum class SpotQueryOutcome : uint8_t {
  kOk,
  kSchema,     // 400 {"error":"BAD_REQUEST"}
  kIdCount,    // 400 {"error":"BAD_REQUEST","detail":"1–100 ids"}
  kIdTooLong,  // native bound: an id over kMaxIdBytes (TypeScript answers 200)
};

struct Query {
  SpotQueryOutcome status = SpotQueryOutcome::kSchema;
  // The decoded `ids` value (UTF-8). Ids are views into it after trimming.
  char ids[kIdsBufferBytes + 1]{};
  size_t ids_length = 0;
  char fiat[6]{};
  size_t id_offset[kMaxIds]{};
  size_t id_length[kMaxIds]{};
  size_t id_count = 0;
  // Complete 400 body for every status other than kOk.
  char error_body[kErrorBodyBytes]{};
  size_t error_body_length = 0;
};

// Parses the query string (the text after the first '?', not including it;
// null or empty when the target has none). Raw target bytes >= 0x80 are
// Latin-1 characters, as Node.js reads a request target.
void ParseQuery(const char* query, size_t length, Query* out);

// fast-querystring's per-component decode: '+' becomes a space, then
// fast-decode-uri-component; when that fails (malformed escape, invalid or
// truncated UTF-8) the raw text, with '+' already replaced, is kept. Writes
// at most `capacity` UTF-8 bytes (no terminator) and returns the full decoded
// length, which exceeds `capacity` when the output was truncated.
size_t DecodeComponent(const char* text, size_t length, char* out, size_t capacity);

// DecodeComponent streamed: the same bytes, in order, go to `sink` (never
// buffered), and the decoded length is returned. Used where a value may be
// as long as the whole request target.
using DecodeSink = void (*)(void* context, unsigned char byte);
size_t DecodeComponentTo(const char* text, size_t length, DecodeSink sink, void* context);

// UTF-16 code units of valid UTF-8 text (JavaScript String length).
size_t Utf16Units(const char* text, size_t length);

}  // namespace spot_query
}  // namespace fortis::prices
