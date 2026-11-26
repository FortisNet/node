#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// C shim: Unikraft scheduler headers cannot be included from C++ translation
// units. Returns cumulative idle CPU nanoseconds for the guest's first vCPU.
// False means the scheduler does not expose an idle thread yet.
int FortisGuestIdleCpuNs(uint64_t* idle_ns);
#ifdef __cplusplus
}
#endif
