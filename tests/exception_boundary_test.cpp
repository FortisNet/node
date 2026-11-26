#include "exception_boundary.h"
#include "composition_hooks.h"
#include "http_server.h"
#include "http_session.h"
#include "kv_store.h"
#include <fcntl.h>
#include "threading.h"
#include "unit_test_support.h"

#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <atomic>
#include <string>
#include <vector>

extern char** environ;

namespace {
namespace base = fortis::base;
using namespace fortis::http;
using fortis::testing::Require;
constexpr const char* kPrivateDetail = "Bearer fintel_dev_private wallet snapshot upstream body";

struct Pair {
  int server = -1;
  int client = -1;
  Pair() {
    int sockets[2];
    Require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "create socket pair");
    server = sockets[0]; client = sockets[1];
    timeval timeout{2, 0};
    if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
      close(server); close(client);
      Require(false, "bound response wait");
    }
  }
  ~Pair() { if (server >= 0) close(server); if (client >= 0) close(client); }
  Pair(const Pair&) = delete;
  Pair& operator=(const Pair&) = delete;
  void Request(const char* method = "GET", const char* path = "/throw") const {
    const std::string request = std::string(method) + " " + path + " HTTP/1.1\r\nHost: unit\r\nContent-Length: 2\r\n\r\n{}";
    Require(write(client, request.data(), request.size()) == static_cast<ssize_t>(request.size()), "send request");
  }
  void CloseServer() { close(server); server = -1; }
  std::string Read() const {
    std::string response;
    char bytes[1024];
    for (;;) {
      const ssize_t count = read(client, bytes, sizeof(bytes));
      if (count < 0 && errno == EINTR) continue;
      Require(count >= 0, "response finishes before timeout");
      if (!count) return response;
      response.append(bytes, static_cast<size_t>(count));
    }
  }
};

class CapturingSink final : public base::LogSink {
 public:
  bool Write(const base::LogRecord& record) override {
    Require(record.level == base::LogLevel::kError && !strcmp(record.component, "exception"), "exception log classification");
    messages.emplace_back(record.text, record.length);
    return true;
  }
  std::vector<std::string> messages;
};
struct SinkScope {
  base::LogSink* previous;
  explicit SinkScope(base::LogSink* sink) : previous(base::SetLogSink(sink)) {}
  ~SinkScope() { base::SetLogSink(previous); }
};

template <typename Action>
void Dies(Action action, const char* context, const char* type, const char* marker = nullptr) {
  int descriptors[2];
  Require(pipe(descriptors) == 0, "create death-test log pipe");
  const pid_t child = fork();
  if (child < 0) { close(descriptors[0]); close(descriptors[1]); Require(false, "fork death test"); }
  if (!child) {
    close(descriptors[0]);
    if (dup2(descriptors[1], STDERR_FILENO) < 0) _Exit(90);
    close(descriptors[1]);
    alarm(3);
    base::SetLogSink(nullptr);
    std::set_terminate([] { _Exit(86); });
    try { action(); } catch (...) { _Exit(87); }
    _Exit(88);
  }
  close(descriptors[1]);
  std::string log;
  char bytes[1024];
  for (;;) {
    const ssize_t count = read(descriptors[0], bytes, sizeof(bytes));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    log.append(bytes, static_cast<size_t>(count));
  }
  close(descriptors[0]);
  int status = 0;
  pid_t waited;
  do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
  Require(waited == child, "reap death-test child");
  Require(WIFEXITED(status) && WEXITSTATUS(status) == 86, "boundary calls terminate without propagating or returning");
  Require(log.find(context) != std::string::npos && log.find(type) != std::string::npos,
          "termination logs trusted context and catch classification");
  Require(log.find(kPrivateDetail) == std::string::npos, "fatal logs omit private exception text");
  Require(!marker || log.find(marker) != std::string::npos, "fallback diagnostic present");
}

void* throwing_entry(void*) { throw std::runtime_error(kPrivateDetail); }
void throwing_thread(int*) { throw std::bad_alloc(); }
int throwing_compare(const void*, const void*) { throw std::runtime_error(kPrivateDetail); }
void throwing_once() { throw 7; }
int add(int first, int second) { return first + second; }

