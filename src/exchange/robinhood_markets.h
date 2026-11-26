#include "threading.h"
#pragma once

#include "clock.h"
#include "kv_store.h"
#include "token_json.h"
#include "ownership.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

namespace fortis::chains { class Transport; }
namespace fortis::providers { struct RobinhoodAssetCatalog; }

// The Robinhood issuer read model: ports of the TypeScript
// RobinhoodStockTokenClient and RobinhoodMarketsFacet over one
// chains::Transport lease.
//
//  - GET https://api.robinhood.com/rhj/assets on the "robinhood-assets" lane,
//    parsed as parseAssets (a shape failure fails the read), kept 15 minutes;
//  - GET /rhj/prices/{encodeURIComponent(symbol)} per contract, parsed as
//    parseReferenceQuote (null when unusable), kept 15 seconds when usable;
//  - GET /rhj/corporate-actions, parsed as parseCorporateActions (never
//    fails on shape), kept one hour.
// Concurrent reads of one document join a single upstream request, as the
// TypeScript in-flight promises do. Each cache is per process.
//
// The Robinhood routes run the client over a chains::Transport lease; the token
// canary runs its own instance through the token provider owner (a FetchFn)
// and shares that instance's catalog with the price route's spot owner
// (FreshCatalog / AdoptCatalog), as TypeScript shares one client between
// Token360 and PricesFacet.
//
// Native differences: the Robinhood routes' instance keeps caches that are
// separate from the token canary's (one instance per canary), so the two
// canaries may each fetch /rhj/assets; a symbol encodeURIComponent cannot
// encode fails before the request (TypeScript fails inside the lane); the
// catalog symbols are sorted in byte order after ASCII lower-casing, as the
// token search does; provider bodies keep the transport bounds (512 KiB, depth 64). The
// instrument's quote and corporate actions are two calls the caller runs
// concurrently (RobinhoodService runs the actions on a companion thread), as
// TypeScript's Promise.all does.
namespace fortis::exchange::robinhood {

using codec::tj::Arena;
using codec::tj::Value;

enum class Failure : uint8_t { kNone, kError, kLaneBusy };

struct Outcome {
  Failure failure = Failure::kNone;
  uint32_t retry_after_ms = 0;
  bool ok() const { return failure == Failure::kNone; }
};

// parseAssets(body): the RobinhoodStockAsset records sorted by ticker
// (discovery::RobinhoodSymbolCompare), or nullptr for an unusable body.
Value* ParseAssets(Arena& arena, const Value* body);
// parseReferenceQuote(body, asset): the quote object, or nullptr (null).
Value* ParseReferenceQuote(Arena& arena, const Value* body, const Value* asset);
// parseCorporateActions(body): an array (empty for an unusable body).
Value* ParseCorporateActions(Arena& arena, const Value* body);

class StockClient {
 public:
  struct Snapshot;

  StockClient() = default;
  ~StockClient();
  StockClient(const StockClient&) = delete;
  StockClient& operator=(const StockClient&) = delete;

  // GET https://api.robinhood.com{path} on the "robinhood-assets" lane at
  // `background` lane priority: the 2xx body parsed as JSON into `arena`, or
  // the failure (HttpClient.json: any non-2xx or unparseable body fails).
  // The base fetcher has no upstream: every read fails.
  class Fetcher {
   public:
    virtual Outcome Fetch(Arena& arena, uint64_t deadline_ms, const char* path, bool background, Value** body);

   protected:
    constexpr Fetcher() = default;
    ~Fetcher() = default;
  };

  // `clock` is the wall clock of the catalog and quote caches.
  void Configure(chains::Transport* transport, fortis::base::Clock* clock);
  // Requests go through `fetcher` instead of a transport (null: every read
  // fails). Reconfigure only while no thread reads through the client.
  void Configure(Fetcher* fetcher, fortis::base::Clock* clock);

  // The number of /rhj/assets requests finished so far: a read records it
  // when it is issued (see Assets).
  static constexpr uint64_t kNotIssued = ~static_cast<uint64_t>(0);
  uint64_t AssetsGeneration();
  // assets(): a held snapshot of the parsed catalog (Release it). Bounded by
  // the monotonic `deadline_ms`. `background` is the lane priority of a
  // request this read starts (TypeScript takes it from the work reason).
  // `issued_generation` is AssetsGeneration() when the read was issued: a
  // read that waited in a queue while a request finished takes that
  // request's failure instead of starting another, as a TypeScript caller
  // that awaited the in-flight promise would (kNotIssued: no such sharing).
  Outcome Assets(uint64_t deadline_ms, std::shared_ptr<const Snapshot>* out, bool background = false,
                 uint64_t issued_generation = kNotIssued);
  // corporateActions(): a held snapshot of every parsed action.
  // `issued_generation` is ActionsGeneration() when the read was issued (see
  // Assets).
  uint64_t ActionsGeneration();
  Outcome CorporateActions(uint64_t deadline_ms, std::shared_ptr<const Snapshot>* out, uint64_t issued_generation = kNotIssued);
  static const Value* Document(const Snapshot* snapshot);
  // referenceQuote(asset): the quote copied into `arena`, or *quote nullptr.
  // `issued_generation` is QuoteGeneration() when the read was issued: the
  // number of quote requests (any contract) finished so far. A read that
  // waited while a request for its contract finished with a failure takes
  // that failure instead of starting another (kNotIssued: no such sharing).
  uint64_t QuoteGeneration();
  Outcome ReferenceQuote(Arena& arena, uint64_t deadline_ms, const Value* asset, Value** quote,
                         bool background = false, uint64_t issued_generation = kNotIssued);

