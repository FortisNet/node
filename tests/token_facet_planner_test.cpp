#include "token_facet_planner.h"

#include "chain_id_set.h"
#include "native_config.h"
#include "network_registry.h"
#include "token_facts.h"
#include "token_ledger.h"
#include "unit_test_support.h"

#include <memory>
#include <string.h>

namespace {
namespace facts = fortis::tokens::token_facts;
namespace ledger = fortis::tokens::token_ledger;
using fortis::networks::TokenRefKind;
using fortis::testing::Require;

// Deliberately no Env, Work, store, provider, clock, worker or server fixture.
struct Input {
  fortis::networks::TokenNetwork network;
  fortis::networks::TokenRef ref;
  Input(const char* id, const char* family, TokenRefKind kind, const char* address) {
    network.id = id;
    network.family = family;
    ref.kind = kind;
    ref.has_address = address != nullptr;
    if (address) snprintf(ref.address, sizeof(ref.address), "%s", address);
  }
  facts::FacetPlanner Planner(const facts::FacetPlannerDependencies& dependencies = {},
                              const fortis::networks::NativeNetworkConfig* seed = nullptr) const {
    return facts::FacetPlanner(network, ref, seed, dependencies);
  }
};

void liquidity_selection() {
  const char* ids[] = {"eth-mainnet", "bnb", "solana", "cardano", "base", "polygon", "avalanche",
                      "cronos", "arbitrum", "optimism", "hyperevm", "sei", "flare", "tron"};
  const char* slugs[] = {"ethereum", "bsc", "solana", "cardano", "base", "polygon", "avalanche",
                        "cronos", "arbitrum", "optimism", "hyperevm", "seiv2", "flare", "tron"};
  for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
    Input in(ids[i], "evm", TokenRefKind::kErc20, "token");
    const auto planner = in.Planner();
    Require(!strcmp(planner.DexScreenerSlug(), slugs[i]), "preserve provider network slug");
    Require(!strcmp(planner.LiquidityProvider(), "dexscreener"), "mapped networks use DexScreener");
  }
  Input hedera("hedera", "hedera", TokenRefKind::kHts, "0.0.123");
  Require(!strcmp(hedera.Planner().LiquidityProvider(), "saucerswap"), "HTS uses SaucerSwap");
  Require(!hedera.Planner({nullptr, nullptr, false}).LiquidityProvider(), "disabled SaucerSwap has no provider");
  Input native("eth-mainnet", "evm", TokenRefKind::kNative, nullptr);
  Require(native.Planner().DexScreenerSlug() && !native.Planner().LiquidityProvider(), "native assets do not request pools");
  Input unknown("unknown", "evm", TokenRefKind::kErc20, "token");
  Require(!unknown.Planner().DexScreenerSlug() && !unknown.Planner().LiquidityProvider(), "unknown network has no pool provider");
}

void holder_policy() {
  auto config = std::make_unique<fortis::config::NativeConfig>();
  fortis::chains::BoundedChainIdSet<32> unsupported;
  const facts::FacetPlannerDependencies dependencies{config.get(), &unsupported, true};
  Input ethereum("eth-mainnet", "evm", TokenRefKind::kErc20, "token");
  Require(ethereum.Planner().HolderSourceSupported(), "Ethereum retains keyless Ethplorer path");
  Input base("eip155:8453", "evm", TokenRefKind::kErc20, "token");
  auto planner = base.Planner(dependencies);
  Require(!planner.HolderSourceSupported(), "Etherscan requires configured credentials");
  config->etherscan_api_key_configured = true;
  Require(planner.HolderSourceSupported(), "supported EVM chain can query Etherscan");
  unsupported.Insert(fortis::chains::ChainId(8453));
  Require(!planner.HolderSourceSupported(), "existing planner observes later unsupported-chain discoveries");
  Require(ethereum.Planner(dependencies).HolderSourceSupported(), "memo does not disable independent Ethplorer path");
  Input unsupported_chain("eip155:12345", "evm", TokenRefKind::kErc20, "token");
  Require(!unsupported_chain.Planner(dependencies).HolderSourceSupported(), "credentials do not enable unknown chain");
  Input symbol("eip155:8453", "evm", TokenRefKind::kSymbol, nullptr);
  Require(!symbol.Planner(dependencies).HolderSourceSupported(), "symbol is not an ERC20 holder source");
}

