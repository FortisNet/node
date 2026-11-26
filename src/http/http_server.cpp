#include <algorithm>
#include <atomic>
#include "exception_boundary.h"
#include "http_server.h"
#include "http_session.h"

#include "threading.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <poll.h>
#include <time.h>
#include <stdarg.h>
#include <unistd.h>
#include <mbedtls/sha256.h>
#include "http_transport.h"

namespace fortis::http {

namespace {
fortis::base::Mutex route_state_mutex;
struct QueueCounters {
  std::atomic<uint64_t> buckets[HttpQueueTimings::Count][HttpQueueTimings::kBuckets]{};
};
QueueCounters queue_timings;

void record_queue_timing(HttpQueueTimings::Series series, uint64_t micros) {
  size_t bucket = 0;
  while (bucket + 1 < HttpQueueTimings::kBuckets && micros > HttpQueueTimings::kBoundsUs[bucket])
    ++bucket;
  (queue_timings.buckets[series][bucket]).fetch_add(1, std::memory_order_relaxed);
}

// Device mutations with independently synchronized state bypass the route lock.
// The challenge route retains its existing lock. For POST /v1/devices, A Play Integrity enrollment waits on Google for up to
// kPlayDecodeDeadlineMs (25 s), which under the global lock stalled every
// other locked route. Every piece of shared state that handler touches has
// its own owner lock or atomic, so the global lock added nothing:
//   - IP quota: RuntimeKvIncrementWindow (kv_runtime.cpp quota_mutex), one
//     atomic increment per request, shared with the challenge route;
//   - KV store: KvStore's own mutex; RuntimeKvStore's lazy open is guarded
//     by runtime_store_mutex. A challenge is consumed only by the
//     conditional frame (KvStore::CompareAndWrite) that also writes the
//     device, so concurrent enrollments of one challenge enroll one device;
//   - Play Integrity: the route's atomic kPlayEnrollRouteMaxConcurrent (3)
//     admission counter, which keeps one of the four workers free of Google
//     waits; PlayIntegrityGoogleClient's mutex (token cache, replay ring,
//     the kPlayDecodeMaxConcurrent slots), released across every network
//     wait; the runtime pointer is published before workers start and
//     stopped after they are joined;
//   - outbound HTTPS (the Play client's transport): its CA cache and
//     resolver-slot mutexes, already shared with the provider owners;
//   - App Attest: the atomic kAppleVerifyConcurrency slot counter; the
//     verifier itself uses only per-call mbedTLS contexts;
//   - response capture: one HttpReply per Dispatch (active_response_capture);
//   - route metrics: metrics_mutex_; completion telemetry: ApiTelemetry's
//     mutex (already called outside the route lock for every route);
//   - NativeConfig: read-only after startup; RNG: getrandom/getentropy.
// The handler never takes route_state_mutex itself, and the owner locks it
// does take are the ones the KV sweeper, provider owners and unlocked probes
// already take without it, so running it unlocked adds no lock-order cycle
// and no lock is held across a Google or Apple wait. Any new state the
// enrollment handler touches must bring its own lock or this exemption must
// be removed. Matching is by the registered method and template, so another
// method or path under /v1/devices never matches.
bool route_runs_without_state_lock(const char* method, const char* path) {
  if (strcmp(method, "POST") != 0) return false;
  // Rotation and revocation use per-request buffers, the independently
  // locked auth/quota caches and KV conditional writes. They never call a
  // provider or touch route state protected by the global mutex.
  return strcmp(path, "/v1/devices") == 0 || strcmp(path, "/v1/devices/rotate") == 0 ||
         strcmp(path, "/v1/devices/revoke") == 0;
}

// The AI history read Dispatch runs without route_state_mutex: exactly the
// GET registration of /v1/ai/conversations/:id/messages
// (AiConversationService::Impl::Messages). HEAD reaches it through that GET
// registration (match->method is "GET"), so both are exempt; the POST at the
// same template, POST /v1/ai/conversations and every other API route stay
// locked. The read is common on mobile and must not queue behind slow locked
// routes. Its path, in order, and the owner of each piece of shared state:
//   - reject_long_id, the id shape and the chat bearer: this request and
//     stack buffers only;
//   - AuthorizeDeviceRequest: RuntimeKvStore (runtime_store_mutex),
//     DeviceAuthStore::Authenticate (two KvStore::GetStatus reads, each
//     under the store's mutex_; a rotation or revoke committing between them
//     is detected as 401 SESSION_EXPIRED and never as success; see
//     device_auth.cpp), the per-minute quota (RuntimeKvIncrementByWindow
//     under quota_mutex) and HttpNoteAuthenticatedDevice (thread-local
//     capture). Device writes (rotation, revoke, presence) are conditional
//     CompareAndWrite frames, so no route-lock ordering is assumed;
//   - ai::Store::Authorize: immutable after Start (now_, context_, kv_,
//     started_), SHA-256 one-shot (no mbedTLS statics: the SHA-256 ALT/
//     accelerated paths and the self-test are not used here) and one
//     conversation read via GetTextStatus, which retries when a concurrent
//     AddMessage snapshot update grows the record between its size probe and
//     its read; a successful read is one consistent KV value;
//   - ai::Store::Messages: one KvStore::Scan, a single mutex_ hold, so it
//     sees each AddMessage/SaveReply frame (rows and indexes) whole or not
//     at all; the visitor only copies into this request's arena. The AI
//     workers already wrote these keys concurrently without the route lock;
//   - EncodeMessagesBody, AcceptsProtobuf, IsoString: per-call arenas and
//     buffers; the AI clock seam is read-only after Start (wall clock in
//     production);
//   - the response: thread-local capture; metrics_mutex_; ApiTelemetry.
// The handler never takes the AI store mutex, the AI task mutex or the
// hourly AI quotas (POST-only), so it adds no lock order: it takes
// runtime_store_mutex, the KV mutex_ and quota_mutex one at a time, as the
// unlocked enrollment route, the KV sweeper and compaction already do.
// KV sweep and compaction only change records under mutex_; an expired
// conversation reads as 404 at `now` whether or not it was swept. Shutdown
// joins the HTTP workers before AiConversationService::Stop frees the
// handler context (main.cpp). Maintenance: any new state this handler (or
// AuthorizeDeviceRequest) touches must bring its own lock, or this exemption
// must go; a write added to the GET path needs a fresh audit.
bool ai_history_read_runs_without_state_lock(const char* method, const char* path) {
  return strcmp(method, "GET") == 0 && strcmp(path, "/v1/ai/conversations/:id/messages") == 0;
}

// The network catalog Dispatch runs without route_state_mutex: exactly the
// GET registration of /v1/networks (network_route.cpp handle). HEAD reaches
// it through that GET registration (match->method is "GET"); any other
// method, /v1/networks/:network/overview and every /v1/networks/:network/...
// template stay locked. The catalog is a mobile bootstrap read and must not
// queue behind slow locked routes. Its path and each owner:
//   - AuthorizeDeviceRequest, as audited for the AI history read above:
//     RuntimeKvStore (runtime_store_mutex), DeviceAuthStore::Authenticate
//     (two KvStore::GetStatus reads under the store's mutex_; a rotation
//     committing between them is 401 SESSION_EXPIRED, never success), the
//     in-memory per-minute quota (RuntimeKvIncrementByWindow, quota_mutex)
//     and HttpNoteAuthenticatedDevice (thread-local capture);
//   - the documents: json_body/proto_body, their lengths and
//     attestation_required are written once by RegisterNetworksRoute, which
//     main.cpp calls before start_http_workers (pthread_create orders the
//     writes before every read), and never again; they have static storage,
//     so shutdown cannot free them under a worker;
//   - AcceptsProtobuf: this request only; the 200 is a static_body capture
//     (a pointer to those immutable bytes), refusals copy literal bodies;
//   - the response: thread-local capture; metrics_mutex_; ApiTelemetry.
// It takes runtime_store_mutex, the KV mutex_ and quota_mutex one at a time
// (the quota table is in memory, so quota_mutex never nests mutex_), as the
// unlocked enrollment route and the KV sweeper already do: no lock order is
// added. Maintenance: the handler must stay a pure read of startup-built
// bytes; regenerating the documents at runtime, adding catalog-specific KV
// access, or adding any other state (here or in AuthorizeDeviceRequest)
// needs its own lock and a fresh audit, or this exemption must go.
bool network_catalog_runs_without_state_lock(const char* method, const char* path) {
  return strcmp(method, "GET") == 0 && strcmp(path, "/v1/networks") == 0;
}

// The spot price read Dispatch runs without route_state_mutex: exactly the
// GET registration of /v1/prices (price_spot_route.cpp handle_prices). HEAD
// reaches it through that GET registration (match->method is "GET"); every
// other method on /v1/prices, GET /v1/prices/history, GET /v1/prices/ohlc,
// POST /v1/prices/history and POST /v1/prices/markets stay locked. Other
// API exemptions are listed above. Provider work already runs on the bounded
// SpotProviderOwner
// and completes through the response handoff; only the request setup queued
// behind slow locked routes. Its path, in order, and each owner:
//   - AuthorizeDeviceRequest, as audited for the AI history read above
//     (runtime_store_mutex, the KV mutex_, the in-memory quota_mutex one at a
//     time; a rotation committing mid-check can return 401 SESSION_EXPIRED);
//     Split-store price reads can use the snapshot auth leaf mutex, which
//     is released before taking either the KV or quota mutex.
//   - fortis::prices::spot_query::ParseQuery and AcceptsProtobuf: pure, writing only
//     this request's stack Query and buffers (error bodies included);
//   - the route's RequestSlot table: every acquire, reference change and
//     release under its own slots_mutex (a leaf lock); a slot's fields are
//     written by this worker before Reserve/Submit publish it, and read by
//     the handoff and owner threads only after those calls' own mutexes;
//   - ReserveHttpResponse: the thread-local capture, then
//     HttpResponseHandoff::mutex_; once it succeeds the handoff owns the
//     socket and this worker never touches it again;
//   - SpotProviderOwner::Submit: CoinPopularity::RecordPriceIds (its own
//     mutex), then the owner's mutex; the token, address and price-job
//     threads already call Submit concurrently without this lock. Cache,
//     series and provider work run on the owner thread, as before;
//   - completion: owner_complete (owner thread) and handoff_terminal (writer
//     thread, or Stop) each drop one slot reference; both already ran
//     outside the route lock. Neither callback is invoked with the owner,
//     handoff or slots mutex held, so slots_mutex never nests another lock;
//   - the response: thread-local capture; metrics_mutex_; ApiTelemetry.
// No route_state_mutex is taken anywhere on this path, so no lock order is
// added. The route context and the owner's popularity pointer are written
// before start_http_workers, and main.cpp joins every worker before it stops
// the owner and then the handoff. Maintenance: handle_prices must stay
// device policy + pure parsing + bounded handoff/owner submission; any new
// shared state (here, in the RequestSlot table or in AuthorizeDeviceRequest)
// needs its own lock and a fresh audit, or this exemption must go.
bool spot_prices_run_without_state_lock(const char* method, const char* path) {
  return strcmp(method, "GET") == 0 && strcmp(path, "/v1/prices") == 0;
}

// HEAD matches the GET registration. Device policy has the same independent
// locks as the spot route. The snapshot mutex protects only a bounded copy
// and is released before handing off the response. The background builder
// owns price-store reads, and workers join before the route is destroyed.
bool price_snapshot_runs_without_state_lock(const char* method, const char* path) {
  return strcmp(method, "GET") == 0 && strcmp(path, "/v1/prices/snapshot") == 0;
}

// Liveness and pure self-check probes Dispatch runs without
// route_state_mutex, so a slow locked route cannot stall them. Each
// (method, template) pair below is served by main.cpp's handle_probe, and
// each was audited against that handler's shared state:
//   - GET|HEAD /healthz: kv_probe_healthy reads "health/check" (a key no
//     route writes) via RuntimeKvStore (lazy open under runtime_store_mutex)
//     and KvStore::GetStatus (one hold of the store's own mutex, the same
//     reads compaction, the expiry sweeper and the unlocked enrollment route
//     already race with). It joins no route's read-modify-write, so the
//     global lock only queued it behind unrelated work;
//   - GET|HEAD /probe and /probe/representation: static bodies plus
//     AcceptsProtobuf over this request only;
//   - POST /probe/protobuf: nanopb decode/encode of a fixed-size stack
//     message (no PB_ENABLE_MALLOC, no callbacks) with stack streams.
// Every response goes through the thread-local capture; route metrics take
// metrics_mutex_. None of these handlers takes route_state_mutex, so no lock
// order is added. The KV mutation probes (POST /probe/kv, POST
// /probe/storage), their GET reads, and /probe/evm stay locked; main.cpp
// registers them only with FORTIS_ENABLE_DIAGNOSTIC_PROBES=1. /probe/evm was
// kept locked because MBEDTLS_SELF_TEST's EC operation counters were
// unsynchronized global writes; fortis_mbedtls_config.h now disables
// MBEDTLS_SELF_TEST, but unlocking the route still needs its own audit.
// Every API route except enrollment, the AI history read, the network
// catalog and the spot price read also stays locked. Matching is by registered method and exact template;
// changing a handler registered at one of these templates requires a fresh
// ownership audit.
bool probe_runs_without_state_lock(const char* method, const char* path) {
  if (strcmp(method, "POST") == 0) return strcmp(path, "/probe/protobuf") == 0;
  if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) return false;
  return strcmp(path, "/healthz") == 0 || strcmp(path, "/probe") == 0 ||
         strcmp(path, "/probe/representation") == 0;
}
// Media serves verified raster objects up to 2 MiB through the async handoff.
// With 16 reserved slots, captured bodies are bounded by 32 MiB in total.
constexpr size_t kCapturedResponseLimit = 2 * 1024 * 1024;

// The reply of the request this thread is dispatching. Migrated handlers are
// handed it directly; it is thread-local only for the raw-fd migration
// adapter (WriteHttpResponse and friends) and the HttpNote* calls.
thread_local HttpReply* active_response_capture = nullptr;

class ActiveResponseScope {
 public:
  explicit ActiveResponseScope(HttpReply* reply) : previous_(active_response_capture) {
    active_response_capture = reply;
  }
  ~ActiveResponseScope() { active_response_capture = previous_; }
  ActiveResponseScope(const ActiveResponseScope&) = delete;
  ActiveResponseScope& operator=(const ActiveResponseScope&) = delete;
 private:
  HttpReply* previous_;
};

uint64_t elapsed_microseconds(const timespec& start);

// Server-Timing: every response head the native routers (public and staff
// bridge) send carries exactly one
//   Server-Timing: app;dur=<milliseconds>
// line. Scope: from HttpRouter::Dispatch entry (the worker has dequeued the
// accepted socket and is about to read the request; time spent in the accept
// queue is excluded) until the response head is formatted, immediately
// before its first byte is sent. Reading the request (headers and body),
// routing, the route lock wait, the handler, a deferred response's delay and
// a reserved response's wait for its owner are inside; sending the head and
// body is not (a duration cannot be written after the bytes that carry it).
// This is the same start the route metrics and completion telemetry use.
// The value is integer microseconds printed as "<ms>.<3 digits>", so it is
// always a finite, nonnegative decimal; a failed or backwards clock reads 0.
// It carries no request data. Formatting uses a fixed stack buffer: no heap
// and no allocation per request.
// The line is added only by write_timed_response and write_handoff_overload;
// a handler can never add a second one (valid_response_headers refuses
// extra_headers that contain it). Asynchronous responses carry the Dispatch
// start explicitly through HttpResponseHandoff::Reserve/Submit
// (Pending::request_start); a Reserve/Submit made outside a dispatch (tests)
// measures from that call.
// "Server-Timing: app;dur=" + 17 integer digits + ".999" + CRLF + NUL fits.
constexpr size_t kServerTimingLineBytes = 64;

bool format_server_timing(const timespec& request_start, char* output, size_t capacity) {
  const uint64_t microseconds = elapsed_microseconds(request_start);
  const int length = snprintf(output, capacity, "Server-Timing: app;dur=%llu.%03u\r\n",
                              static_cast<unsigned long long>(microseconds / 1000ULL),
                              static_cast<unsigned>(microseconds % 1000ULL));
  return length > 0 && static_cast<size_t>(length) < capacity;
}

// Generated singleton metadata cannot also come from a route's static headers.
bool names_generated_metadata(const char* headers) {
  for (const char* line = headers; line && *line;) {
    if (strncasecmp(line, "server-timing:", 14) == 0 || strncasecmp(line, "date:", 5) == 0) return true;
    const char* next = strstr(line, "\r\n");
    line = next ? next + 2 : nullptr;
  }
  return false;
}

// NGINX previously supplied this origin-server metadata. Keep it locale-free
// and reentrant; when the wall clock is unavailable, omit rather than invent it.
bool format_http_date(char* output, size_t capacity) {
  const time_t seconds = time(nullptr);
  tm utc{};
  if (seconds == static_cast<time_t>(-1) || !gmtime_r(&seconds, &utc) ||
      utc.tm_wday < 0 || utc.tm_wday > 6 || utc.tm_mon < 0 || utc.tm_mon > 11 ||
      utc.tm_year < 70 || utc.tm_year > 8099 || utc.tm_mday < 1 || utc.tm_mday > 31 ||
      utc.tm_hour < 0 || utc.tm_hour > 23 || utc.tm_min < 0 || utc.tm_min > 59 ||
      utc.tm_sec < 0 || utc.tm_sec > 60) return false;
  static constexpr const char* weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static constexpr const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const int length = snprintf(output, capacity, "Date: %s, %02d %s %04d %02d:%02d:%02d GMT\r\n",
      weekdays[utc.tm_wday], utc.tm_mday, months[utc.tm_mon], utc.tm_year + 1900,
      utc.tm_hour, utc.tm_min, utc.tm_sec);
  return length > 0 && static_cast<size_t>(length) < capacity;
}

bool make_deadline(timespec* deadline, int64_t timeout_ms) {
  if (!deadline || clock_gettime(CLOCK_MONOTONIC, deadline) != 0) return false;
  deadline->tv_sec += static_cast<time_t>(timeout_ms / 1000);
  deadline->tv_nsec += static_cast<long>((timeout_ms % 1000) * 1000000);
  if (deadline->tv_nsec >= 1000000000L) {
    ++deadline->tv_sec;
    deadline->tv_nsec -= 1000000000L;
  }
  return true;
}

bool wait_writable_until(int fd, const timespec& deadline) {
  if (fd < 0) return false;
  for (;;) {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return false;
    long seconds = deadline.tv_sec - now.tv_sec;
    long nanoseconds = deadline.tv_nsec - now.tv_nsec;
    if (nanoseconds < 0) { --seconds; nanoseconds += 1000000000L; }
    if (seconds < 0 || (seconds == 0 && nanoseconds <= 0)) return false;
    const int timeout_ms = static_cast<int>(seconds * 1000 + (nanoseconds + 999999) / 1000000);
    pollfd descriptor{fd, POLLOUT, 0};
    const int selected = poll(&descriptor, 1, timeout_ms);
    if (selected > 0) return (descriptor.revents & POLLOUT) != 0;
    if (selected == 0) return false;
    if (errno != EINTR) return false;
  }
}

bool send_all_until(int fd, const char* data, size_t length,
                    const timespec& deadline) {
  if (auto* transport = FindHttpTransport(fd)) return transport->Write(data, length, deadline);
  while (length) {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
        now.tv_sec > deadline.tv_sec ||
        (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) return false;
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
#ifdef MSG_DONTWAIT
    flags |= MSG_DONTWAIT;
#endif
    const ssize_t sent = send(fd, data, length, flags);
    if (sent < 0 && errno == EINTR) continue;
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (wait_writable_until(fd, deadline)) continue;
      return false;
    }
    if (sent <= 0) return false;
    data += sent;
    length -= static_cast<size_t>(sent);
  }
  return true;
}



