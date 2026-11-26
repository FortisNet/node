#pragma once

#include "coingecko_price.h"
#include "provider_runtime_config.h"
#include "outbound_https.h"

#include <string_view>

namespace fortis::providers {

/// Exact USD market mapping. Returns a static borrowed name, or null; no ticker inference.
inline const char* KrakenUsdPair(std::string_view id) {
  if (id == "tron") return "TRXUSD";
  if (id == "avalanche-2") return "AVAXUSD";
  if (id == "the-open-network") return "TONUSD";
  return nullptr;
}

constexpr int64_t kKrakenMaxTradeAgeMs = 300'000;
constexpr size_t kKrakenMaxResponseBytes = 8192;

/** Build a credential-free, verified HTTPS recent-trade request for one mapped USD asset.
 * Path storage and config are borrowed until transport copies the request. False clears
 * request/path on invalid arguments or configuration. No network access or shared state.
 */
bool BuildKrakenPriceRequest(const config::ProviderRuntimeConfig& config, std::string_view id, char* path,
                             size_t capacity, http::OutboundRequest* request);

/** Extract one positive USD last-trade price with its original provider timestamp.
 * Rejects errors, wrong markets, malformed/oversized bodies, trades older than five
 * minutes and future timestamps. Workspaces/input are synchronous
 * borrows; the returned result owns all data. No allocation or shared mutable state.
 */
CoinGeckoSpotResult ExtractKrakenPrice(const unsigned char* input, size_t length, std::string_view id, int64_t now_ms,
                                       codec::BoundedJsonToken* tokens, size_t capacity);

}  // namespace fortis::providers
