#pragma once

#include "threading.h"

#include <array>
#include <cmath>
#include <optional>
#include <stdint.h>

namespace fortis::chains {

class ChainId {
 public:
  constexpr ChainId() : value_(0) {}
  explicit constexpr ChainId(uint64_t value) : value_(value) {}
  static std::optional<ChainId> FromNumber(double value) {
    // Avoid undefined floating-to-integer conversion at 2^64 and preserve zero.
    if (!std::isfinite(value) || value < 0 || value >= 18446744073709551616.0 || std::trunc(value) != value)
      return std::nullopt;
    return ChainId(static_cast<uint64_t>(value));
  }
  friend constexpr bool operator==(ChainId a, ChainId b) { return a.value_ == b.value_; }
 private:
  uint64_t value_;
};

// Process-local insertion-order memo: duplicate/full inserts are no-ops.
// Instances retain their own capacity and mutex; no global or eviction policy.
template <size_t N>
class BoundedChainIdSet {
 public:
  bool Contains(ChainId id) const {
    base::LockGuard lock(mutex_);
    return ContainsLocked(id);
  }
  void Insert(ChainId id) {
    base::LockGuard lock(mutex_);
    if (size_ < N && !ContainsLocked(id)) ids_[size_++] = id;
  }
 private:
  bool ContainsLocked(ChainId id) const {
    for (size_t i = 0; i < size_; ++i) if (ids_[i] == id) return true;
    return false;
  }
  mutable base::Mutex mutex_;
  // ChainId's explicit constructor prevents accidental numeric narrowing.
  std::array<ChainId, N> ids_{};
  size_t size_ = 0;
};

} // namespace fortis::chains
