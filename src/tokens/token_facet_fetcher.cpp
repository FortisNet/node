// Token 360 facts: what each sub-facet reads. Which providers back a facet
// for a network and ref, the provider request (or immediate value) a
// sub-facet fetcher plans, and how a provider answer becomes the facet value.
#include "threading.h"
#include "token_facts_internal.h"
#include "token_facet_fetcher.h"

namespace tj = fortis::codec::tj;

namespace fortis::tokens::token_facts {

namespace {

void mark_etherscan_unsupported(const Env& env, uint32_t chain) {
  if (env.etherscan_unsupported) env.etherscan_unsupported->Insert(chains::ChainId(chain));
}

// `network?.id === 'robinhood' && ref.address !== null && this.deps.robinhood`:
// the ref Token360 asks RobinhoodStockTokenClient about.
bool robinhood_ref(const Env& env, const networks::TokenNetwork& network, const networks::TokenRef& ref) {
  return env.robinhood && token_robinhood::IssuerRef(network, ref);
}

// The network's deployment RPC endpoints for the EVM contract read.
const char* const* evm_rpc_urls(const Env& env, const FacetInputs& in, size_t* count) {
  if (env.robinhood_rpc_url_count && is(in.network->id, "robinhood")) {
    *count = env.robinhood_rpc_url_count;
    return env.robinhood_rpc_urls;
  }
  *count = in.seed ? in.seed->rpc_url_count : 0;
  return in.seed ? in.seed->rpc_urls : nullptr;
}

// Token360's Robinhood branches: chainMeta and profile answer from a listed
// contract's catalog row, the market from its reference quote; a failed
// catalog (or quote) read fails the sub-facet. kRequest here means "not
// answered by the issuer": an unlisted contract (or a null quote) takes the
// ordinary path.
}  // namespace

FacetFetchPlan TokenFacetFetcher::Issuer(Facet facet, Value** value, bool* lane_busy) {
  Work& work = work_;
  const auto& in = inputs_;
  if (!in.robinhood || !robinhood_ref(*work.env, *in.network, *in.ref) ||
      (facet != kMarket && facet != kProfile && facet != kChainMeta))
    return FacetFetchPlan::kRequest;
  token_robinhood::Memo* memo = in.robinhood;
  exchange::robinhood::StockClient* client = work.env->robinhood;
  const token_robinhood::Listing listing = token_robinhood::Asset(work, client, memo, *in.ref, lane_busy);
  if (listing == token_robinhood::Listing::kFailed) {
    memo->catalog_failed |= FacetBit(facet);
    return FacetFetchPlan::kFailed;
  }
  if (listing == token_robinhood::Listing::kUnlisted) return FacetFetchPlan::kRequest;
  Arena& a = work.arena;
  switch (facet) {
    case kChainMeta: *value = token_robinhood::ChainMeta(a, memo->asset, work.env->kv, work.Now()); break;
    case kProfile: *value = token_robinhood::Profile(a, memo->asset, work.env->kv, work.Now()); break;
    default: {
      Value* quote = nullptr;
      if (!token_robinhood::Quote(work, client, memo, &quote, lane_busy)) return FacetFetchPlan::kFailed;
      // A null quote: CoinGecko for a resolved coin id, else null.
      if (!quote) return FacetFetchPlan::kRequest;
      *value = token_robinhood::MarketRow(a, memo->asset, quote);
      break;
    }
  }
  return *value ? FacetFetchPlan::kValue : FacetFetchPlan::kFailed;
}

FacetFetchPlan TokenFacetFetcher::Prepare(Facet facet, providers::TokenFetch* fetch, Value** value, bool* lane_busy) {
  Work& work = work_;
  const auto& in = inputs_;
  Arena& a = work.arena;
  const networks::TokenRef& ref = *in.ref;
  *value = tj::Null(a);
  fetch->background = work.background;
  const FacetFetchPlan issuer = Issuer(facet, value, lane_busy);
  if (issuer != FacetFetchPlan::kRequest) return issuer;
  *value = tj::Null(a);
  switch (facet) {
    case kMarket:
      if (in.coin_id) {
        const char* ids[] = {in.coin_id};
        return token_fetch::CoinMarkets(fetch, ids, 1) ? FacetFetchPlan::kRequest : FacetFetchPlan::kFailed;
      }
      // Hedera's native DEX fills the gap for an unlisted HTS token.
      return MakeFacetPlanner(*work.env, in).SnapshotBacked(facet, in.coin_id != nullptr) ? FacetFetchPlan::kSnapshot : FacetFetchPlan::kValue;
    case kProfile:
      if (in.coin_id) return token_fetch::CoinProfile(fetch, in.coin_id) ? FacetFetchPlan::kRequest : FacetFetchPlan::kFailed;
      return MakeFacetPlanner(*work.env, in).SnapshotBacked(facet, in.coin_id != nullptr) ? FacetFetchPlan::kSnapshot : FacetFetchPlan::kValue;  // hederaProfile
    case kLiquidity: {
      if (!ref.has_address) return FacetFetchPlan::kValue;
      if (!MakeFacetPlanner(*work.env, in).DexScreenerSlug())
        return MakeFacetPlanner(*work.env, in).SnapshotBacked(facet, in.coin_id != nullptr) ? FacetFetchPlan::kSnapshot : FacetFetchPlan::kValue;  // SaucerSwap pools
      return token_fetch::DexPools(fetch, ref.address) ? FacetFetchPlan::kRequest : FacetFetchPlan::kFailed;
    }
    case kHolders: {
      if (!MakeFacetPlanner(*work.env, in).HolderSourceSupported()) return FacetFetchPlan::kValue;
      // TronDriver.readTokenHolders, StellarDriver / CardanoDriver
      // tokenHolders: an identifier the driver does not look up answers null
      // without a read.
      if (is(in.network->family, "tron") || token_ledger::HoldersSupported(*in.network, ref))
        return MakeFacetPlanner(*work.env, in).SnapshotBacked(facet, in.coin_id != nullptr) ? FacetFetchPlan::kSnapshot : FacetFetchPlan::kValue;
      const config::ProviderRuntimeConfig* config = work.env->config;
      if (is(in.network->id, "eth-mainnet"))
        return token_fetch::EthplorerHolders(fetch, ref.address,
                   config && config->ethplorer_api_key_configured ? config->ethplorer_api_key : nullptr)
                   ? FacetFetchPlan::kRequest : FacetFetchPlan::kFailed;
      return token_fetch::EtherscanHolders(fetch, MakeFacetPlanner(*work.env, in).EvmChainId(), ref.address,
                                           config->etherscan_api_key)
                 ? FacetFetchPlan::kRequest : FacetFetchPlan::kFailed;
    }
    case kChainMeta:
    case kInsight: {
      if (!ref.has_address) {
        if (facet == kInsight) *value = tj::Arr(a);
        return *value ? FacetFetchPlan::kValue : FacetFetchPlan::kFailed;
      }
      if (is(in.network->family, "tron")) {
        // TronDriver.tokenMetadata / tokenInsight over its token snapshot.
        if (MakeFacetPlanner(*work.env, in).SnapshotBacked(facet, in.coin_id != nullptr)) return FacetFetchPlan::kSnapshot;
        if (facet == kInsight) *value = tj::Arr(a);
        return *value ? FacetFetchPlan::kValue : FacetFetchPlan::kFailed;
      }
      if ((facet == kInsight && token_ledger::FamilyOf(*in.network, ref) != token_ledger::Family::kNone) ||
          (facet == kChainMeta && token_ledger::MetadataSupported(*in.network, ref))) {
        // StellarDriver / CardanoDriver / AlgorandDriver / XrplDriver /
        // HederaDriver tokenInsight, and HederaDriver / SuiDriver
        // tokenMetadata, over the ledger read (the other families have no
        // tokenMetadata: chainMeta falls through to null below; the Sui
        // driver has no tokenInsight: [] without a read). A ref the driver
        // does not read answers null / [].
        if (MakeFacetPlanner(*work.env, in).SnapshotBacked(facet, in.coin_id != nullptr)) return FacetFetchPlan::kSnapshot;
        if (facet == kInsight) *value = tj::Arr(a);
        return *value ? FacetFetchPlan::kValue : FacetFetchPlan::kFailed;
      }
      if (facet == kChainMeta && ref.kind == networks::TokenRefKind::kSpl && is(in.network->family, "solana")) {
        // No Solana driver metadata port: the Jupiter token-list row.
        return token_fetch::JupiterSearch(fetch, ref.address) ? FacetFetchPlan::kRequest : FacetFetchPlan::kFailed;
      }
      if (!is(in.network->family, "evm")) {
        // No driver capability: chainMeta falls through to its SPL branch
        // (null for every other covered family); insight is [] (Solana's
        // token-list sections ride the chainMeta read in ComputeFacts).
        if (facet == kInsight) *value = tj::Arr(a);
        return *value ? FacetFetchPlan::kValue : FacetFetchPlan::kFailed;
      }
      // contractFacts: an invalid EVM address answers null with no request.
      size_t rpc_count = 0;
      const char* const* rpc_urls = evm_rpc_urls(*work.env, in, &rpc_count);
      size_t length = 0;
      char probe[providers::kTokenMaxRpcBodyBytes];
      if (!token_evm::ContractFactsRequest(ref.address, probe, sizeof(probe), &length)) {
        if (facet == kInsight) *value = tj::Arr(a);
        return *value ? FacetFetchPlan::kValue : FacetFetchPlan::kFailed;
      }
      // DriverTransport: RangeError('no endpoints configured') is thrown.
      if (!rpc_count) return FacetFetchPlan::kFailed;
      return token_fetch::EvmContractFacts(fetch, rpc_urls, rpc_count, ref.address)
                 ? FacetFetchPlan::kRequest : FacetFetchPlan::kFailed;
    }
    case kContract: {
      char buffer[16];
      const char* chain = MakeFacetPlanner(*work.env, in).GoPlusChain(buffer);
      if (!chain) return FacetFetchPlan::kValue;
      return token_fetch::GoPlus(fetch, chain, ref.address) ? FacetFetchPlan::kRequest : FacetFetchPlan::kValue;
    }
    case kFacetCount: break;
  }
  return FacetFetchPlan::kFailed;
}

// Records `liquidity` diagnostics/snapshots exactly where the TypeScript
// liquidity fetcher does, then answers the compact snapshot.
bool TokenFacetFetcher::LiquiditySuccess(Value* pools, Value** value) {
  Work& work = work_;
  const auto& in = inputs_;
  Arena& a = work.arena;
  const networks::TokenRef& ref = *in.ref;
  Value* detailed = nullptr;
  if (!token_liquidity::CalculateLiquiditySnapshot(a, pools, ref.address, token_liquidity::kAllPools,
                                                   &detailed))
    return false;
  Value* snapshot = is_null(detailed) ? tj::Null(a)
                                      : token_liquidity::CompactSnapshot(a, detailed, token_liquidity::kDefaultPoolLimit);
  if (!snapshot) return false;
  // Prime the opt-in detail key with what this live read already saw.
  char key[kKeyBytes];
  const int64_t now = work.Now();
  if (fact_key(in.network->id, ref.token_id, cache_facet_name("liquidity-detail", in.network), key, sizeof(key)))
    work.env->swr->Write(key, token_swr::kTokenLiquidityDetail, now, detailed ? detailed : tj::Null(a),
                         work.scratch, work.scratch_capacity, nullptr);
  if (in.persisted) {
    token_history::ObservationContext context{};
    context.source = MakeFacetPlanner(*work.env, in).LiquidityProvider();
    if (is_null(snapshot)) token_history::RecordNotFound(work, in.network->id, ref.token_id, context);
    else token_history::RecordSuccess(work, in.network->id, ref.token_id, snapshot, context);
  }
  *value = snapshot;
  return true;
}

// Interprets a finished request. False where the TypeScript fetcher throws.
bool TokenFacetFetcher::Interpret(Facet facet, const providers::TokenFetch& fetch, Value** value) {
  Work& work = work_;
  const auto& in = inputs_;
  Arena& a = work.arena;
  const networks::TokenRef& ref = *in.ref;
  *value = nullptr;
  using token_fetch::Outcome;
  switch (facet) {
    case kMarket: {
      Value* rows = nullptr;
      if (token_fetch::MarketRows(a, fetch, &rows) != Outcome::kValue) return false;
      *value = token_fetch::MarketRowFor(a, rows, in.coin_id);
      return *value != nullptr;
    }
    case kProfile: {
      Value* raw = nullptr;
      if (token_fetch::Profile(a, fetch, &raw) != Outcome::kValue || is_null(raw)) return false;
      return token_views::TrimProfile(a, raw, work.env->kv, work.Now(), value);
    }
    case kLiquidity: {
      Value* pools = nullptr;
      if (token_fetch::Pools(a, fetch, ref.address, MakeFacetPlanner(*work.env, in).DexScreenerSlug(), &pools) !=
          Outcome::kValue)
        return false;
      return LiquiditySuccess(pools, value);
    }
    case kHolders: {
      bool has = false;
      double count = 0;
      const char* source = nullptr;
      if (is(in.network->id, "eth-mainnet")) {
        if (token_fetch::EthplorerCount(a, fetch, &has, &count) != Outcome::kValue) return false;
        source = "ethplorer";
      } else {
        bool unsupported = false;
        if (token_fetch::EtherscanCount(a, fetch, &has, &count, &unsupported) != Outcome::kValue) return false;
        if (unsupported) mark_etherscan_unsupported(*work.env, MakeFacetPlanner(*work.env, in).EvmChainId());
        source = "etherscan";
      }
      if (!has) { *value = tj::Null(a); return *value != nullptr; }
      Value* o = tj::Obj(a);
      if (!o || !tj::Set(a, o, "count", tj::Num(a, count)) || !tj::Set(a, o, "atLeast", tj::Bool(a, false)) ||
          !tj::Set(a, o, "source", tj::Str(a, source)))
        return false;
      *value = o;
      return true;
    }
    case kChainMeta:
    case kInsight: {
      if (fetch.upstream == providers::TokenUpstream::kJupiter) {
        const Value* row = nullptr;
        if (token_fetch::JupiterExactRow(a, fetch, ref.address, &row) != Outcome::kValue) return false;
        *value = row ? token_views::SolanaChainMeta(a, row, work.env->kv, work.Now()) : tj::Null(a);
        return *value != nullptr;
      }
      Value* facts = nullptr;
      if (token_fetch::EvmFacts(a, fetch, &facts) != Outcome::kValue) return false;
      *value = facet == kChainMeta ? token_evm::TokenMetadata(a, facts) : token_evm::TokenInsight(a, facts);
      return *value != nullptr;
    }
    case kContract: {
      char buffer[16];
      const char* chain = MakeFacetPlanner(*work.env, in).GoPlusChain(buffer);
      return chain && token_fetch::Contract(a, fetch, chain, ref.address, work.Now(), value) == Outcome::kValue;
    }
    case kFacetCount: break;
  }
  return false;
}

// The fetcher's own failure side effect (the liquidity diagnostic).
void TokenFacetFetcher::RecordFailure(Facet facet) {
  Work& work = work_;
  const auto& in = inputs_;
  if (facet != kLiquidity || !in.persisted || !in.ref->has_address) return;
  token_history::RecordDiagnostic(work, in.network->id, in.ref->token_id, "error", work.Now(),
                                  MakeFacetPlanner(*work.env, in).LiquidityProvider());
}

}  // namespace fortis::tokens::token_facts
