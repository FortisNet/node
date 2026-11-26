#pragma once
#include "http_listener.h"
#include <memory>

namespace fortis::app {
namespace https_detail {
struct Ingress;
}
/** Direct public ingress. One bounded nonblocking reactor owns handshakes and
 * uploads; complete requests borrow the existing pool/router. No route worker
 * waits for a client upload or an idle keep-alive connection. Completion writers
 * wake ingress through a bounded nonblocking self-pipe; its descriptors outlive
 * every transport user, including a Finish call publishing its final state.
 * Construct after crypto initialization; destroy only after workers and response
 * handoff stop. Run interrupts sockets on exit but retains all transports.
 */
class HttpsListener {
 public:
  HttpsListener();
  ~HttpsListener();
  HttpsListener(const HttpsListener&) = delete;
  HttpsListener& operator=(const HttpsListener&) = delete;
  /// True when FORTIS_HTTPS_CERT_DIR is set. Invalid configuration fails Start.
  static bool Enabled();
  /// Borrows listener/pool/health until destruction. Binds redirect port itself.
  /// Start is single-use, including failure; destroy a refused candidate.
  bool Start(int listener, HttpWorkerPool& pool, http::HttpRouter& health);
  /// Serve until shutdown; false before successful Start or on fatal error.
  bool Run();

 private:
  std::unique_ptr<https_detail::Ingress> impl_;
};
}  // namespace fortis::app
