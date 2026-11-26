#include "ownership.h"
#include <memory>
#include "function_ref.h"
#include "fixed_string.h"
#include "threading.h"
#pragma once

#include "address_records.h"
#include "address_store.h"
#include "chain_core.h"
#include "chain_registry.h"
#include "wallet_readers.h"
#include "chain_transport.h"
#include "clock.h"
#include "provider_runtime_config.h"
#include "token_swr.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

namespace fortis::storage { class KvStore; }
namespace fortis::prices { class PriceSeriesStore; class SpotProviderOwner; }

// The address facets, ported from the TypeScript facets: BalancesFacet,
// InsightFacet, NftsFacet, TxDetailFacet (+ pending watcher),
// TxDetailViewCache with MethodSignatureResolver, TronTokenHoldingsFacet,
// TransfersFacet with TransferIngester and fiat enrichment, and
// NetworkOverviewFacet. Each runs on a worker thread (never an HTTP worker)
// with a request arena, and returns the payload the TypeScript route sends
// or the error its runFacet contract maps to a status.
namespace fortis::wallet::address {

// The TypeScript cache TTL entries of these facets.
constexpr tokens::token_swr::Ttl kTtlBalances{20000, 600000};
constexpr tokens::token_swr::Ttl kTtlBalancesUtxo{60000, 600000};
constexpr tokens::token_swr::Ttl kTtlInsight{30000, 300000};
constexpr tokens::token_swr::Ttl kTtlTxDetail{3600000, 86400000};
constexpr tokens::token_swr::Ttl kTtlMethodSignature{86400000, 30LL * 86400000};
constexpr tokens::token_swr::Ttl kTtlNetworkOverview{300000, 3600000};
constexpr tokens::token_swr::Ttl kTtlTokenHolders{100800000, 604800000};

// runFacet's contract: kBadRequest = a RangeError (400 BAD_REQUEST + detail),
// kUpstream = 502 UPSTREAM_UNAVAILABLE, kLaneBusy = 429 RATE_LIMITED,
// kNotFound = 404 NOT_FOUND (tx view), kUnsupported = 400 UNSUPPORTED.
enum class Outcome : uint8_t { kOk, kBadRequest, kUpstream, kLaneBusy, kNotFound, kUnsupported };

struct FacetReply {
  Outcome outcome = Outcome::kUpstream;
  Value* payload = nullptr;
  char detail[512]{};
  uint32_t retry_after_ms = 0;
  // Overview: the NetworkOverviewResponse protobuf is built from the same
  // payload by the route.
};

// One background stale-while-revalidate refresh, executed by the service's
// revalidation worker. Malloc'd; the worker frees it.
struct Revalidation {
  enum class Kind : uint8_t { kBalances, kInsight, kView, kMethodSignature, kTronHoldings, kOverviewTokens, kHolders };
  Kind kind = Kind::kBalances;
  base::FixedString<1024> key;
  base::FixedString<96> network;
  base::FixedString<640> subject;   // address, hash, selector or token id
  base::FixedString<640> extra;     // holders: the token address
  size_t token_count = 0;
  std::unique_ptr<char*[], base::CStringArrayDeleter> tokens;  // balances: the tracked list (allocated strings)
};
void FreeRevalidation(Revalidation* job);

// The TypeScript AccountChangeTracker, shared by the EVM change
// detector job and the ingester. Bounded at kMaxAccounts accounts.
class ChangeTracker {
 public:
  struct Observation {
    bool observed = false;
    bool has_fingerprint = false;
    uint64_t fingerprint = 0;  // hash of the comparable balance string
    bool log_matched = false;
    bool full_coverage = false;
  };
  ChangeTracker();
  ~ChangeTracker();
  void Observe(const char* account_id, const Observation& observation, int64_t at_ms);
  bool IsUnchangedSince(const char* account_id, int64_t synced_at_ms, int64_t now_ms);
  // dirty(now): account ids (copied into `out`, at most `capacity`).
  size_t Dirty(int64_t now_ms, char (*out)[kAccountIdBytes + 1], size_t capacity);
  void MarkIngestRequested(const char* account_id, int64_t at_ms);
  // retainOnly(ids).
  void RetainOnly(const char (*ids)[kAccountIdBytes + 1], size_t count);
  size_t size();

