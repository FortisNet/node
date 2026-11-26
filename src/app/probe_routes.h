#pragma once

// Liveness, the request-local self-checks, the opt-in diagnostic probes,
// GET /metrics and the device enrollment routes: the public router's
// always-present surface.

#include "http_server.h"

#include <stddef.h>

namespace fortis::ai { struct AiServiceConfig; }
namespace fortis::auth { struct DeviceEnrollmentConfig; }

namespace fortis::app {

class CompositionHooks;

// The probes' slice of the startup configuration (NativeConfig fields of the
// same names), built once by the composition root.
struct ProbeRoutesConfig {
  // FORTIS_ENABLE_DIAGNOSTIC_PROBES=1 registers the diagnostic probes.
  bool diagnostic_probes_enabled = false;
  // GET /probe/tls's target (FORTIS_TLS_PROBE_HOST, _PORT and _CA).
  bool tls_probe_configured = false;
  char tls_probe_host[254]{};
  char tls_probe_port[6]{};
  char tls_probe_ca[1024]{};
  // METRICS_TOKEN, trimmed; unset or blank leaves /metrics disabled.
  bool metrics_enabled = false;
  char metrics_token[513]{};
  // The models /healthz lists as supportedModels.
  const ai::AiServiceConfig* ai = nullptr;
};

struct MetricsRouteContext {
  const ProbeRoutesConfig* config = nullptr;
  http::HttpRouter* router = nullptr;
};

// GET|HEAD /healthz, /probe, /probe/representation, POST /probe/protobuf and
// the diagnostic probes. `context` is the ProbeRoutesConfig. The isolated
// health listener registers it for its /healthz too.
void HandleProbe(int client, const http::HttpRequest& request, bool head, void* context);

// Liveness and the request-local self-checks are always registered. The
// diagnostic probes reach outbound DNS, TCP and HTTPS, run signature
// recovery, or write and fsync the KV and storage counters for any caller,
// so they exist only when FORTIS_ENABLE_DIAGNOSTIC_PROBES=1
// (ProbeRoutesConfig::diagnostic_probes_enabled); otherwise the router
// answers its generic 404 for every method. The composition hooks' probe
// routes (none in production) register after the diagnostic probes and
// before POST /probe/protobuf, so a replay binary's route table and its
// /metrics rendering keep their order.
bool RegisterProbeRoutes(http::HttpRouter& router, ProbeRoutesConfig* config,
                         auth::DeviceEnrollmentConfig* devices,
                         MetricsRouteContext* metrics_context,
                         CompositionHooks& hooks);

}  // namespace fortis::app
