#include "http_request.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

/// Shared libFuzzer entry point: no sockets, clocks, secrets or network services.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > fortis::http::kHttpMaxHeaderBytes + 1) return 0;
  fortis::http::ParsedRequest parsed;
  const auto result = fortis::http::RequestParser::Head(reinterpret_cast<const char*>(data), size, &parsed.get());
  if (result == fortis::http::HttpReadResult::Ok) {
    const auto& request = parsed.get();
    assert(request.method && request.path && request.raw_target && request.host);
    assert(request.body_length <= fortis::http::kHttpMaxBodyBytes);
    assert(request.path[0] == '/');
  }
  return 0;
}

#ifndef FORTIS_LIBFUZZER
int main() try {
  // Reproducible mutation corpus, never a security or production random source.
  std::mt19937 random(0x48545450U);  // NOLINT(cert-msc51-cpp)
  const std::string seeds[] = {
      "GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n",
      "POST /v1/devices HTTP/1.1\r\nHost: localhost\r\nContent-Length: 524288\r\n\r\n",
      "GET /%70robe%2fasync?x=%00 HTTP/1.1\r\nHost: localhost\r\n\r\n",
      "POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n",
      "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
      "GET / HTTP/1.1\r\nHost: a\r\nAuthorization: a\r\nAuthorization: b\r\n\r\n"};
  for (unsigned iteration = 0; iteration < 100000; ++iteration) {
    std::string wire = seeds[random() % std::size(seeds)];
    for (unsigned mutation = 0; mutation < 1 + iteration % 8; ++mutation) {
      const size_t index = random() % wire.size();
      switch (random() % 3) {
        case 0: wire[index] = static_cast<char>(random() & 255U); break;
        case 1: wire.resize(1 + index); break;
        default: wire.insert(index, 1, static_cast<char>(random() & 255U)); break;
      }
    }
    LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(wire.data()), wire.size());
  }
  puts("HTTP boundary: 100000 deterministic mutated inputs passed");
  return 0;
} catch (...) { return 1; }
#endif
