#include <string_view>
#pragma once

#include "coingecko_price.h"
#include "coingecko_request.h"
#include "price_id_parser.h"
#include "price_spot_cache.h"
#include "spot_response_serializer.h"
#include "status.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::prices {

constexpr size_t kSpotResolutionMaxIds = 100;

enum class SpotResolutionIntentKind : uint8_t {
  kFetch,
  kBackgroundRevalidation,
  kUnsupportedProvider,
};

struct SpotResolutionIntent {
  SpotResolutionIntentKind kind = SpotResolutionIntentKind::kFetch;
  providers::PriceIdKind id_kind = providers::PriceIdKind::kInvalid;
  std::string_view source_id;
  // True when this unsupported source is an already-served stale quote.
  bool stale_value_served = false;
  bool background_revalidation = false;
};

enum class SpotResolutionCompletionStatus : uint8_t {
  kNotNeeded,
  kApplied,
  kPartiallyApplied,
  // Attempted fetch failed, returned malformed JSON, or had a malformed result.
  // A durable floor, if any, is now exposed as stale in the response.
  kProviderFailed,
  kInvalidWorkspace,
};

enum class SpotResolutionBatchState : uint8_t {
  kPending,
  kAnswered,
  kTransportFailure,
  kParseFailure,
  kInvalidResult,
};

enum class SpotResolutionBatchReceipt : uint8_t {
  kRecordedAnswer,
  kRecordedFailure,
  kDuplicateBatch,
  kInvalidBatch,
  kInvalidAccumulator,
};

struct SpotResolutionCompletion {
  SpotResolutionCompletionStatus status = SpotResolutionCompletionStatus::kNotNeeded;
  // A failed cache write does not discard provider answers from the response.
  fortis::prices::PriceSpotStoreStatus cache_store_status = fortis::prices::PriceSpotStoreStatus::kNoItems;
};

// Large fixed workspace (caller-owned; do not put on a small worker stack).
// Response and intent views point into this workspace and remain valid until
// the next BeginSpotResolution call. The workspace is deliberately non-copyable.
class SpotResolutionWorkspace {
 public:
  SpotResolutionWorkspace() = default;
  SpotResolutionWorkspace(const SpotResolutionWorkspace&) = delete;
  SpotResolutionWorkspace& operator=(const SpotResolutionWorkspace&) = delete;

 private:
  friend fortis::base::Status BeginSpotResolution(
      const std::string_view*, size_t, std::string_view, fortis::prices::PriceSpotCache&,
      int64_t, SpotResolutionWorkspace*, int64_t);
  friend SpotResolutionCompletion CompleteSpotResolution(
      SpotResolutionWorkspace*, fortis::prices::PriceSpotCache&,
      const providers::CoinGeckoSpotResult*, int64_t);
  friend const SpotResponseInput* SpotResolutionResponse(
      const SpotResolutionWorkspace*, bool*);
  friend const SpotResolutionIntent* SpotResolutionIntents(
      const SpotResolutionWorkspace*, size_t*);
  friend const char* const* SpotResolutionFetchIds(
      const SpotResolutionWorkspace*, size_t*);
  friend const providers::PriceIdKind* SpotResolutionFetchKinds(
      const SpotResolutionWorkspace*, size_t*);
  friend size_t SpotResolutionUniqueIdCount(const SpotResolutionWorkspace*);
  friend bool InitializeSpotResolutionBatchAccumulator(
      SpotResolutionWorkspace*, const providers::CoinGeckoSpotBatchPlan*,
      class SpotResolutionBatchAccumulator*);
  friend SpotResolutionCompletion FinalizeSpotResolutionBatches(
      SpotResolutionWorkspace*, fortis::prices::PriceSpotCache&,
      class SpotResolutionBatchAccumulator*, int64_t, bool);

  enum class ValueState : uint8_t { kPending, kPositive, kMissing, kUnsupported };
  struct Value {
    char source[fortis::prices::kPriceSpotMaxProviderBytes + 1]{};
    double price = 0;
    double change24h_pct = 0;
    bool has_change24h_pct = false;
    int64_t as_of_ms = 0;
    bool stale = false;
    bool cache_hit = false;
    bool has_cache_timestamp = false;
    int64_t cache_timestamp_ms = 0;
  };
  struct UniqueId {
    char source_id[providers::kPriceIdMaxSourceIdBytes + 1]{};
    providers::PriceIdKind kind = providers::PriceIdKind::kInvalid;
    ValueState state = ValueState::kPending;
    Value value{};
  };