 private:
  struct State;
  static constexpr size_t kMaxAccounts = 20000;
  std::unique_ptr<State[], base::StorageDeleter> states_;
  size_t count_ = 0;
  fortis::base::Mutex mutex_;
};

struct FacetDeps {
  const config::ProviderRuntimeConfig* config = nullptr;
  const chains::Catalog* catalog = nullptr;
  chains::ChainTransport* transport = nullptr;
  const chains::ChainRegistry* chains = nullptr;  // null: native deployment capabilities
  Store* store = nullptr;
  tokens::token_swr::Cache* swr = nullptr;
  storage::KvStore* kv = nullptr;
  WalletSeriesReader* series = nullptr;  // candle reads for fiat-at-time
  WalletSpotReader* spot = nullptr;   // the price plane; null = unpriced
  ChangeTracker* changes = nullptr;
  // NewsFacet.feed({topics: ['chains','protocol','security'], limit: 100}):
  // the cached wire payload {items, asOf, stale}. False = the feed threw.
  bool (*news_feed)(void* context, int64_t now_ms, Arena& arena, Value** payload) = nullptr;
  void* news_context = nullptr;
  // Background revalidation queue; false when full (the refresh is shed).
  bool (*enqueue)(void* context, Revalidation* job) = nullptr;
  void* enqueue_context = nullptr;
  // The delta poll's race: runs EnsureFresh on the service's ingest worker
  // and waits at most `wait_ms` for it. False = not started (the caller then
  // ingests inline).
  bool (*ingest_async)(void* context, const char* account_id, const Network& network, const char* address,
                       int64_t max_age_ms, uint64_t wait_ms) = nullptr;
  void* ingest_context = nullptr;
  // The facets' clock (NowMs); null uses the SWR cache clock.
  base::Clock* clock = nullptr;
};

struct WalletQueryContext;
class WalletQueryService {
 public:
  explicit WalletQueryService(const FacetDeps& deps);
  ~WalletQueryService();
  WalletQueryService(const WalletQueryService&) = delete;
  WalletQueryService& operator=(const WalletQueryService&) = delete;

  int64_t Now() const;

  // GET …/address/{a}/balances (tokens = the trimmed, non-empty list).
  FacetReply Balances(Arena& arena, const char* network, const char* address, const char* const* tokens,
                  size_t token_count);
  // GET …/address/{a}/insight.
  FacetReply Insight(Arena& arena, const char* network, const char* address);
  // GET …/address/{a}/nfts.
  FacetReply Nfts(Arena& arena, const char* network, const char* address);
  // GET /v1/networks/{n}/tx/{hash}.
  FacetReply TxDetail(Arena& arena, const char* network, const char* hash);
  // GET /v1/networks/{n}/tx/{hash}/view (after the route's family check).
  FacetReply TxView(Arena& arena, const char* network, const char* hash);
  // GET …/address/{a}/token-holdings: {data: holdings} (route-validated).
  FacetReply TronHoldings(Arena& arena, const char* address);
  // GET …/address/{a}/transfers.
  FacetReply Transfers(Arena& arena, const char* network, const char* address, const TransferQuery& query,
                   const char* device_id);
  // GET /v1/networks/{n}/overview.
  FacetReply Overview(Arena& arena, const char* network);

  // rememberAddressTopic / rememberTxTopic (failures are logged only).
  void RememberAddressTopic(Arena& arena, const char* device_id, const char* network, const char* address);
  void RememberTxTopic(Arena& arena, const char* device_id, const char* network, const char* hash);

  // ---- jobs --------------------------------------------------------------
  // TxDetailFacet.pollPendingOnce: transitions recorded.
  size_t PollPendingOnce(Arena& arena);
  // One revalidation (worker thread).
  void RunRevalidation(Revalidation* job, Arena& arena);
  // TransferIngester.ensureFresh (maxAge 0 forces). Blocks until the ingest
  // (or the one already running for the account) finishes.
  void EnsureFresh(const char* account_id, const Network& network, const char* address, int64_t max_age_ms);
  // Whether an ingest source covers the network (ingester.sourceFor).
  bool HasSource(const Network& network) const;
  // The TypeScript pruneTransfers (transfer retention).
  size_t PruneTransfers(Arena& arena);
  // The TypeScript OperationCandleRepairWorker.run;
  // `history` produces 1h/1d candles for a price id over [from, to) seconds
  // (PriceHistoryPort.history, background) and reports whether points exist.
  struct RepairResult { size_t groups = 0, histories_ready = 0, transfers_repaired = 0; };
  RepairResult RepairOperationCandles(Arena& arena,
      base::FunctionRef<bool(const char* price_id, int64_t from_sec, int64_t to_sec, bool hourly)> history);
  // backfillFiatAtTime for one account (tokenId/priceSourceId optional).
  size_t BackfillFiat(Arena& arena, const Network& network, const char* id, const char* token,
                      const char* price_id, int64_t now);

