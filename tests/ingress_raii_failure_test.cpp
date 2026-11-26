#include "https_test_support.h"
#include "outbound_https.h"
#include <cstdlib>
#include <new>

// Isolated executable-only fault injection. No allocator hook enters the node.
namespace {
thread_local int allocations_until_failure = -1;
}
[[gnu::noinline]] void* operator new(std::size_t size) {
  if (allocations_until_failure == 0) throw std::bad_alloc();
  if (allocations_until_failure > 0) --allocations_until_failure;
  if (void* value = std::malloc(size ? size : 1)) return value;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace {
using fortis::testing::Require;
using namespace fortis::app::https_detail;
using namespace fortis::testing::https;
size_t Descriptors() {
  size_t result = 0;
  for (int fd = 0; fd < 4096; ++fd)
    if (fcntl(fd, F_GETFD) >= 0) ++result;
  return result;
}
struct Inject {
  explicit Inject(int index) { allocations_until_failure = index; }
  ~Inject() { allocations_until_failure = -1; }
};
void ReadFileUnwind() {
  fortis::testing::TemporaryDirectory temporary;
  const auto path = fs::path(temporary.path()) / "data";
  {
    std::ofstream out(path);
    out << std::string(65536, 'x');
  }
  fortis::base::FileDescriptor directory(open(temporary.path(), O_RDONLY | O_DIRECTORY));
  std::vector<unsigned char> bytes;
  const size_t before = Descriptors();
  bool threw = false;
  {
    Inject fail(0);
    try {
      (void)read_file(directory.get(), "data", false, bytes);
    } catch (const std::bad_alloc&) { threw = true; }
  }
  Require(threw && Descriptors() == before, "vector allocation after open unwinds descriptor exactly once");
}
void AcceptedSocketUnwind() {
  fortis::base::FileDescriptor listener(fortis::app::ListenOnPort(0, 8));
  Require(listener.get() >= 0 && nonblocking(listener.get()), "listener");
  sockaddr_in address{};
  socklen_t size = sizeof(address);
  Require(getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &size) == 0, "listener address");
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  fortis::base::FileDescriptor client(socket(AF_INET, SOCK_STREAM, 0));
  Require(connect(client.get(), reinterpret_cast<sockaddr*>(&address), size) == 0, "connect");
  pollfd ready{listener.get(), POLLIN, 0};
  Require(poll(&ready, 1, 1000) == 1, "connection ready");
  Ingress ingress;
  const size_t before = Descriptors();
  bool threw = false;
  {
    Inject fail(0);
    try {
      ingress.Accept(listener.get(), false);
    } catch (const std::bad_alloc&) { threw = true; }
  }
  Require(threw && Descriptors() == before, "failed connection allocation closes the accepted socket");
}
void CertificateLoadUnwind() {
  Certificates certificates;
  const std::string directory = certificates.root.string();
  unsigned throws = 0;
  for (int index = 0; index < 8; ++index) {
    TlsConfiguration candidate;
    const size_t before = Descriptors();
    {
      Inject fail(index);
      try {
        (void)candidate.Load(directory, "localhost", "127.0.0.1");
      } catch (const std::bad_alloc&) { ++throws; }
    }
    Require(Descriptors() == before, "certificate/trust load allocations unwind every descriptor");
  }
  Require(throws > 2, "multiple allocation boundaries actually exercised");
}
}  // namespace
int main() {
  if (!fortis::http::OutboundHttpsInitialize()) return 1;
  fortis::testing::UnitSuite suite;
  suite.Run("file allocation failure", ReadFileUnwind);
  suite.Run("accepted socket allocation failure", AcceptedSocketUnwind);
  suite.Run("certificate/trust allocation failures", CertificateLoadUnwind);
  return suite.Finish("ingress_raii_failure_test");
}
