#pragma once
// Private ingress implementation. Kept separate from the public lifetime API
// so deterministic unit tests can exercise the production state transitions.
#include "https_listener.h"
#include "file_descriptor.h"
#include "http_session.h"
#include "http_transport.h"
#include "log.h"
#include "shutdown_signal.h"

#include <mbedtls/ssl.h>
#include <mbedtls/ssl_ciphersuites.h>
#include <mbedtls/oid.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <psa/crypto.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace fortis::app::https_detail {
constexpr size_t kConnections = 256;
constexpr size_t kHandshakes = 32;
constexpr size_t kApplicationRequests = 224;
constexpr size_t kUploads = 32;
constexpr size_t kPerClientConnections = 224;
constexpr size_t kPerClientUploads = 8;
constexpr uint64_t kHandshakeMs = 3000;
constexpr uint64_t kRequestMs = 5000;
constexpr uint64_t kIdleMs = 15000;
constexpr unsigned kMaxRequests = 100;
constexpr size_t kMaxCertificateGenerations = 4;

uint64_t now_ms();
int random_bytes(void*, unsigned char* bytes, size_t length);
bool nonblocking(int fd);
std::string environment(const char* key);
bool valid_domain(const std::string& domain);
bool read_file(int directory, const char* name, bool secret, std::vector<unsigned char>& bytes);
bool public_path(const char* path);

struct Certificate {
  mbedtls_x509_crt chain{};
  mbedtls_pk_context key{};
  Certificate();
  ~Certificate();
  Certificate(const Certificate&) = delete;
  Certificate& operator=(const Certificate&) = delete;
  bool Load(int directory, const char* cert_name, const char* key_name, mbedtls_x509_crt& trust, const char* hostname);
};

struct TlsConfiguration {
  Certificate domain;
  Certificate ip;
  mbedtls_ssl_config config{};
  std::string domain_name;
  std::string ip_name;
  TlsConfiguration();
  ~TlsConfiguration();
  TlsConfiguration(const TlsConfiguration&) = delete;
  TlsConfiguration& operator=(const TlsConfiguration&) = delete;
  static int Sni(void* opaque, mbedtls_ssl_context* ssl, const unsigned char* name, size_t length);
  bool Load(const std::string& directory, const std::string& hostname, const std::string& address);
};

// One bounded nonblocking self-pipe per ingress. Read/Drain are reactor-only;
// Notify may be called by concurrent response writers and never blocks. A full
// pipe already represents a pending wake. The 10 ms poll timeout remains a
// fallback for interrupted/failed notifications, timers and shutdown signals.
// Keep this owner alive until every connection user has stopped/joined.
class CompletionWake final {
 public:
  bool Open();
  int ReadFd() const { return reader_.get(); }
  int WriteFd() const { return writer_.get(); }
  // True means delivered or already queued; false retains timer fallback.
  static bool Notify(int fd) noexcept;
  void Drain() const;

 private:
  base::FileDescriptor reader_;
  base::FileDescriptor writer_;
};

enum class Phase { QueuedHandshake, Handshake, Reading, Dispatched, Finished };
struct Connection final : http::HttpTransport {
  base::FileDescriptor descriptor;
  const int fd;  // Borrowed registry key; descriptor owns the stable socket.
  // Borrowed from Ingress until all workers/handoffs join. Finish copies this
  // before publishing Finished and never touches *this after that publication.
  const int completion_fd;
  std::string peer;
  std::shared_ptr<TlsConfiguration> configuration;
  mbedtls_ssl_context ssl{};
  std::atomic<Phase> phase{Phase::QueuedHandshake};
  short events = POLLIN;
  uint64_t deadline = now_ms() + kHandshakeMs;
  unsigned requests = 0;
  bool upload = false;
  bool keep_alive = false;
  bool written = false;
  bool idle = false;
  bool encrypted;
  bool registered = false;
  bool reactor_response = true;
  size_t head_size = 0;
  size_t frame_size = 0;
  size_t cursor = 0;
  std::vector<char> input;
  http::ParsedRequest parsed;
  Connection(base::FileDescriptor socket, std::string address, std::shared_ptr<TlsConfiguration> tls,
             int completion = -1);
  ~Connection() override;
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  static int Send(void* opaque, const unsigned char* bytes, size_t length);
  static int Receive(void* opaque, unsigned char* bytes, size_t length);
  bool Start();
  bool BeginHandshake();
  ssize_t Read(char* bytes, size_t length) override;
  bool Write(const char* bytes, size_t length, const timespec& limit) override;
  bool KeepAlive() const override;
  void Written(bool success) override;
  void DisallowReuse() override;
  void Finish() override;
  void Reset(uint64_t now);
  bool HasBufferedInput() const;  // Reactor-only; complete framing or decrypted bytes.
};

struct Ingress {
  bool started = false;
  bool ready = false;
  int listener = -1;  // Borrowed from the application through shutdown.
  base::FileDescriptor redirect;
  base::FileDescriptor challenges;
  CompletionWake completions;  // Declared before connections: destroyed afterwards.
  HttpWorkerPool* pool = nullptr;
  http::HttpRouter* health = nullptr;
  std::string directory;
  std::string domain;
  std::string ip;
  std::shared_ptr<TlsConfiguration> configuration;
  std::array<std::weak_ptr<TlsConfiguration>, kMaxCertificateGenerations> generations;
  std::array<std::unique_ptr<Connection>, kConnections> connections;
  struct Rate {
    std::string peer;
    double tokens = 400;
    uint64_t updated = 0;
  };
  std::array<Rate, 512> rates;
  uint64_t accepted = 0;
  uint64_t rejected = 0;
  uint64_t handshake_failures = 0;
  uint64_t expired = 0;
  uint64_t protocol_refusals = 0;
  uint64_t next_summary = 0;
  ~Ingress();
  bool Reload();
  bool RateAllowed(const std::string& peer, uint64_t now);
  void Answer(Connection& connection, int status, const char* reason, const char* body = "");
  bool HostAllowed(const char* host) const;
  bool AdmitHead(Connection& connection);
  void Redirect(Connection& connection);
  void Dispatch(Connection& connection);
  void Advance(Connection& connection);
  void Accept(int server, bool encrypted);
  void ScheduleHandshakes(uint64_t now);
  void Reap(uint64_t now);
  bool Run();
};

}  // namespace fortis::app::https_detail
