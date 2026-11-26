#include "http_request.h"
#include "http_transport.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>

namespace fortis::http {

namespace {
constexpr size_t kMaxIfNoneMatch = 256;

ssize_t recv_before_deadline(int fd, char* buffer, size_t length,
                             const timespec& deadline) {
  if (auto* transport = FindHttpTransport(fd)) return transport->Read(buffer, length);
  for (;;) {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    long seconds = deadline.tv_sec - now.tv_sec;
    long nanoseconds = deadline.tv_nsec - now.tv_nsec;
    if (nanoseconds < 0) { --seconds; nanoseconds += 1'000'000'000L; }
    if (seconds < 0 || (seconds == 0 && nanoseconds <= 0)) {
      errno = EAGAIN;
      return -1;
    }
    const int timeout_ms = static_cast<int>(seconds * 1000 + (nanoseconds + 999999) / 1000000);
#ifdef MSG_DONTWAIT
    // lwIP may have unread bytes buffered internally after a short recv even
    // when poll no longer reports the socket readable. Probe that buffer
    // first, then wait only when the nonblocking read says it is empty.
    const ssize_t available = recv(fd, buffer, length, MSG_DONTWAIT);
    if (available >= 0) return available;
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
#endif
    pollfd descriptor{fd, POLLIN, 0};
    const int selected = poll(&descriptor, 1, timeout_ms);
    if (selected < 0 && errno == EINTR) continue;
    if (selected < 0) return -1;
    if (selected == 0) {
      errno = EAGAIN;
      return -1;
    }
    if (descriptor.revents & POLLNVAL) { errno = EBADF; return -1; }
#ifdef MSG_DONTWAIT
#else
    const ssize_t count = recv(fd, buffer, length, 0);
    if (count < 0 && errno == EINTR) continue;
    return count;
#endif
  }
}

bool token_char(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || strchr("!#$%&'*+-.^_`|~", c);
}

bool hex_char(unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

unsigned hex_value(unsigned char c) {
  return c <= '9' ? c - '0' : c <= 'F' ? c - 'A' + 10 : c - 'a' + 10;
}

bool valid_utf8_bytes(const unsigned char* bytes, size_t length) {
  size_t i = 0;
  while (i < length) {
    const unsigned char c = bytes[i++];
    if (c < 0x80) continue;
    const unsigned count = (c & 0xe0) == 0xc0 ? 1 :
                           (c & 0xf0) == 0xe0 ? 2 :
                           (c & 0xf8) == 0xf0 ? 3 : 99;
    if (count == 99 || length - i < count) return false;
    uint32_t codepoint = c & (count == 1 ? 0x1f : count == 2 ? 0x0f : 0x07);
    for (unsigned j = 0; j < count; ++j) {
      const unsigned char next = bytes[i++];
      if ((next & 0xc0) != 0x80) return false;
      codepoint = (codepoint << 6) | (next & 0x3f);
    }
    if ((count == 1 && codepoint < 0x80) ||
        (count == 2 && codepoint < 0x800) ||
        (count == 3 && codepoint < 0x10000) || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
  }
  return true;
}

char* find_crlf(char* start, char* end) {
  for (char* p = start; p + 1 < end; ++p) {
    if (p[0] == '\r' && p[1] == '\n') return p;
  }
  return nullptr;
}

bool parse_request_line(char* first, char* end, HttpRequest* request, HttpReadResult* error) {
  char* space = static_cast<char*>(memchr(first, ' ', end - first));
  if (!space || space == first || space - first > 15) return false;
  for (char* p = first; p < space; ++p)
    if (!token_char(static_cast<unsigned char>(*p))) return false;
  *space++ = '\0';
  char* second = static_cast<char*>(memchr(space, ' ', end - space));
  if (!second || second == space || static_cast<size_t>(second - space) > kHttpMaxTargetBytes ||
      space[0] != '/' || end - second != 9 ||
      memcmp(second + 1, "HTTP/1.1", 8) != 0) return false;
  for (char* p = space; p < second; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c <= 0x20 || c == 0x7f || c == '#') return false;
  }
  const char* path_end = static_cast<const char*>(memchr(space, '?', second - space));
  if (!path_end) path_end = second;
  unsigned char decoded_path[1024];
  size_t decoded_length = 0;
  for (const char* p = space; p < path_end; ++p) {
    unsigned char c = static_cast<unsigned char>(*p);
    if (c == '%') {
      if (path_end - p < 3 || !hex_char(static_cast<unsigned char>(p[1])) ||
          !hex_char(static_cast<unsigned char>(p[2]))) return false;
      c = static_cast<unsigned char>((hex_value(static_cast<unsigned char>(p[1])) << 4) |
                                     hex_value(static_cast<unsigned char>(p[2])));
      p += 2;
    }
    if (decoded_length == sizeof(decoded_path)) return false;
    decoded_path[decoded_length++] = c;
  }
  if (!valid_utf8_bytes(decoded_path, decoded_length)) return false;
  *second = '\0';
  request->method = first;
  request->path = space;
  const size_t target_length = static_cast<size_t>(second - space);
  request->target_allocation = base::AllocateStorage<char[]>(target_length + 1);
  if (!request->target_allocation) {
    *error = HttpReadResult::Unavailable;
    return false;
  }
  memcpy(request->target_allocation.get(), space, target_length);
  request->target_allocation[target_length] = '\0';
  request->raw_target = request->target_allocation.get();
  char* query = static_cast<char*>(memchr(space, '?', target_length));
  if (query) *query = '\0';
  return true;
}

bool parse_headers(char* start, char* end, HttpRequest* request,
                   HttpReadResult* error) {
  char* line_end = find_crlf(start, end);
  if (!line_end || !parse_request_line(start, line_end, request, error)) return false;
  char* line = line_end + 2;
  bool host = false;
  while (line < end - 2) {
    line_end = find_crlf(line, end);
    if (!line_end || line_end == line) return false;
    char* colon = static_cast<char*>(memchr(line, ':', line_end - line));
    if (!colon || colon == line) return false;
    for (char* p = line; p < colon; ++p)
      if (!token_char(static_cast<unsigned char>(*p))) return false;
    *colon++ = '\0';
    while (colon < line_end && (*colon == ' ' || *colon == '\t')) ++colon;
    for (char* p = colon; p < line_end; ++p) {
      const unsigned char c = static_cast<unsigned char>(*p);
      if (c < 0x20 && c != '\t') return false;
      if (c == 0x7f) return false;
    }
    char* value_end = line_end;
    while (value_end > colon && (value_end[-1] == ' ' || value_end[-1] == '\t'))
      --value_end;
    *value_end = '\0';
    if (strcasecmp(line, "Host") == 0) {
      if (host || colon == value_end) return false;
      host = true;
      request->host = colon;
    } else if (strcasecmp(line, "Connection") == 0) {
      // Honor an explicit keep-alive; any close/unknown token wins, including
      // across repeated headers. Never let a later field undo Connection: close.
      request->connection_close = request->connection_close || strcasecmp(colon, "keep-alive") != 0;
    } else if (strcasecmp(line, "Expect") == 0) {
      request->expect_continue = true;
    } else if (strcasecmp(line, "Content-Length") == 0) {
      if (request->has_content_length || colon == value_end) return false;
      request->has_content_length = true;
      size_t length = 0;
      for (char* p = colon; p < value_end; ++p) {
        if (*p < '0' || *p > '9') return false;
        const unsigned digit = static_cast<unsigned>(*p - '0');
        if (length > (kHttpMaxBodyBytes - digit) / 10) {
          *error = HttpReadResult::BodyTooLarge;
          return false;
        }
        length = length * 10 + digit;
      }
      request->body_length = length;
    } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
      // Chunked input is not supported; reject it even without Content-Length.
      return false;
    } else if (strcasecmp(line, "Content-Encoding") == 0) {
      if (request->has_content_encoding) return false;
      request->has_content_encoding = true;
      // No request decompressor exists. Never interpret encoded bytes as the
      // claimed JSON/protobuf, or allocate a body for an unsupported encoding.
      if (strcasecmp(colon, "identity") != 0) {
        *error = HttpReadResult::UnsupportedEncoding;
        return false;
      }
    } else if (strcasecmp(line, "Content-Type") == 0) {
      if (request->has_content_type) return false;
      request->has_content_type = true;
      char* media_end = colon + strcspn(colon, "; \t");
      const char saved = *media_end;
      *media_end = '\0';
      request->protobuf_content_type = strcasecmp(colon, "application/protobuf") == 0;
      request->json_content_type = strcasecmp(colon, "application/json") == 0;
      request->unsupported_content_type = !request->protobuf_content_type &&
          !request->json_content_type;
      *media_end = saved;
    } else if (strcasecmp(line, "Authorization") == 0) {
      if (request->authorization) return false;
      request->authorization = colon;
    } else if (strcasecmp(line, "Origin") == 0) {
      if (request->origin) return false;
      request->origin = colon;
    } else if (strcasecmp(line, "Accept") == 0) {
      if (request->accept) return false;
      request->accept = colon;
    } else if (strcasecmp(line, "X-Intelligence-Authorization") == 0) {
      if (request->intelligence_authorization) return false;
      request->intelligence_authorization = colon;
    } else if (strcasecmp(line, "If-None-Match") == 0) {
      if (request->if_none_match ||
          static_cast<size_t>(value_end - colon) > kMaxIfNoneMatch) return false;
      request->if_none_match = colon;
    }
    line = line_end + 2;
  }
  return host && line == end - 2;
}
}  // namespace

HttpReadResult RequestParser::Head(const char* bytes, size_t length, HttpRequest* request) {
  if (!request || !bytes) return HttpReadResult::Unavailable;
  *request = HttpRequest{};
  if (length > kHttpMaxHeaderBytes) return HttpReadResult::HeaderTooLarge;
  if (length < 4 || memcmp(bytes + length - 4, "\r\n\r\n", 4) != 0 || memchr(bytes, '\0', length))
    return HttpReadResult::BadRequest;
  request->allocation = base::AllocateStorage<char[]>(length + 1);
  char* storage = request->allocation.get();
  if (!storage) return HttpReadResult::Unavailable;
  memcpy(storage, bytes, length);
  HttpReadResult result = HttpReadResult::BadRequest;
  if (!parse_headers(storage, storage + length, request, &result))
    return result;
  return HttpReadResult::Ok;
}

HttpReadResult RequestParser::Read(int client, HttpRequest* request) {
  if (!request) return HttpReadResult::Unavailable;
  *request = HttpRequest{};
  request->allocation = base::AllocateStorage<char[]>(kHttpMaxHeaderBytes + 1);
  char* bytes = request->allocation.get();
  if (!bytes) return HttpReadResult::Unavailable;
  timespec deadline{};
  if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) return HttpReadResult::Unavailable;
  deadline.tv_sec += kHttpReadDeadlineSeconds;
  size_t used = 0;
  size_t header_length = 0;
  while (!header_length) {
    if (used == kHttpMaxHeaderBytes) return HttpReadResult::HeaderTooLarge;
    const ssize_t count = recv_before_deadline(client, bytes + used, kHttpMaxHeaderBytes - used, deadline);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return HttpReadResult::Timeout;
    if (count <= 0) return used ? HttpReadResult::BadRequest : HttpReadResult::Empty;
    used += static_cast<size_t>(count);
    for (size_t i = used >= static_cast<size_t>(count) + 3
                        ? used - static_cast<size_t>(count) - 3 : 0;
         i + 3 < used; ++i) {
      if (memcmp(bytes + i, "\r\n\r\n", 4) == 0) {
        header_length = i + 4;
        break;
      }
    }
  }
  if (memchr(bytes, '\0', header_length)) return HttpReadResult::BadRequest;
  HttpReadResult error = HttpReadResult::BadRequest;
  if (!parse_headers(bytes, bytes + header_length, request, &error)) return error;
  size_t body_used = used - header_length;
  if (request->body_length > kHttpMaxHeaderBytes - header_length) {
    request->body_allocation = base::AllocateStorage<char[]>(request->body_length + 1);
    if (!request->body_allocation) return HttpReadResult::Unavailable;
    if (body_used) memcpy(request->body_allocation.get(), bytes + header_length, body_used);
    request->body = request->body_allocation.get();
  } else {
    request->body = bytes + header_length;
  }
  while (body_used < request->body_length) {
    const ssize_t count = recv_before_deadline(client, request->body + body_used,
                               request->body_length - body_used, deadline);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return HttpReadResult::Timeout;
    if (count <= 0) return HttpReadResult::BadRequest;
    body_used += static_cast<size_t>(count);
  }
  return HttpReadResult::Ok;
}

void RequestParser::Release(HttpRequest* request) noexcept {
  if (!request) return;
  request->target_allocation.reset();
  request->body_allocation.reset();
  request->allocation.reset();
  *request = HttpRequest{};
}

const HttpPathParam* HttpPathParamFind(const HttpRequest& request, const char* name) {
  if (!name) return nullptr;
  for (size_t i = 0; i < request.path_param_count; ++i)
    if (strcmp(request.path_params[i].name, name) == 0)
      return &request.path_params[i];
  return nullptr;
}

const char* HttpPathParamValue(const HttpRequest& request, const char* name) {
  const HttpPathParam* param = HttpPathParamFind(request, name);
  return param ? param->value : nullptr;
}

HttpReadResult ReadHttpRequest(int client, HttpRequest* request) { return RequestParser::Read(client, request); }
void FreeHttpRequest(HttpRequest* request) { RequestParser::Release(request); }

}  // namespace fortis::http
