#pragma once

// The test probes of every replay server binary (never the production binary
// or the guest image): GET|HEAD|POST /probe/async and GET|HEAD
// /probe/async-timeout behind FORTIS_ENABLE_ASYNC_TEST_PROBE=1, and GET|HEAD
// /probe/provider-test behind FORTIS_ENABLE_PROVIDER_TEST_ROUTE=1 with its
// loopback upstream (FORTIS_TEST_UPSTREAM_PORT, FORTIS_TEST_TLS_FAILURE_PORT
// and FORTIS_TEST_UPSTREAM_CA). Their switches come from the test-hook
// environment (test_hook_env.h).

#include "http_server.h"

class ProviderHttpBridge;

namespace fortis::app {

struct ProviderTestRouteContext {
  ProviderHttpBridge* bridge = nullptr;
  fortis::http::HttpResponseHandoff* handoff = nullptr;
};

struct ProviderTestRouteConfig {
  bool enabled = false;
  const char* upstream_port = nullptr;
  const char* tls_failure_port = nullptr;
  const char* upstream_ca = nullptr;
};

// Reads FORTIS_ENABLE_PROVIDER_TEST_ROUTE and, when it is 1, validates the
// upstream ports and CA bundle. False (after "Invalid test provider upstream
// configuration") stops startup.
bool LoadProviderTestRouteConfig(ProviderTestRouteConfig* config);

// Registers the async probes when FORTIS_ENABLE_ASYNC_TEST_PROBE=1; true
// otherwise.
bool RegisterAsyncTestProbe(fortis::http::HttpRouter& router);
// Registers /probe/provider-test when `context` is set; true otherwise.
bool RegisterProviderTestRoute(fortis::http::HttpRouter& router, ProviderTestRouteContext* context);

}  // namespace fortis::app
