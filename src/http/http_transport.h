#pragma once
#include "threading.h"
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>

namespace fortis::http {
/** Optional connection transport. The ingress owner registers before dispatch
 * and retains it (and the fd) until Finish is observed after the last writer.
 * Only the current request owner may read/write; Finish transfers ownership
 * back to ingress. Shutdown may interrupt the socket, never free this object.
 */
class HttpTransport {
 public:
  virtual ~HttpTransport() = default;
  virtual ssize_t Read(char* bytes, size_t length) = 0;
  virtual bool Write(const char* bytes, size_t length, const timespec& deadline) = 0;
  virtual bool KeepAlive() const = 0;
  virtual void Written(bool success) = 0;
  virtual void DisallowReuse() = 0;
  virtual void Finish() = 0;
};
namespace transport_detail {
struct Entry { int fd = -1; HttpTransport* transport = nullptr; };
inline Entry entries[256];
inline base::Mutex mutex;
}
/// Borrow only while owning this connection. Registry removal requires Finish.
inline HttpTransport* FindHttpTransport(int fd) {
  base::LockGuard lock(transport_detail::mutex);
  for (const auto& entry : transport_detail::entries)
    if (entry.fd == fd) return entry.transport;
  return nullptr;
}
/// Ingress-only registration/removal. Null removes; repeated registration of
/// the same owner is idempotent. False means invalid fd, occupied fd or full table.
inline bool RegisterHttpTransport(int fd, HttpTransport* transport) {
  if (fd < 0) return false;
  base::LockGuard lock(transport_detail::mutex);
  for (auto& entry : transport_detail::entries) {
    if (entry.fd != fd) continue;
    if (transport && entry.transport != transport) return false;
    entry.fd = transport ? fd : -1;
    entry.transport = transport;
    return true;
  }
  if (transport) {
    for (auto& entry : transport_detail::entries) {
      if (entry.fd != -1) continue;
      entry.fd = fd;
      entry.transport = transport;
      return true;
    }
  }
  return false;
}
/// Finish a dispatched request, or close an ordinary unregistered socket.
inline void CloseHttpConnection(int fd) {
  if (auto* transport = FindHttpTransport(fd)) transport->Finish();
  else close(fd);
}
}  // namespace fortis::http