  char fiat_[6]{};
  char raw_ids_[kSpotResolutionMaxIds][providers::kPriceIdMaxRawBytes + 1]{};
  size_t raw_lengths_[kSpotResolutionMaxIds]{};
  uint16_t raw_to_unique_[kSpotResolutionMaxIds]{};
  bool raw_invalid_[kSpotResolutionMaxIds]{};
  UniqueId unique_[kSpotResolutionMaxIds]{};
  size_t raw_count_ = 0;
  size_t unique_count_ = 0;
  size_t intent_count_ = 0;
  SpotResolutionIntent intents_[kSpotResolutionMaxIds]{};
  size_t fetch_count_ = 0;
  const char* fetch_ids_[kSpotResolutionMaxIds]{};
  providers::PriceIdKind fetch_kinds_[kSpotResolutionMaxIds]{};
  int64_t started_at_ms_ = 0;
  bool provider_completed_ = false;
  bool batch_managed_ = false;
  uint32_t generation_ = 0;
  SpotPriceInput response_prices_[kSpotResolutionMaxIds]{};
  std::string_view response_missing_[kSpotResolutionMaxIds]{};
  std::string_view response_invalid_[kSpotResolutionMaxIds]{};
  size_t response_price_count_ = 0;
  size_t response_missing_count_ = 0;
  size_t response_invalid_count_ = 0;
  SpotResponseInput response_{};
  void Reset();
  void CopyValue(const fortis::prices::PriceSpotValue& source, bool stale,
                 bool cache_hit, Value* output);
  void BuildResponse();
  bool ResponseReady() const;
};

// Per-batch inputs are copied into this fixed workspace when recorded; it does
// not retain pointers into extractor response storage. One successful-batch
// cache frame is written at finalization, containing only answered batches.
class SpotResolutionBatchAccumulator {
 public:
  SpotResolutionBatchAccumulator() = default;
  SpotResolutionBatchAccumulator(const SpotResolutionBatchAccumulator&) = delete;
  SpotResolutionBatchAccumulator& operator=(const SpotResolutionBatchAccumulator&) = delete;

 private:
  friend bool InitializeSpotResolutionBatchAccumulator(
      SpotResolutionWorkspace*, const providers::CoinGeckoSpotBatchPlan*,
      SpotResolutionBatchAccumulator*);
  friend SpotResolutionBatchReceipt RecordSpotResolutionBatchResult(
      SpotResolutionBatchAccumulator*, size_t, const providers::CoinGeckoSpotResult*, int64_t);
  friend SpotResolutionBatchReceipt RecordSpotResolutionBatchFailure(
      SpotResolutionBatchAccumulator*, size_t, bool);
  friend const SpotResolutionBatchState* SpotResolutionBatchStates(
      const SpotResolutionBatchAccumulator*, size_t*);
  friend SpotResolutionCompletion FinalizeSpotResolutionBatches(
      SpotResolutionWorkspace*, fortis::prices::PriceSpotCache&,
      SpotResolutionBatchAccumulator*, int64_t, bool);

  providers::CoinGeckoSpotBatchPlan plan_{};
  SpotResolutionBatchState states_[kSpotResolutionMaxIds]{};
  int64_t answered_at_ms_[kSpotResolutionMaxIds]{};
  providers::CoinGeckoSpotQuote quotes_[kSpotResolutionMaxIds]{};
  uint16_t quote_fetch_index_[kSpotResolutionMaxIds]{};
  int64_t quote_answered_at_ms_[kSpotResolutionMaxIds]{};
  size_t quote_count_ = 0;
  size_t recorded_count_ = 0;
  uint32_t workspace_generation_ = 0;
  int64_t started_at_ms_ = 0;
  bool initialized_ = false;
  bool finalized_ = false;
};

