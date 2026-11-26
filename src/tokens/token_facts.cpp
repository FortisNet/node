#include <string_view>
// Token 360 facts: names and provenance, the durable write-through, and the
// public entry points ComputeFacts and LiquidityDetails. The sub-facet
// planning, the sub-facet runner, coin-id resolution and background
// revalidation live in the token_facts_*.cpp units beside this file.
#include "token_facts_internal.h"
#include "token_facts_service.h"

namespace tj = fortis::codec::tj;

namespace fortis::tokens::token_facts {

namespace {

Value* copy_text(Arena& a, const char* text, size_t length) { return tj::Str(a, text, length); }

// `typeof v === 'number' && Number.isSafeInteger(v) && v >= 0`.
bool safe_count(const Value* v) {
  return tj::IsNumber(v) && isfinite(v->number) && v->number >= 0 && v->number <= 9007199254740991.0 &&
         floor(v->number) == v->number;
}

}  // namespace

namespace internal {

const networks::NativeNetworkConfig* seed_for(const char* id) {
  networks::NativeNetworkRegistry registry;
  size_t count = 0;
  const networks::NativeNetworkConfig* list = registry.list(&count);
  for (size_t i = 0; i < count; ++i) if (is(list[i].id, id)) return &list[i];
  return nullptr;
}

// cacheFacetName(facet, network).
const char* cache_facet_name(const char* facet, const networks::TokenNetwork* network) {
  const char* family = network ? network->family : nullptr;
  if (is(family, "bittensor") && is(facet, "market")) return "market-bittensor-v2";
  if (is(family, "hedera")) {
    if (is(facet, "market")) return "market-hedera-v1";
    if (is(facet, "profile")) return "profile-hedera-v1";
    if (is(facet, "chainMeta")) return "chainMeta-hedera-v1";
    if (is(facet, "insight")) return "insight-hedera-v1";
    if (is(facet, "liquidity")) return "liquidity-hedera-v1";
    if (is(facet, "liquidity-detail")) return "liquidity-detail-hedera-v1";
  }
  if (is(facet, "liquidity")) return "liquidity-v2";
  if (is(facet, "liquidity-detail")) return "liquidity-detail-v3";
  return facet;
}

bool fact_key(const char* network, const char* scope, const char* facet_name, char* out, size_t capacity) {
  const int n = snprintf(out, capacity, "tf:%s:%s:%s", network, scope, facet_name);
  return n > 0 && static_cast<size_t>(n) < capacity;
}

const token_swr::Ttl& facet_ttl(Facet facet) {
  switch (facet) {
    case kMarket: return token_swr::kTokenMarket;
    case kProfile: return token_swr::kTokenProfile;
    case kLiquidity: return token_swr::kTokenLiquidity;
    case kHolders: return token_swr::kTokenHolders;
    case kChainMeta: return token_swr::kTokenChainMeta;
    case kInsight: return token_swr::kTokenNativeInsight;
    case kContract: return token_swr::kTokenContract;
    case kFacetCount: break;
  }
  return token_swr::kTokenMarket;
}

bool is_fetch_facet(Facet facet) {
  return facet == kMarket || facet == kProfile || facet == kLiquidity || facet == kHolders;
}

// asHolderCount(count, source).
Value* as_holder_count(Arena& a, const Value* count, const char* source) {
  if (!safe_count(count)) return nullptr;
  Value* o = tj::Obj(a);
  if (!o || !tj::Set(a, o, "count", tj::Num(a, count->number)) ||
      !tj::Set(a, o, "atLeast", tj::Bool(a, false)) || !tj::Set(a, o, "source", tj::Str(a, source)))
    return nullptr;
  return o;
}

// isHolderObservation(value).
bool is_holder_observation(const Value* v) {
  if (!tj::IsObject(v)) return false;
  const Value* source = tj::Get(v, "source");
  return safe_count(tj::Get(v, "count")) && tj::IsBool(tj::Get(v, "atLeast")) && tj::IsString(source) &&
         source->length > 0;
}

}  // namespace internal

int64_t Work::Now() const { return env->swr->Now(); }

const char* FacetName(Facet facet) {
  switch (facet) {
    case kMarket: return "market";
    case kProfile: return "profile";
    case kLiquidity: return "liquidity";
    case kHolders: return "holders";
    case kChainMeta: return "chainMeta";
    case kInsight: return "insight";
    case kContract: return "contract";
    case kFacetCount: break;
  }
  return "market";
}

const char* ResolutionSourceName(ResolutionSource source) {
  switch (source) {
    case ResolutionSource::kNone: return nullptr;
    case ResolutionSource::kRef: return "ref";
    case ResolutionSource::kRegistry: return "registry";
    case ResolutionSource::kContract: return "contract";
    case ResolutionSource::kSearch: return "search";
    case ResolutionSource::kHint: return "hint";
  }
  return nullptr;
}

Value* IsoValue(Arena& arena, int64_t epoch_ms) {
  char iso[32];
  if (!codec::provider_text::IsoString(epoch_ms, iso)) return tj::Null(arena);
  return tj::Str(arena, iso);
}

Value* ProvenanceJson(Arena& a, const Provenance& p) {
  Value* o = tj::Obj(a);
  if (!o || !tj::Set(a, o, "hit", tj::Bool(a, p.hit)) ||
      !tj::Set(a, o, "timestamp", p.has_timestamp ? IsoValue(a, p.timestamp) : tj::Null(a)))
    return nullptr;
  if (p.partial && !tj::Set(a, o, "partial", tj::Bool(a, true))) return nullptr;
  return o;
}

Provenance CombineProvenance(const Provenance* entries, size_t count) {
  Provenance out{};
  if (!count) return out;
  size_t cached = 0;
  bool any = false;
  int64_t newest = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!entries[i].hit) continue;
    ++cached;
    if (entries[i].has_timestamp && (!any || entries[i].timestamp > newest)) {
      newest = entries[i].timestamp;
      any = true;
    }
  }
  out.hit = cached == count;
  out.has_timestamp = any;
  out.timestamp = newest;
  out.partial = cached > 0 && cached < count;
  return out;
}

