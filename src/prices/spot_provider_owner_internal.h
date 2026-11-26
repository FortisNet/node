#include <memory>
#include "fixed_string.h"
#include <string_view>
#include "threading.h"
#pragma once

// Internal to the spot provider owner units (spot_provider_owner.cpp,
// spot_owner_requests.cpp, spot_owner_batches.cpp, spot_owner_jupiter.cpp,
// spot_owner_robinhood.cpp): the owner's state and the bounds its units
// share. Nothing outside those units includes this.
#include "spot_provider_owner.h"
#include "ownership.h"

#include "bounded_json.h"
#include "clock.h"
#include "coin_popularity.h"
#include "coingecko_request.h"
#include "coingecko_price.h"
#include "coingecko_contract_price.h"
#include "jupiter_price.h"
#include "jupiter_request.h"
#include "kraken_price.h"
#include "defillama_price.h"
#include "mexc_price.h"
#include "vestige_price.h"
#include "vestige_request.h"
#include "tronscan_price.h"
#include "tronscan_request.h"
#include "robinhood_price.h"
#include "price_spot_cache.h"
#include "price_series_store.h"
#include "provider_execution.h"
#include "provider_execution_coordinator.h"
#include "provider_lane.h"
#include "upstream_redirect.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::prices {

namespace spot_provider_owner_internal {

constexpr size_t kResponseCapacity = 512 * 1024;
constexpr uint32_t kAttemptTimeoutMs = 10000;
constexpr uint64_t kWaitSliceMs = 100;
constexpr uint64_t kBatchWindowMs = 20;

constexpr size_t kMaxForegroundFlightDemand =
    kSpotProviderOwnerMaxRequests * kSpotResolutionMaxIds;

constexpr size_t kMaxBackgroundOnlyFlights = 400;

// A cancelled TLS attempt retains its immutable flight indices until result
// drain. Reserve for every fixed adapter job so new foreground work stays
// admissible while those bounded orphans settle.
constexpr size_t kMaxDrainingFlightEntries =
    providers::kProviderExecutionMaxJobs * providers::kCoinGeckoSpotMaxBatchCount;

constexpr size_t kMaxSharedFlights =
    kMaxForegroundFlightDemand + kMaxBackgroundOnlyFlights +
    kMaxDrainingFlightEntries;

constexpr size_t kMaxBackgroundBatchesInFlight = 1;
constexpr size_t max_size(size_t a, size_t b) { return a > b ? a : b; }

// Every provider path in a shared batch uses the same fixed buffer.
constexpr size_t kSharedBatchPathCapacity = max_size(
    max_size(providers::kJupiterPriceMaxPathBytes, providers::kCoinGeckoTokenPriceMaxPathBytes),
    max_size(max_size(providers::kVestigePriceMaxPathBytes, providers::kTronScanPriceMaxPathBytes),
             providers::kRobinhoodPricePathBytes)) + 1;

constexpr size_t kSplPrefixBytes = sizeof("spl:") - 1;

const char* lane_for_kind(providers::PriceIdKind kind);
const char* flight_platform(providers::PriceIdKind kind, const char* source_id);

// Jupiter pages commit as one group, like PricesFacet.runGroup over
// JupiterClient.prices(): pages are requested one after another, every page
// must parse before any quote or negative is written or served, and one
// failed page fails the whole group without a write (callers fall back to
// their durable floors). A group is bounded to one atomic cache frame, so a
// cohort larger than that forms further groups; TypeScript's WindowBatcher
// bounds its group at 250 mints.
constexpr size_t kJupiterGroupMaxMints = fortis::prices::kPriceSpotMaxBatchItems;
constexpr size_t kMaxJupiterGroups = kSpotProviderOwnerMaxRequests;
constexpr size_t kNoGroup = SIZE_MAX;
constexpr size_t kNoBatch = SIZE_MAX;

// As the TypeScript HttpClient: at most three attempts per page, and a body that
// fails
// res.json() counts as a transport failure that is retried after 250 ms and
// then 1 s. The adapter already retries 5xx/408/transport within one job.
constexpr uint32_t kJupiterPageMaxAttempts = providers::kProviderExecutionMaxAttempts;
constexpr uint64_t kJupiterParseRetryDelaysMs[2] = {250, 1000};


}  // namespace spot_provider_owner_internal
using namespace spot_provider_owner_internal;