void write_error(int fd, int status, const char* reason, const char* body,
                 bool head, const timespec& request_start) {
  const HttpResponse response = JsonResponse(status, reason, body);
  ResponseWriter::Write(fd, response, head, request_start);
}

void write_handoff_overload(int fd, bool head, const timespec& request_start) {
  // The one response head not built by write_timed_response; it appends the
  // same single Server-Timing line itself (see format_server_timing).
  static constexpr char kHead[] =
      "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n"
      "Content-Type: application/json\r\nContent-Length: 22\r\n"
      "Retry-After: 1\r\nCache-Control: no-store\r\n";
  static constexpr char kBody[] = "{\"error\":\"OVERLOADED\"}";
  char timing[kServerTimingLineBytes];
  if (!format_server_timing(request_start, timing, sizeof(timing))) return;
  char response[sizeof(kHead) + kServerTimingLineBytes + 2 + sizeof(kBody)];
  size_t length = sizeof(kHead) - 1;
  memcpy(response, kHead, length);
  const size_t timing_length = strlen(timing);
  std::copy_n(timing, timing_length, response + length);
  length += timing_length;
  std::copy_n("\r\n", 2, response + length);  // end of the head
  length += 2;
  if (!head) {
    memcpy(response + length, kBody, sizeof(kBody) - 1);
    length += sizeof(kBody) - 1;
  }
  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif
#ifdef MSG_DONTWAIT
  flags |= MSG_DONTWAIT;
#endif
  // Best-effort and deliberately nonblocking: saturation must not pin an
  // HTTP request worker behind a peer that has stopped reading.
  if (auto* transport = FindHttpTransport(fd)) {
    transport->DisallowReuse();
    timespec deadline{};
    if (make_deadline(&deadline, 100)) transport->Written(transport->Write(response, length, deadline));
  } else send(fd, response, length, flags);
}

