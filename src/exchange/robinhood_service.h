#pragma once

#include <memory>
#include "status.h"

#include "clock.h"
#include "ownership.h"
#include "http_server.h"
#include "exchange_config.h"
#include "kv_store.h"
#include "seams.h"
#include "token_json.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::providers { class ProviderExecutionCoordinator; }
namespace fortis::chains { class Transport; }

namespace fortis::exchange {

// Test seams. A normal composition passes none: the wall clock, a transport
// on the provider hosts with the validated CA bundle and CSPRNG tickets.
struct RobinhoodServiceSeams {
  // The wall clock (NowMs) of the routes, the tickets and the stock client.
  fortis::base::Clock* clock = nullptr;
  // Starts the transport instead of Transport::Start (a local fixture host).
  fortis::base::Starter<fortis::chains::Transport>* transport_starter = nullptr;
  // The 24 random bytes of a ticket (randomBytes(24)).
  fortis::base::RandomSource* ticket_random = nullptr;
  // Replaces the 25-second provider operation deadline.
  uint32_t provider_deadline_ms = 0;
};

// Robinhood stock tokens and sponsored execution behind
// FORTIS_NATIVE_ROBINHOOD_CANARY=1:
//   GET  /v1/networks/robinhood/stock-tokens                      (cost 1)
//   GET  /v1/networks/robinhood/stock-tokens/{address}            (cost 1)
//   GET  /v1/networks/robinhood/sponsored-trades/capabilities     (cost 1)
//   POST /v1/networks/robinhood/transactions/diagnose             (cost 20)
//   POST /v1/networks/robinhood/sponsored-trades/prepare          (cost 20)
//   POST /v1/networks/robinhood/sponsored-trades/submit           (cost 20)
// Body parsing, the device hook, protobuf decoding, the zod schemas, the
// unconfigured 503s and the ticket claim run on the HTTP worker; every
// upstream read runs as one task on kWorkers threads and answers through the
// response handoff (30 s), with a 25 s provider deadline.
//
// ALCHEMY_API_KEY enables diagnostics (eth_simulateV1 and the optional
// debug_traceCallMany on robinhood-mainnet.g.alchemy.com) and, with
// ALCHEMY_GAS_POLICY_ID, sponsored trades (wallet_prepareCalls /
// wallet_sendPreparedCalls on api.g.alchemy.com). The key and the policy id
// stay server-side; the service never holds a wallet key or signs: prepare
// returns Alchemy's signature requests and submit forwards the wallet's
// signatures for the exact prepared operation bound to its device ticket.
// Tickets are process memory (two minutes, at most 1,000 pending); a restart
// drops them as in TypeScript.
// The pure steps of the three POST handlers (R43–R45), exposed for
// tests/robinhood_writes_test.cpp; the handlers run exactly these.
enum class RobinhoodWriteKind : uint8_t { kDiagnose, kPrepare, kSubmit };
// fromBinary(RobinhoodDiagnoseRequest | RobinhoodPrepareRequest |
// RobinhoodSubmitRequest) mapped to the object the route validates: {from,
// calls[, sponsored]} or {ticket, signatures}. Null where protobuf-es throws.
fortis::codec::tj::Value* RobinhoodDecodeWriteRequest(RobinhoodWriteKind kind, const uint8_t* body, size_t length,
                                               fortis::codec::tj::Arena& arena);
// The route's zod schema (diagnosticsBody, prepareBody, submitBody, each
// .strict()); `*sponsored` is diagnosticsBody's default-false flag.
bool RobinhoodValidWriteRequest(RobinhoodWriteKind kind, const fortis::codec::tj::Value* input, bool* sponsored);
// {jsonrpc:'2.0', id:1, method, params}: the body of every Alchemy request.
fortis::codec::tj::Value* RobinhoodRpcEnvelope(fortis::codec::tj::Arena& arena, const char* method, fortis::codec::tj::Value* params);
// wallet_prepareCalls: [{from, chainId:'0x1237', calls, capabilities:{paymasterService:{policyId}}}].
fortis::codec::tj::Value* RobinhoodPrepareParams(fortis::codec::tj::Arena& arena, fortis::codec::tj::Value* from_lower,
                                          fortis::codec::tj::Value* executable, const char* policy_id);
// eth_simulateV1: [{blockStateCalls:[{calls}], validation: !sponsored, traceTransfers: true}].
fortis::codec::tj::Value* RobinhoodSimulateParams(fortis::codec::tj::Arena& arena, fortis::codec::tj::Value* simulation_calls,
                                           bool sponsored);
// debug_traceCallMany: [[{transactions: calls}], {blockNumber:'latest', transactionIndex:-1}, {tracer:'callTracer'}].
fortis::codec::tj::Value* RobinhoodTraceParams(fortis::codec::tj::Arena& arena, fortis::codec::tj::Value* simulation_calls);
// prepare()'s answer: {ticket, expiresAt, prepared, signatureRequests}.
fortis::codec::tj::Value* RobinhoodPrepareResult(fortis::codec::tj::Arena& arena, const char* ticket, const char* expires_iso,
                                          fortis::codec::tj::Value* prepared, fortis::codec::tj::Value* requests);
// randomBytes(24).toString('base64url'): 32 characters and a NUL.
bool RobinhoodTicketText(const uint8_t random[24], char out[33]);
// sendProtobufOrJson's protobuf branch: RobinhoodDynamicResponse{payload:
// Struct}; nothing for a null payload, an empty Struct for a non-object.
bool RobinhoodEncodeDynamic(fortis::codec::tj::OutBuffer& out, const fortis::codec::tj::Value* payload);

struct RobinhoodServiceDeps {
  providers::ProviderExecutionCoordinator& coordinator;
  storage::KvStore* kv = nullptr;
};

class RobinhoodService {
 public:
  static constexpr size_t kWorkers = 4;
  static constexpr size_t kJobs = 8;

  RobinhoodService(RobinhoodService&&) noexcept;
  RobinhoodService& operator=(RobinhoodService&&) = delete;
  ~RobinhoodService();
  RobinhoodService(const RobinhoodService&) = delete;
  RobinhoodService& operator=(const RobinhoodService&) = delete;

  static base::Result<RobinhoodService> Create(const RobinhoodConfig& config, RobinhoodServiceDeps deps,
                                      const RobinhoodServiceSeams* seams = nullptr);
  bool Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff);
  // Waits until no job is queued or running (tests).
  bool WaitIdle(uint32_t timeout_ms);

  struct Impl;

 private:
  explicit RobinhoodService(Impl* impl);
  void Stop();
  // Defined beside Impl: only that unit can destroy one.
  struct ImplDeleter { void operator()(Impl* impl) const; };
  std::unique_ptr<Impl, ImplDeleter> impl_;
};

}  // namespace fortis::exchange