struct SpotTypes {

  enum class SlotState : uint8_t { kFree, kQueued, kWorking, kInFlight };
  enum class FlightState : uint8_t { kFree, kWindow, kInFlight };
  struct FlightWaiter {
    size_t slot_index = 0;
    uint32_t generation = 0;
    size_t fetch_index = 0;
  };
  struct SharedFlight {
    FlightState state = FlightState::kFree;
    bool background = false;
    providers::PriceIdKind kind = providers::PriceIdKind::kInvalid;
    uint32_t generation = 0;
    base::FixedString<6> fiat;
    // Canonical source ID for one provider flight.
    base::FixedString<providers::kPriceIdMaxSourceIdBytes + 1> id;
    base::FixedString<64> platform;
    uint64_t window_deadline_ms = 0;
    size_t waiter_count = 0;
    // Waiters from foreground requests. Background requests (the scheduled
    // active refresh) wait without making the flight foreground work.
    size_t foreground_waiters = 0;
    // A stale-served id's refresh (PricesFacet.revalidate): it needs no
    // waiter, but TypeScript runs it at FOREGROUND lane priority ("a stale
    // price is user-visible"), so it is neither shed by a busy lane nor held
    // behind the one background batch.
    bool stale_refresh = false;
    bool kraken_fallback = false;
    bool defillama_fallback = false;
    bool mexc_fallback = false;
    double usdt_usd = 0;
    int64_t fx_observed_at_ms = 0;
    FlightWaiter waiters[kSpotProviderOwnerMaxRequests]{};
  };
  struct Slot {
    size_t pending_flights = 0;
    uint16_t flight_indices[kSpotResolutionMaxIds]{};
    bool flight_registered[kSpotResolutionMaxIds]{};
    uint64_t deadline_ms = 0;
    uint32_t generation = 0;
    uint64_t request_id = 0;
    SpotProviderOwnerFormat format = SpotProviderOwnerFormat::kJson;
    SpotProviderOwnerCompletionFn completion = nullptr;
    void* completion_context = nullptr;
    bool cancel_requested = false;
    bool callback_sent = false;
    bool batch_initialized = false;
    // The deadline passed while provider flights were pending: those ids were
    // recorded as failed fetches and the request answers from the cache and
    // durable floor (TypeScript's answer once its fetch gives up), not kTimeout.
    bool deadline_fallback = false;
    // PricesFacet.spot(..., { background: true }): misses become background
    // flights on the provider lane's background priority.
    bool background = false;
    int64_t refresh_lead_ms = 0;
    SlotState state = SlotState::kFree;
    char fiat[6]{};
    char raw_ids[kSpotResolutionMaxIds][providers::kPriceIdMaxRawBytes + 1]{};
    std::string_view raw_views[kSpotResolutionMaxIds]{};
    size_t raw_count = 0;
    SpotResolutionWorkspace workspace;
    providers::CoinGeckoSpotBatchPlan plan{};
    SpotResolutionBatchAccumulator accumulator;
  };
  struct SharedBatch {
    bool used = false;
    bool background = false;
    bool tronscan_anonymous_retry = false;
    bool kraken_fallback = false;
    bool defillama_fallback = false;
    bool mexc_fallback = false;
    double usdt_usd = 0;
    int64_t fx_observed_at_ms = 0;
    bool robinhood = false;
    bool robinhood_catalog_request = false;
    uint16_t robinhood_group_index = UINT16_MAX;
    uint32_t robinhood_group_generation = 0;
    size_t robinhood_position = 0;
    providers::PriceIdKind kind = providers::PriceIdKind::kInvalid;
    uint32_t generation = 0;
    uint64_t job_id = 0;
    size_t flight_count = 0;
    // The Jupiter group this page belongs to; CoinGecko batches have none.
    size_t group = kNoGroup;
    uint16_t flights[providers::kCoinGeckoSpotMaxBatchCount]{};
    char path[kSharedBatchPathCapacity]{};
  };
  struct JupiterGroupQuote {
    uint16_t flight = 0;
    double price = 0;
    double change24h_pct = 0;
    bool has_change24h_pct = false;
  };
  struct RobinhoodQuoteCacheEntry {
    bool used = false;
    char address[43]{};
    double price = 0;
    int64_t until_ms = 0;
  };
  struct RobinhoodGroup {
    bool used = false;
    bool catalog_complete = false;
    uint32_t generation = 0;
    size_t flight_count = 0;
    size_t next_position = 0;
    size_t completed_count = 0;
    size_t active_quotes = 0;
    uint16_t flights[kSpotResolutionMaxIds]{};
    struct Value { double price = 0; bool has_price = false; } values[kSpotResolutionMaxIds]{};
  };
  static constexpr size_t kMaxRobinhoodGroups = kSpotProviderOwnerMaxRequests;
  static constexpr size_t kRobinhoodQuoteConcurrency = 2;
  struct JupiterGroup {
    bool used = false;
    bool background = false;
    uint32_t generation = 0;
    char fiat[6]{};
    size_t flight_count = 0;
    uint16_t flights[kJupiterGroupMaxMints]{};
    // flights[next_page_start..] have not been answered yet.
    size_t next_page_start = 0;
    size_t active_batch = kNoBatch;
    // HTTP attempts spent on the current page across malformed-body retries.
    uint32_t page_attempts = 0;
    // Nonzero: resubmit the current page no earlier than this monotonic time.
    uint64_t retry_at_ms = 0;
    size_t quote_count = 0;
    JupiterGroupQuote quotes[kJupiterGroupMaxMints]{};
    int64_t answered_at_ms = 0;
  };
  enum class PageSubmit : uint8_t { kSubmitted, kBusy, kFailed };
  static_assert(sizeof(Slot) <= 384 * 1024,
                "a request slot must remain within a fixed memory budget");
  struct CompletionHandle {
    SpotProviderOwnerCompletionFn callback = nullptr;
    void* context = nullptr;
    uint64_t request_id = 0;
    SpotProviderOwnerResultStatus status = SpotProviderOwnerResultStatus::kInternalFailure;
    bool valid = false;
  };


};
struct SpotRuntime;

