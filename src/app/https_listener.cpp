#include "https_listener_internal.h"
#include "file_descriptor.h"

namespace fortis::app {
namespace https_detail {
uint64_t now_ms() {
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1000000;
}
int random_bytes(void*, unsigned char* bytes, size_t length) {
  return psa_generate_random(bytes, length) == PSA_SUCCESS ? 0 : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}
bool nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
std::string environment(const char* key) {
  const char* value = getenv(key);
  return value ? value : "";
}
bool valid_domain(const std::string& domain) {
  if (domain.empty() || domain.size() > 253) return false;
  size_t start = 0;
  while (start < domain.size()) {
    const size_t end = domain.find('.', start);
    const size_t stop = end == std::string::npos ? domain.size() : end;
    const auto alnum = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (stop == start || stop - start > 63 || !alnum(domain[start]) || !alnum(domain[stop - 1])) return false;
    for (size_t i = start; i < stop; ++i)
      if (!alnum(domain[i]) && domain[i] != '-') return false;
    if (stop == domain.size()) return true;
    start = stop + 1;
  }
  return false;
}
// Pin one generation directory before opening any files: an atomic current
// symlink switch can never combine a new certificate with an old private key.
bool read_file(int directory, const char* name, bool secret, std::vector<unsigned char>& bytes) {
  base::FileDescriptor file(openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK));
  const int fd = file.get();
  if (fd < 0) return false;
  struct stat info {};
  const bool allowed = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 && info.st_size <= 65536 &&
                       (!secret || (info.st_mode & 0077) == 0);
  if (!allowed) { return false; }
  bytes.resize(static_cast<size_t>(info.st_size) + 1);
  size_t offset = 0;
  while (offset < bytes.size() - 1) {
    const ssize_t count = read(fd, bytes.data() + offset, bytes.size() - 1 - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { return false; }
    offset += static_cast<size_t>(count);
  }
  bytes.back() = 0;
  return true;
}
Certificate::Certificate() {
  mbedtls_x509_crt_init(&chain);
  mbedtls_pk_init(&key);
}

Certificate::~Certificate() {
  mbedtls_pk_free(&key);
  mbedtls_x509_crt_free(&chain);
}

bool Certificate::Load(int directory, const char* cert_name, const char* key_name, mbedtls_x509_crt& trust,
                       const char* hostname) {
  std::vector<unsigned char> pem;
  if (!read_file(directory, cert_name, false, pem) || mbedtls_x509_crt_parse(&chain, pem.data(), pem.size()) != 0)
    return false;
  if (!read_file(directory, key_name, true, pem)) {
    mbedtls_platform_zeroize(pem.data(), pem.size());
    return false;
  }
  const int result = mbedtls_pk_parse_key(&key, pem.data(), pem.size(), nullptr, 0, random_bytes, nullptr);
  mbedtls_platform_zeroize(pem.data(), pem.size());
  uint32_t flags = 0;
  return result == 0 && mbedtls_pk_check_pair(&chain.pk, &key, random_bytes, nullptr) == 0 &&
         mbedtls_x509_crt_verify(&chain, &trust, nullptr, hostname, &flags, nullptr, nullptr) == 0 && flags == 0 &&
         mbedtls_x509_crt_check_extended_key_usage(&chain, MBEDTLS_OID_SERVER_AUTH,
                                                   MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH)) == 0;
}

TlsConfiguration::TlsConfiguration() { mbedtls_ssl_config_init(&config); }

TlsConfiguration::~TlsConfiguration() { mbedtls_ssl_config_free(&config); }

int TlsConfiguration::Sni(void* opaque, mbedtls_ssl_context* ssl, const unsigned char* name, size_t length) {
  auto& self = *static_cast<TlsConfiguration*>(opaque);
  Certificate* certificate = nullptr;
  // No domain is configured on an address-only node: only the address matches.
  if (!self.domain_name.empty() && length == self.domain_name.size() &&
      strncasecmp(reinterpret_cast<const char*>(name), self.domain_name.c_str(), length) == 0)
    certificate = &self.domain;
  else if (length == self.ip_name.size() && memcmp(name, self.ip_name.data(), length) == 0) certificate = &self.ip;
  return certificate ? mbedtls_ssl_set_hs_own_cert(ssl, &certificate->chain, &certificate->key) : -1;
}

bool TlsConfiguration::Load(const std::string& directory, const std::string& hostname, const std::string& address) {
  domain_name = hostname;
  ip_name = address;
  base::FileDescriptor generation_file(open((directory + "/current").c_str(), O_RDONLY | O_DIRECTORY));
  const int generation = generation_file.get();
  if (generation < 0) return false;
  mbedtls_x509_crt trust;
  mbedtls_x509_crt_init(&trust);
  // Own only the C context internals; this deleter never deletes stack storage.
  std::unique_ptr<mbedtls_x509_crt, decltype(&mbedtls_x509_crt_free)> trust_owner(&trust, mbedtls_x509_crt_free);
  std::string ca = environment("FORTIS_CA_BUNDLE_PATH");
  if (ca.empty()) ca = "/etc/ssl/certs/ca-certificates.crt";
  const bool valid =
      mbedtls_x509_crt_parse_file(&trust, ca.c_str()) == 0 &&
      (hostname.empty() || domain.Load(generation, "domain.pem", "domain.key", trust, hostname.c_str())) &&
      ip.Load(generation, "ip.pem", "ip.key", trust, address.c_str());

  if (!valid || mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                            MBEDTLS_SSL_PRESET_DEFAULT) != 0)
    return false;
  mbedtls_ssl_conf_rng(&config, random_bytes, nullptr);
  mbedtls_ssl_conf_min_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
  // Forward-secret AEAD suites only; retain both RSA and ECDSA certificates.
  static const int suites[] = {MBEDTLS_TLS1_3_AES_128_GCM_SHA256,
                               MBEDTLS_TLS1_3_AES_256_GCM_SHA384,
                               MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,
                               MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
                               MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
                               MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
                               MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
                               MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
                               MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
                               0};
  mbedtls_ssl_conf_ciphersuites(&config, suites);
#if defined(MBEDTLS_SSL_RENEGOTIATION)
  mbedtls_ssl_conf_renegotiation(&config, MBEDTLS_SSL_RENEGOTIATION_DISABLED);
#endif
  mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_NONE);
  mbedtls_ssl_conf_session_tickets(&config, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
  static const char* protocols[] = {"http/1.1", nullptr};
  if (mbedtls_ssl_conf_alpn_protocols(&config, protocols) != 0 ||
      mbedtls_ssl_conf_own_cert(&config, &ip.chain, &ip.key) != 0)
    return false;
  mbedtls_ssl_conf_sni(&config, Sni, this);
  return true;
}

