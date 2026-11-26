#pragma once

#include <memory>
#include "chain_core.h"
#include "ownership.h"
#include "transport_config.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

namespace fortis::providers { class ProviderExecutionCoordinator; }

// The TypeScript DriverTransport and HttpClient for the address,
// transaction and network routes, over one lease of the process
// ProviderExecutionCoordinator:
//
//  - jsonRpc: JSON-RPC 2.0 POST {jsonrpc:'2.0',id:1,method,params} to each
//    endpoint in order. A body with `error` is an UpstreamAnswerError
//    ("<method>: <JSON.stringify(error)>", no failover); a missing `result`
//    is an answer error too ("<method>: empty result"). Any HTTP failure,
//    timeout or busy lane moves on to the next endpoint. POSTs are not
//    retried in place.
//  - getJson: GET base+path (joinUrl: one trailing "/" of the base removed).
//    A 4xx is an answer error ("http <status>", no failover); 5xx, 429,
//    transport failures, busy lanes and a body failing `validate` fail over.
//    GETs keep HttpClient's three attempts (transport failure, 5xx and 408;
//    never 429) with its 250 ms / 1 s backoff.
//  - postJson: POST base+path with JSON.stringify(body); 4xx answer errors as
//    getJson; `retry_post` opts into the GET retry policy.
//
// A 2xx body that is not JSON is a transport failure (HttpClient wraps the
// parse error), retried like any other GET transport failure. Each endpoint
// runs on its lane: the call's `lane` (laneFor) or "rpc:{host}". The last
// endpoint's failure is the call's failure: a LaneBusyError surfaces as
// kLaneBusy (the facet's 429), everything else as kTransport.
//
// Native bounds (documented differences): request bodies at most 256 KiB
// (kProviderExecutionMaxRequestBody), responses at most 512 KiB
// (kProviderExecutionMaxResponseBody) — a larger answer is a transport
// failure; at most kTransportMaxCalls calls per Run.
namespace fortis::chains {

// kRawPost: a binary POST (the DashPay resolver's gRPC-web getDocuments)
// whose 2xx body is returned unparsed; a 4xx is an answer error as for kPost.
enum class CallKind : uint8_t { kJsonRpc, kGet, kPost, kRawPost };
// kNoEndpoints: an empty URL list (withFailover's RangeError 'no endpoints
// configured', which the facet answers as a 400).
enum class CallFailure : uint8_t { kNone, kAnswer, kTransport, kLaneBusy, kCancelled, kNoEndpoints };

constexpr size_t kCallMaxHeaders = 4;
constexpr size_t kTransportMaxCalls = 48;

struct CallHeader {
  const char* name = nullptr;
  const char* value = nullptr;
};

struct Call {
  // ---- inputs ----
  CallKind kind = CallKind::kJsonRpc;
  // Endpoint bases in failover order (a network's rpc_urls, or one fixed host
  // such as "https://api.etherscan.io").
  const char* const* urls = nullptr;
  size_t url_count = 0;
  // kGet/kPost: the path (with its query) appended to each base.
  const char* path = nullptr;
  // kJsonRpc: method and params (null params means []).
  const char* method = nullptr;
  const Value* params = nullptr;
  // kPost: the JSON body value.
  const Value* body = nullptr;
  // laneFor(): a fixed lane for every endpoint; null = "rpc:{host}".
  const char* lane = nullptr;
  CallHeader headers[kCallMaxHeaders]{};
  size_t header_count = 0;
  bool retry_post = false;
  // EndpointCall.validate (getJson): an invalid 2xx body is an outage.
  bool (*validate)(const Value* body) = nullptr;
  // parseJsonExactIntegers (NEAR): integer literals of 16+ digits parse as
  // strings.
  bool exact_integers = false;
  // Background lane priority (scheduled jobs and SWR revalidation).
  bool background = false;
  // HttpRequestOptions.timeoutMs per attempt; 0 keeps the 10 s default.
  uint32_t timeout_ms = 0;
  // kRawPost: the body and its Content-Type (at most 63 printable bytes).
  const uint8_t* raw_body = nullptr;
  size_t raw_body_length = 0;
  const char* content_type = nullptr;
  // A response cap below the transport's (kProviderExecutionMaxResponseBody);
  // 0 keeps that default.
  size_t max_response_bytes = 0;

