#include "http_transport.h"
#include "http_session.h"
#include "unit_test_support.h"
#include <atomic>
#include <algorithm>
#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <vector>

namespace {
using namespace fortis::http;
using fortis::testing::Require;
struct Transport final : HttpTransport {
  std::string input, output;
  size_t cursor = 0, fragment = 3;
  std::atomic<unsigned> finishes{0};
  unsigned writes = 0;
  bool reusable = true, success = false, fail_write = false;
  ssize_t Read(char* bytes, size_t length) override {
    const size_t n = std::min({length, fragment, input.size() - cursor});
    memcpy(bytes, input.data() + cursor, n); cursor += n; return static_cast<ssize_t>(n);
  }
  bool Write(const char* bytes, size_t length, const timespec&) override {
    ++writes; if (fail_write) return false; output.append(bytes, length); return true;
  }
  bool KeepAlive() const override { return reusable; }
  void Written(bool result) override { success = result; }
  void DisallowReuse() override { reusable = false; }
  void Finish() override { finishes.fetch_add(1, std::memory_order_release); }
};
struct Registered {
  int fd;
  Transport transport;
  int peer;
  Registered() {
    int sockets[2]; Require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "transport socketpair");
    fd = sockets[0]; peer = sockets[1]; Require(RegisterHttpTransport(fd, &transport), "register transport");
  }
  ~Registered() { RegisterHttpTransport(fd, nullptr); close(fd); close(peer); }
  Registered(const Registered&) = delete;
  Registered& operator=(const Registered&) = delete;
};
void registry_capacity_collisions_and_reuse() {
  Transport one, two;
  Require(!RegisterHttpTransport(-1, &one) && !RegisterHttpTransport(-1, nullptr), "negative fd never registered");
  struct Cleanup { ~Cleanup() { for (int i = 0; i < 257; ++i) RegisterHttpTransport(100000 + i, nullptr); } } cleanup;
  for (int i = 0; i < 256; ++i) Require(RegisterHttpTransport(100000 + i, &one), "all bounded registry slots usable");
  Require(!RegisterHttpTransport(100256, &one), "registry overflow fails without eviction");
  Require(RegisterHttpTransport(100100, &one), "same owner registration idempotent at capacity");
  Require(!RegisterHttpTransport(100100, &two) && FindHttpTransport(100100) == &one, "collision cannot steal existing owner");
  Require(RegisterHttpTransport(100100, nullptr) && !FindHttpTransport(100100), "removal clears owner");
  Require(!RegisterHttpTransport(100100, nullptr), "missing removal reports absent");
  Require(RegisterHttpTransport(100256, &two) && FindHttpTransport(100256) == &two, "free slot reused after overflow");
  for (int i = 0; i < 256; ++i) if (i != 100) Require(FindHttpTransport(100000 + i) == &one, "unrelated registry entries preserved");
}
void close_transfers_borrowed_ownership() {
  Registered registered;
  CloseHttpConnection(registered.fd);
  Require(registered.transport.finishes == 1 && fcntl(registered.fd, F_GETFD) >= 0, "borrower finishes without closing ingress socket");
  Require(FindHttpTransport(registered.fd) == &registered.transport, "ingress retains registration until observing Finish");
  int sockets[2]; Require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "raw socketpair");
  CloseHttpConnection(sockets[0]);
  Require(fcntl(sockets[0], F_GETFD) == -1 && errno == EBADF, "ordinary unregistered socket really closed"); close(sockets[1]);
}
void registry_thread_safety() {
  Transport owner;
  std::atomic<bool> correct{true}, done{false};
  std::thread reader([&] {
    while (!done.load(std::memory_order_acquire)) {
      auto* value = FindHttpTransport(90000);
      if (value && value != &owner) correct = false;
    }
  });
  for (unsigned i = 0; i < 10000; ++i) {
    if (!RegisterHttpTransport(90000, &owner) || FindHttpTransport(90000) != &owner || !RegisterHttpTransport(90000, nullptr)) correct = false;
  }
  done.store(true, std::memory_order_release); reader.join();
  Require(correct && !FindHttpTransport(90000), "registry publishes coherent owner under concurrent lookup");
}
void borrowed_request_parser() {
  for (size_t fragment : {size_t{1}, size_t{3}, size_t{65536}}) {
    Registered r; r.transport.fragment = fragment;
    r.transport.input = "POST /v1/devices?signature=a%2Bb HTTP/1.1\r\nHost: localhost\r\nContent-Length: 3\r\n\r\n";
    r.transport.input.append("a\0b", 3);
    ParsedRequest parsed;
    Require(RequestParser::Read(r.fd, &parsed.get()) == HttpReadResult::Ok, "borrowed transport handles fragmented request");
    Require(parsed.get().body_length == 3 && !memcmp(parsed.get().body, "a\0b", 3) && !strcmp(parsed.get().raw_target, "/v1/devices?signature=a%2Bb"), "opaque body and raw signed target preserved");
    Require(r.transport.cursor == r.transport.input.size(), "exact framed bytes read");
  }
  Registered r; r.transport.input = "GET / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n\r\na";
  ParsedRequest parsed; Require(RequestParser::Read(r.fd, &parsed.get()) == HttpReadResult::BadRequest, "truncated borrowed frame rejected");
}
struct Reservation {
  ResponseHandoff* handoff;
  unsigned mode;
  unsigned terminals = 0;
  bool abandoned = false;
  uint64_t token = 0;
};
void terminal(void* context, bool abandoned) {
  auto& state = *static_cast<Reservation*>(context); ++state.terminals; state.abandoned = abandoned;
}
void reserved(const HttpRequest&, HttpReply& reply, void* context) {
  auto& state = *static_cast<Reservation*>(context);
  if (state.mode == 2) { Require(reply.Send(JsonResponse(200, "OK", "deferred")) && reply.Defer(0), "defer captured response"); return; }
  Require(reply.Reserve(state.mode == 4 ? 10000 : 20, &state.token, terminal, &state), "reserve borrowed transport");
  if (state.mode == 0 || state.mode == 3) {
    Require(state.handoff->Complete(state.token, JsonResponse(200, "OK", "reserved")), "reserved completion");
    Require(!state.handoff->Complete(state.token, JsonResponse(200, "OK", "duplicate")), "completion consumed once");
  }
}
void deferred_reserved_timeout_and_stop() {
  for (unsigned mode = 0; mode < 5; ++mode) {
    Registered r; ResponseHandoff handoff; Require(handoff.Start(), "handoff writers");
    Reservation state{&handoff, mode}; HttpRouter router; router.SetResponseHandoff(&handoff);
    Require(router.Add("GET", "/reply", reserved, &state), "reserved route");
    r.transport.input = "GET /reply HTTP/1.1\r\nHost: localhost\r\n\r\n";
    r.transport.fail_write = mode == 3;
    Require(router.Dispatch(r.fd, "127.0.0.1"), "dispatch asynchronous borrowed request");
    if (mode == 4) handoff.Stop();
    const uint64_t deadline = [] { timespec now{}; clock_gettime(CLOCK_MONOTONIC, &now); return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1000000 + 3000; }();
    while (!r.transport.finishes.load(std::memory_order_acquire)) {
      timespec now{}; clock_gettime(CLOCK_MONOTONIC, &now);
      Require(static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1000000 < deadline, "bounded asynchronous response completion");
      poll(nullptr, 0, 1);
    }
    handoff.Stop();
    Require(r.transport.finishes == 1 && fcntl(r.fd, F_GETFD) >= 0 && FindHttpTransport(r.fd) == &r.transport, "asynchronous writer finishes once without freeing ingress transport");
    if (mode == 3 || mode == 4) Require(!r.transport.success && r.transport.output.empty(), "failed/abandoned response cannot claim successful write");
    else Require(r.transport.success && r.transport.output.starts_with(mode == 1 ? "HTTP/1.1 504 " : "HTTP/1.1 200 "), "completion/defer/deadline use borrowed transport");
    Require(state.terminals == (mode == 2 ? 0U : 1U) && state.abandoned == (mode == 1 || mode == 4), "reserved terminal callback exactly once with deadline/stop classification");
    if (mode != 2) Require(!handoff.Complete(state.token, JsonResponse(200, "OK", "late")), "late completion rejected after terminal path");
  }
}
void pure_header_boundaries() {
  ParsedRequest parsed;
  Require(RequestParser::Head(nullptr, 0, &parsed.get()) == HttpReadResult::Unavailable &&
          RequestParser::Head("", 0, nullptr) == HttpReadResult::Unavailable, "null header inputs refused");
  std::string base = "GET / HTTP/1.1\r\nHost: localhost\r\nX-Padding: ";
  const std::string exact = base + std::string(kHttpMaxHeaderBytes - base.size() - 4, 'x') + "\r\n\r\n";
  Require(RequestParser::Head(exact.data(), exact.size(), &parsed.get()) == HttpReadResult::Ok, "exact header ceiling accepted");
  const std::string oversized = base + std::string(kHttpMaxHeaderBytes - base.size() - 3, 'x') + "\r\n\r\n";
  Require(RequestParser::Head(oversized.data(), oversized.size(), &parsed.get()) == HttpReadResult::HeaderTooLarge, "ceiling plus one refused");
  for (const std::string& headers : {std::string(), std::string("Connection: KEEP-ALIVE\r\n"), std::string("Connection: close\r\nConnection: keep-alive\r\n"), std::string("Connection: upgrade\r\n"), std::string("Connection: keep-alive, close\r\n")}) {
    const std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\n" + headers + "\r\n";
    Require(RequestParser::Head(request.data(), request.size(), &parsed.get()) == HttpReadResult::Ok, "Connection framing parsed");
    Require(parsed.get().connection_close == (!headers.empty() && headers.find("KEEP-ALIVE") == std::string::npos), "close/unknown tokens cannot be undone");
  }
  std::string embedded = "GET / HTTP/1.1\r\nHost: localhost\r\nX: a";
  embedded.push_back(0); embedded += "b\r\n\r\n";
  Require(RequestParser::Head(embedded.data(), embedded.size(), &parsed.get()) == HttpReadResult::BadRequest, "embedded NUL cannot split framing");
}
void metadata_head_and_failed_writes() {
  Registered r;
  timespec started{}; clock_gettime(CLOCK_MONOTONIC, &started);
  auto response = JsonResponse(200, "OK", "body");
  Require(ResponseWriter::Write(r.fd, response, false, started) && r.transport.success, "writer uses registered transport");
  const auto first = r.transport.output;
  Require(first.find("Connection: keep-alive\r\n") != std::string::npos && first.ends_with("\r\n\r\nbody"), "keep-alive and body framing");
  const auto date = first.find("Date: "); Require(date != std::string::npos && first.find("Date: ", date + 1) == std::string::npos, "exactly one generated Date");
  tm utc{}; const auto value = first.substr(date + 6, 29); const char* end = strptime(value.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &utc);
  Require(end && !*end && value.ends_with(" GMT"), "Date uses IMF-fixdate GMT");
  r.transport.output.clear(); r.transport.DisallowReuse();
  Require(ResponseWriter::Write(r.fd, response, true, started), "HEAD write");
  Require(r.transport.output.ends_with("\r\n\r\n") && r.transport.output.find("Content-Length: 4\r\n") != std::string::npos && r.transport.output.find("Connection: close\r\n") != std::string::npos, "HEAD preserves size without body and honors forced close");
  r.transport.fail_write = true; Require(!ResponseWriter::Write(r.fd, response, false, started) && !r.transport.success, "failed TLS write cannot be reused");
  for (const char* extra : {"Date: forged\r\n", "X-Unit: yes\r\ndAtE: forged\r\n", "Server-Timing: forged\r\n"}) {
    response.extra_headers = extra; const auto writes = r.transport.writes;
    Require(!ResponseWriter::Write(r.fd, response, false, started) && writes == r.transport.writes, "generated metadata cannot be duplicated by extra headers");
  }
}
}  // namespace
int main() {
  fortis::testing::UnitSuite suite;
  suite.Run("registry capacity/collision/reuse", registry_capacity_collisions_and_reuse);
  suite.Run("borrowed close ownership", close_transfers_borrowed_ownership);
  suite.Run("registry concurrent publication", registry_thread_safety);
  suite.Run("borrowed fragmented parser", borrowed_request_parser);
  suite.Run("deferred/reserved/timeout/disconnect/stop", deferred_reserved_timeout_and_stop);
  suite.Run("pure header limits/Connection/NUL", pure_header_boundaries);
  suite.Run("Date/HEAD/connection/write failure", metadata_head_and_failed_writes);
  return suite.Finish("http_transport_unit_test");
}
