#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct FortisGuestSocketStats {
  uint32_t capacity;
  uint32_t used;
  uint32_t slot_bytes;
  uint32_t sampled_peak;
};

void fortis_guest_socket_stats(struct FortisGuestSocketStats* out);

#ifdef __cplusplus
}
#endif
