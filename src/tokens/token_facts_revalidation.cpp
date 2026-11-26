// Token 360 facts: background revalidation (SwrCache.revalidate +
// persistRevalidatedFacet), single and batched.
#include "token_facts_internal.h"
#include "token_facts_service.h"

namespace tj = fortis::codec::tj;

namespace fortis::tokens::token_facts {

void TokenFactsService::Revalidate(const Revalidation& task) {
  Work& work = work_;
  Arena& a = work.arena;
  token_swr::Cache* swr = work.env->swr;
  networks::TokenNetwork network;
  networks::TokenRef ref;
  const networks::NativeNetworkConfig* seed = nullptr;
  bool failed = true;
  bool lane_busy = false;
  if (ResolveTokenNetwork(task.network.data(), strlen(task.network.data()), &network) &&
      ParseTokenRef(&network, task.token_id.data(), strlen(task.token_id.data()), &ref)) {
    seed = seed_for(network.id);
    switch (task.kind) {
      case RevalidationKind::kResolution: {
        Value* live = nullptr;
        if (ResolveTokenIdentityLive(work, network, ref, &live)) {
          swr->Write(task.key.data(), token_swr::kTokenIdentity, work.Now(), live, work.scratch, work.scratch_capacity,
                     nullptr);
          failed = false;
        }
        break;
      }
      case RevalidationKind::kHolderTrends: {
        Value* trends = nullptr;
        if (token_history::Trends(work, network.id, ref.token_id, task.source.data(), work.Now(), &trends)) {
          Value* stored = trends;
          if (tj::IsObject(trends)) {
            stored = tj::Clone(a, trends);
            const Value* latest = tj::Get(trends, "latestAt");
            if (stored && tj::IsNumber(latest))
              tj::Set(a, stored, "latestAt", IsoValue(a, static_cast<int64_t>(latest->number)));
          }
          swr->Write(task.key.data(), token_swr::kTokenHolderTrends, work.Now(), stored ? stored : tj::Null(a),
                     work.scratch, work.scratch_capacity, nullptr);
          failed = false;
        }
        break;
      }
      case RevalidationKind::kFacet:
      case RevalidationKind::kLiquidityDetail: {
        token_robinhood::Memo robinhood_memo;
        FacetInputs in{};
        in.network = &network;
        in.seed = seed;
        in.ref = &ref;
        in.coin_id = task.has_coin ? task.coin_id.data() : nullptr;
        in.scope = task.scope.data();
        in.persisted = task.persisted;
        in.robinhood = &robinhood_memo;
        auto fetch_storage = providers::ConstructArenaFetches(static_cast<providers::TokenFetch*>(a.Alloc(sizeof(providers::TokenFetch) * kSharedStageSlots)), kSharedStageSlots);
  auto* fetch = fetch_storage.get();
        if (!fetch) break;
        *fetch = providers::TokenFetch{};
        Value* value = nullptr;
        const FacetFetchPlan plan = TokenFacetFetcher(work, in).Prepare(task.facet, fetch, &value, &lane_busy);
        bool ok = plan == FacetFetchPlan::kValue;
        bool diagnostic = true;
        if (plan == FacetFetchPlan::kSnapshot) {
          const uint8_t parts = MakeFacetPlanner(*work.env, in).SharedParts(task.facet, in.coin_id != nullptr);
          LanePlan lanes;
          const token_ledger::Family family = token_ledger::FamilyOf(network, ref);
          if (family == token_ledger::Family::kHedera) lanes.Revalidate(parts);
          else if (family == token_ledger::Family::kBittensor) lanes.Background(parts);
          else lanes.Foreground(parts);
          SharedRead shared;
          read_shared(work, network, seed, ref, lanes, fetch, 0, &shared);
          ok = shared_value(work, task.facet, in, shared, &value, &diagnostic, true);
          lane_busy = !ok && shared.LaneBusy(task.facet);
        }
        if (plan == FacetFetchPlan::kRequest) {
          work.env->executor->FetchAll(fetch, 1, work.deadline_ms);
          lane_busy = fetch->lane_rejected;
          ok = TokenFacetFetcher(work, in).Interpret(task.facet, *fetch, &value);
          TokenFetchRelease(fetch);
        }
        if (!ok) {
          if (diagnostic) TokenFacetFetcher(work, in).RecordFailure(task.facet);
          break;
        }
        int64_t as_of = work.Now();
        swr->Write(task.key.data(), facet_ttl(task.facet), as_of, value ? value : tj::Null(a), work.scratch,
                   work.scratch_capacity, &as_of);
        if (task.persisted)
          TokenFactWriter(work).PersistRevalidated( task.facet, network, seed, ref.token_id, in.coin_id,
                              is_null(value) ? nullptr : value, as_of);
        failed = false;
        break;
      }
    }
  }
  swr->EndRevalidate(task.key.data(), work.Now(), failed, lane_busy);
}

bool BatchableMarket(const Revalidation& task) {
  if (task.kind != RevalidationKind::kFacet || task.facet != kMarket || !task.has_coin) return false;
  // A Robinhood contract's market asks the issuer quote before CoinGecko.
  networks::TokenNetwork network;
  networks::TokenRef ref;
  return !(ResolveTokenNetwork(task.network.data(), strlen(task.network.data()), &network) &&
           ParseTokenRef(&network, task.token_id.data(), strlen(task.token_id.data()), &ref) &&
           token_robinhood::IssuerRef(network, ref));
}

bool BatchableSnapshot(const Revalidation& task, const Env& env) {
  if (task.kind != RevalidationKind::kFacet) return false;
  networks::TokenNetwork network;
  networks::TokenRef ref;
  return ResolveTokenNetwork(task.network.data(), strlen(task.network.data()), &network) &&
         ParseTokenRef(&network, task.token_id.data(), strlen(task.token_id.data()), &ref) &&
         MakeFacetPlanner(env, network, ref).SnapshotBacked(task.facet, task.has_coin);
}

bool SameSnapshot(const Revalidation& a, const Revalidation& b) {
  return strcmp(a.network.data(), b.network.data()) == 0 && strcmp(a.token_id.data(), b.token_id.data()) == 0;
}

void TokenFactsService::RevalidateSnapshots(const Revalidation* const* tasks, size_t count) {
  Work& work = work_;
  if (!count) return;
  Arena& a = work.arena;
  token_swr::Cache* swr = work.env->swr;
  networks::TokenNetwork network;
  networks::TokenRef ref;
  bool resolved = ResolveTokenNetwork(tasks[0]->network.data(), strlen(tasks[0]->network.data()), &network) &&
                  ParseTokenRef(&network, tasks[0]->token_id.data(), strlen(tasks[0]->token_id.data()), &ref) &&
                  MakeFacetPlanner(*work.env, network, ref).SnapshotBacked(tasks[0]->facet, tasks[0]->has_coin);
  const networks::NativeNetworkConfig* seed = resolved ? seed_for(network.id) : nullptr;
  SharedRead shared;
  auto stage_storage = providers::ConstructArenaFetches(static_cast<providers::TokenFetch*>(a.Alloc(sizeof(providers::TokenFetch) * kSharedStageSlots)), kSharedStageSlots);
  auto* stage = stage_storage.get();
  // Sibling facets of one token share the read. Hedera reads only the parts
  // the batch needs, admitted as TypeScript's lanes admit revalidations
  // that start together (LanePlan).
  LanePlan plan;
  const token_ledger::Family family = resolved ? token_ledger::FamilyOf(network, ref) : token_ledger::Family::kNone;
  if (resolved)
    for (size_t i = 0; i < count; ++i) {
      const uint8_t parts = MakeFacetPlanner(*work.env, network, ref).SharedParts(tasks[i]->facet, tasks[i]->has_coin);
      if (family == token_ledger::Family::kHedera) plan.Revalidate(parts);
      else if (family == token_ledger::Family::kBittensor) plan.Background(parts);
      else plan.Foreground(parts);
    }
  if (resolved && stage) read_shared(work, network, seed, ref, plan, stage, 0, &shared);
  for (size_t i = 0; i < count; ++i) {
    const Revalidation& task = *tasks[i];
    FacetInputs in{};
    in.network = &network;
    in.seed = seed;
    in.ref = &ref;
    in.coin_id = task.has_coin ? task.coin_id.data() : nullptr;
    in.scope = task.scope.data();
    in.persisted = task.persisted;
    Value* value = nullptr;
    bool diagnostic = true;
    const bool ok = resolved && stage && shared_value(work, task.facet, in, shared, &value, &diagnostic, true);
    if (ok) {
      int64_t as_of = work.Now();
      swr->Write(task.key.data(), facet_ttl(task.facet), as_of, value, work.scratch, work.scratch_capacity, &as_of);
      if (task.persisted)
        TokenFactWriter(work).PersistRevalidated( task.facet, network, seed, ref.token_id, in.coin_id,
                            is_null(value) ? nullptr : value, as_of);
    } else if (resolved && diagnostic) {
      TokenFacetFetcher(work, in).RecordFailure(task.facet);
    }
    swr->EndRevalidate(task.key.data(), work.Now(), !ok, !ok && shared.LaneBusy(task.facet));
  }
}

void TokenFactsService::RevalidateMarkets(const Revalidation* const* tasks, size_t count) {
  Work& work = work_;
  Arena& a = work.arena;
  token_swr::Cache* swr = work.env->swr;
  const char* ids[256];
  size_t id_count = 0;
  for (size_t i = 0; i < count && id_count < 256; ++i) {
    bool seen = false;
    for (size_t k = 0; k < id_count && !seen; ++k) seen = strcmp(ids[k], tasks[i]->coin_id.data()) == 0;
    if (!seen) ids[id_count++] = tasks[i]->coin_id.data();
  }
  auto fetch_storage = providers::ConstructArenaFetches(static_cast<providers::TokenFetch*>(a.Alloc(sizeof(providers::TokenFetch))), 1);
  auto* fetch = fetch_storage.get();
  Value* rows = nullptr;
  bool lane_busy = false;
  bool ok = fetch != nullptr;
  if (ok) {
    *fetch = providers::TokenFetch{};
    fetch->background = true;
    ok = token_fetch::CoinMarkets(fetch, ids, id_count);
    if (ok) {
      work.env->executor->FetchAll(fetch, 1, work.deadline_ms);
      lane_busy = fetch->lane_rejected;
      ok = token_fetch::MarketRows(a, *fetch, &rows) == token_fetch::Outcome::kValue;
      TokenFetchRelease(fetch);
    }
  }
  for (size_t i = 0; i < count; ++i) {
    const Revalidation& task = *tasks[i];
    bool failed = !ok;
    if (ok) {
      networks::TokenNetwork network;
      networks::TokenRef ref;
      if (ResolveTokenNetwork(task.network.data(), strlen(task.network.data()), &network) &&
          ParseTokenRef(&network, task.token_id.data(), strlen(task.token_id.data()), &ref)) {
        Value* value = token_fetch::MarketRowFor(a, rows, task.coin_id.data());
        int64_t as_of = work.Now();
        swr->Write(task.key.data(), token_swr::kTokenMarket, as_of, value ? value : tj::Null(a), work.scratch,
                   work.scratch_capacity, &as_of);
        if (task.persisted)
          TokenFactWriter(work).PersistRevalidated( kMarket, network, seed_for(network.id), ref.token_id, task.coin_id.data(),
                              is_null(value) ? nullptr : value, as_of);
      } else {
        failed = true;
      }
    }
    swr->EndRevalidate(task.key.data(), work.Now(), failed, lane_busy);
  }
}

// Compatibility entry points for existing facts clients.
void RunRevalidation(Work& work, const Revalidation& task) { TokenFactsService(work).Revalidate(task); }
void RunSnapshotRevalidations(Work& work, const Revalidation* const* tasks, size_t count) {
  TokenFactsService(work).RevalidateSnapshots(tasks, count);
}
void RunMarketRevalidations(Work& work, const Revalidation* const* tasks, size_t count) {
  TokenFactsService(work).RevalidateMarkets(tasks, count);
}

}  // namespace fortis::tokens::token_facts
