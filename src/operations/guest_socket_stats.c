// Guest-only. Read the same lwIP socket entries used by accept and socket.
#include "lwip/priv/sockets_priv.h"

#include "guest_socket_stats.h"

void fortis_guest_socket_stats(struct FortisGuestSocketStats* out) {
  SYS_ARCH_DECL_PROTECT(level);
  uint32_t used = 0;
  SYS_ARCH_PROTECT(level);
  for (int slot = 0; slot < NUM_SOCKETS; ++slot) {
    const struct lwip_sock* socket = lwip_socket_dbg_get_socket(slot + LWIP_SOCKET_OFFSET);
    if (socket->conn) ++used;
  }
  SYS_ARCH_UNPROTECT(level);
  *out = (struct FortisGuestSocketStats){NUM_SOCKETS, used, sizeof(struct lwip_sock), 0};
}
