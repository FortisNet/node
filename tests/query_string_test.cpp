#include "test_support.h"
#include "query_string.h"

#include <stdio.h>
#include <array>
#include <initializer_list>
#include <string>
#include <string_view>

namespace qs = fortis::http::query_string;
int failures = 0;
void check(bool condition, const char* label) {
  if (!condition) { fprintf(stderr, "FAIL: %s\n", label); ++failures; }
}

void CheckDecodedUnsigned() {
  struct Case {
    std::string_view input;
    uint64_t maximum;
    bool valid;
    uint64_t expected;
  };
  const Case cases[] = {
      {"0", 0, true, 0}, {"0001", 1, true, 1}, {"100", 100, true, 100},
      {"100", 99, false, 0}, {"18446744073709551615", UINT64_MAX, true, UINT64_MAX},
      {"18446744073709551616", UINT64_MAX, false, 0},
      {"", UINT64_MAX, false, 0}, {"+1", UINT64_MAX, false, 0},
      {"-1", UINT64_MAX, false, 0}, {" 1", UINT64_MAX, false, 0},
      {"1 ", UINT64_MAX, false, 0}, {"1.0", UINT64_MAX, false, 0},
      {"1e2", UINT64_MAX, false, 0}, {"0x10", UINT64_MAX, false, 0},
      {"%31", UINT64_MAX, false, 0}, {std::string_view{"1\0", 2}, UINT64_MAX, false, 0},
  };
  constexpr uint64_t unchanged = 42;
  for (const auto& [input, maximum, valid, expected] : cases) {
    auto actual = unchanged;
    check(qs::ParseUnsigned(input.data(), input.size(), maximum, &actual) == valid,
          "decoded integer grammar and bounds");
    check(actual == (valid ? expected : unchanged), "failure preserves the output value");
  }
  auto actual = unchanged;
  check(!qs::ParseUnsigned(nullptr, 0, UINT64_MAX, &actual) && actual == unchanged,
        "empty null input is rejected without modifying output");
  const std::array<char, 3> bounded{'1', '2', 'x'};
  check(qs::ParseUnsigned(bounded.data(), 2, 12, &actual) && actual == 12,
        "only the supplied nonterminated input range is parsed");
  auto leading_zeros = std::string(4096, '0');
  leading_zeros += '1';
  check(qs::ParseUnsigned(leading_zeros.data(), leading_zeros.size(), 1, &actual) && actual == 1,
        "arbitrarily long leading zeros do not overflow");
}

void CheckStreamingDecode() {
  const auto append = [](void* context, unsigned char byte) {
    static_cast<std::string*>(context)->push_back(static_cast<char>(byte));
  };
  constexpr std::string_view encoded = "a+b%2Bc%2520%00%F4%8F%BF%BF";
  constexpr char expected[] = "a b+c%20\0\xf4\x8f\xbf\xbf";
  std::string output;
  check(qs::Component{encoded.data(), encoded.size()}.DecodeTo(append, &output) == sizeof(expected) - 1 &&
        output == std::string(expected, sizeof(expected) - 1),
        "streaming decode preserves plus, escapes, NUL and UTF-8 bytes");
  for (const std::string_view invalid : {"ok%", "ok%0", "ok%GG", "ok%F4%90%80%80", "ok%E2%82"}) {
    output.clear();
    check(qs::Component{invalid.data(), invalid.size()}.DecodeTo(append, &output) == SIZE_MAX &&
          output.empty(), "invalid input never emits a partial decoded value");
  }
  check(qs::Component{}.DecodeTo(append, &output) == 0 && output.empty(), "empty stream emits nothing");
  uint64_t number = 42;
  for (const std::string_view invalid : {"+1", "%2B1", "1%00", "%GG", "%1", "%"}) {
    check(!qs::Component{invalid.data(), invalid.size()}.Unsigned(UINT64_MAX, &number) && number == 42,
          "invalid encoded numbers preserve the output value");
  }
}

int main() try {
  CheckDecodedUnsigned();
  CheckStreamingDecode();
  const auto query = qs::Query::FromTarget(
      "/v1/test?ids=coingecko%3Abitcoin%2Cspl%3Aabc&refresh=true&x=a+b%2Bc%2520"
      "&%69ds=ignored&empty&empty=second&eq=a=b");
  check(query.Valid(), "Dio/URLSearchParams grammar");
  qs::Component value;
  check(query.First("ids", &value) && value.Equals("coingecko:bitcoin,spl:abc"),
        "encoded separators and first decoded key wins");
  check(query.First("refresh", &value) && value.Equals("true"), "literal refresh=true");
  check(query.First("x", &value) && value.Equals("a b+c%20"), "plus and single percent decode");
  check(query.First("empty", &value) && value.Equals(""), "first empty value wins");
  check(query.First("eq", &value) && value.Equals("a=b"), "first equals separates");
  check(!query.First("absent", &value), "absent field");
  check(qs::Query::FromTarget(nullptr).Valid() && qs::Query::FromTarget("/test?").Valid(),
        "empty queries");

  const char* invalid[] = {"x=%", "x=%0", "x=%GG", "%GG=x", "x=%80", "x=%C0%AF",
      "x=%E0%80%80", "x=%ED%A0%80", "x=%F4%90%80%80", "x=%F5%80%80%80",
      "x=%E2%82", "x=%E2x%AC", "ok=1&unused=%FF", "ok=1&ok=%FF", "x=\xff"};
  for (const char* text : invalid)
    check(!qs::Query(text, strlen(text)).Valid(), text);
  const auto utf8 = qs::Query::FromTarget("/?x=%C2%A0%E2%82%AC%F4%8F%BF%BF&nul=%00");
  check(utf8.Valid(), "Unicode boundaries and embedded NUL");
  char output[10]{};
  check(utf8.First("x", &value) && value.Decode(output, sizeof(output)) == 9 &&
        memcmp(output, "\xc2\xa0\xe2\x82\xac\xf4\x8f\xbf\xbf", 9) == 0, "UTF-8 bytes retained");
  char prefix[3] = {'!', '!', '!'};
  check(value.Decode(prefix, 2) == 9 && prefix[2] == '!', "bounded output reports full length");

  uint64_t number = 0;
  const auto numbers = qs::Query::FromTarget("/?n=000%31%30%30&max=18446744073709551615");
  check(numbers.First("n", &value) && value.Unsigned(100, &number) && number == 100 &&
        !value.Unsigned(99, &number), "decoded decimal and inclusive bound");
  check(numbers.First("max", &value) && value.Unsigned(UINT64_MAX, &number) && number == UINT64_MAX,
        "full uint64 range without rounding");
  const char* non_decimal[] = {"", "+1", "-1", "1.0", "1e2", "0x10", "true", "%201", "18446744073709551616"};
  for (const char* text : non_decimal)
    check(!qs::Component{text, strlen(text)}.Unsigned(UINT64_MAX, &number), text);
  if (failures) return 1;
  puts("query_string_test: encoding, Unicode, duplicates and decimal boundaries OK");
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
