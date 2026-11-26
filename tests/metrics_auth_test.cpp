#include "test_support.h"
// GET /metrics bearer policy (src/operations/metrics_auth.h): exact "Bearer <token>"
// bytes, as TypeScript's `authorization !== \`Bearer ${expected}\``.
#undef NDEBUG
#include "metrics_auth.h"

#include <assert.h>
#include <stdio.h>

#include <string>

using namespace fortis::operations;

namespace {

bool matches(const std::string& authorization, const std::string& token) {
  return MatchesMetricsBearer(authorization.c_str(), token.c_str());
}

void test_exact_match_only() {
  const std::string token = "Metrics-Token_9";
  assert(matches("Bearer " + token, token));
  // Missing or absent values never match, whatever the other side holds.
  assert(!MatchesMetricsBearer(nullptr, token.c_str()));
  assert(!MatchesMetricsBearer(("Bearer " + token).c_str(), nullptr));
  assert(!MatchesMetricsBearer(nullptr, nullptr));
  assert(!matches("", token));
  assert(!matches("Bearer", token));
  assert(!matches("Bearer ", token));
  // The scheme is case-sensitive and must be followed by exactly one space.
  assert(!matches("bearer " + token, token));
  assert(!matches("BEARER " + token, token));
  assert(!matches("Basic " + token, token));
  assert(!matches("Bearer  " + token, token));
  assert(!matches("Bearer\t" + token, token));
  assert(!matches(token, token));
  // Token bytes are case-sensitive.
  assert(!matches("Bearer metrics-token_9", token));
  // Prefixes and extensions of the token, in either direction.
  assert(!matches("Bearer " + token.substr(0, token.size() - 1), token));
  assert(!matches("Bearer " + token + "0", token));
  assert(!matches("Bearer x" + token, token));
  assert(!matches("Bearer " + token + " ", token));
  // Equal length, one wrong byte at the start, middle and end.
  for (size_t position : {size_t{0}, token.size() / 2, token.size() - 1}) {
    std::string wrong = token;
    wrong[position] = static_cast<char>(wrong[position] ^ 0x01);
    assert(!matches("Bearer " + wrong, token));
  }
}

void test_empty_and_oversized_tokens_never_match() {
  // An empty configured token is a disabled route, not an open one: the bare
  // "Bearer " header (an empty credential) must not authenticate.
  assert(!matches("Bearer ", ""));
  assert(!matches("Bearer x", ""));
  // A token past the configured bound would otherwise compare only its first
  // kMetricsTokenMaxBytes bytes; it is refused outright.
  const std::string too_long(kMetricsTokenMaxBytes + 1, 'k');
  assert(!matches("Bearer " + too_long, too_long));
  std::string too_long_tail = too_long;
  too_long_tail.back() = 'z';
  assert(!matches("Bearer " + too_long_tail, too_long));
}

void test_maximum_length_token() {
  std::string token;
  for (size_t i = 0; i < kMetricsTokenMaxBytes; ++i)
    token.push_back(static_cast<char>('!' + (i * 7) % 94));
  assert(token.size() == kMetricsTokenMaxBytes);
  assert(matches("Bearer " + token, token));
  // The final byte participates in the comparison.
  std::string last = token;
  last.back() = static_cast<char>(last.back() == 'A' ? 'B' : 'A');
  assert(!matches("Bearer " + last, token));
  assert(!matches("Bearer " + token.substr(0, kMetricsTokenMaxBytes - 1), token));
  assert(!matches("Bearer " + token + "A", token));
  // Header values far past the bound (the request parser allows ~16 KiB)
  // are rejected without reading beyond the bound plus one byte.
  assert(!matches("Bearer " + token + std::string(16 * 1024, 'A'), token));
  assert(!matches("Bearer " + std::string(16 * 1024, 'A'), token));
}

void test_non_ascii_bytes_compare_exactly() {
  // Natively the header and the token are both raw bytes, so a UTF-8 token
  // matches its own bytes (Node's Latin-1 header decoding never would).
  const std::string token = "z\xc5\x82oty";
  assert(matches("Bearer " + token, token));
  assert(!matches("Bearer z\xc5\x81oty", token));
  assert(!matches("Bearer zloty", token));
}

}  // namespace

int main() try {
  test_exact_match_only();
  test_empty_and_oversized_tokens_never_match();
  test_maximum_length_token();
  test_non_ascii_bytes_compare_exactly();
  puts("metrics auth test passed");
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
