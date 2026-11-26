#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <cstring>
#include <string_view>

namespace fortis::base {

/** Copy a C string into a bounded buffer, always terminating when capacity > 0.
 * Embedded NUL ends the input. Returns false if the input was truncated.
 * Source and destination may overlap; the caller provides writable capacity.
 */
inline bool CopyCString(char* output, std::size_t capacity, std::string_view input) noexcept {
  const auto nul = input.find('\0');
  const auto length = nul == std::string_view::npos ? input.size() : nul;
  if (!capacity) return length == 0;
  const auto copied = std::min(length, capacity - 1);
  if (copied) std::memmove(output, input.data(), copied);
  output[copied] = '\0';
  return length < capacity;
}
template <std::size_t N>
inline bool CopyCString(char (&output)[N], std::string_view input) noexcept {
  return CopyCString(output, N, input);
}

/** Concatenate C-string parts without allocation; false reports truncation.
 * Each part ends at its first NUL. Output must not overlap any input part.
 */
inline bool ConcatCString(char* output, std::size_t capacity, std::initializer_list<std::string_view> parts) noexcept {
  std::size_t used = 0;
  bool complete = true;
  for (auto part : parts) {
    part = part.substr(0, part.find('\0'));
    const auto copied = capacity ? std::min(part.size(), capacity - used - 1) : 0;
    if (copied) std::memcpy(output + used, part.data(), copied);
    used += copied;
    complete = complete && copied == part.size();
  }
  if (capacity) output[used] = '\0';
  return complete;
}

/** Value-semantic bounded C string, retaining the existing N-byte record layout.
 * Assign truncates at N-1 bytes and reports false; embedded NUL ends input.
 * All operations are nonthrowing. data/c_str/view borrow this object's storage;
 * mutation invalidates prior views. C callees must stay within capacity and
 * terminate their output. Concurrent mutation requires external synchronization.
 */
template <std::size_t N>
class FixedString {
  static_assert(N > 0, "a C string needs a terminator");

 public:
  constexpr FixedString() = default;
  bool Assign(std::string_view input) noexcept { return CopyCString(bytes_.data(), bytes_.size(), input); }
  bool Assign(const char* input) noexcept { return Assign(std::string_view(input)); }
  constexpr char* data() & noexcept { return bytes_.data(); }
  constexpr const char* data() const& noexcept { return bytes_.data(); }
  constexpr const char* c_str() const& noexcept { return bytes_.data(); }
  char* data() && = delete;
  const char* data() const&& = delete;
  const char* c_str() const&& = delete;
  constexpr std::size_t capacity() const noexcept { return bytes_.size(); }
  std::string_view view() const& noexcept {
    const auto terminator = std::find(bytes_.begin(), bytes_.end(), '\0');
    return {bytes_.data(), static_cast<std::size_t>(terminator - bytes_.begin())};
  }
  /// Views borrow this object until mutation/destruction; temporaries cannot lend.
  std::string_view view() const&& = delete;
  constexpr char& operator[](std::size_t index) & noexcept { return bytes_[index]; }
  constexpr const char& operator[](std::size_t index) const& noexcept { return bytes_[index]; }

  /// Indexing a temporary returns a value, never a dangling reference.
  constexpr char operator[](std::size_t index) const&& noexcept { return bytes_[index]; }

 private:
  std::array<char, N> bytes_{};
};

}  // namespace fortis::base
