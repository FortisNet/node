#pragma once

#include "token_json.h"

#include <stddef.h>
#include <stdint.h>

// The wallet-supplied portfolio snapshot of an AI conversation: ports of the
// TypeScript portfolioSnapshotSchema, snapshotLinksAreValid and
// portfolioContext. The snapshot is untrusted, possibly stale client
// data; nothing here fetches anything. Values live in the caller's tj arena
// and use its JavaScript semantics (WTF-8 strings, JSON.stringify output).
namespace fortis::ai {

// Request-side bounds of the source schemas.
constexpr size_t kSnapshotJsonMaxUnits = 300000;  // snapshotJson .max(300_000)
constexpr size_t kBodyMaxUnits = 8000;            // body/firstMessage .max(8_000)
constexpr size_t kContextMaxUnits = 40000;        // portfolioContext text bound
constexpr size_t kContextHoldings = 300;
constexpr size_t kContextTrackedTokens = 100;

// JSON.parse(text) (no BOM, JSON whitespace only) then
// portfolioSnapshotSchema.safeParse: the zod output object (keys in schema
// order, unknown keys rejected by .strict()), or null.
const fortis::codec::tj::Value* ParseSnapshot(const char* text, size_t length, fortis::codec::tj::Arena& arena);
// The same schema applied to an already decoded value.
const fortis::codec::tj::Value* ValidateSnapshot(const fortis::codec::tj::Value* v, fortis::codec::tj::Arena& arena);
// snapshotLinksAreValid: unique wallet ids and asset ids, every holding
// naming a listed wallet and token.
bool SnapshotLinksValid(const fortis::codec::tj::Value* snapshot);

// portfolioContext(snapshot): the JSON text the model receives, at most
// kContextMaxUnits UTF-16 units (tracked tokens dropped first, then
// holdings from the end). The text is NUL-terminated in the arena.
bool PortfolioContext(const fortis::codec::tj::Value* snapshot, fortis::codec::tj::Arena& arena, const char** text,
                      size_t* length);

// Prefix bounded in UTF-16 units, ending at a complete Unicode scalar.
size_t Utf16Prefix(const char* text, size_t length, size_t limit);
// The prefix as an arena string (valid UTF-8, NUL-terminated).
bool Utf16Slice(const char* text, size_t length, size_t limit, fortis::codec::tj::Arena& arena,
                const char** output, size_t* output_length);

// z.string().trim().min(1).max(8_000): the trimmed span, or false.
bool ZodTrimmedBody(const char* text, size_t length, size_t* begin, size_t* end);

}  // namespace fortis::ai
