#include "https_listener_internal.h"
#include "unit_test_support.h"
#include <filesystem>
#include <fstream>
#include <thread>
#include <atomic>
#include <sys/resource.h>

namespace {
using namespace fortis::app;
using namespace fortis::app::https_detail;
using fortis::testing::Require;
using fortis::testing::TemporaryDirectory;
namespace fs = std::filesystem;

struct Sockets {
  int fd[2]{-1, -1};
  Sockets() { Require(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0, "socketpair"); }
  ~Sockets() { for (int f : fd) if (f >= 0) close(f); }
  Sockets(const Sockets&) = delete;
  Sockets& operator=(const Sockets&) = delete;
  int Take() { const int result = fd[0]; fd[0] = -1; return result; }
  std::string Output() const {
    std::string result;
    char bytes[8192];
    for (;;) {
      const ssize_t n = recv(fd[1], bytes, sizeof(bytes), MSG_DONTWAIT);
      if (n <= 0) break;
      result.append(bytes, static_cast<size_t>(n));
    }
    return result;
  }
};
std::unique_ptr<Ingress> ingress() {
  auto value = std::make_unique<Ingress>();
  value->domain = "localhost";
  value->ip = "127.0.0.1";
  return value;
}
std::string wire(const std::string& target = "/v1/prices", const std::string& headers = "", const char* method = "GET") {
  return std::string(method) + " " + target + " HTTP/1.1\r\nHost: localhost\r\n" + headers + "\r\n";
}
void head(Connection& connection, const std::string& bytes) {
  connection.input.assign(bytes.begin(), bytes.end());
  connection.head_size = bytes.size();
}
void completion_wake_and_publication() {
  {
    struct SoftDescriptorLimit {
      rlimit saved{};
      SoftDescriptorLimit() {
        Require(getrlimit(RLIMIT_NOFILE, &saved) == 0, "read descriptor limit");
        rlimit limited = saved; limited.rlim_cur = 0;
        Require(setrlimit(RLIMIT_NOFILE, &limited) == 0, "temporarily exhaust descriptor allowance");
      }
      ~SoftDescriptorLimit() { (void)setrlimit(RLIMIT_NOFILE, &saved); }
    } limit;
    CompletionWake unavailable;
    Require(!unavailable.Open() && unavailable.ReadFd() < 0 && unavailable.WriteFd() < 0,
            "startup refuses an unavailable pipe without partial ownership");
  }
  CompletionWake wake;
  Require(wake.Open(), "completion notification channel opens");
  const int reader = wake.ReadFd(), writer = wake.WriteFd();
  Require(wake.Open() && wake.ReadFd() == reader && wake.WriteFd() == writer, "opening is idempotent");
  for (int fd : {reader, writer}) {
    Require((fcntl(fd, F_GETFL) & O_NONBLOCK) != 0, "completion channel cannot block a worker");
    Require((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "completion descriptors do not survive exec");
  }
  pollfd ready{reader, POLLIN, 0};
  Require(poll(&ready, 1, 0) == 0, "empty completion channel is not readable");
  for (int i = 0; i < 100000; ++i) Require(CompletionWake::Notify(writer), "notification delivers or coalesces at capacity");
  Require(poll(&ready, 1, 0) == 1, "full notification buffer stays readable without blocking writers");
  for (int i = 0; i < 128; ++i) wake.Drain();
  Require(poll(&ready, 1, 0) == 0, "bounded drain batches consume all queued notifications");
  Require(!CompletionWake::Notify(-1), "unavailable channel explicitly retains timed fallback");
  for (int cycle = 0; cycle < 200; ++cycle) {
    auto connection = std::make_unique<Connection>(fortis::base::FileDescriptor{}, "peer", nullptr, writer);
    Connection* borrowed = connection.get();
    std::thread worker([borrowed] { borrowed->Written(true); borrowed->Finish(); });
    while (connection->phase.load(std::memory_order_acquire) != Phase::Finished) std::this_thread::yield();
    Require(connection->written, "completion state publishes prior response writes");
    // Intentionally destroy before joining: Finish must use only the copied
    // ingress-owned descriptor after publishing the terminal phase.
    connection.reset();
    worker.join();
    Require(poll(&ready, 1, 0) == 1, "completion notifies reactor even if transport has been reaped");
    wake.Drain();
  }
  std::atomic<bool> notify{false};
  std::thread worker([&] {
    while (!notify.load(std::memory_order_acquire)) std::this_thread::yield();
    CompletionWake::Notify(writer);
  });
  notify.store(true, std::memory_order_release);
  const int count = poll(&ready, 1, 1000);
  worker.join();
  Require(count == 1 && (ready.revents & POLLIN), "worker completion wakes an actual poll wait");
  wake.Drain();
}

void buffered_input_readiness() {
  Connection connection(fortis::base::FileDescriptor{}, "peer", nullptr);
  Require(!connection.HasBufferedInput(), "empty request must wait for socket input");
  const std::string partial = "GET / HTTP/1.1\r\nHost: localhost\r\n";
  connection.input.assign(partial.begin(), partial.end());
  Require(!connection.HasBufferedInput(), "partial retained header cannot cause a busy poll loop");
  connection.input.push_back('\r'); connection.input.push_back('\n');
  Require(connection.HasBufferedInput(), "complete retained header advances without timeout");
  connection.head_size = connection.input.size();
  connection.frame_size = connection.input.size() + 1;
  Require(!connection.HasBufferedInput(), "incomplete body waits for socket input");
  connection.input.push_back('x');
  Require(connection.HasBufferedInput(), "complete retained body advances without timeout");
}

void policy_domains() {
  for (const auto& name : {std::string("localhost"), std::string("node.fortisnet.io"), std::string("0-9.a"), std::string(63, 'a'),
                           std::string(63, 'a') + "." + std::string(63, 'b') + "." + std::string(63, 'c') + "." + std::string(61, 'd')})
    Require(valid_domain(name), "valid configured DNS name");
  for (const auto& name : {std::string(), std::string(".a"), std::string("a."), std::string("a..b"), std::string("-a.b"),
                           std::string("a-.b"), std::string("a._b"), std::string("A.b"), std::string("a/b"), std::string("a\nb"),
                           std::string(64, 'a'), std::string(254, 'b')})
    Require(!valid_domain(name), "invalid configured DNS name refused");
}
void policy_authorities() {
  auto value = ingress();
  for (const char* host : {"localhost", "LOCALHOST", "LocalHost:443", "127.0.0.1", "127.0.0.1:8080", "localhost:1", "localhost:65535", "localhost:00443"})
    Require(value->HostAllowed(host), "configured authority and valid numeric port");
  for (const char* host : {"", "evil.test", "localhost.evil", "user@localhost", "localhost.", "localhost:", "localhost:0", "localhost:65536", "localhost:99999", "localhost:000000", "localhost:-1", "localhost:+443", "localhost:44x", "localhost:443:80", " localhost", "[127.0.0.1]", "localhost\r\nX: y"})
    Require(!value->HostAllowed(host), "unconfigured or malformed authority refused");
  // Address-only: the empty domain must never match an empty or absent host.
  value->domain.clear();
  Require(value->HostAllowed("127.0.0.1") && value->HostAllowed("127.0.0.1:443"), "address-only authority");
  for (const char* host : {"", ":443", "localhost"})
    Require(!value->HostAllowed(host), "address-only node refuses every name");
}
void policy_paths() {
  for (const char* path : {"/", "/healthz", "/v1/prices", "/probes", "/probe-other", "/v1/%E2%82%AC", "/v1/a%20b", "/.well-known/acme-challenge/a_B-9"})
    Require(public_path(path), "ordinary public path");
  for (const char* path : {"/probe", "/probe/", "/probe/async", "/%70robe", "/pro%62e%2fasync", "/%2e/probe", "/v1/../probe", "/v1/%2e%2E/probe", "//probe", "/probe//async", "/%2570robe", "/%00", "/%0d", "/%7f", "/%5cprobe", "/\\probe", "/%3fprobe", "/%23probe", "/%", "/%1", "/%gg", "/%1g", "/%g1"})
    Require(!public_path(path), "diagnostic/ambiguous/invalid escaped path refused");
  for (unsigned c = 0; c < 32; ++c) {
    char path[16];
    snprintf(path, sizeof(path), "/v1/%%%02X", c);
    Require(!public_path(path), "every escaped control refused");
  }
}
void rate_burst_and_refill() {
  auto value = ingress();
  for (unsigned n = 0; n < 400; ++n) Require(value->RateAllowed("one", 1000), "full initial burst");
  Require(!value->RateAllowed("one", 1000), "burst plus one refused");
  Require(!value->RateAllowed("one", 1009), "fractional token insufficient");
  Require(value->RateAllowed("one", 1010), "exact 10ms replenishes one token");
  Require(!value->RateAllowed("one", 1010), "refilled token consumed once");
  Require(!value->RateAllowed("one", 0), "backward clock cannot grant a burst");
  Require(value->RateAllowed("two", 1010), "independent peer bucket");
  for (unsigned n = 0; n < 400; ++n) Require(value->RateAllowed("one", 1000000), "long idle refill capped at burst");
  Require(!value->RateAllowed("one", 1000000), "idle refill never exceeds cap");
}
void rate_table_bounds() {
  auto value = ingress();
  for (unsigned n = 0; n < 512; ++n) Require(value->RateAllowed(std::to_string(n), 1000), "bounded identity table filled");
  Require(!value->RateAllowed("overflow", 61000), "idle eviction is strictly after 60s");
  Require(!value->RateAllowed("overflow", 0), "clock regression cannot evict live identity");
  Require(value->RateAllowed("0", 61000), "existing identity still usable at full table");
  Require(value->RateAllowed("overflow", 61001), "old idle identity reusable");
  Require(value->rates[0].peer == "0" && value->rates[1].peer == "overflow", "active identity preserved when evicting");
}
void certificate_file_bounds() {
  TemporaryDirectory directory;
  const fs::path root(directory.path());
  const int dir = open(directory.path(), O_RDONLY | O_DIRECTORY);
  Require(dir >= 0, "open fixture directory");
  struct Close { int fd; ~Close() { close(fd); } } guard{dir};
  std::vector<unsigned char> bytes;
  auto put = [&](const char* name, size_t size, mode_t mode) {
    if (fs::exists(root / name)) Require(chmod((root / name).c_str(), 0600) == 0, "make fixture writable");
    std::ofstream file(root / name, std::ios::binary); file << std::string(size, 'x'); Require(file.good(), "write fixture"); file.close();
    Require(chmod((root / name).c_str(), mode) == 0, "fixture permission");
  };
  for (size_t size : {size_t{1}, size_t{65536}}) {
    put("key", size, 0600);
    Require(read_file(dir, "key", true, bytes) && bytes.size() == size + 1 && bytes.back() == 0, "bounded key is NUL-terminated");
  }
  for (mode_t mode : {0400, 0600}) {
    put("key", 3, mode); Require(read_file(dir, "key", true, bytes), "owner-only key readable");
  }
  for (mode_t mode : {0640, 0620, 0604, 0602, 0644}) {
    put("key", 3, mode); Require(!read_file(dir, "key", true, bytes), "group/world accessible keys refused");
    Require(read_file(dir, "key", false, bytes), "public certificates may be readable");
  }
  for (size_t size : {size_t{0}, size_t{65537}}) {
    put("bad", size, 0600); Require(!read_file(dir, "bad", true, bytes), "empty/oversized key refused");
  }
  fs::create_directory(root / "subdir");
  fs::create_symlink(root / "key", root / "link");
  Require(mkfifo((root / "fifo").c_str(), 0600) == 0, "fixture FIFO");
  for (const char* name : {"missing", "subdir", "link", "fifo"})
    Require(!read_file(dir, name, false, bytes), "nonregular files refused without blocking");
  Require(!read_file(-1, "key", true, bytes), "invalid directory refused");
}
void request_refusals() {
  const std::pair<std::string, int> cases[] = {
    {"GET / HTTP/1.1\r\n\r\n", 400},
    {wire("/", "Host: localhost\r\n"), 400},
    {wire("/", "Transfer-Encoding: chunked\r\n"), 400},
    {wire("/", "Content-Length: 524289\r\n", "POST"), 413},
    {wire("/", "Content-Length: 1\r\nContent-Length: 1\r\n", "POST"), 400},
    {"GET / HTTP/1.1\r\nHost: evil.test\r\n\r\n", 421},
    {"GET / HTTP/1.1\r\nHost: localhost:65536\r\n\r\n", 421},
    {wire("/%70robe/async"), 404},
    {wire("/a/../probe"), 404},
    {wire("/", "Expect: 100-continue\r\n", "POST"), 417},
    {wire("/", "Content-Length: 1\r\n", "GET"), 405},
    {wire("/", "Content-Length: 1\r\n", "HEAD"), 405},
    {wire("/", "Content-Length: 1\r\n", "OPTIONS"), 405},
  };
  for (const auto& [request, status] : cases) {
    auto value = ingress(); Sockets sockets; Connection c(fortis::base::FileDescriptor(sockets.Take()), "peer", nullptr);
    Require(c.Start(), "connection start"); head(c, request);
    Require(!value->AdmitHead(c) && c.phase == Phase::Finished && !c.keep_alive, "refused frame never dispatched/reused");
    Require(sockets.Output().starts_with("HTTP/1.1 " + std::to_string(status) + " "), "correct public refusal status");
    Require(!c.upload && value->protocol_refusals == 1, "refused request has no upload admission");
  }
}
void request_frames_and_raw_target() {
  for (const std::string& headers : {std::string(), std::string("Connection: keep-alive\r\n"), std::string("Connection: close\r\nConnection: keep-alive\r\n")}) {
    auto value = ingress(); Sockets sockets; Connection c(fortis::base::FileDescriptor(sockets.Take()), "peer", nullptr); Require(c.Start(), "start");
    const auto request = wire("/v1/prices?ids=a%2Fb&signature=a%2Bb", headers);
    head(c, request); Require(value->AdmitHead(c), "valid header admitted");
    Require(std::string(c.parsed.get().raw_target) == "/v1/prices?ids=a%2Fb&signature=a%2Bb" && std::string(c.parsed.get().path) == "/v1/prices", "signed raw target preserved while query separated");
    Require(c.frame_size == request.size() && !c.keep_alive, "plaintext redirect is one request only");
  }
  auto value = ingress(); Sockets sockets; Connection c(fortis::base::FileDescriptor(sockets.Take()), "peer", nullptr); Require(c.Start(), "start");
  head(c, wire("/v1/devices", "Content-Length: 524288\r\n", "POST"));
  Require(value->AdmitHead(c) && c.upload && c.frame_size == c.head_size + 524288, "maximum body admitted before allocation");
  Require(c.input.size() == c.head_size && !c.parsed.get().body, "header admission does not allocate/read the body");
}
void upload_limits() {
  for (bool same_peer : {false, true}) {
    auto value = ingress(); const size_t limit = same_peer ? kPerClientUploads : kUploads;
    for (size_t i = 0; i < limit; ++i) {
      value->connections[i] = std::make_unique<Connection>(fortis::base::FileDescriptor{}, same_peer ? "peer" : std::to_string(i), nullptr);
      value->connections[i]->upload = true;
    }
    Sockets sockets; Connection c(fortis::base::FileDescriptor(sockets.Take()), "peer", nullptr); Require(c.Start(), "start");
    head(c, wire("/", "Content-Length: 1\r\n", "POST"));
    Require(!value->AdmitHead(c) && sockets.Output().starts_with("HTTP/1.1 503 "), "global/per-peer upload limit refuses before allocation");
    value->connections[0].reset();
    Sockets next; Connection allowed(fortis::base::FileDescriptor(next.Take()), "peer", nullptr); Require(allowed.Start(), "start");
    head(allowed, wire("/", "Content-Length: 1\r\n", "POST"));
    Require(value->AdmitHead(allowed) && allowed.upload, "one freed upload slot reusable");
  }
}
void health_rate_exemption() {
  auto value = ingress();
  for (unsigned i = 0; i < 400; ++i) Require(value->RateAllowed("peer", now_ms()), "consume rate quota");
  // Prevent wall-clock scheduling from adding a token while checking the policy.
  value->rates[0].updated = now_ms() + 10000;
  value->rates[0].tokens = 0;
  Sockets sockets; Connection c(fortis::base::FileDescriptor(sockets.Take()), "peer", nullptr); Require(c.Start(), "start"); head(c, wire("/healthz"));
  Require(value->AdmitHead(c), "health never consumes client rate quota");
  Sockets next; Connection other(fortis::base::FileDescriptor(next.Take()), "peer", nullptr); Require(other.Start(), "start"); head(other, wire());
  Require(!value->AdmitHead(other) && next.Output().starts_with("HTTP/1.1 429 "), "application route still rate limited");
}
void connection_io_and_reuse() {
  Sockets sockets; Connection c(fortis::base::FileDescriptor(sockets.Take()), "peer", nullptr); Require(c.Start() && !c.Start(), "start exactly once");
  Require((fcntl(c.fd, F_GETFL, 0) & O_NONBLOCK) != 0, "nonblocking socket");
  char bytes[16]; Require(Connection::Receive(&c, reinterpret_cast<unsigned char*>(bytes), sizeof(bytes)) == MBEDTLS_ERR_SSL_WANT_READ, "empty socket wants read");
  head(c, "firstNEXT"); c.frame_size = 5;
  Require(c.Read(bytes, 2) == 2 && std::string(bytes, 2) == "fi", "partial borrowed read");
  Require(c.Read(bytes, sizeof(bytes)) == 3 && std::string(bytes, 3) == "rst" && c.Read(bytes, sizeof(bytes)) == 0, "reader cannot consume pipelined next frame");
  c.upload = true; c.keep_alive = true; c.Written(true); c.Finish();
  Require(c.phase.load(std::memory_order_acquire) == Phase::Finished && c.KeepAlive(), "finish publishes successful reusable request");
  c.Reset(1000);
  Require(std::string(c.input.begin(), c.input.end()) == "NEXT" && c.phase == Phase::Reading && c.deadline == 6000 && !c.idle, "pipelined bytes retain order and get frame deadline");
  Require(!c.upload && !c.keep_alive && !c.written && c.frame_size == 0 && c.cursor == 0 && c.parsed.get().path == nullptr, "reuse releases request and upload state");
  c.input.clear(); c.Reset(2000);
  Require(c.idle && c.deadline == 17000, "empty connection uses idle deadline");
  timespec deadline{}; clock_gettime(CLOCK_MONOTONIC, &deadline); ++deadline.tv_sec;
  Require(c.Write("abc", 3, deadline) && sockets.Output() == "abc", "transport writes plaintext exactly");
  deadline = {}; Require(!c.Write("x", 1, deadline) && c.Write("", 0, deadline), "expired write refuses bytes but empty write completes");
  c.keep_alive = true; c.DisallowReuse(); Require(!c.KeepAlive(), "writer can force close");
}
void backpressure_does_not_pin_reactor() {
  Sockets sockets; Connection c(fortis::base::FileDescriptor(sockets.Take()), "peer", nullptr); Require(c.Start(), "start");
  std::string body(65536, 'x');
  while (send(c.fd, body.data(), body.size(), 0) > 0) {}
  timespec deadline{}; clock_gettime(CLOCK_MONOTONIC, &deadline); deadline.tv_sec += 30;
  const auto started = now_ms();
  Require(!c.Write("x", 1, deadline) && now_ms() - started < 1000, "reactor write returns immediately on backpressure");
}
void finished_and_deadline_ownership() {
  auto value = ingress();
  for (size_t i = 0; i < 5; ++i) value->connections[i] = std::make_unique<Connection>(fortis::base::FileDescriptor{}, "peer", nullptr);
  value->connections[0]->phase = Phase::Reading; value->connections[0]->deadline = 1000;
  value->connections[1]->phase = Phase::Dispatched; value->connections[1]->deadline = 0;
  value->connections[2]->keep_alive = true; value->connections[2]->written = true; value->connections[2]->Finish();
  value->connections[3]->keep_alive = true; value->connections[3]->written = false; value->connections[3]->Finish();
  value->connections[4]->phase = Phase::QueuedHandshake; value->connections[4]->deadline = 1001;
  value->Reap(1000);
  Require(!value->connections[0] && value->expired == 1, "deadline expires at exact boundary");
  Require(value->connections[1] && value->connections[1]->phase == Phase::Dispatched, "never destroy transport borrowed by worker/handoff");
  Require(value->connections[2] && value->connections[2]->phase == Phase::Reading && value->connections[2]->deadline == 16000, "finished successful response reused");
  Require(!value->connections[3] && value->connections[4], "failed write closed; unexpired handshake retained");
  value->Reap(1001); Require(!value->connections[4] && value->expired == 2, "queued handshake has absolute deadline");
}
void fragmented_request_and_pipeline() {
  auto value = ingress(); Sockets sockets; Connection c(fortis::base::FileDescriptor(sockets.Take()), "peer", nullptr); Require(c.Start(), "start");
  const std::string first = wire("/first?signature=a%2Bb");
  for (size_t i = 0; i < first.size() - 1; ++i) {
    Require(send(sockets.fd[1], first.data() + i, 1, 0) == 1, "send fragmented byte"); value->Advance(c);
    Require(c.phase == Phase::Reading, "incomplete header never dispatches");
  }
  const std::string tail = first.substr(first.size() - 1) + wire("/second");
  Require(send(sockets.fd[1], tail.data(), tail.size(), 0) == static_cast<ssize_t>(tail.size()), "send pipelined tail");
  value->Advance(c);
  Require(c.phase == Phase::Finished && c.frame_size == first.size(), "one request dispatched with exact frame boundary");
  const auto reply = sockets.Output();
  Require(reply.starts_with("HTTP/1.1 308 ") && reply.find("Location: https://localhost/first?signature=a%2Bb\r\n") != std::string::npos, "canonical redirect preserves signed target");
  Require(reply.find("/second") == std::string::npos, "pipelined bytes never leak into first response");
}
void incomplete_and_oversized_frames() {
  auto value = ingress();
  { Sockets sockets; Connection c(fortis::base::FileDescriptor(sockets.Take()), "peer", nullptr); Require(c.Start(), "start");
    head(c, wire("/", "Content-Length: 2\r\n", "POST")); c.head_size = 0;
    c.input.push_back('a'); value->Advance(c);
    Require(c.upload && c.phase == Phase::Reading && c.input.size() + 1 == c.frame_size, "partial body stays with ingress");
    Require(send(sockets.fd[1], "b", 1, 0) == 1, "last body byte"); value->Advance(c);
    Require(c.phase == Phase::Finished, "complete upload dispatches once"); }
  { Sockets sockets; Connection c(fortis::base::FileDescriptor(sockets.Take()), "peer", nullptr); Require(c.Start(), "start");
    c.input.assign(fortis::http::kHttpMaxHeaderBytes, 'x'); value->Advance(c);
    Require(c.phase == Phase::Finished && sockets.Output().starts_with("HTTP/1.1 431 "), "header ceiling without terminator refused"); }
  { Sockets sockets; Connection c(fortis::base::FileDescriptor(sockets.Take()), "peer", nullptr); Require(c.Start(), "start");
    shutdown(sockets.fd[1], SHUT_WR); value->Advance(c);
    Require(c.phase == Phase::Finished && !c.keep_alive, "peer EOF never reuses connection"); }
}
struct TcpListener {
  int fd = ListenOnPort(0, 128);
  sockaddr_in address{};
  TcpListener() {
    Require(fd >= 0 && nonblocking(fd), "ephemeral nonblocking listener");
    socklen_t size = sizeof(address); Require(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0, "listener address");
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  }
  ~TcpListener() { close(fd); }
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;
  void Ready() const {
    pollfd descriptor{fd, POLLIN, 0};
    Require(poll(&descriptor, 1, 1000) == 1 && (descriptor.revents & POLLIN), "listener has pending connection");
  }
  int Connect() const {
    const int client = socket(AF_INET, SOCK_STREAM, 0); Require(client >= 0, "client socket");
    if (connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) { close(client); throw std::runtime_error("loopback connect"); }
    return client;
  }
};
void inherited_listeners() {
  // The orchestrator binds; the node only adopts what FORTIS_LISTEN_FDS names.
  TcpListener owner;
  const std::string map = "8080=" + std::to_string(owner.fd) + ",8082=" + std::to_string(owner.fd);
  Require(setenv("FORTIS_LISTEN_FDS", map.c_str(), 1) == 0, "set inherited listener map");
  Require(ListenOnPort(8080, 64) == owner.fd && ListenOnPort(8082, 32) == owner.fd, "listed ports adopt the inherited socket");
  Require((fcntl(owner.fd, F_GETFD) & FD_CLOEXEC) != 0, "adopted listener never leaks to children");
  const int client = owner.Connect(); owner.Ready(); close(client);
  errno = 0; Require(ListenOnPort(8081, 16) < 0 && errno == EADDRNOTAVAIL, "unlisted port is refused, never bound");
  const int plain = socket(AF_INET, SOCK_STREAM, 0); Require(plain >= 3, "unbound socket");
  const std::string unbound = "8080=" + std::to_string(plain);
  Require(setenv("FORTIS_LISTEN_FDS", unbound.c_str(), 1) == 0 && ListenOnPort(8080, 64) < 0, "non-listening socket refused");
  close(plain);
  for (const char* invalid : {"8080", "8080=", "=3", "8080=3;8081=4", "8080=99999", "a=3", "8080=2"}) {
    Require(setenv("FORTIS_LISTEN_FDS", invalid, 1) == 0 && ListenOnPort(8080, 64) < 0, "malformed or reserved descriptor map refused");
  }
  Require(unsetenv("FORTIS_LISTEN_FDS") == 0, "clear inherited listener map");
  const int own = ListenOnPort(0, 16); Require(own >= 0, "unmanaged process still binds"); close(own);
}
void accept_connection_limits_and_batch() {
  TcpListener listener;
  for (bool per_peer : {false, true}) {
    auto value = ingress(); const size_t count = per_peer ? kPerClientConnections : kConnections;
    for (size_t i = 0; i < count; ++i) value->connections[i] = std::make_unique<Connection>(fortis::base::FileDescriptor{}, per_peer ? "127.0.0.1" : "other", nullptr);
    const int denied = listener.Connect(); listener.Ready(); value->Accept(listener.fd, false); close(denied);
    Require(value->accepted == 0 && value->rejected == 1, "total/per-peer connection cap");
    value->connections[0].reset(); const int allowed = listener.Connect(); listener.Ready(); value->Accept(listener.fd, false); close(allowed);
    Connection* accepted = nullptr;
    for (const auto& connection : value->connections) if (connection && connection->fd >= 0) accepted = connection.get();
    Require(value->accepted == 1 && value->rejected == 1 && accepted, "freed connection capacity reusable");
    Require(accepted->peer == "127.0.0.1" && accepted->phase == Phase::Reading, "real peer preserved and plaintext skips handshake");
    int enabled = 0; socklen_t size = sizeof(enabled);
    Require(getsockopt(accepted->fd, IPPROTO_TCP, TCP_NODELAY, &enabled, &size) == 0 && enabled != 0, "TCP_NODELAY enabled");
  }
  auto value = ingress();
  struct Clients { std::vector<int> fds; ~Clients() { for (int fd : fds) close(fd); } } clients;
  for (unsigned i = 0; i < 65; ++i) clients.fds.push_back(listener.Connect());
  listener.Ready(); value->Accept(listener.fd, false);
  Require(value->accepted > 0 && value->accepted <= 64, "accept batch bounded at 64");
  for (unsigned attempt = 0; attempt < 4 && value->accepted < 65; ++attempt) { listener.Ready(); value->Accept(listener.fd, false); }
  Require(value->accepted == 65, "remaining connections serviced by later batches");
}

void challenge_files_and_redirects() {
  TemporaryDirectory directory; const fs::path root(directory.path());
  std::ofstream(root / "valid") << "proof";
  std::ofstream(root / "max") << std::string(4096, 'x');
  std::ofstream(root / "large") << std::string(4097, 'x');
  { std::ofstream empty(root / "empty"); Require(empty.good(), "empty challenge fixture"); }
  fs::create_directory(root / "dir"); fs::create_symlink(root / "valid", root / "link");
  Require(mkfifo((root / "fifo").c_str(), 0600) == 0, "FIFO challenge fixture");
  auto value = ingress(); value->challenges.reset(open(directory.path(), O_RDONLY | O_DIRECTORY)); Require(value->challenges.get() >= 0, "challenge directory");
  for (const char* token : {"valid", "max", "large", "empty", "dir", "link", "fifo", "missing", "../valid", "a/b", "a%2fb", "a.b", "", "a?x"}) {
    Sockets sockets; Connection c(fortis::base::FileDescriptor(sockets.Take()), "peer", nullptr); Require(c.Start(), "start");
    head(c, wire(std::string("/.well-known/acme-challenge/") + token));
    const auto parsed = fortis::http::RequestParser::Head(c.input.data(), c.head_size, &c.parsed.get());
    Require(parsed == fortis::http::HttpReadResult::Ok, "parse challenge request");
    value->Redirect(c); const auto response = sockets.Output();
    const bool ok = !strcmp(token, "valid") || !strcmp(token, "max");
    Require(response.starts_with(ok ? "HTTP/1.1 200 " : "HTTP/1.1 404 "), "restricted regular challenge file only");
  }
  { Sockets sockets; Connection c(fortis::base::FileDescriptor(sockets.Take()), "peer", nullptr); Require(c.Start(), "start");
    head(c, wire("/" + std::string(224, 'x'))); Require(value->AdmitHead(c), "long valid target admitted");
    value->Redirect(c); Require(sockets.Output().starts_with("HTTP/1.1 414 "), "redirect overflow refused without truncating target"); }
  for (const char* method : {"GET", "HEAD", "POST"}) {
    Sockets sockets; Connection c(fortis::base::FileDescriptor(sockets.Take()), "peer", nullptr); Require(c.Start(), "start");
    head(c, wire("/.well-known/acme-challenge/valid", "", method)); Require(value->AdmitHead(c), "admit challenge method");
    value->Redirect(c); const auto response = sockets.Output();
    Require(response.starts_with(!strcmp(method, "POST") ? "HTTP/1.1 405 " : "HTTP/1.1 200 "), "ACME serves only GET/HEAD");
    if (!strcmp(method, "POST")) Require(response.find("Allow: GET, HEAD\r\n") != std::string::npos, "405 names allowed challenge methods");
    if (!strcmp(method, "HEAD")) Require(response.ends_with("\r\n\r\n") && response.find("Content-Length: 5\r\n") != std::string::npos, "HEAD has representation length without body");
  }
}
}  // namespace

int main() {
  fortis::testing::UnitSuite suite;
  suite.Run("completion wake/publication/reap lifetime", completion_wake_and_publication);
  suite.Run("buffered input versus partial-frame readiness", buffered_input_readiness);
  suite.Run("DNS label boundaries", policy_domains);
  suite.Run("Host authorities/ports", policy_authorities);
  suite.Run("public path normalization", policy_paths);
  suite.Run("rate burst/refill/clock regression", rate_burst_and_refill);
  suite.Run("rate identity capacity/eviction", rate_table_bounds);
  suite.Run("certificate file size/type/permissions", certificate_file_bounds);
  suite.Run("request refusal ordering/status", request_refusals);
  suite.Run("frame lengths/raw signed target", request_frames_and_raw_target);
  suite.Run("global/per-peer upload admission", upload_limits);
  suite.Run("health rate exemption", health_rate_exemption);
  suite.Run("connection I/O/reuse", connection_io_and_reuse);
  suite.Run("reactor write backpressure", backpressure_does_not_pin_reactor);
  suite.Run("finished/deadline ownership", finished_and_deadline_ownership);
  suite.Run("fragmented/pipelined frames", fragmented_request_and_pipeline);
  suite.Run("incomplete/oversized/EOF frames", incomplete_and_oversized_frames);
  suite.Run("connection caps/accept fairness", accept_connection_limits_and_batch);
  suite.Run("ACME/redirect filesystem policy", challenge_files_and_redirects);
  suite.Run("orchestrator-owned listeners", inherited_listeners);
  return suite.Finish("https_listener_unit_test");
}
