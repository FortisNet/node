#pragma once
#include <array>
#include <stddef.h>
#include <type_traits>
namespace fortis::tokens {
// Allocation-free FIFO mechanics only. The runtime's existing shared lock
// protects every operation; callers retain pointee ownership until transfer.
template <typename T, size_t N>
class TokenQueue {
  static_assert(N > 0, "token queues have positive capacity");
  static_assert(std::is_trivially_copyable<T>::value, "queue entries are trivial handles");
 public:
  bool Push(T value) {
    if (count_ == N) return false;
    entries_[(head_ + count_) % N] = value;
    ++count_;
    return true;
  }
  T Pop() {
    if (!count_) return T{};
    T value = entries_[head_];
    head_ = (head_ + 1) % N;
    --count_;
    return value;
  }
  size_t size() const { return count_; }
  // Stable removal: same FIFO-relative scan/compaction as the token batcher.
  template <typename Predicate>
  size_t Select(Predicate joins, T* output, size_t limit) {
    size_t kept = 0, selected = 0;
    for (size_t i = 0; i < count_; ++i) {
      T value = entries_[(head_ + i) % N];
      if (joins(value) && selected < limit) output[selected++] = value;
      else entries_[(head_ + kept++) % N] = value;
    }
    count_ = kept;
    return selected;
  }
 private:
  std::array<T, N> entries_{};
  size_t head_ = 0, count_ = 0;
};
}  // namespace fortis::tokens