bool CompletionWake::Open() {
  if (reader_.get() >= 0 && writer_.get() >= 0) return true;
  int descriptors[2]{-1, -1};
  if (pipe(descriptors) != 0) return false;
  base::FileDescriptor reader(descriptors[0]);
  base::FileDescriptor writer(descriptors[1]);
  if (!nonblocking(reader.get()) || !nonblocking(writer.get()) || fcntl(reader.get(), F_SETFD, FD_CLOEXEC) != 0 ||
      fcntl(writer.get(), F_SETFD, FD_CLOEXEC) != 0)
    return false;
  reader_ = std::move(reader);
  writer_ = std::move(writer);
  return true;
}

bool CompletionWake::Notify(int fd) noexcept {
  if (fd < 0) return false;
  const unsigned char notification = 1;
  // EINTR/other errors retain the timer fallback; EAGAIN means a wake is queued.
  // The reader outlives every writer by the ingress shutdown contract, so this
  // owned pipe cannot acquire a dangling reader or produce SIGPIPE.
  const ssize_t sent = write(fd, &notification, sizeof(notification));
  return sent == sizeof(notification) || (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
}

void CompletionWake::Drain() const {
  unsigned char notifications[64];
  // Fixed work per reactor turn even while completions arrive continuously.
  for (unsigned batch = 0; batch < 16; ++batch)
    if (read(reader_.get(), notifications, sizeof(notifications)) <= 0) break;
}

Connection::Connection(base::FileDescriptor socket, std::string address, std::shared_ptr<TlsConfiguration> tls,
                       int completion)
    : descriptor(std::move(socket)),
      fd(descriptor.get()),
      completion_fd(completion),
      peer(std::move(address)),
      configuration(std::move(tls)),
      encrypted(configuration != nullptr) {
  mbedtls_ssl_init(&ssl);
}

Connection::~Connection() {
  if (registered) http::RegisterHttpTransport(fd, nullptr);
  if (encrypted && written && mbedtls_ssl_is_handshake_over(&ssl)) (void)mbedtls_ssl_close_notify(&ssl);
  mbedtls_ssl_free(&ssl);
}

int Connection::Send(void* opaque, const unsigned char* bytes, size_t length) {
  const auto& self = *static_cast<Connection*>(opaque);
  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags = MSG_NOSIGNAL;
#endif
  const ssize_t count = send(self.fd, bytes, length, flags);
  if (count >= 0) return static_cast<int>(count);
  return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_WRITE
                                                                   : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

int Connection::Receive(void* opaque, unsigned char* bytes, size_t length) {
  const auto& self = *static_cast<Connection*>(opaque);
  const ssize_t count = recv(self.fd, bytes, length, 0);
  if (count >= 0) return static_cast<int>(count);
  return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_READ
                                                                   : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

bool Connection::Start() {
  if (registered || !nonblocking(fd) || !http::RegisterHttpTransport(fd, this)) return false;
  registered = true;
  if (!encrypted) {
    phase = Phase::Reading;
    deadline = now_ms() + kRequestMs;
    return true;
  }
  return true;
}

bool Connection::BeginHandshake() {
  if (mbedtls_ssl_setup(&ssl, &configuration->config) != 0) return false;
  mbedtls_ssl_set_bio(&ssl, this, Send, Receive, nullptr);
  phase = Phase::Handshake;
  return true;
}

ssize_t Connection::Read(char* bytes, size_t length) {
  const size_t count = std::min(length, frame_size - cursor);
  if (count) memcpy(bytes, input.data() + cursor, count);
  cursor += count;
  return static_cast<ssize_t>(count);
}

bool Connection::Write(const char* bytes, size_t length, const timespec& limit) {
  const uint64_t end = static_cast<uint64_t>(limit.tv_sec) * 1000 + static_cast<uint64_t>(limit.tv_nsec) / 1000000;
  while (length && !ShutdownRequested()) {
    const uint64_t now = now_ms();
    if (now >= end) return false;
    const size_t portion = std::min(length, static_cast<size_t>(16384));
    const int result = encrypted ? mbedtls_ssl_write(&ssl, reinterpret_cast<const unsigned char*>(bytes), portion)
                                 : Send(this, reinterpret_cast<const unsigned char*>(bytes), portion);
    if (result > 0) {
      bytes += result;
      length -= static_cast<size_t>(result);
      continue;
    }
    if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) return false;
    if (reactor_response) return false;  // No slow reader may pin ingress/health.
    pollfd socket{fd, static_cast<short>(result == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT), 0};
    const int ready = poll(&socket, 1, static_cast<int>(std::min(end - now, uint64_t{100})));
    if (ready < 0 && errno != EINTR) return false;
    if (socket.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
  }
  return length == 0;
}

bool Connection::KeepAlive() const { return keep_alive; }

void Connection::Written(bool success) { written = success; }

void Connection::DisallowReuse() { keep_alive = false; }

void Connection::Finish() {
  const int notification = completion_fd;
  // The reactor may reset/destroy this connection as soon as Finished is
  // published. Only the copied ingress-owned descriptor is used afterwards.
  phase.store(Phase::Finished, std::memory_order_release);
  (void)CompletionWake::Notify(notification);
}

bool Connection::HasBufferedInput() const {
  if (frame_size && input.size() >= frame_size) return true;
  if (!head_size &&
      std::search(input.begin(), input.end(), std::begin("\r\n\r\n"), std::end("\r\n\r\n") - 1) != input.end())
    return true;
  return encrypted && mbedtls_ssl_get_bytes_avail(&ssl) > 0;
}

void Connection::Reset(uint64_t now) {
  // Release upload capacity between requests; otherwise sequential uploads
  // could leave every idle connection retaining a maximum-sized allocation.
  std::vector<char> remaining(input.begin() + static_cast<ptrdiff_t>(frame_size), input.end());
  input.swap(remaining);
  http::RequestParser::Release(&parsed.get());
  frame_size = head_size = cursor = 0;
  keep_alive = written = upload = false;
  reactor_response = true;
  idle = input.empty();
  deadline = now + (idle ? kIdleMs : kRequestMs);
  events = POLLIN;
  phase = Phase::Reading;
}

// Reject alternate spellings before route selection. Decoding once and rejecting
// dots, repeated slashes and residual escapes makes probe exclusion unambiguous
// without changing route parameter decoding or signed request targets.
bool public_path(const char* path) {
  std::string decoded;
  for (size_t i = 0; path[i]; ++i) {
    unsigned char c = static_cast<unsigned char>(path[i]);
    if (c == '%') {
      const auto hex = [](unsigned char digit) -> int {
        if (digit >= '0' && digit <= '9') return digit - '0';
        if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
        if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
        return -1;
      };
      if (!path[i + 1] || !path[i + 2]) return false;
      const int high = hex(static_cast<unsigned char>(path[i + 1]));
      const int low = hex(static_cast<unsigned char>(path[i + 2]));
      if (high < 0 || low < 0) return false;
      c = static_cast<unsigned char>((high << 4) | low);
      i += 2;
    }
    if (c < 0x20 || c == 0x7f || c == '%' || c == '\\' || c == '?' || c == '#') return false;
    decoded += static_cast<char>(c);
  }
  if (decoded.find("//") != std::string::npos) return false;
  size_t start = 0;
  while (start < decoded.size()) {
    const size_t end = decoded.find('/', start);
    const std::string segment = decoded.substr(start, end == std::string::npos ? end : end - start);
    if (segment == "." || segment == "..") return false;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return decoded != "/probe" && decoded.compare(0, 7, "/probe/") != 0;
}
Ingress::~Ingress() {
  base::LogInfo("https", "Ingress stopped accepted=%llu rejected=%llu handshake_failed=%llu expired=%llu refused=%llu",
                static_cast<unsigned long long>(accepted), static_cast<unsigned long long>(rejected),
                static_cast<unsigned long long>(handshake_failures), static_cast<unsigned long long>(expired),
                static_cast<unsigned long long>(protocol_refusals));
}

bool Ingress::Reload() {
  size_t free_slot = generations.size();
  for (size_t i = 0; i < generations.size(); ++i)
    if (generations[i].expired()) {
      free_slot = i;
      break;
    }
  if (free_slot == generations.size()) return false;
  auto next = std::make_shared<TlsConfiguration>();
  if (!next->Load(directory, domain, ip)) return false;
  configuration = std::move(next);
  generations[free_slot] = configuration;
  return true;
}

bool Ingress::RateAllowed(const std::string& peer, uint64_t now) {
  Rate* candidate = nullptr;
  for (auto& rate : rates) {
    if (rate.peer == peer) {
      candidate = &rate;
      break;
    }
    if (!candidate && (rate.peer.empty() || (now > rate.updated && now - rate.updated > 60000))) candidate = &rate;
  }
  if (!candidate) return false;
  if (candidate->peer != peer) {
    candidate->peer = peer;
    candidate->tokens = 400;
    candidate->updated = now;
  }
  candidate->tokens = std::min(
      400.0, candidate->tokens + static_cast<double>(now >= candidate->updated ? now - candidate->updated : 0) / 10.0);
  candidate->updated = std::max(candidate->updated, now);
  if (candidate->tokens < 1) return false;
  --candidate->tokens;
  return true;
}

void Ingress::Answer(Connection& connection, int status, const char* reason, const char* body) {
  ++protocol_refusals;
  connection.keep_alive = false;
  connection.phase = Phase::Dispatched;
  http::HttpResponse response = http::JsonResponse(status, reason, body);
  response.write_timeout_ms = 100;
  timespec started{};
  clock_gettime(CLOCK_MONOTONIC, &started);
  http::ResponseWriter::Write(connection.fd, response, false, started);
  connection.Finish();
}

bool Ingress::HostAllowed(const char* host) const {
  // DNS authorities are case-insensitive; a port is a nonzero 16-bit number.
  const std::string value(host);
  const size_t colon = value.find(':');
  std::string name = value.substr(0, colon);
  for (char& c : name)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  if ((domain.empty() || name != domain) && name != ip) return false;
  if (colon == std::string::npos) return true;
  const std::string port = value.substr(colon + 1);
  if (port.empty() || port.size() > 5) return false;
  unsigned number = 0;
  for (unsigned char c : port) {
    if (c < '0' || c > '9') return false;
    number = number * 10 + static_cast<unsigned>(c - '0');
  }
  return number > 0 && number <= 65535;
}
bool Ingress::AdmitHead(Connection& connection) {
  auto& request = connection.parsed.get();
  const auto result = http::RequestParser::Head(connection.input.data(), connection.head_size, &request);
  if (result != http::HttpReadResult::Ok) {
    Answer(connection, result == http::HttpReadResult::BodyTooLarge ? 413 : 400, "Bad Request");
    return false;
  }
  if (!HostAllowed(request.host)) {
    Answer(connection, 421, "Misdirected Request");
    return false;
  }
  if (!public_path(request.path)) {
    Answer(connection, 404, "Not Found");
    return false;
  }
  if (request.expect_continue) {
    Answer(connection, 417, "Expectation Failed");
    return false;
  }
  const bool read = strcmp(request.method, "GET") == 0 || strcmp(request.method, "HEAD") == 0 ||
                    strcmp(request.method, "OPTIONS") == 0;
  if (read && request.body_length) {
    Answer(connection, 405, "Method Not Allowed");
    return false;
  }
  if (strcmp(request.path, "/healthz") != 0 && !RateAllowed(connection.peer, now_ms())) {
    Answer(connection, 429, "Too Many Requests");
    return false;
  }
  if (request.body_length) {
    size_t total = 0, client = 0;
    for (const auto& other : connections) {
      if (!other || !other->upload) continue;
      ++total;
      if (other->peer == connection.peer) ++client;
    }
    if (total >= kUploads || client >= kPerClientUploads) {
      Answer(connection, 503, "Service Unavailable");
      return false;
    }
    connection.upload = true;
  }
  connection.frame_size = connection.head_size + request.body_length;
  connection.keep_alive = connection.encrypted && !request.connection_close && ++connection.requests < kMaxRequests;
  return true;
}

void Ingress::Redirect(Connection& connection) {
  auto& request = connection.parsed.get();
  static constexpr char prefix[] = "/.well-known/acme-challenge/";
  http::HttpResponse response = http::JsonResponse(404, "Not Found", "");
  std::array<uint8_t, 4096> body{};
  if (strncmp(request.path, prefix, sizeof(prefix) - 1) == 0) {
    const bool readable = strcmp(request.method, "GET") == 0 || strcmp(request.method, "HEAD") == 0;
    if (!readable) {
      response.status = 405;
      response.reason = "Method Not Allowed";
      response.extra_headers = "Allow: GET, HEAD\r\n";
    }
    const std::string token(request.path + sizeof(prefix) - 1);
    const bool safe =
        readable && !token.empty() && token.size() <= 128 &&
        std::all_of(token.begin(), token.end(), [](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        });
    if (safe && challenges.get() >= 0) {
      base::FileDescriptor challenge(openat(challenges.get(), token.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK));
      const int fd = challenge.get();
      struct stat info {};
      if (fd >= 0 && fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0 &&
          info.st_size <= static_cast<off_t>(body.size())) {
        const ssize_t length = read(fd, body.data(), body.size());
        if (length == info.st_size) {
          response.status = 200;
          response.reason = "OK";
          response.body = body.data();
          response.body_length = static_cast<size_t>(length);
          response.content_type = "text/plain";
        }
      }
    }
  } else {
    response.status = 308;
    response.reason = "Permanent Redirect";
    const std::string destination = "https://" + (domain.empty() ? ip : domain) + request.raw_target;
    if (destination.size() < sizeof(response.location))
      memcpy(response.location, destination.c_str(), destination.size() + 1);
    else {
      response.status = 414;
      response.reason = "URI Too Long";
    }
  }
  connection.keep_alive = false;
  timespec started{};
  clock_gettime(CLOCK_MONOTONIC, &started);
  response.write_timeout_ms = 100;
  http::ResponseWriter::Write(connection.fd, response, strcmp(request.method, "HEAD") == 0, started);
  connection.Finish();
}

void Ingress::Dispatch(Connection& connection) {
  connection.phase = Phase::Dispatched;
  if (!connection.encrypted) {
    Redirect(connection);
    return;
  }
  if (strcmp(connection.parsed.get().path, "/healthz") == 0) {
    if (!health->Dispatch(connection.fd, connection.peer.c_str())) connection.Finish();
    return;
  }
  size_t application_requests = 0;
  for (const auto& other : connections)
    if (other && other->phase == Phase::Dispatched && !other->reactor_response) ++application_requests;
  base::LockGuard lock(pool->mutex);
  if (application_requests >= kApplicationRequests || pool->count == kPendingConnectionCapacity || pool->stopping) {
    Answer(connection, 503, "Service Unavailable");
    return;
  }
  connection.reactor_response = false;
  auto& pending = pool->pending[(pool->head + pool->count) % kPendingConnectionCapacity];
  pending.client = connection.fd;
  if (connection.peer.size() >= sizeof(pending.remote_ip)) {
    Answer(connection, 503, "Service Unavailable");
    return;
  }
  memcpy(pending.remote_ip, connection.peer.c_str(), connection.peer.size() + 1);
  clock_gettime(CLOCK_MONOTONIC, &pending.accepted);
  ++pool->count;
  pool->available.Signal();
}

void Ingress::Advance(Connection& connection) {
  if (connection.phase == Phase::Handshake) {
    const int result = mbedtls_ssl_handshake(&connection.ssl);
    if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) {
      connection.events = result == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT;
      return;
    }
    if (result != 0) {
      ++handshake_failures;
      connection.keep_alive = false;
      connection.Finish();
      return;
    }
    connection.phase = Phase::Reading;
    connection.deadline = now_ms() + kRequestMs;
    connection.events = POLLIN;
  }
  // Per-turn byte budget preserves fairness even for hot pipelined clients.
  for (unsigned step = 0; step < 4; ++step) {
    if (!connection.head_size) {
      const auto found = std::search(connection.input.begin(), connection.input.end(), std::begin("\r\n\r\n"),
                                     std::end("\r\n\r\n") - 1);
      if (found != connection.input.end()) {
        connection.head_size = static_cast<size_t>(found - connection.input.begin()) + 4;
        if (!AdmitHead(connection)) return;
      } else if (connection.input.size() >= http::kHttpMaxHeaderBytes) {
        Answer(connection, 431, "Request Header Fields Too Large");
        return;
      }
    }
    if (connection.frame_size && connection.input.size() >= connection.frame_size) {
      Dispatch(connection);
      return;
    }
    const size_t capacity = connection.frame_size ? connection.frame_size - connection.input.size()
                                                  : http::kHttpMaxHeaderBytes - connection.input.size();
    unsigned char bytes[4096];
    const size_t wanted = std::min(capacity, sizeof(bytes));
    const int count = connection.encrypted ? mbedtls_ssl_read(&connection.ssl, bytes, wanted)
                                           : Connection::Receive(&connection, bytes, wanted);
    if (count == MBEDTLS_ERR_SSL_WANT_READ || count == MBEDTLS_ERR_SSL_WANT_WRITE) {
      connection.events = count == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT;
      return;
    }
    if (count <= 0) {
      connection.keep_alive = false;
      connection.Finish();
      return;
    }
    if (connection.idle) {
      connection.idle = false;
      connection.deadline = now_ms() + kRequestMs;
    }
    connection.input.insert(connection.input.end(), reinterpret_cast<char*>(bytes),
                            reinterpret_cast<char*>(bytes) + count);
  }
}

void Ingress::Accept(int server, bool encrypted) {
  // Bounded accept batch; no TLS work on the accept path.
  for (unsigned batch = 0; batch < 64; ++batch) {
    sockaddr_in peer{};
    socklen_t length = sizeof(peer);
    base::FileDescriptor accepted_socket(accept(server, reinterpret_cast<sockaddr*>(&peer), &length));
    const int fd = accepted_socket.get();
    if (fd < 0) return;
    const int no_delay = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay)) != 0) { continue; }
    char address[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address));
    size_t count = 0, free_slot = connections.size();
    for (size_t i = 0; i < connections.size(); ++i) {
      if (!connections[i]) {
        free_slot = i;
        continue;
      }
      if (connections[i]->peer == address) ++count;
    }
    if (free_slot == connections.size() || count >= kPerClientConnections) {
      ++rejected;
      continue;
    }
    auto connection = std::make_unique<Connection>(std::move(accepted_socket), address,
                                                   encrypted ? configuration : nullptr, completions.WriteFd());
    if (connection->Start()) {
      ++accepted;
      connections[free_slot] = std::move(connection);
    } else ++rejected;
  }
}

