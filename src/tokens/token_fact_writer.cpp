#include "token_fact_writer.h"
#include "token_facts_internal.h"
namespace tj = fortis::codec::tj;
namespace fortis::tokens::token_facts {
namespace { Value* str(Arena& a, const char* text) { return text ? tj::Str(a, text) : tj::Null(a); } }
TokenFactChange TokenFactWriter::Change(Value* value, bool has_as_of, int64_t as_of, const char* fallback,
                                        const char* candidate_source_id) {
  Arena& a = work_.arena;
  TokenFactChange change{};
  change.present = true;
  change.value = value;
  change.has_as_of = has_as_of;
  change.as_of = as_of;
  const Value* embedded = tj::IsObject(value) ? tj::Get(value, "source") : nullptr;
  change.source = tj::IsString(embedded) && embedded->length ? tj::CString(a, embedded) : fallback;
  change.source_id = is(change.source, "coingecko") ? candidate_source_id : nullptr;
  return change;
}

bool TokenFactWriter::Write(const char* network, const char* token_id, const TokenFactChange& market,
                            const TokenFactChange& profile, const TokenFactChange& holders,
                            const TokenFactChange& contract, const TokenFactChange& safety) {
  Work& work = work_;
  Arena& a = work.arena;
  const int64_t now = work.Now();
  Value* fields = tj::Obj(a);
  if (!fields) return false;
  auto put = [&](const char* name, const TokenFactChange& change, const char* as_of_name, const char* source_name,
                 const char* source_id_name) {
    if (!change.present) return true;
    const int64_t at = change.has_as_of ? change.as_of : now;
    return tj::Set(a, fields, name, change.value ? change.value : tj::Null(a)) &&
           tj::Set(a, fields, as_of_name, tj::Num(a, static_cast<double>(at))) &&
           tj::Set(a, fields, source_name, str(a, change.source)) &&
           (!source_id_name || tj::Set(a, fields, source_id_name, str(a, change.source_id)));
  };
  if (!put("market", market, "marketAsOf", "marketSource", "marketSourceId") ||
      !put("profile", profile, "profileAsOf", "profileSource", "profileSourceId") ||
      !put("holders", holders, "holdersAsOf", "holdersSource", nullptr) ||
      !put("contract", contract, "contractAsOf", "contractSource", nullptr) ||
      !put("safety", safety, "safetyAsOf", "safetySource", nullptr))
    return false;
  // TypeScript patches facts and comparison in two transactions; one frame
  // here (token_writes::WriteThrough) closes the window between them.
  token_store::ComparisonPatch patch{};
  const bool market_row = market.present && tj::IsObject(market.value);
  if (market_row) {
    const int64_t at = market.has_as_of ? market.as_of : now;
    token_views::MarketComparisonPatch(a, market.value, at, market.source_id,
                                       market.source ? market.source : "unknown", &patch);
  }
  return token_writes::WriteThrough(*work.env->data, network, token_id, fields, market_row ? &patch : nullptr, now);
}

void TokenFactWriter::PersistRevalidated(Facet facet, const networks::TokenNetwork& network, const networks::NativeNetworkConfig* seed,
                         const char* token_id, const char* coin_id, Value* value, int64_t as_of) {
  Work& work = work_;
  Arena& a = work.arena;
  networks::TokenRef ref;
  const bool parsed = ParseTokenRef(&network, token_id, strlen(token_id), &ref);
  TokenFactChange market, profile, holders, contract, safety;
  // persistRevalidatedFacet: the issuer catalog for a Robinhood contract,
  // CoinGecko for a coin id, else SaucerSwap for any Hedera network
  // (TypeScript checks only the family here).
  const char* fallback = parsed && token_robinhood::IssuerRef(network, ref) ? "robinhood-assets"
                       : coin_id                                            ? "coingecko"
                       : is(network.family, "hedera")                       ? "saucerswap"
                                                                            : nullptr;
  if (facet == kMarket) market = Change(value, true, as_of, fallback, coin_id);
  if (facet == kProfile) profile = Change(value, true, as_of, fallback, coin_id);
  if (facet == kContract) {
    char buffer[16];
    FacetInputs in{};
    in.network = &network;
    in.seed = seed;
    in.ref = &ref;
    const bool goplus = parsed && MakeFacetPlanner(*work.env, in).GoPlusChain(buffer) != nullptr;
    contract = Change(value, true, as_of, goplus ? "goplus" : nullptr, nullptr);
  }
  if (facet == kHolders && is_holder_observation(value)) {
    holders = Change(value, true, as_of, nullptr, nullptr);
    const Value* source = tj::Get(value, "source");
    token_history::RecordCount(work, network.id, token_id, tj::Get(value, "count")->number,
                               tj::Get(value, "atLeast")->type == tj::Type::kTrue, source, as_of);
    char key[kKeyBytes];
    const char* source_text = tj::CString(a, source);
    if (source_text && HolderTrendsKey(network.id, token_id, source_text, key, sizeof(key)))
      work.env->swr->Invalidate(key);
  }
  // Side-effect holder counts keep their own daily series.
  const char* side = facet == kChainMeta ? "jupiter" : facet == kContract ? "goplus" : nullptr;
  if (side && tj::IsObject(value)) {
    Value* count = as_holder_count(a, tj::Get(value, "holderCount"), side);
    if (count) {
      Value* source = tj::Get(count, "source");
      token_history::RecordCount(work, network.id, token_id, tj::Get(count, "count")->number, false, source, as_of);
      char key[kKeyBytes];
      if (HolderTrendsKey(network.id, token_id, side, key, sizeof(key))) work.env->swr->Invalidate(key);
    }
  }
  if (market.present || profile.present || holders.present || contract.present)
    Write( network.id, token_id, market, profile, holders, contract, safety);
}

}  // namespace fortis::tokens::token_facts
