#pragma once
#include "coingecko_price.h"
#include "provider_runtime_config.h"
#include "outbound_https.h"
#include <string_view>

namespace fortis::providers {
constexpr size_t kMexcMaxResponseBytes = 8192;
/** Accept only the verified Wiki Cat slug or its exact BSC contract.
 * The view is a synchronous borrow; no ticker inference or shared state.
 */
bool MexcUsdSupported(std::string_view id);
/** Build one credential-free, verified HTTPS recent-trade request.
 * Config/path are borrowed until transport copies them. Failure clears outputs.
 */
bool BuildMexcPriceRequest(const config::ProviderRuntimeConfig& config, std::string_view id, char* path,
                           size_t capacity, http::OutboundRequest* request);
/** Convert a recent WKC/USDT trade using an independently observed USDT/USD rate.
 * Both observations must be within five minutes and never future. The returned
 * owned quote uses the older timestamp and kMexc origin. Malformed, duplicate,
 * nonfinite or unrepresentable data fail closed. Workspaces are synchronous
 * borrows; no allocation, retained views or shared state.
 */
CoinGeckoSpotResult ExtractMexcPrice(const unsigned char* input, size_t length, std::string_view id, int64_t now_ms,
                                     double usdt_usd, int64_t fx_observed_at_ms, codec::BoundedJsonToken* tokens,
                                     size_t capacity);
}  // namespace fortis::providers
