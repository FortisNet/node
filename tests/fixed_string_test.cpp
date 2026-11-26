#include "fixed_string.h"
#include "unit_test_support.h"

#include <algorithm>
#include <stdio.h>
#include <string>
#include <type_traits>

namespace {
using fortis::base::FixedString;
using fortis::testing::Require;
uint32_t state = 0x19f17ed;
uint32_t Next() {
  state = state * 1664525u + 1013904223u;
  return state;
}

// This libc oracle deliberately exercises truncation, including N == 2.
// Production formatting remains warning-as-error; this exact negative control
// must retain snprintf rather than comparing the implementation to itself.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
template <size_t N>
void Differential() {
  static_assert(sizeof(FixedString<N>) == N, "preserve bounded record layout");
  static_assert(alignof(FixedString<N>) == alignof(char));
  static_assert(std::is_trivially_copyable_v<FixedString<N>>);
  static_assert(std::is_trivially_destructible_v<FixedString<N>>);
  for (size_t sample = 0; sample < 2000; ++sample) {
    const size_t length = sample < 4 ? (sample == 0 ? 0 : N + sample - 2) : Next() % (2 * N + 3);
    std::string input(length, 'x');
    for (char& byte : input) byte = static_cast<char>(Next() >> 24);
    FixedString<N> actual;
    char expected[N];
    memset(actual.data(), 0x5a, N);
    memset(expected, 0x5a, N);
    const int count = snprintf(expected, N, "%s", input.c_str());
    const bool fits = actual.Assign(input.c_str());
    Require(fits == (static_cast<size_t>(count) < N), "named truncation result matches snprintf");
    Require(memcmp(actual.data(), expected, N) == 0, "all buffer bytes, including untouched tail, match snprintf");
    // The view can end before the backing buffer's terminator.
    const size_t prefix = input.empty() ? 0 : Next() % input.size();
    const std::string bounded(input.data(), prefix);
    memset(actual.data(), 0x5a, N);
    memset(expected, 0x5a, N);
    snprintf(expected, N, "%s", bounded.c_str());
    actual.Assign(std::string_view(input.data(), prefix));
    Require(memcmp(actual.data(), expected, N) == 0, "bounded nonterminated view follows the same NUL/truncation rule");
  }
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

void CopiesAndEmpty() {
  constexpr FixedString<6> empty;
  static_assert(empty[0] == '\0', "constant initialization is empty");
  Require(empty.view().empty(), "empty initial state");
  FixedString<6> value;
  Require(value.Assign("abcde"), "N-1 bytes fit");
  Require(!value.Assign("abcdef"), "N bytes truncate");
  const auto copy = value;
  Require(copy.view() == "abcde", "copies own their bytes");
  value.Assign(value.view().substr(1));
  Require(value.view() == "bcde" && copy.view() == "abcde", "overlapping assignment and independent copy");
  Require(value.Assign(std::string_view{}), "default empty view fits");
  Require(value.view().empty(), "empty assignment terminates");
  std::fill_n(value.data(), value.capacity(), 'x');
  Require(value.view() == "xxxxxx", "view remains bounded when a C callee omits the terminator");
  value[2] = '\0';
  Require(value.view() == "xx", "view stops at the first embedded terminator");
}
void BoundedCStrings() {
  using fortis::base::CopyCString;
  using fortis::base::ConcatCString;
  char text[6];
  Require(ConcatCString(text, sizeof(text), {"a", "bc", "de"}) && std::string_view(text) == "abcde",
          "concatenation fills the capacity exactly");
  Require(!ConcatCString(text, sizeof(text), {"abcdef", "xyz"}) && std::string_view(text) == "abcde",
          "concatenation reports truncation and preserves prefix order");
  Require(ConcatCString(nullptr, 0, {""}) && !ConcatCString(nullptr, 0, {"x"}), "zero capacity is bounded");
  Require(ConcatCString(text, sizeof(text), {std::string_view("a\0b", 3), "c"}) && std::string_view(text) == "ac",
          "each C string part stops at NUL");
  Require(CopyCString(text, "abcde") && CopyCString(text, text + 1) && std::string_view(text) == "bcde",
          "copy supports overlapping source and destination");
}
void Randomized() {
  Differential<1>();
  Differential<2>();
  Differential<6>();
  Differential<27>();
  Differential<64>();
  Differential<96>();
  Differential<128>();
  Differential<160>();
  Differential<200>();
  Differential<260>();
  Differential<400>();
  Differential<513>();
  Differential<640>();
  Differential<650>();
  Differential<1024>();
  Differential<1057>();
  Differential<1100>();
  Differential<2158>();
}
}  // namespace
template <typename T>
concept TemporaryStringView = requires(T value) {
  std::move(value).view();
};
static_assert(!TemporaryStringView<fortis::base::FixedString<8>>);
static_assert(std::is_same_v<decltype(std::declval<fortis::base::FixedString<8>&&>()[0]), char>);

int main() {
  fortis::testing::UnitSuite suite;
  suite.Run("snprintf differential across record capacities and byte/NUL inputs", Randomized);
  suite.Run("constant empty state, copies, overlap and boundaries", CopiesAndEmpty);
  suite.Run("bounded C string copying and concatenation", BoundedCStrings);
  return suite.Finish("fixed_string_test");
}
