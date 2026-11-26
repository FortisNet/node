#pragma once

#include <memory>
#include "status.h"

#include "clock.h"
#include "ownership.h"
#include "dashpay_client.h"
#include "exchange_config.h"
#include "http_server.h"
#include "seams.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::providers { class ProviderExecutionCoordinator; }
namespace fortis::chains { class Transport; }

namespace fortis::exchange {

// Test seams. A normal composition passes none: the wall clock, a transport
// on the trusted quorum service and evonodes with the validated CA bundle,
// and the TypeScript deadlines.
// Rewrites a lookup before the route's own checks (the TypeScript contract's
// malformed-identity case); `identity` is the Base58 text. The base leaves
// the lookup as it is.
class DashPayLookupHook {
 public:
  virtual void AfterLookup(fortis::exchange::dashpay::Outcome* outcome, char* identity, size_t capacity) {
    (void)outcome; (void)identity; (void)capacity;
  }

 protected:
  constexpr DashPayLookupHook() = default;
  ~DashPayLookupHook() = default;
};

struct DashPayServiceSeams {
  // The wall clock (NowMs) of the freshness check.
  fortis::base::Clock* clock = nullptr;
  fortis::exchange::dashpay::NodeChoice* node_choice = nullptr;
  // Starts the transport instead of Transport::Start (a local fixture host).
  fortis::base::Starter<fortis::chains::Transport>* transport_starter = nullptr;
  DashPayLookupHook* lookup_hook = nullptr;
  uint32_t connect_ms = 0;
  uint32_t lookup_ms = 0;
  uint32_t request_ms = 0;
};

}  // namespace fortis::exchange

// The pure request/response pieces of the route (tests/dashpay_resolve_test.cpp).
namespace fortis::exchange::dashpay {

// zod's max(69) on the trimmed value: "@" + a 63-byte label + ".dash".
constexpr size_t kNameUnits = 69;
// The largest 200 body: the JSON form with a 63-byte label and a 50-char
// identity is 145 bytes.
constexpr size_t kNameResponseBytes = 160;

enum class NameQuery : uint8_t { kValid, kInvalid, kNoMemory };
// The raw request target (path + "?" + query, undecoded) → the normalized
// DPNS label (3..63 of [a-z0-9-], NUL-terminated, without ".dash"), or
// kInvalid for every input TypeScript answers 400 INVALID_DASHPAY_NAME.
// Allocates query length × 3 + 3 bytes for the call (kNoMemory if refused).
NameQuery ParseNameQuery(const char* raw_target, char label[kMaxLabelBytes + 1], size_t* label_length);
// validDashPlatformIdentifier: 32..50 Base58 characters decoding to 32 bytes.
bool ValidPlatformIdentifier(const char* text);
// The 200 body for "<label>.dash": JSON {"name","identityId"} (null on a
// verified absence) or the DashPayNameResponse wire (identity_id absent on a
// miss). `identity` is null for an absence. Returns the length, 0 if refused.
size_t EncodeNameResponse(const char* label, size_t label_length, const char* identity, bool protobuf, uint8_t* out,
                          size_t capacity);

}  // namespace fortis::exchange::dashpay

