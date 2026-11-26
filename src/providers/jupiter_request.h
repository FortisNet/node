#pragma once

#include "provider_runtime_config.h"
#include "outbound_https.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::providers {

// Mirrors the TypeScript client: `GET {base}/price/v3?ids=<=50 mints>` where the
// base is api.jup.ag with x-api-key when JUPITER_API_KEY is configured and the
// keyless lite-api.jup.ag host otherwise. Mints are case-sensitive base58 of
// 32-44 bytes; the joined list is percent-encoded exactly like
// encodeURIComponent (only the comma separator changes, to %2C).
constexpr size_t kJupiterPriceMaxMints = 50;
constexpr size_t kJupiterMintMinBytes = 32;
constexpr size_t kJupiterMintMaxBytes = 44;
constexpr size_t kJupiterPricePathPrefixBytes = sizeof("/price/v3?ids=") - 1;
// Fifty maximum-length mints joined by "%2C": 2,361 bytes, below the 4,096-byte
// outbound transport cap, so a full TypeScript batch is always one request.
constexpr size_t kJupiterPriceMaxPathBytes =
    kJupiterPricePathPrefixBytes + kJupiterPriceMaxMints * kJupiterMintMaxBytes +
    (kJupiterPriceMaxMints - 1) * 3;

enum class JupiterPriceRequestError : uint8_t {
  kNone,
  kInvalidArguments,
  kInvalidConfig,
  kBufferTooSmall,
};

// Base58 alphabet check with the TypeScript SPL_MINT_RE length bounds.
bool JupiterValidMint(const char* mint, size_t* length);

// `path_storage` is caller-owned and must live through Submit(); the execution
// adapter copies the path and inline auth header into its bounded job slot.
// A configured CA bundle is required so this request cannot disable TLS trust.
JupiterPriceRequestError BuildJupiterPriceRequest(
    const config::ProviderRuntimeConfig& config, const char* const* mints, size_t mint_count,
    char* path_storage, size_t path_capacity, http::OutboundRequest* request);

}  // namespace fortis::providers