bool safe_header_value(const char* value, size_t maximum, bool allow_empty = false) {
  if (!value) return allow_empty;
  const size_t length = strnlen(value, maximum + 1);
  if (length > maximum || (!allow_empty && length == 0)) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

bool valid_namespace(const char* name_space) {
  if (!name_space) return false;
  const size_t length = strnlen(name_space, 65);
  if (length == 0 || length > 64) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(name_space[i]);
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  }
  return true;
}

__attribute__((format(printf, 4, 5)))
bool append_format(char* output, size_t capacity, size_t* used,
                   const char* format, ...) {
  if (!output || !used || *used >= capacity) return false;
  va_list arguments;
  va_start(arguments, format);
  const int written = vsnprintf(output + *used, capacity - *used, format, arguments);
  va_end(arguments);
  if (written < 0 || static_cast<size_t>(written) >= capacity - *used) return false;
  *used += static_cast<size_t>(written);
  return true;
}

uint64_t elapsed_microseconds(const timespec& start) {
  timespec finish{};
  if (clock_gettime(CLOCK_MONOTONIC, &finish) != 0) return 0;
  int64_t seconds = static_cast<int64_t>(finish.tv_sec) - static_cast<int64_t>(start.tv_sec);
  int64_t nanoseconds = static_cast<int64_t>(finish.tv_nsec) - static_cast<int64_t>(start.tv_nsec);
  if (nanoseconds < 0) { --seconds; nanoseconds += 1000000000LL; }
  if (seconds < 0) return 0;
  const uint64_t usec = static_cast<uint64_t>(seconds) * 1000000ULL +
                        static_cast<uint64_t>(nanoseconds / 1000);
  return usec;
}

uint64_t monotonic_milliseconds() {
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return static_cast<uint64_t>(now.tv_sec) * 1000ULL +
         static_cast<uint64_t>(now.tv_nsec / 1000000L);
}

timespec realtime_after_milliseconds(uint64_t milliseconds) {
  timespec wake{};
  clock_gettime(CLOCK_REALTIME, &wake);
  wake.tv_sec += static_cast<time_t>(milliseconds / 1000ULL);
  wake.tv_nsec += static_cast<long>((milliseconds % 1000ULL) * 1000000ULL);
  if (wake.tv_nsec >= 1000000000L) {
    ++wake.tv_sec;
    wake.tv_nsec -= 1000000000L;
  }
  return wake;
}

void saturating_increment(uint64_t* value) {
  if (*value != UINT64_MAX) ++*value;
}

void saturating_add(uint64_t* value, uint64_t amount) {
  *value = amount > UINT64_MAX - *value ? UINT64_MAX : *value + amount;
}

bool valid_response_headers(const HttpResponse& response) {
  return safe_header_value(response.reason, 80) &&
         (response.status == 304 || response.omit_content_type ||
          safe_header_value(response.content_type, 127)) &&
         safe_header_value(response.etag, sizeof(response.etag) - 1, true) &&
         safe_header_value(response.www_authenticate,
                           sizeof(response.www_authenticate) - 1, true) &&
         safe_header_value(response.cache_control, 127, true) &&
         safe_header_value(response.location, sizeof(response.location) - 1, true) &&
         safe_header_value(response.next_cursor, sizeof(response.next_cursor) - 1, true) &&
         safe_header_value(response.ai_reply_for, sizeof(response.ai_reply_for) - 1, true) &&
         safe_header_value(response.ai_reply_status, sizeof(response.ai_reply_status) - 1, true) &&
         !names_generated_metadata(response.extra_headers);
}

void report_completion(const HttpCompletionTag& tag, int status) {
  if ((!tag.hook && !tag.details) || !tag.route || status <= 0) return;
  const double duration = static_cast<double>(elapsed_microseconds(tag.start)) / 1000.0;
  if (tag.hook) fortis::base::FatalBoundary("HTTP completion hook", [&] {
    tag.hook(tag.context, tag.route, status, tag.device_id[0] ? tag.device_id : nullptr, duration);
  });
  if (tag.details) fortis::base::FatalBoundary("HTTP completion details", [&] {
    tag.details(tag.details_context, tag, status, duration);
  });
}

timespec monotonic_now() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return now;
}

// The single place a response head is built and sent (the 503 overload
// literal in write_handoff_overload is the only other head). It appends the
// Server-Timing line measured from `request_start`; see
// kServerTimingLineBytes for the exact scope. Dispatch passes its entry
// time; the handoff writers pass Pending::request_start.

}  // namespace

bool ResponseWriter::Write(int client, const HttpResponse& response, bool head,
                           const timespec& request_start) noexcept {
  if (!valid_response_headers(response) || (response.body_length && !response.body))
    return false;
  // 1024 bytes of standard headers (the pre-Server-Timing bound) plus the
  // timing line.
  char headers[1024 + 64 + kServerTimingLineBytes];
  const char* vary = response.vary_accept ? "Vary: Accept\r\n" : "";
  char content_type[160]{};
  if (response.status != 304 && !response.omit_content_type) {
    const int length = snprintf(content_type, sizeof(content_type),
                                "Content-Type: %s\r\n", response.content_type);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(content_type)) return false;
  }
  char content_length[64]{};
  if (response.status != 304) {
    const int length = snprintf(content_length, sizeof(content_length),
                                "Content-Length: %zu\r\n", response.body_length);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(content_length)) return false;
  }
  char etag_header[160]{};
  if (response.etag[0]) {
    const int length = snprintf(etag_header, sizeof(etag_header), "ETag: %s\r\n", response.etag);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(etag_header)) return false;
  }
  char authenticate_header[176]{};
  if (response.www_authenticate[0]) {
    const int length = snprintf(authenticate_header, sizeof(authenticate_header),
                                "WWW-Authenticate: %s\r\n", response.www_authenticate);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(authenticate_header)) return false;
  }
  char location_header[224]{};
  if (response.location[0]) {
    const int length = snprintf(location_header, sizeof(location_header), "Location: %s\r\n", response.location);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(location_header)) return false;
  }
  char next_cursor_header[48]{};
  if (response.next_cursor[0]) {
    const int length = snprintf(next_cursor_header, sizeof(next_cursor_header),
                                "X-Next-Cursor: %s\r\n", response.next_cursor);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(next_cursor_header)) return false;
  }
  char ai_reply_headers[128]{};
  if (response.ai_reply_for[0] && response.ai_reply_status[0])
    snprintf(ai_reply_headers, sizeof(ai_reply_headers),
             "X-AI-Reply-For: %s\r\nX-AI-Reply-Status: %s\r\n",
             response.ai_reply_for, response.ai_reply_status);
  char retry_after[48]{};
  if (response.retry_after) snprintf(retry_after, sizeof(retry_after), "Retry-After: %u\r\n", response.retry_after);
  char quota_headers[96]{};
  if (response.rate_limit_cost >= 0) snprintf(quota_headers, sizeof(quota_headers),
      "x-rate-limit-cost: %d\r\nx-rate-limit-remaining: %u\r\n",
      response.rate_limit_cost, response.rate_limit_remaining);
  char date[64]{};
  if (!format_http_date(date, sizeof(date))) date[0] = '\0';
  // Formatted last so the measurement ends as close to the send as possible.
  char timing[kServerTimingLineBytes];
  if (!format_server_timing(request_start, timing, sizeof(timing))) return false;
  const int count = snprintf(headers, sizeof(headers),
      "HTTP/1.1 %d %s\r\n%s%s"
      "Connection: %s\r\nx-content-type-options: nosniff\r\n"
      "referrer-policy: no-referrer\r\n"
      "permissions-policy: camera=(), microphone=(), geolocation=()\r\n"
      "cache-control: %s\r\n%s%s%s%s%s%s%s%s%s%s%s\r\n",
      response.status, response.reason, content_type,
      content_length, (FindHttpTransport(client) && FindHttpTransport(client)->KeepAlive()) ? "keep-alive" : "close", response.cache_control ? response.cache_control : "no-store",
      vary, retry_after, quota_headers, etag_header, authenticate_header, location_header,
      next_cursor_header, ai_reply_headers,
      response.extra_headers ? response.extra_headers : "", timing, date);
  if (count <= 0 || static_cast<size_t>(count) >= sizeof(headers)) return false;
  timespec deadline{};
  if (!make_deadline(&deadline, response.write_timeout_ms)) return false;