void fatal_callbacks() {
  static_assert(noexcept(base::FatalCallback<add>(1, 2)), "C callback must be noexcept");
  Require(base::FatalCallback<add>(3, 4) == 7, "normal callback result preserved");
  Dies([] {
    pthread_t thread;
    if (pthread_create(&thread, nullptr, base::FatalCallback<throwing_entry>, nullptr)) _Exit(89);
    pthread_join(thread, nullptr);
  }, "throwing_entry", "std::exception");
  Dies([] {
    base::Thread thread;
    int context = 0;
    if (!thread.Start(throwing_thread, &context)) _Exit(89);
    thread.Join();
  }, "Thread::Trampoline", "std::bad_alloc");
  Dies([] {
    base::Thread thread;
    if (!thread.Start([] { throw std::runtime_error(kPrivateDetail); })) _Exit(89);
    thread.Join();
  }, "Thread::Trampoline", "std::exception");
  Dies([] {
    base::Thread thread;
    if (!thread.StartEntry<throwing_entry>(nullptr)) _Exit(89);
    thread.Join();
  }, "Thread::Trampoline", "std::exception");
  Dies([] {
    if (!base::StartDetached<throwing_entry>(static_cast<void*>(nullptr))) _Exit(89);
    for (;;) pause();
  }, "throwing_entry", "std::exception");
  Dies([] { int values[2]{2, 1}; qsort(values, 2, sizeof(int), base::FatalCallback<throwing_compare>); },
       "throwing_compare", "std::exception");
  Dies([] { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, base::FatalCallback<throwing_once>); },
       "throwing_once", "unknown exception");
}

struct ThrowingSink final : base::LogSink {
  bool Write(const base::LogRecord&) override { throw std::runtime_error(kPrivateDetail); }
};
struct ThrowingHooks final : fortis::app::CompositionHooks {
  bool LoadConfig() override { throw std::bad_alloc(); }
};
void server_main_boundary() {
  Dies([] {
    // Configuration is isolated in this fork, before any listener/plane starts.
    std::vector<std::string> names;
    for (char** entry = environ; *entry; ++entry) {
      if (!strncmp(*entry, "FORTIS_", 7)) {
        const char* equal = strchr(*entry, '=');
        if (equal) names.emplace_back(*entry, static_cast<size_t>(equal - *entry));
      }
    }
    for (const auto& name : names) unsetenv(name.c_str());
    ThrowingHooks hooks;
    char name[] = "boundary-test";
    char* argv[]{name, nullptr};
    fortis::app::ServerMain(1, argv, hooks);
  }, "ServerMain", "std::bad_alloc");
}

void logging_failure_is_contained() {
  Dies([] {
    ThrowingSink sink;
    SinkScope guard(&sink);
    base::FatalBoundary("throwing log sink", [] { throw std::bad_alloc(); });
  }, "throwing log sink", "std::bad_alloc", "logging failed");
}

struct HandlerState {
  bool oom = false;
  bool send_first = false;
  bool reserve = false;
  bool complete_first = false;
  bool unknown = false;
  uint64_t token = 0;
  HttpResponseHandoff* handoff = nullptr;
  std::atomic<int> terminals{0};
  std::atomic<int> cancelled{0};
};
void terminal(void* raw, bool cancelled) {
  auto& state = *static_cast<HandlerState*>(raw);
  if (cancelled) ++state.cancelled;
  ++state.terminals;
}
void throwing_handler(const HttpRequest&, HttpReply& reply, void* raw) {
  auto& state = *static_cast<HandlerState*>(raw);
  if (state.send_first) {
    const std::string body(1024, 'x');
    Require(reply.Send(JsonResponse(200, "OK", body.c_str())), "capture heap body before throw");
    Require(reply.Defer(10000), "defer before throw");
  }
  if (state.reserve) {
    Require(reply.Reserve(1000, &state.token, terminal, &state), "reserve before throw");
    if (state.complete_first) Require(state.handoff->Complete(state.token, JsonResponse(202, "Accepted", "{}")), "complete before throw");
  }
  if (state.unknown) throw 1;
  if (state.oom) throw std::bad_alloc();
  throw std::runtime_error(kPrivateDetail);
}
void healthy_handler(const HttpRequest&, HttpReply& reply, void*) { reply.Send(JsonResponse(200, "OK", "{}")); }
void raw_handler(int client, const HttpRequest&, bool, void*) {
  constexpr char partial[] = "HTTP/1.1 200 OK\r\nContent-Length: 99\r\n\r\npartial";
  Require(write(client, partial, sizeof(partial) - 1) == sizeof(partial) - 1, "write raw partial response");
  throw std::runtime_error(kPrivateDetail);
}