class SpotRequestProcessor {
 public:
  using SlotState = SpotTypes::SlotState;
  using FlightWaiter = SpotTypes::FlightWaiter;
  using SharedFlight = SpotTypes::SharedFlight;
  using Slot = SpotTypes::Slot;
  using CompletionHandle = SpotTypes::CompletionHandle;
  void RecordDemand(const std::string_view* raw_ids, size_t count,
                                    const char* device_id);
  SpotProviderOwnerSubmission Submit(
    const std::string_view* raw_ids, size_t raw_count, std::string_view fiat,
    SpotProviderOwnerFormat format, uint64_t deadline_monotonic_ms,
    SpotProviderOwnerCompletionFn completion, void* completion_context,
    SpotSubmitMode mode, const char* device_id, int64_t refresh_lead_ms);
  bool Cancel(uint64_t request_id);
  explicit SpotRequestProcessor(SpotRuntime& runtime) : runtime_(runtime) {}
  SpotRequestProcessor(const SpotRequestProcessor&) = delete;
  SpotRequestProcessor& operator=(const SpotRequestProcessor&) = delete;
  void Release(size_t index);
  static void ResetSlotFields(Slot& slot, uint32_t generation);
  bool IsState(size_t index, SlotState expected);
  CompletionHandle CommitCompletion(size_t index,
                                                      SpotProviderOwnerResultStatus requested_status);
  void EmitSerialized(size_t index, SpotResolutionCompletionStatus resolution);
  void FinishWithoutBody(size_t index, SpotProviderOwnerResultStatus status,
                                                SpotResolutionCompletionStatus resolution);
  void ScheduleStaleRevalidations(SpotResolutionWorkspace* workspace,
                                                         const char* fiat, int64_t refresh_lead_ms);
  void ProcessQueued(size_t index);
  void CompleteRequest(size_t index);
  void ExpireToFallback(size_t index, uint32_t generation);
  void CancelOrExpire(uint64_t now, bool stopping);
 private:
  friend struct SpotRuntime;
  friend class SpotProviderOwner;
  friend class FlightTable;
  friend class SpotBatchScheduler;
  friend class JupiterSpotBatch;
  friend class RobinhoodSpotBatch;
  SpotRuntime& runtime_;
  Slot slots[kSpotProviderOwnerMaxRequests]{};
  uint8_t serialized[kResponseCapacity]{};
  size_t serialized_capacity = kResponseCapacity;
  uint64_t next_request_id = 1;
};