void hedera_read_plan() {
  Input hedera("hedera", "hedera", TokenRefKind::kHts, "0.0.123");
  const auto planner = hedera.Planner();
  Require(planner.SharedParts(facts::kMarket, false) == (ledger::kSaucerTokenPart | ledger::kSaucerPricesPart), "unresolved market reads listing and optional prices");
  Require(planner.SharedParts(facts::kProfile, false) == ledger::kSaucerTokenPart, "unresolved profile reads listing");
  Require(!planner.SnapshotBacked(facts::kMarket, true) && !planner.SnapshotBacked(facts::kProfile, true), "resolved CoinGecko coin takes precedence");
  Require(planner.SharedParts(facts::kLiquidity, true) == ledger::kSaucerPoolsPart, "pool discovery remains independent of coin resolution");
  Require(planner.SharedParts(facts::kInsight, false) == (ledger::kIndexPart | ledger::kSaucerTokenPart), "insight combines mirror and optional listing");
  Require(planner.SharedParts(facts::kHolders, false) == ledger::kHolderPart && planner.SharedParts(facts::kChainMeta, false) == ledger::kIndexPart, "holders and metadata use distinct reads");
  const auto disabled = hedera.Planner({nullptr, nullptr, false});
  Require(!disabled.SharedParts(facts::kMarket, false) && !disabled.SharedParts(facts::kLiquidity, false) && disabled.SharedParts(facts::kInsight, false) == ledger::kIndexPart, "disabled DEX retains ledger insight");
  Require(facts::FacetPlanner::DecisivePart(facts::kMarket, hedera.network, hedera.ref) == ledger::kSaucerTokenPart && facts::FacetPlanner::DecisivePart(facts::kInsight, hedera.network, hedera.ref) == ledger::kIndexPart, "optional reads do not decide facet failure");
}

void ledger_boundaries() {
  Input sui("sui", "sui", TokenRefKind::kSui, "0x2::sui::SUI");
  Require(sui.Planner().SharedParts(facts::kChainMeta, false) == ledger::kIndexPart && !sui.Planner().SnapshotBacked(facts::kInsight, false) && !sui.Planner().HolderSourceSupported(), "Sui exposes metadata without inventing insight or holders");
  Input hedera("hedera", "hedera", TokenRefKind::kHts, "not-an-entity");
  Require(hedera.Planner().HolderSourceSupported() && !hedera.Planner().SnapshotBacked(facts::kHolders, false), "driver capability is separate from indexable identifier");
  Input no_address("hedera", "hedera", TokenRefKind::kNative, nullptr);
  for (uint8_t i = 0; i < facts::kFacetCount; ++i)
    Require(!no_address.Planner().SharedParts(static_cast<facts::Facet>(i), false), "native ref requires no token snapshot");
}

void keyed_snapshots() {
  auto config = std::make_unique<fortis::config::NativeConfig>();
  facts::FacetPlannerDependencies dependencies{config.get(), nullptr, true};
  Input tron("tron", "tron", TokenRefKind::kTrc10, "1002000");
  Require(tron.Planner(dependencies).SnapshotBacked(facts::kChainMeta, false) && !tron.Planner(dependencies).SnapshotBacked(facts::kHolders, false), "Tron metadata is keyless, holders require key");
  config->tronscan_api_key_configured = true;
  Require(tron.Planner(dependencies).SnapshotBacked(facts::kHolders, false), "Tron holder key enables shared read");
  Input subnet("bittensor", "bittensor", TokenRefKind::kBittensorAlpha, "1");
  Require(!subnet.Planner(dependencies).SnapshotBacked(facts::kMarket, false), "Bittensor requires Taostats key");
  config->taostats_api_key_configured = true;
  Require(subnet.Planner(dependencies).SharedParts(facts::kMarket, false) == (ledger::kIndexPart | ledger::kTaoPricePart), "unresolved subnet reads pool plus TAO price");
  Require(!subnet.Planner(dependencies).SnapshotBacked(facts::kMarket, true) && subnet.Planner(dependencies).SnapshotBacked(facts::kInsight, true), "resolved coin only suppresses subnet market read");
  Require(facts::FacetPlanner::DecisivePart(facts::kMarket, subnet.network, subnet.ref) == ledger::kIndexPart, "TAO quote is optional for market failure");
}

void chain_id_and_security() {
  Input generic("eip155:4294967295", "evm", TokenRefKind::kErc20, "token");
  Require(generic.Planner().EvmChainId() == UINT32_MAX, "preserve legacy token policy's uint32 upper bound");
  generic.network.id = "eip155:4294967296";
  Require(!generic.Planner().EvmChainId(), "oversized chain does not wrap");
  generic.network.id = "eip155:12x";
  Require(!generic.Planner().EvmChainId(), "malformed chain is absent");
  fortis::networks::NativeNetworkConfig seed{};
  seed.has_evm_chain_id = true;
  seed.evm_chain_id = 1;
  char chain[16];
  Require(generic.Planner({}, &seed).EvmChainId() == 1 && !strcmp(generic.Planner({}, &seed).GoPlusChain(chain), "1"), "seed identity takes precedence and feeds security provider");
  generic.ref.kind = TokenRefKind::kNative;
  Require(!generic.Planner({}, &seed).GoPlusChain(chain), "native assets do not request token security");
}
}  // namespace

int main() {
  fortis::testing::UnitSuite suite;
  suite.Run("liquidity provider selection", liquidity_selection);
  suite.Run("holder capability and live unsupported memo", holder_policy);
  suite.Run("Hedera reads and optional dependencies", hedera_read_plan);
  suite.Run("ledger capability and identifier boundaries", ledger_boundaries);
  suite.Run("keyed Tron and Bittensor snapshots", keyed_snapshots);
  suite.Run("chain identity and security selection", chain_id_and_security);
  return suite.Finish("token_facet_planner_test");
}
