#pragma once

#include <stddef.h>
#include <stdint.h>

// Native port of the TypeScript `parseTokenRef` and of the registry lookup
// it is used with (the TypeScript registry's `network(id)`).
// Pure, allocation-free and guest-safe (no C++ standard-library headers).
// Frozen against tests/corpus/token_ref_corpus.json.
//
// Inputs are UTF-8. The TypeScript grammar is ASCII-only except for
// String.prototype.trim (Unicode whitespace) and toLowerCase, whose only
// non-ASCII-to-ASCII mapping relevant here is U+212A KELVIN SIGN -> 'k'; both
// are mirrored. Invalid UTF-8 is rejected (false).

namespace fortis::networks {

constexpr size_t kTokenRefMaxInputBytes = 1024;
constexpr size_t kTokenRefMaxIdBytes = kTokenRefMaxInputBytes + 32;
// "{network}:erc20:{address}" with a network id of at most 127 bytes.
constexpr size_t kTokenRefMaxPriceIdBytes = kTokenRefMaxIdBytes + 160;
constexpr size_t kTokenNetworkMaxGenericIdBytes = 127;

enum class TokenRefKind : uint8_t {
  kNative, kErc20, kTrc10, kTrc20, kSpl, kAsa, kCardano, kStellar, kHts, kXrpl,
  kBittensorAlpha, kAptos, kSui, kDenom, kSymbol, kCoinGecko,
};

// The TypeScript `kind` string ("native", "erc20", ..., "coingecko").
const char* TokenRefKindName(TokenRefKind kind);

// NetworkInfo fields the grammar reads. `id` is the canonical id (an
// eip155:N alias of a catalogued chain resolves to that chain's id). The
// strings point into the static catalog, or into generic_id for an unknown
// eip155:N network, which has no native coin. The structure must therefore
// not be copied after ResolveTokenNetwork filled it from generic_id.
struct TokenNetwork {
  const char* id = nullptr;
  const char* family = nullptr;
  const char* native_symbol = nullptr;        // null when the seed has none
  const char* native_coingecko_id = nullptr;  // null when the seed has none
  char generic_id[kTokenNetworkMaxGenericIdBytes + 1]{};
};

// registry.network(id): exact catalogued id, then `eip155:<digits>` matched by
// Number(digits) against catalogued EVM chain ids, then a generic EVM network
// for any other `eip155:<digits>`. False where TypeScript returns null (and
// for an unknown eip155 id longer than kTokenNetworkMaxGenericIdBytes).
bool ResolveTokenNetwork(const char* id, size_t length, TokenNetwork* out);

// TokenRef without the echoed `input`. Absent (TypeScript null) fields have
// their has_* flag false and an empty string.
struct TokenRef {
  TokenRefKind kind = TokenRefKind::kSymbol;
  char token_id[kTokenRefMaxIdBytes + 1]{};
  bool has_address = false;
  char address[kTokenRefMaxInputBytes + 1]{};
  bool has_symbol = false;
  char symbol[kTokenRefMaxInputBytes + 1]{};
  bool has_issuer = false;
  char issuer[kTokenRefMaxInputBytes + 1]{};
  bool has_coingecko_id = false;
  char coingecko_id[kTokenRefMaxInputBytes + 1]{};
  bool has_price_id = false;
  char price_id[kTokenRefMaxPriceIdBytes + 1]{};
};

// parseTokenRef(network, raw). `network` may be null (the network-agnostic
// route). False where TypeScript returns null, and for a trimmed input longer
// than kTokenRefMaxInputBytes bytes.
bool ParseTokenRef(const TokenNetwork* network, const char* raw, size_t length,
                   TokenRef* out);

// The TypeScript canonicalHederaEntityId(input): the trimmed
// `shard.realm.num` without leading zeros, or the long-zero 20-byte EVM form
// decoded to it. False where TypeScript returns null (or `capacity` is too
// small; 40 bytes always suffice).
bool CanonicalHederaEntityId(const char* input, size_t length, char* out, size_t capacity);

}  // namespace fortis::networks
