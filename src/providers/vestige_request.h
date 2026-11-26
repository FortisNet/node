#pragma once

#include "provider_runtime_config.h"
#include "outbound_https.h"

#include <stddef.h>

namespace fortis::providers {

constexpr size_t kVestigePriceMaxIds = 100;
constexpr size_t kVestigePriceMaxPathBytes =
    sizeof("/assets/price?network_id=0&asset_ids=") - 1 +
    kVestigePriceMaxIds * 20 + (kVestigePriceMaxIds - 1) * 3 +
    sizeof("&denominating_asset_id=31566704") - 1;

enum class VestigePriceRequestError : uint8_t {
  kNone, kInvalidArguments, kInvalidConfig, kBufferTooSmall
};

VestigePriceRequestError BuildVestigePriceRequest(
    const config::ProviderRuntimeConfig& config, const char* const* source_ids, size_t id_count,
    char* path_storage, size_t path_capacity, http::OutboundRequest* request);

}  // namespace fortis::providers
