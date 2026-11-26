#include "token_facet_planner.h"

#include "chain_id_set.h"
#include "provider_runtime_config.h"
#include "token_bittensor.h"
#include "token_contract.h"
#include "token_facts.h"
#include "token_ledger.h"
#include "token_tron.h"

#include <string.h>

namespace fortis::tokens::token_facts {
namespace {
bool is(const char* a, const char* b) { return a && b && strcmp(a, b) == 0; }
bool etherscan_supported_chain(uint32_t chain) {
  static constexpr uint32_t kChains[] = {1, 10, 50, 56, 137, 999, 1329, 8453, 42161, 43114};
  for (uint32_t c : kChains) if (c == chain) return true;
  return false;
}

}  // namespace

const char* FacetPlanner::DexScreenerSlug() const {
  struct Slug { const char* network; const char* slug; };
  static constexpr Slug kSlugs[] = {
      {"eth-mainnet", "ethereum"}, {"bnb", "bsc"}, {"solana", "solana"}, {"cardano", "cardano"},
      {"base", "base"}, {"polygon", "polygon"}, {"avalanche", "avalanche"}, {"cronos", "cronos"},
      {"arbitrum", "arbitrum"}, {"optimism", "optimism"}, {"hyperevm", "hyperevm"},
      {"sei", "seiv2"}, {"flare", "flare"}, {"tron", "tron"},
  };
  for (const Slug& s : kSlugs) if (is(s.network, network_.id)) return s.slug;
  return nullptr;
}

uint32_t FacetPlanner::EvmChainId() const {
  if (seed_) return seed_->has_evm_chain_id ? seed_->evm_chain_id : 0;
  // A generic eip155:N network.
  if (strncmp(network_.id, "eip155:", 7) != 0) return 0;
  double value = 0;
  for (const char* p = network_.id + 7; *p; ++p) {
    if (*p < '0' || *p > '9') return 0;
    value = value * 10 + (*p - '0');
  }
  return value > 4294967295.0 ? 0 : static_cast<uint32_t>(value);
}

// `network_.family === 'hedera' && ref_.kind === 'hts' && this.deps.saucerswap`
// (with `ref_.address !== null`): the ref SaucerSwapClient reads.
bool FacetPlanner::SaucerSwapSupported() const {
  return dependencies_.saucerswap && ref_.has_address && ref_.kind == networks::TokenRefKind::kHts && is(network_.family, "hedera");
}

// liquidityProvider(network_, ref, slug).
const char* FacetPlanner::LiquidityProvider() const {
  if (!ref_.has_address) return nullptr;
  if (DexScreenerSlug()) return "dexscreener";
  return SaucerSwapSupported() ? "saucerswap" : nullptr;
}

// CompositeTokenHolderSource.supports: EvmTokenHolderSource, then
// DriverTokenHolderSource for Tron, Stellar, Cardano and Hedera.
bool FacetPlanner::HolderSourceSupported() const {
  if (token_tron::HoldersSupported(dependencies_.config, network_, ref_)) return true;
  if (token_ledger::HoldersSupported(network_, ref_)) return true;
  if (ref_.kind != networks::TokenRefKind::kErc20 || !is(network_.family, "evm")) return false;
  if (is(network_.id, "eth-mainnet")) return true;
  if (!dependencies_.config || !dependencies_.config->etherscan_api_key_configured) return false;
  const uint32_t chain = EvmChainId();
  return chain && etherscan_supported_chain(chain) && !(dependencies_.etherscan_unsupported && dependencies_.etherscan_unsupported->Contains(chains::ChainId(chain)));
}

const char* FacetPlanner::GoPlusChain(char buffer[16]) const {
  if (!ref_.has_address || ref_.kind == networks::TokenRefKind::kNative) return nullptr;
  return token_contract::GoPlusChainId(network_, EvmChainId(), buffer);
}

// The shared read's parts a sub-facet projects (0: not shared-backed):
// chainMeta and insight of a TRC-20/TRC-10 ref, and its holders where the
// Tron driver has the tokenHolders capability (a TronScan key); holders and
// insight of a Stellar asset or Cardano unit the driver looks up; insight
// of an Algorand ASA or XRPL issued token the driver reads (no
// tokenHolders); chainMeta and insight (the Mirror token-info part) and
// holders (the balances part) of a Hedera token; chainMeta of a Sui coin
// type (the GraphQL coinMetadata read; no insight or holders); chainMeta
// and insight (the Taostats pool) and, when no coin id resolved, market (the
// pool and the TAO/USD row) of a Bittensor subnet while the keyed driver
// exists; and, for a Hedera HTS ref SaucerSwapClient reads, the listing
// section of insight (the SaucerSwap token part), market and profile when no
// coin id resolved (CoinGecko keeps precedence) and liquidity (Hedera has no
// DexScreener slug).
uint8_t FacetPlanner::SharedParts(Facet facet, bool has_coin) const {
  using namespace token_ledger;
  if (token_tron::Snapshotted(network_, ref_)) {
    if (facet == kChainMeta || facet == kInsight) return kIndexPart;
    return facet == kHolders && token_tron::HoldersSupported(dependencies_.config, network_, ref_) ? kIndexPart : 0;
  }
  if (FamilyOf(network_, ref_) == Family::kBittensor) {
    if (!token_bittensor::Keyed(dependencies_.config) || !Indexed(network_, ref_)) return 0;
    if (facet == kChainMeta || facet == kInsight) return kIndexPart;
    return facet == kMarket && !has_coin ? kIndexPart | kTaoPricePart : 0;
  }
  const bool saucer = SaucerSwapSupported();
  switch (facet) {
    case kMarket: return saucer && !has_coin ? kSaucerTokenPart | kSaucerPricesPart : 0;
    case kProfile: return saucer && !has_coin ? kSaucerTokenPart : 0;
    case kLiquidity: return saucer && !DexScreenerSlug() ? kSaucerPoolsPart : 0;
    case kHolders:
      if (!HoldersSupported(network_, ref_) || !Indexed(network_, ref_)) return 0;
      return FamilyOf(network_, ref_) == Family::kHedera ? kHolderPart : kIndexPart;
    case kChainMeta: return MetadataSupported(network_, ref_) && Indexed(network_, ref_) ? kIndexPart : 0;
    case kInsight:
      if (!InsightSupported(network_, ref_) || !Indexed(network_, ref_)) return 0;
      return static_cast<uint8_t>(kIndexPart | (saucer ? kSaucerTokenPart : 0));
    default: return 0;
  }
}

// The part whose failure fails the sub-facet: SaucerSwap's recent prices
// (market) and its listing row (insight) are optional, so they never decide,
// nor does Bittensor's TAO/USD row (the market's pool decides).
uint8_t FacetPlanner::DecisivePart(Facet facet, const networks::TokenNetwork& network, const networks::TokenRef& ref) {
  using namespace token_ledger;
  if (FamilyOf(network, ref) == Family::kBittensor) return kIndexPart;
  switch (facet) {
    case kMarket: case kProfile: return kSaucerTokenPart;
    case kLiquidity: return kSaucerPoolsPart;
    case kHolders: return FamilyOf(network, ref) == Family::kHedera ? kHolderPart : kIndexPart;
    default: return kIndexPart;
  }
}

}  // namespace fortis::tokens::token_facts
