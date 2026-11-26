#pragma once

// The public HTTP listener: the bounded accept queue and fixed worker pool
// that dispatch to the router, the accept loop, and the isolated /healthz
// listener (FORTIS_HEALTH_PORT).

#include "http_server.h"
#include "threading.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stddef.h>
#include <time.h>

namespace fortis::operations { class ApiTelemetry; }

namespace fortis::app {

constexpr size_t kHttpWorkerCount = 4;
// A 100-device mixed Portfolio burst can open 200 connections. The bounded
// queue has headroom for this burst and uses an explicit overload response.
constexpr size_t kPendingConnectionCapacity = 256;

struct PendingConnection {
  int client = -1;
  char remote_ip[INET_ADDRSTRLEN]{};
  timespec accepted{};
};

struct HttpWorkerPool {
  http::HttpRouter* router = nullptr;
  PendingConnection pending[kPendingConnectionCapacity]{};
  fortis::base::ThreadHandle workers[kHttpWorkerCount];
  size_t head = 0;
  size_t count = 0;
  size_t started = 0;
  int active_clients[kHttpWorkerCount]{};
  bool stopping = false;
  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable available;
};

// Starts every worker (256 KiB stacks); returns how many started, 0 after
// joining the partial set when one fails.
size_t StartHttpWorkers(HttpWorkerPool* pool);
// Closes queued connections, shuts down active ones and joins the workers.
void StopHttpWorkers(HttpWorkerPool* pool);

// A listening IPv4 socket on INADDR_ANY:`port` (SO_REUSEADDR), or -1 with
// errno from bind/listen. When FORTIS_LISTEN_FDS ("PORT=FD,...") is set, the
// orchestrator owns every listener: the listed descriptor is adopted with
// this backlog and nothing is bound; an unlisted port fails (EADDRNOTAVAIL).
int ListenOnPort(unsigned port, int backlog);

// Accepts until a shutdown signal, queueing each connection for the workers
// (a full queue answers the admission overload). Resource exhaustion backs
// off and keeps serving. Returns false when accept failed fatally.
bool RunAcceptLoop(int server, HttpWorkerPool* pool, operations::ApiTelemetry* telemetry);

// The isolated health listener: its own router (GET /healthz only), one
// thread, one connection at a time, until shutdown.
struct HealthListener {
  http::HttpRouter router;
  int server = -1;
  fortis::base::ThreadHandle thread;
  static void* Run(void* context);
  bool Start(unsigned port);
  void Stop();
};

}  // namespace fortis::app
