#pragma once

#include "provider_runtime_config.h"
#include "outbound_https.h"
#include "price_id_parser.h"

#include <stddef.h>

namespace fortis::providers {

// Bare coin-ID batches keep their 2,048-byte bound; token-price requests use
// the transport's 8,192-byte bound for encoded contract addresses.
constexpr size_t kCoinGeckoSpotMaxPathBytes = 2048;
constexpr size_t kCoinGeckoTokenPriceMaxPathBytes = 8192;
constexpr size_t kCoinGeckoSpotMaxBatchPathBytes = kCoinGeckoSpotMaxPathBytes;
constexpr size_t kCoinGeckoSpotMaxBatchCount = 100;

struct CoinGeckoSpotBatch {
  size_t first_id = 0;
  size_t id_count = 0;
  size_t path_length = 0;
};

// Stable, normalized, de-duplicated plan. IDs and descriptors are owned by
// this fixed-size value so callers can build each request without heap storage.
struct CoinGeckoSpotBatchPlan {
  size_t id_count = 0;
  size_t batch_count = 0;
  char ids[kCoinGeckoSpotMaxBatchCount][kPriceIdMaxSourceIdBytes + 1]{};
  CoinGeckoSpotBatch batches[kCoinGeckoSpotMaxBatchCount]{};
};

enum class CoinGeckoSpotPlanError : uint8_t {
  kNone,
  kInvalidArguments,
  kInvalidConfig,
  kPathLimit,
};

enum class CoinGeckoSpotRequestError : uint8_t {
  kNone,
  kInvalidArguments,
  kInvalidConfig,
  kBufferTooSmall,
};

// Builds a bounded GET request for ASCII slugs and a lowercase fiat code.
// `path_storage` is caller-owned and must live through Submit(); the execution
// adapter copies the path and inline auth header into its bounded job slot.
// A configured CA bundle is required so this request cannot disable TLS trust.
CoinGeckoSpotRequestError BuildCoinGeckoSpotRequest(
    const config::ProviderRuntimeConfig& config, const char* const* ids, size_t id_count,
    const char* fiat, char* path_storage, size_t path_capacity,
    http::OutboundRequest* request);

// Builds /simple/token_price/{platform} for canonical contract source IDs.
// Every ID must resolve to the supplied platform; URLs and IDs stay bounded.
CoinGeckoSpotRequestError BuildCoinGeckoTokenPriceRequest(
    const config::ProviderRuntimeConfig& config, const char* platform,
    const char* const* source_ids, size_t id_count, const char* fiat,
    char* path_storage, size_t path_capacity, http::OutboundRequest* request);

// Copies normalized unique IDs and greedily takes the largest ordered prefix
// whose exact BuildCoinGeckoSpotRequest path is <= 2,048 bytes. For positive
// per-ID lengths this yields the fewest stable-order batches. On failure the
// returned plan counts are zero; no input is truncated or reordered.
CoinGeckoSpotPlanError BuildCoinGeckoSpotBatchPlan(
    const config::ProviderRuntimeConfig& config, const char* const* ids, size_t id_count,
    const char* fiat, CoinGeckoSpotBatchPlan* plan);

}  // namespace fortis::providers
