#pragma once

#include <stddef.h>
#include <stdint.h>
#include "ownership.h"

namespace fortis::http {

constexpr size_t kHttpMaxPathParams = 4;
constexpr size_t kHttpPathParamNameBytes = 32;
constexpr size_t kHttpPathParamValueBytes = 512;
// Wire budgets include the complete header block and exclude body terminators.
// Keep the edge policy and direct-runtime resource tests aligned with these.
inline constexpr size_t kHttpMaxHeaderBytes = 16 * 1024;
inline constexpr size_t kHttpMaxBodyBytes = 512 * 1024;
inline constexpr size_t kHttpMaxTargetBytes = 12500;
inline constexpr long kHttpReadDeadlineSeconds = 3;

struct HttpPathParam {
  char name[kHttpPathParamNameBytes + 1]{};
  char value[kHttpPathParamValueBytes + 1]{};
  // String.prototype.length (UTF-16 code units) of the decoded segment.
  size_t utf16_units = 0;
};

// One request owns its storage. All pointers remain valid until FreeHttpRequest.
struct HttpRequest {
  std::unique_ptr<char[], base::StorageDeleter> allocation;
  std::unique_ptr<char[], base::StorageDeleter> body_allocation;
  std::unique_ptr<char[], base::StorageDeleter> target_allocation;
  char* method = nullptr;
  char* path = nullptr;
  // Original request target, including any query string, for route handlers.
  char* raw_target = nullptr;
  char* body = nullptr;
  size_t body_length = 0;
  bool has_content_length = false;
  bool protobuf_content_type = false;
  bool json_content_type = false;
  bool has_content_type = false;
  bool has_content_encoding = false;
  // A Content-Type other than application/json or application/protobuf.
  bool unsupported_content_type = false;
  const char* authorization = nullptr;
  const char* origin = nullptr;
  const char* accept = nullptr;
  const char* if_none_match = nullptr;
  // x-intelligence-authorization (the AI conversation bearer). A repeated
  // header is refused (400) like the other credential headers.
  const char* intelligence_authorization = nullptr;
  // The Host header value (request.host; no trusted-proxy forwarding).
  const char* host = nullptr;
  bool connection_close = false;
  bool expect_continue = false;
  char remote_ip[64]{};
  HttpPathParam path_params[kHttpMaxPathParams]{};
  size_t path_param_count = 0;
};

// Returns a decoded, request-owned parameter value for a matched route.
const char* HttpPathParamValue(const HttpRequest& request, const char* name);
// The matched parameter itself (null when the route has no such name).
const HttpPathParam* HttpPathParamFind(const HttpRequest& request, const char* name);

enum class HttpReadResult {
  Ok,
  Empty,
  BadRequest,
  HeaderTooLarge,
  BodyTooLarge,
  UnsupportedEncoding,
  Timeout,
  Unavailable,
};

// One parser implementation; callers release even a partially read request.
class RequestParser {
 public:
  static HttpReadResult Read(int client, HttpRequest* request);
  /// Parse a complete header block into request-owned storage, without I/O.
  /// Used by ingress before allocating/admitting a body; release on all paths.
  static HttpReadResult Head(const char* bytes, size_t length, HttpRequest* request);
  static void Release(HttpRequest* request) noexcept;
};
// One connection's parsed storage. Header/body/parameter views borrow this
// object and expire on destruction, including when route execution throws.
class ParsedRequest {
 public:
  ParsedRequest() = default;
  ~ParsedRequest() noexcept { RequestParser::Release(&value_); }
  ParsedRequest(const ParsedRequest&) = delete;
  ParsedRequest& operator=(const ParsedRequest&) = delete;
  HttpRequest& get() { return value_; }
 private:
  HttpRequest value_{};
};

HttpReadResult ReadHttpRequest(int client, HttpRequest* request);
void FreeHttpRequest(HttpRequest* request);

}  // namespace fortis::http
