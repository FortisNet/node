#pragma once

#include "network_registry.h"
#include "robinhood_markets.h"
#include "token_facts.h"
#include "token_ref.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::storage { class KvStore; }

// Robinhood Chain stock tokens on the token routes: Token360Facet's
// `robinhood` branches (the TypeScript chainMeta, robinhoodProfile and
// marketData) over the token canary's RobinhoodStockTokenClient
// (exchange::robinhood::StockClient, driven through the token provider owner):
//  - chainMeta: a listed contract is the issuer's registration — the
//    sanitized catalog symbol and name, decimals 18, `verified: true` (the
//    issuer registered it), no holder count and the issuer logo registered
//    as media; an unlisted contract falls through to the EVM driver's
//    contract read;
//  - profile: the tokenized-debt description with the Stock Token docs link
//    and the same logo, else CoinGecko (a resolved coin id) or null;
//  - market: the reference quote midpoint (`current_price`; volume and the
//    24 h range unknown), else — a null quote or an unlisted contract —
//    CoinGecko for a resolved coin id, else null.
// The catalog lookup is assetByAddress: the first row of the symbol-sorted
// catalog whose contract equals the ref's address case-insensitively. A
// catalog or quote failure fails the facets that read it (nothing is cached
// as an answer). Guest-safe.
namespace fortis::tokens::token_robinhood {

using codec::tj::Arena;
using codec::tj::Value;

// `network.id === 'robinhood' && ref.address !== null`: the refs Token360
// asks the issuer catalog about.
bool IssuerRef(const networks::TokenNetwork& network, const networks::TokenRef& ref);

// The StockClient fetcher over the token provider owner: one
// TokenUpstream::kRobinhood GET at `background` lane priority. A 2xx whose
// body is not JSON is retried — three requests in all, as HttpClient.json
// retries a parse failure; any other non-2xx, transport failure or
// cancellation is kError and a lane refusal kLaneBusy.
class OwnerFetcher final : public exchange::robinhood::StockClient::Fetcher {
 public:
  explicit OwnerFetcher(providers::TokenProviderOwner* owner) : owner_(owner) {}
  exchange::robinhood::Outcome Fetch(Arena& arena, uint64_t deadline_ms, const char* path, bool background,
                           Value** body) override;

 private:
  providers::TokenProviderOwner* owner_;
};

// One facts() call's (or one revalidation's) issuer reads. TypeScript's
// sub-facets of one call share the client's in-flight catalog and quote
// promises, so they see one outcome: the first sub-facet reads, the others
// reuse its answer (a failed catalog is not read again in the same call).
struct Memo {
  bool catalog_read = false;
  exchange::robinhood::Outcome catalog;
  Value* asset = nullptr;  // the listed asset (a work-arena copy); nullptr: unlisted
  bool quote_read = false;
  exchange::robinhood::Outcome quote_outcome;
  Value* quote = nullptr;  // nullptr: the null quote
  // FacetBit set of the sub-facets a failed catalog read of this call
  // failed (TypeScript records them chainMeta, market, profile).
  uint32_t catalog_failed = 0;
};

enum class Listing : uint8_t { kListed, kUnlisted, kFailed };
// assetByAddress(ref.address) at the work's lane priority. *lane_busy is
// set when the failure is a lane refusal.
Listing Asset(token_facts::Work& work, exchange::robinhood::StockClient* client, Memo* memo, const networks::TokenRef& ref,
              bool* lane_busy);
// referenceQuote(memo->asset) for a listed asset: false on failure (then
// *lane_busy as above), else *quote (nullptr for the null quote).
bool Quote(token_facts::Work& work, exchange::robinhood::StockClient* client, Memo* memo, Value** quote, bool* lane_busy);

// The projections; nullptr on arena exhaustion.
Value* ChainMeta(Arena& arena, const Value* asset, storage::KvStore* kv, int64_t now_ms);
Value* MarketRow(Arena& arena, const Value* asset, const Value* quote);
Value* Profile(Arena& arena, const Value* asset, storage::KvStore* kv, int64_t now_ms);

}  // namespace fortis::tokens::token_robinhood