namespace fortis::exchange {

// GET /v1/dashpay/names/resolve?name=… — what DashPay name owns which Dash
// Platform identity, answered only from proof-verified Platform state.
// Native port of the TypeScript DashPay route and name resolver, behind
// FORTIS_NATIVE_DASHPAY_CANARY=1 (which requires FORTIS_CA_BUNDLE_PATH).
// The Flutter caller (IntelligenceApi.resolveDashPayName) always asks for
// protobuf, re-checks the canonical name and the identifier shape, and treats
// a missing identity as "no such name".
//
// Order of answers (HEAD runs the same handler and sends the GET status and
// headers, including content-length, with no body):
//  1. The /v1 device hook (AuthorizeDeviceRequest, cost 1): 403
//     BROWSER_ORIGIN_FORBIDDEN, 401 UNAUTHORIZED / SESSION_EXPIRED /
//     ATTESTATION_REQUIRED, then 429 RATE_LIMITED with Retry-After (per-
//     minute device quota by tier). A missing or invalid name never jumps
//     ahead of these, and every request past the hook costs 1, even a 400.
//  2. The query, on the HTTP worker (ParseNameQuery). The query is the
//     target after the first `?`, split as fast-querystring does: `&`
//     pairs, `+` → space, then all-or-nothing percent decoding (a malformed
//     escape or a non-UTF-8 result keeps the field's raw text). `name` must
//     occur exactly once (repeated → array → 400; a percent-encoded key such
//     as `n%61me` counts). The value is trimmed with JavaScript's trim()
//     (Unicode White_Space + LF/CR/LS/PS + BOM, not NEL/ZWSP), must then be
//     1..69 UTF-16 units, and must pass normalizeDashPayName: one optional
//     leading `@`, one optional case-insensitive `.dash` suffix, then a
//     label /^[a-zA-Z0-9][a-zA-Z0-9-]{1,61}[a-zA-Z0-9]$/ that is lowercased.
//     Anything else is 400 {"error":"INVALID_DASHPAY_NAME"} (JSON even when
//     protobuf is accepted). A raw byte >= 0x80 in the target is also 400
//     here: Node's parser refuses such a request line before routing.
//  3. The lookup, on one of kWorkers threads through the response handoff
//     (DashPayService answers the socket later; 30 s handoff). The client
//     (dashpay_client.h) connects a trusted context (quorum keys from the
//     Dash quorum service, the only trust anchor; evonodes from
//     /masternodes) and sends one getDocuments for the DPNS `domain`
//     document under parent "dash", verifies the GroveDB proof and the
//     quorum's BLS signature over the app hash (dashpay_proof.h), and checks
//     freshness (signed time within 31 min of the wall clock; height at most
//     one block below the highest this connection has seen). Up to three
//     evonodes are tried; a failing one is banned for a while.
//       200 {"name":"<label>.dash","identityId":"<Base58>"}  a proven document;
//           the identity is its owner and must be 32..50 Base58 chars
//           decoding to 32 bytes, else 503 (fail closed);
//       200 {"name":"<label>.dash","identityId":null}  a proven absence;
//       503 {"error":"DASHPAY_UNAVAILABLE"}  everything unproven: transport,
//           TLS, gRPC status, framing, proof, unknown quorum, signature,
//           stale metadata, deadline, shutdown, admission (all kJobs slots
//           in use) and out-of-memory. Always JSON.
//     The 200 name is the canonical query name (the SDK echoes it). With an
//     accepted application/protobuf the 200 is DashPayNameResponse
//     {name = 1, optional identity_id = 2} as application/protobuf, else
//     JSON.stringify bytes as application/json; charset=utf-8. A 200 adds
//     Vary: Accept; every answer carries the baseline security headers,
//     cache-control: no-store, x-rate-limit-cost/-remaining (after the
//     hook). No ETag/304, no caching of answers.
// Deadlines: connect 15 s and lookup 15 s (the TypeScript deadline()s), each
// gRPC request 12 s, all inside kLookupBudgetMs (28 s) from admission, so
// the handoff never times out first.
// Concurrency: kJobs (8) admitted lookups, kWorkers (2) running at once;
// TypeScript has no bound (each request awaits the SDK). A client that
// disconnects does not cancel its lookup; the answer is dropped.
// State: one trusted connection per process, kept across verified answers
// (found or absent) and dropped only by an unverified lookup of the same
// generation; nothing is persisted, so a restart reconnects. Device quota
// buckets are the only shared state the route touches. Names and identities
// are never logged; an unverified lookup logs only its failure class (at
// most every 10 s).
// Intentional differences: the job bound and admission 503; the router-wide
// request line (refuses `#` and targets over 12 500 bytes, where Node splits
// the query at `#` and allows 16 KiB of headers).
struct DashPayServiceDeps {
  providers::ProviderExecutionCoordinator& coordinator;
};

class DashPayService {
 public:
  static constexpr size_t kWorkers = 2;
  static constexpr size_t kJobs = 8;
  static constexpr uint32_t kLookupBudgetMs = 28000;

  DashPayService(DashPayService&&) noexcept;
  DashPayService& operator=(DashPayService&&) = delete;
  ~DashPayService();
  DashPayService(const DashPayService&) = delete;
  DashPayService& operator=(const DashPayService&) = delete;

  static base::Result<DashPayService> Create(const DashPayConfig& config, DashPayServiceDeps deps,
                                      const DashPayServiceSeams* seams = nullptr);
  bool Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff);
  // Waits until no job is queued or running (tests).
  bool WaitIdle(uint32_t timeout_ms);
  // The trusted connection generation (tests; 0 = none).
  uint64_t generation() const;
  // The last unverified lookup's diagnostic (no name or identity), or "".
  void last_failure(char* out, size_t capacity) const;

  struct Impl;

 private:
  explicit DashPayService(Impl* impl);
  void Stop();
  // Defined beside Impl: only that unit can destroy one.
  struct ImplDeleter { void operator()(Impl* impl) const; };
  std::unique_ptr<Impl, ImplDeleter> impl_;
};

}  // namespace fortis::exchange
