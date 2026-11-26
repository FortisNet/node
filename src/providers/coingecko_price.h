#pragma once

#include "bounded_json.h"

namespace fortis::providers {

constexpr size_t kCoinGeckoMaxSpotIds = 100;
// Coin IDs are at most 100 bytes; canonical contract source IDs can reach the
// native route's 512-byte source-ID bound.
constexpr size_t kCoinGeckoIdCapacity = 513;

enum class CoinGeckoSpotError : uint8_t {
  kNone, kInvalidArguments, kInvalidJson, kResponseTooLarge,
  kTokenLimit, kDepthLimit
};

// Internal source provenance; not a wire or persistence representation.
enum class SpotQuoteOrigin : uint8_t { kDefault, kKraken, kDefiLlama, kMexc };

struct CoinGeckoSpotQuote {
  char id[kCoinGeckoIdCapacity]{};
  double price = 0;
  double change24hPct = 0;
  double volume24h = 0;
  bool hasChange24hPct = false;
  bool hasVolume24h = false;
  int64_t provider_observed_at_ms = 0;
  SpotQuoteOrigin origin = SpotQuoteOrigin::kDefault;
};

struct CoinGeckoSpotResult {
  CoinGeckoSpotError error = CoinGeckoSpotError::kNone;
  size_t count = 0;
  CoinGeckoSpotQuote quotes[kCoinGeckoMaxSpotIds]{};
};

// Parses one bounded CoinGecko /simple/price response and extracts only
// requested CoinGecko IDs. IDs are normalized to lowercase ASCII slugs; fiat
// must be a lowercase 3-5 letter currency code. The caller supplies the fixed
// token workspace, so this operation performs no dynamic allocation.
CoinGeckoSpotResult ExtractCoinGeckoSimplePrice(
    const unsigned char* input, size_t input_length,
    const char* const* requested_ids, size_t requested_id_count,
    const char* fiat, codec::BoundedJsonToken* token_workspace,
    size_t token_workspace_capacity, uint32_t* key_index_workspace,
    size_t key_index_workspace_capacity);

}  // namespace fortis::providers

