#include "test_probe_routes.h"

#include "app_support.h"
#include "outbound_https.h"
#include "provider_http_bridge.h"
#include "test_hook_env.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace fortis::app {
namespace {

void handle_async_probe(int client, const http::HttpRequest& request, bool head, void*) {
  const unsigned delay_ms = query_unsigned(request.raw_target, "delay_ms", 250, 10000);
  const unsigned status = query_unsigned(request.raw_target, "status", 200, 599);
  if (request.body_length > kProbeBody ||
      (strcmp(request.method, "POST") != 0 && request.body_length != 0)) {
    respond(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", head);
    return;
  }
  static const uint8_t default_body[] = "async-ready";
  const uint8_t* body = request.body_length
      ? reinterpret_cast<const uint8_t*>(request.body) : default_body;
  const size_t body_length = request.body_length ? request.body_length : sizeof(default_body) - 1;
  const bool error = status >= 400;
  const http::HttpResponse response{error ? static_cast<int>(status) : 200,
      error ? "Bad Gateway" : "OK", "application/octet-stream", body, body_length};
  WriteHttpResponse(client, response, head);
  if (!http::DeferHttpResponse(delay_ms))
    respond(client, 503, "Service Unavailable", "{\"error\":\"UNAVAILABLE\"}", head);
}

void handle_async_timeout_probe(int client, const http::HttpRequest& request,
                                bool head, void*) {
  const unsigned timeout_ms = query_unsigned(request.raw_target, "timeout_ms", 100, 30000);
  uint64_t token = 0;
  if (!http::ReserveHttpResponse(client, head, timeout_ms, &token))
    respond(client, 503, "Service Unavailable", "{\"error\":\"OVERLOADED\"}", head);
  (void)token;
}

bool provider_test_mode(const char* raw_target, ProviderHttpTestMode* mode) {
  if (!raw_target || !mode) return false;
  const char* query = strchr(raw_target, '?');
  if (!query) return false;
  const char* part = query + 1;
  while (*part) {
    const char* end = strchr(part, '&');
    if (!end) end = part + strlen(part);
    if (static_cast<size_t>(end - part) > 5 && strncmp(part, "mode=", 5) == 0) {
      const size_t length = static_cast<size_t>(end - (part + 5));
      struct ModeName { const char* name; ProviderHttpTestMode mode; };
      static const ModeName names[] = {
          {"ok", ProviderHttpTestMode::kOk},
          {"slow", ProviderHttpTestMode::kSlow},
          {"retry503", ProviderHttpTestMode::kRetry503},
          {"always503", ProviderHttpTestMode::kAlways503},
          {"429", ProviderHttpTestMode::kRateLimited},
          {"tlsfail", ProviderHttpTestMode::kTlsFailure},
          {"stall", ProviderHttpTestMode::kStall},
      };
      for (const ModeName& candidate : names) {
        if (strlen(candidate.name) == length &&
            memcmp(part + 5, candidate.name, length) == 0) {
          *mode = candidate.mode;
          return true;
        }
      }
      return false;
    }
    part = *end ? end + 1 : end;
  }
  return false;
}

uint64_t monotonic_milliseconds() {
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return static_cast<uint64_t>(now.tv_sec) * 1000ULL +
         static_cast<uint64_t>(now.tv_nsec / 1000000L);
}

void handle_provider_test(int client, const http::HttpRequest& request,
                          bool head, void* opaque) {
  auto* context = static_cast<ProviderTestRouteContext*>(opaque);
  ProviderHttpTestMode mode{};
  if (!context || !context->bridge || !context->handoff ||
      (strcmp(request.method, "GET") != 0 && !head) ||
      request.body_length != 0 || !provider_test_mode(request.raw_target, &mode)) {
    respond(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", head);
    return;
  }
  const unsigned timeout_ms = query_unsigned(request.raw_target, "timeout_ms", 30000, 30000);
  if (timeout_ms < 100) {
    respond(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", head);
    return;
  }
  const uint64_t http_deadline_ms = monotonic_milliseconds() + timeout_ms;
  uint64_t token = 0;
  if (!http::ReserveHttpResponse(client, head, timeout_ms, &token)) {
    respond(client, 503, "Service Unavailable", "{\"error\":\"UNAVAILABLE\"}", head);
    return;
  }
  if (!context->bridge->Enqueue(token, mode, http_deadline_ms)) {
    http::HttpResponse busy = http::JsonResponse(503, "Service Unavailable",
                                     "{\"error\":\"UPSTREAM_BUSY\"}");
    context->handoff->Complete(token, busy);
  }
}

}  // namespace

bool LoadProviderTestRouteConfig(ProviderTestRouteConfig* config) {
  const char* provider_test_flag = TestHookEnv("FORTIS_ENABLE_PROVIDER_TEST_ROUTE");
  config->enabled = provider_test_flag && strcmp(provider_test_flag, "1") == 0;
  config->upstream_port = TestHookEnv("FORTIS_TEST_UPSTREAM_PORT");
  config->tls_failure_port = TestHookEnv("FORTIS_TEST_TLS_FAILURE_PORT");
  config->upstream_ca = TestHookEnv("FORTIS_TEST_UPSTREAM_CA");
  if (config->enabled &&
      (parse_port(config->upstream_port) < 0 || parse_port(config->tls_failure_port) < 0 ||
       !config->upstream_ca || !*config->upstream_ca ||
       !http::OutboundHttpsValidateCaBundle(config->upstream_ca))) {
    fprintf(stderr, "Invalid test provider upstream configuration\n");
    return false;
  }
  return true;
}

bool RegisterAsyncTestProbe(http::HttpRouter& router) {
  const bool async_test_probe = TestHookEnv("FORTIS_ENABLE_ASYNC_TEST_PROBE") &&
      strcmp(TestHookEnv("FORTIS_ENABLE_ASYNC_TEST_PROBE"), "1") == 0;
  return !async_test_probe ||
         (router.Add("GET", "/probe/async", handle_async_probe) &&
          router.Add("HEAD", "/probe/async", handle_async_probe) &&
          router.Add("POST", "/probe/async", handle_async_probe) &&
          router.Add("GET", "/probe/async-timeout", handle_async_timeout_probe) &&
          router.Add("HEAD", "/probe/async-timeout", handle_async_timeout_probe));
}

bool RegisterProviderTestRoute(http::HttpRouter& router, ProviderTestRouteContext* context) {
  return !context ||
         (router.Add("GET", "/probe/provider-test", handle_provider_test, context) &&
          router.Add("HEAD", "/probe/provider-test", handle_provider_test, context));
}

}  // namespace fortis::app
