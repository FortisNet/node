#pragma once

// Internal to the Token 360 facts units (token_facts.cpp, token_facts_plan.cpp,
// token_facts_subfacets.cpp, token_facts_resolution.cpp,
// token_facts_revalidation.cpp): the types and helpers they share. Nothing
// outside src/tokens/token_facts*.cpp includes this.
#include "token_facts.h"
#include "token_facet_planner.h"
#include "token_facet_fetcher.h"
#include "token_fact_writer.h"

#include "provider_runtime_config.h"
#include "kv_store.h"
#include "price_id_parser.h"
#include "token_bittensor.h"
#include "token_comparison_store.h"
#include "token_contract.h"
#include "token_data.h"
#include "token_evm.h"
#include "token_fetchers.h"
#include "token_history.h"
#include "token_jobs.h"
#include "token_ledger.h"
#include "token_liquidity.h"
#include "token_provider.h"
#include "token_robinhood.h"
#include "token_saucerswap.h"
#include "token_swr.h"
#include "token_text.h"
#include "token_tron.h"
#include "token_views.h"
#include "token_writes.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

namespace fortis::tokens::token_facts {

namespace internal {

constexpr const char* kUnavailable = "UPSTREAM_UNAVAILABLE";
constexpr size_t kKeyBytes = 1100;
inline bool is(const char* a, const char* b) { return a && b && strcmp(a, b) == 0; }
inline bool is_null(const Value* v) { return !v || v->type == codec::tj::Type::kNull; }

const networks::NativeNetworkConfig* seed_for(const char* id);
const char* cache_facet_name(const char* facet, const networks::TokenNetwork* network);
bool fact_key(const char* network, const char* scope, const char* facet_name, char* out, size_t capacity);
const token_swr::Ttl& facet_ttl(Facet facet);
bool is_fetch_facet(Facet facet);
Value* as_holder_count(Arena& a, const Value* count, const char* source);
bool is_holder_observation(const Value* v);

// ============================================================================
// Sub-facet machinery (Token360Facet.subFacet over SwrCache.read).
// ============================================================================

// The inputs a sub-facet fetcher closes over.
struct FacetInputs {
  const networks::TokenNetwork* network = nullptr;
  const networks::NativeNetworkConfig* seed = nullptr;
  const networks::TokenRef* ref = nullptr;
  const char* coin_id = nullptr;  // null: none
  const char* scope = nullptr;
  bool persisted = false;
  // This call's issuer reads (a Robinhood contract), shared by its
  // sub-facets.
  token_robinhood::Memo* robinhood = nullptr;
};

inline FacetPlanner MakeFacetPlanner(const Env& env, const networks::TokenNetwork& network,
                                     const networks::TokenRef& ref,
                                     const networks::NativeNetworkConfig* seed = nullptr) {
  return FacetPlanner(network, ref, seed, {env.config, env.etherscan_unsupported, env.saucerswap});
}
inline FacetPlanner MakeFacetPlanner(const Env& env, const FacetInputs& in) {
  return MakeFacetPlanner(env, *in.network, *in.ref, in.seed);
}

// What a fetcher does before (or instead of) a provider request. kSnapshot:
// one driver read shared by several sub-facets answers it (the Tron token
// snapshot, token_tron::Read, or the Stellar/Cardano asset-index page, the
// Algorand/XRPL issuer reads, the Hedera Mirror token reads and the Sui
// coinMetadata read, token_ledger::Read).


// Provider slots the shared read may use after the caller's requests.
constexpr size_t kSharedStageSlots =
    token_tron::kStageSlots > token_ledger::kStageSlots ? token_tron::kStageSlots : token_ledger::kStageSlots;

// How one Hedera read admits the revalidations of stale sub-facets.
// TypeScript starts them together, at background priority, after the
// foreground reads of the same call: a part already read is shared (the
// driver's and SaucerSwapClient's single flights); otherwise the first part
// to reach an idle lane is sent and every later one on that lane is refused
// BACKGROUND_BUSY (LaneQueue admits a background call only on a lane with
// nothing in flight). So a background market refresh always loses its
// recent-prices read behind its own token read.
struct LanePlan {
  uint8_t read = 0;        // parts with a request, foreground or background
  uint8_t background = 0;  // parts sent at background priority
  uint8_t refused = 0;     // parts answered BACKGROUND_BUSY without a request
  uint8_t lanes = 0;       // token_ledger::PartLane bits with a request

