#pragma once

#include "clock.h"
#include "seams.h"

#include <stdint.h>

// Test fakes of the shared seams (src/base/clock.h, src/base/seams.h).
namespace fortis::testing {

// A clock that moves only when told: NowMs and MonotonicMs return the set
// values, and SleepMs advances both instead of blocking. Reads and writes are
// atomic, so a test thread may move it while service threads read it.
class ManualClock final : public fortis::base::Clock {
 public:
  explicit ManualClock(int64_t now_ms = 0, uint64_t monotonic_ms = 0)
      : now_ms_(now_ms), monotonic_ms_(monotonic_ms) {}

  int64_t NowMs() override { return __atomic_load_n(&now_ms_, __ATOMIC_SEQ_CST); }
  uint64_t MonotonicMs() override { return __atomic_load_n(&monotonic_ms_, __ATOMIC_SEQ_CST); }
  void SleepMs(uint32_t ms) override { Advance(ms); }

  void SetNowMs(int64_t now_ms) { __atomic_store_n(&now_ms_, now_ms, __ATOMIC_SEQ_CST); }
  void SetMonotonicMs(uint64_t monotonic_ms) { __atomic_store_n(&monotonic_ms_, monotonic_ms, __ATOMIC_SEQ_CST); }
  // Moves both clocks forward by `ms`.
  void Advance(uint64_t ms) {
    __atomic_add_fetch(&now_ms_, static_cast<int64_t>(ms), __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&monotonic_ms_, ms, __ATOMIC_SEQ_CST);
  }

 private:
  int64_t now_ms_;
  uint64_t monotonic_ms_;
};

// A clock whose wall time is a variable the test owns (for example a global
// the test moves between steps); monotonic time and sleeps stay real.
class PointerClock final : public base::Clock {
 public:
  explicit PointerClock(const int64_t* now_ms) : now_ms_(now_ms) {}
  int64_t NowMs() override { return __atomic_load_n(now_ms_, __ATOMIC_SEQ_CST); }

 private:
  const int64_t* now_ms_;
};

// Always the same jitter sample (0.5 is "no jitter" for the +-10 % rule).
class FixedJitter final : public fortis::base::JitterSource {
 public:
  explicit FixedJitter(double sample = 0.5) : sample_(sample) {}
  bool Sample(double* sample) override {
    *sample = sample_;
    return true;
  }

 private:
  double sample_;
};

}  // namespace fortis::testing
