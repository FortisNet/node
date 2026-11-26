#pragma once

#include "coingecko_price.h"

namespace fortis::providers {

// Extracts live priceInUsd rows from TronScan's /api/getAssetWithPriceList.
// Only requested TRC-10 IDs with finite positive prices become quotes. The
// caller supplies bounded token storage.
CoinGeckoSpotResult ExtractTronScanTrc10Prices(
    const unsigned char* input, size_t input_length,
    const char* const* requested_source_ids, size_t requested_count,
    codec::BoundedJsonToken* token_workspace, size_t token_workspace_capacity);

}  // namespace fortis::providers
