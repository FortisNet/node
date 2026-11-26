#pragma once

#include "token_json.h"

#include <stddef.h>
#include <stdint.h>

// Pure ports of the provider-neutral liquidity domain: the TypeScript
// DexScreener client (pair mapping, chain filter, dedupe) and
// calculateLiquiditySnapshot, over the token_json DOM so
// key order and null semantics follow the TypeScript object literals.
// Guest-safe.
namespace fortis::tokens::token_liquidity {

// DEFAULT_LIQUIDITY_POOL_LIMIT.
constexpr double kDefaultPoolLimit = 6;
// Number.MAX_SAFE_INTEGER, the detailed (pools route) limit.
constexpr double kAllPools = 9007199254740991.0;

enum class PoolsStatus : uint8_t { kOk, kFailed, kArena };

// DexScreenerClient.pools(address, chainSlug) over the parsed body of
// /latest/dex/tokens/{address}: every pair mapped, chain-filtered to pairs
// touching `address`, chainId dropped, deduplicated. kFailed where
// TypeScript throws (a null body or a null pair element).
PoolsStatus DexScreenerPools(codec::tj::Arena& arena, const codec::tj::Value* body, const char* address,
                             const char* chain_slug, codec::tj::Value** pools);

// calculateLiquiditySnapshot(pools, tokenAddress, displayedPoolLimit).
// `address` may be null. *snapshot is null when TypeScript returns null.
bool CalculateLiquiditySnapshot(codec::tj::Arena& arena, const codec::tj::Value* pools, const char* address,
                                double displayed_pool_limit, codec::tj::Value** snapshot);

// {...snapshot, pools: snapshot.pools.slice(0, limit)} (a shallow copy).
codec::tj::Value* CompactSnapshot(codec::tj::Arena& arena, const codec::tj::Value* snapshot, double limit);

// The TypeScript chainAddressKey / tokenAddressesEqual.
codec::tj::Value* ChainAddressKey(codec::tj::Arena& arena, const char* text, size_t length);
bool TokenAddressesEqual(codec::tj::Arena& arena, const codec::tj::Value* left, const char* right);

// A JavaScript value as a template literal writes it (`${value}`), for the
// primitives and plain objects a provider body can hold.
codec::tj::Value* TemplateString(codec::tj::Arena& arena, const codec::tj::Value* value);
// JavaScript truthiness of a parsed JSON value (undefined = null pointer).
bool Truthy(const codec::tj::Value* value);

}  // namespace fortis::tokens::token_liquidity
