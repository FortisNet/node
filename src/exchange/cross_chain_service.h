#pragma once

#include <memory>
#include "status.h"

#include "clock.h"
#include "ownership.h"
#include "cross_chain_providers.h"
#include "exchange_config.h"
#include "http_server.h"
#include "seams.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::providers { class ProviderExecutionCoordinator; }
namespace fortis::chains { class Transport; }

// ---- GET /v1/exchange/cross-chain/providers -------------------------------
//
// What it is. The wallet's directory of cross-chain routing services and
// what this deployment can do with each (TypeScript: getCrossChainProviders
// over CrossChainRouter.providers() and each adapter's descriptor()). The
// Flutter app (IntelligenceApi.crossChainProviders) always asks for protobuf.
//
// Method and path. `GET` exactly; the router answers `HEAD` with the same
// handler: GET's status and headers, including content-length, without a
// body. Other methods, `/providers/`, `/PROVIDERS` and `%70roviders` are
// router 404s (Fastify decodes `%70` and serves the last one: router-wide).
// The query string is ignored entirely and is never echoed.
//
// Hooks, in TypeScript's /v1 order. Any Origin header (even `null`) → 403
// BROWSER_ORIGIN_FORBIDDEN; then a `Bearer ` (case-sensitive) device token
// that authenticates, else 401 UNAUTHORIZED / SESSION_EXPIRED /
// ATTESTATION_REQUIRED; then the device quota at cost 1 (premium 1200 per
// wall-clock minute) → 429 RATE_LIMITED with retry-after: 60. Error bodies
// are always JSON, whatever Accept says, and carry no vary header.
//
// Configuration. The answer is a pure function of process configuration,
// encoded once by Start() (EncodeProviderDirectory) and served from two
// immutable buffers; nothing changes until a restart. Six providers, always
// all six, sorted by id (localeCompare; byte order for these ASCII ids):
//   id            name                 custody        configured when
//   bridge-vet    VeChain Bridge       custodial      always
//   lifi          LI.FI                non-custodial  LIFI_API_KEY set
//   maya          MAYAChain            non-custodial  always
//   near-intents  NEAR Intents 1Click  custodial      always (the JWT only
//                                                    picks a fee tier)
//   simpleswap    SimpleSwap           custodial      SIMPLESWAP_API_KEY set
//   squid         Squid                non-custodial  SQUID_INTEGRATOR_ID set
// "Set" means non-empty after trimming space, tab, CR and LF. Capabilities
// are {quote, execution, status} = all true, except MAYA, whose quote and
// execution are both MAYA_DASH_EXECUTION_ENABLED=true with at least two
// MAYA_NODE_URLS of distinct WHATWG origins (LoadProviderSettings). `status`
// is always true. A credential never changes a capability; `configured`
// false still lists the provider. websiteUrl is the trusted constant from
// the provider directory. No credential, header value or MAYA URL is ever
// part of the answer.
//
// Wire shape. JSON (default): {"providers":[{id,name,websiteUrl,configured,
// custody,capabilities:{quote,execution,status}}...],"cache":{"hit":false,
// "timestamp":null}} in that key order, application/json; charset=utf-8.
// Protobuf when acceptsProtobuf() holds (an `application/protobuf` entry,
// case-insensitive, whose first q= parameter is absent or > 0):
// CrossChainProvidersResponse{repeated CrossChainProvider providers = 1;
// CacheInfo cache = 2}. proto3 implicit presence: false booleans are
// omitted, so an unconfigured provider has no field 4 and MAYA without
// execution has capabilities {status} only; capabilities is always present.
// cache is present and empty (hit false, no timestamp, no partial), which
// the app reads as {hit: false, timestamp: null}. 200 answers carry
// vary: Accept, cache-control: no-store, x-rate-limit-cost: 1 and
// x-rate-limit-remaining. Largest bodies: 1137 bytes JSON (kDirectoryJsonBytes
// 2048), 432 bytes protobuf (kDirectoryWireBytes 1024).
//
// Cache metadata. The directory is never a cache read: hit is false and
// timestamp null on every answer, as in TypeScript.
//
// No upstream work. The handler runs on the HTTP worker, touches no provider,
// no transport, no KV other than the device hook, and takes no lock: the
// buffers are written before Register() and only read afterwards, so any
// number of concurrent reads see the same bytes.
//
// Startup. TypeScript's loadConfig refuses the process for an invalid
// MAYA_NODE_URLS entry, or for execution without two distinct origins.
// NativeConfig checks the text; LoadProviderSettings repeats the check on
// WHATWG origins and URL parsing, so Start() fails (the process exits 2)
// instead of reporting a silently disabled MAYA.
//
// Intentional differences. (1) Startup is stricter: NativeConfig refuses
// credentials containing other controls or non-ASCII after the trim (JS trim
// would strip \v, \f or NBSP and could leave the provider unconfigured),
// MAYA_NODE_URLS that are not literal lower-case https:// or carry ?, # or
// userinfo, and a NEAR_INTENTS_JWT over 248 bytes. (2) Router-wide: Accept
// q values are read with strtod and the media type is trimmed (q=inf and
// `application/protobuf ;q=1` give protobuf, q=0b1 JSON; TypeScript the
// reverse), `%70roviders` is 404, 404 bodies differ, every response has
// Connection: close, and HTTP admission holds 4 workers + 8 pending
// connections (main.cpp), so a 13th simultaneous connect is closed without
// a response where Node would queue it.
// Quota counters are process memory in both, so a restart resets them.
namespace fortis::exchange::cross_chain {
struct ProviderSettings;
constexpr size_t kDirectoryJsonBytes = 2048;
constexpr size_t kDirectoryWireBytes = 1024;
// The provider settings of `config` (credentials, MAYA opt-in and roots).
// False when TypeScript's loadConfig would refuse the MAYA configuration.
bool LoadProviderSettings(const CrossChainConfig& config, ProviderSettings* out);
// The GET /providers JSON text and protobuf wire for `settings`. False (and
// both lengths untouched) when either buffer is too small.
bool EncodeProviderDirectory(const ProviderSettings& settings, char* json, size_t json_capacity,
                             size_t* json_length, uint8_t* wire, size_t wire_capacity, size_t* wire_length);
}  // namespace fortis::exchange::cross_chain

