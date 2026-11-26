#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fortis::providers {

constexpr size_t kPriceIdMaxSourceIdBytes = 512;
constexpr size_t kPriceIdMaxRawBytes = kPriceIdMaxSourceIdBytes;

enum class PriceIdKind : uint8_t {
  kInvalid,
  kCoinGecko,
  kSpl,
  kAsa,
  kTrc10,
  kContract,
  kRobinhood,
};

struct ParsedPriceId {
  PriceIdKind kind = PriceIdKind::kInvalid;
  char source_id[kPriceIdMaxSourceIdBytes + 1]{};
  char network[kPriceIdMaxSourceIdBytes + 1]{};
  // CoinGecko slug, mint, asset ID, or contract address as appropriate.
  char address[kPriceIdMaxSourceIdBytes + 1]{};
  char standard[8]{};  // erc20, trc20, aptos, or empty.
};

// Parses and canonicalizes one bounded route ID without heap allocation,
// mirroring the TypeScript parsePriceId: JavaScript trim, the slug's
// toLowerCase (U+212A KELVIN SIGN is the one non-ASCII code point that
// lowers to ASCII) and the contract network's full JavaScript toLowerCase.
// Native bounds: at most 512 bytes in and out, and no NUL anywhere (a NUL
// inside a contract network name is valid TypeScript, refused here).
bool ParsePriceId(const char* raw, size_t raw_length, ParsedPriceId* output);

// Authoritative CoinGecko asset-platform seed map mirrored from the
// TypeScript network registry. Returns null for unindexed chains.
const char* CoinGeckoPlatformForNetwork(const char* network);

// Length in bytes of the JavaScript WhiteSpace/LineTerminator code point that
// starts (or, for the End variant, ends) valid UTF-8 `text`, or zero. This is
// String.prototype.trim's set: TAB, LF, VT, FF, CR, SPACE, U+00A0, U+1680,
// U+2000–U+200A, U+2028, U+2029, U+202F, U+205F, U+3000 and U+FEFF.
size_t JsWhitespaceAtStart(const char* text, size_t length);
size_t JsWhitespaceAtEnd(const char* text, size_t length);
}  // namespace fortis::providers
