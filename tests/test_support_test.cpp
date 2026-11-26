#include "test_support.h"

#include <unistd.h>
#include <fstream>
#include <string>

namespace {
void require(bool condition) {
  if (!condition) throw std::runtime_error("test support contract failed");
}

template <typename Function>
void rejects(Function function) {
  try {
    function();
  } catch (const std::runtime_error&) {
    return;
  }
  throw std::runtime_error("invalid test input was accepted");
}

struct ScratchDirectory {
  char path[48] = "/tmp/fortis-test-support-XXXXXX";
  ScratchDirectory() {
    if (!mkdtemp(path)) throw std::runtime_error("cannot create test directory");
  }
  ~ScratchDirectory() { fortis::test::RemoveTree(path); }
};
}  // namespace

int main() try {
  using namespace fortis::test;
  require(ParseInteger<int>(" +123suffix") == 123);
  require(ParseInteger<int>("-42") == -42);
  rejects([] { ParseInteger<int>(""); });
  rejects([] { ParseInteger<int>("+-1"); });
  rejects([] { ParseInteger<int>("++1"); });
  rejects([] { ParseInteger<int>("99999999999999999999999999"); });
  rejects([] { ParseInteger<unsigned>("-1"); });
  require(ParseDouble("1.25") == 1.25);
  rejects([] { ParseDouble("1.25tail"); });

  unsigned byte = 0;
  require(ParseHexByte("Ff", &byte) && byte == 255);
  require(!ParseHexByte("f", &byte) && !ParseHexByte("xz", &byte));
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, millis = 0;
  require(ParseTimestampParts("2026-11-20T12:34:56.789Z", &year, &month, &day,
                              &hour, &minute, &second, &millis));
  require(year == 2026 && month == 11 && day == 20 && hour == 12 && minute == 34 &&
          second == 56 && millis == 789);
  require(!ParseTimestampParts("2026/11-20T12:34:56.789Z", &year, &month, &day,
                               &hour, &minute, &second, &millis));

  ScratchDirectory scratch;
  const auto child = std::filesystem::path(scratch.path) / "path with spaces";
  std::filesystem::create_directory(child);
  { std::ofstream file(child / "fixture"); file << "test"; }
  RemoveTree(child);
  require(!std::filesystem::exists(child) && std::filesystem::exists(scratch.path));
  std::puts("test support: checked parsing and shell-free cleanup passed");
} catch (...) {
  return fortis::test::ReportException();
}
