#pragma once

// The wall clocks the price owners read in test binaries. A replay moves the
// clock between requests by rewriting a file, or fixes it for the process:
//   FORTIS_TEST_PRICE_NOW_FILE / FORTIS_TEST_PRICE_NOW_MS      the spot owner
//   FORTIS_TEST_HISTORY_NOW_FILE / FORTIS_TEST_HISTORY_NOW_MS  history coverage
// The file wins over the fixed value, and with neither the system clock
// answers. Both variables are read on every call (a fixture changes them
// between owners). Test binaries only: they install the clock with
// SpotProviderOwner::SetClock (or PlaneOptions::price_owner_clock) and
// SetPriceHistoryCoverageClock.

#include "clock.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

class EnvClock final : public fortis::base::Clock {
 public:
  constexpr EnvClock(const char* file_variable, const char* fixed_variable)
      : file_variable_(file_variable), fixed_variable_(fixed_variable) {}

  int64_t NowMs() override {
    // A clock file (moved between requests by a replay) wins over a fixed value.
    const char* file = getenv(file_variable_);
    if (file && *file) {
      if (FILE* handle = fopen(file, "r")) {
        char buffer[32]{};
        const size_t length = fread(buffer, 1, sizeof(buffer) - 1, handle);
        fclose(handle);
        buffer[length] = '\0';
        char* end = nullptr;
        const long long value = strtoll(buffer, &end, 10);
        if (end && end != buffer && value > 0 && (!*end || *end == '\n'))
          return static_cast<int64_t>(value);
      }
    }
    const char* fixed = getenv(fixed_variable_);
    if (fixed && *fixed) {
      char* end = nullptr;
      const long long value = strtoll(fixed, &end, 10);
      if (end && *end == '\0' && value > 0) return static_cast<int64_t>(value);
    }
    return fortis::base::Clock::NowMs();
  }

 private:
  const char* file_variable_;
  const char* fixed_variable_;
};

inline EnvClock& PriceOwnerEnvClock() {
  static EnvClock clock("FORTIS_TEST_PRICE_NOW_FILE", "FORTIS_TEST_PRICE_NOW_MS");
  return clock;
}

inline EnvClock& HistoryCoverageEnvClock() {
  static EnvClock clock("FORTIS_TEST_HISTORY_NOW_FILE", "FORTIS_TEST_HISTORY_NOW_MS");
  return clock;
}
