#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Plain C snapshot of the Unikraft default allocator's statistics. The guest
// shim (guest_allocator_stats.c) is compiled only into the Unikraft image,
// because <uk/alloc.h> is neither C++ clean nor present on a host build.
struct FortisGuestAllocatorStats {
  int64_t allocated_bytes;       // uk_alloc_stats.cur_mem_use
  int64_t allocated_peak_bytes;  // uk_alloc_stats.max_mem_use
  int64_t available_bytes;       // uk_alloc_availmem(); < 0 when unsupported
  uint64_t failures;             // uk_alloc_stats.nb_enomem
};

// Returns 1 and fills `out` when the image was built with
// CONFIG_LIBUKALLOC_IFSTATS and a default allocator is registered; otherwise
// returns 0 and leaves `out` untouched. Copies a fixed struct; never allocates.
int fortis_guest_allocator_stats(struct FortisGuestAllocatorStats* out);

#ifdef __cplusplus
}
#endif
