#pragma once

#include <stdint.h>

enum class ProviderHttpTestMode : uint8_t {
  kOk,
  kSlow,
  kRetry503,
  kAlways503,
  kRateLimited,
  kTlsFailure,
  kStall,
};

namespace fortis::http { class ResponseHandoff; using HttpResponseHandoff = ResponseHandoff; }

// Test-only HTTP-to-provider bridge. HTTP threads may only enqueue a bounded
// fixed-mode intent; the owner thread is the sole lane/adapter caller.
class ProviderHttpBridge {
 public:
  ProviderHttpBridge();
  ~ProviderHttpBridge();
  ProviderHttpBridge(const ProviderHttpBridge&) = delete;
  ProviderHttpBridge& operator=(const ProviderHttpBridge&) = delete;

  bool Start(const char* upstream_port, const char* tls_failure_port,
            const char* ca_file, fortis::http::HttpResponseHandoff* handoff);
  bool Enqueue(uint64_t http_token, ProviderHttpTestMode mode,
               uint64_t http_deadline_ms);
  void Stop();
  bool valid() const;

 private:
  struct Impl;
  Impl* impl_;
};
