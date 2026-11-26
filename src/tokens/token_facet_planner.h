#pragma once

#include "chain_id_set.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::config { struct ProviderRuntimeConfig; }
namespace fortis::networks { struct TokenNetwork; struct TokenRef; struct NativeNetworkConfig; }

namespace fortis::tokens::token_facts {

enum Facet : uint8_t;

// Only the deployment capabilities and memo consulted by facet selection.
// Borrowed dependencies must outlive the planner. No provider, store, arena,
// worker or response state is needed to decide which reads a facet requires.
struct FacetPlannerDependencies {
  const config::ProviderRuntimeConfig* config = nullptr;
  const chains::BoundedChainIdSet<32>* etherscan_unsupported = nullptr;
  bool saucerswap = true;
};

// A request-scoped view of one resolved network/token pair. The referenced
// values must outlive this object; no result retains request-local storage.
// Planning performs no I/O or allocation. The memo is consulted on each call
// so concurrent unsupported-chain observations remain visible.
class FacetPlanner {
 public:
  FacetPlanner(const networks::TokenNetwork& network, const networks::TokenRef& ref,
               const networks::NativeNetworkConfig* seed, const FacetPlannerDependencies& dependencies)
      : network_(network), ref_(ref), seed_(seed), dependencies_(dependencies) {}

  bool SaucerSwapSupported() const;
  const char* DexScreenerSlug() const;
  const char* LiquidityProvider() const;
  bool HolderSourceSupported() const;
  const char* GoPlusChain(char buffer[16]) const;
  uint32_t EvmChainId() const;
  uint8_t SharedParts(Facet facet, bool has_coin) const;
  bool SnapshotBacked(Facet facet, bool has_coin) const { return SharedParts(facet, has_coin) != 0; }
  static uint8_t DecisivePart(Facet facet, const networks::TokenNetwork& network,
                              const networks::TokenRef& ref);

 private:
  const networks::TokenNetwork& network_;
  const networks::TokenRef& ref_;
  const networks::NativeNetworkConfig* seed_;
  FacetPlannerDependencies dependencies_;
};

}  // namespace fortis::tokens::token_facts
