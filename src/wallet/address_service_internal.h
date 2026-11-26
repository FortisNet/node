#include <memory>
#include "threading.h"
#pragma once

// Internal to the address plane's service and route units (address_service.cpp,
// address_route_support.cpp, address_account_routes.cpp,
// address_transfers_route.cpp, address_network_routes.cpp): the service
// implementation's layout, the queued task and the request/reply helpers the
// route handlers share. Nothing outside those units includes this.
#include "address_service.h"
#include "ownership.h"

#include "address_facets_internal.h"
#include "wallet_price_access.h"
#include "address_jobs.h"
#include "address_overview.h"
#include "chain_transport.h"
#include "device_auth.h"
#include "device_enrollment.h"
#include "discovery_cache.h"
#include "kv_store.h"
#include "news_routes.h"
#include "news_store.h"
#include "price_history_batch_codec.h"
#include "price_history_service.h"
#include "price_series_store.h"
#include "sync_route.h"
#include "provider_execution_coordinator.h"
#include "token_swr.h"
#include "provider_text.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::wallet {

using fortis::wallet::address::Outcome;
using fortis::wallet::address::FacetReply;
using fortis::wallet::address::Revalidation;
using codec::tj::Value;

namespace address_service_internal {

constexpr size_t kForegroundWorkers = 3;
constexpr size_t kQueueSlots = 32;
constexpr size_t kRevalidationSlots = 64;
constexpr size_t kIngestSlots = 16;
constexpr uint32_t kHandoffTimeoutMs = 30000;  // the handoff maximum
constexpr size_t kMaxTokens = 200;
constexpr size_t kQueryBytes = 16 * 1024;
constexpr const char* kJsonType = "application/json; charset=utf-8";
const char* const kUpstreamUnavailable = "{\"error\":\"UPSTREAM_UNAVAILABLE\"}";

const char* const kBusy =
    "{\"error\":\"RATE_LIMITED\",\"detail\":\"Upstream data is temporarily busy; retained data remains available.\"}";

struct QueryValue {
  size_t count = 0;
  char* value = nullptr;  // the first value, decoded (allocated)
  size_t length = 0;
  bool too_long = false;
  std::unique_ptr<char[], base::StorageDeleter> storage;
};

bool query_value(const char* target, const char* name, QueryValue* out);
bool string_value(const QueryValue& q, size_t max_units);
bool coerce_int(const QueryValue& q, double lo, double hi, double* out);
void send_text(int client, int status, const char* reason, const char* body, bool head, int cost, uint32_t remaining);
size_t error_body(const char* error, const char* detail, char* out, size_t capacity);
void send_error(int client, int status, const char* reason, const char* error, const char* detail, bool head, int cost,
                uint32_t remaining);
bool reply_policy(int client, const auth::DevicePolicyResult& policy, bool head);
bool reject_long_params(int client, const http::HttpRequest& request, bool head);
bool reject_raw_non_ascii(int client, const http::HttpRequest& request, bool head);
bool path_param(const http::HttpRequest& request, const char* name, size_t max_units, char* out,
                size_t capacity);

}  // namespace address_service_internal
using namespace address_service_internal;

// ============================================================================
// Service internals
// ============================================================================

struct AddressTask {
  enum Kind : uint8_t { kBalances, kInsight, kNfts, kHoldings, kTransfers, kTx, kView, kOverview };
  Kind kind = kBalances;
  uint64_t token = 0;
  bool head = false;
  bool protobuf = false;
  bool has_if_none_match = false;
  char if_none_match[160]{};
  int cost = 1;
  uint32_t remaining = 0;
  char device_id[27]{};
  char network[260]{};
  char subject[600]{};  // address or hash
  std::unique_ptr<char*[], base::CStringArrayDeleter> tokens;
  size_t token_count = 0;
  fortis::wallet::address::TransferQuery query{};
  char since[200]{};
  char before[2400]{};
  char token_id[600]{};
  char price_id[420]{};
  char flow[16]{};
};

void free_task(AddressTask* task);

struct IngestJob {
  char account_id[fortis::wallet::address::kAccountIdBytes + 1]{};
  fortis::chains::Network network{};
  char address[640]{};
  int64_t max_age = 0;
  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable done;
  bool finished = false;
  bool abandoned = false;
};

struct AddressService::Impl {
  void HandleInsight(int client, const http::HttpRequest& request, bool head);
  void HandleNfts(int client, const http::HttpRequest& request, bool head);
  void HandleHoldings(int client, const http::HttpRequest& request, bool head);
  void HandleBalances(int client, const http::HttpRequest& request, bool head);
  void HandleTx(int client, const http::HttpRequest& request, bool head);
  void HandleView(int client, const http::HttpRequest& request, bool head);
  void HandleOverview(int client, const http::HttpRequest& request, bool head);
  void HandleTransfers(int client, const http::HttpRequest& request, bool head);