bool HolderTrendsKey(const char* network, const char* token_id, const char* source, char* out,
                     size_t capacity) {
  char encoded[512];
  if (!token_fetch::EncodeComponent(source, encoded, sizeof(encoded))) return false;
  const int n = snprintf(out, capacity, "tf:%s:%s:holderTrends:%s", network, token_id, encoded);
  return n > 0 && static_cast<size_t>(n) < capacity;
}

// Every family's address refs reach ported providers: Hedera HTS refs the
// Mirror Node and SaucerSwap reads, Sui Coin<T> refs the GraphQL
// coinMetadata read, Bittensor subnet refs the keyed Taostats pool and
// TAO/USD reads (with no key the driver does not exist: honest nulls) and
// Robinhood Chain contracts the issuer catalog and reference quotes
// (token_robinhood.h).
FactsOutcome Resolve(std::string_view network_input, std::string_view ref_input,
               networks::TokenNetwork* network, const networks::NativeNetworkConfig** seed, networks::TokenRef* ref) {
  const char* network_id = network_input.data();
  const size_t network_length = network_input.size();
  const char* raw_ref = ref_input.data();
  const size_t raw_length = ref_input.size();
  if (!ResolveTokenNetwork(network_id, network_length, network)) return FactsOutcome::kUnknownNetwork;
  if (!ParseTokenRef(network, raw_ref, raw_length, ref)) return FactsOutcome::kUnsupportedRef;
  *seed = seed_for(network->id);
  return FactsOutcome::kOk;
}

