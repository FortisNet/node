#include "http_request.h"

#include <cassert>
#include <cerrno>
#include <new>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

namespace {
// Only this thread's nothrow arrays are affected. Exercise the real production
// allocator without adding a runtime switch or replacing parser logic.
thread_local int allocations_before_failure = -1;
}

void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
  if (allocations_before_failure == 0) return nullptr;
  if (allocations_before_failure > 0) --allocations_before_failure;
  try { return ::operator new[](bytes); }
  catch (...) { return nullptr; }
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
  ::operator delete[](pointer);
}

namespace {
using namespace fortis::http;

struct Sockets {
  int ends[2]{-1, -1};
  Sockets() { assert(socketpair(AF_UNIX, SOCK_STREAM, 0, ends) == 0); }
  ~Sockets() { for (const int fd : ends) if (fd >= 0) close(fd); }
  Sockets(const Sockets&) = delete;
  Sockets& operator=(const Sockets&) = delete;
};

HttpReadResult Parse(const std::string& wire, int fail_after = -1, bool slow = false) {
  Sockets sockets;
  std::thread writer([&] {
    std::size_t sent = 0;
    while (sent < wire.size()) {
      const auto count = send(sockets.ends[1], wire.data() + sent, wire.size() - sent, MSG_NOSIGNAL);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      sent += static_cast<std::size_t>(count);
    }
    if (!slow) shutdown(sockets.ends[1], SHUT_WR);
  });
  ParsedRequest request;
  allocations_before_failure = fail_after;
  const auto result = RequestParser::Read(sockets.ends[0], &request.get());
  allocations_before_failure = -1;
  if (result == HttpReadResult::Ok) {
    assert(request.get().body_length <= kHttpMaxBodyBytes);
    assert(request.get().raw_target && request.get().path);
  }
  shutdown(sockets.ends[0], SHUT_RDWR);
  writer.join();
  return result;
}

std::string Body(std::size_t length) {
  return "POST /body HTTP/1.1\r\nHost: test\r\nContent-Length: " +
      std::to_string(length) + "\r\n\r\n" + std::string(length, 'x');
}

void BodyLimits() {
  for (const auto length : {kHttpMaxBodyBytes - 1, kHttpMaxBodyBytes})
    assert(Parse(Body(length)) == HttpReadResult::Ok);
  assert(Parse(Body(kHttpMaxBodyBytes + 1)) == HttpReadResult::BodyTooLarge);
  assert(Parse("POST / HTTP/1.1\r\nHost: test\r\nContent-Length: 999999999999999999999\r\n\r\n") ==
         HttpReadResult::BodyTooLarge);
}

void HeaderLimits() {
  const std::string prefix = "GET / HTTP/1.1\r\nHost: test\r\nX-Padding: ";
  constexpr auto suffix = "\r\n\r\n";
  for (const auto length : {kHttpMaxHeaderBytes - 1, kHttpMaxHeaderBytes}) {
    const auto wire = prefix + std::string(length - prefix.size() - 4, 'x') + suffix;
    assert(wire.size() == length && Parse(wire) == HttpReadResult::Ok);
  }
  assert(Parse(prefix + std::string(kHttpMaxHeaderBytes, 'x') + suffix) == HttpReadResult::HeaderTooLarge);
}

void FramingAndAllocation() {
  constexpr auto valid = "GET /target HTTP/1.1\r\nHost: test\r\n\r\n";
  assert(Parse(valid, 0) == HttpReadResult::Unavailable);  // Header storage.
  assert(Parse(valid, 1) == HttpReadResult::Unavailable);  // Original target.
  assert(Parse(Body(kHttpMaxBodyBytes), 2) == HttpReadResult::Unavailable);
  assert(Parse(valid) == HttpReadResult::Ok);  // Subsequent allocation succeeds.
  assert(Parse("GET / HTTP/1.1\r\nHost: test\r\nContent-Encoding: identity\r\n\r\n") == HttpReadResult::Ok);
  assert(Parse("POST / HTTP/1.1\r\nHost: test\r\nContent-Encoding: gzip\r\nContent-Length: 524288\r\n\r\n") ==
         HttpReadResult::UnsupportedEncoding);
  assert(Parse("GET / HTTP/1.1\r\nHost: test\r\nContent-Encoding: identity\r\nContent-Encoding: identity\r\n\r\n") ==
         HttpReadResult::BadRequest);
  assert(Parse("POST / HTTP/1.1\r\nHost: test\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx") ==
         HttpReadResult::BadRequest);
  assert(Parse("POST / HTTP/1.1\r\nHost: test\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\nx") ==
         HttpReadResult::BadRequest);
  assert(Parse("GET / HTTP/1.1\r\nHost: test\r\n", -1, true) == HttpReadResult::Timeout);
  assert(Parse("POST / HTTP/1.1\r\nHost: test\r\nContent-Length: 2\r\n\r\nx", -1, true) ==
         HttpReadResult::Timeout);
}
}  // namespace

int main() try {
  BodyLimits();
  HeaderLimits();
  FramingAndAllocation();
} catch (...) {
  return 1;
}