void Ingress::ScheduleHandshakes(uint64_t now) {
  size_t active = 0;
  for (const auto& connection : connections)
    if (connection && connection->phase == Phase::Handshake) ++active;
  // Admit oldest first. Queued sockets have no TLS buffers yet and share
  // the original absolute handshake deadline, including time in this queue.
  while (active < kHandshakes) {
    Connection* oldest = nullptr;
    for (const auto& connection : connections) {
      if (!connection || connection->phase != Phase::QueuedHandshake || connection->deadline <= now) continue;
      if (!oldest || connection->deadline < oldest->deadline) oldest = connection.get();
    }
    if (!oldest) break;
    if (!oldest->BeginHandshake()) {
      ++handshake_failures;
      oldest->Finish();
    } else ++active;
  }
}

void Ingress::Reap(uint64_t now) {
  for (auto& connection : connections) {
    if (!connection) continue;
    if (connection->phase.load(std::memory_order_acquire) == Phase::Finished) {
      if (connection->keep_alive && connection->written) connection->Reset(now);
      else {
        connection.reset();
        continue;
      }
    }
    // Dispatched requests belong to workers/handoff, whose write deadlines
    // govern them. Ingress must not destroy a borrowed transport.
    if (connection->phase != Phase::Dispatched && now >= connection->deadline) {
      ++expired;
      connection.reset();
    }
  }
}