// ---- POST /quotes and POST /status: the handlers' pure steps --------------
//
// Each route handler (contract comments above Impl::Quotes and
// Impl::StatusRoute in cross_chain_service.cpp) runs these after the body
// parser and the device hook; tests/cross_chain_post_test.cpp checks them
// against the TypeScript oracles (cross_chain_{quotes,status}_edges.json).

namespace fortis::exchange {

// The parsed body a JSON request hands the route (nullptr when it does not
// parse).
const fortis::codec::tj::Value* CrossChainRequestInput(const uint8_t* bytes, size_t length,
                                                fortis::codec::tj::Arena& arena);
// fromBinary(CrossChain{Quote,Status}RequestSchema) mapped to the object the
// route validates (protobuf-es 2.16 semantics); nullptr when it throws.
fortis::codec::tj::Value* CrossChainDecodeQuoteRequest(const uint8_t* body, size_t length, fortis::codec::tj::Arena& arena);
fortis::codec::tj::Value* CrossChainDecodeStatusRequest(const uint8_t* body, size_t length, fortis::codec::tj::Arena& arena);

enum class CrossChainPlanKind : uint8_t { kBadRequest, kUnknownProviders, kDispatch };
struct CrossChainQuotePlan {
  CrossChainPlanKind kind = CrossChainPlanKind::kBadRequest;
  fortis::exchange::cross_chain::QuoteRequest request;
  uint8_t providers[fortis::exchange::cross_chain::kProviderCount] = {};  // registration indices, request order
  size_t provider_count = 0;
  const fortis::codec::tj::Value* unknown[10] = {};  // unregistered ids, request order, de-duplicated
  size_t unknown_count = 0;
};
// quoteBody.safeParse(input), then CrossChainRouter.quote's provider selection.
void CrossChainPlanQuote(const fortis::codec::tj::Value* input, fortis::codec::tj::Arena& arena, CrossChainQuotePlan* plan);
struct CrossChainStatusPlan {
  CrossChainPlanKind kind = CrossChainPlanKind::kBadRequest;
  fortis::exchange::cross_chain::StatusRequest request;
  int provider = -1;  // registration index
};
// statusBody.safeParse(input) (with its bridge → bridgeId transform), then
// the router's provider lookup.
void CrossChainPlanStatus(const fortis::codec::tj::Value* input, fortis::codec::tj::Arena& arena, CrossChainStatusPlan* plan);
// sendProtobufOrJson's protobuf branch: quoteMessage / statusMessage over a
// result object, toBinary. False when protobuf-es would throw.
bool CrossChainEncodeQuoteResponse(const fortis::codec::tj::Value* result, fortis::codec::tj::OutBuffer& out);
bool CrossChainEncodeStatusResponse(const fortis::codec::tj::Value* result, fortis::codec::tj::OutBuffer& out);

// Test seams. A normal composition passes none: the wall clock, real sleeps
// (bridge.vet's order-readiness delays) and a transport on the provider
// hosts with the validated CA bundle.
struct CrossChainServiceSeams {
  // NowMs (the wall clock) and SleepMs (the order-readiness delays).
  fortis::base::Clock* clock = nullptr;
  // Starts the transport instead of Transport::Start (a local fixture host).
  fortis::base::Starter<fortis::chains::Transport>* transport_starter = nullptr;
  // Replaces the 25-second provider operation deadline.
  uint32_t provider_deadline_ms = 0;
};

// Cross-chain exchange behind FORTIS_NATIVE_CROSS_CHAIN_CANARY=1:
//   GET  /v1/exchange/cross-chain/providers  (cost 1; no upstream traffic)
//   POST /v1/exchange/cross-chain/quotes     (cost 10)
//   POST /v1/exchange/cross-chain/status     (cost 10)
// Body parsing, the device hook, protobuf decoding and the zod schemas run
// on the HTTP worker. A quote fans out to every selected provider as one
// task each on kWorkers threads (each provider fails independently and is
// reported in `attempts`); a status lookup is one task. Results are written
// through the response handoff, which holds a socket for at most 30 s, so
// every provider operation has a 25 s deadline. The six adapters reach their
// upstreams only through one chains::Transport lease of the process provider
// coordinator (lanes, breaker, bounded queue, retry policy, 512 KiB bodies).
// The service holds provider credentials only as request headers, never
// wallet keys, and never signs or broadcasts.
struct CrossChainServiceDeps {
  providers::ProviderExecutionCoordinator& coordinator;
};

class CrossChainService {
 public:
  static constexpr size_t kWorkers = 6;
  static constexpr size_t kJobs = 12;

  CrossChainService(CrossChainService&&) noexcept;
  CrossChainService& operator=(CrossChainService&&) = delete;
  ~CrossChainService();
  CrossChainService(const CrossChainService&) = delete;
  CrossChainService& operator=(const CrossChainService&) = delete;

  static base::Result<CrossChainService> Create(const CrossChainConfig& config, CrossChainServiceDeps deps,
                                      const CrossChainServiceSeams* seams = nullptr);
  bool Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff);
  // Waits until no job is queued or running (tests).
  bool WaitIdle(uint32_t timeout_ms);

  struct Impl;

 private:
  explicit CrossChainService(Impl* impl);
  void Stop();
  // Defined beside Impl: only that unit can destroy one.
  struct ImplDeleter { void operator()(Impl* impl) const; };
  std::unique_ptr<Impl, ImplDeleter> impl_;
};

}  // namespace fortis::exchange
