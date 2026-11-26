#pragma once

#include <functional>
#include <memory>
#include <type_traits>

#include "http_request.h"

#include "threading.h"

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

namespace fortis::http {

struct HttpQueueTimings {
  static constexpr uint64_t kBoundsUs[] = {100, 500, 1000, 5000, 10000, 50000,
      100000, 500000, 1000000, 2000000, 5000000};
  static constexpr size_t kBuckets = sizeof(kBoundsUs) / sizeof(kBoundsUs[0]) + 1;
  enum Series { Dispatch, ReadLockWait, ReadLockHold, MutationLockWait, MutationLockHold, Count };
  uint64_t buckets[Count][kBuckets]{};
};
HttpQueueTimings ReadHttpQueueTimings();

enum class HttpAdmission : uint8_t {
  None, ConnectionQueue, SpotSlots, ResponseSlots, SpotOwner, SnapshotUnavailable,
  AcceptResourceExhaustion, SnapshotAuthorityBusy, ConnectionQueueExpired, Count
};

struct HttpResponse {
  int status = 200;
  const char* reason = "OK";
  const char* content_type = "application/json";
  const uint8_t* body = nullptr;
  size_t body_length = 0;
  bool vary_accept = false;
  unsigned retry_after = 0;
  int rate_limit_cost = -1;
  uint32_t rate_limit_remaining = 0;
  char etag[128]{};
  char www_authenticate[128]{};
  bool omit_content_type = false;
  // A static-storage Cache-Control value; null sends "no-store".
  const char* cache_control = nullptr;
  // The body has static storage (or outlives every write of this response),
  // so a captured response may reference it instead of copying it.
  bool static_body = false;
  // Static-storage, preformatted extra header lines ("Name: value\r\n"…),
  // written after the standard headers; null writes none.
  const char* extra_headers = nullptr;
  // A Location header value (redirects), copied with the response.
  char location[192]{};
  // Exclusive oldest-message cursor on paged conversation history replies.
  char next_cursor[27]{};
  char ai_reply_for[27]{};
  char ai_reply_status[16]{};
  HttpAdmission admission = HttpAdmission::None;
  unsigned write_timeout_ms = 3000;
  double owner_ms = 0;
};

// Called once per finished response of a matched route other than /metrics,
// after the final status is known (a reserved socket reports on completion
// or timeout). `route` is the registered template; `device_id` is the device
// the handler authenticated, or null.
using HttpCompletionHook = void (*)(void* context, const char* route, int status,
                                    const char* device_id, double duration_ms);

struct HttpCompletionTag {
  HttpCompletionHook hook = nullptr;
  void* context = nullptr;
  const char* route = nullptr;
  char device_id[27]{};
  timespec start{};
  const char* method = nullptr;
  uint8_t platform = 0;
  double queue_ms = 0;
  double owner_ms = 0;
  HttpAdmission admission = HttpAdmission::None;
  void (*details)(void*, const HttpCompletionTag&, int, double) = nullptr;
  void* details_context = nullptr;
};

// Valid only inside a route handler: notes the device a protected route
// authenticated, for the completion hook.
void HttpNoteAuthenticatedDevice(const char* device_id, uint8_t platform = 0);
void WriteHttpAdmissionOverload(int client, const timespec* dispatch_started = nullptr);
void HttpNoteAdmission(HttpAdmission reason);
void HttpNoteDevicePlatform(uint8_t platform);

using HttpHandoffTerminalFn = void (*)(void* context, bool cancelled);

// A bounded owner for sockets whose route response is ready later. Submitted
// responses are copied, including their body, before Submit returns.
class ResponseHandoff {
 public:
  static constexpr size_t kCapacity = 16;
  static constexpr size_t kWriterCount = 2;
  ResponseHandoff();
  ~ResponseHandoff();
  ResponseHandoff(const ResponseHandoff&) = delete;
  ResponseHandoff& operator=(const ResponseHandoff&) = delete;
  bool Start();
  void Stop();
  // Reserve transfers socket ownership immediately. A successful token may be
  // completed once from any thread before its monotonic deadline.
  bool Reserve(int client, bool head, uint32_t timeout_ms, uint64_t* token);
  // `request_start` (CLOCK_MONOTONIC) is the Dispatch entry time the
  // writer's Server-Timing measures from; null means "now" (direct callers
  // outside a dispatch). It is copied into the slot, never retained.
  bool Reserve(int client, bool head, uint32_t timeout_ms, uint64_t* token,
               HttpHandoffTerminalFn terminal, void* terminal_context,
               const HttpCompletionTag* completion = nullptr,
               const timespec* request_start = nullptr);
  bool Complete(uint64_t token, const HttpResponse& response);
  bool Submit(int client, const HttpResponse& response, bool head,
              uint32_t delay_ms, const timespec* request_start = nullptr);