void request_recovery_and_capture_reset() {
  CapturingSink sink;
  SinkScope sink_scope(&sink);
  for (bool unlocked : {false, true}) {
    for (bool oom : {false, true}) {
      HttpRouter router;
      HandlerState state;
      state.oom = oom;
      state.send_first = true;
      Require(router.Add("GET", "/throw", throwing_handler, &state), "register throwing route");
      Require(router.Add("GET", "/healthy", healthy_handler), "register healthy route");
      if (unlocked) router.RunWithoutStateLock("/throw");
      for (const char* method : {"GET", "HEAD"}) {
        Pair pair;
        pair.Request(method);
        Require(!router.Dispatch(pair.server), "failure retains caller socket ownership");
        pair.CloseServer();
        const auto response = pair.Read();
        Require(response.find(oom ? "HTTP/1.1 503 " : "HTTP/1.1 500 ") == 0, "standard exception maps to HTTP error");
        const size_t split = response.find("\r\n\r\n");
        Require(split != std::string::npos && (strcmp(method, "HEAD") ? response.size() > split + 4 : response.size() == split + 4),
                "HEAD suppresses only the failure body");
        Require(response.find("HTTP/1.1", 1) == std::string::npos && response.find("xxx") == std::string::npos,
                "discard captured/deferred success before error response");
        Require(sink.messages.back().find("/throw") != std::string::npos &&
                    sink.messages.back().find(oom ? "std::bad_alloc" : "std::exception") != std::string::npos,
                "request error logged with route and catch classification");
        Require(sink.messages.back().find(kPrivateDetail) == std::string::npos &&
                    response.find(kPrivateDetail) == std::string::npos,
                "exception payload never enters logs or the HTTP error");
        // If thread-local capture dangles, this write is captured into freed
        // stack storage instead of reaching the unrelated socket.
        Pair direct;
        Require(WriteHttpResponse(direct.server, JsonResponse(201, "Created", "{}")), "write outside dispatch after exception");
        direct.CloseServer();
        Require(direct.Read().find("HTTP/1.1 201 ") == 0, "response capture reset after exception");
        Pair next;
        next.Request("GET", "/healthy");
        Require(!router.Dispatch(next.server), "subsequent request retains ownership");
        next.CloseServer();
        Require(next.Read().find("HTTP/1.1 200 ") == 0, "route lock released and worker remains usable");
      }
    }
  }
}

void raw_partial_response_is_closed() {
  CapturingSink sink;
  SinkScope guard(&sink);
  HttpRouter router;
  Require(router.Add("GET", "/throw", raw_handler), "register raw route");
  Pair pair;
  pair.Request();
  Require(!router.Dispatch(pair.server), "raw failure retains caller close ownership");
  const auto response = pair.Read();  // shutdown must end I/O even before close
  Require(response.find("partial") != std::string::npos && response.find("HTTP/1.1 500") == std::string::npos,
          "never append error HTTP bytes after a partial raw response");
}

void raw_reserving_handler(int client, const HttpRequest&, bool head, void* raw) {
  auto& state = *static_cast<HandlerState*>(raw);
  Require(ReserveHttpResponse(client, head, 1000, &state.token, terminal, &state), "raw handler reserves socket");
  throw std::runtime_error(kPrivateDetail);
}
void raw_reserved_failure() {
  CapturingSink sink;
  SinkScope guard(&sink);
  HttpResponseHandoff handoff;
  Require(handoff.Start(), "start raw reservation writer");
  HandlerState state;
  HttpRouter router;
  router.SetResponseHandoff(&handoff);
  Require(router.Add("GET", "/throw", raw_reserving_handler, &state), "register raw reserving handler");
  Pair pair;
  pair.Request();
  Require(router.Dispatch(pair.server), "failed raw reservation keeps transferred close ownership");
  pair.server = -1;
  Require(pair.Read().empty(), "failed raw reservation closes without appending HTTP bytes");
  Require(!handoff.Complete(state.token, JsonResponse(200, "OK", "late")), "raw reservation cannot be completed after failure");
  handoff.Stop();
  Require(state.terminals == 1 && state.cancelled == 1, "raw reservation cancelled once");
}