#ifdef SO_NOSIGPIPE
  const int suppress_sigpipe = 1;
  if (setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &suppress_sigpipe,
                 sizeof(suppress_sigpipe)) < 0) return false;
#endif
  int nonblocking = 1;
  if (ioctl(client, FIONBIO, &nonblocking) < 0) return false;
  const bool written = send_all_until(client, headers, static_cast<size_t>(count), deadline) &&
         (response.status == 304 || head || response.body_length == 0 ||
          send_all_until(client, reinterpret_cast<const char*>(response.body), response.body_length, deadline));
  if (auto* transport = FindHttpTransport(client)) transport->Written(written);
  return written;
}

void HttpNoteAuthenticatedDevice(const char* device_id, uint8_t platform) {
  if (!active_response_capture || !device_id) return;
  snprintf(active_response_capture->completion_.device_id,
           sizeof(active_response_capture->completion_.device_id), "%s", device_id);
  active_response_capture->completion_.platform = platform;
}

void HttpNoteDevicePlatform(uint8_t platform) {
  if (active_response_capture) active_response_capture->completion_.platform = platform;
}

void HttpNoteAdmission(HttpAdmission reason) {
  if (active_response_capture) active_response_capture->completion_.admission = reason;
}

void WriteHttpAdmissionOverload(int client, const timespec* dispatch_started) {
  if (auto* transport = FindHttpTransport(client)) {
    transport->DisallowReuse();
    HttpResponse response = JsonResponse(503, "Service Unavailable", "{\"error\":\"OVERLOADED\"}");
    response.write_timeout_ms = 100;
    ResponseWriter::Write(client, response, false, dispatch_started ? *dispatch_started : monotonic_now());
    return;
  }
  // Never hold the accept loop on an overloaded socket.
  static constexpr char prefix[] = "HTTP/1.1 503 Service Unavailable\r\n"
      "Connection: close\r\nRetry-After: 1\r\nCache-Control: no-store\r\n"
      "Content-Length: 0\r\n";
  int nonblocking = 1;
#ifdef SO_NOSIGPIPE
  const int suppress_sigpipe = 1;
  if (setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &suppress_sigpipe,
                 sizeof(suppress_sigpipe)) < 0) return;
#endif
  if (ioctl(client, FIONBIO, &nonblocking) == 0) {
    const uint64_t micros = dispatch_started ? elapsed_microseconds(*dispatch_started) : 0;
    char reply[256];
    const int length = snprintf(reply, sizeof(reply), "%sServer-Timing: app;dur=%llu.%03llu\r\n\r\n",
        prefix, static_cast<unsigned long long>(micros / 1000),
        static_cast<unsigned long long>(micros % 1000));
    if (length < 0 || static_cast<size_t>(length) >= sizeof(reply)) return;
#ifdef MSG_NOSIGNAL
    (void)send(client, reply, static_cast<size_t>(length), MSG_NOSIGNAL);
#else
    (void)send(client, reply, static_cast<size_t>(length), 0);
#endif
    // Queue the FIN before close and discard already-buffered request bytes.
    // The fixed nonblocking drain limits work even for a streaming peer.
    shutdown(client, SHUT_WR);
    char discarded[1024];
    for (unsigned i = 0; i < 16; ++i)
      if (recv(client, discarded, sizeof(discarded), 0) <= 0) break;
  }
}

ResponseHandoff::ResponseHandoff() {
  for (size_t i = 0; i < kWriterCount; ++i) active_clients_[i] = -1;
}

ResponseHandoff::~ResponseHandoff() { Stop(); }

bool ResponseHandoff::Start() {
  fortis::base::UniqueLock lock(mutex_);
  if (started_writers_ == kWriterCount) return true;
  if (started_writers_ != 0 || stopping_) return false;
  lock.Unlock();
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) return false;
  if (pthread_attr_setstacksize(&attributes, 128 * 1024) != 0) {
    pthread_attr_destroy(&attributes);
    return false;
  }
  for (size_t i = 0; i < kWriterCount; ++i) {
    contexts_[i] = {this, i};
    if ((writers_[i].StartEntry<WriterMain>(&contexts_[i], &attributes) ? 0 : -1) != 0) {
      pthread_attr_destroy(&attributes);
      Stop();
      return false;
    }
    lock.Lock();
    ++started_writers_;
    lock.Unlock();
  }
  pthread_attr_destroy(&attributes);
  return true;
}

void ResponseHandoff::Stop() {
  int pending_clients[kCapacity];
  std::unique_ptr<uint8_t[], base::StorageDeleter> pending_bodies[kCapacity];
  HttpHandoffTerminalFn pending_terminals[kCapacity];
  void* pending_contexts[kCapacity];
  size_t pending_count = 0;
  fortis::base::UniqueLock lock(mutex_);
  stopping_ = true;
  for (size_t i = 0; i < kCapacity; ++i) {
    if (!pending_[i].occupied) continue;
    pending_clients[pending_count] = pending_[i].client;
    pending_bodies[pending_count] = std::move(pending_[i].body);
    pending_terminals[pending_count] = pending_[i].terminal;
    pending_contexts[pending_count] = pending_[i].terminal_context;
    ++pending_count;
    pending_[i] = Pending{};
  }
  for (size_t i = 0; i < kWriterCount; ++i)
    if (active_clients_[i] >= 0) shutdown(active_clients_[i], SHUT_RDWR);
  available_.Broadcast();
  const size_t count = started_writers_;
  lock.Unlock();
  for (size_t i = 0; i < pending_count; ++i) {
    CloseHttpConnection(pending_clients[i]);
    pending_bodies[i].reset();
    if (pending_terminals[i]) fortis::base::FatalBoundary("HTTP stop terminal callback", [&] {
      pending_terminals[i](pending_contexts[i], true);
    });
  }
  for (size_t i = 0; i < count; ++i) writers_[i].Join();
  lock.Lock();
  started_writers_ = 0;
}

bool ResponseHandoff::Submit(int client, const HttpResponse& response,
                                 bool head, uint32_t delay_ms,
                                 const timespec* request_start) {
  if (client < 0 || response.body_length > kCapturedResponseLimit ||
      (response.body_length && !response.body) || delay_ms > 10000 ||
      !valid_response_headers(response)) return false;
  std::unique_ptr<uint8_t[], base::StorageDeleter> body;
  if (response.body_length) {
    body = base::AllocateStorage<uint8_t[]>(response.body_length);
    if (!body) return false;
    memcpy(body.get(), response.body, response.body_length);
  }
  fortis::base::UniqueLock lock(mutex_);
  if (stopping_ || started_writers_ != kWriterCount) {
    lock.Unlock();
    body.reset();
    return false;
  }
  size_t slot = kCapacity;
  for (size_t i = 0; i < kCapacity; ++i)
    if (!pending_[i].occupied) { slot = i; break; }
  if (slot == kCapacity) {
    lock.Unlock();
    body.reset();
    return false;
  }
  Pending& pending = pending_[slot];
  pending.occupied = true;
  pending.ready = true;
  pending.completion = HttpCompletionTag{};
  pending.client = client;
  pending.token = 0;
  pending.response = response;
  if (response.admission != HttpAdmission::None) pending.completion.admission = response.admission;
  snprintf(pending.reason, sizeof(pending.reason), "%s", response.reason);
  pending.response.reason = pending.reason;
  if (response.content_type) {
    snprintf(pending.content_type, sizeof(pending.content_type), "%s", response.content_type);
    pending.response.content_type = pending.content_type;
  } else {
    pending.response.content_type = nullptr;
  }
  pending.body = std::move(body);
  pending.response.body = pending.body.get();
  pending.head = head;
  pending.request_start = request_start ? *request_start : monotonic_now();
  pending.due_ms = monotonic_milliseconds() + delay_ms;
  available_.Broadcast();
  return true;
}

bool ResponseHandoff::Reserve(int client, bool head, uint32_t timeout_ms,
                                  uint64_t* token) {
  return Reserve(client, head, timeout_ms, token, nullptr, nullptr);
}

bool ResponseHandoff::Reserve(int client, bool head, uint32_t timeout_ms,
                                  uint64_t* token, HttpHandoffTerminalFn terminal,
                                  void* terminal_context,
                                  const HttpCompletionTag* completion,
                                  const timespec* request_start) {
  if (client < 0 || !token || timeout_ms == 0 || timeout_ms > 30000) return false;
  fortis::base::LockGuard lock(mutex_);
  if (stopping_ || started_writers_ != kWriterCount) return false;
  size_t slot = kCapacity;
  for (size_t i = 0; i < kCapacity; ++i)
    if (!pending_[i].occupied) { slot = i; break; }
  if (slot == kCapacity) return false;
  uint64_t reserved_token = next_token_++;
  if (reserved_token == 0) reserved_token = next_token_++;
  Pending& pending = pending_[slot];
  pending = Pending{};
  pending.occupied = true;
  pending.ready = false;
  pending.client = client;
  pending.token = reserved_token;
  pending.head = head;
  // Complete never touches it: the writer's Server-Timing (ready or 504)
  // measures from the original Dispatch entry.
  pending.request_start = request_start ? *request_start : monotonic_now();
  pending.due_ms = monotonic_milliseconds() + timeout_ms;
  pending.terminal = terminal;
  pending.terminal_context = terminal_context;
  if (completion) pending.completion = *completion;
  *token = reserved_token;
  available_.Broadcast();
  return true;
}

