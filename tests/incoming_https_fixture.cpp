#include "https_listener.h"
#include "http_session.h"
#include "outbound_https.h"
#include "shutdown_signal.h"
#include <cassert>
#include <charconv>
#include <cstring>
#include <unistd.h>

namespace {
void healthy(const fortis::http::HttpRequest&, fortis::http::HttpReply& reply, void*) {
  reply.Send(fortis::http::JsonResponse(200, "OK", "{\"ok\":true}"));
}
void deferred(const fortis::http::HttpRequest&, fortis::http::HttpReply& reply, void*) {
  reply.Send(fortis::http::JsonResponse(200, "OK", "deferred"));
  reply.Defer(20);
}
void reserved(const fortis::http::HttpRequest& request, fortis::http::HttpReply& reply, void* opaque) {
  auto& handoff = *static_cast<fortis::http::ResponseHandoff*>(opaque);
  uint64_t token = 0;
  if (!reply.Reserve(100, &token)) { reply.Send(fortis::http::JsonResponse(503, "Service Unavailable", "")); return; }
  if (request.path[1] == 'r')
    handoff.Complete(token, fortis::http::JsonResponse(200, "OK", "reserved"));
}
}
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  using namespace fortis;
  app::InstallShutdownSignals();
  assert(http::OutboundHttpsInitialize());
  unsigned port = 0;
  const auto converted = std::from_chars(argv[1], argv[1] + strlen(argv[1]), port);
  if (converted.ec != std::errc{} || *converted.ptr || !port || port > 65535) return 2;
  const int server = app::ListenOnPort(port, 64);
  assert(server >= 0);
  http::HttpRouter router, health;
  http::ResponseHandoff handoff;
  assert(handoff.Start());
  router.SetResponseHandoff(&handoff);
  assert(health.Add("GET", "/healthz", healthy));
  assert(router.Add("GET", "/deferred", deferred));
  assert(router.Add("GET", "/reserved", reserved, &handoff));
  assert(router.Add("GET", "/timeout", reserved, &handoff));
  app::HttpWorkerPool workers;
  workers.router = &router;
  assert(app::StartHttpWorkers(&workers) == app::kHttpWorkerCount);
  app::HttpsListener ingress;
  assert(ingress.Start(server, workers, health));
  const bool served = ingress.Run();
  close(server);
  app::StopHttpWorkers(&workers);
  handoff.Stop();
  return served ? 0 : 1;
}