// Holder trend read behind its own SWR entry (Token360Facet.holderTrends).
Value* TokenFactsService::HolderTrends(const networks::TokenNetwork& network, const networks::TokenRef& ref,
                                       const Value* holders, const FacetResult* selected) {
  Work& work = work_;
  Arena& a = work.arena;
  const Value* source = tj::Get(holders, "source");
  const char* source_text = tj::CString(a, source);
  char key[kKeyBytes];
  if (!source_text || !HolderTrendsKey(network.id, ref.token_id, source_text, key, sizeof(key))) return nullptr;
  const Value* count = tj::Get(holders, "count");
  const Value* at_least = tj::Get(holders, "atLeast");
  if (selected && selected->source == Source::kLive) {
    // recordHolderObservation: the write, then the invalidation.
    const int64_t observed = selected->has_as_of ? selected->as_of : work.Now();
    if (tj::IsNumber(count))
      token_history::RecordCount(work, network.id, ref.token_id, count->number,
                                 at_least && at_least->type == tj::Type::kTrue, source, observed);
    work.env->swr->Invalidate(key);
  }
  const int64_t now = work.Now();
  Value* cached = nullptr;
  int64_t as_of = 0;
  const token_swr::State state =
      work.env->swr->Read(key, now, work.scratch, work.scratch_capacity, a, &cached, &as_of);
  if (state != token_swr::State::kMissing) {
    if (state == token_swr::State::kStale && work.env->revalidation_queue && work.env->swr->BeginRevalidate(key, now)) {
      Revalidation task{};
      task.kind = RevalidationKind::kHolderTrends;
      task.key.Assign(key);
      task.network.Assign(network.id);
      task.token_id.Assign(ref.token_id);
      task.source.Assign(source_text);
      if (!work.env->revalidation_queue->Enqueue(task))
        work.env->swr->EndRevalidate(key, now, false, false);
    }
    return is_null(cached) ? nullptr : cached;
  }
  if (work.env->swr->MissHeld(key, now)) return nullptr;
  Value* trends = nullptr;
  if (!token_history::Trends(work, network.id, ref.token_id, source_text, now, &trends)) {
    work.env->swr->HoldMiss(key, now, false);
    return nullptr;
  }
  // The cached copy stores latestAt as JSON.stringify writes a Date.
  Value* stored = trends;
  if (tj::IsObject(trends)) {
    stored = tj::Clone(a, trends);
    const Value* latest = tj::Get(trends, "latestAt");
    if (stored && tj::IsNumber(latest))
      tj::Set(a, stored, "latestAt", IsoValue(a, static_cast<int64_t>(latest->number)));
  }
  work.env->swr->Write(key, token_swr::kTokenHolderTrends, now, stored ? stored : tj::Null(a), work.scratch,
                       work.scratch_capacity, nullptr);
  return is_null(stored) ? nullptr : stored;
}



// ============================================================================
// Token360Facet.facts
// ============================================================================