// Which kinds a fetch can reach: bare CoinGecko slugs and indexed contracts in
// any fiat, and SPL mints through Jupiter in USD only (the TypeScript Jupiter client
// prices USD only, and PricesFacet leaves other fiats missing without a provider call
// or write).
bool SpotResolutionKindFetchable(providers::PriceIdKind kind, const char* fiat);
// Provider name recorded as `source`/cache provenance: "coingecko" or
// "jupiter"; null for kinds without a native provider.
const char* SpotResolutionProviderName(providers::PriceIdKind kind);
// Exact request path length for a one-ID provider batch, or zero when the
// kind has no native provider. Owners and the accumulator share this formula.
size_t SpotResolutionSingleIdPathLength(providers::PriceIdKind kind, const char* source_id,
                                        const char* fiat);

// Parses at most 100 raw IDs, keeps exact aliases in the workspace, and reads
// the durable cache once per canonical source ID. The returned plan contains
// canonical fetch IDs with their kinds (bare CoinGecko slugs, `spl:` mints in
// USD), stale revalidation intents, and typed unsupported provider intents for
// the other recognized kinds. No network call occurs. kInvalidArgument when
// the workspace, the ids, the fiat or the time are unusable (the workspace is
// left reset); there is no other failure.
fortis::base::Status BeginSpotResolution(
    const std::string_view* raw_ids, size_t raw_count, std::string_view fiat,
    fortis::prices::PriceSpotCache& cache, int64_t now_ms,
    SpotResolutionWorkspace* workspace, int64_t refresh_lead_ms = 0);

// Complete the one fetch attempt selected by Begin. `provider_result` carries
// quotes keyed by canonical source ID (CoinGecko slug or `spl:` mint) for the
// workspace's fetch IDs. A null/error result is treated as fetch failure and
// never writes negatives. Valid results store positives plus unanswered IDs,
// one atomic frame per provider; live quotes remain in the response even if
// that cache write fails.
SpotResolutionCompletion CompleteSpotResolution(
    SpotResolutionWorkspace* workspace, fortis::prices::PriceSpotCache& cache,
    const providers::CoinGeckoSpotResult* provider_result, int64_t completed_at_ms);

// Response views are workspace-owned. `ready` is false while a fetch is
// pending or an unsupported ID has no served value; stale cached values
// remain ready even if their background provider is unsupported. Unsupported
// IDs are never mislabeled as missing. The response is serializable when ready.
const SpotResponseInput* SpotResolutionResponse(
    const SpotResolutionWorkspace* workspace, bool* ready);
const SpotResolutionIntent* SpotResolutionIntents(
    const SpotResolutionWorkspace* workspace, size_t* count);
const char* const* SpotResolutionFetchIds(
    const SpotResolutionWorkspace* workspace, size_t* count);
// Parallel to SpotResolutionFetchIds: the kind of each fetch ID.
const providers::PriceIdKind* SpotResolutionFetchKinds(
    const SpotResolutionWorkspace* workspace, size_t* count);
size_t SpotResolutionUniqueIdCount(const SpotResolutionWorkspace* workspace);

// Binds a plan to the current request. Plan IDs are the workspace's canonical
// fetch IDs in order; each batch holds IDs of one kind and its exact provider
// path length. Each batch can then be recorded exactly once with an extractor
// result (quotes keyed by canonical ID) or an explicit transport/parse failure.
// Finalize waits for every batch and applies successes and failures separately.
bool InitializeSpotResolutionBatchAccumulator(
    SpotResolutionWorkspace* workspace, const providers::CoinGeckoSpotBatchPlan* plan,
    SpotResolutionBatchAccumulator* accumulator);
SpotResolutionBatchReceipt RecordSpotResolutionBatchResult(
    SpotResolutionBatchAccumulator* accumulator, size_t batch_index,
    const providers::CoinGeckoSpotResult* result, int64_t answered_at_ms);
// `transport_failure` selects transport when true and parse when false.
SpotResolutionBatchReceipt RecordSpotResolutionBatchFailure(
    SpotResolutionBatchAccumulator* accumulator, size_t batch_index,
    bool transport_failure);
const SpotResolutionBatchState* SpotResolutionBatchStates(
    const SpotResolutionBatchAccumulator* accumulator, size_t* count);
SpotResolutionCompletion FinalizeSpotResolutionBatches(
    SpotResolutionWorkspace* workspace, fortis::prices::PriceSpotCache& cache,
    SpotResolutionBatchAccumulator* accumulator, int64_t completed_at_ms,
    bool persist_cache = true);

}  // namespace fortis::prices