  void Foreground(uint8_t parts) {
    read |= parts;
    for (uint8_t part : token_ledger::kPartOrder)
      if (parts & part) lanes |= token_ledger::PartLane(part);
  }
  void Revalidate(uint8_t parts) {
    for (uint8_t part : token_ledger::kPartOrder) {
      if (!(parts & part) || (read & part) || (refused & part)) continue;
      if (lanes & token_ledger::PartLane(part)) {
        refused |= part;
        continue;
      }
      read |= part;
      background |= part;
      lanes |= token_ledger::PartLane(part);
    }
  }
  // Bittensor: the parts not read yet join at background priority; the read
  // itself decides admission, after the driver's working set answered what
  // it holds (a held part makes no lane call).
  void Background(uint8_t parts) {
    background |= static_cast<uint8_t>(parts & ~read);
    read |= parts;
  }
};

// One driver read several sub-facets project: the Tron token snapshot, the
// Stellar/Cardano asset-index page, the Algorand/XRPL issuer read, the Sui
// coinMetadata read, or the Hedera Mirror token-info and balances parts and
// the SaucerSwap token, recent-prices and pools parts.
struct SharedRead {
  bool tron = false;
  bool tron_failed = true;
  bool present = false;  // Tron: a snapshot (false: the null snapshot)
  chains::tron_token::Snapshot snapshot;
  token_ledger::Family family = token_ledger::Family::kNone;
  const networks::TokenNetwork* network = nullptr;
  const networks::TokenRef* ref = nullptr;
  token_ledger::Pages pages;  // per part: page, failed, lane_busy

  size_t Slot(Facet facet) const { return token_ledger::PartSlot(FacetPlanner::DecisivePart(facet, *network, *ref)); }
  bool Failed(Facet facet) const { return tron ? tron_failed : !network || pages.failed[Slot(facet)]; }
  // The failed read's last endpoint was refused by its lane.
  bool LaneBusy(Facet facet) const { return !tron && network && pages.lane_busy[Slot(facet)]; }
  bool PartOk(uint8_t part) const { return !tron && !pages.failed[token_ledger::PartSlot(part)]; }
  const Value* Page(uint8_t part) const { return pages.page[token_ledger::PartSlot(part)]; }
};

void read_shared(Work& work, const networks::TokenNetwork& network, const networks::NativeNetworkConfig* seed, const networks::TokenRef& ref,
                 const LanePlan& plan, providers::TokenFetch* batch, size_t extra, SharedRead* out);
bool shared_value(Work& work, Facet facet, const FacetInputs& in, const SharedRead& read, Value** value,
                  bool* diagnostic, bool revalidation);

struct SubFacet {
  Facet facet = kMarket;
  bool wanted = false;
  char key[kKeyBytes]{};
  FacetResult result;
  bool request = false;   // waiting on `fetch`
  bool snapshot = false;  // answered by the shared driver read
  bool revalidate = false;  // stale and snapshot-backed: revalidation pending
  bool inline_revalidation = false;  // revalidated from this call's snapshot
  bool held_miss = false;
  providers::TokenFetch* fetch = nullptr;
  uint64_t started = 0;
  bool stale = false;
  Value* immediate = nullptr;  // an answer without a request, stored with the batch
};

void run_subfacets(Work& work, const FacetInputs& in, SubFacet* subs, size_t count, Value* errors);
bool ResolveTokenIdentityLive(Work& work, const networks::TokenNetwork& network, const networks::TokenRef& ref, Value** out);
void ResolveTokenIdentity(Work& work, const networks::TokenNetwork& network, const networks::TokenRef& ref, const char* hint,
                     Resolution* resolution);

// ============================================================================
// Durable write-through (writeThrough / durableFactChange).
// ============================================================================

}  // namespace internal
using namespace internal;

}  // namespace fortis::tokens::token_facts
