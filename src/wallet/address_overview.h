#pragma once

#include "chain_core.h"

// Network overview helpers shared with the route layer (address_overview.cpp).
namespace fortis::wallet::address {

// NetworkOverviewFacet.supportsNews / supportsPopularTokens (GET /v1/networks).
bool OverviewSupportsNews(const chains::Network& network);
bool OverviewSupportsPopularTokens(const chains::Network& network);
// The NetworkOverviewResponse message (networks.proto) of an overview
// payload, exactly as the TypeScript route builds it with protobuf-es. False when
// `out` is too small or, with *refused set, when protobuf-es refuses the
// payload: a holderCount BigInt() rejects (a fraction, Infinity) or the
// uint64 writer rejects (negative, >= 2^64).
bool OverviewProtobuf(const chains::Value* payload, chains::OutBuffer& out, bool* refused = nullptr);

// Exposed for tests/network_overview_test.cpp:
// overviewTokenAddress(network, raw) (null where the app has no grammar),
// isNetworkNews(item, networkProfile(network_id)) (false without a profile),
// and GeckoTerminalClient.trending's projection of a trending_pools body
// (any JSON value, null included) into the app's rows.
chains::Value* OverviewTokenAddress(chains::Arena& arena, const chains::Network& network, const char* raw,
                                   size_t length);
bool OverviewIsNetworkNews(const chains::Value* item, const char* network_id);
bool OverviewProjectTrending(const chains::Network& network, const chains::Value* body, chains::Arena& arena,
                             chains::Value** rows);

}  // namespace fortis::wallet::address
