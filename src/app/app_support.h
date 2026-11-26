#pragma once

// Small helpers shared by the composition root's translation units (the
// probe routes, the replay binaries' probes and ServerMain): response shorthands, the port
// parser and the probe body bound.

#include "http_server.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace fortis::app {

constexpr size_t kProbeBody = 4096;

inline void respond_bytes(int client, int status, const char* reason,
                          const char* content_type, const uint8_t* body,
                          size_t length, bool head = false) {
  http::WriteHttpResponse(client, {status, reason, content_type, body, length}, head);
}

inline void respond(int client, int status, const char* reason, const char* body,
                    bool head = false) {
  const http::HttpResponse response = http::JsonResponse(status, reason, body);
  WriteHttpResponse(client, response, head);
}

// A decimal TCP port 1..65535, else -1 (argv[1]).
inline int parse_port(const char* value) {
  if (!value || !*value) return -1;
  char* end = nullptr;
  const long port = strtol(value, &end, 10);
  return *end == '\0' && port > 0 && port <= 65535 ? static_cast<int>(port) : -1;
}

// Query parameter `name` of `raw_target` as an unsigned decimal at most
// `maximum`; `fallback` when absent or malformed.
inline unsigned query_unsigned(const char* raw_target, const char* name,
                               unsigned fallback, unsigned maximum) {
  if (!raw_target || !name) return fallback;
  const char* query = strchr(raw_target, '?');
  if (!query) return fallback;
  const size_t name_length = strlen(name);
  for (const char* part = query + 1; *part;) {
    const char* end = strchr(part, '&');
    if (!end) end = part + strlen(part);
    if (static_cast<size_t>(end - part) > name_length + 1 &&
        strncmp(part, name, name_length) == 0 && part[name_length] == '=') {
      unsigned value = 0;
      for (const char* digit = part + name_length + 1; digit < end; ++digit) {
        if (*digit < '0' || *digit > '9') return fallback;
        const unsigned next = value * 10U + static_cast<unsigned>(*digit - '0');
        if (next > maximum || next < value) return fallback;
        value = next;
      }
      return value;
    }
    part = *end ? end + 1 : end;
  }
  return fallback;
}

}  // namespace fortis::app