class FlightTable {
 public:
  using SlotState = SpotTypes::SlotState;
  using FlightState = SpotTypes::FlightState;
  using FlightWaiter = SpotTypes::FlightWaiter;
  using SharedFlight = SpotTypes::SharedFlight;
  using Slot = SpotTypes::Slot;
  static constexpr size_t kMaxRobinhoodGroups = SpotTypes::kMaxRobinhoodGroups;
  explicit FlightTable(SpotRuntime& runtime) : runtime_(runtime) {}
  FlightTable(const FlightTable&) = delete;
  FlightTable& operator=(const FlightTable&) = delete;
  size_t FindOrCreateFlight(const char* fiat, const char* id, providers::PriceIdKind kind,
                                                   bool background);
  void CompleteFlight(size_t flight_index, bool success,
                                             const providers::CoinGeckoSpotResult* extracted,
                                             int64_t answered_at_ms);
  void CompleteFlightFailure(size_t flight_index, bool transport_failure);
  // Owner-thread only; requeues one eligible flight once, preserving waiters/deadlines.
  bool TryPriceFallback(size_t flight_index);
  void DetachSlotWaiters(size_t slot_index, uint32_t generation);
  const SharedFlight& at(size_t index) const { return flights[index]; }
 private:
  friend struct SpotRuntime;
  friend class SpotProviderOwner;
  friend class SpotRequestProcessor;
  friend class SpotBatchScheduler;
  friend class JupiterSpotBatch;
  friend class RobinhoodSpotBatch;
  SpotRuntime& runtime_;
  SharedFlight flights[kMaxSharedFlights]{};
};

class SpotBatchScheduler {
 public:
  using FlightState = SpotTypes::FlightState;
  using FlightWaiter = SpotTypes::FlightWaiter;
  using SharedFlight = SpotTypes::SharedFlight;
  using SharedBatch = SpotTypes::SharedBatch;
  using RobinhoodGroup = SpotTypes::RobinhoodGroup;
  using JupiterGroup = SpotTypes::JupiterGroup;
  using PageSubmit = SpotTypes::PageSubmit;
  explicit SpotBatchScheduler(SpotRuntime& runtime) : runtime_(runtime) {}
  SpotBatchScheduler(const SpotBatchScheduler&) = delete;
  SpotBatchScheduler& operator=(const SpotBatchScheduler&) = delete;
  size_t ActiveBackgroundBatches() const;
  void ResetBatch(SharedBatch& batch);
  void ScheduleSharedBatches();
  void CompleteBatch(const providers::ProviderExecutionResult& result, size_t body_length);
 private:
  friend struct SpotRuntime;
  friend class SpotProviderOwner;
  friend class FlightTable;
  friend class JupiterSpotBatch;
  friend class RobinhoodSpotBatch;
  SpotRuntime& runtime_;
  SharedBatch shared_batches[kSpotProviderOwnerMaxRequests]{};
  bool anonymous_contract_singletons = false;
};