FactsOutcome TokenFactsService::Compute(std::string_view network_input, std::string_view ref_input,
                                             const char* coingecko_hint, uint32_t only_mask, Facts* facts) {
  Work& work = work_;
  const char* raw_ref = ref_input.data();
  const size_t raw_length = ref_input.size();
  Arena& a = work.arena;
  *facts = Facts{};
  const FactsOutcome resolved = Resolve(network_input, ref_input, &facts->network, &facts->seed, &facts->ref);
  if (resolved != FactsOutcome::kOk) return resolved;
  const networks::TokenNetwork& network = facts->network;
  const networks::TokenRef& ref = facts->ref;
  size_t begin = 0, end = 0;
  codec::provider_text::Trim(raw_ref, raw_length, &begin, &end);
  facts->input = copy_text(a, raw_ref + begin, end - begin);
  facts->errors = tj::Obj(a);
  if (!facts->input || !facts->errors) return FactsOutcome::kInternal;
  Value* errors = facts->errors;

  ResolveTokenIdentity(work, network, ref, coingecko_hint, &facts->resolution);
  const char* coin_id = facts->resolution.has_coin ? facts->resolution.coin_id : nullptr;
  // factScope: a hinted coin id gets its own cache scope.
  char* scope = static_cast<char*>(a.Alloc(networks::kTokenRefMaxIdBytes + kCoinIdBytes + 2, 1));
  if (!scope) return FactsOutcome::kInternal;
  if (facts->resolution.source == ResolutionSource::kHint)
    snprintf(scope, networks::kTokenRefMaxIdBytes + kCoinIdBytes + 2, "%s@%s", ref.token_id, coin_id);
  else
    snprintf(scope, networks::kTokenRefMaxIdBytes + kCoinIdBytes + 2, "%s", ref.token_id);
  const bool persisted = strcmp(scope, ref.token_id) == 0;
  auto wants = [&](Facet facet) { return (only_mask & FacetBit(facet)) != 0; };

  token_robinhood::Memo robinhood_memo;
  FacetInputs in{};
  in.network = &network;
  in.seed = facts->seed;
  in.ref = &ref;
  in.coin_id = coin_id;
  in.scope = scope;
  in.persisted = persisted;
  in.robinhood = &robinhood_memo;

  SubFacet subs[kFacetCount];
  for (size_t i = 0; i < kFacetCount; ++i) {
    subs[i].facet = static_cast<Facet>(i);
    subs[i].wanted = wants(static_cast<Facet>(i));
    if (!fact_key(network.id, scope, cache_facet_name(FacetName(subs[i].facet), &network), subs[i].key,
                  sizeof(subs[i].key)))
      return FactsOutcome::kInternal;
  }
  run_subfacets(work, in, subs, kFacetCount, errors);
  FacetResult& market_result = subs[kMarket].result;
  FacetResult& profile_result = subs[kProfile].result;
  FacetResult& liquidity_result = subs[kLiquidity].result;
  FacetResult& holders_result = subs[kHolders].result;
  FacetResult& chain_meta_result = subs[kChainMeta].result;
  FacetResult& insight_result = subs[kInsight].result;
  FacetResult contract_facet = subs[kContract].result;

  Value* chain_meta = chain_meta_result.value;
  Value* market = market_result.value ? token_views::MarketWithChainSupply(a, market_result.value, chain_meta)
                                      : nullptr;
  if (market && market->type == tj::Type::kNull) market = nullptr;
  Value* profile = profile_result.value;
  Value* liquidity = liquidity_result.value;

  // Holder candidates, most authoritative first.
  char goplus_buffer[16];
  const bool goplus_can_answer = MakeFacetPlanner(*work.env, in).GoPlusChain(goplus_buffer) != nullptr;
  const bool dedicated_can_answer = MakeFacetPlanner(*work.env, in).HolderSourceSupported();
  const bool solana = is(network.family, "solana");
  struct Candidate { Value* holders; const FacetResult* from; bool answers; };
  auto candidates_with = [&](const FacetResult& contract, Candidate out[3]) {
    out[0] = Candidate{holders_result.value, &holders_result, dedicated_can_answer};
    out[1] = Candidate{as_holder_count(a, tj::Get(chain_meta, "holderCount"), "jupiter"), &chain_meta_result, solana};
    out[2] = Candidate{as_holder_count(a, tj::Get(contract.value, "holderCount"), "goplus"), &contract, goplus_can_answer};
  };
  Candidate candidates[3];
  candidates_with(contract_facet, candidates);
  const Candidate* selected = nullptr;
  for (const Candidate& c : candidates) if (c.holders) { selected = &c; break; }

  // The lazy scanner probe for a holders-only refresh.
  if (!selected && wants(kHolders) && !wants(kContract) && goplus_can_answer) {
    Value* probe_errors = tj::Obj(a);
    SubFacet probe{};
    probe.facet = kContract;
    probe.wanted = true;
    memcpy(probe.key, subs[kContract].key, sizeof(probe.key));
    if (!probe_errors) return FactsOutcome::kInternal;
    run_subfacets(work, in, &probe, 1, probe_errors);
    contract_facet = probe.result;
    candidates_with(contract_facet, candidates);
    for (const Candidate& c : candidates) if (c.holders) { selected = &c; break; }
  }

  Value* holders = selected ? selected->holders : nullptr;
  Provenance holders_cache;
  if (selected) {
    holders_cache = selected->from->cache;
  } else {
    const Provenance entries[3] = {candidates[0].from->cache, candidates[1].from->cache, candidates[2].from->cache};
    holders_cache = CombineProvenance(entries, 3);
  }
  const FacetResult* selected_result = selected ? selected->from : nullptr;

  // The holders error contract.
  if (holders) {
    tj::Remove(errors, "holders");
  } else if (wants(kHolders)) {
    bool failed = false;
    for (const Candidate& c : candidates)
      failed = failed || (c.answers && (c.from->source == Source::kFailed ||
                                        (c.from->source == Source::kFloor && !c.from->value)));
    if (failed) tj::Set(a, errors, "holders", tj::Str(a, kUnavailable));
  }
  Value* trends = persisted && holders ? HolderTrends( network, ref, holders, selected_result) : nullptr;
  if (persisted && contract_facet.source == Source::kLive && contract_facet.value) {
    token_history::RecordDistribution(work, network.id, ref.token_id, contract_facet.value,
                                      contract_facet.has_as_of ? contract_facet.as_of : work.Now());
  }
  Value* holder_view = holders;
  if (holders && tj::IsObject(trends)) {
    holder_view = tj::Clone(a, holders);
    if (!holder_view) return FactsOutcome::kInternal;
    static const char* const kWindows[] = {"change24hPct", "change7dPct", "change30dPct", "change90dPct",
                                           "change365dPct"};
    for (const char* window : kWindows) {
      const Value* change = tj::Get(trends, window);
      if (!change || change->type == tj::Type::kNull) continue;
      if (!tj::Set(a, holder_view, window, tj::Clone(a, change))) return FactsOutcome::kInternal;
    }
  }

  // Safety: the scorer over the resolved sub-facets.
  token_views::SafetyInputs safety{};
  safety.is_native = ref.kind == networks::TokenRefKind::kNative;
  safety.coingecko_listed = coin_id != nullptr;
  double number = 0;
  safety.has_liquidity = token_views::FiniteNumber(tj::Get(liquidity, "totalUsd"), &number);
  safety.liquidity_usd = number;
  const Value* holder_count = tj::Get(holders, "count");
  if (holders && holder_count && holder_count->type != tj::Type::kNull) {
    // holders?.count ?? null: any non-nullish count is used as given.
    if (tj::IsNumber(holder_count)) { safety.has_holders = true; safety.holder_count = holder_count->number; }
  }
  if (token_views::FiniteNumber(tj::Get(market, "total_volume"), &number)) {
    safety.has_volume = true; safety.volume_24h_usd = number;
  } else if (token_views::FiniteNumber(tj::Get(liquidity, "volume24hUsd"), &number)) {
    safety.has_volume = true; safety.volume_24h_usd = number;
  }
  safety.has_market_cap = token_views::FiniteNumber(tj::Get(market, "market_cap"), &number);
  safety.market_cap_usd = safety.has_market_cap ? number : 0;
  double oldest = 0;
  if (token_views::FiniteNumber(tj::Get(liquidity, "oldestPairCreatedAt"), &oldest)) {
    safety.has_age = true;
    safety.trading_age_days = floor((static_cast<double>(work.Now()) - oldest) / 86400000.0);
  }
  facts->safety = token_views::ScoreTokenSafety(a, safety);
  if (!facts->safety) return FactsOutcome::kInternal;

  // What the tokens registry already knew.
  {
    TokenComparisonStore store(work.env->kv);
    token_store::IdentityRecord row{};
    if (store.ReadIdentity(network.id, ref.token_id, work.Now(), &row) == token_store::ReadStatus::kOk &&
        row.found) {
      facts->registry.found = true;
      facts->registry.symbol = row.symbol.present ? copy_text(a, row.symbol.text, row.symbol.length) : tj::Null(a);
      facts->registry.name = row.name.present ? copy_text(a, row.name.text, row.name.length) : tj::Null(a);
    }
  }

  if (persisted) {
    TokenFactChange market_change, profile_change, holders_change, contract_change, safety_change;
    // The provenance of a live market/profile row: the issuer catalog for a
    // Robinhood contract (whichever source answered), CoinGecko for a
    // resolved coin id, else SaucerSwap for a Hedera HTS ref.
    const char* fallback = token_robinhood::IssuerRef(network, ref) ? "robinhood-assets"
                         : coin_id                                ? "coingecko"
                         : MakeFacetPlanner(*work.env, network, ref).SaucerSwapSupported()    ? "saucerswap"
                                                                  : nullptr;
    if (market_result.source == Source::kLive)
      market_change = TokenFactWriter(work).Change(market, market_result.has_as_of, market_result.as_of, fallback, coin_id);
    if (profile_result.source == Source::kLive)
      profile_change = TokenFactWriter(work).Change(profile, profile_result.has_as_of, profile_result.as_of, fallback, coin_id);
    if (selected_result && selected_result->source == Source::kLive)
      holders_change = TokenFactWriter(work).Change(holders, selected_result->has_as_of, selected_result->as_of, nullptr,
                                      nullptr);
    if (contract_facet.source == Source::kLive)
      contract_change = TokenFactWriter(work).Change(contract_facet.value, contract_facet.has_as_of, contract_facet.as_of,
                                       goplus_can_answer ? "goplus" : nullptr, nullptr);
    if (market_result.source == Source::kLive || liquidity_result.source == Source::kLive ||
        (selected_result && selected_result->source == Source::kLive))
      safety_change = TokenFactWriter(work).Change(facts->safety, true, work.Now(), "fortis", nullptr);
    if (market_change.present || profile_change.present || holders_change.present || contract_change.present ||
        safety_change.present)
      TokenFactWriter(work).Write( network.id, ref.token_id, market_change, profile_change, holders_change,
                    contract_change, safety_change);
  }

  facts->chain_meta = chain_meta;
  facts->market = market;
  facts->profile = profile;
  facts->liquidity = liquidity;
  facts->holders = holder_view;
  // Driver sections, plus Solana's, which ride the chain-meta read rather
  // than costing a request of their own.
  facts->insight = tj::Arr(a);
  if (!facts->insight) return FactsOutcome::kInternal;
  if (tj::IsArray(insight_result.value))
    for (uint32_t i = 0; i < insight_result.value->count; ++i)
      if (!tj::Push(a, facts->insight, insight_result.value->items[i])) return FactsOutcome::kInternal;
  if (wants(kInsight) && solana && !token_views::AppendSolanaInsight(a, chain_meta, facts->insight))
    return FactsOutcome::kInternal;
  facts->contract = contract_facet.value;
  facts->cache[kMarket] = market_result.cache;
  facts->cache[kProfile] = profile_result.cache;
  facts->cache[kLiquidity] = liquidity_result.cache;
  facts->cache[kHolders] = holders_cache;
  facts->cache[kChainMeta] = chain_meta_result.cache;
  facts->cache[kContract] = contract_facet.cache;
  if (solana) {
    const Provenance parts[2] = {insight_result.cache, chain_meta_result.cache};
    facts->cache[kInsight] = CombineProvenance(parts, 2);
  } else {
    facts->cache[kInsight] = insight_result.cache;
  }
  return a.ok ? FactsOutcome::kOk : FactsOutcome::kInternal;
}