bool ResponseHandoff::Complete(uint64_t token, const HttpResponse& response) {
  if (token == 0 || response.body_length > kCapturedResponseLimit ||
      (response.body_length && !response.body) ||
      !valid_response_headers(response)) return false;
  std::unique_ptr<uint8_t[], base::StorageDeleter> body;
  if (response.body_length) {
    body = base::AllocateStorage<uint8_t[]>(response.body_length);
    if (!body) return false;
    memcpy(body.get(), response.body, response.body_length);
  }
  fortis::base::UniqueLock lock(mutex_);
  const uint64_t now = monotonic_milliseconds();
  for (size_t i = 0; i < kCapacity; ++i) {
    Pending& pending = pending_[i];
    if (!pending.occupied || pending.ready || pending.token != token) continue;
    if (now >= pending.due_ms || stopping_) break;
    pending.response = response;
    if (response.admission != HttpAdmission::None) pending.completion.admission = response.admission;
    pending.completion.owner_ms = response.owner_ms;
    snprintf(pending.reason, sizeof(pending.reason), "%s", response.reason);
    pending.response.reason = pending.reason;
    if (response.content_type) {
      snprintf(pending.content_type, sizeof(pending.content_type), "%s", response.content_type);
      pending.response.content_type = pending.content_type;
    } else {
      pending.response.content_type = nullptr;
    }
    pending.response.body = body.get();
    pending.body = std::move(body);
    pending.ready = true;
    pending.token = 0;
    pending.due_ms = now;
    const auto terminal = pending.terminal;
    void* terminal_context = pending.terminal_context;
    pending.terminal = nullptr;
    pending.terminal_context = nullptr;
    available_.Broadcast();
    lock.Unlock();
    if (terminal) fortis::base::FatalBoundary("HTTP terminal callback", [&] { terminal(terminal_context, false); });
    return true;
  }
  lock.Unlock();
  body.reset();
  return false;
}

bool ResponseHandoff::FailReservation(uint64_t token, int status) {
  if (!token || (status != 0 && status != 500 && status != 503)) return false;
  fortis::base::UniqueLock lock(mutex_);
  for (auto& pending : pending_) {
    if (!pending.occupied || pending.ready || pending.token != token) continue;
    const auto terminal = pending.terminal;
    void* context = pending.terminal_context;
    const int client = pending.client;
    const HttpCompletionTag completion = pending.completion;
    if (status == 0) {
      pending = Pending{};
    } else {
      // Static error bytes keep OOM recovery independent of allocation.
      pending.response = status == 503
          ? JsonResponse(503, "Service Unavailable", "{\"error\":\"UNAVAILABLE\"}")
          : JsonResponse(500, "Internal Server Error", "{\"error\":\"INTERNAL_ERROR\"}");
      snprintf(pending.reason, sizeof(pending.reason), "%s", pending.response.reason);
      snprintf(pending.content_type, sizeof(pending.content_type), "%s", pending.response.content_type);
      pending.response.reason = pending.reason;
      pending.response.content_type = pending.content_type;
      pending.ready = true;
      pending.token = 0;
      pending.due_ms = monotonic_milliseconds();
      pending.terminal = nullptr;
      pending.terminal_context = nullptr;
    }
    available_.Broadcast();
    lock.Unlock();
    if (status == 0) CloseHttpConnection(client);
    if (terminal) fortis::base::FatalBoundary("HTTP failed reservation terminal", [&] { terminal(context, true); });
    if (status == 0) report_completion(completion, 500);
    return true;
  }
  // A completed/timed-out reservation already belongs to its writer.
  return false;
}

void* ResponseHandoff::WriterMain(void* opaque) {
  auto* context = static_cast<WriterContext*>(opaque);
  context->owner->WriterLoop(context->index);
  return nullptr;
}

void ResponseHandoff::WriterLoop(size_t index) {
  for (;;) {
    Pending item{};
    fortis::base::UniqueLock lock(mutex_);
    for (;;) {
      if (stopping_) return;
      size_t selected = kCapacity;
      uint64_t earliest = UINT64_MAX;
      for (size_t i = 0; i < kCapacity; ++i) {
        if (pending_[i].occupied && pending_[i].due_ms < earliest) {
          selected = i;
          earliest = pending_[i].due_ms;
        }
      }
      if (selected == kCapacity) {
        available_.WaitLocked(mutex_);
        continue;
      }
      const uint64_t now = monotonic_milliseconds();
      if (earliest > now) {
        const uint64_t wait_ms = earliest - now > 100 ? 100 : earliest - now;
        const timespec wake = realtime_after_milliseconds(wait_ms);
        available_.WaitUntilLocked(mutex_, wake);
        int reserved_fds[kCapacity];
        uint64_t reserved_tokens[kCapacity];
        size_t reserved_count = 0;
        for (size_t i = 0; i < kCapacity; ++i) {
          if (pending_[i].occupied && !pending_[i].ready &&
              pending_[i].client >= 0) {
            reserved_fds[reserved_count] = pending_[i].client;
            reserved_tokens[reserved_count++] = pending_[i].token;
          }
        }
        lock.Unlock();
        for (size_t i = 0; i < reserved_count; ++i) {
          lock.Lock();
          Pending* candidate = nullptr;
          for (auto& pending : pending_) {
            if (pending.occupied && !pending.ready && pending.token == reserved_tokens[i]) {
              candidate = &pending;
              break;
            }
          }
          if (!candidate) { lock.Unlock(); continue; }
          // TLS records cannot be inspected with a raw socket peek. Its
          // bounded reservation deadline and eventual TLS write detect loss.
          if (FindHttpTransport(candidate->client)) { lock.Unlock(); continue; }
          // Keep the reservation owned until the nonblocking peek completes.
          int nonblocking = 1;
          if (ioctl(candidate->client, FIONBIO, &nonblocking) != 0) {
            lock.Unlock();
            continue;
          }
          pollfd descriptor{reserved_fds[i], POLLIN, 0};
          if (poll(&descriptor, 1, 0) <= 0 ||
              !(descriptor.revents & (POLLIN | POLLHUP | POLLERR))) {
            lock.Unlock();
            continue;
          }
          char byte = 0;
          int flags = MSG_PEEK;
#ifdef MSG_DONTWAIT
          flags |= MSG_DONTWAIT;
#endif
          const ssize_t received = recv(reserved_fds[i], &byte, 1, flags);
          if (received > 0 ||
              (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))) {
            lock.Unlock();
            continue;
          }
          const int disconnected_client = candidate->client;
          auto disconnected_body = std::move(candidate->body);
          const auto disconnected_terminal = candidate->terminal;
          void* disconnected_context = candidate->terminal_context;
          *candidate = Pending{};
          lock.Unlock();
          if (disconnected_client >= 0) CloseHttpConnection(disconnected_client);
          disconnected_body.reset();
          if (disconnected_terminal) fortis::base::FatalBoundary("HTTP terminal callback", [&] { disconnected_terminal(disconnected_context, true); });
        }
        lock.Lock();
        continue;
      }
      item = std::move(pending_[selected]);
      item.response.reason = item.reason;
      if (item.content_type[0]) item.response.content_type = item.content_type;
      pending_[selected] = Pending{};
      active_clients_[index] = item.client;
      break;
    }
    lock.Unlock();

    // Writer threads never have an active capture; the explicit start makes
    // Server-Timing cover the dispatch, the owner's work or the deferral
    // delay, and the wait for this writer.
    if (item.ready) {
      ResponseWriter::Write(item.client, item.response, item.head, item.request_start);
    } else {
      const HttpResponse timeout = JsonResponse(504, "Gateway Timeout",
                                                "{\"error\":\"TIMEOUT\"}");
      ResponseWriter::Write(item.client, timeout, item.head, item.request_start);
    }
    // A reserved socket's final status is known only here; submitted
    // (deferred) responses carry no tag and were reported at dispatch.
    report_completion(item.completion, item.ready ? item.response.status : 504);
    lock.Lock();
    active_clients_[index] = -1;
    available_.Broadcast();
    lock.Unlock();
    CloseHttpConnection(item.client);
    item.body.reset();
    if (item.terminal) fortis::base::FatalBoundary("HTTP terminal callback", [&] { item.terminal(item.terminal_context, !item.ready); });
  }
}

HttpReply::~HttpReply() = default;

bool HttpReply::Defer(uint32_t delay_ms) {
  if (!written_ || deferred_ || reserved_ || delay_ms > 10000) return false;
  deferred_ = true;
  delay_ms_ = delay_ms;
  return true;
}

bool HttpReply::Reserve(uint32_t timeout_ms, uint64_t* token, HttpHandoffTerminalFn terminal,
                        void* terminal_context) {
  return ReserveSocket(client_, request_head_, timeout_ms, token, terminal, terminal_context);
}

bool HttpReply::ReserveSocket(int client, bool head, uint32_t timeout_ms, uint64_t* token,
                              HttpHandoffTerminalFn terminal, void* terminal_context) {
  if (!handoff_ || reserved_ || written_) return false;
  const HttpCompletionTag* completion = (completion_.hook || completion_.details) ? &completion_ : nullptr;
  // Hand the Dispatch entry time to the handoff with the socket, so the
  // eventual Server-Timing (completion or 504) covers the whole request.
  if (!handoff_->Reserve(client, head, timeout_ms, token, terminal, terminal_context, completion,
                         &request_start_))
    return false;
  reserved_ = true;
  reservation_token_ = *token;
  return true;
}

bool HttpReply::Capture(const HttpResponse& response, bool head) {
  if (!valid_response_headers(response) || (response.body_length && !response.body))
    return false;
  if (reserved_) return false;
  if (written_ || response.body_length > kCapturedResponseLimit ||
      (response.body_length && !response.body)) {
    overflow_ = true;
    return false;
  }
  if (response.body_length && response.static_body) {
    response_ = response;
  } else if (response.body_length) {
    uint8_t* body = inline_body_;
    if (response.body_length > sizeof(inline_body_)) {
      heap_body_ = base::AllocateStorage<uint8_t[]>(response.body_length);
      body = heap_body_.get();
    }
    if (!body) {
      overflow_ = true;
      allocation_failed_ = true;
      return false;
    }
    memcpy(body, response.body, response.body_length);
    response_ = response;
    response_.body = body;
  } else {
    response_ = response;
  }
  snprintf(reason_, sizeof(reason_), "%s", response.reason);
  response_.reason = reason_;
  if (response.content_type) {
    snprintf(content_type_, sizeof(content_type_), "%s", response.content_type);
    response_.content_type = content_type_;
  } else {
    response_.content_type = nullptr;
  }
  head_ = head;
  written_ = true;
  return true;
}

// ---- raw-fd migration adapter (see HttpRouteHandler in http_server.h) ----

bool DeferHttpResponse(uint32_t delay_ms) {
  return active_response_capture && active_response_capture->Defer(delay_ms);
}

