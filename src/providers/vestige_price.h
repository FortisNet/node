#pragma once

#include "coingecko_price.h"

namespace fortis::providers {

// Parses the keyless Vestige /assets/price array. Only numeric asset_id and
// price fields are accepted, matching VestigeClient.prices. Non-array JSON
// documents are successful empty replies, as the TypeScript client treats
// them as an empty list. The caller supplies bounded token storage.
CoinGeckoSpotResult ExtractVestigePrices(
    const unsigned char* input, size_t input_length,
    const char* const* requested_source_ids, size_t requested_count,
    codec::BoundedJsonToken* token_workspace, size_t token_workspace_capacity);

}  // namespace fortis::providers
