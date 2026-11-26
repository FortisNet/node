#pragma once

#include <charconv>
#include <cstdint>
#include <cassert>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace fortis::test {

/** Read a binary fixture with scoped file ownership, including error paths.
 * @return false on open/read failure; allocation exceptions propagate.
 */
inline bool ReadFixture(const char* path, std::vector<uint8_t>& bytes) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return false;
  bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  return !input.bad();
}


// Fixture fields sometimes have a delimiter after the number (HTTP status,
// timestamps and hex bytes). Reject conversion errors while leaving that suffix.
template <typename Integer>
Integer ParseInteger(std::string_view text, int base = 10) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  if (!text.empty() && text.front() == '+') {
    text.remove_prefix(1);
    if (!text.empty() && text.front() == '-') throw std::runtime_error("invalid integer sign in test input");
  }
  if (text.empty()) throw std::runtime_error("empty integer in test input");
  Integer value{};
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
  if (result.ec != std::errc{}) throw std::runtime_error("invalid integer in test input");
  return value;
}

inline bool ParseHexByte(const char* text, unsigned* value) {
  if (!text || !text[0] || !text[1]) return false;
  const auto result = std::from_chars(text, text + 2, *value, 16);
  return result.ec == std::errc{} && result.ptr == text + 2;
}

inline bool ParseTimestampParts(const char* text, int* year, int* month, int* day,
                               int* hour, int* minute, int* second, int* millis) {
  if (!text || std::strlen(text) != 24 || text[4] != '-' || text[7] != '-' ||
      text[10] != 'T' || text[13] != ':' || text[16] != ':' || text[19] != '.' || text[23] != 'Z')
    return false;
  const size_t offsets[] = {0, 5, 8, 11, 14, 17, 20};
  const size_t widths[] = {4, 2, 2, 2, 2, 2, 3};
  std::array<int, 7> values{};
  for (size_t i = 0; i < values.size(); ++i) {
    const char* end = text + offsets[i] + widths[i];
    const auto result = std::from_chars(text + offsets[i], end, values[i]);
    if (result.ec != std::errc{} || result.ptr != end) return false;
  }
  *year = values[0];
  *month = values[1];
  *day = values[2];
  *hour = values[3];
  *minute = values[4];
  *second = values[5];
  *millis = values[6];
  return true;
}

inline double ParseDouble(const char* text) {
  if (!text) throw std::runtime_error("missing number in test input");
  char* end = nullptr;
  errno = 0;
  const double value = std::strtod(text, &end);
  if (end == text || *end || errno == ERANGE) throw std::runtime_error("invalid number in test input");
  return value;
}

// Called only from a main function's catch handler, so unexpected exceptions
// become an explicit test failure with a useful diagnostic.
inline int ReportException() noexcept {
  try {
    throw;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "test exception: %s\n", error.what());
  } catch (...) {
    std::fputs("unknown test exception\n", stderr);
  }
  return EXIT_FAILURE;
}

// Cleanup failures fail the test, including when cleanup runs in a destructor.
template <typename Path>
void RemoveTree(const Path& path) noexcept {
  try {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    if (error) throw std::system_error(error, "removing test directory");
  } catch (...) {
    ReportException();
    std::abort();
  }
}

}  // namespace fortis::test