bool ReserveHttpResponse(int client, bool head, uint32_t timeout_ms,
                         uint64_t* token, HttpHandoffTerminalFn terminal,
                         void* terminal_context) {
  return active_response_capture &&
         active_response_capture->ReserveSocket(client, head, timeout_ms, token, terminal,
                                                terminal_context);
}

bool WriteHttpResponse(int client, const HttpResponse& response, bool head) {
  if (active_response_capture) return active_response_capture->Capture(response, head);
  if (!valid_response_headers(response) || (response.body_length && !response.body))
    return false;
  // Outside a dispatch (tests, tools) the duration starts at this call.
  return ResponseWriter::Write(client, response, head, monotonic_now());
}

HttpResponse JsonResponse(int status, const char* reason, const char* json) {
  return {status, reason, "application/json",
          reinterpret_cast<const uint8_t*>(json), strlen(json)};
}

bool AcceptsProtobuf(const HttpRequest& request) {
  if (!request.accept) return false;
  const char* part = request.accept;
  while (*part) {
    const char* end = strchr(part, ',');
    if (!end) end = part + strlen(part);
    const char* media_start = part;
    while (media_start < end && (*media_start == ' ' || *media_start == '\t')) ++media_start;
    const char* media_end = media_start;
    while (media_end < end && *media_end != ';') ++media_end;
    while (media_end > media_start && (media_end[-1] == ' ' || media_end[-1] == '\t')) --media_end;
    bool protobuf = media_end - media_start == 20;
    static constexpr char kType[] = "application/protobuf";
    if (protobuf) {
      for (size_t i = 0; i < sizeof(kType) - 1; ++i) {
        char c = media_start[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if (c != kType[i]) { protobuf = false; break; }
      }
    }
    bool positive_quality = true;
    const char* parameter = media_end;
    while (parameter < end) {
      if (*parameter == ';') ++parameter;
      while (parameter < end && (*parameter == ' ' || *parameter == '\t')) ++parameter;
      const char* parameter_end = parameter;
      while (parameter_end < end && *parameter_end != ';') ++parameter_end;
      const char* trimmed_end = parameter_end;
      while (trimmed_end > parameter && (trimmed_end[-1] == ' ' || trimmed_end[-1] == '\t')) --trimmed_end;
      if (trimmed_end - parameter >= 2 && parameter[0] == 'q' && parameter[1] == '=') {
        char quality_text[64];
        size_t length = static_cast<size_t>(trimmed_end - (parameter + 2));
        if (length >= sizeof(quality_text)) { positive_quality = false; break; }
        memcpy(quality_text, parameter + 2, length);
        quality_text[length] = '\0';
        char* number_end = nullptr;
        const double quality = strtod(quality_text, &number_end);
        while (number_end && (*number_end == ' ' || *number_end == '\t')) ++number_end;
        positive_quality = number_end != quality_text && number_end && *number_end == '\0' &&
                           !isnan(quality) && quality > 0;
        break;
      }
      parameter = parameter_end;
    }
    if (protobuf && positive_quality) return true;
    part = *end ? end + 1 : end;
  }
  return false;
}

bool ConditionalStructuredResponse(const HttpRequest& request,
                                   const char* name_space,
                                   const HttpResponse& json,
                                   const HttpResponse& protobuf,
                                   HttpResponse* response,
                                   const uint8_t* json_validator,
                                   size_t json_validator_length,
                                   const uint8_t* protobuf_validator,
                                   size_t protobuf_validator_length) {
  if (!response || !valid_namespace(name_space)) return false;
  const bool use_protobuf = AcceptsProtobuf(request);
  const HttpResponse& selected = use_protobuf ? protobuf : json;
  if (!safe_header_value(selected.content_type, 127) ||
      (selected.body_length && !selected.body)) return false;
  const uint8_t* validator = use_protobuf ? protobuf_validator : json_validator;
  size_t validator_length = use_protobuf ? protobuf_validator_length : json_validator_length;
  if (!validator && validator_length == 0) {
    validator = selected.body;
    validator_length = selected.body_length;
  } else if (!validator) {
    return false;
  }
  unsigned char digest[32];
  static const unsigned char kEmpty = 0;
  if (mbedtls_sha256(validator ? validator : &kEmpty, validator_length,
                     digest, 0) != 0) return false;
  char hex[25];
  for (size_t i = 0; i < 12; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
  hex[24] = '\0';

  *response = selected;
  const int etag_length = snprintf(response->etag, sizeof(response->etag),
      use_protobuf ? "W/\"%s-protobuf-%s\"" : "W/\"%s-%s\"",
      name_space, hex);
  if (etag_length <= 0 || static_cast<size_t>(etag_length) >= sizeof(response->etag)) return false;
  response->vary_accept = use_protobuf;
  if (request.if_none_match && strcmp(request.if_none_match, response->etag) == 0) {
    response->status = 304;
    response->reason = "Not Modified";
    response->content_type = nullptr;
    response->body = nullptr;
    response->body_length = 0;
  }
  return true;
}

namespace {
bool route_param_char(char c, bool first) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (!first && ((c >= '0' && c <= '9') || c == '_'));
}

bool valid_route_pattern(const char* path) {
  if (!path || path[0] != '/') return false;
  size_t params = 0;
  for (const char* segment = path + 1; *segment;) {
    const char* end = strchr(segment, '/');
    if (!end) end = segment + strlen(segment);
    if (end == segment) return false;
    if (*segment == ':') {
      const size_t length = static_cast<size_t>(end - segment - 1);
      if (!length || length > kHttpPathParamNameBytes ||
          ++params > kHttpMaxPathParams) return false;
      for (size_t i = 0; i < length; ++i)
        if (!route_param_char(segment[i + 1], i == 0)) return false;
    } else {
      for (const char* p = segment; p < end; ++p)
        if (*p == ':') return false;
    }
    segment = *end ? end + 1 : end;
    if (*end && !*segment) return false;
  }
  return true;
}

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Percent-decodes one segment into `param`. The request parser has already
// refused malformed escapes and a decoded path that is not strict UTF-8, so
// the units are counted on valid UTF-8 (a 4-byte sequence is two units).
// An empty segment, a control character/DEL and a value over
// kHttpPathParamValueBytes do not match (the router answers 404 before any
// handler). The decoding itself is what the app relies on: it
// percent-encodes ids, so "%2F" must stay inside the value
// (docs/compat-decisions.md#path-encoding).
bool decode_param(const char* begin, const char* end, HttpPathParam* param) {
  char* output = param->value;
  size_t used = 0;
  param->utf16_units = 0;
  for (const char* p = begin; p < end; ++p) {
    unsigned char c = static_cast<unsigned char>(*p);
    if (c == '%') {
      if (end - p < 3) return false;
      const int high = hex_digit(p[1]), low = hex_digit(p[2]);
      if (high < 0 || low < 0) return false;
      c = static_cast<unsigned char>((high << 4) | low);
      p += 2;
    }
    if ((c & 0xc0) != 0x80) param->utf16_units += c >= 0xf0 ? 2 : 1;
    if (c < 0x20 || c == 0x7f || used == kHttpPathParamValueBytes) return false;
    output[used++] = static_cast<char>(c);
  }
  output[used] = '\0';
  return used != 0;
}

bool route_path_matches(const char* pattern, const char* path,
                        HttpPathParam* params, size_t* param_count,
                        size_t* specificity) {
  *param_count = 0;
  *specificity = 0;
  const char* a = pattern;
  const char* b = path;
  size_t segment_index = 0;
  while (*a || *b) {
    if (*a != '/' || *b != '/') return false;
    ++a; ++b;
    const char* a_end = strchr(a, '/');
    const char* b_end = strchr(b, '/');
    if (!a_end) a_end = a + strlen(a);
    if (!b_end) b_end = b + strlen(b);
    if (a == a_end || b == b_end)
      return a == a_end && b == b_end && !*a_end && !*b_end;
    if (*a == ':') {
      if (*param_count == kHttpMaxPathParams) return false;
      HttpPathParam& param = params[(*param_count)++];
      const size_t name_length = static_cast<size_t>(a_end - a - 1);
      memcpy(param.name, a + 1, name_length);
      param.name[name_length] = '\0';
      if (!decode_param(b, b_end, &param)) return false;
    } else {
      const size_t length = static_cast<size_t>(a_end - a);
      if (length != static_cast<size_t>(b_end - b) || memcmp(a, b, length) != 0) return false;
      // Route precedence: a literal at an earlier segment wins over a
      // literal at a later segment, regardless of the later segment's length.
      if (segment_index >= sizeof(size_t) * 8) return false;
      *specificity |= size_t{1} << (sizeof(size_t) * 8 - 1 - segment_index);
    }
    a = a_end;
    b = b_end;
    ++segment_index;
  }
  return true;
}

// GET /metrics exposition, shared by RenderPrometheus and the worst-case
// bound HttpRouter::Add enforces, so the two can never disagree.
constexpr char kPrometheusHeader[] =
    "# HELP fortis_native_http_requests_total Completed native HTTP requests by fixed route template.\n"
    "# TYPE fortis_native_http_requests_total counter\n"
    "# HELP fortis_native_http_errors_total Native HTTP responses with status 500 or greater by fixed route template.\n"
    "# TYPE fortis_native_http_errors_total counter\n"
    "# HELP fortis_native_http_request_duration_ms_sum Sum of native HTTP request durations in milliseconds by fixed route template.\n"
    "# TYPE fortis_native_http_request_duration_ms_sum counter\n"
    "# HELP fortis_native_http_request_duration_ms_max Maximum native HTTP request duration in milliseconds by fixed route template.\n"
    "# TYPE fortis_native_http_request_duration_ms_max gauge\n";
// Arguments: route label, then requests, errors, duration sum and max. The
// label is either "METHOD /template" or "other"; Add admits only printable
// ASCII without '"' or '\\', so no label ever needs escaping.
constexpr char kPrometheusRouteSamples[] =
    "fortis_native_http_requests_total{route=\"%s\"} %llu\n"
    "fortis_native_http_errors_total{route=\"%s\"} %llu\n"
    "fortis_native_http_request_duration_ms_sum{route=\"%s\"} %.3f\n"
    "fortis_native_http_request_duration_ms_max{route=\"%s\"} %.3f\n";

// Bytes kPrometheusRouteSamples takes for `label` with every counter
// saturated (RecordMetrics saturates instead of wrapping).
size_t prometheus_worst_case_samples(const char* label) {
  const unsigned long long count = UINT64_MAX;
  const double milliseconds = static_cast<double>(UINT64_MAX) / 1000.0;
  const int length = snprintf(nullptr, 0, kPrometheusRouteSamples, label, count, label,
                              count, label, milliseconds, label, milliseconds);
  return length > 0 ? static_cast<size_t>(length) : 0;
}
}  // namespace

bool HttpRouter::Add(const char* method, const char* path, HttpHandler handler,
                     void* context) {
  return handler && AddRoute(method, path, handler, nullptr, context);
}

bool HttpRouter::Add(const char* method, const char* path, HttpRouteHandler handler,
                     void* context) {
  return handler && AddRoute(method, path, nullptr, handler, context);
}

bool HttpRouter::AddRoute(const char* method, const char* path, HttpHandler handler,
                          HttpRouteHandler raw_handler, void* context) {
  if (!method || !path || (!handler && !raw_handler)) return false;
  const size_t method_length = strnlen(method, 16);
  const size_t path_length = strnlen(path, 128);
  if (!method_length || method_length >= 16 || !path_length || path_length >= 128 ||
      !valid_route_pattern(path)) return false;
  for (const char* p = method; *p; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c < 0x21 || c >= 0x7f || c == '"' || c == '\\') return false;
  }
  for (const char* p = path; *p; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c < 0x21 || c >= 0x7f || c == '"' || c == '\\') return false;
  }
  for (size_t i = 0; i < route_count_; ++i) {
    const Route& route = routes_[i];
    if (strcmp(route.method, method) == 0 && strcmp(route.path, path) == 0) return false;
  }
  if (route_count_ == kMaxRoutes) return false;
  // A route whose saturated samples would push the scrape past its budget is
  // refused here, at startup, rather than turning every later scrape into a
  // 500 once enough routes have been hit.
  char label[16 + 128];
  snprintf(label, sizeof(label), "%s %s", method, path);
  const size_t registered_bytes = PrometheusWorstCaseBytes();
  const size_t route_bytes = prometheus_worst_case_samples(label);
  if (!route_bytes || registered_bytes > kMetricsRouteOutputBudget ||
      route_bytes > kMetricsRouteOutputBudget - registered_bytes) return false;
  metrics_worst_case_bytes_ = registered_bytes + route_bytes;
  routes_[route_count_++] = {method, path, handler, raw_handler, context};
  return true;
}

