#include "fixed_string.h"
#include "ownership.h"
#include <string_view>
#pragma once

#include "chain_id_set.h"

#include "network_registry.h"
#include "token_json.h"
#include "token_ref.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

namespace fortis::config { struct ProviderRuntimeConfig; }
namespace fortis::providers { class TokenProviderOwner; struct TokenFetch; }
namespace fortis::storage { class KvStore; }
namespace fortis::prices { struct PriceSeriesCandle; }
namespace fortis::tokens::token_data { class TokenDataStore; }
namespace fortis::tokens::token_swr { class Cache; }
namespace fortis::tokens::token_jobs { class TokenStats; }
namespace fortis::tokens::token_tron { struct Flights; }
namespace fortis::tokens::token_bittensor { class WorkingSet; }
namespace fortis::exchange::robinhood { class StockClient; }

// Native port of the TypeScript Token360Facet (facts / liquidityDetails
// and their persistence), the composition every token route reads.
// One facts() call runs on a token worker thread: it blocks on provider
// batches (TokenProviderOwner::FetchAll) and builds every document in the
// work arena. Guest-safe.
namespace fortis::tokens::token_facts {

using codec::tj::Arena;
using codec::tj::Value;

// The TypeScript CacheProvenance.
struct Provenance {
  bool hit = false;
  bool has_timestamp = false;
  int64_t timestamp = 0;
  bool partial = false;
};
Value* ProvenanceJson(Arena& arena, const Provenance& provenance);
Provenance CombineProvenance(const Provenance* entries, size_t count);

enum Facet : uint8_t { kMarket, kProfile, kLiquidity, kHolders, kChainMeta, kInsight, kContract, kFacetCount };
constexpr uint32_t FacetBit(Facet facet) { return 1u << facet; }
constexpr uint32_t kAllFacets = (1u << kFacetCount) - 1;
const char* FacetName(Facet facet);

enum class Source : uint8_t { kSkipped, kFailed, kCache, kLive, kFloor };

struct FacetResult {
  Value* value = nullptr;  // nullptr is JavaScript null
  Provenance cache;
  bool has_as_of = false;
  int64_t as_of = 0;
  Source source = Source::kSkipped;
};

enum class ResolutionSource : uint8_t { kNone, kRef, kRegistry, kContract, kSearch, kHint };
const char* ResolutionSourceName(ResolutionSource source);  // null for kNone
constexpr size_t kCoinIdBytes = 1100;
struct Resolution {
  bool has_coin = false;
  char coin_id[kCoinIdBytes]{};
  ResolutionSource source = ResolutionSource::kNone;
  bool ambiguous = false;
};

// A background revalidation of one SWR entry (SwrCache.revalidate), queued
// by a foreground read that served a stale value.
enum class RevalidationKind : uint8_t { kFacet, kLiquidityDetail, kResolution, kHolderTrends };
struct Revalidation {
  RevalidationKind kind = RevalidationKind::kFacet;
  Facet facet = kMarket;
  bool persisted = false;
  bool has_coin = false;
  base::FixedString<1100> key;
  base::FixedString<128> network;
  base::FixedString<networks::kTokenRefMaxIdBytes + 1> token_id;
  base::FixedString<networks::kTokenRefMaxIdBytes + kCoinIdBytes + 2> scope;
  base::FixedString<kCoinIdBytes> coin_id;
  base::FixedString<96> source;  // kHolderTrends: the holder source
};

// spot quote seam (PricesFacet.spot for one id): fills price/change/asOf and
// cache provenance; false when the price plane has no hit or failed.
struct SpotQuote {
  double price = 0;
  bool has_change = false;
  double change = 0;
  bool has_as_of = false;
  char as_of[40]{};
  Provenance cache;
};

struct Work;
using fortis::prices::PriceSeriesCandle;

// price series seam (PricesFacet.history(id, 'usd', from, to, interval) for
// the page `series` section): candles live in the work arena.
enum class SeriesStatus : uint8_t {
  kOk,           // a (possibly empty) series
  kNoSeries,     // history() answered null
  kUnavailable,  // history() threw: a read failure, shutdown or cancellation
};
struct SeriesRead {
  const PriceSeriesCandle* candles = nullptr;
  size_t count = 0;
  Provenance cache;
};
// Borrowed collaborators, owned by application composition. These interfaces
// are never deleted through a base pointer; values borrow the request arena.
class TokenFactExecutor {
 public:
  virtual void FetchAll(providers::TokenFetch* fetches, size_t count, uint64_t deadline_ms) = 0;
 protected:
  ~TokenFactExecutor() = default;
};
class ProviderOwnerExecution final : public TokenFactExecutor {
 public:
  explicit ProviderOwnerExecution(providers::TokenProviderOwner& owner) : owner_(owner) {}
  void FetchAll(providers::TokenFetch* fetches, size_t count, uint64_t deadline_ms) override;
 private:
  providers::TokenProviderOwner& owner_;
};
class TokenSpotReader {
 public:
  virtual bool Quote(const char* price_id, uint64_t deadline_ms, SpotQuote* quote) = 0;
 protected:
  ~TokenSpotReader() = default;
};
class TokenSeriesReader {
 public:
  virtual SeriesStatus Read(Work& work, const char* price_id, int64_t from_seconds,
                            int64_t to_seconds, bool hourly, SeriesRead* result) = 0;
 protected:
  ~TokenSeriesReader() = default;
};
class TokenRevalidationQueue {
 public:
  virtual bool Enqueue(const Revalidation& task) = 0;
 protected:
  ~TokenRevalidationQueue() = default;
};

// Shared, process-lifetime collaborators.
struct Env {
  storage::KvStore* kv = nullptr;
  token_data::TokenDataStore* data = nullptr;
  token_swr::Cache* swr = nullptr;
  TokenFactExecutor* executor = nullptr;
  token_jobs::TokenStats* stats = nullptr;
  const config::ProviderRuntimeConfig* config = nullptr;
  // Queues a revalidation; false when the queue is full (the stale value is
  // still served, TypeScript's revalidation would simply run later).
  TokenRevalidationQueue* revalidation_queue = nullptr;
  TokenSpotReader* spot = nullptr;
  // Null means the optional history service is not composed.
  TokenSeriesReader* series = nullptr;
  // EtherscanHolderClient.unsupportedChainIds (process lifetime).
  chains::BoundedChainIdSet<32>* etherscan_unsupported = nullptr;
  // TronDriver's token snapshot flights (null: every read runs solo).
  token_tron::Flights* tron_flights = nullptr;
  // BittensorDriver's 15 s subnet and TAO/USD working set (null: every read
  // goes to Taostats).
  token_bittensor::WorkingSet* bittensor = nullptr;
  // Token360Facet's `robinhood` dependency (RobinhoodStockTokenClient); null
  // only in compositions without one: Robinhood refs then take the EVM path.
  exchange::robinhood::StockClient* robinhood = nullptr;
  // The Robinhood network's deployment RPC endpoints (registry options
  // rpcUrls: Alchemy's with ALCHEMY_API_KEY); none: the catalog seed's.
  const char* const* robinhood_rpc_urls = nullptr;
  size_t robinhood_rpc_url_count = 0;
  // Token360Facet's `saucerswap` dependency: production TypeScript always
  // builds SaucerSwapClient (keyless works). Only a test seam turns it off,
  // to replay fixtures frozen without the client.
  bool saucerswap = true;
};

// Per-task state: the arena every document lives in and a KV scratch.
struct Work {
  Work() = default;
  Work(const Work&) = delete;
  Work& operator=(const Work&) = delete;
  Work(Work&&) = delete;
  Work& operator=(Work&&) = delete;
  Env* env = nullptr;
  Arena arena;
  uint8_t* scratch = nullptr;  // KV values (read and serialized writes)
  size_t scratch_capacity = 0;
  bool background = false;      // lane priority of provider work
  uint64_t deadline_ms = 0;     // monotonic deadline for provider batches
  std::unique_ptr<uint8_t[], base::StorageDeleter> scratch_storage;
  std::unique_ptr<uint8_t[], base::StorageDeleter> arena_storage;
  std::unique_ptr<Arena, codec::tj::ArenaBlocksDeleter> growing_blocks;
  int64_t Now() const;
};

struct Registry {
  bool found = false;
  Value* symbol = nullptr;  // string or null
  Value* name = nullptr;
};

// TokenFacts. Documents (Value*) live in the work arena; nullptr is null.
struct Facts {
  networks::TokenNetwork network;
  const networks::NativeNetworkConfig* seed = nullptr;
  networks::TokenRef ref;
  Value* input = nullptr;  // ref.input: the trimmed raw ref
  Resolution resolution;
  Registry registry;
  Value* chain_meta = nullptr;
  Value* market = nullptr;
  Value* profile = nullptr;
  Value* liquidity = nullptr;
  Value* holders = nullptr;
  Value* insight = nullptr;  // array
  Value* contract = nullptr;
  Value* safety = nullptr;
  Value* errors = nullptr;  // object, insertion-ordered
  Provenance cache[kFacetCount];
};

enum class FactsOutcome : uint8_t {
  kOk,
  kUnknownNetwork,   // RangeError('unknown network: …')
  kUnsupportedRef,   // RangeError('unsupported token ref: …')
  kInternal,         // arena exhaustion or a KV failure TypeScript would throw
};

// Resolves {network, ref} exactly as facts() does (registry.network +
// parseTokenRef over the trimmed input). Every family's refs reach ported
// providers.
FactsOutcome Resolve(std::string_view network_input, std::string_view ref_input,
               networks::TokenNetwork* network, const networks::NativeNetworkConfig** seed, networks::TokenRef* ref);

// Token360Facet.facts(networkId, rawRef, {coingeckoIdHint, only}).
// `only_mask` is a FacetBit set (kAllFacets for "omitted").
FactsOutcome ComputeFacts(Work& work, std::string_view network_input, std::string_view ref_input, const char* coingecko_hint, uint32_t only_mask, Facts* facts);

// Token360Facet.liquidityDetails: the TokenLiquidityDetails document.
FactsOutcome LiquidityDetails(Work& work, std::string_view network_input, std::string_view ref_input, Value** document);

// Runs one queued revalidation (SwrCache.revalidate + onRevalidated).
void RunRevalidation(Work& work, const Revalidation& task);
// True for a market revalidation the CoinGecko batcher can merge.
bool BatchableMarket(const Revalidation& task);
// True for a revalidation answered by a shared driver read: the Tron token
// snapshot (chainMeta, insight, holders of a TRC-20/TRC-10 ref), the
// Stellar/Cardano asset index (holders, insight), the Algorand/XRPL issuer
// read (insight) or a Hedera HTS ref's Mirror and SaucerSwap reads
// (chainMeta, insight, holders; market and profile without a coin id;
// liquidity).
bool BatchableSnapshot(const Revalidation& task, const Env& env);
// The two revalidations read the same snapshot (network and token id).
bool SameSnapshot(const Revalidation& a, const Revalidation& b);
// Runs snapshot revalidations of one token as the driver's flight does when
// their SwrCache revalidations start together: one shared read, then each
// task's store, persistence and EndRevalidate.
void RunSnapshotRevalidations(Work& work, const Revalidation* const* tasks, size_t count);
// Runs market revalidations as CoinGeckoClient.coinMarket does under its
// WindowBatcher: one /coins/markets request for every distinct coin id (in
// first-enqueue order), then each task's row, store and persistence.
void RunMarketRevalidations(Work& work, const Revalidation* const* tasks, size_t count);

// Holder trend helpers shared with the holder history route.
// "tf:{network}:{tokenId}:holderTrends:{encodeURIComponent(source)}".
bool HolderTrendsKey(const char* network, const char* token_id, const char* source, char* out,
                     size_t capacity);

// "2026-01-02T03:04:05.000Z" as a string value; null for an invalid time.
Value* IsoValue(Arena& arena, int64_t epoch_ms);

}  // namespace fortis::tokens::token_facts
