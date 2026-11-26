#pragma once

#include "coingecko_price.h"
#include "provider_runtime_config.h"
#include "outbound_https.h"

namespace fortis::providers {

constexpr size_t kRobinhoodMaxAssets = 256;
constexpr size_t kRobinhoodMaxSymbolBytes = 32;
constexpr size_t kRobinhoodPricePathBytes = 256;

struct RobinhoodAsset {
  char address[43]{};
  char symbol[kRobinhoodMaxSymbolBytes + 1]{};
  double multiplier = 0;
};

struct RobinhoodAssetCatalog {
  size_t count = 0;
  RobinhoodAsset assets[kRobinhoodMaxAssets]{};
};

enum class RobinhoodError : uint8_t {
  kNone, kInvalidArguments, kInvalidJson, kResponseTooLarge, kTokenLimit,
  kCatalogLimit, kBufferTooSmall, kInvalidQuote
};

RobinhoodError ExtractRobinhoodAssets(const unsigned char* input, size_t input_length,
    codec::BoundedJsonToken* tokens, size_t token_capacity, RobinhoodAssetCatalog* output);
bool BuildRobinhoodPricePath(const char* symbol, char* output, size_t capacity);
bool BuildRobinhoodRequest(const config::ProviderRuntimeConfig& config, const char* path,
    http::OutboundRequest* request);
RobinhoodError ExtractRobinhoodQuote(const unsigned char* input, size_t input_length,
    const RobinhoodAsset& requested, codec::BoundedJsonToken* tokens, size_t token_capacity,
    double* midpoint_usd);

}  // namespace fortis::providers