void reserved_response_failure() {
  CapturingSink sink;
  SinkScope guard(&sink);
  for (bool complete_first : {false, true}) {
    HttpResponseHandoff handoff;
    Require(handoff.Start(), "start reservation writers");
    HandlerState state;
    state.reserve = true;
    state.oom = true;
    state.complete_first = complete_first;
    state.handoff = &handoff;
    HttpRouter router;
    router.SetResponseHandoff(&handoff);
    Require(router.Add("GET", "/throw", throwing_handler, &state), "register reserving handler");
    Pair pair;
    pair.Request();
    Require(router.Dispatch(pair.server), "exception preserves transferred socket ownership");
    pair.server = -1;
    const auto response = pair.Read();
    Require(response.find(complete_first ? "HTTP/1.1 202 " : "HTTP/1.1 503 ") == 0,
            "pending reservation fails; already completed answer wins");
    Require(!handoff.Complete(state.token, JsonResponse(200, "OK", "late")), "late completion cannot replace terminal response");
    handoff.Stop();
    Require(state.terminals == 1 && state.cancelled == (complete_first ? 0 : 1), "reservation terminal fires once with cancellation disposition");
  }
}

void unknown_request_exception_terminates() {
  Dies([] {
    HttpRouter router;
    HandlerState state;
    state.unknown = true;
    Require(router.Add("GET", "/throw", throwing_handler, &state), "register unknown throwing route");
    Pair pair;
    pair.Request();
    router.Dispatch(pair.server);
  }, "/throw", "unknown exception");
}

void throw_failure(int kind) {
  if (kind == 0) throw std::bad_alloc();
  if (kind == 1) throw std::runtime_error(kPrivateDetail);
  throw 7;
}

void parser_failures_are_request_local() {
  CapturingSink sink;
  SinkScope guard(&sink);
  for (int kind = 0; kind < 3; ++kind) {
    HttpRouter router;
    Require(router.Add("GET", "/healthy", healthy_handler), "register next request");
    for (const char* method : {"GET", "HEAD"}) {
      Pair failed;
      failed.Request(method);
      auto reader = [&](int fd, HttpRequest* request) -> HttpReadResult {
        Require(RequestParser::Read(fd, request) == HttpReadResult::Ok, "own parsed storage before injected failure");
        // Exercise independently owned partial scratch storage as well as the
        // target/header owner. No handler or shared application state is touched.
        request->body_allocation = base::AllocateStorage<char[]>(32768);
        Require(request->body_allocation != nullptr, "partial parser allocation");
        throw_failure(kind);
        return HttpReadResult::Ok;
      };
      Require(!ConnectionSession(router).Dispatch(failed.server, nullptr, nullptr, reader), "parser failure retains caller close ownership");
      Require(fcntl(failed.server, F_GETFD) >= 0, "parser boundary never closes caller socket");
      failed.CloseServer();
      const auto response = failed.Read();
      Require(response.find(kind == 0 ? "HTTP/1.1 503 " : "HTTP/1.1 500 ") == 0, "parser failure classification");
      Require(response.find(kPrivateDetail) == std::string::npos && sink.messages.back().find(kPrivateDetail) == std::string::npos,
              "parser errors and logs omit exception payload");
      if (!strcmp(method, "HEAD")) Require(response.ends_with("\r\n\r\n"), "HEAD parser failure has no body");
      Pair next;
      next.Request("GET", "/healthy");
      Require(!router.Dispatch(next.server), "next request retains ownership");
      next.CloseServer();
      Require(next.Read().find("HTTP/1.1 200 ") == 0, "parser failure leaves server usable");
    }
    char metrics[8192];
    const size_t size = router.RenderPrometheus(metrics, sizeof(metrics));
    const std::string rendered(metrics, size);
    Require(rendered.find("fortis_native_http_errors_total{route=\"other\"} 2") != std::string::npos,
            "parser failures counted once without a matched route");
  }
}

void throwing_completion(const void* raw) { throw_failure(*static_cast<const int*>(raw)); }
void throwing_hook(void* raw, const char*, int, const char*, double) { throwing_completion(raw); }
void throwing_details(void* raw, const HttpCompletionTag&, int, double) { throwing_completion(raw); }
void throwing_terminal(void* raw, bool) { throwing_completion(raw); }

