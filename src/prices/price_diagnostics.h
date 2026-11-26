#pragma once

#include "error_reporter.h"
#include "price_id_parser.h"
#include "spot_response_serializer.h"
#include "network_registry.h"

#include <algorithm>

namespace fortis::prices {

/** Record unpriced public market IDs without copying wallet/contract addresses.
 * The response and its views are borrowed only for this synchronous call.
 * The reporter copies fields and performs no storage/network I/O.
 */
inline void ReportMissingPrices(base::ErrorReporter* reporter, const SpotResponseInput& response) noexcept {
  if (!reporter) return;
  for (std::size_t index = 0; index < response.missing_count; ++index) {
    const auto id = response.missing[index];
    providers::ParsedPriceId parsed{};
    if (!providers::ParsePriceId(id.data(), id.size(), &parsed)) continue;
    std::size_t network_count = 0;
    const auto* catalog = networks::NativeNetworkRegistry{}.list(&network_count);
    const bool public_coin = parsed.kind == providers::PriceIdKind::kCoinGecko &&
        std::any_of(catalog, catalog + network_count, [&](const auto& network) {
          return network.native_coingecko_id && std::string_view(parsed.source_id) == network.native_coingecko_id;
        });
    reporter->Report("prices", "quote_unavailable", public_coin ? parsed.source_id : "", {base::ErrorOrigin::kUnclassified});
  }
  if (response.invalid_count) reporter->Report("prices", "invalid_identifier", {}, {base::ErrorOrigin::kUnclassified});
}

}  // namespace fortis::prices