size_t HttpRouter::PrometheusWorstCaseBytes() const {
  // The header and a saturated "other" block are always possible; the
  // registered routes add their own saturated blocks.
  if (metrics_worst_case_bytes_) return metrics_worst_case_bytes_;
  return sizeof(kPrometheusHeader) - 1 + prometheus_worst_case_samples("other");
}

void HttpRouter::RecordMetrics(size_t route_index, int status, uint64_t duration_us) {
  if (route_index > kMaxRoutes) route_index = kMaxRoutes;
  fortis::base::LockGuard metrics_lock(metrics_mutex_);
  RouteMetrics& sample = metrics_[route_index];
  saturating_increment(&sample.requests);
  if (status >= 500) saturating_increment(&sample.errors);
  saturating_add(&sample.duration_us, duration_us);
  if (duration_us > sample.max_duration_us) sample.max_duration_us = duration_us;
}

size_t HttpRouter::RenderPrometheus(char* output, size_t capacity) {
  if (!output || !capacity) return 0;
  size_t used = 0;
  output[0] = '\0';
  // The /metrics schema is deliberately small: lifetime route
  // request/error counters and elapsed-time sum/max
  // only. It has no 10-minute windows, histogram buckets, client counts,
  // V8/process-memory fields, DB/cache/coordinator/provider metrics, or labels
  // beyond fixed registered route templates plus the aggregate "other".
  // Add keeps the saturated output within kMetricsRouteOutputBudget, so a
  // caller passing at least that capacity never sees a size failure.
  fortis::base::LockGuard metrics_lock(metrics_mutex_);
  bool ok = append_format(output, capacity, &used, "%s", kPrometheusHeader);
  char label[16 + 128];
  for (size_t i = 0; ok && i < route_count_; ++i) {
    const RouteMetrics& sample = metrics_[i];
    if (!sample.requests) continue;
    const Route& route = routes_[i];
    snprintf(label, sizeof(label), "%s %s", route.method, route.path);
    ok = append_format(output, capacity, &used, kPrometheusRouteSamples,
        label, static_cast<unsigned long long>(sample.requests),
        label, static_cast<unsigned long long>(sample.errors),
        label, static_cast<double>(sample.duration_us) / 1000.0,
        label, static_cast<double>(sample.max_duration_us) / 1000.0);
  }
  const RouteMetrics& other = metrics_[kMaxRoutes];
  if (ok && other.requests) {
    ok = append_format(output, capacity, &used, kPrometheusRouteSamples,
        "other", static_cast<unsigned long long>(other.requests),
        "other", static_cast<unsigned long long>(other.errors),
        "other", static_cast<double>(other.duration_us) / 1000.0,
        "other", static_cast<double>(other.max_duration_us) / 1000.0);
  }
  return ok ? used : 0;
}

HttpQueueTimings ReadHttpQueueTimings() {
  HttpQueueTimings result;
  for (size_t series = 0; series < HttpQueueTimings::Count; ++series)
    for (size_t i = 0; i < HttpQueueTimings::kBuckets; ++i)
      result.buckets[series][i] = (queue_timings.buckets[series][i]).load(std::memory_order_relaxed);
  return result;
}

bool HttpRouter::Dispatch(int client, const char* remote_ip, const timespec* accepted) {
  return ConnectionSession(*this).Dispatch(client, remote_ip, accepted);
}

