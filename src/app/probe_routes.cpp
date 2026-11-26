#include "probe_routes.h"
#include "log.h"

#include "ai_config.h"
#include "app_support.h"
#include "composition_hooks.h"
#include "device_challenge.h"
#include "device_enrollment.h"
#include "evm_proof.h"
#include "guest_allocator_metrics.h"
#include "kv_expiry_sweeper.h"
#include "kv_probe.h"
#include "kv_runtime.h"
#include "kv_store.h"
#include "metrics_auth.h"
#include "price_history_batch_codec.h"
#include "protobuf_probe.h"
#include "storage_probe.h"
#include "tls_probe.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef FORTIS_UNIKRAFT
// The vendored lwIP header pulls Unikraft C headers that are not C++ clean.
// LWIP_DNS_API_DECLARE_STRUCTS=0 uses the POSIX addrinfo layout from netdb.h.
extern "C" int lwip_getaddrinfo(const char*, const char*, const struct addrinfo*,
                                struct addrinfo**);
extern "C" void lwip_freeaddrinfo(struct addrinfo*);
#endif

namespace fortis::app {
namespace {

bool can_connect_tcp(const addrinfo* target) {
  if (!target || !target->ai_addr || target->ai_addrlen == 0) return false;
  // lwIP's getaddrinfo returns sockaddr_storage length even for IPv4. The
  // POSIX socket shim expects the actual sockaddr_in length for connect().
  const socklen_t address_length = target->ai_family == AF_INET
      ? static_cast<socklen_t>(sizeof(sockaddr_in)) : target->ai_addrlen;
  if (target->ai_addrlen < address_length) return false;
  const int fd = socket(target->ai_family, target->ai_socktype, target->ai_protocol);
  if (fd < 0) {
    fortis::base::LogErrno(fortis::base::LogLevel::kError, "probe", "platform probe: socket", errno);
    return false;
  }
  int nonblocking = 1;
  if (ioctl(fd, FIONBIO, &nonblocking) < 0) {
    fortis::base::LogErrno(fortis::base::LogLevel::kError, "probe", "platform probe: set nonblocking", errno);
    close(fd);
    return false;
  }
  bool connected = connect(fd, target->ai_addr, address_length) == 0;
  if (!connected && errno == EINPROGRESS) {
    pollfd descriptor{fd, POLLOUT, 0};
    const int ready = poll(&descriptor, 1, 2000);
    if (ready > 0 && !(descriptor.revents & POLLNVAL)) {
      int socket_error = 0;
      socklen_t error_size = sizeof(socket_error);
      connected = getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) == 0 &&
                  socket_error == 0;
      if (!connected) fortis::base::LogError("probe", "platform probe: socket error=%d errno=%d",
                              socket_error, errno);
    } else {
      fortis::base::LogError("probe", "platform probe: poll result=%d errno=%d", ready, errno);
    }
  } else if (!connected) {
    fortis::base::LogErrno(fortis::base::LogLevel::kError, "probe", "platform probe: connect", errno);
  }
  close(fd);
  return connected;
}

static_assert(operations::kGuestAllocatorMetricsMaxBytes < http::HttpRouter::kMetricsOutputCapacity / 4,
              "allocator gauges must leave route metrics most of the output bound");
static_assert(operations::kGuestAllocatorMetricsMaxBytes + storage::kKvCompactionMetricsMaxBytes <
                  http::HttpRouter::kMetricsOutputCapacity / 4,
              "allocator and compaction gauges must leave route metrics most of the output bound");
static_assert(http::HttpRouter::kMetricsRouteOutputBudget + storage::kKvCompactionMetricsMaxBytes +
                  operations::kGuestAllocatorMetricsMaxBytes <= http::HttpRouter::kMetricsOutputCapacity,
              "a saturated route table plus every appended gauge must fit one scrape");

// GET /metrics (operation prometheusMetrics) and its HEAD twin: aggregate
// Prometheus text for an operator's scraper. Counterpart of the TypeScript
// /metrics route.
//
// Routing. Registered for GET and HEAD on the literal path "/metrics"; the
// query string is ignored (it is neither parsed nor echoed, and can never
// authenticate). "/metrics/", "/METRICS" and other methods are not this
// route: the router answers its generic 404. The route is outside the device
// API: no device bearer, no quota, no browser-origin refusal, and it does not
// take the router's route-state lock (it reads only mutex-guarded counters).
// HEAD runs the same checks and builds the same body; WriteHttpResponse then
// sends the headers, including the GET body's Content-Length, and no body.
//
// Enablement. METRICS_TOKEN (native_config.cpp) is trimmed of SP/HTAB/CR/LF;
// unset or blank leaves the route disabled, and every request, with or
// without credentials, gets 404 with an empty body, Content-Length: 0 and no
// Content-Type (TypeScript's `reply.code(404).send()`). A trimmed token over
// 512 bytes (or a raw value over 4096) stops startup instead.
//
// Authorization. MatchesMetricsBearer (metrics_auth.h): the header must be
// exactly "Bearer <token>" (case-sensitive scheme, one space, exact bytes),
// compared in time independent of the mismatch position. Otherwise 401 with
// body {"error":"UNAUTHORIZED"}, Content-Type application/json;
// charset=utf-8 and WWW-Authenticate: Bearer realm="fortis-metrics". The
// token and the presented header are never echoed, logged or stored.
//
// Success. 200, Content-Type text/plain; version=0.0.4; charset=utf-8,
// cache-control no-store, plus the router's fixed security headers. The body
// is, in order: HttpRouter::RenderPrometheus (lifetime request, 5xx-error,
// duration-sum and duration-max samples per registered route template that
// has served at least one request, and an aggregate route="other" for
// unmatched and unparseable requests); then, once the process KV store is
// open, the fortis_kv_* compaction gauges (kv_expiry_sweeper.cpp); then, on
// guest images built with CONFIG_LIBUKALLOC_IFSTATS only, the
// fortis_guest_allocator_* gauges. Labels are registered templates (":param"
// names, never values), so no address, device, query or raw path appears.
//
// Effects. Read-only. It never opens the KV store (RuntimeKvStoreIfOpen),
// never touches the database or a provider, and allocates nothing beyond the
// router's response capture. Reading compaction stats takes the KV store
// mutex briefly, so a scrape can wait behind a long KV critical section.
// Every /metrics response — 200, 401, 404 and HEAD — is excluded from the
// route counters and from the API telemetry completion hook (Dispatch), as
// TypeScript registers the route outside its telemetry plugin; a scrape
// never counts itself.
//
// Bounds. The body is built in one kMetricsOutputCapacity (64 KiB) stack
// buffer. The router refuses at registration any route that would make a
// saturated table exceed kMetricsRouteOutputBudget (48 KiB), and the static
// asserts above reserve room for both appended gauge sets, so the size check
// below is defensive: a formatting failure answers 500 {"error":"INTERNAL"}
// rather than a truncated exposition. A missing route context also answers
// 500 (a wiring bug, unreachable in the shipped registration).
//
// Intentional differences from TypeScript. The metric families are native,
// not TypeScript's: TypeScript exports process/V8 memory, event-loop delay,
// SWR, 10-minute windows, histogram buckets, database, coordinator, lane,
// cache and upstream-work series under fortis_* names with route labels like
// "/v1/networks"; native exports fortis_native_http_* lifetime counters with
// labels like "GET /v1/networks", plus the KV compaction and allocator
// gauges TypeScript does not have. Native counts every matched route except
// /metrics (TypeScript counts only device-API routes) and adds "other".
// Dashboards and alerts written for one runtime do not apply to the other.
// Request parsing (http_request.cpp), not this handler, differs too: a
// repeated Authorization header is 400 natively (Node keeps the first), and
// a percent-encoded path such as /%6Detrics is 404 natively (Fastify decodes
// it and serves the route). Native compares header bytes; Node decodes the
// header as Latin-1 against a UTF-8 token, so a non-ASCII METRICS_TOKEN only
// works natively. The 401 carries cache-control: no-store (TypeScript sends
// none).
void handle_metrics(int client, const http::HttpRequest& request, bool head, void* opaque) {
  const auto* context = static_cast<const MetricsRouteContext*>(opaque);
  if (!context || !context->config || !context->router) {
    respond(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", head);
    return;
  }
  const ProbeRoutesConfig& config = *context->config;
  if (!config.metrics_enabled) {
    http::HttpResponse disabled{404, "Not Found", nullptr, nullptr, 0};
    disabled.omit_content_type = true;
    WriteHttpResponse(client, disabled, head);
    return;
  }
  if (!operations::MatchesMetricsBearer(request.authorization, config.metrics_token)) {
    http::HttpResponse unauthorized = http::JsonResponse(401, "Unauthorized",
                                             "{\"error\":\"UNAUTHORIZED\"}");
    unauthorized.content_type = "application/json; charset=utf-8";
    snprintf(unauthorized.www_authenticate, sizeof(unauthorized.www_authenticate),
             "Bearer realm=\"fortis-metrics\"");
    WriteHttpResponse(client, unauthorized, head);
    return;
  }
  char body[http::HttpRouter::kMetricsOutputCapacity];
  // Guest images built with CONFIG_LIBUKALLOC_IFSTATS append fixed allocator
  // gauges inside the same 64 KiB bound; host builds collect nothing and
  // keep the route-only output and capacity.
  FortisGuestAllocatorStats allocator{};
  const bool has_allocator = operations::CollectGuestAllocatorSample(&allocator);
  // Compaction counters follow the routes once the process store is open
  // (a scrape never opens it).
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStoreIfOpen();
  const size_t route_capacity = sizeof(body) - storage::kKvCompactionMetricsMaxBytes -
      (has_allocator ? operations::kGuestAllocatorMetricsMaxBytes : 0);
  size_t length = context->router->RenderPrometheus(body, route_capacity);
  if (length && kv) {
    fortis::storage::KvCompactionStats compaction;
    kv->CompactionStats(&compaction);
    if (!AppendKvCompactionMetrics(compaction, body, sizeof(body), &length)) length = 0;
  }
  if (length && has_allocator &&
      !operations::AppendGuestAllocatorMetrics(allocator, body, sizeof(body), &length)) {
    length = 0;
  }
  if (!length) {
    respond(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", head);
    return;
  }
  const http::HttpResponse metrics{200, "OK", "text/plain; version=0.0.4; charset=utf-8",
      reinterpret_cast<const uint8_t*>(body), length};
  WriteHttpResponse(client, metrics, head);
}

}  // namespace

// GET|HEAD /healthz, /probe, /probe/representation and POST
// /probe/protobuf run without the router's route_state_mutex (audited in
// http_server.cpp's probe_runs_without_state_lock), as do /probe/tls and
// /probe/platform. Those branches may touch only request-local data,
// constants, and state with its own owner lock; anything else must be added
// under review or the route removed from that list. The /probe/kv and
// /probe/storage and /probe/evm branches stay under the route lock. Every
// branch except /healthz, /probe, /probe/representation and /probe/protobuf
// is reachable only when RegisterProbeRoutes registered the diagnostic
// probes (FORTIS_ENABLE_DIAGNOSTIC_PROBES=1).
void HandleProbe(int client, const http::HttpRequest& request, bool head, void* context) {
  const auto* probes = static_cast<const ProbeRoutesConfig*>(context);

  const char* method = request.method;
  const char* path = request.path;
  const size_t content_length = request.body_length;
  const bool has_content_length = request.has_content_length;
  const bool protobuf_content_type = request.protobuf_content_type;
  if (strcmp(path, "/probe/representation") == 0) {
    if (content_length != 0) {
      respond(client, 405, "Method Not Allowed", "{\"error\":\"METHOD_NOT_ALLOWED\"}");
      return;
    }
    http::HttpResponse response;
    if (AcceptsProtobuf(request)) {
      static const uint8_t encoded[] = {0x0a, 0x05, 'p', 'r', 'o', 'b', 'e'};
      response = {200, "OK", "application/protobuf", encoded, sizeof(encoded)};
    } else {
      response = http::JsonResponse(200, "OK", "{\"representation\":\"json\"}");
    }
    response.vary_accept = true;
    WriteHttpResponse(client, response, head);
    return;
  }
  if (strcmp(method, "POST") == 0 && strcmp(path, "/probe/protobuf") == 0) {
    if (!has_content_length || !protobuf_content_type || content_length == 0) {
      respond(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}");
      return;
    }
    if (content_length > kProbeBody) {
      respond(client, 413, "Payload Too Large", "{\"error\":\"BODY_TOO_LARGE\"}");
      return;
    }
    uint8_t encoded[kProbeBody]{};
    size_t encoded_size = 0;
    if (!codec::protobuf_roundtrip(reinterpret_cast<uint8_t*>(request.body),
                            content_length, encoded, sizeof(encoded), &encoded_size)) {
      respond(client, 400, "Bad Request", "{\"error\":\"BAD_PROTOBUF\"}");
      return;
    }
    respond_bytes(client, 200, "OK", "application/protobuf", encoded, encoded_size);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/probe/kv") == 0 &&
             content_length == 0) {
    uint64_t counter = 0;
    if (!storage::kv_probe_increment(&counter)) {
      respond(client, 503, "Service Unavailable", "{\"error\":\"KV_UNAVAILABLE\"}");
      return;
    }
    char body[64];
    snprintf(body, sizeof(body), "{\"counter\":%llu}",
             static_cast<unsigned long long>(counter));
    respond(client, 200, "OK", body);
  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/probe/storage") == 0 &&
             content_length == 0) {
    uint64_t counter = 0;
    if (!storage::storage_probe_increment(&counter)) {
      respond(client, 503, "Service Unavailable", "{\"error\":\"STORAGE_UNAVAILABLE\"}");
      return;
    }
    char body[64];
    snprintf(body, sizeof(body), "{\"counter\":%llu}",
             static_cast<unsigned long long>(counter));
    respond(client, 200, "OK", body);
  } else if ((strcmp(method, "GET") != 0 && !head) || content_length != 0) {
    respond(client, 405, "Method Not Allowed",
            "{\"error\":\"METHOD_NOT_ALLOWED\"}");
  } else if (strcmp(path, "/healthz") == 0) {
    const bool healthy = storage::kv_probe_healthy();
    const ai::AiServiceConfig* config = probes ? probes->ai : nullptr;
    char body[1024];
    fortis::codec::history_batch::OutBuffer out{reinterpret_cast<uint8_t*>(body), sizeof(body)};
    out.Text(healthy ? "{\"ok\":true" : "{\"ok\":false");
    if (config && config->native_ai_canary &&
        ((config->ai_model_configured && config->ai_provider_configured &&
          strcmp(config->ai_provider, "anthropic") == 0 && config->anthropic_api_key_path_configured) ||
         (config->openai_model_configured && config->openai_api_key_configured))) {
      out.Text(",\"supportedModels\":[");
      bool comma = false;
      if (config->ai_model_configured && config->ai_provider_configured &&
          strcmp(config->ai_provider, "anthropic") == 0 && config->anthropic_api_key_path_configured) {
        fortis::codec::history_batch::AppendJsonString(out, config->ai_model, strlen(config->ai_model));
        comma = true;
      }
      if (config->openai_model_configured && config->openai_api_key_configured) {
        if (comma) out.Byte(',');
        fortis::codec::history_batch::AppendJsonString(out, config->openai_model, strlen(config->openai_model));
      }
      out.Byte(']');
    }
    out.Byte('}');
    if (!out.ok) { respond(client, 503, "Service Unavailable", "{\"ok\":false}", head); return; }
    respond_bytes(client, healthy ? 200 : 503, healthy ? "OK" : "Service Unavailable",
                  "application/json", reinterpret_cast<const uint8_t*>(body), out.used, head);
  } else if (strcmp(path, "/probe") == 0) {
    respond(client, 200, "OK",
            "{\"service\":\"fortis_node\",\"stage\":\"bootstrap\"}", head);
  } else if (strcmp(path, "/probe/evm") == 0) {
    constexpr char message[] = "FortisNet native proof";
    constexpr char signature[] =
        "0x16467bd18cd195a85b71883fc36e55c3248928f0e9113cf3f20c670be21c83f8"
        "1b331e42b83f7d524157cbef6611dfbf737c80657b0d7ab40e2127b12bbc6fe01c";
    char address[43];
    const bool ok = auth::evm_recover_address(reinterpret_cast<const uint8_t*>(message),
                                        strlen(message), signature, address) &&
                    strcmp(address, "0xfcad0b19bb29d4674531d6f115237e16afce377c") == 0;
    respond(client, ok ? 200 : 503, ok ? "OK" : "Service Unavailable",
            ok ? "{\"valid\":true}" : "{\"valid\":false}", head);
  } else if (strcmp(path, "/probe/platform") == 0) {
    timespec now{};
    const bool clock_ok = clock_gettime(CLOCK_REALTIME, &now) == 0 &&
                          now.tv_sec > 1704067200;
    uint8_t random_bytes[32]{};
#ifdef __APPLE__
    const bool entropy_ok = getentropy(random_bytes, sizeof(random_bytes)) == 0;
#else
    const bool entropy_ok = getrandom(random_bytes, sizeof(random_bytes), 0) ==
                            static_cast<ssize_t>(sizeof(random_bytes));
#endif
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
#ifdef FORTIS_UNIKRAFT
    const int dns_result = lwip_getaddrinfo("example.com", "443", &hints, &addresses);
#else
    const int dns_result = getaddrinfo("example.com", "443", &hints, &addresses);
#endif
    const bool dns_ok = dns_result == 0 && addresses != nullptr;
    const bool tcp_ok = dns_ok && can_connect_tcp(addresses);
    if (!dns_ok) fortis::base::LogError("probe", "platform probe: DNS lookup failed: %d", dns_result);
    if (dns_ok && !tcp_ok) fortis::base::LogError("probe", "platform probe: outbound TCP failed");
    if (addresses) {
#ifdef FORTIS_UNIKRAFT
      lwip_freeaddrinfo(addresses);
#else
      freeaddrinfo(addresses);
#endif
    }
    char body[160];
    snprintf(body, sizeof(body),
             "{\"clock\":%s,\"entropy\":%s,\"dns\":%s,\"tcp\":%s}",
             clock_ok ? "true" : "false", entropy_ok ? "true" : "false",
             dns_ok ? "true" : "false", tcp_ok ? "true" : "false");
    const bool platform_ok = clock_ok && entropy_ok && dns_ok && tcp_ok;
    respond(client, platform_ok ? 200 : 503,
            platform_ok ? "OK" : "Service Unavailable", body, head);
  } else if (strcmp(path, "/probe/storage") == 0) {
    uint64_t counter = 0;
    if (!storage::storage_probe_read(&counter)) {
      respond(client, 503, "Service Unavailable", "{\"error\":\"STORAGE_UNAVAILABLE\"}", head);
      return;
    }
    char body[64];
    snprintf(body, sizeof(body), "{\"counter\":%llu}",
             static_cast<unsigned long long>(counter));
    respond(client, 200, "OK", body, head);
  } else if (strcmp(path, "/probe/kv") == 0) {
    uint64_t counter = 0;
    if (!storage::kv_probe_read(&counter)) {
      respond(client, 503, "Service Unavailable", "{\"error\":\"KV_UNAVAILABLE\"}", head);
      return;
    }
    char body[64];
    snprintf(body, sizeof(body), "{\"counter\":%llu}",
             static_cast<unsigned long long>(counter));
    respond(client, 200, "OK", body, head);
  } else if (strcmp(path, "/probe/tls") == 0) {
    const ProbeRoutesConfig* config = probes;
    if (!config || !config->tls_probe_configured) {
      respond(client, 503, "Service Unavailable",
              "{\"error\":\"TLS_PROBE_NOT_CONFIGURED\"}", head);
      return;
    }
    const http::TlsProbeResult result = http::tls_probe_https(config->tls_probe_host,
        config->tls_probe_port, config->tls_probe_ca);
    char body[160];
    snprintf(body, sizeof(body),
             "{\"connected\":%s,\"verified\":%s,\"response\":%s,\"error\":%d}",
             result.connected ? "true" : "false",
             result.verified ? "true" : "false",
             result.response ? "true" : "false", result.error);
    respond(client, result.response ? 200 : 503,
            result.response ? "OK" : "Service Unavailable", body, head);
  } else {
    respond(client, 404, "Not Found", "{\"error\":\"NOT_FOUND\"}", head);
  }
}

bool RegisterProbeRoutes(http::HttpRouter& router, ProbeRoutesConfig* config,
                         auth::DeviceEnrollmentConfig* devices,
                         MetricsRouteContext* metrics_context,
                         CompositionHooks& hooks) {
  if (!config) return false;
  const char* get_paths[] = {"/healthz", "/probe", "/probe/representation"};
  for (const char* path : get_paths) {
    if (!router.Add("GET", path, HandleProbe, config) ||
        !router.Add("HEAD", path, HandleProbe, config)) return false;
  }
  if (config->diagnostic_probes_enabled) {
    const char* diagnostic_paths[] = {"/probe/evm", "/probe/platform", "/probe/storage",
                                      "/probe/kv", "/probe/tls"};
    for (const char* path : diagnostic_paths) {
      if (!router.Add("GET", path, HandleProbe, config) ||
          !router.Add("HEAD", path, HandleProbe, config)) return false;
    }
    if (!router.Add("POST", "/probe/kv", HandleProbe, config) ||
        !router.Add("POST", "/probe/storage", HandleProbe, config)) return false;
  }
  if (!hooks.RegisterProbeRoutes(router)) return false;
  return router.Add("POST", "/probe/protobuf", HandleProbe, config) &&
         router.Add("GET", "/metrics", handle_metrics, metrics_context) &&
         router.Add("HEAD", "/metrics", handle_metrics, metrics_context) &&
         auth::RegisterDeviceChallengeRoute(router) &&
         RegisterDeviceEnrollmentRoute(router, devices) &&
         RegisterDeviceRotationRoute(router, devices) &&
         RegisterDeviceRevocationRoute(router, devices);
}

}  // namespace fortis::app
