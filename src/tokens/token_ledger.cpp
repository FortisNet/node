#include "token_ledger.h"

#include "chain_asset_index.h"
#include "token_bittensor.h"
#include "token_fetchers.h"
#include "token_saucerswap.h"
#include "token_views.h"

#include <stdio.h>
#include <string.h>

#include <new>

namespace tj = fortis::codec::tj;

namespace fortis::tokens::token_ledger {
namespace {

// HttpClient.json: one try and two retries per endpoint (GET).
constexpr int kAttemptsPerEndpoint = 3;
// The longest XRPL currency the ref grammar yields (a 40-hex code).
constexpr size_t kCurrencyBytes = 64;
constexpr size_t kIssuerBytes = 40;

bool is(const char* a, const char* b) { return a && b && strcmp(a, b) == 0; }

// The GET path of a Stellar, Cardano or Algorand ref.
bool index_path(Family family, const networks::TokenRef& ref, char* path, size_t capacity) {
  if (family == Family::kStellar)
    return chains::asset_index::StellarAssetPath(ref.address, ref.has_symbol ? ref.symbol : nullptr, path, capacity);
  if (family == Family::kCardano) return chains::asset_index::CardanoSummaryPath(ref.address, path, capacity);
  if (family == Family::kAlgorand) return chains::asset_index::AlgorandAssetPath(ref.address, path, capacity);
  return false;
}

bool xrpl_token(const networks::TokenRef& ref, char currency[kCurrencyBytes], char issuer[kIssuerBytes]) {
  return chains::asset_index::XrplIssuedToken(ref.address, ref.has_symbol ? ref.symbol : nullptr, currency,
                                      kCurrencyBytes, issuer, kIssuerBytes);
}

providers::TokenUpstream get_upstream(Family family) {
  return family == Family::kStellar ? providers::TokenUpstream::kHorizon
       : family == Family::kCardano ? providers::TokenUpstream::kKoios
       : family == Family::kHedera  ? providers::TokenUpstream::kHederaMirror
                                    : providers::TokenUpstream::kAlgod;
}

// assetPage / assetSummary / the algod asset GET / the Mirror token GETs /
// the SaucerSwap GETs / the Sui GraphQL POST (HttpClient retries it like a
// GET): `count` independent requests at batch[extra..], all in
// the caller's first provider batch. An unparseable 2xx is retried on its
// endpoint (HttpClient's three attempts), then a network GET fails over;
// a SaucerSwap 404 is getOrNull's JSON null page. Each stage's page lands
// in out->page[slots[i]].
void read_gets(token_facts::Work& work, providers::TokenFetch* batch, size_t extra, size_t count, const size_t* slots,
               Pages* out) {
  providers::TokenFetch* stages = batch + extra;
  work.env->executor->FetchAll(batch, extra + count, work.deadline_ms);
  int attempts[kStageSlots];
  bool open[kStageSlots];
  for (size_t i = 0; i < kStageSlots; ++i) { attempts[i] = 1; open[i] = i < count; }
  for (;;) {
    providers::TokenFetch* again[kStageSlots];
    size_t again_count = 0;
    for (size_t i = 0; i < count; ++i) {
      if (!open[i]) continue;
      providers::TokenFetch& fetch = stages[i];
      const size_t slot = slots[i];
      bool retry = false;
      const bool saucer =
          fetch.upstream == providers::TokenUpstream::kSaucerSwap || fetch.upstream == providers::TokenUpstream::kSaucerSwapTestnet;
      if (saucer && !fetch.cancelled && fetch.answered && fetch.status == 404) {
        out->page[slot] = tj::Null(work.arena);
        out->failed[slot] = out->page[slot] == nullptr;
      } else if (!fetch.cancelled && TokenFetchOk(fetch)) {
        tj::Value* body = fetch.body_length ? tj::Parse(work.arena, fetch.body.get(), fetch.body_length) : nullptr;
        if (body) {
          out->page[slot] = body;
          out->failed[slot] = false;
        } else if (work.arena.ok) {
          size_t next = fetch.next_url;
          if (attempts[i] < kAttemptsPerEndpoint) {
            ++attempts[i];
            retry = true;
          } else if (next + 1 < fetch.rpc_url_count) {
            ++next;
            attempts[i] = 1;
            retry = true;
          }
          fetch.first_url = next;
        }
      }
      if (retry) {
        TokenFetchRelease(&fetch);
        again[again_count++] = &fetch;
        continue;
      }
      open[i] = false;
      if (out->failed[slot]) out->lane_busy[slot] = fetch.lane_rejected;
      TokenFetchRelease(&fetch);
    }
    if (!again_count) return;
    // Re-issue together when every stage retries (they are adjacent).
    if (again_count == count) work.env->executor->FetchAll(stages, count, work.deadline_ms);
    else for (size_t i = 0; i < again_count; ++i) work.env->executor->FetchAll(again[i], 1, work.deadline_ms);
  }
}

// `!!value` for a JSON value (nullptr is undefined).
bool truthy(const tj::Value* v) {
  if (!v) return false;
  switch (v->type) {
    case tj::Type::kNull: case tj::Type::kFalse: return false;
    case tj::Type::kTrue: case tj::Type::kArray: case tj::Type::kObject: return true;
    case tj::Type::kNumber: return v->number == v->number && v->number != 0;
    case tj::Type::kString: return v->length != 0;
  }
  return false;
}

// One DriverTransport.jsonRpc call after a provider round.
enum class Rpc : uint8_t { kResult, kFailed, kNextEndpoint };

// The call's `result`, a failure, or the next endpoint (an unparseable or
// null body: HttpClient's transport failure, or the TypeError on
// `body.error`, both caught by the failover loop).
Rpc interpret_rpc(token_facts::Work& work, providers::TokenFetch& fetch, const tj::Value** result) {
  if (fetch.cancelled || !TokenFetchOk(fetch)) return Rpc::kFailed;
  tj::Value* body = fetch.body_length ? tj::Parse(work.arena, fetch.body.get(), fetch.body_length) : nullptr;
  TokenFetchRelease(&fetch);
  if (!body || body->type == tj::Type::kNull) {
    if (!work.arena.ok) return Rpc::kFailed;
    return fetch.next_url + 1 < fetch.rpc_url_count ? Rpc::kNextEndpoint : Rpc::kFailed;
  }
  // A truthy `body.error`, then `body.result === undefined`: answers
  // (UpstreamAnswerError), never failed over.
  if (truthy(tj::Get(body, "error"))) return Rpc::kFailed;
  const tj::Value* value = tj::Get(body, "result");
  if (!value) return Rpc::kFailed;
  *result = value;
  return Rpc::kResult;
}

// XrplDriver.tokenInsight's Promise.all of gateway_balances and account_info:
// both calls in one provider batch, each failed over on its own.
void read_xrpl(token_facts::Work& work, providers::TokenFetch* batch, size_t extra, Pages* out) {
  providers::TokenFetch* stages = batch + extra;
  work.env->executor->FetchAll(batch, extra + 2, work.deadline_ms);
  const tj::Value* results[2] = {nullptr, nullptr};
  Rpc state[2] = {Rpc::kFailed, Rpc::kFailed};
  bool open[2] = {true, true};
  for (;;) {
    bool again[2] = {false, false};
    for (size_t i = 0; i < 2; ++i) {
      if (!open[i]) continue;
      state[i] = interpret_rpc(work, stages[i], &results[i]);
      if (state[i] != Rpc::kNextEndpoint) { open[i] = false; continue; }
      again[i] = true;
      stages[i].first_url = stages[i].next_url + 1;
    }
    if (!again[0] && !again[1]) break;
    if (again[0] && again[1]) work.env->executor->FetchAll(stages, 2, work.deadline_ms);
    else work.env->executor->FetchAll(&stages[again[0] ? 0 : 1], 1, work.deadline_ms);
  }
  for (size_t i = 0; i < 2; ++i) TokenFetchRelease(&stages[i]);
  if (state[0] == Rpc::kResult && state[1] == Rpc::kResult) {
    tj::Value* pair = tj::Arr(work.arena);
    if (!pair || !tj::Push(work.arena, pair, const_cast<tj::Value*>(results[0])) ||
        !tj::Push(work.arena, pair, const_cast<tj::Value*>(results[1])))
      return;
    out->page[0] = pair;
    out->failed[0] = false;
    return;
  }
  // Promise.all rejects with the first rejection, and a refused lane rejects
  // at once: a failed call whose last endpoint was refused decides it.
  for (size_t i = 0; i < 2; ++i)
    if (state[i] == Rpc::kFailed && !stages[i].answered && stages[i].lane_rejected) out->lane_busy[0] = true;
}

// The Bittensor driver's pool and TAO/USD reads (subnetPool, taoPriceUsd):
// the working set answers a part it holds; the others are GETs on the
// 'taostats' lane in kPartOrder, a background part refused while an
// earlier part of this read is on the lane (LaneQueue's BACKGROUND_BUSY).
// Pages are projected (token_bittensor::PoolPage / TaoPricePage) and kept
// in the working set.
void read_bittensor(token_facts::Work& work, const networks::TokenNetwork& network, const networks::NativeNetworkConfig* seed,
                    const networks::TokenRef& ref, uint8_t parts, providers::TokenFetch* batch, size_t extra, Pages* out,
                    uint8_t background) {
  static constexpr uint8_t kOrder[] = {kIndexPart, kTaoPricePart};
  providers::TokenFetch* stage = batch + extra;
  token_bittensor::WorkingSet* set = work.env->bittensor;
  uint32_t netuid = 0;
  const bool endpoints = seed && seed->rpc_url_count;
  size_t slots[kStageSlots];
  uint8_t sent[kStageSlots];
  size_t count = 0;
  bool on_lane = false;
  if (token_bittensor::SubnetNetuid(ref.address, &netuid)) {
    for (uint8_t part : kOrder) {
      if (!(parts & part)) continue;
      const size_t slot = PartSlot(part);
      tj::Value* held = nullptr;
      const bool hit = set && (part == kIndexPart ? set->Pool(netuid, work.Now(), work.arena, &held)
                                                  : set->Price(work.Now(), work.arena, &held));
      if (hit) {
        out->page[slot] = held;
        out->failed[slot] = false;
        continue;
      }
      // DriverTransport: RangeError('no endpoints configured') is thrown.
      if (!endpoints) continue;
      if ((background & part) && on_lane) {
        out->lane_busy[slot] = true;
        continue;
      }
      providers::TokenFetch* fetch = stage + count;
    *fetch = providers::TokenFetch{};
      fetch->upstream = providers::TokenUpstream::kTaostats;
      fetch->background = work.background || (background & part);
      fetch->network = network.id;
      fetch->rpc_urls = seed->rpc_urls;
      fetch->rpc_url_count = seed->rpc_url_count;
      const bool built = part == kIndexPart
          ? token_bittensor::PoolPath(netuid, fetch->path, sizeof(fetch->path))
          : snprintf(fetch->path, sizeof(fetch->path), "%s", token_bittensor::kTaoPricePath) > 0;
      if (!built) continue;
      slots[count] = slot;
      sent[count] = part;
      ++count;
      on_lane = true;
    }
  }
  if (!count) {
    if (extra) work.env->executor->FetchAll(batch, extra, work.deadline_ms);
    return;
  }
  read_gets(work, batch, extra, count, slots, out);
  for (size_t i = 0; i < count; ++i) {
    const size_t slot = slots[i];
    if (out->failed[slot]) continue;
    tj::Value* page = sent[i] == kIndexPart ? token_bittensor::PoolPage(work.arena, out->page[slot], netuid)
                                            : token_bittensor::TaoPricePage(work.arena, out->page[slot]);
    out->page[slot] = page;
    if (!page) {
      // The driver throws (a JSON null body or row): nothing is kept.
      out->failed[slot] = true;
      continue;
    }
    if (!set) continue;
    if (sent[i] == kIndexPart) set->RememberPool(netuid, work.Now(), page);
    else set->RememberPrice(work.Now(), page);
  }
}

}  // namespace

Family FamilyOf(const networks::TokenNetwork& network, const networks::TokenRef& ref) {
  if (!ref.has_address) return Family::kNone;
  if (is(network.family, "stellar")) return Family::kStellar;
  if (is(network.family, "cardano")) return Family::kCardano;
  if (is(network.family, "algorand")) return Family::kAlgorand;
  if (is(network.family, "xrpl")) return Family::kXrpl;
  if (is(network.family, "hedera")) return Family::kHedera;
  if (is(network.family, "sui")) return Family::kSui;
  if (is(network.family, "bittensor") && ref.kind == networks::TokenRefKind::kBittensorAlpha) return Family::kBittensor;
  return Family::kNone;
}

bool Indexed(const networks::TokenNetwork& network, const networks::TokenRef& ref) {
  const Family family = FamilyOf(network, ref);
  if (family == Family::kXrpl) {
    char currency[kCurrencyBytes], issuer[kIssuerBytes];
    return xrpl_token(ref, currency, issuer);
  }
  if (family == Family::kSui) {
    // canonicalSuiCoinType(token.address): the ref grammar already
    // canonicalized it, so only its body must build.
    char body[providers::kTokenMaxRpcBodyBytes];
    size_t length = 0;
    return ref.kind == networks::TokenRefKind::kSui && chains::asset_index::SuiCoinMetadataBody(ref.address, body, sizeof(body), &length);
  }
  if (family == Family::kBittensor) {
    uint32_t netuid = 0;
    return token_bittensor::SubnetNetuid(ref.address, &netuid);
  }
  char path[chains::asset_index::kMaxPathBytes];
  if (family == Family::kHedera) return chains::asset_index::HederaTokenPath(ref.address, path, sizeof(path));
  return index_path(family, ref, path, sizeof(path));
}

void Read(token_facts::Work& work, const networks::TokenNetwork& network, const networks::NativeNetworkConfig* seed,
          const networks::TokenRef& ref, uint8_t parts, providers::TokenFetch* batch, size_t extra, Pages* out, uint8_t background,
          uint8_t refused) {
  *out = Pages{};
  const Family family = FamilyOf(network, ref);
  providers::TokenFetch* stage = batch + extra;
  // DriverTransport: RangeError('no endpoints configured') is thrown.
  const bool endpoints = seed && seed->rpc_url_count;
  auto fail = [&] {
    if (extra) work.env->executor->FetchAll(batch, extra, work.deadline_ms);
  };
  if (family == Family::kBittensor)
    return read_bittensor(work, network, seed, ref, parts, batch, extra, out, background);
  if (family == Family::kXrpl) {
    *stage = providers::TokenFetch{};
    stage[1] = providers::TokenFetch{};
    char currency[kCurrencyBytes], issuer[kIssuerBytes];
    const bool ready = endpoints && xrpl_token(ref, currency, issuer) &&
        chains::asset_index::XrplGatewayBalancesBody(issuer, stage[0].rpc_body, sizeof(stage[0].rpc_body),
                                             &stage[0].rpc_body_length) &&
        chains::asset_index::XrplAccountInfoBody(issuer, stage[1].rpc_body, sizeof(stage[1].rpc_body),
                                         &stage[1].rpc_body_length);
    if (!ready) return fail();
    for (size_t i = 0; i < 2; ++i) {
      stage[i].upstream = providers::TokenUpstream::kJsonRpc;
      stage[i].background = work.background;
      stage[i].network = network.id;
      stage[i].rpc_urls = seed->rpc_urls;
      stage[i].rpc_url_count = seed->rpc_url_count;
    }
    read_xrpl(work, batch, extra, out);
    out->page[1] = out->page[0];
    out->failed[1] = out->failed[0];
    out->lane_busy[1] = out->lane_busy[0];
    return;
  }
  // The GETs: one page, or Hedera's Mirror and SaucerSwap parts in
  // kPartOrder, the background ones last. A part whose request cannot be
  // built stays failed (DriverTransport's RangeError for a network without
  // endpoints).
  size_t slots[kStageSlots];
  size_t count = 0;
  auto add = [&](uint8_t part, bool background_part) {
    providers::TokenFetch* fetch = stage + count;
    *fetch = providers::TokenFetch{};
    fetch->background = work.background || background_part;
    bool built = false;
    if (family == Family::kSui) {
      // SuiDriver.coinMetadata: `network.graphqlUrls ?? []`; an empty list
      // is DriverTransport's RangeError('no endpoints configured').
      fetch->upstream = providers::TokenUpstream::kGraphql;
      fetch->network = network.id;
      built = seed && seed->graphql_url_count && ref.kind == networks::TokenRefKind::kSui &&
              chains::asset_index::SuiCoinMetadataBody(ref.address, fetch->rpc_body, sizeof(fetch->rpc_body),
                                               &fetch->rpc_body_length);
      if (built) {
        fetch->rpc_urls = seed->graphql_urls;
        fetch->rpc_url_count = seed->graphql_url_count;
      }
    } else if (part & kSaucerParts) {
      fetch->upstream = seed && seed->testnet ? providers::TokenUpstream::kSaucerSwapTestnet : providers::TokenUpstream::kSaucerSwap;
      built = part == kSaucerTokenPart    ? saucerswap::TokenPath(ref.address, fetch->path, sizeof(fetch->path))
            : part == kSaucerPricesPart ? saucerswap::PricesPath(ref.address, work.Now(), fetch->path,
                                                                  sizeof(fetch->path))
                                        : saucerswap::PoolsPath(ref.address, fetch->path, sizeof(fetch->path));
    } else {
      built = endpoints && (family == Family::kHedera
          ? (part == kHolderPart ? chains::asset_index::HederaBalancesPath(ref.address, fetch->path, sizeof(fetch->path))
                                 : chains::asset_index::HederaTokenPath(ref.address, fetch->path, sizeof(fetch->path)))
          : index_path(family, ref, fetch->path, sizeof(fetch->path)));
      fetch->upstream = get_upstream(family);
      fetch->network = network.id;
      if (endpoints) {
        fetch->rpc_urls = seed->rpc_urls;
        fetch->rpc_url_count = seed->rpc_url_count;
      }
    }
    if (built) slots[count++] = PartSlot(part);
  };
  if (family != Family::kHedera) {
    add(kIndexPart, false);
  } else {
    for (uint8_t part : kPartOrder)
      if ((parts & part) && !(background & part) && !(refused & part)) add(part, false);
    for (uint8_t part : kPartOrder)
      if ((parts & part) && (background & part) && !(refused & part)) add(part, true);
    // TypeScript's LaneQueue refuses a background call while another call
    // of its lane is in flight (BACKGROUND_BUSY).
    for (uint8_t part : kPartOrder)
      if (refused & part) out->lane_busy[PartSlot(part)] = true;
  }
  if (!count) return fail();
  read_gets(work, batch, extra, count, slots, out);
  if (family != Family::kHedera) {
    out->page[1] = out->page[0];
    out->failed[1] = out->failed[0];
    out->lane_busy[1] = out->lane_busy[0];
  }
}

tj::Value* Holders(tj::Arena& arena, Family family, const tj::Value* page) {
  if (family == Family::kStellar) return chains::asset_index::StellarHolders(arena, page);
  if (family == Family::kCardano) return chains::asset_index::CardanoHolders(arena, page);
  if (family == Family::kHedera) return chains::asset_index::HederaHolders(arena, page);
  return nullptr;
}

tj::Value* Metadata(tj::Arena& arena, Family family, const tj::Value* page) {
  if (family == Family::kHedera) return chains::asset_index::HederaMetadata(arena, page);
  if (family == Family::kSui) return chains::asset_index::SuiMetadata(arena, page);
  if (family == Family::kBittensor) return token_bittensor::Metadata(arena, page);
  return nullptr;
}

tj::Value* Insight(tj::Arena& arena, Family family, const networks::TokenRef& ref, const tj::Value* page) {
  if (family == Family::kStellar) return chains::asset_index::StellarInsight(arena, page);
  if (family == Family::kCardano) return chains::asset_index::CardanoInsight(arena, page);
  if (family == Family::kAlgorand) return chains::asset_index::AlgorandInsight(arena, page);
  if (family == Family::kXrpl) {
    char currency[kCurrencyBytes], issuer[kIssuerBytes];
    if (!tj::IsArray(page) || page->count != 2 || !xrpl_token(ref, currency, issuer)) return nullptr;
    return chains::asset_index::XrplInsight(arena, currency, page->items[0], page->items[1]);
  }
  if (family == Family::kHedera) {
    char id[48];
    if (!networks::CanonicalHederaEntityId(ref.address, strlen(ref.address), id, sizeof(id))) return nullptr;
    return chains::asset_index::HederaInsight(arena, id, page);
  }
  if (family == Family::kBittensor) {
    uint32_t netuid = 0;
    if (!token_bittensor::SubnetNetuid(ref.address, &netuid)) return nullptr;
    return token_bittensor::Insight(arena, netuid, page);
  }
  return nullptr;
}

tj::Value* BittensorMarket(token_facts::Work& work, const networks::NativeNetworkConfig* seed, const tj::Value* pool,
                           const tj::Value* price, bool background) {
  tj::Arena& a = work.arena;
  if (!tj::IsObject(pool)) return tj::Null(a);
  bool has_usd = tj::IsNumber(price);
  double usd = has_usd ? price->number : 0;
  const char* native = seed ? seed->native_coingecko_id : nullptr;
  if (!has_usd && native && *native) {
    // `coingecko.coinMarket(nativeCoinId, 'usd').catch(() => null)`, then
    // finiteNumber(row?.current_price).
    auto fetch_storage = providers::ConstructArenaFetches(static_cast<providers::TokenFetch*>(a.Alloc(sizeof(providers::TokenFetch))), 1);
  auto* fetch = fetch_storage.get();
    if (!fetch) return nullptr;
    *fetch = providers::TokenFetch{};
    fetch->background = background;
    const char* ids[] = {native};
    if (token_fetch::CoinMarkets(fetch, ids, 1)) {
      work.env->executor->FetchAll(fetch, 1, work.deadline_ms);
      tj::Value* rows = nullptr;
      if (token_fetch::MarketRows(a, *fetch, &rows) == token_fetch::Outcome::kValue) {
        const tj::Value* row = token_fetch::MarketRowFor(a, rows, native);
        has_usd = token_views::FiniteNumberOrString(tj::Get(row, "current_price"), &usd);
      }
      TokenFetchRelease(fetch);
    }
  }
  return token_bittensor::MarketRow(a, pool, has_usd, usd);
}

}  // namespace fortis::tokens::token_ledger