 private:
  friend class HttpRouter;
  friend class ConnectionSession;
  // Error recovery after a handler reserved the socket. No allocation; zero
  // status abandons a raw-fd connection that might already contain bytes.
  bool FailReservation(uint64_t token, int status);
  struct Pending {
    bool occupied = false;
    bool ready = false;
    int client = -1;
    uint64_t token = 0;
    HttpResponse response{};
    char reason[81]{};
    char content_type[128]{};
    std::unique_ptr<uint8_t[], base::StorageDeleter> body;
    bool head = false;
    uint64_t due_ms = 0;
    HttpHandoffTerminalFn terminal = nullptr;
    void* terminal_context = nullptr;
    HttpCompletionTag completion{};
    // Dispatch entry time carried for Server-Timing (see http_server.cpp).
    timespec request_start{};
  };
  struct WriterContext { ResponseHandoff* owner = nullptr; size_t index = 0; };
  static void* WriterMain(void* opaque);
  void WriterLoop(size_t index);
  Pending pending_[kCapacity]{};
  int active_clients_[kWriterCount]{};
  fortis::base::Thread writers_[kWriterCount];
  WriterContext contexts_[kWriterCount]{};
  size_t started_writers_ = 0;
  uint64_t next_token_ = 1;
  fortis::base::Mutex mutex_;
  fortis::base::ConditionVariable available_;
  bool stopping_ = false;
};
using HttpResponseHandoff = ResponseHandoff;


// ---- Route handlers ---------------------------------------------------------
//
// A route handler receives the parsed request and an HttpReply. It answers
// by handing the reply one HttpResponse (Send), or by reserving the reply
// for an owner that completes it later through the response handoff
// (Reserve). It never sees the socket: the router serialises the response
// after the handler returns (status line, headers, Content-Length, HEAD
// handling and Connection: close are the router's, in http_server.cpp).
//
// A default-constructed HttpReply is detached: it has no socket and no
// handoff, so a handler can be called directly from a unit test and its
// answer read back with sent() / response() / head_only(). Reserve fails on
// a detached reply (the handler then takes its own "no handoff" path).
class HttpReply {
 public:
  HttpReply() = default;
  // A detached reply to a HEAD (true) or other (false) request.
  explicit HttpReply(bool head) : request_head_(head) {}
  ~HttpReply();
  HttpReply(const HttpReply&) = delete;
  HttpReply& operator=(const HttpReply&) = delete;

  // The request was a HEAD: Send keeps the status, every header and the
  // declared Content-Length, and the router omits the body.
  bool head() const { return request_head_; }
  // Answers the request. The response is copied, body included (a
  // static_body is referenced), so its buffers may be the handler's stack.
  // One response per request. A response with invalid headers is refused
  // (false) and nothing is recorded; a second Send or a body over the
  // capture limit is refused and the router then answers 500 (503 when the
  // copy could not be allocated), as it always has.
  bool Send(const HttpResponse& response) { return Capture(response, request_head_); }
  // After Send: the router hands the captured response to the handoff, which
  // writes it `delay_ms` (at most 10 s) later.
  bool Defer(uint32_t delay_ms);
  // Instead of Send: transfers the socket to the router's response handoff.
  // On success `*token` is completed once, from any thread, with
  // HttpResponseHandoff::Complete before `timeout_ms` passes.
  bool Reserve(uint32_t timeout_ms, uint64_t* token, HttpHandoffTerminalFn terminal = nullptr,
               void* terminal_context = nullptr);

  // What the handler produced (the router's view; unit tests read it too).
  bool sent() const { return written_; }
  bool reserved() const { return reserved_; }
  bool deferred() const { return deferred_; }
  // The body is omitted when the response is written.
  bool head_only() const { return head_; }
  // Valid when sent(); the pointers live as long as this reply.
  const HttpResponse& response() const { return response_; }

