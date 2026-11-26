#include "guest_allocator_metrics.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace fortis::operations {

namespace {

__attribute__((format(printf, 4, 5)))
bool append(char* output, size_t capacity, size_t* used, const char* format, ...) {
  if (*used >= capacity) return false;
  va_list arguments;
  va_start(arguments, format);
  const int written = vsnprintf(output + *used, capacity - *used, format, arguments);
  va_end(arguments);
  if (written < 0 || static_cast<size_t>(written) >= capacity - *used) return false;
  *used += static_cast<size_t>(written);
  return true;
}

}  // namespace

bool CollectGuestAllocatorSample(FortisGuestAllocatorStats* sample) {
  if (!sample) return false;
#ifdef FORTIS_UNIKRAFT
  return fortis_guest_allocator_stats(sample) != 0;
#else
  return false;
#endif
}

bool AppendGuestAllocatorMetrics(const FortisGuestAllocatorStats& sample,
                                 char* output, size_t capacity, size_t* used) {
  if (!output || !used || *used >= capacity) return false;
  // Format on the stack first so a short buffer never receives a partial block.
  char block[kGuestAllocatorMetricsMaxBytes];
  size_t length = 0;
  bool ok = append(block, sizeof(block), &length,
      "# HELP fortis_guest_allocator_allocated_bytes Bytes in live allocations tracked by the Unikraft default allocator's statistics, counted in whole pages. Memory from any other allocator, or not tracked by these counters, is omitted. Allocator figure only: not guest RSS, kernel or static data, page tables, or host/QEMU memory.\n"
      "# TYPE fortis_guest_allocator_allocated_bytes gauge\n"
      "fortis_guest_allocator_allocated_bytes %lld\n"
      "# HELP fortis_guest_allocator_allocated_peak_bytes High-water mark of fortis_guest_allocator_allocated_bytes since guest boot. Allocator figure only: not peak guest RSS or host memory.\n"
      "# TYPE fortis_guest_allocator_allocated_peak_bytes gauge\n"
      "fortis_guest_allocator_allocated_peak_bytes %lld\n",
      static_cast<long long>(sample.allocated_bytes),
      static_cast<long long>(sample.allocated_peak_bytes));
  if (ok && sample.available_bytes >= 0) {
    ok = append(block, sizeof(block), &length,
        "# HELP fortis_guest_allocator_available_bytes Free guest RAM bytes the Unikraft default allocator can still hand out; fragmentation can refuse a large request below this total. Allocator figure only: not host free memory.\n"
        "# TYPE fortis_guest_allocator_available_bytes gauge\n"
        "fortis_guest_allocator_available_bytes %lld\n",
        static_cast<long long>(sample.available_bytes));
  }
  if (ok) {
    ok = append(block, sizeof(block), &length,
        "# HELP fortis_guest_allocator_failures_total Allocation requests the Unikraft default allocator refused for lack of memory since guest boot.\n"
        "# TYPE fortis_guest_allocator_failures_total counter\n"
        "fortis_guest_allocator_failures_total %llu\n",
        static_cast<unsigned long long>(sample.failures));
  }
  if (!ok || length >= capacity - *used) return false;
  memcpy(output + *used, block, length + 1);
  *used += length;
  return true;
}

}  // namespace fortis::operations
