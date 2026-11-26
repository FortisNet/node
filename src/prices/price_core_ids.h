#pragma once
#include <stddef.h>

namespace fortis::prices {
// Native wallet coverage only. No address or device-specific membership.
inline constexpr const char* kPriceCoreIds[] = {
    "bitcoin", "ethereum", "binancecoin", "ripple", "solana", "algorand"};
inline constexpr size_t kPriceCoreCount = sizeof(kPriceCoreIds) / sizeof(kPriceCoreIds[0]);
}
