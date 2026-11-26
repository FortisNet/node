// Token 360 facts: the sub-facet runner (Token360Facet.subFacet over
// SwrCache.read): shared driver reads, cache reads, one provider batch for
// the misses, floors, stats and revalidation hand-off.
#include "token_facts_internal.h"

namespace tj = fortis::codec::tj;

namespace fortis::tokens::token_facts {

namespace {

// Token360Facet.readFactFloor(network, tokenId, facet).
bool read_floor(Work& work, const char* network, const char* scope, Facet facet, Value** value,
                int64_t* as_of) {
  const char* field = nullptr;
  const char* field_as_of = nullptr;
  switch (facet) {
    case kMarket: field = "market"; field_as_of = "marketAsOf"; break;
    case kProfile: field = "profile"; field_as_of = "profileAsOf"; break;
    case kLiquidity: field = "liquidity"; field_as_of = "liquidityAsOf"; break;
    case kHolders: field = "holders"; field_as_of = "holdersAsOf"; break;
    case kContract: field = "contract"; field_as_of = "contractAsOf"; break;
    default: return false;
  }
  Value* row = nullptr;
  if (work.env->data->ReadFacts(network, scope, work.Now(), work.arena, &row) != token_data::DataOutcome::kOk ||
      !tj::IsObject(row))
    return false;
  if (facet == kLiquidity) {
    const Value* status = tj::Get(row, "liquidityStatus");
    if (tj::StringEquals(status, "not_found") || tj::StringEquals(status, "unsupported")) return false;
  }
  const Value* date = tj::Get(row, field_as_of);
  if (!tj::IsNumber(date)) return false;
  Value* stored = tj::Get(row, field);
  *value = is_null(stored) ? nullptr : stored;
  *as_of = static_cast<int64_t>(date->number);
  return true;
}

void floor_or_fail(Work& work, SubFacet& sub, const FacetInputs& in, bool* failed) {
  *failed = false;
  Value* value = nullptr;
  int64_t as_of = 0;
  if (in.persisted && read_floor(work, in.network->id, in.scope, sub.facet, &value, &as_of)) {
    sub.result.value = value;
    sub.result.source = Source::kFloor;
    sub.result.has_as_of = true;
    sub.result.as_of = as_of;
    sub.result.cache = Provenance{true, true, as_of, false};
    return;
  }
  *failed = true;
  sub.result = FacetResult{};
  sub.result.source = Source::kFailed;
}

void record_stats(Work& work, const SubFacet& sub, const FacetInputs& in, bool failure) {
  if (!work.env->stats || !is_fetch_facet(sub.facet)) return;
  token_jobs::FetchSample sample{};
  sample.network = in.network->id;
  // canonicalScope(scope): the token id before a caller-hint suffix.
  char canonical[networks::kTokenRefMaxIdBytes + 1];
  const char* at = strchr(in.scope, '@');
  const size_t length = at ? static_cast<size_t>(at - in.scope) : strlen(in.scope);
  if (length >= sizeof(canonical)) return;
  memcpy(canonical, in.scope, length);
  canonical[length] = '\0';
  sample.token_id = canonical;
  sample.facet = sub.facet == kMarket ? token_jobs::FetchFacet::kMarket
               : sub.facet == kProfile ? token_jobs::FetchFacet::kProfile
               : sub.facet == kLiquidity ? token_jobs::FetchFacet::kLiquidity
                                          : token_jobs::FetchFacet::kHolders;
  sample.source_id = in.coin_id;
  if (failure) {
    sample.outcome = token_jobs::FetchOutcome::kFailure;
  } else {
    sample.outcome = sub.result.source == Source::kLive ? token_jobs::FetchOutcome::kLive
                   : sub.result.source == Source::kFloor ? token_jobs::FetchOutcome::kFloor
                   : sub.stale ? token_jobs::FetchOutcome::kStale
                               : token_jobs::FetchOutcome::kFresh;
    sample.negative = sub.result.value == nullptr;
  }
  sample.foreground = true;
  sample.latency_ms = static_cast<double>(providers::TokenMonotonicMs() - sub.started);
  sample.at_ms = work.Now();
  work.env->stats->Record(sample);
}

void enqueue_facet_revalidation(Work& work, const SubFacet& sub, const FacetInputs& in) {
  if (!work.env->revalidation_queue) return;
  Revalidation task{};
  task.kind = RevalidationKind::kFacet;
  task.facet = sub.facet;
  task.persisted = in.persisted;
  task.has_coin = in.coin_id != nullptr;
  task.key.Assign(sub.key);
  task.network.Assign(in.network->id);
  task.token_id.Assign(in.ref->token_id);
  task.scope.Assign(in.scope);
  if (in.coin_id) snprintf(task.coin_id.data(), task.coin_id.capacity(), "%s", in.coin_id);
  if (!work.env->swr->BeginRevalidate(task.key.data(), work.Now())) return;
  if (!work.env->revalidation_queue->Enqueue(task))
    work.env->swr->EndRevalidate(task.key.data(), work.Now(), false, false);
}

}  // namespace

namespace internal {

// The reads of `parts` for a shared_backed ref (the Tron snapshot ignores
// them), with the Hedera revalidation lane plan's background and refused
// parts. `batch[0..extra)` are the caller's own requests, run in its first
// provider batch; `batch` has kSharedStageSlots more slots.
void read_shared(Work& work, const networks::TokenNetwork& network, const networks::NativeNetworkConfig* seed, const networks::TokenRef& ref,
                 const LanePlan& plan, providers::TokenFetch* batch, size_t extra, SharedRead* out) {
  *out = SharedRead{};
  if (token_tron::Snapshotted(network, ref)) {
    out->tron = true;
    const token_tron::Outcome outcome = token_tron::Read(work, network, seed, ref.address, batch, extra, &out->snapshot);
    out->tron_failed = outcome == token_tron::Outcome::kFailed;
    out->present = outcome == token_tron::Outcome::kSnapshot;
    return;
  }
  out->family = token_ledger::FamilyOf(network, ref);
  out->network = &network;
  out->ref = &ref;
  token_ledger::Read(work, network, seed, ref, static_cast<uint8_t>(plan.read | plan.refused), batch, extra,
                     &out->pages, plan.background, plan.refused);
}

// The shared-read sub-facet's answer: tokenMetadata, tokenInsight or
// readTokenHolders over one Tron snapshot; tokenHolders / tokenMetadata /
// tokenInsight over the ledger read's part (with the SaucerSwap listing
// section appended when that optional part answered); or the SaucerSwap
// market row, profile or pools (liquidity then runs the fetcher's snapshot
// and persistence); or Bittensor's tokenMetadata / tokenInsight over the
// pool and its market row (pool plus the fail-soft TAO/USD row, falling back
// to CoinGecko's TAO quote at background priority for a `revalidation`).
// False where the fetcher fails; *diagnostic is then false when the
// liquidity fetcher would not record its 'error' diagnostic (a
// calculateLiquiditySnapshot TypeError after the pool read answered).
bool shared_value(Work& work, Facet facet, const FacetInputs& in, const SharedRead& read, Value** value,
                  bool* diagnostic, bool revalidation) {
  using namespace token_ledger;
  Arena& a = work.arena;
  *value = nullptr;
  if (diagnostic) *diagnostic = true;
  if (read.Failed(facet)) return false;
  if (read.tron) {
    const chains::tron_token::Snapshot* s = read.present ? &read.snapshot : nullptr;
    switch (facet) {
      case kChainMeta: *value = chains::tron_token::Metadata(a, s); break;
      case kInsight: *value = chains::tron_token::Insight(a, s); break;
      case kHolders: *value = chains::tron_token::Holders(a, s); break;
      default: break;
    }
    return *value != nullptr;
  }
  if (facet == kMarket && read.family == Family::kBittensor) {
    *value = BittensorMarket(work, in.seed, read.Page(kIndexPart),
                             read.PartOk(kTaoPricePart) ? read.Page(kTaoPricePart) : nullptr,
                             revalidation || work.background);
    return *value != nullptr;
  }
  switch (facet) {
    case kHolders: *value = Holders(a, read.family, read.Page(FacetPlanner::DecisivePart(facet, *in.network, *in.ref))); break;
    case kChainMeta: *value = Metadata(a, read.family, read.Page(kIndexPart)); break;
    case kInsight:
      *value = Insight(a, read.family, *in.ref, read.Page(kIndexPart));
      // A failed SaucerSwap listing read is logged and dropped: Mirror's
      // sections stay.
      if (*value && MakeFacetPlanner(*work.env, in).SaucerSwapSupported() && read.PartOk(kSaucerTokenPart) &&
          !saucerswap::AppendInsight(a, read.Page(kSaucerTokenPart), *value))
        *value = nullptr;
      break;
    case kMarket:
      // recentPrices(...).catch(() => []).
      *value = saucerswap::MarketRow(a, read.Page(kSaucerTokenPart),
                                     read.PartOk(kSaucerPricesPart) ? read.Page(kSaucerPricesPart) : nullptr);
      break;
    case kProfile: *value = saucerswap::Profile(a, read.Page(kSaucerTokenPart), work.env->kv, work.Now()); break;
    case kLiquidity: {
      Value* pools = nullptr;
      const saucerswap::PoolsStatus status = saucerswap::Pools(a, read.Page(kSaucerPoolsPart), in.ref->address, &pools);
      if (status != saucerswap::PoolsStatus::kOk) {
        if (diagnostic) *diagnostic = status == saucerswap::PoolsStatus::kFailed;
        return false;
      }
      return TokenFacetFetcher(work, in).LiquiditySuccess( pools, value);
    }
    default: break;
  }
  return *value != nullptr;
}

// Runs every wanted sub-facet: cache reads, one provider batch for the
// misses, interpretation, stores, floors and stats. Errors are recorded in
// facet order (see the report: the TypeScript order follows promise
// resolution, which is timing-dependent).
void run_subfacets(Work& work, const FacetInputs& in, SubFacet* subs, size_t count, Value* errors) {
  Arena& a = work.arena;
  token_swr::Cache* swr = work.env->swr;
  // Room for the shared read's provider stage after the requests.
  auto fetches_storage = providers::ConstructArenaFetches(static_cast<providers::TokenFetch*>(a.Alloc(sizeof(providers::TokenFetch) * (count + kSharedStageSlots))), (count + kSharedStageSlots));
  auto* fetches = fetches_storage.get();
  size_t fetch_count = 0;
  uint8_t snapshot_parts = 0;  // the shared read's parts this call reads
  bool failed[kFacetCount] = {};
  for (size_t i = 0; i < count; ++i) {
    SubFacet& sub = subs[i];
    if (!sub.wanted) {
      sub.result = FacetResult{};
      sub.result.source = Source::kSkipped;
      continue;
    }
    sub.started = providers::TokenMonotonicMs();
    const int64_t now = work.Now();
    Value* cached = nullptr;
    int64_t as_of = 0;
    const token_swr::State state =
        swr->Read(sub.key, now, work.scratch, work.scratch_capacity, a, &cached, &as_of);
    if (state != token_swr::State::kMissing) {
      sub.result.value = is_null(cached) ? nullptr : cached;
      sub.result.source = Source::kCache;
      sub.result.has_as_of = true;
      sub.result.as_of = as_of;
      sub.result.cache = Provenance{true, true, as_of, false};
      sub.stale = state == token_swr::State::kStale;
      // A stale snapshot-backed entry waits: if this call reads a snapshot
      // anyway, its revalidation joins that read (the driver's flight).
      if (sub.stale && MakeFacetPlanner(*work.env, in).SnapshotBacked(sub.facet, in.coin_id != nullptr)) sub.revalidate = true;
      else if (sub.stale) enqueue_facet_revalidation(work, sub, in);
      continue;
    }
    if (swr->MissHeld(sub.key, now)) {
      floor_or_fail(work, sub, in, &failed[i]);
      continue;
    }
    if (!fetches) { failed[i] = true; sub.result.source = Source::kFailed; continue; }
    providers::TokenFetch* fetch = &fetches[fetch_count];
    *fetch = providers::TokenFetch{};
    Value* immediate = nullptr;
    bool lane_busy = false;
    const FacetFetchPlan plan = TokenFacetFetcher(work, in).Prepare(sub.facet, fetch, &immediate, &lane_busy);
    if (plan == FacetFetchPlan::kRequest) {
      sub.request = true;
      sub.fetch = fetch;
      ++fetch_count;
      continue;
    }
    if (plan == FacetFetchPlan::kSnapshot) {
      sub.snapshot = true;
      snapshot_parts |= MakeFacetPlanner(*work.env, in).SharedParts(sub.facet, in.coin_id != nullptr);
      continue;
    }
    if (plan == FacetFetchPlan::kFailed) {
      TokenFacetFetcher(work, in).RecordFailure(sub.facet);
      swr->HoldMiss(sub.key, work.Now(), lane_busy);
      floor_or_fail(work, sub, in, &failed[i]);
      continue;
    }
    // Stored after the provider batch, with the requested answers: a
    // concurrent request for the same token that reads meanwhile misses and
    // composes its own live answer, as the TypeScript SWR single flight
    // hands every concurrent reader the one live read.
    sub.immediate = immediate;
  }
  // Stale snapshot-backed entries revalidate from this call's read when it
  // has their part, else through the queue (where siblings share one read).
  // Hedera reads (Mirror and SaucerSwap parts) follow TypeScript's lane
  // admission instead: when this call reads any part, every stale entry's
  // revalidation joins it as background work (LanePlan); with no foreground
  // part they all go through the queue.
  // Bittensor's revalidations join a read of their pool part, adding their
  // other parts at background priority (LanePlan::Background).
  LanePlan lane_plan;
  lane_plan.Foreground(snapshot_parts);
  const token_ledger::Family family = token_ledger::FamilyOf(*in.network, *in.ref);
  const bool hedera = family == token_ledger::Family::kHedera;
  for (size_t i = 0; i < count; ++i) {
    SubFacet& sub = subs[i];
    if (!sub.revalidate) continue;
    const uint8_t parts = MakeFacetPlanner(*work.env, in).SharedParts(sub.facet, in.coin_id != nullptr);
    if (!fetches || (hedera ? !snapshot_parts : !(snapshot_parts & parts))) {
      enqueue_facet_revalidation(work, sub, in);
      continue;
    }
    sub.inline_revalidation = swr->BeginRevalidate(sub.key, work.Now());
    if (sub.inline_revalidation && hedera) lane_plan.Revalidate(parts);
    if (sub.inline_revalidation && family == token_ledger::Family::kBittensor) lane_plan.Background(parts);
  }
  // The shared read's provider stages run with the requests (one read
  // shared by every snapshot-backed sub-facet, and by concurrent readers
  // through the Tron flight or the owner's in-flight GET).
  SharedRead shared;
  if (snapshot_parts && fetches)
    read_shared(work, *in.network, in.seed, *in.ref, lane_plan, fetches, fetch_count, &shared);
  else if (fetch_count) work.env->executor->FetchAll(fetches, fetch_count, work.deadline_ms);
  for (size_t i = 0; i < count; ++i) {
    SubFacet& sub = subs[i];
    if (!sub.inline_revalidation) continue;
    Value* value = nullptr;
    bool diagnostic = true;
    const bool revalidated = shared_value(work, sub.facet, in, shared, &value, &diagnostic, true);
    if (revalidated) {
      int64_t as_of = work.Now();
      swr->Write(sub.key, facet_ttl(sub.facet), as_of, value, work.scratch, work.scratch_capacity, &as_of);
      if (in.persisted)
        TokenFactWriter(work).PersistRevalidated( sub.facet, *in.network, in.seed, in.ref->token_id, in.coin_id,
                            is_null(value) ? nullptr : value, as_of);
    } else if (diagnostic) {
      TokenFacetFetcher(work, in).RecordFailure(sub.facet);
    }
    swr->EndRevalidate(sub.key, work.Now(), !revalidated, !revalidated && shared.LaneBusy(sub.facet));
  }
  for (size_t i = 0; i < count; ++i) {
    SubFacet& sub = subs[i];
    if (!sub.immediate) continue;
    const int64_t now = work.Now();
    int64_t stored_at = now;
    swr->Write(sub.key, facet_ttl(sub.facet), now, sub.immediate, work.scratch, work.scratch_capacity,
               &stored_at);
    sub.result.value = is_null(sub.immediate) ? nullptr : sub.immediate;
    sub.result.source = Source::kLive;
    sub.result.has_as_of = true;
    sub.result.as_of = stored_at;
    sub.result.cache = Provenance{false, true, stored_at, false};
  }
  for (size_t i = 0; i < count; ++i) {
    SubFacet& sub = subs[i];
    if (!sub.request && !sub.snapshot) continue;
    Value* value = nullptr;
    bool ok = false;
    bool lane_busy = false;
    bool diagnostic = true;
    if (sub.snapshot) {
      ok = shared_value(work, sub.facet, in, shared, &value, &diagnostic, false);
      lane_busy = !ok && shared.LaneBusy(sub.facet);
    } else {
      ok = TokenFacetFetcher(work, in).Interpret(sub.facet, *sub.fetch, &value);
      lane_busy = sub.fetch->lane_rejected;
      TokenFetchRelease(sub.fetch);
    }
    const int64_t now = work.Now();
    if (!ok) {
      if (diagnostic) TokenFacetFetcher(work, in).RecordFailure(sub.facet);
      swr->HoldMiss(sub.key, now, lane_busy);
      floor_or_fail(work, sub, in, &failed[i]);
      continue;
    }
    int64_t stored_at = now;
    swr->Write(sub.key, facet_ttl(sub.facet), now, value ? value : tj::Null(a), work.scratch,
               work.scratch_capacity, &stored_at);
    sub.result.value = is_null(value) ? nullptr : value;
    sub.result.source = Source::kLive;
    sub.result.has_as_of = true;
    sub.result.as_of = stored_at;
    sub.result.cache = Provenance{false, true, stored_at, false};
  }
  for (size_t i = 0; i < count; ++i) {
    SubFacet& sub = subs[i];
    if (!sub.wanted) continue;
    record_stats(work, sub, in, failed[i]);
    if (failed[i] || (sub.result.source == Source::kFloor && !sub.result.value))
      tj::Set(a, errors, FacetName(sub.facet), tj::Str(a, kUnavailable));
  }
  // One failed Robinhood catalog read fails chainMeta, market and profile
  // together. chainMeta awaits assetByAddress directly while marketData and
  // robinhoodProfile settle a microtask later: TypeScript records chainMeta
  // first, then market, then profile. (A held miss replays facet order.)
  if (in.robinhood && (in.robinhood->catalog_failed & FacetBit(kChainMeta)) && tj::IsObject(errors)) {
    uint32_t first = errors->count, chain_meta = errors->count;
    for (uint32_t m = 0; m < errors->count; ++m) {
      const tj::Member& member = errors->members[m];
      const bool market = member.key_length == 6 && memcmp(member.key, "market", 6) == 0 &&
                          (in.robinhood->catalog_failed & FacetBit(kMarket));
      const bool profile = member.key_length == 7 && memcmp(member.key, "profile", 7) == 0 &&
                           (in.robinhood->catalog_failed & FacetBit(kProfile));
      if ((market || profile) && first == errors->count) first = m;
      if (member.key_length == 9 && memcmp(member.key, "chainMeta", 9) == 0) chain_meta = m;
    }
    if (first < chain_meta && chain_meta < errors->count) {
      const tj::Member moved = errors->members[chain_meta];
      for (uint32_t k = chain_meta; k > first; --k) errors->members[k] = errors->members[k - 1];
      errors->members[first] = moved;
    }
  }
  // One failed Bittensor pool read fails market, chainMeta and insight
  // together. tokenMetadata and tokenInsight reject with the pool read, while
  // tokenMarket's Promise.all([pool, price]) and marketData's await settle
  // later: TypeScript records the live market error last. (A held miss
  // replays them in facet order.)
  const bool market_errored = count == kFacetCount && subs[kMarket].snapshot &&
      (failed[kMarket] || (subs[kMarket].result.source == Source::kFloor && !subs[kMarket].result.value));
  if (family == token_ledger::Family::kBittensor && market_errored && tj::IsObject(errors)) {
    for (uint32_t m = 0; m + 1 < errors->count; ++m) {
      const tj::Member& member = errors->members[m];
      if (member.key_length != 6 || memcmp(member.key, "market", 6) != 0) continue;
      const tj::Member moved = member;
      for (uint32_t k = m + 1; k < errors->count; ++k) errors->members[k - 1] = errors->members[k];
      errors->members[errors->count - 1] = moved;
      break;
    }
  }
  // One failed SaucerSwap token read fails market and profile together. In
  // TypeScript hederaProfile awaits token() directly while marketData awaits
  // Promise.all([token, prices]), one microtask later: the profile error is
  // recorded first. (A held miss replays both in facet order.)
  bool market_live = false, profile_live = false;
  for (size_t i = 0; i < count; ++i) {
    const bool errored = failed[i] || (subs[i].result.source == Source::kFloor && !subs[i].result.value);
    if (!subs[i].snapshot || !errored) continue;
    if (subs[i].facet == kMarket) market_live = true;
    if (subs[i].facet == kProfile) profile_live = true;
  }
  if (market_live && profile_live && shared.Failed(kMarket) && tj::IsObject(errors)) {
    uint32_t market = errors->count, profile = errors->count;
    for (uint32_t m = 0; m < errors->count; ++m) {
      const tj::Member& member = errors->members[m];
      if (member.key_length == 6 && memcmp(member.key, "market", 6) == 0) market = m;
      if (member.key_length == 7 && memcmp(member.key, "profile", 7) == 0) profile = m;
    }
    if (market < profile && profile < errors->count) {
      const tj::Member swap = errors->members[market];
      errors->members[market] = errors->members[profile];
      errors->members[profile] = swap;
    }
  }
}

}  // namespace internal

}  // namespace fortis::tokens::token_facts