  // The chain context of one operation.
  chains::Context ChainContext(Arena& arena);


 private:
  std::unique_ptr<WalletQueryContext, fortis::base::ObjectDeleter<WalletQueryContext>> impl_;
};

using Facets = WalletQueryService;  // compatibility name for existing route owners

// getAddressBalances' tracked list: value.split(',').map(t => t.trim())
// .filter(t => t !== '') (String.prototype.trim's whitespace set). `*items`
// owns a C-compatible array of strings, including partial allocation failures.
bool SplitTrackedTokens(const char* value, size_t length, std::unique_ptr<char*[], base::CStringArrayDeleter>& items, size_t* count);

// The TypeScript BalancesFacet cacheKey: bal:{network}:{address}:{sha1(sorted tokens)[:12]}.
bool BalancesCacheKey(const char* network, const char* address, const char* const* tokens, size_t count,
                      char* output, size_t capacity);

// InsightFacet's SWR key: ins:{resolved network id}:{canonicalAddress} (EVM
// folded with String.prototype.toLowerCase, other families verbatim).
bool InsightCacheKey(codec::tj::Arena& arena, const chains::Network& network, const char* address, char* output,
                     size_t capacity);

// The token-holdings route's address gate, /^T[1-9A-HJ-NP-Za-km-z]{33}$/ (a
// Base58 alphabet check only: no checksum, no hex form).
bool ValidTronAddress(const char* address);
// TronTokenHoldingsFacet's SWR key: tronhold:{address} (verbatim; Base58 is
// case-significant).
bool TronHoldingsCacheKey(const char* address, char* output, size_t capacity);

// NftsFacet's durable records. nft/item/{enc(account)}/{enc(nftId)}: kUriError
// where encodeURIComponent throws (a lone surrogate), kTooLong over the native
// KV key bound (1,024 bytes), kNoRoom when `capacity` is too small. Records
// are stored as written (their JSON text) and read back as stored.
enum class NftKeyStatus : uint8_t { kOk, kUriError, kTooLong, kNoRoom };
NftKeyStatus NftItemKey(const char* account_id, const char* nft_id, char* output, size_t capacity);
// nft/item/{enc(account)}/ (the account's item range) and nft/scan/{enc(account)}.
bool NftItemPrefix(const char* account_id, char* output, size_t capacity);
bool NftScanKey(const char* account_id, char* output, size_t capacity);
// NftsFacet.scanDue: no record, or (no error within 60 s of now) and a last
// scan more than 10 minutes before now. `state` = the stored scan record.
bool NftScanDue(const codec::tj::Value* state, int64_t now_ms);
// TxDetailFacet's durable keys: txDetailRecordKey(network, hash) =
// tx/detail/{enc(network)}/{enc(hash)} and txPendingKey(pollUntil, network,
// hash) = tx/pending/{pollUntil padded to 13 digits}/{enc(network)}/
// {enc(hash)} (enc = encodeURIComponent). False when `capacity` is too small.
constexpr size_t kTxKeyBytes = 4096;
// A tx record is stored as its JSON text and a cached read answers it as
// stored: -0 reads back as 0 and Infinity as null (JSON has neither).
bool TxRecordKey(const char* network, const char* hash, char* out, size_t capacity);
// TxDetailViewCache's SWR key: keys.txDetail(network id, hash) =
// txd:{network id}:{hash}, both as given. False when `capacity` is too small.
bool TxViewKey(const char* network, const char* hash, char* out, size_t capacity);
bool TxPendingKey(int64_t poll_until, const char* network, const char* hash, char* out, size_t capacity);

}  // namespace fortis::wallet::address
