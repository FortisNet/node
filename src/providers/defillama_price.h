#pragma once

#include "coingecko_price.h"
#include "provider_runtime_config.h"
#include "outbound_https.h"

#include <string_view>

namespace fortis::providers {

constexpr size_t kDefiLlamaMaxBatch = 15;
constexpr size_t kDefiLlamaMaxResponseBytes = 65536;
constexpr size_t kDefiLlamaKeyCapacity = 112;

/** Resolve an exact canonical source ID into a DefiLlama key.
 * Only explicit supported chain mappings and CoinGecko slugs are accepted;
 * never guesses symbols. Output is caller-owned, empty on failure. No shared state.
 */
bool DefiLlamaPriceKey(std::string_view id, char* output, size_t capacity);

/** Build a bounded credential-free USD request using verified HTTPS.
 * Config and path are borrowed until transport copies them. False clears request
 * and path. IDs must be distinct canonical IDs, at most kDefiLlamaMaxBatch.
 * Verified WKC requests append one internal tether conversion key when absent.
 */
bool BuildDefiLlamaPriceRequest(const config::ProviderRuntimeConfig& config, const char* const* ids, size_t count,
                                char* path, size_t capacity, http::OutboundRequest* request);

/** Extract exact requested USD quotes with original timestamps and confidence >= .9.
 * Missing, stale (> five minutes), future, low-confidence or invalid quotes are
 * omitted. Duplicate keys/malformed JSON fail the entire batch. Caller workspaces
 * are synchronous borrows; the result owns all data. No allocation or concurrency.
 */
CoinGeckoSpotResult ExtractDefiLlamaPrices(const unsigned char* input, size_t length, const char* const* ids,
                                           size_t count, int64_t now_ms, codec::BoundedJsonToken* tokens,
                                           size_t capacity);

/** Read only the internal USDT/USD conversion quote from a DefiLlama response.
 * Caller outputs are cleared on failure. The same bounds, timestamp, confidence
 * and duplicate-key rules apply as for ordinary quotes. No retained borrows or
 * shared state; call synchronously with caller-owned token workspace.
 */
bool ExtractDefiLlamaTetherUsd(const unsigned char* input, size_t length, int64_t now_ms,
                               codec::BoundedJsonToken* tokens, size_t capacity, double* rate, int64_t* observed_at_ms);
}  // namespace fortis::providers
