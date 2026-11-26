#include "threading.h"
#pragma once

#include <variant>
#include "chain_id_set.h"

#include "address_facets.h"
#include "wallet_components.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// Shared internals of address_facets.cpp, address_transfers.cpp and
// address_overview.cpp. Not a public interface.
namespace fortis::wallet::address {

constexpr size_t kSwrScratchBytes = 1024 * 1024;
constexpr size_t kFlights = 64;
constexpr size_t kFlightKeyBytes = 1024;

// SwrCache.read: the TypeScript stale-while-revalidate algorithm over
// tokens::token_swr::Cache (the envelope the token routes share).
struct SwrFetcher {
  // fetcher(): the value (may be JSON null) or a thrown error.
  bool (*fetch)(WalletQueryContext* impl, void* context, Arena& arena, Value** value, chains::Error* error) = nullptr;
  void* context = nullptr;
  // options.shouldStore (null = always store).
  bool (*should_store)(WalletQueryContext* impl, void* context, const Value* value) = nullptr;
  // options.floor (null = none): false = no floor.
  bool (*floor)(WalletQueryContext* impl, void* context, Arena& arena, Value** value, int64_t* as_of) = nullptr;
  // Builds the background job of a stale read (null = no revalidation).
  Revalidation* (*revalidation)(void* context, const char* key) = nullptr;
  // A fetch that throws "unknown" (TxDetailViewCache's UnknownTransactionError).
  bool unknown_is_error = false;
};

enum class SwrSource : uint8_t { kCache, kLive, kFloor };

struct SwrResult {
  bool ok = false;
  Value* value = nullptr;
  int64_t as_of_ms = 0;
  bool stale = false;
  SwrSource source = SwrSource::kLive;
  chains::Error error{};
  // The fetch reported "unknown" (no error class; the caller answers 404).
  bool unknown = false;
};

// A fetch reports "unknown" with this error message marker.
constexpr const char* kUnknownMarker = "\x01unknown";

struct WalletQueryContext {
  FacetDeps deps;
  WalletValuation valuation;
  TransferIngestor ingestor;
  TransferRepairService repair;
  WalletOverviewBuilder overview;
  chains::ChainRegistry native_chains;
  const chains::ChainRegistry& Chains() const { return deps.chains ? *deps.chains : native_chains; }
  fortis::base::Mutex flight_mutex;
  fortis::base::ConditionVariable flight_done;
  struct Flight {
    bool used = false;
    uint64_t hash = 0;
    char key[kFlightKeyBytes]{};
    uint64_t generation = 0;  // this flight's number (flight_generation)
    size_t waiters = 0;       // callers waiting on it
  };
  Flight flights[kFlights]{};
  uint64_t flight_generation = 0;
  // A coalesced miss whose live value was not stored (shouldStore false):
  // TypeScript's followers share the leader's promise and answer the same
  // value, so the leader leaves its JSON text here, under flight_mutex, for
  // the followers that waited on its flight. A ring; an entry is freed when
  // its last follower took it or when it is overwritten (a follower that
  // finds none fetches by itself).
  static constexpr size_t kSharedLive = 16;
  struct SharedLive {
    uint64_t generation = 0;
    size_t remaining = 0;
    std::unique_ptr<uint8_t[], base::StorageDeleter> text;
    size_t length = 0;
  };
  SharedLive shared_live[kSharedLive]{};
  size_t shared_live_next = 0;
  // The RangeErrors of recently failed misses. TypeScript's hold replays the
  // stored error object, so a RangeError raised after provider reads (an EVM
  // nonce that BigInt() refuses) must replay without fetching again. A small
  // ring under flight_mutex; an evicted message is re-derived by fetching.
  static constexpr size_t kHeldRanges = 16;
  struct HeldRange {
    char key[kFlightKeyBytes]{};
    char message[sizeof(chains::Error::message)]{};
  };
  HeldRange held_ranges[kHeldRanges]{};
  size_t held_range_next = 0;

  explicit WalletQueryContext(const FacetDeps& d) : deps(d), valuation(deps), ingestor(*this), repair(*this), overview(*this) {}
  ~WalletQueryContext() {
    for (SharedLive& shared : shared_live) shared.text.reset();
  }

  int64_t Now() const { return deps.clock ? deps.clock->NowMs() : deps.swr->Now(); }

  // Query clock adapter for the chain contexts.
  class FacetClock final : public base::Clock {
   public:
    explicit FacetClock(const WalletQueryContext* impl) : impl_(impl) {}
    int64_t NowMs() override { return impl_->Now(); }

   private:
    const WalletQueryContext* impl_;
  } clock{this};

  // In-process single flight: true = this caller leads `key` (call
  // EndFlight); false = another caller led it and has finished (or the wait
  // timed out / the table is full: the caller proceeds alone).
  // *generation (when given) names the flight a follower waited on.
  bool BeginFlight(const char* key, size_t* slot, bool* waited, uint64_t* generation = nullptr);
  // `share`: the leader's live value that was not stored, kept for the
  // flight's followers (TakeShared).
  void EndFlight(size_t slot, const Value* share = nullptr);
  bool TakeShared(uint64_t generation, Arena& arena, Value** value);
  void HoldRange(const char* key, const char* message);
  bool HeldRangeMessage(const char* key, chains::Error* error);

  chains::Context ChainContext(Arena& arena);
  using NetworkResolution = std::variant<Network, FacetReply>;
  NetworkResolution Resolve(const char* id) const;

  bool SwrRead(const char* key, const tokens::token_swr::Ttl& ttl, const SwrFetcher& fetcher, Arena& arena,
               SwrResult* out);
  // The revalidation of one stale key: fetch, store when shouldStore.
  void Revalidate(const char* key, const tokens::token_swr::Ttl& ttl, const SwrFetcher& fetcher, Arena& arena);
  bool Enqueue(Revalidation* job);

  // Balances fetch (driver + priceJoin + writeThrough), shared by the route
  // and its revalidation.
  bool BalancesFetch(const Network& network, const char* address, const char* const* tokens, size_t count,
                     Arena& arena, Value** payload, chains::Error* error);
  const char* MethodSignature(const char* selector, Arena& arena);
  bool TxViewFetch(const Network& network, const char* hash, Arena& arena, Value** detail, chains::Error* error);
};

// address_overview.cpp: the overview popular-token and holder refreshes.
void RunOverviewRevalidation(WalletQueryContext* impl, Revalidation* job, const Network& network, Arena& arena);

// Shared helpers.
Value* CacheJson(Arena& arena, bool hit, int64_t timestamp_ms, bool has_timestamp);
Value* SwrProvenance(Arena& arena, const SwrResult& result);
void SetError(FacetReply* result, const chains::Error& error);
const char* Canonical(Arena& arena, const Network& network, const char* address);
bool IsUtxoFamily(chains::Family family);
uint64_t KeyHash(const char* key);

}  // namespace fortis::wallet::address
