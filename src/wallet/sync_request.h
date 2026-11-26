#pragma once

#include "bounded_json.h"
#include "token_json.h"

#include <stddef.h>
#include <stdint.h>

// The request half of POST /v1/sync, in the order the
// TypeScript server applies it:
//
//  1. After the auth hook (auth-first), the JSON body schema: ajv's rules
//     (coerceTypes 'array', removeAdditional, strict numbers, ajv-formats
//     "full" date-time). A protobuf body skips it. CheckSchema leaves the
//     coerced values; a failure is 400 {"error":"BAD_REQUEST"}.
//  2. A protobuf body is decoded (DecodeProto, false where fromBinary
//     throws: the bare {"error":"BAD_REQUEST"}).
//  3. The schema (Validate) on the coerced/decoded values; any failure is
//     400 {"error":"BAD_REQUEST"}.
//
// Guest-safe: no C++ standard-library headers.
namespace fortis::wallet::sync {

using codec::tj::OutBuffer;

// A request string (UTF-8) in the request or the caller's arena.
struct Text {
  const char* data = nullptr;
  size_t length = 0;
};

// The fields after the body schema (JSON) or decode (protobuf).
struct Input {
  bool has_cursor = false;
  Text cursor;
  bool has_after = false;
  Text after;
  bool has_models = false;
  Text* models = nullptr;  // arena-owned
  size_t model_count = 0;
  bool has_limit = false;
  double limit = 0;
};

// The validated read: what SyncFacet.read() receives.
constexpr size_t kCursorBytes = 256;  // 64 UTF-16 units of UTF-8
struct Request {
  bool has_cursor = false;
  char cursor[kCursorBytes]{};  // trimmed
  size_t cursor_length = 0;
  bool has_after = false;
  double after_ms = 0;  // new Date(after).getTime() (NaN possible)
  bool include_transfer = true;
  uint32_t limit = 200;
};

// Step 1. `json` is the parsed body, or null when the body is not JSON
// (no body), which the object schema rejects. False on the first failure.
bool CheckSchema(const codec::BoundedJsonDocument* json, codec::tj::Arena& arena, Input* input);

// Step 2: fromBinary(SyncRequestSchema, body).
bool DecodeProto(const uint8_t* body, size_t length, codec::tj::Arena& arena, Input* input);

// Step 3. True fills `request`; false when the request is refused.
bool Validate(const Input& input, Request* request);

// Exposed for tests: the two date-time grammars.
// ajv-formats "date-time" (full mode, time zone required).
bool AjvDateTime(const char* text, size_t length);
// zod z.iso.datetime({ offset: true }).
bool ZodDateTime(const char* text, size_t length);

}  // namespace fortis::wallet::sync
