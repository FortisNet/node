#include "guest_cpu_stats.h"
#ifdef FORTIS_UNIKRAFT
#include <uk/sched.h>
#include <uk/thread.h>
#endif

int FortisGuestIdleCpuNs(uint64_t* idle_ns) {
  if (!idle_ns) return 0;
#ifdef FORTIS_UNIKRAFT
  struct uk_sched* scheduler = uk_sched_current();
  if (!scheduler) return 0;
  const struct uk_thread* idle = uk_sched_idle_thread(scheduler, 0);
  if (!idle) return 0;
  *idle_ns = idle->exec_time;
  return 1;
#else
  return 0;
#endif
}
