// Guest-only (FORTIS_GUEST_ONLY_SOURCES in sources.mk): never in the Makefile.
#include <uk/alloc.h>

#include "guest_allocator_stats.h"

int fortis_guest_allocator_stats(struct FortisGuestAllocatorStats* out) {
#if CONFIG_LIBUKALLOC_IFSTATS
  struct uk_alloc* allocator = uk_alloc_get_default();
  struct uk_alloc_stats stats;
  __ssz available;
  if (!out || !allocator) return 0;
  // Per-allocator counters only (not CONFIG_LIBUKALLOC_IFSTATS_GLOBAL). Under
  // the cooperative single-CPU scheduler the copy cannot interleave with an
  // allocation; uk_alloc_stats_get brackets it with uk_preempt_disable().
  uk_alloc_stats_get(allocator, &stats);
  available = uk_alloc_availmem(allocator);
  out->allocated_bytes = (int64_t) stats.cur_mem_use;
  out->allocated_peak_bytes = (int64_t) stats.max_mem_use;
  out->available_bytes = available >= 0 ? (int64_t) available : -1;
  out->failures = (uint64_t) stats.nb_enomem;
  return 1;
#else
  (void) out;
  return 0;
#endif
}
