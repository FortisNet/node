#include "https_test_support.h"
#include "outbound_https.h"
#include <csignal>
#include <thread>

namespace {
using namespace fortis::app;
using namespace fortis::app::https_detail;
using namespace fortis::testing::https;
using fortis::testing::Require;
void healthy(const fortis::http::HttpRequest&, fortis::http::HttpReply& reply, void*) {
  reply.Send(fortis::http::JsonResponse(200, "OK", "{}"));
}
struct Session {
  Client client;
  std::unique_ptr<Connection> server;
  Session(std::shared_ptr<TlsConfiguration> config, const Certificates& certificates, const char* hostname,
          mbedtls_ssl_protocol_version version = MBEDTLS_SSL_VERSION_TLS1_2) {
    int sockets[2]; Require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "TLS socketpair");
    server = std::make_unique<Connection>(fortis::base::FileDescriptor(sockets[0]), "127.0.0.1", std::move(config));
    client.Start(sockets[1], certificates.root / "ca.pem", hostname, version);
    Require(server->Start() && server->BeginHandshake(), "server handshake setup");
  }
  bool Handshake(Ingress& ingress) {
    for (unsigned step = 0; step < 1000; ++step) {
      if (!mbedtls_ssl_is_handshake_over(&client.ssl)) {
        const int result = mbedtls_ssl_handshake(&client.ssl);
        if (result && result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) return false;
      }
      ingress.Advance(*server);
      if (server->phase == Phase::Finished) return false;
      if (mbedtls_ssl_is_handshake_over(&client.ssl) && server->phase == Phase::Reading) return true;
    }
    return false;
  }
  void Send(const char* path = "/healthz", const char* headers = "") {
    const std::string request = std::string("GET ") + path + " HTTP/1.1\r\nHost: LOCALHOST\r\n" + headers + "\r\n";
    const int n = mbedtls_ssl_write(&client.ssl, reinterpret_cast<const unsigned char*>(request.data()), request.size());
    Require(n == static_cast<int>(request.size()), "TLS request written");
  }
  std::string Response() {
    std::string result;
    for (unsigned step = 0; step < 1000; ++step) {
      unsigned char bytes[4096]; const int n = mbedtls_ssl_read(&client.ssl, bytes, sizeof(bytes));
      if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
      Require(n > 0, "complete TLS response before close"); result.append(reinterpret_cast<char*>(bytes), static_cast<size_t>(n));
      const size_t end = result.find("\r\n\r\n"), length = result.find("Content-Length: ");
      if (end != std::string::npos && length != std::string::npos && result.size() >= end + 4 + std::stoul(result.substr(length + 16))) return result;
    }
    throw std::runtime_error("TLS response did not complete");
  }
};
void certificate_validation() {
  Certificates certificates;
  Require(certificates.Load() != nullptr, "valid pair");
  { TlsConfiguration config; Require(!config.Load(certificates.root.string(), "wrong.test", "127.0.0.1"), "wrong DNS SAN refused"); }
  { TlsConfiguration config; Require(!config.Load(certificates.root.string(), "localhost", "127.0.0.2"), "wrong IP SAN refused"); }
  fs::copy_file(certificates.root / "generation/domain.key", certificates.root / "generation/ip.key", fs::copy_options::overwrite_existing);
  { TlsConfiguration config; Require(!config.Load(certificates.root.string(), "localhost", "127.0.0.1"), "mismatched private key refused"); }
  certificates.ip.Save(certificates.root / "generation/ip.key");
  certificates.Write("domain.pem", certificates.domain, false, false, "20000101000000", "20010101000000");
  { TlsConfiguration config; Require(!config.Load(certificates.root.string(), "localhost", "127.0.0.1"), "expired certificate refused"); }
  certificates.Write("domain.pem", certificates.domain, false, false, "20980101000000", "20990101000000");
  { TlsConfiguration config; Require(!config.Load(certificates.root.string(), "localhost", "127.0.0.1"), "not-yet-valid certificate refused"); }
  certificates.Write("domain.pem", certificates.domain, false, false, "20200101000000", "20990101000000", true);
  { TlsConfiguration config; Require(!config.Load(certificates.root.string(), "localhost", "127.0.0.1"), "client-only EKU refused"); }
  certificates.Write("domain.pem", certificates.domain, false, false);
  { Environment ca("FORTIS_CA_BUNDLE_PATH", "/nonexistent/fortis-test-ca"); TlsConfiguration config; Require(!config.Load(certificates.root.string(), "localhost", "127.0.0.1"), "unreadable trust store refused"); }
  { Certificates other; TlsConfiguration config; Require(!config.Load(certificates.root.string(), "localhost", "127.0.0.1"), "different trust root refused"); }
  // An address-only node has no domain: only the address pair is needed, and
  // a stale or absent domain pair must not stop it.
  fs::rename(certificates.root / "generation/domain.pem", certificates.root / "generation/domain.saved");
  { TlsConfiguration config; Require(!config.Load(certificates.root.string(), "localhost", "127.0.0.1"), "configured domain still needs its pair"); }
  { TlsConfiguration config; Require(config.Load(certificates.root.string(), "", "127.0.0.1"), "address-only pair accepted"); }
  { TlsConfiguration config; Require(!config.Load(certificates.root.string(), "", "127.0.0.2"), "address-only pair still checks the address"); }
  fs::rename(certificates.root / "generation/domain.saved", certificates.root / "generation/domain.pem");
  fs::remove(certificates.root / "current");
  { TlsConfiguration config; Require(!config.Load(certificates.root.string(), "localhost", "127.0.0.1"), "missing generation refused"); }
}
void reload_generation_ownership() {
  Certificates certificates;
  auto value = std::make_unique<Ingress>(); value->directory = certificates.root.string(); value->domain = "localhost"; value->ip = "127.0.0.1";
  Require(value->Reload(), "first certificate generation");
  auto old = value->configuration;
  fs::rename(certificates.root / "generation/domain.key", certificates.root / "generation/domain.saved");
  Require(!value->Reload() && value->configuration == old, "invalid reload leaves active pair untouched");
  fs::rename(certificates.root / "generation/domain.saved", certificates.root / "generation/domain.key");
  std::vector<std::shared_ptr<TlsConfiguration>> retained{old}; old.reset();
  for (unsigned n = 1; n < 4; ++n) { Require(value->Reload(), "retained session allows bounded generations"); retained.push_back(value->configuration); }
  const auto current = value->configuration;
  Require(!value->Reload() && value->configuration == current, "generation cap refuses reload atomically");
  retained.erase(retained.begin());
  Require(value->Reload() && value->configuration != current, "released old session makes reload possible");
}
void sni_alpn_versions_and_keepalive() {
  Certificates certificates; const auto config = certificates.Load();
  for (const auto version : {MBEDTLS_SSL_VERSION_TLS1_2, MBEDTLS_SSL_VERSION_TLS1_3}) {
    for (const char* hostname : {"localhost", "LOCALHOST", "127.0.0.1"}) {
      Ingress ingress; ingress.domain = "localhost"; ingress.ip = "127.0.0.1";
      fortis::http::HttpRouter health; Require(health.Add("GET", "/healthz", healthy), "health route"); ingress.health = &health;
      Session session(config, certificates, hostname, version);
      Require(session.Handshake(ingress) && mbedtls_ssl_get_verify_result(&session.client.ssl) == 0, "verified SNI certificate for TLS version");
      const char* protocol = mbedtls_ssl_get_alpn_protocol(&session.client.ssl);
      Require(protocol && !strcmp(protocol, "http/1.1"), "HTTP/1.1 ALPN");
      ingress.connections[0] = std::move(session.server);
      for (unsigned request = 1; request <= kMaxRequests; ++request) {
        session.Send(); ingress.Advance(*ingress.connections[0]);
        const auto response = session.Response();
        Require(response.starts_with("HTTP/1.1 200 ") && response.ends_with("{}"), "TLS health framing/body");
        const bool reusable = request < kMaxRequests;
        Require(ingress.connections[0]->KeepAlive() == reusable, "keep-alive request cap boundary");
        Require(response.find(reusable ? "Connection: keep-alive\r\n" : "Connection: close\r\n") != std::string::npos, "wire connection header matches lifetime");
        ingress.Reap(now_ms()); Require(static_cast<bool>(ingress.connections[0]) == reusable, "successful requests reused only up to limit");
      }
    }
  }
  Ingress ingress; Session unknown(config, certificates, "evil.test");
  Require(!unknown.Handshake(ingress) && ingress.handshake_failures == 1, "unknown SNI fails handshake");
}
void handshake_queue_limits() {
  Certificates certificates; auto value = std::make_unique<Ingress>(); const auto config = certificates.Load();
  for (unsigned i = 0; i < 40; ++i) {
    value->connections[i] = std::make_unique<Connection>(fortis::base::FileDescriptor{}, std::to_string(i), config);
    value->connections[i]->deadline = 2000 + i;
  }
  value->ScheduleHandshakes(1000);
  for (unsigned i = 0; i < 40; ++i)
    Require(value->connections[i]->phase == (i < 32 ? Phase::Handshake : Phase::QueuedHandshake) && value->connections[i]->deadline == 2000 + i, "oldest 32 admitted without resetting absolute deadline");
  value->Reap(2000); value->ScheduleHandshakes(2000);
  Require(!value->connections[0] && value->connections[32]->phase == Phase::Handshake && value->connections[33]->phase == Phase::QueuedHandshake, "expired active handshake releases exactly one slot");
  auto failure = std::make_unique<Ingress>();
  failure->connections[0] = std::make_unique<Connection>(fortis::base::FileDescriptor{}, "one", std::make_shared<TlsConfiguration>());
  failure->connections[0]->deadline = 2000; failure->ScheduleHandshakes(1000);
  Require(failure->handshake_failures == 1 && failure->connections[0]->phase == Phase::Finished, "TLS setup failure finishes without spinning");
}
void application_admission_and_health() {
  Certificates certificates; const auto config = certificates.Load();
  for (unsigned mode = 0; mode < 5; ++mode) {
    Ingress ingress; ingress.domain = "localhost"; ingress.ip = "127.0.0.1";
    HttpWorkerPool workers; ingress.pool = &workers;
    fortis::http::HttpRouter health; Require(health.Add("GET", "/healthz", healthy), "health route"); ingress.health = &health;
    Session session(config, certificates, "localhost"); Require(session.Handshake(ingress), "TLS handshake");
    if (mode == 1) workers.count = kPendingConnectionCapacity;
    if (mode == 2 || mode == 4) workers.stopping = true;
    if (mode == 3) for (size_t i = 0; i < kApplicationRequests; ++i) {
      ingress.connections[i] = std::make_unique<Connection>(fortis::base::FileDescriptor{}, "busy", nullptr);
      ingress.connections[i]->phase = Phase::Dispatched; ingress.connections[i]->reactor_response = false;
    }
    session.Send(mode == 4 ? "/healthz" : "/app"); ingress.Advance(*session.server);
    if (mode == 0) {
      Require(workers.count == 1 && workers.pending[0].client == session.server->fd && !strcmp(workers.pending[0].remote_ip, "127.0.0.1"), "complete request queued with actual peer");
      Require(session.server->phase == Phase::Dispatched && !session.server->reactor_response, "worker exclusively owns dispatched transport");
      workers.count = 0;
    } else {
      const auto response = session.Response();
      Require(response.starts_with(mode == 4 ? "HTTP/1.1 200 " : "HTTP/1.1 503 "), "queue/worker/application cap refusal; isolated health bypass");
    }
  }
}
struct Listener {
  int fd = ListenOnPort(0, 16);
  sockaddr_in address{};
  Listener() {
    Require(fd >= 0 && nonblocking(fd), "ephemeral listener");
    socklen_t size = sizeof(address); Require(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0, "listener port");
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  }
  ~Listener() { if (fd >= 0) close(fd); }
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;
  std::string Port() const { return std::to_string(ntohs(address.sin_port)); }
};
void startup_configuration() {
  Certificates certificates; Listener server;
  Environment cert("FORTIS_HTTPS_CERT_DIR", certificates.root.string()), domain("FORTIS_HTTPS_DOMAIN", "localhost"), ip("FORTIS_HTTPS_IP", "127.0.0.1"), acme("FORTIS_ACME_CHALLENGE_DIR", "");
  HttpWorkerPool workers; fortis::http::HttpRouter health;
  Require(HttpsListener::Enabled(), "configured TLS enabled");
  { HttpsListener unstarted; Require(!unstarted.Run(), "unstarted listener refuses Run without polling forever"); }
  { Environment absent("FORTIS_HTTPS_CERT_DIR", ""); Require(!HttpsListener::Enabled(), "unset/empty TLS disabled"); }
  const std::pair<const char*, const char*> cases[] = {
    {"FORTIS_HTTPS_CERT_DIR", "relative"}, {"FORTIS_HTTPS_DOMAIN", "a..b"}, {"FORTIS_HTTPS_IP", "::1"}, {"FORTIS_HTTPS_IP", "999.1.1.1"},
    {"FORTIS_HTTP_REDIRECT_PORT", "0"}, {"FORTIS_HTTP_REDIRECT_PORT", "65536"}, {"FORTIS_HTTP_REDIRECT_PORT", "-1"}, {"FORTIS_HTTP_REDIRECT_PORT", " 80"}, {"FORTIS_HTTP_REDIRECT_PORT", "123456"},
  };
  for (const auto& [name, value] : cases) {
    Environment invalid(name, value); HttpsListener listener;
    Require(!listener.Start(server.fd, workers, health) && !listener.Run(), "invalid startup configuration refused without running");
    Require(!listener.Start(server.fd, workers, health), "refused startup cannot be reused");
  }
  { HttpsListener listener; Require(!listener.Start(-1, workers, health), "invalid listener refused"); }
  { Environment port("FORTIS_HTTP_REDIRECT_PORT", server.Port()); HttpsListener listener; Require(!listener.Start(server.fd, workers, health), "occupied redirect port refused"); }
  Listener port; const auto available = port.Port(); close(port.fd); port.fd = -1;
  Environment redirect("FORTIS_HTTP_REDIRECT_PORT", available);
  { Environment missing("FORTIS_ACME_CHALLENGE_DIR", (certificates.root / "missing").string()); HttpsListener listener; Require(!listener.Start(server.fd, workers, health), "missing challenge directory refused"); }
  fs::create_directory_symlink("generation", certificates.root / "challenge-link");
  { Environment link("FORTIS_ACME_CHALLENGE_DIR", (certificates.root / "challenge-link").string()); HttpsListener listener; Require(!listener.Start(server.fd, workers, health), "challenge directory symlink refused"); }
  { Environment challenge("FORTIS_ACME_CHALLENGE_DIR", certificates.root.string()); HttpsListener listener;
    Require(listener.Start(server.fd, workers, health) && !listener.Start(server.fd, workers, health), "valid startup is single-use without leaking redirect socket"); }
  Require(fcntl(server.fd, F_GETFD) >= 0, "ingress never owns borrowed listening socket");
}
void signals_and_shutdown_interrupt() {
  InstallShutdownSignals();
  Require(!TakeCertificateReload() && !ShutdownRequested(), "initial signal state");
  Require(raise(SIGHUP) == 0 && TakeCertificateReload() && !TakeCertificateReload(), "reload consumed once");
  Require(raise(SIGHUP) == 0 && raise(SIGHUP) == 0 && TakeCertificateReload() && !TakeCertificateReload(), "reload signals coalesce");
  Certificates certificates; Listener listening;
  Ingress ingress; ingress.listener = listening.fd; ingress.directory = certificates.root.string(); ingress.domain = "localhost"; ingress.ip = "127.0.0.1"; ingress.configuration = certificates.Load();
  const auto initial = ingress.configuration;
  fortis::http::HttpRouter health; Require(health.Add("GET", "/healthz", healthy), "health route"); ingress.health = &health;
  struct Reactor {
    Ingress& ingress;
    bool result = false;
    std::thread thread;
    explicit Reactor(Ingress& value) : ingress(value), thread([this] { result = ingress.Run(); }) {}
    void Stop() { MarkShutdown(); if (thread.joinable()) thread.join(); }
    ~Reactor() { Stop(); }
  } reactor(ingress);
  Require(raise(SIGHUP) == 0, "request live certificate reload");
  Client client; const int socket = ::socket(AF_INET, SOCK_STREAM, 0); Require(socket >= 0, "reactor client socket");
  if (connect(socket, reinterpret_cast<sockaddr*>(&listening.address), sizeof(listening.address)) != 0) { close(socket); throw std::runtime_error("reactor connect"); }
  client.Start(socket, certificates.root / "ca.pem", "localhost", MBEDTLS_SSL_VERSION_TLS1_2);
  const uint64_t deadline = now_ms() + 5000;
  while (!mbedtls_ssl_is_handshake_over(&client.ssl)) {
    const int result = mbedtls_ssl_handshake(&client.ssl);
    Require(result == 0 || result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE, "reactor TLS handshake");
    Require(now_ms() < deadline, "reactor handshake bounded");
    if (result) { pollfd descriptor{socket, static_cast<short>(result == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT), 0}; poll(&descriptor, 1, 10); }
  }
  for (unsigned i = 0; i < 2; ++i) {
    static constexpr char request[] = "GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n";
    Require(mbedtls_ssl_write(&client.ssl, reinterpret_cast<const unsigned char*>(request), sizeof(request) - 1) == sizeof(request) - 1, "reactor health request");
    std::string response;
    while (!response.ends_with("{}")) {
      unsigned char bytes[4096]; const int n = mbedtls_ssl_read(&client.ssl, bytes, sizeof(bytes));
      if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) { pollfd descriptor{socket, POLLIN, 0}; poll(&descriptor, 1, 10); }
      else { Require(n > 0, "reactor health response"); response.append(reinterpret_cast<char*>(bytes), static_cast<size_t>(n)); }
      Require(now_ms() < deadline, "reactor response bounded");
    }
    Require(response.starts_with("HTTP/1.1 200 "), "real reactor served isolated TLS health");
  }
  reactor.Stop();
  Require(ShutdownRequested() && reactor.result && ingress.configuration != initial && !TakeCertificateReload(), "cross-thread shutdown and live reload complete");
  size_t retained = 0;
  for (const auto& connection : ingress.connections) if (connection) {
    ++retained; Require(fortis::http::FindHttpTransport(connection->fd) == connection.get(), "borrowed transports retained until owner shutdown");
  }
  Require(retained == 1, "live socket interrupted but retained through shutdown");
  Listener redirect; const auto port = redirect.Port(); close(redirect.fd); redirect.fd = -1;
  Environment cert("FORTIS_HTTPS_CERT_DIR", certificates.root.string()), domain("FORTIS_HTTPS_DOMAIN", "localhost"), ip("FORTIS_HTTPS_IP", "127.0.0.1"), http("FORTIS_HTTP_REDIRECT_PORT", port), acme("FORTIS_ACME_CHALLENGE_DIR", "");
  HttpWorkerPool workers; HttpsListener wrapper;
  Require(wrapper.Start(listening.fd, workers, health) && wrapper.Run(), "public listener API honors already requested shutdown");
}

}  // namespace
int main() {
  if (!fortis::http::OutboundHttpsInitialize()) return 1;
  fortis::testing::UnitSuite suite;
  suite.Run("certificate trust/name/key/time/purpose", certificate_validation);
  suite.Run("reload atomicity/generation ownership", reload_generation_ownership);
  suite.Run("SNI/ALPN/TLS1.2/TLS1.3/100 keep-alive requests", sni_alpn_versions_and_keepalive);
  suite.Run("bounded oldest-first handshake queue", handshake_queue_limits);
  suite.Run("application limits/isolated health", application_admission_and_health);
  suite.Run("startup configuration/failure cleanup", startup_configuration);
  suite.Run("reload signals/shutdown ownership", signals_and_shutdown_interrupt);
  return suite.Finish("https_tls_unit_test");
}
