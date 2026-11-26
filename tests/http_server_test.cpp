#include "test_support.h"
#include "../src/http/http_server.h"
#include "../src/http/http_middleware.h"
#include "../src/auth/device_policy_step.h"
#include "../src/storage/kv_probe.h"
#include "../src/storage/kv_runtime.h"
#include "../src/codec/protobuf_probe.h"
#include "conditional_fixtures.h"

#include <assert.h>
#include <atomic>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <poll.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <string>
#include <vector>

using namespace fortis::auth;
using namespace fortis::codec;
using namespace fortis::http;
using namespace fortis::storage;

namespace {
void check_high_socket_descriptor() {
  constexpr int high_fd = 1024;
  rlimit limit{};
  assert(getrlimit(RLIMIT_NOFILE, &limit) == 0);
  if (limit.rlim_max <= high_fd) return;
  if (limit.rlim_cur <= high_fd) {
    const rlimit raised{high_fd + 1, limit.rlim_max};
    assert(setrlimit(RLIMIT_NOFILE, &raised) == 0);
  }
  int pair[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  const int server = fcntl(pair[0], F_DUPFD, high_fd);
  assert(server >= high_fd);
  close(pair[0]);
  HttpRouter router;
  assert(router.Add("GET", "/healthz", [](int client, const HttpRequest&, bool head, void*) {
    WriteHttpResponse(client, JsonResponse(200, "OK", "{}"), head);
  }, nullptr));
  constexpr char request[] = "GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n";
  assert(write(pair[1], request, sizeof(request) - 1) == sizeof(request) - 1);
  assert(!router.Dispatch(server));
  char response[256]{};
  assert(read(pair[1], response, sizeof(response) - 1) > 0);
  assert(strncmp(response, "HTTP/1.1 200", 12) == 0);
  close(server);
  close(pair[1]);
  assert(setrlimit(RLIMIT_NOFILE, &limit) == 0);
}

struct TerminalState { std::atomic<int> calls{0}; std::atomic<int> cancelled{-1}; };
void terminal_callback(void* opaque, bool cancelled) {
  auto* state = static_cast<TerminalState*>(opaque);
  state->cancelled.store(cancelled ? 1 : 0);
  state->calls.fetch_add(1);
}
bool wait_for_terminal(const TerminalState& state) {
  for (size_t attempt = 0; attempt < 1000; ++attempt) {
    if (state.calls.load() != 0) return true;
    timespec pause{0, 1000 * 1000};
    nanosleep(&pause, nullptr);
  }
  return state.calls.load() != 0;
}
void no_op(int, const HttpRequest&, bool, void*) {}

void large_response(int client, const HttpRequest&, bool, void* context) {
  const size_t length = *static_cast<size_t*>(context);
  auto* body = static_cast<uint8_t*>(malloc(length));
  assert(body);
  memset(body, 'x', length);
  const HttpResponse response{200, "OK", "application/octet-stream", body, length};
  WriteHttpResponse(client, response);
  free(body);
}

void health_response(int client, const HttpRequest&, bool head, void*) {
  const HttpResponse response = JsonResponse(200, "OK", "{}");
  WriteHttpResponse(client, response, head);
}

void path_param_response(int client, const HttpRequest& request, bool head, void*) {
  const char* id = HttpPathParamValue(request, "id");
  assert(id && !HttpPathParamValue(request, "missing"));
  char body[600];
  const int length = snprintf(body, sizeof(body), "{\"id\":\"%s\"}", id);
  assert(length > 0 && static_cast<size_t>(length) < sizeof(body));
  assert(WriteHttpResponse(client, JsonResponse(200, "OK", body), head));
}

std::string dispatch_raw(HttpRouter& router, const char* raw) {
  int pair[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  assert(send(pair[1], raw, strlen(raw), 0) == static_cast<ssize_t>(strlen(raw)));
  shutdown(pair[1], SHUT_WR);
  router.Dispatch(pair[0]);
  close(pair[0]);
  std::string response;
  char bytes[1024];
  for (;;) {
    const ssize_t count = recv(pair[1], bytes, sizeof(bytes), 0);
    if (!count) break;
    assert(count > 0);
    response.append(bytes, static_cast<size_t>(count));
  }
  close(pair[1]);
  return response;
}

void test_route_params() {
  HttpRouter router;
  assert(router.Add("GET", "/v1/:id/longliteral", health_response));
  assert(router.Add("GET", "/v1/news/:id", path_param_response));
  assert(router.Add("GET", "/v1/news/reactions", health_response));
  assert(!router.Add("GET", "/v1/news/:", path_param_response));
  assert(!router.Add("GET", "/v1/news/:123", path_param_response));
  assert(!router.Add("GET", "/v1/news/:id/", path_param_response));
  const std::string fixed = dispatch_raw(router,
      "GET /v1/news/reactions HTTP/1.1\r\nHost: test\r\n\r\n");
  assert(fixed.find("HTTP/1.1 200") == 0 && fixed.find("\r\n\r\n{}") != std::string::npos);
  const std::string dynamic = dispatch_raw(router,
      "GET /v1/news/a%2Fb?ignored=1 HTTP/1.1\r\nHost: test\r\n\r\n");
  assert(dynamic.find("HTTP/1.1 200") == 0 &&
         dynamic.find("\r\n\r\n{\"id\":\"a/b\"}") != std::string::npos);
  const std::string earlier_literal = dispatch_raw(router,
      "GET /v1/news/longliteral HTTP/1.1\r\nHost: test\r\n\r\n");
  assert(earlier_literal.find("\r\n\r\n{\"id\":\"longliteral\"}") !=
         std::string::npos);
  const std::string head = dispatch_raw(router,
      "HEAD /v1/news/123 HTTP/1.1\r\nHost: test\r\n\r\n");
  assert(head.find("HTTP/1.1 200") == 0 && head.find("{\"id\":") == std::string::npos);
  const std::string wrong_method = dispatch_raw(router,
      "POST /v1/news/123 HTTP/1.1\r\nHost: test\r\nContent-Length: 0\r\n\r\n");
  assert(wrong_method.find("HTTP/1.1 404") == 0);
  const std::string invalid_escape = dispatch_raw(router,
      "GET /v1/news/a%ZZ HTTP/1.1\r\nHost: test\r\n\r\n");
  assert(invalid_escape.find("HTTP/1.1 400") == 0);
  const std::string invalid_utf8 = dispatch_raw(router,
      "GET /v1/news/%FF HTTP/1.1\r\nHost: test\r\n\r\n");
  assert(invalid_utf8.find("HTTP/1.1 400") == 0);
  const std::string long_path = "GET /" + std::string(1024, 'a') +
      " HTTP/1.1\r\nHost: test\r\n\r\n";
  assert(dispatch_raw(router, long_path.c_str()).find("HTTP/1.1 400") == 0);
  const std::string valid_utf8 = dispatch_raw(router,
      "GET /v1/news/%E2%82%AC HTTP/1.1\r\nHost: test\r\n\r\n");
  assert(valid_utf8.find("HTTP/1.1 200") == 0 &&
         valid_utf8.find("\r\n\r\n{\"id\":\"\xe2\x82\xac\"}") != std::string::npos);
  // opaque-params: an empty segment, a decoded control character
  // or DEL and a value over 512 bytes never reach a handler: 404 NOT_FOUND
  // from the router, for every route.
  for (const char* target : {"/v1/news/", "/v1/news/%00", "/v1/news/a%09b", "/v1/news/%7F", "/v1/news/%1F"}) {
    const std::string request = std::string("GET ") + target + " HTTP/1.1\r\nHost: test\r\n\r\n";
    const std::string refused = dispatch_raw(router, request.c_str());
    assert(refused.find("HTTP/1.1 404") == 0 && refused.find("{\"error\":\"NOT_FOUND\"}") != std::string::npos);
  }
  const std::string at_bound = "GET /v1/news/" + std::string(512, 'a') + " HTTP/1.1\r\nHost: test\r\n\r\n";
  assert(dispatch_raw(router, at_bound.c_str()).find("HTTP/1.1 200") == 0);
  const std::string over_bound = "GET /v1/news/" + std::string(513, 'a') + " HTTP/1.1\r\nHost: test\r\n\r\n";
  assert(dispatch_raw(router, over_bound.c_str()).find("HTTP/1.1 404") == 0);
}

void ephemeral_body_response(int client, const HttpRequest&, bool, void*) {
  uint8_t body[] = "captured-after-handler-return";
  char reason[] = "OK";
  char content_type[] = "text/plain";
  const HttpResponse response{200, reason, content_type, body, sizeof(body) - 1};
  assert(WriteHttpResponse(client, response));
  memset(body, 'z', sizeof(body) - 1);
  memset(reason, 'r', sizeof(reason) - 1);
  memset(content_type, 'c', sizeof(content_type) - 1);
}

struct ReservedRouteContext { uint64_t token = 0; };
void reserve_response(int client, const HttpRequest&, bool head, void* opaque) {
  auto* context = static_cast<ReservedRouteContext*>(opaque);
  assert(ReserveHttpResponse(client, head, 1500, &context->token));
  const HttpResponse ignored = JsonResponse(200, "OK", "ignored");
  assert(!WriteHttpResponse(client, ignored, head));
}

struct DispatchArgs { HttpRouter* router; int client; };
void* dispatch_thread(void* opaque) {
  DispatchArgs* args = static_cast<DispatchArgs*>(opaque);
  args->router->Dispatch(args->client);
  close(args->client);
  return nullptr;
}

void send_request(int fd, const char* path) {
  char request[128];
  const int length = snprintf(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: test\r\n\r\n", path);
  assert(length > 0 && static_cast<size_t>(length) < sizeof(request));
  assert(send(fd, request, static_cast<size_t>(length), 0) == length);
}

std::vector<uint8_t> decode_hex(const char* hex) {
  const size_t length = strlen(hex);
  assert((length % 2) == 0);
  std::vector<uint8_t> bytes(length / 2);
  for (size_t i = 0; i < bytes.size(); ++i) {
    unsigned value = 0;
    assert(fortis::test::ParseHexByte(hex + i * 2, &value) == 1);
    bytes[i] = static_cast<uint8_t>(value);
  }
  return bytes;
}

HttpReadResult parse_request(const std::string& raw, HttpRequest* request) {
  int pair[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  assert(send(pair[1], raw.data(), raw.size(), 0) == static_cast<ssize_t>(raw.size()));
  shutdown(pair[1], SHUT_WR);
  const HttpReadResult result = ReadHttpRequest(pair[0], request);
  close(pair[0]); close(pair[1]);
  return result;
}

std::string serialized_response(const HttpResponse& response, bool head = false,
                                bool* wrote = nullptr) {
  int pair[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  const bool success = WriteHttpResponse(pair[0], response, head);
  if (wrote) *wrote = success;
  close(pair[0]);
  std::string bytes;
  char buffer[512];
  for (;;) {
    const ssize_t count = recv(pair[1], buffer, sizeof(buffer), 0);
    if (count == 0) break;
    assert(count > 0);
    bytes.append(buffer, static_cast<size_t>(count));
  }
  close(pair[1]);
  return bytes;
}

void server_error_response(int client, const HttpRequest&, bool head, void*) {
  WriteHttpResponse(client, JsonResponse(503, "Service Unavailable", "{}"), head);
}

void scrape_response(int client, const HttpRequest& request, bool head, void*) {
  static const char kText[] = "# scrape\n";
  if (!request.authorization) {
    WriteHttpResponse(client, JsonResponse(401, "Unauthorized", "{\"error\":\"UNAUTHORIZED\"}"), head);
    return;
  }
  const HttpResponse ok{200, "OK", "text/plain; version=0.0.4; charset=utf-8",
                        reinterpret_cast<const uint8_t*>(kText), sizeof(kText) - 1};
  WriteHttpResponse(client, ok, head);
}

std::string render(HttpRouter& router) {
  std::vector<char> output(HttpRouter::kMetricsRouteOutputBudget);
  const size_t length = router.RenderPrometheus(output.data(), output.size());
  assert(length && length < output.size() && output[length] == '\0');
  assert(length <= router.PrometheusWorstCaseBytes());
  return std::string(output.data(), length);
}

size_t count_of(const std::string& text, const std::string& needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
    ++count;
  return count;
}

// Every sample line is `fortis_native_http_<family>{route="<label>"} <number>`
// with <label> drawn only from `labels`; comment lines are HELP/TYPE.
void assert_well_formed(const std::string& text, const std::vector<std::string>& labels) {
  assert(!text.empty() && text.back() == '\n');
  size_t start = 0;
  while (start < text.size()) {
    const size_t end = text.find('\n', start);
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    if (line.rfind("# HELP fortis_native_http_", 0) == 0 ||
        line.rfind("# TYPE fortis_native_http_", 0) == 0) continue;
    assert(line.rfind("fortis_native_http_", 0) == 0);
    const size_t open = line.find("{route=\"");
    const size_t close = line.find("\"} ");
    assert(open != std::string::npos && close != std::string::npos && close > open);
    const std::string label = line.substr(open + 8, close - open - 8);
    bool known = false;
    for (const std::string& allowed : labels) known = known || label == allowed;
    assert(known);
    const std::string value = line.substr(close + 3);
    assert(!value.empty() && value.find_first_not_of("0123456789.") == std::string::npos);
  }
}

void test_prometheus_policy() {
  HttpRouter router;
  assert(router.Add("GET", "/healthz", health_response));
  assert(router.Add("GET", "/v1/networks/:network/token/:tokenId", health_response));
  assert(router.Add("GET", "/unavailable", server_error_response));
  assert(router.Add("GET", "/metrics", scrape_response));
  assert(router.Add("HEAD", "/metrics", scrape_response));

  // Before traffic: HELP/TYPE only, no sample for any registered route.
  const std::string empty = render(router);
  assert(count_of(empty, "# HELP ") == 4 && count_of(empty, "# TYPE ") == 4);
  assert(empty.find("route=") == std::string::npos);

  dispatch_raw(router, "GET /healthz HTTP/1.1\r\nHost: test\r\n\r\n");
  dispatch_raw(router, "GET /healthz?wallet=0xquerysecret HTTP/1.1\r\nHost: test\r\n\r\n");
  // HEAD falls back to the GET route and counts under its template.
  dispatch_raw(router, "HEAD /healthz HTTP/1.1\r\nHost: test\r\n\r\n");
  dispatch_raw(router, "GET /v1/networks/privatenet/token/0xpathsecret HTTP/1.1\r\nHost: test\r\n\r\n");
  dispatch_raw(router, "GET /unavailable HTTP/1.1\r\nHost: test\r\n\r\n");
  // Unmatched paths, unregistered methods and unparseable requests share
  // the aggregate label; an empty connection is not a request.
  dispatch_raw(router, "GET /missing/0xunmatchedsecret HTTP/1.1\r\nHost: test\r\n\r\n");
  dispatch_raw(router, "POST /metrics HTTP/1.1\r\nHost: test\r\nContent-Length: 0\r\n\r\n");
  dispatch_raw(router, "GARBAGE\r\n\r\n");
  dispatch_raw(router, "");

  const std::string before = render(router);
  assert_well_formed(before, {"GET /healthz", "GET /v1/networks/:network/token/:tokenId",
                              "GET /unavailable", "other"});
  assert(before.find("fortis_native_http_requests_total{route=\"GET /healthz\"} 3\n") !=
         std::string::npos);
  assert(before.find("fortis_native_http_errors_total{route=\"GET /healthz\"} 0\n") !=
         std::string::npos);
  assert(before.find("fortis_native_http_requests_total{route=\"GET /v1/networks/:network/token/:tokenId\"} 1\n") !=
         std::string::npos);
  // Only status >= 500 is an error.
  assert(before.find("fortis_native_http_errors_total{route=\"GET /unavailable\"} 1\n") !=
         std::string::npos);
  assert(before.find("fortis_native_http_requests_total{route=\"other\"} 3\n") !=
         std::string::npos);
  assert(before.find("fortis_native_http_errors_total{route=\"other\"} 0\n") !=
         std::string::npos);
  // Each sampled route has exactly its four series.
  assert(count_of(before, "route=\"GET /healthz\"") == 4);
  assert(count_of(before, "route=\"other\"") == 4);
  for (const char* secret : {"0xquerysecret", "privatenet", "0xpathsecret",
                             "0xunmatchedsecret", "wallet=", "?"})
    assert(before.find(secret) == std::string::npos);

  // Scrapes, authorized or not, GET or HEAD, with or without a query, never
  // move a counter: the exposition is byte-identical afterwards.
  for (const char* raw : {
           "GET /metrics HTTP/1.1\r\nHost: test\r\n\r\n",
           "HEAD /metrics HTTP/1.1\r\nHost: test\r\n\r\n",
           "GET /metrics HTTP/1.1\r\nHost: test\r\nAuthorization: Bearer x\r\n\r\n",
           "HEAD /metrics HTTP/1.1\r\nHost: test\r\nAuthorization: Bearer x\r\n\r\n",
           "GET /metrics?debug=1 HTTP/1.1\r\nHost: test\r\nAuthorization: Bearer x\r\n\r\n"}) {
    const std::string response = dispatch_raw(router, raw);
    assert(response.rfind("HTTP/1.1 401 ", 0) == 0 || response.rfind("HTTP/1.1 200 ", 0) == 0);
  }
  assert(render(router) == before);
  assert(before.find("/metrics") == std::string::npos);

  // Undersized buffers fail whole, never write past capacity, and never
  // return a truncated exposition.
  std::vector<char> exact(before.size() + 1);
  assert(router.RenderPrometheus(exact.data(), exact.size()) == before.size());
  assert(std::string(exact.data(), before.size()) == before);
  std::vector<char> short_by_one(before.size() + 8, '\x7f');
  assert(router.RenderPrometheus(short_by_one.data(), before.size()) == 0);
  for (size_t i = before.size(); i < short_by_one.size(); ++i)
    assert(short_by_one[i] == '\x7f');
  char tiny[16];
  memset(tiny, '\x7f', sizeof(tiny));
  assert(router.RenderPrometheus(tiny, 8) == 0);
  for (size_t i = 8; i < sizeof(tiny); ++i) assert(tiny[i] == '\x7f');
  assert(router.RenderPrometheus(nullptr, 64) == 0 && router.RenderPrometheus(tiny, 0) == 0);

  // Labels are emitted unescaped, so a template that would need escaping
  // (or could smuggle a line break or a second label) is never registered.
  const size_t routes = router.RouteCount();
  for (const char* bad_path : {"/quote\"", "/back\\slash", "/space here", "/line\nbreak",
                               "/tab\t", "/del\x7f", "/utf8\xc3\xa9", "relative", ""})
    assert(!router.Add("GET", bad_path, no_op));
  assert(!router.Add("GE\"T", "/ok", no_op));
  assert(!router.Add("GET", "/healthz", no_op));  // duplicate
  assert(router.RouteCount() == routes);
}

void test_prometheus_worst_case_budget() {
  // An empty table can still emit the header and a saturated "other" block.
  HttpRouter router;
  const std::string header = render(router);
  const std::string count = "18446744073709551615";
  const std::string milliseconds = "18446744073709552.000";
  const std::string other =
      "fortis_native_http_requests_total{route=\"other\"} " + count + "\n" +
      "fortis_native_http_errors_total{route=\"other\"} " + count + "\n" +
      "fortis_native_http_request_duration_ms_sum{route=\"other\"} " + milliseconds + "\n" +
      "fortis_native_http_request_duration_ms_max{route=\"other\"} " + milliseconds + "\n";
  assert(router.PrometheusWorstCaseBytes() == header.size() + other.size());

  // 127-byte templates exhaust the byte budget long before the route table.
  static char paths[HttpRouter::kMaxRoutes][128];
  size_t accepted = 0;
  for (; accepted < HttpRouter::kMaxRoutes; ++accepted) {
    char* path = paths[accepted];
    const int prefix = snprintf(path, sizeof(paths[0]), "/long/%03zu/", accepted);
    assert(prefix > 0);
    memset(path + prefix, 'a', 127 - static_cast<size_t>(prefix));
    path[127] = '\0';
    const size_t bound = router.PrometheusWorstCaseBytes();
    if (!router.Add("OPTIONS", path, health_response)) {
      assert(router.PrometheusWorstCaseBytes() == bound);  // refusal changes nothing
      break;
    }
    assert(router.PrometheusWorstCaseBytes() > bound);
  }
  assert(accepted > 40 && accepted < HttpRouter::kMaxRoutes);
  assert(router.RouteCount() == accepted);
  assert(router.PrometheusWorstCaseBytes() <= HttpRouter::kMetricsRouteOutputBudget);
  // A shorter template may still fit the remainder; the bound holds either way.
  if (router.Add("GET", "/s", health_response))
    assert(router.PrometheusWorstCaseBytes() <= HttpRouter::kMetricsRouteOutputBudget);

  // With every accepted route and "other" sampled, the scrape fits the budget.
  for (size_t i = 0; i < accepted; ++i) {
    const std::string raw = std::string("OPTIONS ") + paths[i] + " HTTP/1.1\r\nHost: test\r\n\r\n";
    assert(dispatch_raw(router, raw.c_str()).rfind("HTTP/1.1 200 ", 0) == 0);
  }
  dispatch_raw(router, "GET /not-registered HTTP/1.1\r\nHost: test\r\n\r\n");
  const std::string full = render(router);
  assert(count_of(full, "fortis_native_http_requests_total{route=") == accepted + 1);
}

void test_conditional_responses() {
  using namespace conditional_fixtures;
  const auto protobuf_bytes = decode_hex(kProtobufHex);
  const HttpResponse json{200, "OK", kJsonContentType,
      reinterpret_cast<const uint8_t*>(kJsonBody), strlen(kJsonBody)};
  const HttpResponse protobuf{200, "OK", kProtobufContentType,
      protobuf_bytes.data(), protobuf_bytes.size()};
  HttpRequest request{};
  HttpResponse response{};

  assert(ConditionalStructuredResponse(request, kNamespace, json, protobuf, &response));
  assert(response.status == 200 && strcmp(response.etag, kJsonEtag) == 0);
  assert(strcmp(response.content_type, kJsonContentType) == 0 && !response.vary_accept);

  request.accept = "application/protobuf";
  assert(ConditionalStructuredResponse(request, kNamespace, json, protobuf, &response));
  assert(response.status == 200 && strcmp(response.etag, kProtobufEtag) == 0);
  assert(strcmp(response.content_type, kProtobufContentType) == 0 && response.vary_accept);

  request.accept = nullptr;
  request.if_none_match = kJsonEtag;
  assert(ConditionalStructuredResponse(request, kNamespace, json, protobuf, &response));
  assert(response.status == 304 && response.body == nullptr && response.body_length == 0);
  assert(response.content_type == nullptr && !response.vary_accept);

  request.accept = "application/protobuf";
  request.if_none_match = kJsonEtag;
  assert(ConditionalStructuredResponse(request, kNamespace, json, protobuf, &response));
  assert(response.status == 200);  // JSON validator cannot validate protobuf bytes.
  request.if_none_match = kProtobufEtag;
  assert(ConditionalStructuredResponse(request, kNamespace, json, protobuf, &response));
  assert(response.status == 304 && response.body == nullptr && response.body_length == 0);
  assert(response.content_type == nullptr && response.vary_accept);
  const std::string not_modified = serialized_response(response, true);
  assert(not_modified.find("HTTP/1.1 304 Not Modified\r\n") == 0);
  assert(not_modified.find("ETag: ") != std::string::npos);
  assert(not_modified.find("Content-Type:") == std::string::npos);
  assert(not_modified.find("Content-Length:") == std::string::npos);
  assert(not_modified.find("Vary: Accept\r\n") != std::string::npos);
  assert(not_modified.find("\r\n\r\n") == not_modified.size() - 4);

  request.accept = nullptr;
  HttpRequest malformed{};
  assert(parse_request("GET / HTTP/1.1\r\nHost: test\r\nIf-None-Match: W/\"fixture-malformed\r\n\r\n",
                       &malformed) == HttpReadResult::Ok);
  request.if_none_match = malformed.if_none_match;
  assert(ConditionalStructuredResponse(request, kNamespace, json, protobuf, &response));
  assert(response.status == 200 && strcmp(response.etag, kJsonEtag) == 0);
  FreeHttpRequest(&malformed);
  bool head_write = false;
  const std::string head_bytes = serialized_response(response, true, &head_write);
  assert(head_write && head_bytes.find("Content-Length: ") != std::string::npos);
  assert(head_bytes.find(kJsonBody) == std::string::npos);

  request.if_none_match = nullptr;
  assert(!ConditionalStructuredResponse(request, "bad\r\nX-Evil: yes", json,
                                        protobuf, &response));

  HttpRequest parsed{};
  const std::string oversized = "GET / HTTP/1.1\r\nHost: test\r\nIf-None-Match: " +
      std::string(257, 'x') + "\r\n\r\n";
  assert(parse_request(oversized, &parsed) == HttpReadResult::BadRequest);
  FreeHttpRequest(&parsed);

  // A repeated x-intelligence-authorization is refused like the other
  // credential headers (compat-decisions.md#node-header-join).
  parsed = HttpRequest{};
  assert(parse_request("GET / HTTP/1.1\r\nHost: test\r\nX-Intelligence-Authorization: Bearer a\r\n\r\n",
                       &parsed) == HttpReadResult::Ok);
  assert(parsed.intelligence_authorization && strcmp(parsed.intelligence_authorization, "Bearer a") == 0);
  FreeHttpRequest(&parsed);
  parsed = HttpRequest{};
  assert(parse_request("GET / HTTP/1.1\r\nHost: test\r\nX-Intelligence-Authorization: Bearer a\r\n"
                       "x-intelligence-authorization: Bearer a\r\n\r\n", &parsed) == HttpReadResult::BadRequest);
  FreeHttpRequest(&parsed);

  // text/plain and form bodies are unsupported media types
  // (compat-decisions.md#form-bodies); JSON and protobuf are not.
  const struct { const char* type; bool unsupported; } media[] = {
      {"text/plain", true}, {"Text/Plain; charset=utf-8", true},
      {"application/x-www-form-urlencoded", true}, {"application/json; charset=utf-8", false},
      {"application/protobuf", false}};
  for (const auto& m : media) {
    parsed = HttpRequest{};
    const std::string raw = std::string("POST / HTTP/1.1\r\nHost: test\r\nContent-Type: ") + m.type +
                            "\r\nContent-Length: 0\r\n\r\n";
    assert(parse_request(raw, &parsed) == HttpReadResult::Ok);
    assert(parsed.has_content_type && parsed.unsupported_content_type == m.unsupported);
    FreeHttpRequest(&parsed);
  }

  HttpResponse injected = json;
  strcpy(injected.etag, "W/\"ok\"\r\nX-Evil: yes");
  bool wrote = true;
  serialized_response(injected, false, &wrote);
  assert(!wrote);
  injected = json;
  injected.content_type = "application/json\r\nX-Evil: yes";
  serialized_response(injected, false, &wrote);
  assert(!wrote);
}

std::string read_to_close(int fd) {
  std::string bytes;
  char buffer[512];
  for (;;) {
    const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
    if (count == 0) return bytes;
    // Linux reports the close of a socket that still held unread request
    // bytes (the 431 and 413 refusals) as a reset after the response.
    if (count < 0 && errno == ECONNRESET) return bytes;
    assert(count > 0);
    bytes.append(buffer, static_cast<size_t>(count));
  }
}

void test_response_handoff() {
  HttpResponseHandoff handoff;
  assert(handoff.Start());
  int pair[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  uint64_t token = 0;
  TerminalState completed_state;
  assert(handoff.Reserve(pair[0], false, 1500, &token, terminal_callback, &completed_state));
  assert(token != 0);
  uint8_t body[] = "owned-after-complete";
  HttpResponse response{502, "Bad Gateway", "text/plain", body, sizeof(body) - 1};
  assert(handoff.Complete(token, response));
  memset(body, 'x', sizeof(body) - 1);
  const std::string completed = read_to_close(pair[1]);
  close(pair[1]);
  assert(completed.find("HTTP/1.1 502 Bad Gateway\r\n") == 0);
  assert(completed.find("\r\n\r\nowned-after-complete") != std::string::npos);
  assert(!handoff.Complete(token, response));  // one completion only
  assert(completed_state.calls.load() == 1 && completed_state.cancelled.load() == 0);

  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  uint64_t expired_token = 0;
  TerminalState expired_state;
  assert(handoff.Reserve(pair[0], true, 80, &expired_token, terminal_callback, &expired_state));
  const std::string timeout = read_to_close(pair[1]);
  close(pair[1]);
  assert(timeout.find("HTTP/1.1 504 Gateway Timeout\r\n") == 0);
  assert(timeout.find("Content-Length: ") != std::string::npos);
  assert(timeout.find("\r\n\r\n") == timeout.size() - 4);  // HEAD has no body
  assert(!handoff.Complete(expired_token, response));
  assert(wait_for_terminal(expired_state));
  assert(expired_state.calls.load() == 1 && expired_state.cancelled.load() == 1);

  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  uint64_t disconnected_token = 0;
  TerminalState disconnected_state;
  assert(handoff.Reserve(pair[0], false, 1500, &disconnected_token, terminal_callback, &disconnected_state));
  close(pair[1]);
  timespec disconnect_pause{0, 300 * 1000 * 1000};
  nanosleep(&disconnect_pause, nullptr);
  assert(!handoff.Complete(disconnected_token, response));
  assert(wait_for_terminal(disconnected_state));
  assert(disconnected_state.calls.load() == 1 && disconnected_state.cancelled.load() == 1);
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  uint64_t reused_slot_token = 0;
  assert(handoff.Reserve(pair[0], false, 1500, &reused_slot_token));
  assert(reused_slot_token != disconnected_token);
  assert(!handoff.Complete(disconnected_token, response));  // stale token cannot target reuse
  uint8_t reused_body[] = "after-disconnect";
  const HttpResponse reused_response{200, "OK", "text/plain", reused_body,
                                     sizeof(reused_body) - 1};
  assert(handoff.Complete(reused_slot_token, reused_response));
  const std::string after_disconnect = read_to_close(pair[1]);
  close(pair[1]);
  assert(after_disconnect.find("after-disconnect") != std::string::npos);

  // A cold media fetch completes through this handoff and may be 1–2 MiB.
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  uint64_t media_token = 0;
  assert(handoff.Reserve(pair[0], false, 5000, &media_token));
  const std::string media_body(1536 * 1024, 'm');
  const HttpResponse media_response{200, "OK", "image/png",
      reinterpret_cast<const uint8_t*>(media_body.data()), media_body.size()};
  assert(handoff.Complete(media_token, media_response));
  const std::string media_reply = read_to_close(pair[1]);
  close(pair[1]);
  assert(media_reply.find("HTTP/1.1 200 OK\r\n") == 0);
  assert(media_reply.size() >= media_body.size());
  assert(media_reply.compare(media_reply.size() - media_body.size(), media_body.size(),
                             media_body) == 0);

  int reserved[HttpResponseHandoff::kCapacity][2];
  uint64_t tokens[HttpResponseHandoff::kCapacity]{};
  TerminalState shutdown_states[HttpResponseHandoff::kCapacity];
  for (size_t i = 0; i < HttpResponseHandoff::kCapacity; ++i) {
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, reserved[i]) == 0);
    assert(handoff.Reserve(reserved[i][0], false, 5000, &tokens[i], terminal_callback, &shutdown_states[i]));
  }
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  uint64_t rejected = 0;
  assert(!handoff.Reserve(pair[0], false, 5000, &rejected));
  close(pair[0]); close(pair[1]);
  handoff.Stop();  // shutdown closes every still-reserved socket
  for (size_t i = 0; i < HttpResponseHandoff::kCapacity; ++i) {
    char byte = 0;
    assert(recv(reserved[i][1], &byte, 1, 0) == 0);
    close(reserved[i][1]);
    assert(shutdown_states[i].calls.load() == 1 && shutdown_states[i].cancelled.load() == 1);
  }
}

// The locked control these router tests time: a test-only template that
// production never registers, so no exemption can match it. Real API reads
// (/v1/networks, /v1/prices) have been exempted one by one and must not be
// used as "still locked" examples.
constexpr char kLockedControlPath[] = "/test-only/locked-control";

// POST /v1/devices (enrollment) runs without the router's route_state_mutex
// (route_runs_without_state_lock); every other route, including the other
// /v1/devices routes and other methods on that path, keeps it.
struct GateRoute {
  std::atomic<int> inside{0};
  std::atomic<int> peak{0};
  std::atomic<int> entered{0};
  std::atomic<bool> release{false};
  // Optional work done on every wait step while the gate holds; `ticks`
  // counts the steps where it succeeded.
  bool (*tick)() = nullptr;
  std::atomic<uint64_t> ticks{0};
};

// Holds until released (at most 20 s), recording how many requests were
// inside at once.
void gated_response(int client, const HttpRequest&, bool head, void* context) {
  auto* gate = static_cast<GateRoute*>(context);
  const int now = gate->inside.fetch_add(1) + 1;
  int peak = gate->peak.load();
  while (now > peak && !gate->peak.compare_exchange_weak(peak, now)) {}
  gate->entered.fetch_add(1);
  for (int i = 0; i < 20000 && !gate->release.load(); ++i) {
    if (gate->tick && gate->tick()) gate->ticks.fetch_add(1);
    timespec pause{0, 1000 * 1000};
    nanosleep(&pause, nullptr);
  }
  gate->inside.fetch_sub(1);
  WriteHttpResponse(client, JsonResponse(200, "OK", "{}"), head);
}

struct PendingDispatch {
  DispatchArgs args{};
  int peer = -1;
  pthread_t thread{};
};

// `headers` are extra preformatted lines; `body` is sent with its length.
void start_dispatch(HttpRouter* router, const char* method, const char* path,
                    PendingDispatch* pending, const char* headers = "",
                    const std::string& body = std::string()) {
  int pair[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  char request[320];
  const int length = snprintf(request, sizeof(request),
      "%s %s HTTP/1.1\r\nHost: test\r\n%sContent-Length: %zu\r\n\r\n", method, path,
      headers, body.size());
  assert(length > 0 && static_cast<size_t>(length) < sizeof(request));
  assert(send(pair[1], request, static_cast<size_t>(length), 0) == length);
  if (!body.empty())
    assert(send(pair[1], body.data(), body.size(), 0) == static_cast<ssize_t>(body.size()));
  shutdown(pair[1], SHUT_WR);
  pending->args = {router, pair[0]};
  pending->peer = pair[1];
  assert(pthread_create(&pending->thread, nullptr, dispatch_thread, &pending->args) == 0);
}

bool readable_within(int fd, int milliseconds) {
  pollfd entry{fd, POLLIN, 0};
  return poll(&entry, 1, milliseconds) == 1;
}

// Joins the dispatch and returns its whole response.
std::string finish_dispatch(PendingDispatch* pending) {
  const std::string response = read_to_close(pending->peer);
  pthread_join(pending->thread, nullptr);
  close(pending->peer);
  return response;
}

bool wait_entered(const GateRoute& gate, int count) {
  for (int i = 0; i < 5000; ++i) {
    if (gate.entered.load() >= count) return true;
    timespec pause{0, 1000 * 1000};
    nanosleep(&pause, nullptr);
  }
  return false;
}

double seconds_since(const timespec& start) {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<double>(now.tv_sec - start.tv_sec) +
         static_cast<double>(now.tv_nsec - start.tv_nsec) / 1e9;
}

void test_enrollment_route_lock() {
  HttpRouter router;
  GateRoute enroll, rotate, get_devices;
  assert(router.Add("POST", "/v1/devices", gated_response, &enroll));
  assert(router.Add("POST", "/v1/devices/rotate", gated_response, &rotate));
  assert(router.Add("GET", "/v1/devices", gated_response, &get_devices));
  assert(router.Add("POST", "/v1/devices/challenge", health_response));
  assert(router.Add("GET", kLockedControlPath, health_response));

  // A held enrollment does not block locked routes: each completes while the
  // enrollment is still inside its handler.
  PendingDispatch first;
  start_dispatch(&router, "POST", "/v1/devices", &first);
  assert(wait_entered(enroll, 1));
  const char* const locked_routes[][2] = {{"GET", kLockedControlPath},
                                          {"POST", "/v1/devices/challenge"}};
  for (const auto& route : locked_routes) {
    timespec start{};
    clock_gettime(CLOCK_MONOTONIC, &start);
    PendingDispatch locked;
    start_dispatch(&router, route[0], route[1], &locked);
    const std::string response = finish_dispatch(&locked);
    assert(response.find("HTTP/1.1 200 OK\r\n") == 0);
    assert(seconds_since(start) < 2.0);
    assert(enroll.inside.load() == 1);
  }
  // A second enrollment enters while the first is still held: the route is
  // not serialized.
  PendingDispatch second;
  start_dispatch(&router, "POST", "/v1/devices", &second);
  assert(wait_entered(enroll, 2));
  assert(enroll.peak.load() == 2 && enroll.inside.load() == 2);
  assert(!readable_within(first.peer, 0) && !readable_within(second.peer, 0));
  enroll.release.store(true);
  assert(finish_dispatch(&first).find("HTTP/1.1 200 OK\r\n") == 0);
  assert(finish_dispatch(&second).find("HTTP/1.1 200 OK\r\n") == 0);

  // Rotation is independently synchronized, while another method on the
  // enrollment path still takes the route lock.
  {
    PendingDispatch held;
    start_dispatch(&router, "POST", "/v1/devices/rotate", &held);
    assert(wait_entered(rotate, 1));
    PendingDispatch waiting;
    start_dispatch(&router, "GET", kLockedControlPath, &waiting);
    assert(readable_within(waiting.peer, 1000));
    rotate.release.store(true);
    assert(finish_dispatch(&held).find("HTTP/1.1 200 OK\r\n") == 0);
    assert(finish_dispatch(&waiting).find("HTTP/1.1 200 OK\r\n") == 0);
  }
  {
    PendingDispatch held;
    start_dispatch(&router, "GET", "/v1/devices", &held);
    assert(wait_entered(get_devices, 1));
    PendingDispatch waiting;
    start_dispatch(&router, "GET", kLockedControlPath, &waiting);
    assert(!readable_within(waiting.peer, 300));
    get_devices.release.store(true);
    assert(finish_dispatch(&held).find("HTTP/1.1 200 OK\r\n") == 0);
    assert(finish_dispatch(&waiting).find("HTTP/1.1 200 OK\r\n") == 0);
    assert(get_devices.peak.load() == 1);
  }

  // Unlocked dispatches still record route metrics.
  char metrics[HttpRouter::kMetricsOutputCapacity];
  assert(router.RenderPrometheus(metrics, sizeof(metrics)) > 0);
  assert(strstr(metrics, "route=\"POST /v1/devices\"} 2\n"));
  puts("enrollment route runs without the route-state lock; other routes keep it");
}

// The probe handlers below mirror main.cpp's handle_probe branches over the
// same library calls, so the unlocked dispatches do real KV and nanopb
// work while another route holds the route-state lock.
void kv_health_probe(int client, const HttpRequest&, bool head, void*) {
  const bool healthy = kv_probe_healthy();
  WriteHttpResponse(client, JsonResponse(healthy ? 200 : 503,
      healthy ? "OK" : "Service Unavailable",
      healthy ? "{\"ok\":true}" : "{\"ok\":false}"), head);
}

void representation_probe(int client, const HttpRequest& request, bool head, void*) {
  HttpResponse response;
  if (AcceptsProtobuf(request)) {
    static const uint8_t encoded[] = {0x0a, 0x05, 'p', 'r', 'o', 'b', 'e'};
    response = {200, "OK", "application/protobuf", encoded, sizeof(encoded)};
  } else {
    response = JsonResponse(200, "OK", "{\"representation\":\"json\"}");
  }
  response.vary_accept = true;
  WriteHttpResponse(client, response, head);
}

void protobuf_probe(int client, const HttpRequest& request, bool head, void*) {
  uint8_t encoded[1024]{};
  size_t encoded_size = 0;
  if (!protobuf_roundtrip(reinterpret_cast<uint8_t*>(request.body), request.body_length,
                          encoded, sizeof(encoded), &encoded_size)) {
    WriteHttpResponse(client, JsonResponse(400, "Bad Request", "{\"error\":\"BAD_PROTOBUF\"}"), head);
    return;
  }
  WriteHttpResponse(client, {200, "OK", "application/protobuf", encoded, encoded_size}, head);
}

void kv_counter_probe(int client, const HttpRequest& request, bool head, void*) {
  uint64_t counter = 0;
  const bool ok = strcmp(request.method, "POST") == 0 ? kv_probe_increment(&counter)
                                                      : kv_probe_read(&counter);
  char body[64];
  snprintf(body, sizeof(body), "{\"counter\":%llu}", static_cast<unsigned long long>(counter));
  WriteHttpResponse(client, ok ? JsonResponse(200, "OK", body)
                               : JsonResponse(503, "Service Unavailable", "{}"), head);
}

bool kv_increment_tick() {
  uint64_t counter = 0;
  return kv_probe_increment(&counter);
}

// Writes expiring keys, sweeps them and compacts the process store until
// stopped, so health reads race store mutation, sweep and compaction.
struct KvRacer {
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> writes{0};
  std::atomic<uint64_t> swept{0};
  std::atomic<uint64_t> compactions{0};
};

void* kv_race_thread(void* opaque) {
  auto* racer = static_cast<KvRacer*>(opaque);
  fortis::storage::KvStore* store = fortis::storage::RuntimeKvStore();
  assert(store);
  char value[96];
  memset(value, 'r', sizeof(value));
  for (uint64_t round = 0; !racer->stop.load(); ++round) {
    char key[32];
    snprintf(key, sizeof(key), "race/%llu", static_cast<unsigned long long>(round % 64));
    const fortis::storage::KvMutation write{key, value, sizeof(value),
                                   static_cast<int64_t>(1000 + round), false};
    assert(store->Write(&write, 1));
    racer->writes.fetch_add(1);
    if (round % 8 == 7)
      racer->swept.fetch_add(store->Sweep(static_cast<int64_t>(1000 + round), 1024));
    if (round % 32 == 31) {
      assert(store->Compact());
      racer->compactions.fetch_add(1);
    }
  }
  return nullptr;
}

std::string requests_sample(const char* route, uint64_t count) {
  return std::string("fortis_native_http_requests_total{route=\"") + route + "\"} " +
         std::to_string(count) + "\n";
}

std::string errors_sample(const char* route, uint64_t count) {
  return std::string("fortis_native_http_errors_total{route=\"") + route + "\"} " +
         std::to_string(count) + "\n";
}

std::string metrics_text(HttpRouter& router) {
  std::vector<char> output(HttpRouter::kMetricsOutputCapacity);
  const size_t length = router.RenderPrometheus(output.data(), output.size());
  assert(length);
  return std::string(output.data(), length);
}

void remove_store_directory(const char* directory) {
  DIR* entries = opendir(directory);
  assert(entries);
  while (dirent* entry = readdir(entries)) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    const std::string path = std::string(directory) + "/" + entry->d_name;
    assert(unlink(path.c_str()) == 0);
  }
  closedir(entries);
  assert(rmdir(directory) == 0);
}

// GET|HEAD /healthz, /probe, /probe/representation and POST
// /probe/protobuf run without the route-state lock
// (probe_runs_without_state_lock); the KV mutation probes, their reads,
// other methods on the exempt paths and API routes keep it.
void test_probe_route_lock() {
  char directory[] = "/tmp/http_server_probe_lock.XXXXXX";
  assert(mkdtemp(directory));
  fortis::storage::RuntimeKvSetDirectory(directory);
  fortis::storage::KvStore* store = fortis::storage::RuntimeKvStore();
  assert(store);
  // Larger than kv_probe_healthy's one-byte buffer, so every health check
  // takes both of its reads (the second one reads the value from the log
  // while compaction moves it).
  const fortis::storage::KvMutation health_key{"health/check", "ok", 2, 0, false};
  assert(store->Write(&health_key, 1));

  HttpRouter router;
  GateRoute holder, locked_alternate;
  holder.tick = kv_increment_tick;
  assert(router.Add("GET", "/v1/slow", gated_response, &holder));
  const char* const read_methods[] = {"GET", "HEAD"};
  for (const char* method : read_methods) {
    assert(router.Add(method, "/healthz", kv_health_probe));
    assert(router.Add(method, "/probe", health_response));
    assert(router.Add(method, "/probe/evm", health_response));
    assert(router.Add(method, "/probe/representation", representation_probe));
    assert(router.Add(method, "/probe/kv", kv_counter_probe));
    assert(router.Add(method, "/probe/storage", health_response));
  }
  assert(router.Add("POST", "/probe/protobuf", protobuf_probe));
  assert(router.Add("POST", "/probe/kv", kv_counter_probe));
  assert(router.Add("POST", "/probe/storage", health_response));
  assert(router.Add("GET", kLockedControlPath, health_response));
  // Test-only neighbours of the exempt routes: another method on an exempt
  // path and a longer path under one. Production registers none of these;
  // they show the exemption is by registered method and exact template.
  assert(router.Add("POST", "/healthz", gated_response, &locked_alternate));
  assert(router.Add("GET", "/probe/protobuf", health_response));
  assert(router.Add("GET", "/healthz/details", health_response));
  assert(router.Add("DELETE", "/probe", health_response));

  KvRacer racer;
  pthread_t race_thread;
  assert(pthread_create(&race_thread, nullptr, kv_race_thread, &racer) == 0);

  PendingDispatch held;
  start_dispatch(&router, "GET", "/v1/slow", &held);
  assert(wait_entered(holder, 1));

  struct Control { const char* method; const char* path; PendingDispatch pending; };
  Control controls[] = {
      {"GET", kLockedControlPath, {}},   {"GET", "/probe/evm", {}},
      {"HEAD", "/probe/evm", {}},       {"GET", "/probe/kv", {}},
      {"HEAD", "/probe/kv", {}},         {"POST", "/probe/kv", {}},
      {"GET", "/probe/storage", {}},     {"POST", "/probe/storage", {}},
      {"GET", "/probe/protobuf", {}},    {"GET", "/healthz/details", {}},
      {"DELETE", "/probe", {}},
  };
  for (Control& control : controls)
    start_dispatch(&router, control.method, control.path, &control.pending);

  const std::string protobuf_body("\x0a\x03" "abc\x12\x02id", 9);
  const std::string representation_protobuf("\x0a\x05" "probe", 7);
  struct Exempt {
    const char* method; const char* path; const char* headers; std::string body;
    const char* content_type; std::string expected_body;
  };
  const Exempt exempt[] = {
      {"GET", "/healthz", "", "", "application/json", "{\"ok\":true}"},
      {"HEAD", "/healthz", "", "", "application/json", "{\"ok\":true}"},
      {"GET", "/probe", "", "", "application/json", "{}"},
      {"HEAD", "/probe", "", "", "application/json", "{}"},
      {"GET", "/probe/representation", "", "", "application/json",
       "{\"representation\":\"json\"}"},
      {"HEAD", "/probe/representation", "Accept: application/protobuf\r\n", "",
       "application/protobuf", representation_protobuf},
      {"POST", "/probe/protobuf", "Content-Type: application/protobuf\r\n", protobuf_body,
       "application/protobuf", protobuf_body},
  };
  constexpr size_t kExemptCount = sizeof(exempt) / sizeof(exempt[0]);
  uint64_t completed[kExemptCount]{};
  // Keep going until the held route has mutated KV and the racer has swept
  // and compacted the store at least twice, all while health reads ran.
  timespec loop_start{};
  clock_gettime(CLOCK_MONOTONIC, &loop_start);
  const uint64_t ticks_before = holder.ticks.load();
  size_t rounds = 0;
  while (rounds < 10 || holder.ticks.load() < ticks_before + 3 ||
         racer.compactions.load() < 2 || racer.swept.load() == 0 ||
         seconds_since(loop_start) < 0.4) {
    assert(seconds_since(loop_start) < 15.0);
    for (size_t i = 0; i < kExemptCount; ++i) {
      const Exempt& route = exempt[i];
      timespec start{};
      clock_gettime(CLOCK_MONOTONIC, &start);
      PendingDispatch pending;
      start_dispatch(&router, route.method, route.path, &pending, route.headers, route.body);
      const std::string response = finish_dispatch(&pending);
      assert(seconds_since(start) < 2.0);
      assert(response.find("HTTP/1.1 200 OK\r\n") == 0);
      assert(response.find(std::string("Content-Type: ") + route.content_type + "\r\n") !=
             std::string::npos);
      assert(response.find("Content-Length: " + std::to_string(route.expected_body.size()) +
                           "\r\n") != std::string::npos);
      const size_t header_end = response.find("\r\n\r\n");
      assert(header_end != std::string::npos);
      const std::string body = response.substr(header_end + 4);
      // HEAD keeps GET's headers and sends no body.
      assert(body == (strcmp(route.method, "HEAD") == 0 ? std::string() : route.expected_body));
      ++completed[i];
    }
    ++rounds;
  }
  const double held_seconds = seconds_since(loop_start);
  assert(holder.inside.load() == 1 && holder.peak.load() == 1);
  assert(holder.ticks.load() >= ticks_before + 3);

  // Every control still waits on the lock the held route owns, and none of
  // them has been counted yet (a route with no requests renders no sample);
  // the exempt routes already have.
  assert(!readable_within(controls[0].pending.peer, 300));
  for (const Control& control : controls) assert(!readable_within(control.pending.peer, 0));
  assert(locked_alternate.entered.load() == 0);
  std::string metrics = metrics_text(router);
  for (const Control& control : controls) {
    const std::string label = std::string("{route=\"") + control.method + " " + control.path + "\"}";
    assert(metrics.find(label) == std::string::npos);
  }
  for (size_t i = 0; i < kExemptCount; ++i) {
    const std::string route = std::string(exempt[i].method) + " " + exempt[i].path;
    assert(metrics.find(requests_sample(route.c_str(), completed[i])) != std::string::npos);
    assert(metrics.find(errors_sample(route.c_str(), 0)) != std::string::npos);
  }

  // POST on the exempt /healthz path is locked too: it waits for the held
  // route, then holds the lock itself so health still answers around it.
  PendingDispatch alternate;
  start_dispatch(&router, "POST", "/healthz", &alternate);
  assert(!readable_within(alternate.peer, 200) && locked_alternate.entered.load() == 0);

  holder.release.store(true);
  assert(finish_dispatch(&held).find("HTTP/1.1 200 OK\r\n") == 0);
  assert(wait_entered(locked_alternate, 1));
  {
    PendingDispatch health;
    start_dispatch(&router, "GET", "/healthz", &health);
    assert(finish_dispatch(&health).find("HTTP/1.1 200 OK\r\n") == 0);
  }
  locked_alternate.release.store(true);
  assert(finish_dispatch(&alternate).find("HTTP/1.1 200 OK\r\n") == 0);
  for (Control& control : controls)
    assert(finish_dispatch(&control.pending).find("HTTP/1.1 200 OK\r\n") == 0);

  racer.stop.store(true);
  pthread_join(race_thread, nullptr);
  assert(racer.writes.load() > 0);

  // Every increment the held route made while health ran is in the store,
  // plus the POST /probe/kv control's.
  PendingDispatch counter;
  start_dispatch(&router, "GET", "/probe/kv", &counter);
  const std::string counter_response = finish_dispatch(&counter);
  assert(counter_response.find("{\"counter\":" + std::to_string(holder.ticks.load() + 1) + "}") !=
         std::string::npos);

  metrics = metrics_text(router);
  assert(metrics.find(requests_sample("GET /probe/kv", 2)) != std::string::npos);
  assert(metrics.find(requests_sample("GET /healthz", completed[0] + 1)) != std::string::npos);
  assert(metrics.find(requests_sample("HEAD /healthz", completed[1])) != std::string::npos);
  assert(metrics.find(requests_sample("POST /healthz", 1)) != std::string::npos);

  fortis::storage::RuntimeKvStoreIfOpen()->Close();
  remove_store_directory(directory);
  printf("health and pure probes run without the route-state lock (%zu rounds in %.2f s, "
         "%llu held KV writes, %llu racing writes, %llu compactions); controls keep it\n",
         rounds, held_seconds, static_cast<unsigned long long>(holder.ticks.load()),
         static_cast<unsigned long long>(racer.writes.load()),
         static_cast<unsigned long long>(racer.compactions.load()));
}
}

// Server-Timing: every head the router sends carries exactly one
// "Server-Timing: app;dur=<ms>" line (http_server.cpp, kServerTimingLineBytes)
// measured from Dispatch entry until the head is formatted. These cases cover
// each emitting path: captured handlers (2xx, 401, 400), HEAD, parser errors,
// unknown routes, NO_RESPONSE, a refused duplicate, deferred and reserved
// handoff responses (completion and 504) and the handoff-overload literal.
struct TimedResponse {
  std::string head;
  std::string body;
  double duration_ms = -1;
  long content_length = -1;
};

// Splits a raw response and parses its single Server-Timing line strictly:
// "app;dur=" then digits, '.', exactly three digits, nothing else.
TimedResponse parse_timed(const std::string& raw) {
  TimedResponse parsed;
  const size_t end = raw.find("\r\n\r\n");
  assert(end != std::string::npos);
  parsed.head = raw.substr(0, end + 2);
  parsed.body = raw.substr(end + 4);
  size_t timing_lines = 0;
  for (size_t line = parsed.head.find("\r\n") + 2; line < parsed.head.size();) {
    const size_t next = parsed.head.find("\r\n", line);
    assert(next != std::string::npos);
    std::string text = parsed.head.substr(line, next - line);
    std::string lower = text;
    for (char& c : lower) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    if (lower.rfind("server-timing:", 0) == 0) {
      ++timing_lines;
      assert(text.rfind("Server-Timing: app;dur=", 0) == 0);
      const std::string value = text.substr(strlen("Server-Timing: app;dur="));
      const size_t dot = value.find('.');
      assert(dot != std::string::npos && dot > 0 && value.size() == dot + 4);
      for (size_t i = 0; i < value.size(); ++i)
        assert(i == dot || (value[i] >= '0' && value[i] <= '9'));
      parsed.duration_ms = strtod(value.c_str(), nullptr);
      assert(parsed.duration_ms >= 0 && parsed.duration_ms < 1e9);
    }
    if (lower.rfind("content-length:", 0) == 0)
      parsed.content_length = strtol(text.c_str() + strlen("content-length:"), nullptr, 10);
    line = next + 2;
  }
  assert(timing_lines == 1);
  if (parsed.content_length >= 0)
    assert(parsed.body.size() == static_cast<size_t>(parsed.content_length) ||
           parsed.body.empty());
  return parsed;
}

struct RawDispatch { HttpRouter* router; int client; bool transferred; };
void* raw_dispatch_thread(void* opaque) {
  auto* args = static_cast<RawDispatch*>(opaque);
  args->transferred = args->router->Dispatch(args->client);
  if (!args->transferred) close(args->client);
  return nullptr;
}

// Dispatches on a thread while this thread writes `raw`, so requests larger
// than the socket buffer (431, 413) cannot deadlock the test.
std::string dispatch_bytes(HttpRouter& router, const std::string& raw) {
  int pair[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
#ifdef SO_NOSIGPIPE
  const int one = 1;
  setsockopt(pair[1], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  RawDispatch args{&router, pair[0], false};
  pthread_t thread;
  assert(pthread_create(&thread, nullptr, raw_dispatch_thread, &args) == 0);
  size_t sent = 0;
  while (sent < raw.size()) {
    const ssize_t count = send(pair[1], raw.data() + sent, raw.size() - sent, 0);
    if (count <= 0) break;  // the server answered early and closed
    sent += static_cast<size_t>(count);
  }
  shutdown(pair[1], SHUT_WR);
  const std::string response = read_to_close(pair[1]);
  pthread_join(thread, nullptr);
  close(pair[1]);
  return response;
}

constexpr char kTimedBody[] = "{\"timed\":true}";
void timed_ok(int client, const HttpRequest&, bool head, void*) {
  WriteHttpResponse(client, JsonResponse(200, "OK", kTimedBody), head);
}
void timed_delayed(int client, const HttpRequest&, bool head, void* context) {
  const long milliseconds = *static_cast<long*>(context);
  timespec pause{milliseconds / 1000, (milliseconds % 1000) * 1000 * 1000};
  nanosleep(&pause, nullptr);
  WriteHttpResponse(client, JsonResponse(200, "OK", kTimedBody), head);
}
void timed_unauthorized(int client, const HttpRequest&, bool head, void*) {
  HttpResponse response = JsonResponse(401, "Unauthorized", "{\"error\":\"UNAUTHORIZED\"}");
  snprintf(response.www_authenticate, sizeof(response.www_authenticate), "Bearer");
  response.rate_limit_cost = 1;
  response.rate_limit_remaining = 59;
  WriteHttpResponse(client, response, head);
}
void timed_bad_request(int client, const HttpRequest&, bool head, void*) {
  WriteHttpResponse(client, JsonResponse(400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}"), head);
}
constexpr char kDuplicateTiming[] = "x-extra: 1\r\nserver-timing: app;dur=1.000\r\n";
void timed_duplicate(int client, const HttpRequest&, bool head, void*) {
  HttpResponse response = JsonResponse(200, "OK", kTimedBody);
  response.extra_headers = kDuplicateTiming;
  assert(!WriteHttpResponse(client, response, head));  // refused, never sent twice
}
void timed_deferred(int client, const HttpRequest&, bool head, void* context) {
  WriteHttpResponse(client, JsonResponse(200, "OK", kTimedBody), head);
  assert(DeferHttpResponse(*static_cast<uint32_t*>(context)));
}
void timed_deferred_without_handoff(int client, const HttpRequest&, bool head, void*) {
  WriteHttpResponse(client, JsonResponse(200, "OK", kTimedBody), head);
  assert(DeferHttpResponse(10));
}
void timed_reserved(int client, const HttpRequest&, bool head, void* context) {
  auto* reserved = static_cast<ReservedRouteContext*>(context);
  assert(ReserveHttpResponse(client, head, 150, &reserved->token));
}

void test_server_timing() {
  HttpResponseHandoff handoff;
  assert(handoff.Start());
  HttpRouter router;
  router.SetResponseHandoff(&handoff);
  long delay_ms = 180;
  uint32_t defer_ms = 160;
  ReservedRouteContext reserved{};
  assert(router.Add("GET", "/timed", timed_ok));
  assert(router.Add("POST", "/timed", timed_ok));
  assert(router.Add("GET", "/timed/delayed", timed_delayed, &delay_ms));
  assert(router.Add("GET", "/timed/unauthorized", timed_unauthorized));
  assert(router.Add("GET", "/timed/bad", timed_bad_request));
  assert(router.Add("GET", "/timed/none", no_op));
  assert(router.Add("GET", "/timed/duplicate", timed_duplicate));
  assert(router.Add("GET", "/timed/deferred", timed_deferred, &defer_ms));
  assert(router.Add("GET", "/timed/reserved", timed_reserved, &reserved));

  // Captured success: one parseable line, body and Content-Length unchanged.
  const TimedResponse ok = parse_timed(dispatch_bytes(router,
      "GET /timed HTTP/1.1\r\nHost: t\r\n\r\n"));
  assert(ok.head.rfind("HTTP/1.1 200 OK\r\n", 0) == 0 && ok.body == kTimedBody);
  assert(ok.content_length == static_cast<long>(strlen(kTimedBody)));
  assert(ok.duration_ms < delay_ms);
  // The existing standard headers are all still present.
  for (const char* header : {"Connection: close\r\n", "x-content-type-options: nosniff\r\n",
                             "referrer-policy: no-referrer\r\n", "cache-control: no-store\r\n",
                             "Content-Type: application/json\r\n"})
    assert(ok.head.find(header) != std::string::npos);

  // HEAD: same head (Content-Length of the GET body) and no body bytes.
  const std::string head_raw = dispatch_bytes(router, "HEAD /timed HTTP/1.1\r\nHost: t\r\n\r\n");
  const TimedResponse head = parse_timed(head_raw);
  assert(head.head.rfind("HTTP/1.1 200 OK\r\n", 0) == 0 && head.body.empty());
  assert(head.content_length == ok.content_length);
  assert(head_raw.size() == head_raw.find("\r\n\r\n") + 4);

  // A delayed handler: the value grows with the handler's elapsed time.
  const TimedResponse delayed = parse_timed(dispatch_bytes(router,
      "GET /timed/delayed HTTP/1.1\r\nHost: t\r\n\r\n"));
  assert(delayed.body == kTimedBody && delayed.duration_ms >= delay_ms);
  assert(delayed.duration_ms > ok.duration_ms);
  printf("server-timing samples: ok=%.3f ms head=%.3f ms delayed(%ld ms)=%.3f ms\n",
         ok.duration_ms, head.duration_ms, delay_ms, delayed.duration_ms);

  // Authentication and validation refusals keep their own headers.
  const TimedResponse unauthorized = parse_timed(dispatch_bytes(router,
      "GET /timed/unauthorized HTTP/1.1\r\nHost: t\r\n\r\n"));
  assert(unauthorized.head.rfind("HTTP/1.1 401 Unauthorized\r\n", 0) == 0);
  assert(unauthorized.head.find("WWW-Authenticate: Bearer\r\n") != std::string::npos);
  assert(unauthorized.head.find("x-rate-limit-remaining: 59\r\n") != std::string::npos);
  const TimedResponse bad = parse_timed(dispatch_bytes(router,
      "GET /timed/bad HTTP/1.1\r\nHost: t\r\n\r\n"));
  assert(bad.head.rfind("HTTP/1.1 400 Bad Request\r\n", 0) == 0);

  // Parser errors (malformed request line, oversized headers and body).
  const TimedResponse malformed = parse_timed(dispatch_bytes(router, "NOT-HTTP\r\n\r\n"));
  assert(malformed.head.rfind("HTTP/1.1 400 ", 0) == 0);
  const TimedResponse large_headers = parse_timed(dispatch_bytes(router,
      "GET /timed HTTP/1.1\r\nX-Fill: " + std::string(20 * 1024, 'a') + "\r\n\r\n"));
  assert(large_headers.head.rfind("HTTP/1.1 431 ", 0) == 0);
  const TimedResponse large_body = parse_timed(dispatch_bytes(router,
      "POST /timed HTTP/1.1\r\nHost: t\r\nContent-Length: 999999999\r\n\r\n"));
  assert(large_body.head.rfind("HTTP/1.1 413 ", 0) == 0);
  // HEAD is known once the request line parsed (here Host is missing); a
  // request line that fails to parse leaves the method unknown, so that 400
  // keeps its body (pre-existing behaviour, still timed).
  const TimedResponse head_parser_error = parse_timed(dispatch_bytes(router,
      "HEAD /timed HTTP/1.1\r\n\r\n"));
  assert(head_parser_error.head.rfind("HTTP/1.1 400 ", 0) == 0 && head_parser_error.body.empty());
  const TimedResponse head_bad_line = parse_timed(dispatch_bytes(router,
      "HEAD /timed/%ZZ HTTP/1.1\r\nHost: t\r\n\r\n"));
  assert(head_bad_line.head.rfind("HTTP/1.1 400 ", 0) == 0);

  // Unknown route and wrong method (404), GET and HEAD.
  const TimedResponse unknown = parse_timed(dispatch_bytes(router,
      "GET /missing HTTP/1.1\r\nHost: t\r\n\r\n"));
  assert(unknown.head.rfind("HTTP/1.1 404 ", 0) == 0 && !unknown.body.empty());
  const TimedResponse unknown_head = parse_timed(dispatch_bytes(router,
      "HEAD /missing HTTP/1.1\r\nHost: t\r\n\r\n"));
  assert(unknown_head.head.rfind("HTTP/1.1 404 ", 0) == 0 && unknown_head.body.empty());

  // A handler that wrote nothing, and one whose extra_headers tried to add a
  // second Server-Timing: both become the router's 500 with one line.
  const TimedResponse none = parse_timed(dispatch_bytes(router,
      "GET /timed/none HTTP/1.1\r\nHost: t\r\n\r\n"));
  assert(none.head.rfind("HTTP/1.1 500 ", 0) == 0);
  const TimedResponse duplicate = parse_timed(dispatch_bytes(router,
      "GET /timed/duplicate HTTP/1.1\r\nHost: t\r\n\r\n"));
  assert(duplicate.head.rfind("HTTP/1.1 500 ", 0) == 0);
  assert(duplicate.head.find("x-extra") == std::string::npos);

  // Deferred handoff: the writer's line includes the deferral delay.
  const TimedResponse deferred = parse_timed(dispatch_bytes(router,
      "GET /timed/deferred HTTP/1.1\r\nHost: t\r\n\r\n"));
  // The handoff schedules in whole milliseconds, so allow 1 ms truncation.
  assert(deferred.body == kTimedBody && deferred.duration_ms >= defer_ms - 1.0);
  const TimedResponse deferred_head = parse_timed(dispatch_bytes(router,
      "HEAD /timed/deferred HTTP/1.1\r\nHost: t\r\n\r\n"));
  assert(deferred_head.body.empty() && deferred_head.content_length == ok.content_length &&
         deferred_head.duration_ms >= defer_ms - 1.0);

  // Reserved handoff completed later: Dispatch's start travels with the slot.
  {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    send_request(pair[1], "/timed/reserved");  // no half-close: EOF reads as a disconnect
    assert(router.Dispatch(pair[0]));
    timespec pause{0, 90 * 1000 * 1000};
    nanosleep(&pause, nullptr);
    assert(handoff.Complete(reserved.token, JsonResponse(202, "Accepted", kTimedBody)));
    const TimedResponse completed = parse_timed(read_to_close(pair[1]));
    close(pair[1]);
    assert(completed.head.rfind("HTTP/1.1 202 Accepted\r\n", 0) == 0);
    assert(completed.body == kTimedBody && completed.duration_ms >= 90);
    // Unanswered: the writer's 504 at the 150 ms deadline is timed too.
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    send_request(pair[1], "/timed/reserved");  // no half-close: EOF reads as a disconnect
    assert(router.Dispatch(pair[0]));
    const TimedResponse timeout = parse_timed(read_to_close(pair[1]));
    close(pair[1]);
    assert(timeout.head.rfind("HTTP/1.1 504 ", 0) == 0 && timeout.duration_ms >= 149.0);
    printf("server-timing samples: deferred(%u ms)=%.3f ms reserved+90 ms=%.3f ms "
           "reserved 504(150 ms)=%.3f ms\n", defer_ms, deferred.duration_ms,
           completed.duration_ms, timeout.duration_ms);
  }
  handoff.Stop();

  // Deferred with no handoff: the 503 overload literal carries the line too.
  HttpRouter no_handoff;
  assert(no_handoff.Add("GET", "/timed/deferred", timed_deferred_without_handoff));
  const TimedResponse overload = parse_timed(dispatch_bytes(no_handoff,
      "GET /timed/deferred HTTP/1.1\r\nHost: t\r\n\r\n"));
  assert(overload.head.rfind("HTTP/1.1 503 ", 0) == 0);
  assert(overload.body == "{\"error\":\"OVERLOADED\"}" && overload.content_length == 22);
  const std::string overload_head_raw = dispatch_bytes(no_handoff,
      "HEAD /timed/deferred HTTP/1.1\r\nHost: t\r\n\r\n");
  assert(parse_timed(overload_head_raw).body.empty());

  // Direct writes outside a dispatch measure from the call.
  const TimedResponse direct = parse_timed(serialized_response(JsonResponse(200, "OK", "{}")));
  assert(direct.body == "{}" && direct.duration_ms < 100);
  HttpResponse refused = JsonResponse(200, "OK", "{}");
  refused.extra_headers = kDuplicateTiming;
  bool wrote = true;
  assert(serialized_response(refused, false, &wrote).empty() && !wrote);
  puts("Server-Timing present, single and parseable on every router response path");
}

static void test_expired_accept_queue() {
  HttpRouter router;
  assert(router.Add("GET", "/queued", [](int, const HttpRequest&, bool, void*) {
    assert(false && "expired work reached its handler");
  }));
  bool counted = false;
  router.SetCompletionDetails([](void* context, const HttpCompletionTag& tag, int status, double) {
    assert(status == 503 && !tag.route);
    assert(tag.admission == HttpAdmission::ConnectionQueueExpired);
    *static_cast<bool*>(context) = true;
  }, &counted);
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(listener >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  assert(listen(listener, 1) == 0);
  socklen_t size = sizeof(address);
  assert(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) == 0);
  int peer = socket(AF_INET, SOCK_STREAM, 0);
  assert(peer >= 0 && connect(peer, reinterpret_cast<sockaddr*>(&address), size) == 0);
  int client = accept(listener, nullptr, nullptr);
  assert(client >= 0);
  close(listener);
  constexpr char request[] = "GET /queued HTTP/1.1\r\nHost: localhost\r\n\r\n";
  assert(send(peer, request, sizeof(request) - 1, 0) == sizeof(request) - 1);
  assert(readable_within(client, 1000));
  timespec accepted{};
  assert(clock_gettime(CLOCK_MONOTONIC, &accepted) == 0);
  accepted.tv_sec -= 2;
  const auto before = ReadHttpQueueTimings();
  assert(!router.Dispatch(client, nullptr, &accepted));
  close(client);
  std::string reply;
  char chunk[256];
  for (;;) {
    assert(readable_within(peer, 1000));
    const ssize_t read = recv(peer, chunk, sizeof(chunk), 0);
    assert(read >= 0);
    if (!read) break;
    reply.append(chunk, static_cast<size_t>(read));
  }
  assert(reply.find("503 Service Unavailable") != std::string::npos);
  assert(reply.find("Retry-After: 1\r\n") != std::string::npos);
  assert(reply.find("Server-Timing: app;dur=") != std::string::npos);
  assert(reply.size() >= 4 && reply.compare(reply.size() - 4, 4, "\r\n\r\n") == 0);
  assert(counted);
  const auto after = ReadHttpQueueTimings();
  uint64_t count = 0;
  for (size_t i = 0; i < HttpQueueTimings::kBuckets; ++i)
    count += after.buckets[HttpQueueTimings::Dispatch][i] - before.buckets[HttpQueueTimings::Dispatch][i];
  assert(count == 1);
  char metrics[HttpRouter::kMetricsOutputCapacity];
  assert(router.RenderPrometheus(metrics, sizeof(metrics)) > 0);
  assert(strstr(metrics, "fortis_native_http_errors_total{route=\"other\"} 1\n"));
  close(peer);
}

// ---- HttpHandler / HttpReply / shared middleware ---------------------------

HttpResponse sample_response() {
  static const char kBody[] = "{\"ok\":true}";
  HttpResponse response{200, "OK", "application/json; charset=utf-8",
                        reinterpret_cast<const uint8_t*>(kBody), sizeof(kBody) - 1};
  response.vary_accept = true;
  response.rate_limit_cost = 10;
  response.rate_limit_remaining = 1190;
  return response;
}
void raw_sample(int client, const HttpRequest&, bool head, void*) {
  WriteHttpResponse(client, sample_response(), head);
}
void reply_sample(const HttpRequest&, HttpReply& reply, void*) {
  assert(reply.Send(sample_response()));
}
void raw_error(int client, const HttpRequest&, bool, void*) {
  WriteRefusal(client, 415, "Unsupported Media Type", "UNSUPPORTED_MEDIA_TYPE", false, HttpQuota{3, 9});
}
void reply_error(const HttpRequest& request, HttpReply& reply, void*) {
  HttpBodyParser parser = HttpBodyParser::kNone;
  assert(!SelectBodyParser(request, reply, HttpQuota{3, 9}, &parser));
}
void reply_reserve(const HttpRequest&, HttpReply& reply, void* context) {
  assert(reply.Reserve(1000, static_cast<uint64_t*>(context)));
  assert(reply.reserved() && !reply.Send(sample_response()));
}

// The response head and body with the Server-Timing value blanked.
std::string wire(HttpRouter& router, const char* request_text) {
  int pair[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  assert(write(pair[1], request_text, strlen(request_text)) == static_cast<ssize_t>(strlen(request_text)));
  assert(!router.Dispatch(pair[0]));
  close(pair[0]);
  std::string out;
  char chunk[4096];
  for (ssize_t n; (n = read(pair[1], chunk, sizeof(chunk))) > 0;) out.append(chunk, static_cast<size_t>(n));
  close(pair[1]);
  const size_t timing = out.find("Server-Timing: app;dur=");
  assert(timing != std::string::npos);
  const size_t end = out.find("\r\n", timing);
  out.replace(timing, end - timing, "Server-Timing: X");
  return out;
}

struct StepState { int ran = 0; bool refuse = false; };
bool count_step(const HttpRequest&, HttpReply&, StepState& state) { ++state.ran; return true; }
bool refuse_step(const HttpRequest&, HttpReply& reply, StepState& state) {
  ++state.ran;
  if (!state.refuse) return true;
  reply.Send(JsonResponse(400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}"));
  return false;
}
constexpr HttpStep<StepState> kSteps[] = {count_step, refuse_step, count_step};

void test_handler_reply_and_middleware() {
  // A migrated handler and the raw-fd adapter put the same bytes on the wire.
  HttpRouter router;
  assert(router.Add("GET", "/raw", raw_sample));
  assert(router.Add("GET", "/reply", reply_sample));
  assert(router.Add("POST", "/raw-error", raw_error));
  assert(router.Add("POST", "/reply-error", reply_error));
  assert(!router.Add("GET", "/reply", reply_sample));  // duplicates are refused across both kinds
  assert(!router.Add("GET", "/reply", raw_sample));
  const std::string get_raw = wire(router, "GET /raw HTTP/1.1\r\nHost: x\r\n\r\n");
  const std::string get_reply = wire(router, "GET /reply HTTP/1.1\r\nHost: x\r\n\r\n");
  assert(get_raw == get_reply);
  assert(get_reply.find("Content-Length: 11\r\n") != std::string::npos);
  assert(get_reply.size() > 11 && get_reply.compare(get_reply.size() - 11, 11, "{\"ok\":true}") == 0);
  // HEAD: the same head, declared length included, and no body.
  const std::string head_raw = wire(router, "HEAD /raw HTTP/1.1\r\nHost: x\r\n\r\n");
  const std::string head_reply = wire(router, "HEAD /reply HTTP/1.1\r\nHost: x\r\n\r\n");
  assert(head_raw == head_reply);
  assert(head_reply == get_reply.substr(0, get_reply.size() - 11));
  // The body parser's refusal (a body without a Content-Type → 415), with
  // the authorized request's quota headers (fastify-errors).
  const char* post_raw = "POST /raw-error HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\n{}";
  const char* post_reply = "POST /reply-error HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\n{}";
  const std::string error_raw = wire(router, post_raw);
  assert(error_raw == wire(router, post_reply));
  assert(error_raw.find("HTTP/1.1 415 Unsupported Media Type\r\n") == 0);
  const std::string refusal = "{\"error\":\"UNSUPPORTED_MEDIA_TYPE\"}";
  assert(error_raw.size() > refusal.size() &&
         error_raw.compare(error_raw.size() - refusal.size(), refusal.size(), refusal) == 0);
  assert(error_raw.find("x-rate-limit-cost: 3\r\n") != std::string::npos &&
         error_raw.find("x-rate-limit-remaining: 9\r\n") != std::string::npos);

  // A detached reply: a handler runs without a socket and its answer is read back.
  {
    HttpRequest request{};
    HttpReply reply;
    reply_sample(request, reply, nullptr);
    assert(reply.sent() && !reply.head_only() && !reply.reserved() && !reply.deferred());
    assert(reply.response().status == 200 && reply.response().body_length == 11);
    assert(memcmp(reply.response().body, "{\"ok\":true}", 11) == 0);
    assert(strcmp(reply.response().content_type, "application/json; charset=utf-8") == 0);
    assert(reply.response().vary_accept && reply.response().rate_limit_cost == 10);
    uint64_t token = 0;
    assert(!reply.Reserve(1000, &token));  // no handoff, and already answered
    assert(reply.Defer(10) && !reply.Defer(10));
  }
  {
    HttpReply reply(true);  // a HEAD request
    assert(reply.head() && !reply.sent() && !reply.Defer(0));
    uint64_t token = 0;
    assert(!reply.Reserve(1000, &token));  // detached: no handoff
    assert(reply.Send(sample_response()) && reply.head_only());
  }
  {
    // The copy outlives the handler's buffers, on both sides of the inline bound.
    for (const size_t length : std::vector<size_t>{1, 512, 513, 70000}) {
      HttpReply reply;
      {
        std::string body(length, 'b');
        const HttpResponse response{200, "OK", "text/plain", reinterpret_cast<const uint8_t*>(body.data()), length};
        assert(reply.Send(response));
        body.assign(length, 'z');
      }
      assert(reply.response().body_length == length);
      for (size_t i = 0; i < length; ++i) assert(reply.response().body[i] == 'b');
    }
    HttpReply reply;
    HttpResponse invalid = sample_response();
    invalid.extra_headers = "Server-Timing: app;dur=1\r\n";
    assert(!reply.Send(invalid) && !reply.sent());
  }

  // Reserve transfers the socket to the handoff; Complete writes the answer.
  {
    HttpResponseHandoff handoff;
    assert(handoff.Start());
    HttpRouter reserved_router;
    reserved_router.SetResponseHandoff(&handoff);
    uint64_t token = 0;
    assert(reserved_router.Add("GET", "/later", reply_reserve, &token));
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    constexpr char request[] = "HEAD /later HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(write(pair[1], request, sizeof(request) - 1) == sizeof(request) - 1);
    assert(reserved_router.Dispatch(pair[0]));  // transferred
    assert(token && handoff.Complete(token, sample_response()));
    std::string out;
    char chunk[1024];
    for (ssize_t n; (n = read(pair[1], chunk, sizeof(chunk))) > 0;) out.append(chunk, static_cast<size_t>(n));
    close(pair[1]);
    handoff.Stop();
    assert(out.find("HTTP/1.1 200 OK\r\n") == 0 && out.find("Content-Length: 11\r\n") != std::string::npos);
    assert(out.size() >= 4 && out.compare(out.size() - 4, 4, "\r\n\r\n") == 0);  // HEAD: no body
  }

  // Steps run in order and stop at the first refusal.
  {
    HttpRequest request{};
    HttpReply reply;
    StepState state;
    assert(RunHttpSteps(kSteps, request, reply, state) && state.ran == 3 && !reply.sent());
    StepState refused;
    refused.refuse = true;
    assert(!RunHttpSteps(kSteps, request, reply, refused) && refused.ran == 2);
    assert(reply.sent() && reply.response().status == 400);
  }

  // Fastify's content type parser selection.
  {
    // text/plain and form bodies are 415 like any other media type
    // (compat-decisions.md#form-bodies): the request parser marks
    // them unsupported.
    struct Case { bool has, json, protobuf; size_t length; bool ok; HttpBodyParser parser; int status; };
    const Case cases[] = {
        {false, false, false, 0, true, HttpBodyParser::kNone, 0},
        {false, false, false, 2, false, HttpBodyParser::kNone, 415},
        {true, true, false, 2, true, HttpBodyParser::kJson, 0},
        {true, true, false, 0, false, HttpBodyParser::kNone, 400},
        {true, false, false, 3, false, HttpBodyParser::kNone, 415},
        {true, false, true, 0, true, HttpBodyParser::kProtobuf, 0},
        {true, false, false, 0, false, HttpBodyParser::kNone, 415},
    };
    for (const Case& c : cases) {
      HttpRequest request{};
      request.has_content_type = c.has;
      request.json_content_type = c.json;
      request.protobuf_content_type = c.protobuf;
      request.body_length = c.length;
      HttpReply reply(false);
      HttpBodyParser parser = HttpBodyParser::kNone;
      assert(SelectBodyParser(request, reply, HttpQuota{5, 7}, &parser) == c.ok);
      assert(reply.sent() == !c.ok);
      if (c.ok) { assert(parser == c.parser); continue; }
      assert(reply.response().status == c.status);
      // fastify-errors: judged after the device policy, so the
      // refusal carries the authorized request's quota headers.
      assert(reply.response().rate_limit_cost == 5 && reply.response().rate_limit_remaining == 7);
      const std::string body(reinterpret_cast<const char*>(reply.response().body), reply.response().body_length);
      assert(body == (c.status == 415 ? "{\"error\":\"UNSUPPORTED_MEDIA_TYPE\"}" : "{\"error\":\"BAD_REQUEST\"}"));
    }
    HttpReply invalid_json;
    assert(SendInvalidJsonBody(invalid_json, HttpQuota{}));
    const std::string invalid(reinterpret_cast<const char*>(invalid_json.response().body),
                              invalid_json.response().body_length);
    assert(invalid == "{\"error\":\"BAD_REQUEST\"}" && invalid_json.response().rate_limit_cost == -1);
    HttpReply head_refusal(true);
    assert(SendRefusal(head_refusal, 414, "URI Too Long", "URI_TOO_LONG") && head_refusal.head_only());
  }

  // The device policy refusal.
  {
    DevicePolicyResult allowed;
    allowed.rate_limit_cost = 1;
    allowed.rate_limit_remaining = 7;
    HttpReply untouched;
    assert(!ReplyDevicePolicy(untouched, allowed) && !untouched.sent());
    assert(DeviceQuotaOf(allowed).cost == 1 && DeviceQuotaOf(allowed).remaining == 7);
    DevicePolicyResult limited;
    limited.status = 429;
    limited.reason = "Too Many Requests";
    limited.error_body = "{\"error\":\"RATE_LIMITED\"}";
    limited.retry_after = 60;
    limited.rate_limit_cost = 10;
    HttpReply reply(true);
    assert(ReplyDevicePolicy(reply, limited) && reply.sent() && reply.head_only());
    assert(reply.response().status == 429 && reply.response().retry_after == 60);
    assert(reply.response().rate_limit_cost == 10 && !reply.response().vary_accept);
    assert(strcmp(reply.response().content_type, "application/json; charset=utf-8") == 0);
    DevicePolicyResult bare;
    bare.status = 500;
    bare.reason = "Internal Server Error";
    HttpReply internal;
    assert(ReplyDevicePolicy(internal, bare));
    assert(internal.response().body_length == 20 &&
           memcmp(internal.response().body, "{\"error\":\"INTERNAL\"}", 20) == 0);
  }
}

int main() try {
  test_handler_reply_and_middleware();
  test_expired_accept_queue();
  check_high_socket_descriptor();
  test_server_timing();
  test_enrollment_route_lock();
  test_probe_route_lock();
  test_route_params();
  test_prometheus_policy();
  test_prometheus_worst_case_budget();
  test_conditional_responses();
  test_response_handoff();
  HttpRouter router;
  char paths[HttpRouter::kMaxRoutes][24];
  for (size_t i = 0; i < HttpRouter::kMaxRoutes; ++i) {
    const int length = snprintf(paths[i], sizeof(paths[i]), "/route/%zu", i);
    assert(length > 0 && static_cast<size_t>(length) < sizeof(paths[i]));
    assert(router.Add("GET", paths[i], no_op));
  }
  assert(router.RouteCount() == HttpRouter::kMaxRoutes);
  assert(!router.Add("GET", "/overflow", no_op));
  assert(router.RouteCount() == HttpRouter::kMaxRoutes);

  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  int send_buffer = 1024;
  assert(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer,
                    sizeof(send_buffer)) == 0);
  constexpr size_t kLargeBody = 8 * 1024 * 1024;
  auto* body = static_cast<uint8_t*>(malloc(kLargeBody));
  assert(body);
  memset(body, 'x', kLargeBody);
  const HttpResponse slow_response{200, "OK", "application/octet-stream", body, kLargeBody};
  timespec start{}, finish{};
  assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
  assert(!WriteHttpResponse(sockets[0], slow_response));
  assert(clock_gettime(CLOCK_MONOTONIC, &finish) == 0);
  const double elapsed = (finish.tv_sec - start.tv_sec) +
      (finish.tv_nsec - start.tv_nsec) / 1e9;
  assert(elapsed >= 2.0 && elapsed < 5.0);
  char partial[8192];
  const ssize_t received = recv(sockets[1], partial, sizeof(partial), MSG_DONTWAIT);
  assert(received > 0 && static_cast<size_t>(received) < kLargeBody);
  free(body);
  close(sockets[0]); close(sockets[1]);

  // The response body is copied while the handler is active, so stack-backed
  // response data remains valid after the handler returns.
  HttpRouter capture_router;
  assert(capture_router.Add("GET", "/capture", ephemeral_body_response));
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  send_request(sockets[1], "/capture");
  capture_router.Dispatch(sockets[0]);
  close(sockets[0]);
  char captured[512]{};
  size_t captured_length = 0;
  for (;;) {
    const ssize_t count = recv(sockets[1], captured + captured_length,
                               sizeof(captured) - captured_length - 1, 0);
    if (count == 0) break;
    assert(count > 0);
    captured_length += static_cast<size_t>(count);
    assert(captured_length < sizeof(captured) - 1);
  }
  close(sockets[1]);
  const char* captured_body = strstr(captured, "\r\n\r\ncaptured-after-handler-return");
  assert(captured_body);
  assert(strstr(captured, "Content-Type: text/plain\r\n"));
  assert(captured_length == static_cast<size_t>((captured_body - captured) +
      strlen("\r\n\r\ncaptured-after-handler-return")));

  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  close(sockets[1]);
  const HttpResponse small_response = JsonResponse(200, "OK", "{}");
  assert(!WriteHttpResponse(sockets[0], small_response));
  close(sockets[0]);

  HttpResponseHandoff dispatch_handoff;
  assert(dispatch_handoff.Start());
  HttpRouter reserved_router;
  ReservedRouteContext reserved_context{};
  assert(reserved_router.Add("GET", "/reserve", reserve_response, &reserved_context));
  reserved_router.SetResponseHandoff(&dispatch_handoff);
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  send_request(sockets[1], "/reserve");
  shutdown(sockets[1], SHUT_WR);
  assert(reserved_router.Dispatch(sockets[0]));
  uint8_t completion_body[] = "completed-after-dispatch";
  char completion_reason[] = "Accepted";
  char completion_type[] = "text/plain";
  const HttpResponse completion{202, completion_reason, completion_type,
                                completion_body, sizeof(completion_body) - 1};
  assert(dispatch_handoff.Complete(reserved_context.token, completion));
  memset(completion_body, 'x', sizeof(completion_body) - 1);
  memset(completion_reason, 'x', sizeof(completion_reason) - 1);
  memset(completion_type, 'x', sizeof(completion_type) - 1);
  const std::string reserved_response = read_to_close(sockets[1]);
  assert(reserved_response.find("HTTP/1.1 202 Accepted\r\n") == 0);
  assert(reserved_response.find("Content-Type: text/plain\r\n") != std::string::npos);
  assert(reserved_response.find("completed-after-dispatch") != std::string::npos);
  close(sockets[1]);
  dispatch_handoff.Stop();

  // A stalled response writer must not keep the route-state lock held. The
  // first handler captures its response under the lock, then blocks writing
  // after releasing it; the health handler must still complete promptly.
  HttpRouter concurrent_router;
  size_t concurrent_body_length = 512 * 1024;
  assert(concurrent_router.Add("GET", "/large", large_response, &concurrent_body_length));
  assert(concurrent_router.Add("GET", "/health", health_response));
  int slow_pair[2], health_pair[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, slow_pair) == 0);
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, health_pair) == 0);
  send_buffer = 1024;
  assert(setsockopt(slow_pair[0], SOL_SOCKET, SO_SNDBUF, &send_buffer,
                    sizeof(send_buffer)) == 0);
  send_request(slow_pair[1], "/large");
  DispatchArgs slow_args{&concurrent_router, slow_pair[0]};
  pthread_t slow_thread;
  assert(pthread_create(&slow_thread, nullptr, dispatch_thread, &slow_args) == 0);
  timespec pause{0, 100 * 1000 * 1000};
  nanosleep(&pause, nullptr);
  send_request(health_pair[1], "/health");
  DispatchArgs health_args{&concurrent_router, health_pair[0]};
  timespec health_start{}, health_finish{};
  assert(clock_gettime(CLOCK_MONOTONIC, &health_start) == 0);
  pthread_t health_thread;
  assert(pthread_create(&health_thread, nullptr, dispatch_thread, &health_args) == 0);
  char health_response_bytes[512];
  const ssize_t health_received = recv(health_pair[1], health_response_bytes,
                                       sizeof(health_response_bytes), 0);
  assert(health_received > 0);
  assert(clock_gettime(CLOCK_MONOTONIC, &health_finish) == 0);
  const double health_elapsed = (health_finish.tv_sec - health_start.tv_sec) +
      (health_finish.tv_nsec - health_start.tv_nsec) / 1e9;
  assert(health_elapsed < 1.0);
  pthread_join(slow_thread, nullptr);
  pthread_join(health_thread, nullptr);
  close(slow_pair[1]); close(health_pair[1]);
  puts("native HTTP router and response-lock separation tests passed");
} catch (...) {
  return fortis::test::ReportException();
}
