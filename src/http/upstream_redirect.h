#pragma once

// The upstream endpoint seam of the provider owners and the chain transport.
// An owner normally sends each request to its provider's host with the
// configured CA bundle and the public-address policy. A composition that
// calls the owner's StartRedirected instead sends every request to one
// endpoint with its own CA file and address policy; the request path, the
// configured keys and the response handling stay the owner's. Production
// starts every owner with Start(), which leaves the redirect off; the replay
// binaries under tests/ point it at a loopback fixture server.

#include "outbound_https.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace fortis::http {

struct UpstreamRedirect {
  char host[254]{};
  char port[6]{};
  char ca_file[1024]{};
  bool (*allow_address)(void* context, uint32_t ipv4_host_order) = nullptr;
  void* context = nullptr;
  // Optional: a request header that carries the host the owner would have
  // dialled (see SetRealHostHeader). Empty sends none.
  char real_host_header[kOutboundHeaderNameCapacity]{};
  bool enabled = false;

  // A decimal TCP port 1..65535 of at most five digits.
  static bool ValidPort(const char* value) {
    if (!value) return false;
    const size_t length = strnlen(value, 6);
    if (!length || length >= 6) return false;
    unsigned number = 0;
    for (size_t i = 0; i < length; ++i) {
      if (value[i] < '0' || value[i] > '9') return false;
      number = number * 10 + static_cast<unsigned>(value[i] - '0');
    }
    return number > 0 && number <= 65535;
  }

  // Whether Set would accept these values: each present and short enough to
  // store, and an address policy given. `strict` also refuses an empty host
  // or CA path and a port that is not a decimal TCP port.
  bool Accepts(const char* new_host, const char* new_port, const char* new_ca,
               bool (*allow)(void*, uint32_t), bool strict) const {
    if (!new_host || strnlen(new_host, sizeof(host)) >= sizeof(host) || !new_port ||
        strnlen(new_port, sizeof(port)) >= sizeof(port) || !new_ca ||
        strnlen(new_ca, sizeof(ca_file)) >= sizeof(ca_file) || !allow)
      return false;
    return !strict || (*new_host && ValidPort(new_port) && *new_ca);
  }

  // Stores the endpoint and turns the redirect on. False (nothing changed)
  // when Accepts refuses the values.
  bool Set(const char* new_host, const char* new_port, const char* new_ca,
           bool (*allow)(void*, uint32_t), void* allow_context, bool strict = false) {
    if (!Accepts(new_host, new_port, new_ca, allow, strict)) return false;
    snprintf(host, sizeof(host), "%s", new_host);
    snprintf(port, sizeof(port), "%s", new_port);
    snprintf(ca_file, sizeof(ca_file), "%s", new_ca);
    allow_address = allow;
    context = allow_context;
    enabled = true;
    return true;
  }

  // Names the header for the real host; false when the name does not fit.
  bool SetRealHostHeader(const char* name) {
    if (!name || strnlen(name, sizeof(real_host_header)) >= sizeof(real_host_header)) return false;
    snprintf(real_host_header, sizeof(real_host_header), "%s", name);
    return true;
  }

  // Points `request` at the endpoint when the redirect is on.
  void Apply(OutboundRequest& request) const {
    if (!enabled) return;
    request.host = host;
    request.port = port;
    request.ca_file = ca_file;
    request.allow_address = allow_address;
    request.policy_context = context;
  }
};

}  // namespace fortis::http
