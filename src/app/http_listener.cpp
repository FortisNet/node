#include "exception_boundary.h"
#include "http_listener.h"
#include "http_transport.h"
#include "log.h"

#include "api_telemetry.h"
#include "play_integrity_enrollment.h"
#include "shutdown_signal.h"
#include "threading.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef FORTIS_UNIKRAFT
#include <uk/config.h>
#endif

namespace fortis::app {

// A Play Integrity enrollment holds its worker while Google answers (up to
// 25 s). POST /v1/devices admits fewer of them than there are workers, so a
// Google stall alone can never take every worker (play_integrity_enrollment.h,
// "Route admission"). Keep this true if either number changes.
static_assert(fortis::auth::kPlayEnrollRouteMaxConcurrent < kHttpWorkerCount,
              "Play enrollments must leave at least one HTTP worker free");
#ifdef FORTIS_UNIKRAFT
static_assert(CONFIG_LWIP_NUM_TCPCON >=
                  kPendingConnectionCapacity + kHttpWorkerCount +
                      http::HttpResponseHandoff::kCapacity + 64,
              "Reserve guest sockets beyond the bounded HTTP pipeline");
static_assert(CONFIG_LIBPOSIX_FDTAB_MAXFDS > CONFIG_LWIP_NUM_TCPCON,
              "Reserve guest descriptors for non-socket resources");
#endif

namespace {

struct HttpWorkerContext {
  HttpWorkerPool* pool = nullptr;
  size_t index = 0;
};

void* http_worker(void* opaque) {
  auto* context = static_cast<HttpWorkerContext*>(opaque);
  HttpWorkerPool* pool = context->pool;
  for (;;) {
    fortis::base::UniqueLock lock(pool->mutex);
    while (pool->count == 0 && !pool->stopping)
      pool->available.Wait(lock);
    if (pool->stopping && pool->count == 0) return nullptr;
    PendingConnection connection = pool->pending[pool->head];
    pool->pending[pool->head] = PendingConnection{};
    pool->head = (pool->head + 1) % kPendingConnectionCapacity;
    --pool->count;
    pool->active_clients[context->index] = connection.client;
    lock.Unlock();

    http::HttpRouter* router = pool->router;
    const bool transferred = router->Dispatch(connection.client, connection.remote_ip, &connection.accepted);
    lock.Lock();
    pool->active_clients[context->index] = -1;
    const bool stopping = pool->stopping;
    lock.Unlock();
    if (!transferred)
      http::CloseHttpConnection(connection.client);
    if (stopping) return nullptr;
  }
  return nullptr;
}

}  // namespace

size_t StartHttpWorkers(HttpWorkerPool* pool) {
  static HttpWorkerContext contexts[kHttpWorkerCount];
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) return 0;
  if (pthread_attr_setstacksize(&attributes, 256 * 1024) != 0) {
    pthread_attr_destroy(&attributes);
    return 0;
  }
  size_t started = 0;
  for (size_t i = 0; i < kHttpWorkerCount; ++i) pool->active_clients[i] = -1;
  for (size_t i = 0; i < kHttpWorkerCount; ++i) {
    contexts[i] = {pool, i};
    const int result = (pool->workers[i].StartEntry<http_worker>(&contexts[i], &attributes) ? 0 : -1);
    if (result != 0) {
      pthread_attr_destroy(&attributes);
      fortis::base::UniqueLock lock(pool->mutex);
      pool->stopping = true;
      pool->available.Broadcast();
      lock.Unlock();
      for (size_t j = 0; j < started; ++j) pool->workers[j].Join();
      pool->started = 0;
      return 0;
    }
    ++started;
    pool->started = started;
  }
  pthread_attr_destroy(&attributes);
  return started;
}

void StopHttpWorkers(HttpWorkerPool* pool) {
  int queued_clients[kPendingConnectionCapacity];
  size_t queued_count = 0;
  fortis::base::UniqueLock lock(pool->mutex);
  pool->stopping = true;
  while (pool->count) {
    PendingConnection& connection = pool->pending[pool->head];
    queued_clients[queued_count++] = connection.client;
    connection = PendingConnection{};
    pool->head = (pool->head + 1) % kPendingConnectionCapacity;
    --pool->count;
  }
  for (size_t i = 0; i < kHttpWorkerCount; ++i)
    if (pool->active_clients[i] >= 0) shutdown(pool->active_clients[i], SHUT_RDWR);
  pool->available.Broadcast();
  const size_t started = pool->started;
  lock.Unlock();
  for (size_t i = 0; i < queued_count; ++i) http::CloseHttpConnection(queued_clients[i]);
  for (size_t i = 0; i < started; ++i) pool->workers[i].Join();
  pool->started = 0;
}