  // The price route's view of the catalog (spot_provider_owner), flattened
  // to address (lower case), symbol and multiplier in catalog order; a
  // repeated contract keeps its last row (PricesFacet's Map of
  // RobinhoodStockTokenClient.prices), a symbol longer than the flat record
  // holds is skipped. Never blocks on a request:
  //  - FreshCatalog: the unexpired catalog, false when there is none;
  //  - AdoptCatalog: parseAssets over a /rhj/assets body the spot owner
  //    fetched, kept as this client's catalog for 15 minutes; false when the
  //    body fails parseAssets (assets() would throw) or does not flatten.
  bool FreshCatalog(providers::RobinhoodAssetCatalog* out);
  bool AdoptCatalog(const uint8_t* body, size_t length, providers::RobinhoodAssetCatalog* out);

 private:
  struct Flight {
    bool running = false;
    uint64_t generation = 0;
    Outcome result;
  };
  struct QuoteEntry {
    bool used = false;
    char key[43]{};
    std::unique_ptr<char[], base::StorageDeleter> json;
    size_t length = 0;
    int64_t until_ms = 0;
    bool running = false;
    uint64_t generation = 0;
    Outcome result;
    bool has_result = false;
    int waiters = 0;  // joiners still to read this entry's result
    uint64_t finished = 0;  // quote_generation_ when its last request finished
    // `json` cannot hold a -0 dailyTradingVolume (JSON writes 0); TypeScript
    // keeps the quote object, so its protobuf double keeps the sign.
    bool negative_zero_volume = false;
  };
  static constexpr size_t kQuoteEntries = 512;

  int64_t Now() const;
  Outcome Fetch(Arena& arena, uint64_t deadline_ms, const char* path, bool background, Value** body);
  Outcome Document(uint64_t deadline_ms, std::shared_ptr<const Snapshot>* cache, Flight* flight, const char* path, int64_t ttl_ms,
                   bool assets, bool background, uint64_t issued_generation, std::shared_ptr<const Snapshot>* out);
  bool FlattenLocked(const Snapshot* snapshot, providers::RobinhoodAssetCatalog* out) const;
  bool WaitLocked(uint64_t deadline_ms, const uint64_t* generation, uint64_t seen);

  chains::Transport* transport_ = nullptr;
  Fetcher* fetcher_ = nullptr;
  fortis::base::Clock* clock_ = nullptr;
  fortis::base::Mutex mutex_;
  fortis::base::ConditionVariable changed_;
  std::shared_ptr<const Snapshot> assets_;
  std::shared_ptr<const Snapshot> actions_;
  Flight assets_flight_;
  Flight actions_flight_;
  std::unique_ptr<QuoteEntry[]> quotes_;
  uint64_t quote_generation_ = 0;
};

// The read model the two routes answer, built in `arena`; any snapshot the
// result points into stays held until the caller releases it.
struct MarketsRequest {
  StockClient* client = nullptr;
  fortis::storage::KvStore* kv = nullptr;  // media registrations
  int64_t now_ms = 0;
  uint64_t deadline_ms = 0;
  // The StockClient generations when the HTTP request was accepted (see
  // StockClient::Assets): what a TypeScript request awaiting the in-flight
  // promises would share.
  uint64_t assets_issued = StockClient::kNotIssued;
  uint64_t quote_issued = StockClient::kNotIssued;
  uint64_t actions_issued = StockClient::kNotIssued;
};
// RobinhoodMarketsFacet.catalog().
Outcome Catalog(Arena& arena, const MarketsRequest& request, Value** result, std::shared_ptr<const StockClient::Snapshot>* held);
// RobinhoodMarketsFacet.instrument(address), in three steps so that the
// caller can run the quote and the corporate actions concurrently
// (Promise.all) and answer at the first failure:
//  - FindInstrument: assetByAddress, the first catalog row (sorted order)
//    whose contract matches `address` case-insensitively; *asset nullptr
//    when absent (404). The asset points into *held_assets.
//  - the reads: StockClient::ReferenceQuote(asset) into `arena` and
//    StockClient::CorporateActions (a held snapshot), both with the
//    request's issued generations;
//  - BuildInstrument: corporateActionsFor (this contract's actions, newest
//    process date first, stable, undated last), the display logo and its
//    media registration, and the response object.
Outcome FindInstrument(const MarketsRequest& request, const char* address, const Value** asset,
                       std::shared_ptr<const StockClient::Snapshot>* held_assets);
bool BuildInstrument(Arena& arena, const MarketsRequest& request, const Value* asset, Value* quote,
                     const StockClient::Snapshot* actions, Value** result);

// protobuf-es toBinary of the two read models (catalogMessage,
// instrumentMessage); false where it would throw.
bool EncodeCatalog(codec::tj::OutBuffer& out, const Value* catalog);
bool EncodeInstrument(codec::tj::OutBuffer& out, const Value* instrument);

}  // namespace fortis::exchange::robinhood
