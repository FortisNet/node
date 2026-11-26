#pragma once

#include "provider_runtime_config.h"
#include "outbound_https.h"

#include <stddef.h>

namespace fortis::providers {

constexpr size_t kTronScanPriceMaxIds = 100;
constexpr size_t kTronScanPriceMaxPathBytes = sizeof("/api/getAssetWithPriceList") - 1;

enum class TronScanPriceRequestError : uint8_t {
  kNone, kInvalidArguments, kInvalidConfig, kBufferTooSmall
};

TronScanPriceRequestError BuildTronScanPriceRequest(
    const config::ProviderRuntimeConfig& config, const char* const* source_ids, size_t id_count,
    bool anonymous, char* path_storage, size_t path_capacity,
    http::OutboundRequest* request);

}  // namespace fortis::providers
