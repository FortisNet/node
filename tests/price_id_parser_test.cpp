#include "../src/providers/price_id_parser.h"
#include "price_id_parser_fixtures.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace fortis::providers;

namespace {
void assert_empty(const ParsedPriceId& value) {
  assert(value.kind == PriceIdKind::kInvalid && value.source_id[0] == '\0' &&
         value.network[0] == '\0' && value.address[0] == '\0' &&
         value.standard[0] == '\0');
}

// js-case: ids and networks are ASCII lower-cased, so the
// recorded TypeScript answers for these non-ASCII inputs no longer apply.
// `network` null: the input is refused. "" : the network is the raw one,
// unchanged (it has no A-Z). Otherwise the expected network.
struct AsciiCaseExpectation {
  const char* name;
  const char* network;
};
constexpr AsciiCaseExpectation kAsciiCase[] = {
    {"coingecko-kelvin-unicode-fold", nullptr},          // U+212A is not 'k'
    {"erc20-unicode-network-fold", "\303\211th"},        // É stays, TH is lowered
    {"erc20-final-sigma-network", "\316\243\316\243"},   // no Final_Sigma
    {"source-id-grows-past-512-when-lowered", ""},       // İ does not grow: valid
};

const AsciiCaseExpectation* ascii_case(const char* name) {
  for (const AsciiCaseExpectation& e : kAsciiCase)
    if (strcmp(e.name, name) == 0) return &e;
  return nullptr;
}

void check_ascii_case(const PriceIdParserFixture& fixture, const AsciiCaseExpectation& expected) {
  ParsedPriceId parsed{};
  const bool valid = ParsePriceId(fixture.raw, strlen(fixture.raw), &parsed);
  if (!expected.network) {
    assert(!valid);
    assert_empty(parsed);
    return;
  }
  char network[1024];
  const size_t network_length = static_cast<size_t>(strchr(fixture.raw, ':') - fixture.raw);
  assert(network_length < sizeof(network));
  memcpy(network, fixture.raw, network_length);
  network[network_length] = '\0';
  const char* want = expected.network[0] ? expected.network : network;
  char source[1100];
  snprintf(source, sizeof(source), "%s:erc20:0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", want);
  assert(valid && parsed.kind == PriceIdKind::kContract);
  assert(strcmp(parsed.network, want) == 0);
  assert(strcmp(parsed.source_id, source) == 0);
  assert(strcmp(parsed.standard, "erc20") == 0);
  assert(strcmp(parsed.address, "0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0);
}

void run_differential_fixtures() {
  size_t documented_gaps = 0;
  size_t ascii_cases = 0;
  for (const PriceIdParserFixture& fixture : kPriceIdParserFixtures) {
    if (const AsciiCaseExpectation* expected = ascii_case(fixture.name)) {
      check_ascii_case(fixture, *expected);
      ++ascii_cases;
      continue;
    }
    ParsedPriceId parsed{};
    const bool valid = ParsePriceId(fixture.raw, strlen(fixture.raw), &parsed);
    if (fixture.native_kind == PriceIdKind::kInvalid) {
      assert(!valid);
      assert_empty(parsed);
      if (fixture.typescript_kind != PriceIdKind::kInvalid) {
        assert(fixture.documented_gap[0] != '\0');
        ++documented_gaps;
      }
      continue;
    }
    assert(valid);
    assert(parsed.kind == fixture.native_kind);
    assert(strcmp(parsed.source_id, fixture.typescript_source_id) == 0);
    assert(strcmp(parsed.network, fixture.typescript_network) == 0);
    assert(strcmp(parsed.address, fixture.typescript_address) == 0);
    assert(strcmp(parsed.standard, fixture.typescript_standard) == 0);
    assert(fixture.typescript_kind == fixture.native_kind);
  }
  // Only the 512-byte source-id bound remains (Unicode trim matches); the
  // case gap that grew a network past it is an ASCII case now.
  assert(documented_gaps == 1);
  assert(ascii_cases == sizeof(kAsciiCase) / sizeof(kAsciiCase[0]));
}

void test_explicit_length_and_failure_clearing() {
  ParsedPriceId parsed{};
  static const char valid[] = "bitcoin";
  assert(ParsePriceId(valid, sizeof(valid) - 1, &parsed));
  static const char embedded_nul[] = {'b', 'i', 't', '\0', 'c', 'o', 'i', 'n'};
  assert(!ParsePriceId(embedded_nul, sizeof(embedded_nul), &parsed));
  assert_empty(parsed);

  char long_network[514];
  memset(long_network, 'n', 464);
  memcpy(long_network + 464, ":erc20:", 7);
  memcpy(long_network + 471, "0x", 2);
  memset(long_network + 473, 'a', 40);
  long_network[513] = '\0';
  // A syntactically valid TypeScript network may canonicalize past the native
  // source-key cap. Failure must leave no partial typed value behind.
  assert(!ParsePriceId(long_network, 513, &parsed));
  assert_empty(parsed);
}

void test_coingecko_network_platform_mapping() {
  assert(strcmp(CoinGeckoPlatformForNetwork("eth-mainnet"), "ethereum") == 0);
  assert(strcmp(CoinGeckoPlatformForNetwork("eip155:01"), "ethereum") == 0);
  assert(strcmp(CoinGeckoPlatformForNetwork("eip155:00056"), "binance-smart-chain") == 0);
  assert(CoinGeckoPlatformForNetwork("eip155:4663") == nullptr);
  assert(CoinGeckoPlatformForNetwork("unsupported-network") == nullptr);
}
}  // namespace

int main() {
  run_differential_fixtures();
  test_explicit_length_and_failure_clearing();
  test_coingecko_network_platform_mapping();
  puts("native bounded price-id parser differential fixtures passed");
}