// ============================================================================
// Token360Facet.liquidityDetails
// ============================================================================

FactsOutcome TokenFactsService::LiquidityDetails(std::string_view network_input, std::string_view ref_input,
                                                      Value** document) {
  Work& work = work_;
  Arena& a = work.arena;
  *document = nullptr;
  networks::TokenNetwork network;
  const networks::NativeNetworkConfig* seed = nullptr;
  networks::TokenRef ref;
  const FactsOutcome resolved = Resolve(network_input, ref_input, &network, &seed, &ref);
  if (resolved != FactsOutcome::kOk) return resolved;
  Value* errors = tj::Obj(a);
  if (!errors) return FactsOutcome::kInternal;
  char key[kKeyBytes];
  if (!fact_key(network.id, ref.token_id, cache_facet_name("liquidity-detail", &network), key, sizeof(key)))
    return FactsOutcome::kInternal;
  const char* source = MakeFacetPlanner(*work.env, network, ref).LiquidityProvider();
  token_swr::Cache* swr = work.env->swr;
  const int64_t now = work.Now();
  FacetResult result{};
  Value* cached = nullptr;
  int64_t as_of = 0;
  const token_swr::State state = swr->Read(key, now, work.scratch, work.scratch_capacity, a, &cached, &as_of);
  bool failed = false;
  if (state != token_swr::State::kMissing) {
    // TTL.tokenLiquidityDetail has no stale window: a hit is always fresh.
    result.value = is_null(cached) ? nullptr : cached;
    result.cache = Provenance{true, true, as_of, false};
  } else if (swr->MissHeld(key, now)) {
    failed = true;
  } else {
    Value* value = tj::Null(a);
    bool ok = true;
    const char* slug = MakeFacetPlanner(*work.env, network, ref).DexScreenerSlug();
    // liquidityPools: DexScreener where the network has a slug, SaucerSwap
    // for a Hedera HTS ref, else unsupported (null, nothing recorded).
    Value* pools = nullptr;
    bool read = false;
    bool lane_busy = false;
    if (ref.has_address && slug) {
      auto fetch_storage = providers::ConstructArenaFetches(static_cast<providers::TokenFetch*>(a.Alloc(sizeof(providers::TokenFetch))), 1);
  auto* fetch = fetch_storage.get();
      if (!fetch) return FactsOutcome::kInternal;
      *fetch = providers::TokenFetch{};
      fetch->background = work.background;
      ok = token_fetch::DexPools(fetch, ref.address);
      if (ok) {
        work.env->executor->FetchAll(fetch, 1, work.deadline_ms);
        ok = token_fetch::Pools(a, *fetch, ref.address, slug, &pools) == token_fetch::Outcome::kValue;
        TokenFetchRelease(fetch);
        read = true;
      }
    } else if (MakeFacetPlanner(*work.env, network, ref).SaucerSwapSupported()) {
      auto stage_storage = providers::ConstructArenaFetches(static_cast<providers::TokenFetch*>(a.Alloc(sizeof(providers::TokenFetch) * kSharedStageSlots)), kSharedStageSlots);
  auto* stage = stage_storage.get();
      if (!stage) return FactsOutcome::kInternal;
      LanePlan plan;
      plan.Foreground(token_ledger::kSaucerPoolsPart);
      SharedRead shared;
      read_shared(work, network, seed, ref, plan, stage, 0, &shared);
      ok = !shared.Failed(kLiquidity) &&
           saucerswap::Pools(a, shared.Page(token_ledger::kSaucerPoolsPart), ref.address, &pools) ==
               saucerswap::PoolsStatus::kOk;
      lane_busy = shared.LaneBusy(kLiquidity);
      read = true;
    }
    if (ok && read) {
      Value* detailed = nullptr;
      ok = token_liquidity::CalculateLiquiditySnapshot(a, pools, ref.address, token_liquidity::kAllPools, &detailed);
      if (ok) {
        token_history::ObservationContext context{};
        context.source = source;
        if (is_null(detailed)) {
          token_history::RecordNotFound(work, network.id, ref.token_id, context);
          value = tj::Null(a);
        } else {
          Value* compact = token_liquidity::CompactSnapshot(a, detailed, token_liquidity::kDefaultPoolLimit);
          if (!compact) return FactsOutcome::kInternal;
          token_history::RecordSuccess(work, network.id, ref.token_id, compact, context);
          value = detailed;
        }
      }
    }
    if (ok) {
      int64_t stored_at = work.Now();
      swr->Write(key, token_swr::kTokenLiquidityDetail, stored_at, value, work.scratch, work.scratch_capacity,
                 &stored_at);
      result.value = is_null(value) ? nullptr : value;
      result.cache = Provenance{false, true, stored_at, false};
    } else {
      swr->HoldMiss(key, work.Now(), lane_busy);
      failed = true;
    }
  }
  if (failed) {
    // The detail read has no durable floor.
    result = FacetResult{};
    tj::Set(a, errors, "liquidity-detail", tj::Str(a, kUnavailable));
  }
  Value* doc = tj::Obj(a);
  Value* provenance = ProvenanceJson(a, result.cache);
  if (!doc || !provenance || !tj::Set(a, doc, "network", tj::Str(a, network.id)) ||
      !tj::Set(a, doc, "tokenId", tj::Str(a, ref.token_id)) ||
      !tj::Set(a, doc, "liquidity", result.value ? result.value : tj::Null(a)) ||
      !tj::Set(a, doc, "errors", errors) || !tj::Set(a, doc, "cache", provenance))
    return FactsOutcome::kInternal;
  *document = doc;
  return FactsOutcome::kOk;
}

FactsOutcome ComputeFacts(Work& work, std::string_view network, std::string_view ref,
                          const char* hint, uint32_t mask, Facts* facts) {
  return TokenFactsService(work).Compute(network, ref, hint, mask, facts);
}
FactsOutcome LiquidityDetails(Work& work, std::string_view network, std::string_view ref, Value** document) {
  return TokenFactsService(work).LiquidityDetails(network, ref, document);
}

}  // namespace fortis::tokens::token_facts
