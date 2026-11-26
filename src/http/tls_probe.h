#pragma once

#include <stddef.h>

namespace fortis::http {

struct TlsProbeResult {
  bool connected;
  bool verified;
  bool response;
  int error;
};

// The CA is loaded from disk for every probe, so replacing the file rotates
// trust without rebuilding the image. No server certificate or private key is
// included in the guest.
TlsProbeResult tls_probe_https(const char* host, const char* port,
                               const char* ca_file);

}  // namespace fortis::http
