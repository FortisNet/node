#pragma once

#include "bounded_json.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::providers {

constexpr size_t kJupiterPriceMaxQuotes = 50;
constexpr size_t kJupiterMintCapacity = 45;  // 44 base58 bytes plus terminator

enum class JupiterPriceError : uint8_t {
  kNone,
  kInvalidArguments,
  kInvalidJson,
  kResponseTooLarge,
  kTokenLimit,
  kDepthLimit,
  // A JSON `null` document. TypeScript's Object.entries(null) throws inside
  // the provider client, which the facet treats like a transport failure:
  // nothing is cached and the next request retries.
  kNullBody,
};

struct JupiterPriceQuote {
  char mint[kJupiterMintCapacity]{};
  double price = 0;
  double change24hPct = 0;
  bool hasChange24hPct = false;
  // /price/v3 never reports volume; the quote carries none rather than zero.
};

struct JupiterPriceResult {
  JupiterPriceError error = JupiterPriceError::kNone;
  size_t count = 0;
  JupiterPriceQuote quotes[kJupiterPriceMaxQuotes]{};
};

// Parses one bounded Jupiter /price/v3 response for at most 50 requested mints
// (exact, case-sensitive keys) with the TypeScript client's semantics: a row
// counts only when it is an object whose `usdPrice` is a finite JSON number;
// `priceChange24h` is kept only when numeric; duplicate keys keep the last
// value like JSON.parse; any non-null, non-object document answers with no
// quotes (Object.entries yields nothing usable). Quotes follow request order.
JupiterPriceResult ExtractJupiterPriceV3(
    const unsigned char* input, size_t input_length,
    const char* const* requested_mints, size_t requested_count,
    codec::BoundedJsonToken* token_workspace, size_t token_workspace_capacity);

}  // namespace fortis::providers
