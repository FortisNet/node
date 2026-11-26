#pragma once

// Internal to the Robinhood service units (robinhood_service.cpp,
// robinhood_routes.cpp, robinhood_write_requests.cpp): the service
// implementation's layout and the reply helpers its HTTP-worker and job sides
// share. Nothing outside those units includes this.
#include <memory>
#include "robinhood_service.h"
#include "ownership.h"
#include "threading.h"

#include "chain_core.h"
#include "chain_transport.h"
#include "cross_chain_support.h"
#include "device_auth.h"
#include "device_enrollment.h"
#include "device_policy_step.h"
#include "discovery_route_support.h"
#include "http_middleware.h"
#include "js_json.h"
#include "proto_wire.h"
#include "robinhood_markets.h"
#include "robinhood_policy.h"
#include "robinhood_tickets.h"
#include "token_json.h"
#include "token_text.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::exchange {

namespace robinhood_service_internal {

using codec::tj::OutBuffer;
using codec::tj::Value;
constexpr const char* kJsonType = http::kHttpJsonUtf8;
constexpr const char* kProtobufType = "application/protobuf";
constexpr uint32_t kHandoffTimeoutMs = 30000;
constexpr uint32_t kProviderDeadlineMs = 25000;
constexpr size_t kJobArenaBytes = 24 * 1024 * 1024;

uint64_t monotonic_ms();

// ---- replies ------------------------------------------------------------------------------

using Policy = auth::DeviceQuota;

http::HttpResponse json_response(int status, const char* reason, const char* body, const Policy& policy);
http::HttpResponse bad_request(const Policy& policy);
http::HttpResponse sponsored_unavailable(const Policy& policy);
http::HttpResponse back_pressure(uint32_t retry_after_ms, const Policy& policy);
bool address_text(const char* t, size_t length);

}  // namespace robinhood_service_internal
using namespace robinhood_service_internal;

// ---- the service -------------------------------------------------------------------------------

struct RobinhoodService::Impl {
  enum class Kind : uint8_t { kCatalog, kInstrument, kDiagnose, kPrepare, kSubmit };
  struct Job {
    bool used = false;
    Kind kind = Kind::kCatalog;
    bool protobuf = false;
    bool head = false;
    uint64_t token = 0;
    uint64_t deadline = 0;
    Policy policy;
    codec::tj::Arena arena{};
    char address[43]{};
    // The StockClient generations when the request was accepted (see
    // StockClient::Assets): the catalog's, or the instrument's three.
    uint64_t assets_issued = robinhood::StockClient::kNotIssued;
    uint64_t quote_issued = robinhood::StockClient::kNotIssued;
    uint64_t actions_issued = robinhood::StockClient::kNotIssued;
    const Value* input = nullptr;
    bool sponsored = false;
    char device_id[27]{};
    robinhood::TicketStore::Claim claim;
    bool claimed = false;
  };

  RobinhoodConfig config{};
  RobinhoodServiceSeams seams{};
  fortis::storage::KvStore* kv = nullptr;
  std::unique_ptr<fortis::chains::Transport, fortis::base::ObjectDeleter<fortis::chains::Transport>> transport;
  robinhood::StockClient client;
  robinhood::TicketStore tickets;
  http::HttpResponseHandoff* handoff = nullptr;
  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable changed;
  fortis::base::ConditionVariable idle;
  // One companion per worker: it runs an instrument's corporate-action read
  // while its worker reads the quote (Promise.all), and answers the request
  // when its read fails first. It touches only the StockClient (its own
  // scratch and snapshot arenas) and the job's immutable token, policy and
  // deadline; the job arena stays the worker's.
  struct Side {
    Impl* impl = nullptr;
    fortis::base::Thread thread;
    fortis::base::Mutex mutex;
    fortis::base::ConditionVariable changed;
    bool stop = false;
    bool pending = false;   // a read is posted
    bool done = false;      // the posted read finished
    bool answered = false;  // the job's response is completed (first failure)
    Job* job = nullptr;
    std::shared_ptr<const robinhood::StockClient::Snapshot> held = nullptr;
  };
  fortis::base::Thread workers[kWorkers];
  size_t started_workers = 0;
  Side sides[kWorkers];
  size_t started_sides = 0;
  bool stopping = false;
  Job jobs[kJobs]{};
  Job* queue[kJobs]{};
  size_t queue_head = 0, queue_count = 0;
  bool diagnostics_enabled = false;
  bool sponsored_enabled = false;
  char wallet_path[300]{};
  char simulation_path[300]{};
  char capabilities_json[256]{};
  size_t capabilities_json_length = 0;
  uint8_t capabilities_proto[128]{};
  size_t capabilities_proto_length = 0;

  // The seam clock, else the system clock (a normal composition): the routes,
  // the tickets and the stock client's caches all read this one.
  fortis::base::Clock& Clock() const { return seams.clock ? *seams.clock : fortis::base::SystemClock(); }
  int64_t Now() const { return Clock().NowMs(); }

  bool BuildCapabilities();
  void HandleCatalog(const http::HttpRequest& request, http::HttpReply& reply);
  void HandleInstrument(const http::HttpRequest& request, http::HttpReply& reply);
  void HandleCapabilities(const http::HttpRequest& request, http::HttpReply& reply);
  void HandleDiagnose(const http::HttpRequest& request, http::HttpReply& reply);
  void HandlePrepare(const http::HttpRequest& request, http::HttpReply& reply);
  void HandleSubmit(const http::HttpRequest& request, http::HttpReply& reply);
  void Capabilities(const http::HttpRequest& request, http::HttpReply& reply);
  void Read(const http::HttpRequest& request, http::HttpReply& reply, Kind kind);
  void Write(const http::HttpRequest& request, http::HttpReply& reply, Kind kind);
  Job* Reserve(http::HttpReply& reply, const Policy& policy);
  void Dispatch(http::HttpReply& reply, Job* job);
  void Release(Job* job);
  static void* WorkerMain(void* opaque);
  static void* SideMain(void* opaque);
  void SideLoop(Side& side);
  void WorkerLoop(Side& side);

  void Complete(Job* job, const http::HttpResponse& response) { handoff->Complete(job->token, response); }

  void Fail(Job* job, const robinhood::Outcome& outcome, bool facet);

  using Encoder = bool (*)(OutBuffer& out, const Value* v);

  void Respond(Job* job, const Value* value, Encoder encode);
  robinhood::MarketsRequest Markets(const Job* job);
  void RunCatalog(Job* job);
  void RunInstrument(Job* job, Side& side);
  robinhood::Outcome ActiveStocks(Job* job, std::shared_ptr<const robinhood::StockClient::Snapshot>* held, robinhood::StockSet* stocks);
  robinhood::Outcome Rpc(Job* job, const char* base, const char* path, const char* lane, const char* method, Value* params,
                  Value** body);
  void RunDiagnose(Job* job);
  void RunPrepare(Job* job);
  void RunSubmit(Job* job);
  bool WaitIdle(uint32_t timeout_ms);
};

}  // namespace fortis::exchange
