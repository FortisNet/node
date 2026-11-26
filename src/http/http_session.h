#pragma once
#include "http_server.h"
#include "function_ref.h"
namespace fortis::http {
// Synchronous per-connection orchestration. The router and acceptance timestamp
// outlive Dispatch; a successful reservation transfers the fd to the handoff.
class ConnectionSession {
 public:
  explicit ConnectionSession(HttpRouter& router) : router_(router) {}
  // reader is borrowed only for Dispatch. It may consume input
  // and fill only request-owned storage; it must not mutate application state,
  // write responses, close/transfer the socket or retain request views.
  bool Dispatch(int client, const char* remote_ip = nullptr, const timespec* accepted = nullptr,
      base::FunctionRef<HttpReadResult(int, HttpRequest*)> reader = RequestParser::Read);
 private:
  HttpRouter& router_;
};
class ResponseWriter {
 public:
  // Borrowed bytes, bounded stack formatting and socket I/O only. Failure may
  // follow a partial write: the caller closes, never retries another response.
  static bool Write(int client, const HttpResponse& response, bool head, const timespec& request_start) noexcept;
};
}  // namespace fortis::http