 private:
  friend class HttpRouter;
  friend class ConnectionSession;
  friend bool WriteHttpResponse(int client, const HttpResponse& response, bool head);
  friend bool DeferHttpResponse(uint32_t delay_ms);
  friend bool ReserveHttpResponse(int client, bool head, uint32_t timeout_ms, uint64_t* token,
                                  HttpHandoffTerminalFn terminal, void* terminal_context);
  friend void HttpNoteAuthenticatedDevice(const char* device_id, uint8_t platform);
  friend void HttpNoteAdmission(HttpAdmission reason);
  friend void HttpNoteDevicePlatform(uint8_t platform);

  static constexpr size_t kInlineBodyBytes = 512;
  bool Capture(const HttpResponse& response, bool head);
  bool ReserveSocket(int client, bool head, uint32_t timeout_ms, uint64_t* token,
                     HttpHandoffTerminalFn terminal, void* terminal_context);

  HttpResponse response_{};
  char reason_[81]{};
  char content_type_[128]{};
  uint8_t inline_body_[kInlineBodyBytes]{};
  std::unique_ptr<uint8_t[], base::StorageDeleter> heap_body_;
  bool request_head_ = false;
  bool head_ = false;
  bool written_ = false;
  bool overflow_ = false;
  bool allocation_failed_ = false;
  bool deferred_ = false;
  uint32_t delay_ms_ = 0;
  bool reserved_ = false;
  uint64_t reservation_token_ = 0;
  // Set by the router for the length of one Dispatch; -1 / null when detached.
  int client_ = -1;
  ResponseHandoff* handoff_ = nullptr;
  HttpCompletionTag completion_{};
  // Dispatch entry time (CLOCK_MONOTONIC). Reserve carries it into the
  // handoff so an asynchronous response's Server-Timing covers the whole
  // request, not just the writer's part.
  timespec request_start_{};
};

// The handler signature every migrated route uses.
using HttpHandler = void (*)(const HttpRequest&, HttpReply& reply, void* context);

// MIGRATION ADAPTER. The original raw-fd handler signature, kept only so
// routes that have not yet moved to HttpHandler keep working unchanged. The
// router runs both kinds through the same HttpReply: inside a dispatch
// WriteHttpResponse / DeferHttpResponse / ReserveHttpResponse below forward
// to the current request's reply; the fd they are handed is the dispatch's
// own socket and is used only by Reserve. Do not write new routes against
// this signature; delete it, the three free functions' capture mode and the
// HttpRouter::Add overload that takes it once the last route has moved.
using HttpRouteHandler = void (*)(int client, const HttpRequest&, bool head, void* context);

// Inside a route handler this captures the response; Dispatch (or the
// handoff writer) sends it later. Every head the router sends carries one
// `Server-Timing: app;dur=<ms>` line measured from Dispatch entry until the
// head is formatted (scope documented at kServerTimingLineBytes in
// http_server.cpp). Called outside a dispatch it sends immediately and
// measures from this call. A response's extra_headers must not name
// Server-Timing (refused as a duplicate).
bool WriteHttpResponse(int client, const HttpResponse& response, bool head = false);
// Valid only inside a route handler. Dispatch snapshots the handler's captured
// response and transfers the socket to the router's configured handoff.
bool DeferHttpResponse(uint32_t delay_ms);
// Uses the current dispatch's configured handoff. On success the socket is
// owned by the handoff and the returned token is valid until timeout/complete.
bool ReserveHttpResponse(int client, bool head, uint32_t timeout_ms,
                         uint64_t* token, HttpHandoffTerminalFn terminal = nullptr,
                         void* terminal_context = nullptr);
HttpResponse JsonResponse(int status, const char* reason, const char* json);
bool AcceptsProtobuf(const HttpRequest& request);

// Selects JSON or protobuf by Accept, computes this server's weak ETag from
// the selected representation (or its optional validator bytes), and returns
// a bodyless 304 on an exact If-None-Match match. Clients treat the ETag as
// opaque (docs/compat-decisions.md#etag).
bool ConditionalStructuredResponse(const HttpRequest& request,
                                   const char* name_space,
                                   const HttpResponse& json,
                                   const HttpResponse& protobuf,
                                   HttpResponse* response,
                                   const uint8_t* json_validator = nullptr,
                                   size_t json_validator_length = 0,
                                   const uint8_t* protobuf_validator = nullptr,
                                   size_t protobuf_validator_length = 0);

class HttpRouter {
 public:
  // 96 slots cover the current 62 production routes plus probes and leave
  // bounded headroom. On 64-bit targets the fixed Route table uses 3.75 KiB.
  static constexpr size_t kMaxRoutes = 96;
  static constexpr size_t kMetricsOutputCapacity = 64 * 1024;
  // RenderPrometheus never needs more than this, even with every counter of
  // every registered route saturated: Add refuses a route that would exceed
  // it. The remaining quarter of kMetricsOutputCapacity is left for the
  // gauges /metrics appends after the routes.
  static constexpr size_t kMetricsRouteOutputBudget = kMetricsOutputCapacity / 4 * 3;
  // A route's parameters never match an empty segment, a decoded control
  // character/DEL or a value over kHttpPathParamValueBytes: such a path is
  // the router's 404 for every route, before any handler.
  bool Add(const char* method, const char* path, HttpHandler handler,
           void* context = nullptr);
  // Migration adapter for a raw-fd handler (see HttpRouteHandler).
  bool Add(const char* method, const char* path, HttpRouteHandler handler,
           void* context = nullptr);
  // The target is borrowed until all dispatch workers have stopped. Route
  // storage remains the existing function pointer plus context pair.
  template <auto Method, typename T>
  bool Add(const char* method, const char* path, T& target) {
    static_assert(std::is_member_function_pointer_v<decltype(Method)>, "bind a member function");
    void* context = const_cast<void*>(static_cast<const void*>(std::addressof(target)));
    if constexpr (std::is_invocable_v<decltype(Method), T&, const HttpRequest&, HttpReply&>) {
      return Add(method, path, [](const HttpRequest& request, HttpReply& reply, void* context) {
        std::invoke(Method, *static_cast<T*>(context), request, reply);
      }, context);
    } else {
      static_assert(std::is_invocable_v<decltype(Method), T&, int, const HttpRequest&, bool>,
                    "unsupported route signature");
      return Add(method, path, [](int client, const HttpRequest& request, bool head, void* context) {
        std::invoke(Method, *static_cast<T*>(context), client, request, head);
      }, context);
    }
  }
  size_t RouteCount() const { return route_count_; }
  // One more route template whose handlers run without the route state lock,
  // beside the audited list in Dispatch: for a handler that only enqueues
  // work and touches no KV, quota or route state. `path` must outlive the
  // router. Call before the workers start.
  void RunWithoutStateLock(const char* path) { unlocked_path_ = path; }
  void SetResponseHandoff(ResponseHandoff* handoff) { handoff_ = handoff; }
  void SetCompletionHook(HttpCompletionHook hook, void* context) {
    completion_hook_ = hook;
    completion_context_ = context;
  }
  // Returns true only when the socket was transferred to the response handoff.
  void SetCompletionDetails(void (*hook)(void*, const HttpCompletionTag&, int, double), void* context) {
    completion_details_ = hook;
    details_context_ = context;
  }
  bool Dispatch(int client, const char* remote_ip = nullptr, const timespec* accepted = nullptr);
  // Lifetime per-route counters as Prometheus text (see http_server.cpp).
  // Returns the length written, or 0 when `capacity` is too small (output is
  // then unspecified but never written past `capacity`).
  size_t RenderPrometheus(char* output, size_t capacity);
  // The RenderPrometheus length bound for the routes registered so far.
  size_t PrometheusWorstCaseBytes() const;

 private:
  friend class ConnectionSession;
  struct Route {
    const char* method;
    const char* path;
    // Exactly one of the two is set.
    HttpHandler handler;
    HttpRouteHandler raw_handler;
    void* context;
  };
  bool AddRoute(const char* method, const char* path, HttpHandler handler,
                HttpRouteHandler raw_handler, void* context);
  struct RouteMetrics {
    uint64_t requests = 0;
    uint64_t errors = 0;
    uint64_t duration_us = 0;
    uint64_t max_duration_us = 0;
  };
  void RecordMetrics(size_t route_index, int status, uint64_t duration_us);
  Route routes_[kMaxRoutes]{};
  size_t route_count_ = 0;
  ResponseHandoff* handoff_ = nullptr;
  const char* unlocked_path_ = nullptr;
  HttpCompletionHook completion_hook_ = nullptr;
  void (*completion_details_)(void*, const HttpCompletionTag&, int, double) = nullptr;
  void* details_context_ = nullptr;
  void* completion_context_ = nullptr;
  RouteMetrics metrics_[kMaxRoutes + 1]{};
  size_t metrics_worst_case_bytes_ = 0;
  fortis::base::Mutex metrics_mutex_;
};

}  // namespace fortis::http