bool ConnectionSession::Dispatch(int client, const char* remote_ip, const timespec* accepted,
    base::FunctionRef<HttpReadResult(int, HttpRequest*)> reader) {
  using Route = HttpRouter::Route;
  auto& routes_ = router_.routes_;
  auto& route_count_ = router_.route_count_;
  auto& handoff_ = router_.handoff_;
  auto& unlocked_path_ = router_.unlocked_path_;
  auto& completion_hook_ = router_.completion_hook_;
  auto& completion_details_ = router_.completion_details_;
  auto& details_context_ = router_.details_context_;
  auto& completion_context_ = router_.completion_context_;

  timespec dispatch_start{};
  clock_gettime(CLOCK_MONOTONIC, &dispatch_start);
  const uint64_t queue_us = accepted ? elapsed_microseconds(*accepted) : 0;
  if (accepted) record_queue_timing(HttpQueueTimings::Dispatch, queue_us);
  // Leave time for parsing and route work. The dedicated health listener,
  // when configured, dispatches without a queue timestamp.
  constexpr uint64_t kMaxAcceptQueueAgeUs = 1000000;
  if (queue_us >= kMaxAcceptQueueAgeUs) {
    WriteHttpAdmissionOverload(client, &dispatch_start);
    router_.RecordMetrics(HttpRouter::kMaxRoutes, 503, elapsed_microseconds(dispatch_start));
    if (completion_details_) {
      HttpCompletionTag tag{};
      tag.admission = HttpAdmission::ConnectionQueueExpired;
      fortis::base::FatalBoundary("HTTP expired queue completion", [&] {
        completion_details_(details_context_, tag, 503, 0);
      });
    }
    return false;
  }
  const double queue_ms = static_cast<double>(queue_us) / 1000.0;
  ParsedRequest parsed;
  HttpRequest& request = parsed.get();
  HttpReadResult read = HttpReadResult::Unavailable;
  int parse_exception_status = 0;
  try {
    if (reader) read = reader(client, &request);
  } catch (const std::bad_alloc&) {
    fortis::base::LogBoundaryException("HTTP request parser", "std::bad_alloc");
    parse_exception_status = 503;
  } catch (const std::exception&) {
    fortis::base::LogBoundaryException("HTTP request parser", "std::exception");
    parse_exception_status = 500;
  } catch (...) {
    // Parsing owns only this request's buffers, before any application state
    // changes or socket handoff. Unwinding can safely discard partial input.
    fortis::base::LogBoundaryException("HTTP request parser", "unknown exception");
    parse_exception_status = 500;
  }
  if (parse_exception_status) {
    const bool head = request.method && strcmp(request.method, "HEAD") == 0;
    write_error(client, parse_exception_status,
                parse_exception_status == 503 ? "Service Unavailable" : "Internal Server Error",
                parse_exception_status == 503 ? "{\"error\":\"UNAVAILABLE\"}" : "{\"error\":\"INTERNAL_ERROR\"}",
                head, dispatch_start);
    router_.RecordMetrics(HttpRouter::kMaxRoutes, parse_exception_status, elapsed_microseconds(dispatch_start));
    return false;
  }
  if (read != HttpReadResult::Ok) {
    const bool head = request.method && strcmp(request.method, "HEAD") == 0;
    int status = 400;
    switch (read) {
      case HttpReadResult::Empty: break;
      case HttpReadResult::HeaderTooLarge:
        status = 431;
        write_error(client, 431, "Request Header Fields Too Large", "{\"error\":\"HEADERS_TOO_LARGE\"}", head,
                    dispatch_start); break;
      case HttpReadResult::BodyTooLarge:
        status = 413;
        write_error(client, 413, "Payload Too Large", "{\"error\":\"BODY_TOO_LARGE\"}", head,
                    dispatch_start); break;
      case HttpReadResult::UnsupportedEncoding:
        status = 415;
        write_error(client, 415, "Unsupported Media Type", "{\"error\":\"UNSUPPORTED_CONTENT_ENCODING\"}", head,
                    dispatch_start); break;
      case HttpReadResult::Timeout:
        status = 408;
        write_error(client, 408, "Request Timeout", "{\"error\":\"REQUEST_TIMEOUT\"}", head,
                    dispatch_start); break;
      case HttpReadResult::Unavailable:
        status = 503;
        write_error(client, 503, "Service Unavailable", "{\"error\":\"UNAVAILABLE\"}", head,
                    dispatch_start); break;
      default: write_error(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", head,
                           dispatch_start); break;
    }
    if (read != HttpReadResult::Empty)
      router_.RecordMetrics(HttpRouter::kMaxRoutes, status, elapsed_microseconds(dispatch_start));
    return false;
  }
  if (remote_ip) snprintf(request.remote_ip, sizeof(request.remote_ip), "%s", remote_ip);
  const bool head = strcmp(request.method, "HEAD") == 0;
  const Route* match = nullptr;
  size_t match_index = HttpRouter::kMaxRoutes;
  size_t best_specificity = 0;
  int best_method_priority = -1;
  HttpPathParam best_params[kHttpMaxPathParams]{};
  HttpPathParam candidate[kHttpMaxPathParams]{};
  size_t best_param_count = 0;
  for (size_t i = 0; i < route_count_; ++i) {
    const Route& route = routes_[i];
    size_t candidate_count = 0, specificity = 0;
    if (!route_path_matches(route.path, request.path, candidate,
                            &candidate_count, &specificity)) continue;
    const int method_priority = strcmp(route.method, request.method) == 0 ? 2 :
                                (head && strcmp(route.method, "GET") == 0) ? 1 : 0;
    if (method_priority &&
        (!match || specificity > best_specificity ||
         (specificity == best_specificity && method_priority > best_method_priority))) {
      match = &route;
      match_index = i;
      best_specificity = specificity;
      best_method_priority = method_priority;
      best_param_count = candidate_count;
      if (candidate_count)
        memcpy(best_params, candidate, candidate_count * sizeof(HttpPathParam));
    }
  }
  if (!match) {
    // A known path with an unregistered method is 404, the same answer as
    // for an unknown path (there is no 405).
    write_error(client, 404, "Not Found", "{\"error\":\"NOT_FOUND\"}", head, dispatch_start);
    router_.RecordMetrics(HttpRouter::kMaxRoutes, 404, elapsed_microseconds(dispatch_start));
  } else {
    request.path_param_count = best_param_count;
    if (best_param_count)
      memcpy(request.path_params, best_params, best_param_count * sizeof(HttpPathParam));
    // Keep current KV, quota, and route state single-writer until those
    // components gain finer-grained locks. Platform and TLS probes only read
    // configuration and perform outbound I/O, so they do not need the lock.
    // The public operations pages read only their own mutex-guarded state, so
    // a refresh flood never queues behind API work on the route lock.
    // Device enrollment, rotation and revocation are exempted; see
    // route_runs_without_state_lock for the audit that allows it. The AI
    // history read (GET|HEAD) is audited in
    // ai_history_read_runs_without_state_lock, the network catalog
    // (GET|HEAD /v1/networks) in network_catalog_runs_without_state_lock and
    // the spot price read (GET|HEAD /v1/prices) in
    // spot_prices_run_without_state_lock. Health and the pure probes are
    // audited in probe_runs_without_state_lock.
    const bool without_state_lock = strcmp(match->path, "/probe/tls") == 0 ||
                                    strcmp(match->path, "/probe/platform") == 0 ||
                                    (unlocked_path_ && strcmp(match->path, unlocked_path_) == 0) ||
                                    strcmp(match->path, "/metrics") == 0 ||
                                    strcmp(match->path, "/") == 0 ||
                                    strcmp(match->path, "/health") == 0 ||
                                    strcmp(match->path, "/health.json") == 0 ||
                                    strcmp(match->path, "/node.json") == 0 ||
                                    strcmp(match->path, "/assets/background.jpg") == 0 ||
                                    strcmp(match->path, "/statusz") == 0 ||
                                    strcmp(match->path, "/stats") == 0 ||
                                    strcmp(match->path, "/benchmark") == 0 ||
                                    strcmp(match->path, "/statsz") == 0 ||
                                    strcmp(match->path, "/errors") == 0 ||
                                    strcmp(match->path, "/error") == 0 ||
                                    strcmp(match->path, "/errorsz") == 0 ||
                                    route_runs_without_state_lock(match->method, match->path) ||
                                    ai_history_read_runs_without_state_lock(match->method,
                                                                            match->path) ||
                                    network_catalog_runs_without_state_lock(match->method,
                                                                            match->path) ||
                                    spot_prices_run_without_state_lock(match->method,
                                                                       match->path) ||
                                    price_snapshot_runs_without_state_lock(match->method,
                                                                           match->path) ||
                                    probe_runs_without_state_lock(match->method, match->path);
    HttpReply capture(head);
    capture.client_ = client;
    capture.handoff_ = handoff_;
    capture.request_start_ = dispatch_start;
    const bool scraped = strcmp(match->path, "/metrics") == 0;
    if ((completion_hook_ || completion_details_) && !scraped) {
      capture.completion_.details = completion_details_;
      capture.completion_.details_context = details_context_;
      capture.completion_.method = head ? "HEAD" : match->method;
      capture.completion_.queue_ms = queue_ms;
      capture.completion_.hook = completion_hook_;
      capture.completion_.context = completion_context_;
      capture.completion_.route = match->path;
      capture.completion_.start = dispatch_start;
    }
    int exception_status = 0;
    {
      ActiveResponseScope active(&capture);
      try {
        if (without_state_lock) {
          if (match->handler) match->handler(request, capture, match->context);
          else match->raw_handler(client, request, head, match->context);
        } else {
          const bool read_only = head || strcmp(match->method, "GET") == 0;
          timespec wait_start{}, hold_start{};
          clock_gettime(CLOCK_MONOTONIC, &wait_start);
          fortis::base::UniqueLock route_state_lock(route_state_mutex);
          clock_gettime(CLOCK_MONOTONIC, &hold_start);
          const uint64_t waited = elapsed_microseconds(wait_start);
          if (match->handler) match->handler(request, capture, match->context);
          else match->raw_handler(client, request, head, match->context);
          const uint64_t held = elapsed_microseconds(hold_start);
          route_state_lock.Unlock();
          record_queue_timing(read_only ? HttpQueueTimings::ReadLockWait :
                             HttpQueueTimings::MutationLockWait, waited);
          record_queue_timing(read_only ? HttpQueueTimings::ReadLockHold :
                             HttpQueueTimings::MutationLockHold, held);
        }
      } catch (const std::bad_alloc&) {
        fortis::base::LogBoundaryException(match->path, "std::bad_alloc");
        exception_status = 503;
      } catch (const std::exception&) {
        fortis::base::LogBoundaryException(match->path, "std::exception");
        exception_status = 500;
      } catch (...) {
        fortis::base::LogBoundaryException(match->path, "unknown exception");
        std::terminate();
      }
    }
    if (exception_status) {
      if (capture.reserved_) {
        // Ownership transferred at Reserve, even if a completion raced with
        // this throw. The worker must never close the transferred descriptor.
        handoff_->FailReservation(capture.reservation_token_, match->handler ? exception_status : 0);
        if (!scraped) router_.RecordMetrics(match_index, exception_status, elapsed_microseconds(dispatch_start));
        return true;
      }
      if (!match->handler) {
        // Legacy handlers may have written directly: never append an error
        // response. The caller retains close ownership; shutdown ends I/O.
        shutdown(client, SHUT_RDWR);
        if (!scraped) router_.RecordMetrics(match_index, exception_status, elapsed_microseconds(dispatch_start));
        if (!scraped) report_completion(capture.completion_, exception_status);
        return false;
      }
      capture.heap_body_.reset();
      capture.written_ = capture.overflow_ = capture.allocation_failed_ = capture.deferred_ = false;
      capture.Send(exception_status == 503
          ? JsonResponse(503, "Service Unavailable", "{\"error\":\"UNAVAILABLE\"}")
          : JsonResponse(500, "Internal Server Error", "{\"error\":\"INTERNAL_ERROR\"}"));
    }

    if (capture.overflow_) {
      write_error(client, capture.allocation_failed_ ? 503 : 500,
                  capture.allocation_failed_ ? "Service Unavailable" : "Internal Server Error",
                  capture.allocation_failed_ ? "{\"error\":\"UNAVAILABLE\"}" :
                                             "{\"error\":\"RESPONSE_TOO_LARGE\"}", head,
                  dispatch_start);
    } else if (!capture.written_ && !capture.reserved_) {
      write_error(client, 500, "Internal Server Error", "{\"error\":\"NO_RESPONSE\"}", head,
                  dispatch_start);
    }
    // Every head below goes out through write_timed_response or
    // write_handoff_overload with dispatch_start, so each carries one
    // Server-Timing line measured from this Dispatch's entry; a deferred
    // response hands the same start to the handoff writer.
    bool transferred = capture.reserved_;
    if (capture.deferred_ && capture.written_ && !capture.overflow_ && handoff_) {
      transferred = handoff_->Submit(client, capture.response_, capture.head_,
                                     capture.delay_ms_, &dispatch_start);
      if (!transferred) {
        write_handoff_overload(client, capture.head_, dispatch_start);
      }
    }
    if (capture.written_ && !capture.overflow_ && !transferred && !capture.deferred_)
      ResponseWriter::Write(client, capture.response_, capture.head_, dispatch_start);
    if (capture.deferred_ && !transferred && !capture.overflow_ && !handoff_)
      write_handoff_overload(client, capture.head_, dispatch_start);
    // For a reserved socket the final HTTP status is not known at dispatch
    // time; metrics record route acceptance and elapsed dispatch time only.
    const int status = capture.reserved_ ? 0 :
                       (transferred || (capture.written_ && !capture.overflow_ && !capture.deferred_)
                           ? capture.response_.status
                           : ((capture.deferred_ || capture.allocation_failed_) ? 503 : 500));
    // The metrics route is not part of the API telemetry: scrapes and their
    // auth failures are excluded from the route counters.
    if (!scraped) {
      router_.RecordMetrics(match_index, status, elapsed_microseconds(dispatch_start));
      if (!capture.reserved_) report_completion(capture.completion_, status);
    }
    return transferred;
  }
  return false;
}

}  // namespace fortis::http