  config::ProviderRuntimeConfig config{};
  fortis::storage::KvStore* kv = nullptr;
  fortis::chains::Catalog catalog;
  fortis::chains::Transport transport;
  fortis::prices::PriceSeriesStore series;
  address::WalletPriceAccess price_access;
  fortis::wallet::address::Store store;
  fortis::tokens::token_swr::Seams swr_seams{};
  fortis::tokens::token_swr::Cache swr;
  fortis::tokens::token_swr::CacheClock swr_clock{&swr};  // Impl::Now
  fortis::wallet::address::ChangeTracker changes;
  fortis::wallet::address::ChangeDetector detector;
  std::unique_ptr<fortis::wallet::address::Facets, fortis::base::ObjectDeleter<fortis::wallet::address::Facets>> facets;
  fortis::wallet::address::FacetDeps deps{};
  prices::SpotProviderOwner* spot = nullptr;
  prices::PriceHistoryService* history = nullptr;
  AddressServiceSeams seams{};
  http::HttpResponseHandoff* handoff = nullptr;
  bool attestation_required = false;
  // POST /v1/sync over this store's journal and watches.
  fortis::wallet::sync::Route sync;

  // The overview's news reader (its own cache instance on the same KV).
  fortis::base::Mutex news_mutex;
  fortis::base::Buffer news_scratch;
  fortis::base::Buffer news_arena;
  fortis::base::Buffer news_output;
  discovery::DiscoveryCacheSeams news_seams{};
  std::unique_ptr<discovery::DiscoveryCache, fortis::base::ObjectDeleter<discovery::DiscoveryCache>> news_cache;
  // One response buffer per foreground worker (Complete copies the body).
  fortis::base::Buffer responses[kForegroundWorkers];
  bool response_busy[kForegroundWorkers]{};

  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable changed;
  bool stopping = false;
  AddressTask* tasks[kQueueSlots]{};
  size_t task_head = 0, task_count = 0;
  Revalidation* revalidations[kRevalidationSlots]{};
  size_t revalidation_head = 0, revalidation_count = 0;
  bool revalidating = false;
  IngestJob* ingests[kIngestSlots]{};
  size_t ingest_head = 0, ingest_count = 0;
  bool ingesting = false;
  fortis::base::Thread workers[kForegroundWorkers];
  size_t started_workers = 0;
  fortis::base::Thread background;
  bool background_started = false;
  fortis::base::Thread ingest_thread;
  bool ingest_started = false;
  fortis::base::Thread scheduler;
  bool scheduler_started = false;
  fortis::base::Mutex job_mutex;

  Impl(const config::ProviderRuntimeConfig& cfg, fortis::storage::KvStore* store_kv, providers::ProviderExecutionCoordinator* coordinator,
       prices::SpotProviderOwner* spot_owner, prices::PriceHistoryService* history_service, const AddressServiceSeams* test_seams);
  ~Impl();

  int64_t Now() const { return swr.Now(); }

  bool Init();
  static bool NewsFeed(void* context, int64_t now_ms, codec::tj::Arena& arena, Value** payload);
  static bool EnqueueRevalidation(void* context, Revalidation* job);
  static void* BackgroundMain(void* opaque);
  static bool IngestAsync(void* context, const char* account_id, const fortis::chains::Network& network,
                          const char* address, int64_t max_age, uint64_t wait_ms);
  static void FinishIngest(IngestJob* job);
  static void* IngestMain(void* opaque);
  bool Submit(AddressTask* task);
  static void* WorkerMain(void* opaque);
  void Complete(const AddressTask& task, const http::HttpResponse& response);
  void CompleteText(const AddressTask& task, int status, const char* reason, const char* body);
  void CompleteError(const AddressTask& task, const FacetReply& result);
  uint8_t* AcquireResponse();
  void ReleaseResponse(uint8_t* body);
  void CompleteStructured(const AddressTask& task, const Value* payload);
  void CompleteOverview(const AddressTask& task, const Value* payload);
  void Run(AddressTask* task);
  static bool History(void* context, const char* price_id, int64_t from, int64_t to, bool hourly);
  bool RunJob(Job job, char* summary, size_t capacity);
  double JitterSample() const;

  struct JobSlot {
    Job job;
    int64_t interval_ms;
    double jitter_pct;
    int64_t initial_ms;  // -1 = random within the interval
    uint64_t due_ms;
  };

  static void* SchedulerMain(void* opaque);
  bool StartThreads();
  void StopThreads();
};

namespace address_service_internal {

// ---- handlers -----------------------------------------------------------------------

AddressTask* new_task(AddressTask::Kind kind, const http::HttpRequest& request, const auth::DevicePolicyResult& policy);
void dispatch(AddressService::Impl& owner, int client, bool head, AddressTask* task);
void bad_request(int client, bool head, const auth::DevicePolicyResult& policy, const char* detail = nullptr);
bool copy_into(char* out, size_t capacity, const char* text);

}  // namespace address_service_internal

}  // namespace fortis::wallet