void completion_failures_remain_fatal() {
  for (int kind = 0; kind < 3; ++kind) {
    const char* type = kind == 0 ? "std::bad_alloc" : kind == 1 ? "std::exception" : "unknown exception";
    for (bool details : {false, true}) {
      Dies([&] {
        HttpRouter router;
        Require(router.Add("GET", "/healthy", healthy_handler), "completion route");
        if (details) router.SetCompletionDetails(throwing_details, &kind);
        else router.SetCompletionHook(throwing_hook, &kind);
        Pair pair;
        pair.Request("GET", "/healthy");
        router.Dispatch(pair.server);
      }, details ? "HTTP completion details" : "HTTP completion hook", type);
    }
    Dies([&] {
      HttpResponseHandoff handoff;
      Require(handoff.Start(), "terminal failure writer");
      Pair pair;
      uint64_t token = 0;
      Require(handoff.Reserve(pair.server, false, 1000, &token, throwing_terminal, &kind), "terminal failure reservation");
      pair.server = -1;
      handoff.Stop();
    }, "HTTP stop terminal callback", type);
  }
}

struct MutationState {
  fortis::storage::KvStore* kv;
  bool commit = false;
};
void mutation_then_throw(const HttpRequest&, HttpReply& reply, void* raw) {
  auto& state = *static_cast<MutationState*>(raw);
  if (state.commit) {
    const fortis::storage::KvMutation writes[] = {
        {"ack", "durable", 7, 0, false}, {"index", "ack", 3, 0, false}};
    Require(state.kv->Write(writes, 2), "atomic durable mutation");
  }
  Require(reply.Send(JsonResponse(201, "Created", "{}")), "capture unacknowledged response");
  throw std::runtime_error(kPrivateDetail);
}
void durable_writes_are_not_rolled_back_by_catch() {
  CapturingSink sink;
  SinkScope guard(&sink);
  fortis::testing::TemporaryKv fixture;
  MutationState state{&fixture.kv};
  HttpRouter router;
  Require(router.Add("POST", "/throw", mutation_then_throw, &state), "mutation failure route");
  Require(router.Add("GET", "/healthy", healthy_handler), "post-mutation healthy route");
  for (bool committed : {false, true}) {
    state.commit = committed;
    Pair failed;
    failed.Request("POST");
    Require(!router.Dispatch(failed.server), "failed mutation retains descriptor");
    failed.CloseServer();
    const auto response = failed.Read();
    Require(response.find("HTTP/1.1 500 ") == 0 && response.find("201 Created") == std::string::npos,
            "an unsent success is never acknowledged after a throw");
    fixture.kv.Close();
    Require(fixture.kv.Open(fixture.directory.path()), "reopen after boundary failure");
    for (const char* key : {"ack", "index"}) {
      char value[32];
      size_t size = 0;
      const auto found = fixture.kv.GetStatus(key, 0, value, sizeof(value), &size);
      Require(found == (committed ? fortis::storage::KvReadStatus::Found : fortis::storage::KvReadStatus::Missing),
              "failure before a frame leaves neither key; failure after commit preserves both");
    }
    Pair next;
    next.Request("GET", "/healthy");
    Require(!router.Dispatch(next.server), "post-mutation ownership");
    next.CloseServer();
    Require(next.Read().find("HTTP/1.1 200 ") == 0, "mutation exception releases route lock");
  }
}
}  // namespace

int main() {
  fortis::testing::UnitSuite suite;
  suite.Run("fatal C callbacks/thread entries", fatal_callbacks);
  suite.Run("throwing log sink", logging_failure_is_contained);
  suite.Run("ServerMain throwing composition hook", server_main_boundary);
  suite.Run("unknown request exception", unknown_request_exception_terminates);
  suite.Run("parser failure ownership and recovery", parser_failures_are_request_local);
  suite.Run("completion and terminal failure policy", completion_failures_remain_fatal);
  suite.Run("durable request writes across exceptions", durable_writes_are_not_rolled_back_by_catch);
  suite.Run("request errors/HEAD/capture reset/lock release", request_recovery_and_capture_reset);
  suite.Run("raw partial response", raw_partial_response_is_closed);
  suite.Run("reservation exception/completion race", reserved_response_failure);
  suite.Run("raw reserved socket exception", raw_reserved_failure);
  return suite.Finish("exception_boundary_test");
}