bool Ingress::Run() {
  if (!completions.Open()) return false;
  while (!ShutdownRequested()) {
    if (TakeCertificateReload()) {
      if (Reload()) base::LogInfo("https", "Certificate generation activated");
      else base::LogError("https", "Certificate reload refused; retaining active generation");
    }
    const uint64_t now = now_ms();
    Reap(now);
    ScheduleHandshakes(now);
    // Spell out the full capacity: Cppcheck 2.13 loses the +3 in a
    // std::array template argument. Keep the real bound compiler-checked.
    static_assert(kConnections + 3 == 259);
    std::array<pollfd, 259> sockets{};
    sockets[0] = {listener, POLLIN, 0};
    sockets[1] = {redirect.get(), POLLIN, 0};
    sockets[2] = {completions.ReadFd(), POLLIN, 0};
    if (now >= next_summary) {
      base::LogInfo("https",
                    "Ingress totals accepted=%llu rejected=%llu handshake_failed=%llu expired=%llu refused=%llu",
                    static_cast<unsigned long long>(accepted), static_cast<unsigned long long>(rejected),
                    static_cast<unsigned long long>(handshake_failures), static_cast<unsigned long long>(expired),
                    static_cast<unsigned long long>(protocol_refusals));
      next_summary = now + 60000;
    }
    bool buffered_input = false;
    for (size_t i = 0; i < connections.size(); ++i) {
      auto& connection = connections[i];
      sockets[i + 3].fd = -1;
      if (!connection) continue;
      if (connection->phase == Phase::Dispatched) continue;
      if (connection->phase != Phase::QueuedHandshake) {
        sockets[i + 3] = {connection->fd, connection->events, 0};
        buffered_input = buffered_input || (connection->phase == Phase::Reading && connection->HasBufferedInput());
      }
    }
    const int ready = poll(sockets.data(), sockets.size(), buffered_input ? 0 : 10);
    if (ready < 0 && errno != EINTR) return false;
    if (sockets[0].revents & (POLLERR | POLLNVAL)) return false;
    if (sockets[2].revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
    if (sockets[2].revents & POLLIN) {
      completions.Drain();
      // Rebuild poll mappings after reaping: old events may refer to a closed
      // slot that Accept could reuse, and retained/pipelined bytes need another
      // reactor turn rather than an artificial timeout wait.
      continue;
    }
    if (sockets[0].revents & POLLIN) Accept(listener, true);
    if (sockets[1].revents & POLLIN) Accept(redirect.get(), false);
    for (size_t i = 0; i < connections.size(); ++i) {
      auto& connection = connections[i];
      if (!connection || connection->phase == Phase::QueuedHandshake || connection->phase == Phase::Dispatched ||
          connection->phase == Phase::Finished)
        continue;
      // Try pending decrypted/retained bytes even without a socket event.
      if (sockets[i + 3].revents || connection->HasBufferedInput()) Advance(*connection);
    }
  }
  for (auto& connection : connections)
    if (connection) shutdown(connection->fd, SHUT_RDWR);
  return true;
}

}  // namespace https_detail

