#pragma once

#include "coingecko_price.h"
#include "price_id_parser.h"

namespace fortis::providers {

// Extracts only numeric prices for requested canonical contract source IDs.
// The result IDs remain full source IDs so the resolver can preserve cache
// identity for EVM, Tron and Aptos contracts.
CoinGeckoSpotResult ExtractCoinGeckoTokenPrice(
    const unsigned char* input, size_t input_length,
    const char* const* requested_source_ids, size_t requested_count,
    const char* fiat, codec::BoundedJsonToken* token_workspace,
    size_t token_workspace_capacity, uint32_t* key_index_workspace,
    size_t key_index_workspace_capacity);

}  // namespace fortis::providers
