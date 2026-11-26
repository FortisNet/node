#include "sandbox_wire.h"
#include "sandbox_socket.h"
#include <cassert>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

using namespace fortis::codec::sandbox;
namespace io = fortis::compat::sandbox;

int main() try {
  for (uint8_t operation = 1; operation <= 3; ++operation) {
    for (uint32_t size : {1U, 48U}) {
      const auto header = Encode(false, operation, size);
      uint8_t parsed = 0;
      uint32_t length = 0;
      assert(Decode(header, false, parsed, length) && parsed == operation && length == size);
      assert(!Decode(header, true, parsed, length));
    }
  }
  uint8_t code = 99;
  uint32_t size = 99;
  for (auto header : {Encode(false, 0, 1), Encode(false, 4, 1), Encode(false, 1, 0), Encode(false, 1, 16385),
                      Encode(false, 1, UINT32_MAX), Encode(false, 2, 49), Encode(true, 8, 1), Encode(true, 0, 8193)}) {
    assert(!Decode(header, header[4] == 1, code, size));
    assert(code == 99 && size == 99);
  }
  const auto valid = Encode(false, 1, 16384);
  for (size_t index : {0U, 1U, 2U, 3U, 4U, 6U, 7U}) {
    auto bad = valid;
    bad[index] ^= 64;
    assert(!Decode(bad, false, code, size));
  }
  assert(Decode(valid, false, code, size) && size == 16384);
  for (uint8_t reply = 0; reply <= 7; ++reply) assert(Decode(Encode(true, reply, 8192), true, code, size));
#ifdef __linux__
  int pair[2]{};
  assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, pair) == 0);
  fortis::base::FileDescriptor right(pair[1]);
  auto moved = [&] {
    fortis::base::FileDescriptor original(pair[0]);
    return fortis::base::FileDescriptor(std::move(original));
  }();
  // The former owner has been destroyed; the transferred socket must still work.
  assert(moved.get() == pair[0] && io::PeerUid(moved.get()) == geteuid());
  std::thread writer([&] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (char value : {'a', 'b', 'c'}) io::Transmit(right.get(), &value, 1, deadline);
    assert(shutdown(right.get(), SHUT_WR) == 0);
  });
  char data[3]{};
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  io::Receive(moved.get(), data, sizeof(data), deadline);
  io::EndOfMessage(moved.get(), deadline);
  writer.join();
  assert(memcmp(data, "abc", 3) == 0);
  // Broken/short peers and slow peers are refused with a single total deadline.
  char byte = 0;
  bool refused = false;
  try {
    io::Receive(moved.get(), &byte, 1, deadline);
  } catch (const std::runtime_error&) { refused = true; }
  assert(refused);
  refused = false;
  try {
    io::Receive(right.get(), &byte, 1, std::chrono::steady_clock::now() + std::chrono::milliseconds(30));
  } catch (const std::runtime_error&) { refused = true; }
  assert(refused);
  io::Transmit(moved.get(), "x", 1, std::chrono::steady_clock::now() + std::chrono::seconds(1));
  refused = false;
  try {
    io::EndOfMessage(right.get(), std::chrono::steady_clock::now() + std::chrono::seconds(1));
  } catch (const std::runtime_error&) { refused = true; }
  assert(refused);
  // Sending to a closed peer cannot deliver SIGPIPE or hang past the deadline.
  right.reset();
  refused = false;
  try {
    io::Transmit(moved.get(), "x", 1, std::chrono::steady_clock::now() + std::chrono::milliseconds(30));
  } catch (const std::runtime_error&) { refused = true; }
  assert(refused);
  refused = false;
  try {
    io::Receive(moved.get(), &byte, 1, std::chrono::steady_clock::now() - std::chrono::seconds(1));
  } catch (const std::runtime_error&) { refused = true; }
  assert(refused);
#endif
  for (const char* path : {"", "relative", "/tmp/../socket", "/tmp/untrusted/socket"}) {
    bool refused = false;
    try {
      auto result = io::Connect(path, std::chrono::steady_clock::now() + std::chrono::milliseconds(50));
    } catch (const std::exception&) { refused = true; }
    assert(refused);
  }
} catch (...) { return 2; }