class JupiterSpotBatch {
 public:
  using FlightWaiter = SpotTypes::FlightWaiter;
  using SharedFlight = SpotTypes::SharedFlight;
  using SharedBatch = SpotTypes::SharedBatch;
  using JupiterGroupQuote = SpotTypes::JupiterGroupQuote;
  using JupiterGroup = SpotTypes::JupiterGroup;
  using PageSubmit = SpotTypes::PageSubmit;
  explicit JupiterSpotBatch(SpotRuntime& runtime) : runtime_(runtime) {}
  JupiterSpotBatch(const JupiterSpotBatch&) = delete;
  JupiterSpotBatch& operator=(const JupiterSpotBatch&) = delete;
  size_t FreeGroupIndex() const;
  void FreeGroup(size_t g);
  bool GroupHasWaiters(const JupiterGroup& group) const;
  void FailGroup(size_t g, bool transport_failure);
  void CommitGroup(size_t g);
  PageSubmit SubmitGroupPage(size_t g, size_t batch_index, uint64_t now);
  void CompleteGroupPage(size_t batch_index, const providers::ProviderExecutionResult& result,
                                                size_t body_length, bool transport_ok);
 private:
  friend struct SpotRuntime;
  friend class SpotProviderOwner;
  friend class FlightTable;
  friend class SpotBatchScheduler;
  SpotRuntime& runtime_;
  JupiterGroup groups[kMaxJupiterGroups]{};
};

class RobinhoodSpotBatch {
 public:
  using FlightState = SpotTypes::FlightState;
  using FlightWaiter = SpotTypes::FlightWaiter;
  using SharedFlight = SpotTypes::SharedFlight;
  using SharedBatch = SpotTypes::SharedBatch;
  using RobinhoodQuoteCacheEntry = SpotTypes::RobinhoodQuoteCacheEntry;
  using RobinhoodGroup = SpotTypes::RobinhoodGroup;
  static constexpr size_t kMaxRobinhoodGroups = SpotTypes::kMaxRobinhoodGroups;
  static constexpr size_t kRobinhoodQuoteConcurrency = SpotTypes::kRobinhoodQuoteConcurrency;
  explicit RobinhoodSpotBatch(SpotRuntime& runtime) : runtime_(runtime) {}
  RobinhoodSpotBatch(const RobinhoodSpotBatch&) = delete;
  RobinhoodSpotBatch& operator=(const RobinhoodSpotBatch&) = delete;
  size_t FreeRobinhoodGroupIndex() const;
  size_t FreeRobinhoodBatchIndex() const;
  const providers::RobinhoodAsset* RobinhoodAssetForFlight(size_t flight_index) const;
  bool RobinhoodCachedQuote(const char* address, int64_t now, double* price) const;
  void SaveRobinhoodQuote(const char* address, double price, int64_t now);
  void SubmitRobinhoodQuote(size_t group_index, size_t position, size_t batch_index,
                                                   const char* path, bool catalog_request);
  void FailRobinhoodGroup(size_t group_index, bool transport_failure);
  void CommitRobinhoodGroup(size_t group_index);
  void AdvanceRobinhoodGroup(size_t group_index);
  void CompleteRobinhoodBatch(SharedBatch& batch, size_t body_length, bool transport_ok);
  bool RobinhoodGroupHasWaiters(const RobinhoodGroup& group) const;
 private:
  friend struct SpotRuntime;
  friend class SpotProviderOwner;
  friend class FlightTable;
  friend class SpotBatchScheduler;
  SpotRuntime& runtime_;
  providers::RobinhoodAssetCatalog robinhood_catalog{};
  int64_t robinhood_catalog_until_ms = 0;
  fortis::base::Mutex robinhood_shared_mutex;
  const SpotRobinhoodCatalog* robinhood_shared = nullptr;
  providers::RobinhoodAssetCatalog robinhood_shared_scratch{};
  RobinhoodQuoteCacheEntry robinhood_quote_cache[providers::kRobinhoodMaxAssets]{};
  RobinhoodGroup robinhood_groups[kMaxRobinhoodGroups]{};
};