HttpsListener::HttpsListener() : impl_(std::make_unique<https_detail::Ingress>()) {}
HttpsListener::~HttpsListener() = default;
bool HttpsListener::Enabled() { return !https_detail::environment("FORTIS_HTTPS_CERT_DIR").empty(); }
bool HttpsListener::Start(int listener, HttpWorkerPool& pool, http::HttpRouter& health) {
  if (impl_->started) return false;
  impl_->started = true;
  impl_->listener = listener;
  impl_->pool = &pool;
  impl_->health = &health;
  impl_->directory = https_detail::environment("FORTIS_HTTPS_CERT_DIR");
  impl_->domain = https_detail::environment("FORTIS_HTTPS_DOMAIN");
  impl_->ip = https_detail::environment("FORTIS_HTTPS_IP");
  in_addr address{};
  if (impl_->directory.empty() || impl_->directory[0] != '/' ||
      (!impl_->domain.empty() && !https_detail::valid_domain(impl_->domain)) ||
      inet_pton(AF_INET, impl_->ip.c_str(), &address) != 1 || !https_detail::nonblocking(listener) ||
      !impl_->Reload() || !impl_->completions.Open())
    return false;
  const std::string port_text = https_detail::environment("FORTIS_HTTP_REDIRECT_PORT");
  unsigned port = 8082;
  if (!port_text.empty()) {
    if (port_text.size() > 5 ||
        !std::all_of(port_text.begin(), port_text.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
      return false;
    port = static_cast<unsigned>(strtoul(port_text.c_str(), nullptr, 10));
  }
  if (!port || port > 65535) return false;
  impl_->redirect.reset(ListenOnPort(port, 32));
  if (impl_->redirect.get() < 0 || !https_detail::nonblocking(impl_->redirect.get())) return false;
  const std::string challenges = https_detail::environment("FORTIS_ACME_CHALLENGE_DIR");
  if (!challenges.empty()) {
    impl_->challenges.reset(open(challenges.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
    if (impl_->challenges.get() < 0) return false;
  }
  impl_->ready = true;
  base::LogInfo("https", "Native HTTP/1.1 TLS ingress enabled; connections=256 handshakes=32 uploads=32");
  return true;
}
bool HttpsListener::Run() {
  if (!impl_->ready) return false;
  try {
    return impl_->Run();
  } catch (const std::bad_alloc&) {
    base::LogError("https", "Ingress allocation failed");
    return false;
  }
}
}  // namespace fortis::app
