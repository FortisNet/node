#include "tls_probe.h"
#include "outbound_https.h"

#include <stdint.h>

namespace fortis::http {

namespace {
// This callback is only supplied by the deliberately local diagnostic route.
// The reusable client has no private-address exception by default.
bool tls_probe_test_endpoint(void*, uint32_t ip) {
  const uint8_t a=static_cast<uint8_t>(ip>>24), b=static_cast<uint8_t>(ip>>16);
  return a==10 || a==127 || (a==172&&b>=16&&b<=31) || (a==192&&b==168) ||
         (a==169&&b==254) || (a==100&&b>=64&&b<=127);
}
}

TlsProbeResult tls_probe_https(const char* host, const char* port, const char* ca_file) {
  TlsProbeResult result{};
  unsigned char response[4096]{};
  OutboundRequest request{};
  request.host=host; request.port=port; request.path="/"; request.ca_file=ca_file;
  request.timeout_ms=5000; request.max_response_bytes=2048;
  request.allow_address=tls_probe_test_endpoint;
  OutboundResponse response_result=OutboundHttpsRequest(request,response,sizeof(response));
  result.error=static_cast<int>(response_result.error);
  result.connected=response_result.error!=OutboundError::kResolve &&
                   response_result.error!=OutboundError::kConnect &&
                   response_result.error!=OutboundError::kPrivateAddress &&
                   response_result.error!=OutboundError::kBusy;
  result.verified=result.connected && response_result.error!=OutboundError::kTls &&
                   response_result.error!=OutboundError::kTimeout;
  result.response=response_result.error==OutboundError::kNone &&
                  response_result.status>=200 && response_result.status<300;
  return result;
}

}  // namespace fortis::http