  // ---- results (filled by Transport::Run) ----
  CallFailure failure = CallFailure::kNone;
  // The final HTTP status (answer errors carry their 4xx), 0 without one.
  int http_status = 0;
  // kJsonRpc: the `result` member (may be JSON null). kGet/kPost: the body.
  Value* value = nullptr;
  // UpstreamAnswerError message (see above); a short reason otherwise.
  char message[640]{};
  // JSON-RPC error.code when it is a number.
  bool has_code = false;
  double code = 0;
  uint32_t retry_after_ms = 0;
  // The endpoint base that produced the final result.
  const char* endpoint = nullptr;
  // kRawPost: the 2xx body, arena-owned.
  const uint8_t* raw_response = nullptr;
  size_t raw_response_length = 0;
  // When this call reached its final result within its Run: 1 for the
  // first call to settle, 2 for the next... (0: it never ran).
  uint32_t settled_rank = 0;

  bool ok() const { return failure == CallFailure::kNone; }
  bool answer() const { return failure == CallFailure::kAnswer; }
  // The thrown error as a driver sees it: answer/transport -> kUpstream,
  // busy lane -> kLaneBusy, no endpoints -> kRange.
  void ToError(Error* error) const;
};

uint64_t ChainMonotonicMs();

class ChainTransport {
 public:
  virtual void Run(Call* calls, size_t count, Arena& arena, uint64_t deadline_ms) = 0;
  void Run(Call* calls, size_t count, Arena& arena) {
    Run(calls, count, arena, ChainMonotonicMs() + 25000);
  }
  void Run(Call& call, Arena& arena) { Run(&call, 1, arena); }
 protected:
  ~ChainTransport() = default;
};

// Transport ownership/lifecycle stays in composition; clients borrow only Run.
class Transport final : public ChainTransport {
 public:
  Transport(const TransportConfig& config, providers::ProviderExecutionCoordinator* coordinator);
  ~Transport();
  Transport(const Transport&) = delete;
  Transport& operator=(const Transport&) = delete;

  bool Start();
  void Stop();
  bool running() const;

  // Runs every call to its final result concurrently (Promise.all) and
  // blocks until all are done or `deadline_ms` (monotonic) passes; bodies
  // parse into `arena`. Each call moves through its endpoints on its own,
  // as withFailover does: a call fails over the moment its endpoint fails
  // or its lane sheds it, while the other calls are still in flight, so a
  // background call can find the next endpoint's lane free (or busy) as
  // TypeScript does. Never call from an HTTP worker thread.
  void Run(Call* calls, size_t count, Arena& arena, uint64_t deadline_ms) override;
  using ChainTransport::Run;

  const TransportConfig& config() const;

  // TypeScript runs a stale-while-revalidate refresh under the
  // `swr_revalidate` work reason, i.e. at background lane priority (shed by
  // a busy lane instead of queueing). Every call made on `thread` (the
  // owner's revalidation worker) runs at that priority. Set once, before
  // the thread runs any revalidation.
  void SetBackgroundThread(pthread_t thread);

  // Start() with every request sent to one endpoint, its CA file and address
  // policy (upstream_redirect.h); with `real_host_header`, each request names
  // the host it would have dialled (host[:port]) in that header. Retry
  // backoff sleeps are skipped. Production composes with Start().
  bool StartRedirected(const char* host, const char* port, const char* ca,
                       bool (*allow_address)(void*, uint32_t), void* context,
                       const char* real_host_header = nullptr);

 private:
  struct Impl;
  std::unique_ptr<Impl, fortis::base::ObjectDeleter<Impl>> impl_;
  pthread_t background_thread_{};
  bool background_thread_set_ = false;
};

uint64_t ChainMonotonicMs();

// parity-only, see compat-decisions.md#promise-all
// Promise.all's rejection: of the calls that failed, the one that settled
// first (null when every call succeeded).
inline const Call* FirstSettledFailure(const Call* calls, size_t count) {
  const Call* first = nullptr;
  for (size_t i = 0; i < count; ++i)
    if (!calls[i].ok() && (!first || calls[i].settled_rank < first->settled_rank)) first = &calls[i];
  return first;
}

}  // namespace fortis::chains