namespace {

// FORTIS_LISTEN_FDS is "PORT=FD[,PORT=FD...]". Returns the descriptor for
// `port`, -1 when the port is not listed, or -2 when the text is malformed.
int InheritedDescriptor(const char* text, unsigned port) {
  int found = -1;
  while (*text) {
    unsigned long values[2] = {0, 0};
    for (int field = 0; field < 2; ++field) {
      size_t digits = 0;
      for (; *text >= '0' && *text <= '9'; ++text, ++digits) {
        values[field] = values[field] * 10 + static_cast<unsigned long>(*text - '0');
        if (values[field] > 65535) return -2;
      }
      if (!digits || *text != (field == 0 ? '=' : (*text ? ',' : '\0'))) return -2;
      if (*text) ++text;
    }
    if (values[0] == port) found = static_cast<int>(values[1]);
  }
  return found;
}

// A descriptor handed over by the orchestrator must already be a listening
// IPv4 stream socket. Re-listening only applies this caller's backlog.
int AdoptListener(int descriptor, int backlog) {
  int type = 0, accepting = 0;
  socklen_t size = sizeof(type), accepting_size = sizeof(accepting);
  sockaddr_in address{};
  socklen_t address_size = sizeof(address);
  if (descriptor < 3 || getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &type, &size) != 0 || type != SOCK_STREAM ||
      getsockopt(descriptor, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &accepting_size) != 0 || !accepting || getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &address_size) != 0 ||
      address.sin_family != AF_INET || fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0 ||
      listen(descriptor, backlog) != 0) {
    errno = EADDRNOTAVAIL;
    return -1;
  }
  return descriptor;
}

}  // namespace

int ListenOnPort(unsigned port, int backlog) {
  // Under the orchestrator this process binds nothing: every listener is
  // inherited, so an unlisted port is refused instead of opened.
  const char* inherited = getenv("FORTIS_LISTEN_FDS");
  if (inherited && *inherited) {
    const int descriptor = InheritedDescriptor(inherited, port);
    if (descriptor >= 0) return AdoptListener(descriptor, backlog);
    errno = descriptor == -2 ? EINVAL : EADDRNOTAVAIL;
    return -1;
  }
  const int server = socket(AF_INET, SOCK_STREAM, 0);
  if (server < 0) return -1;
  const int reuse = 1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(static_cast<unsigned short>(port));
  if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
      listen(server, backlog) == 0) return server;
  const int error = errno;
  close(server);
  errno = error;
  return -1;
}

bool RunAcceptLoop(int server, HttpWorkerPool* pool, operations::ApiTelemetry* telemetry) {
  HttpWorkerPool& worker_pool = *pool;
  operations::ApiTelemetry& api_telemetry = *telemetry;
  unsigned accept_exhaustions = 0;
  while (!ShutdownRequested()) {
    sockaddr_in peer{};
    socklen_t peer_length = sizeof(peer);
    const int client = accept(server, reinterpret_cast<sockaddr*>(&peer), &peer_length);
    if (client < 0) {
      const int error = errno;
      if (error == EINTR || error == ECONNABORTED) continue;
      if (error == ENFILE || error == EMFILE || error == ENOBUFS || error == ENOMEM) {
        // Leave the listener and workers running so completed requests can
        // release sockets. Back off instead of spinning on a full table.
        api_telemetry.NoteAdmission(http::HttpAdmission::AcceptResourceExhaustion);
        if (accept_exhaustions++ % 50 == 0)
          fortis::base::LogWarn("app", "HTTP accept resource exhaustion: errno=%d consecutive=%u",
                  error, accept_exhaustions);
        const timespec pause{0, 20 * 1000 * 1000};
        nanosleep(&pause, nullptr);
        continue;
      }
      fortis::base::LogErrno(fortis::base::LogLevel::kError, "app", "accept", errno);
      return false;
    }
    accept_exhaustions = 0;
    char remote_ip[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &peer.sin_addr, remote_ip, sizeof(remote_ip)))
      remote_ip[0] = '\0';
    fortis::base::UniqueLock lock(worker_pool.mutex);
    if (worker_pool.count == kPendingConnectionCapacity) {
      lock.Unlock();
      api_telemetry.NoteAdmission(http::HttpAdmission::ConnectionQueue);
      http::WriteHttpAdmissionOverload(client);
      close(client);
      continue;
    }
    const size_t tail = (worker_pool.head + worker_pool.count) % kPendingConnectionCapacity;
    worker_pool.pending[tail].client = client;
    clock_gettime(CLOCK_MONOTONIC, &worker_pool.pending[tail].accepted);
    snprintf(worker_pool.pending[tail].remote_ip,
             sizeof(worker_pool.pending[tail].remote_ip), "%s", remote_ip);
    ++worker_pool.count;
    worker_pool.available.Signal();
  }
  return true;
}

void* HealthListener::Run(void* context) {
  auto& listener = *static_cast<HealthListener*>(context);
  while (!ShutdownRequested()) {
    pollfd socket{listener.server, POLLIN, 0};
    if (poll(&socket, 1, 100) <= 0 || !(socket.revents & POLLIN)) continue;
    const int client = accept(listener.server, nullptr, nullptr);
    if (client < 0) continue;
    listener.router.Dispatch(client);
    close(client);
  }
  return nullptr;
}

bool HealthListener::Start(unsigned port) {
  server = ListenOnPort(port, 16);
  if (server < 0) return false;
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) == 0) {
    const bool started = pthread_attr_setstacksize(&attributes, 256 * 1024) == 0 &&
        (thread.StartEntry<Run>(this, &attributes) ? 0 : -1) == 0;
    pthread_attr_destroy(&attributes);
    if (started) return true;
  }
  close(server);
  server = -1;
  return false;
}

void HealthListener::Stop() {
  if (server < 0) return;
  thread.Join();
  close(server);
  server = -1;
}

}  // namespace fortis::app