struct SpotRuntime {
  using SlotState = SpotTypes::SlotState;
  using FlightState = SpotTypes::FlightState;
  using FlightWaiter = SpotTypes::FlightWaiter;
  using SharedFlight = SpotTypes::SharedFlight;
  using Slot = SpotTypes::Slot;
  using SharedBatch = SpotTypes::SharedBatch;
  using JupiterGroupQuote = SpotTypes::JupiterGroupQuote;
  using RobinhoodQuoteCacheEntry = SpotTypes::RobinhoodQuoteCacheEntry;
  using RobinhoodGroup = SpotTypes::RobinhoodGroup;
  using JupiterGroup = SpotTypes::JupiterGroup;
  using PageSubmit = SpotTypes::PageSubmit;
  using CompletionHandle = SpotTypes::CompletionHandle;
  static constexpr size_t kMaxRobinhoodGroups = SpotTypes::kMaxRobinhoodGroups;
  static constexpr size_t kRobinhoodQuoteConcurrency = SpotTypes::kRobinhoodQuoteConcurrency;
  // The coin popularity tracker foreground submits record into (or null).
  fortis::discovery::CoinPopularity* popularity = nullptr;
  fortis::discovery::CoinPopularity* contract_popularity = nullptr;

  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable changed;
  fortis::base::Thread owner_thread;
  bool owner_started = false;
  bool stop_requested = false;
  bool shutdown_started = false;
  bool workers_joined = false;
  bool owner_exited = false;
  SpotProviderOwnerStorageStats storage_stats{};
  // CoinGecko's anonymous token-price endpoint may reject multiple contract
  // addresses with HTTP 400. Keep batching until it does, then use one
  // address per request for the rest of this process.
  config::ProviderRuntimeConfig config{};
  fortis::prices::PriceSpotCache& cache;
  fortis::prices::PriceSeriesStore& series;
  providers::ProviderExecutionCoordinator* coordinator = nullptr;
  uint64_t coordinator_owner_id = 0;
  base::Clock& monotonic_clock;
  providers::ProviderLaneRegistry lanes;
  std::unique_ptr<providers::ProviderExecutionAdapter, fortis::base::ObjectDeleter<providers::ProviderExecutionAdapter>> adapter;
  codec::BoundedJsonToken json_tokens[codec::kBoundedJsonMaxTokens]{};
  // A shared catalog (SetRobinhoodCatalog) replaces the two above; the
  // mutex covers every call into it. `robinhood_shared_scratch` receives a
  // shared catalog before it replaces robinhood_catalog.
  uint32_t key_index[codec::kBoundedJsonMaxTokens]{};
  // Every spot request builder caps its body at kBoundedJsonMaxInputBytes.
  unsigned char response_body[providers::kProviderExecutionDefaultResponseBody]{};
  static_assert(codec::kBoundedJsonMaxInputBytes <= providers::kProviderExecutionDefaultResponseBody,
                "spot provider bodies must fit the default response buffer");
  http::OutboundRequest scratch_request{};
  // Off in production (see upstream_redirect.h).
  http::UpstreamRedirect redirect;
  // The wall clock: cache ages, answer timestamps and catalog freshness.
  fortis::base::Clock* clock = &fortis::base::SystemClock();
  base::ErrorReporter* errors = nullptr;
  uint64_t MonotonicMs() const { return monotonic_clock.MonotonicMs(); }
  int64_t wall_ms() const { return clock->NowMs(); }

  SpotRuntime(fortis::prices::PriceSpotCache& price_cache, fortis::prices::PriceSeriesStore& price_series,
       const config::ProviderRuntimeConfig& native_config,
       providers::ProviderExecutionCoordinator* shared_coordinator, base::Clock& monotonic);
  ~SpotRuntime();
  static void WakeOwner(void* opaque);
  size_t ProviderLiveJobCount() const;
  uint64_t ProviderNextWakeupMs();
  void ProviderPoll() { if (!coordinator) adapter->Poll(); }
  bool ProviderCancel(uint64_t job_id);
  providers::ProviderExecutionSubmission ProviderSubmit(const char* lane,
      const http::OutboundRequest& request, void* context,
      const providers::ProviderExecutionOptions& options);
  providers::ProviderExecutionTakeStatus ProviderTakeResult(providers::ProviderExecutionResult* result);
  void RecordStorageFailures(bool cache_failed, bool series_failed);
  static void* OwnerEntry(void* opaque);
  bool Start();



































  void DrainResults(bool);


  void BeginShutdown();
  void Wait(bool stopping);
  void OwnerLoop();

  SpotRequestProcessor requests{*this};
  FlightTable flight_table{*this};
  SpotBatchScheduler batches{*this};
  JupiterSpotBatch jupiter{*this};
  RobinhoodSpotBatch robinhood{*this};
};

}  // namespace fortis::prices
