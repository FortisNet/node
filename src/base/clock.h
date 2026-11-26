#pragma once

#include <errno.h>
#include <stdint.h>
#include <time.h>

// The time seam. A component that reads the time or sleeps takes a Clock
// instead of a function pointer and a `void* context`, so a test (or a replay
// binary) substitutes one typed object.
//
// The base class is the system clock: a fake overrides only what it controls
// and keeps the real behaviour for the rest (for example a frozen wall clock
// with real sleeps). No method is pure, so the guest needs no
// __cxa_pure_virtual, and the destructor is not virtual, so no object is ever
// deleted through this borrowed interface. The native and guest C++ runtimes
// both support allocation/destruction; protected nonvirtual destruction enforces
// this seam's non-owning contract rather than a runtime-library limitation.
namespace fortis::base {

class Clock {
 public:
  // The wall clock: milliseconds since the Unix epoch (CLOCK_REALTIME).
  virtual int64_t NowMs() {
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
  }
  // Milliseconds on CLOCK_MONOTONIC: deadlines and intervals only.
  virtual uint64_t MonotonicMs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1000000;
  }
  // Blocks the calling thread for `ms` milliseconds (resumed after a signal).
  virtual void SleepMs(uint32_t ms) {
    timespec delay{static_cast<time_t>(ms / 1000), static_cast<long>(ms % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
  }

 protected:
  constexpr Clock() = default;
  ~Clock() = default;
};

namespace clock_detail {
class System final : public Clock {
 public:
  constexpr System() = default;
};
// Constant-initialized and trivially destructible: usable before main() and
// during shutdown, with no guard and no atexit entry.
inline System system_clock;
}  // namespace clock_detail

// The production clock, shared by the process.
inline Clock& SystemClock() { return clock_detail::system_clock; }

}  // namespace fortis::base
