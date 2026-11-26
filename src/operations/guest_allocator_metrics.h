#pragma once

#include <stddef.h>
#include <stdint.h>

#include "guest_allocator_stats.h"

namespace fortis::operations {

// Upper bound of AppendGuestAllocatorMetrics output: fixed help text plus four
// 20-digit values. The test checks the worst case fits.
constexpr size_t kGuestAllocatorMetricsMaxBytes = 2048;

// Fills `sample` from the Unikraft default allocator. Host builds, and guest
// images without CONFIG_LIBUKALLOC_IFSTATS, return false: no allocator
// figures exist there and none are invented.
bool CollectGuestAllocatorSample(FortisGuestAllocatorStats* sample);

// Appends the fixed allocator gauges to `output` at `*used`. Never allocates.
// On overflow returns false and leaves `*used` and output[*used] unchanged.
// A negative available_bytes omits that one gauge.
bool AppendGuestAllocatorMetrics(const FortisGuestAllocatorStats& sample,
                                 char* output, size_t capacity, size_t* used);

}  // namespace fortis::operations
