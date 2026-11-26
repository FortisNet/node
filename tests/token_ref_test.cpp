#include "test_support.h"
// Replays tests/corpus/token_ref_corpus.json (generated from the
// TypeScript parseTokenRef, DriverRegistry.network and sanitize functions by
// the earlier TypeScript implementation's tests) against the
// native token_ref and text_sanitize ports.
#include "strict_sanitize_cases.h"
#include "bounded_json.h"
#include "price_history_batch_codec.h"
#include "text_sanitize.h"
#include "token_ref.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace fortis::codec;
using namespace fortis::networks;

namespace {

using fortis::codec::history_batch::OutBuffer;

constexpr size_t kTokens = 131072;
BoundedJsonDocument doc;
char text_a[8192], text_b[8192];
int failures = 0;

void fail(const char* what, const char* detail) {
  if (++failures <= 40) fprintf(stderr, "FAIL %s: %s\n", what, detail);
}

const BoundedJsonToken* tok(uint32_t i) { return doc.token(i); }

uint32_t field(uint32_t object, const char* key) { return doc.FindObjectValue(object, key); }

bool is_null(uint32_t i) { return tok(i) && tok(i)->type == BoundedJsonType::kNull; }

const char* str(uint32_t i, char* out, size_t capacity, size_t* length = nullptr) {
  size_t n = 0;
  if (!doc.DecodeString(i, out, capacity, &n)) { fprintf(stderr, "bad string\n"); exit(2); }
  if (length) *length = n;
  return out;
}

uint32_t nth(uint32_t array, size_t index) {
  uint32_t child = tok(array)->first_child;
  for (size_t i = 0; i < index && child != kBoundedJsonNoToken; ++i) child = tok(child)->next_sibling;
  return child;
}

size_t hex_decode(const char* hex, char* out) {
  const size_t n = strlen(hex) / 2;
  for (size_t i = 0; i < n; ++i) {
    unsigned value = 0;
    if (!fortis::test::ParseHexByte(hex + 2 * i, &value)) throw std::runtime_error("invalid hex in test fixture");
    out[i] = static_cast<char>(value);
  }
  return n;
}

void check_optional(const char* what, const char* raw, uint32_t expected, bool has,
                    const char* actual) {
  char want[2048];
  if (is_null(expected)) {
    if (has) { snprintf(text_b, sizeof(text_b), "%s: got %s, want null", raw, actual); fail(what, text_b); }
    return;
  }
  str(expected, want, sizeof(want));
  if (!has || strcmp(want, actual) != 0) {
    snprintf(text_b, sizeof(text_b), "%s: got %s, want %s", raw, has ? actual : "null", want);
    fail(what, text_b);
  }
}

void check_networks(uint32_t networks) {
  for (uint32_t item = tok(networks)->first_child; item != kBoundedJsonNoToken;
       item = tok(item)->next_sibling) {
    char id[256]; size_t id_length = 0;
    str(field(item, "id"), id, sizeof(id), &id_length);
    TokenNetwork network;
    const bool found = ResolveTokenNetwork(id, id_length, &network);
    const uint32_t result = field(item, "result");
    if (is_null(result)) { if (found) fail("network should be null", id); continue; }
    if (!found) { fail("network missing", id); continue; }
    char want[256];
    if (strcmp(str(field(result, "id"), want, sizeof(want)), network.id) != 0) fail("network id", id);
    if (strcmp(str(field(result, "family"), want, sizeof(want)), network.family) != 0)
      fail("network family", id);
    if (strcmp(id, "ton") == 0) {
      // Preserve the frozen TypeScript oracle. The current catalog deliberately
      // corrects its obsolete GRAM/gram identity instead of creating an alias.
      if (strcmp(str(field(result, "nativeSymbol"), want, sizeof(want)), "GRAM") != 0 ||
          strcmp(str(field(result, "nativeCoingeckoId"), want, sizeof(want)), "gram") != 0)
        fail("frozen TON oracle changed", id);
      if (!network.native_symbol || strcmp(network.native_symbol, "TON") != 0)
        fail("corrected native symbol", id);
      if (!network.native_coingecko_id || strcmp(network.native_coingecko_id, "the-open-network") != 0)
        fail("corrected native coin ID", id);
      continue;
    }
    check_optional("nativeSymbol", id, field(result, "nativeSymbol"), network.native_symbol != nullptr,
                   network.native_symbol ? network.native_symbol : "");
    check_optional("nativeCoingeckoId", id, field(result, "nativeCoingeckoId"),
                   network.native_coingecko_id != nullptr,
                   network.native_coingecko_id ? network.native_coingecko_id : "");
  }
}

void check_refs(uint32_t ref_networks, uint32_t refs) {
  static TokenNetwork networks[64];
  static bool has_network[64];
  size_t network_count = 0;
  for (uint32_t item = tok(ref_networks)->first_child; item != kBoundedJsonNoToken;
       item = tok(item)->next_sibling, ++network_count) {
    has_network[network_count] = !is_null(item);
    if (!has_network[network_count]) continue;
    char id[256]; size_t n = 0;
    str(item, id, sizeof(id), &n);
    if (!ResolveTokenNetwork(id, n, &networks[network_count])) fail("ref network", id);
  }
  size_t count = 0;
  static TokenRef ref;
  for (uint32_t item = tok(refs)->first_child; item != kBoundedJsonNoToken;
       item = tok(item)->next_sibling, ++count) {
    double index = 0;
    doc.Number(nth(item, 0), &index);
    const size_t network_index = static_cast<size_t>(index);
    size_t raw_length = 0;
    str(nth(item, 1), text_a, sizeof(text_a), &raw_length);
    char label[1200];
    snprintf(label, sizeof(label), "[%zu] '%.1170s'", network_index, text_a);
    const TokenNetwork* network = has_network[network_index] ? &networks[network_index] : nullptr;
    const bool parsed = ParseTokenRef(network, text_a, raw_length, &ref);
    const uint32_t expected = nth(item, 2);
    if (is_null(expected)) {
      if (parsed) {
        snprintf(text_b, sizeof(text_b), "%s parsed as %s %s", label, TokenRefKindName(ref.kind), ref.token_id);
        fail("ref should be null", text_b);
      }
      continue;
    }
    if (!parsed) { fail("ref missing", label); continue; }
    char want[2048];
    if (strcmp(str(nth(expected, 0), want, sizeof(want)), TokenRefKindName(ref.kind)) != 0) {
      snprintf(text_b, sizeof(text_b), "%s kind %s want %s", label, TokenRefKindName(ref.kind), want);
      fail("kind", text_b);
    }
    if (strcmp(str(nth(expected, 1), want, sizeof(want)), ref.token_id) != 0) {
      snprintf(text_b, sizeof(text_b), "%s tokenId %s want %s", label, ref.token_id, want);
      fail("tokenId", text_b);
    }
    check_optional("address", label, nth(expected, 2), ref.has_address, ref.address);
    check_optional("symbol", label, nth(expected, 3), ref.has_symbol, ref.symbol);
    check_optional("issuer", label, nth(expected, 4), ref.has_issuer, ref.issuer);
    check_optional("coingeckoId", label, nth(expected, 5), ref.has_coingecko_id, ref.coingecko_id);
    check_optional("priceId", label, nth(expected, 6), ref.has_price_id, ref.price_id);
  }
  printf("token refs: %zu cases\n", count);
}

void check_wire(const char* what, const char* input_hex, uint32_t expected,
                size_t (*sanitize)(const char*, size_t, char*, size_t), size_t max_bytes) {
  char input[1024];
  const size_t input_length = hex_decode(input_hex, input);
  char output[512];
  const size_t n = sanitize(input, input_length, output, max_bytes + 1);
  if (n == kSanitizeFailed) { fail(what, input_hex); return; }
  char want[1024];
  str(field(expected, "json"), want, sizeof(want));
  const char* changed_json = nullptr;
  const char* changed_hex = nullptr;
  if (strict::SanitizeCase(input_hex, what, &changed_json, &changed_hex))
    snprintf(want, sizeof(want), "%s", changed_json);
  uint8_t json[1024];
  OutBuffer out{json, sizeof(json)};
  AppendSanitizedJsonString(out, output, n);
  if (!out.ok || out.used != strlen(want) || memcmp(json, want, out.used) != 0) {
    snprintf(text_b, sizeof(text_b), "%s json %.*s want %s", input_hex,
             static_cast<int>(out.used), json, want);
    fail(what, text_b);
  }
  char utf8[512];
  const size_t utf8_length = SanitizedToProtobufUtf8(output, n, utf8, sizeof(utf8));
  char want_hex[1024], got_hex[1024];
  str(field(expected, "utf8Hex"), want_hex, sizeof(want_hex));
  if (changed_hex) snprintf(want_hex, sizeof(want_hex), "%s", changed_hex);
  for (size_t i = 0; i < utf8_length; ++i)
    snprintf(got_hex + 2 * i, 3, "%02x", static_cast<unsigned char>(utf8[i]));
  got_hex[2 * utf8_length] = '\0';
  if (strcmp(got_hex, want_hex) != 0) {
    snprintf(text_b, sizeof(text_b), "%s utf8 %s want %s", input_hex, got_hex, want_hex);
    fail(what, text_b);
  }
}

void check_sanitize(uint32_t cases) {
  size_t count = 0;
  for (uint32_t item = tok(cases)->first_child; item != kBoundedJsonNoToken;
       item = tok(item)->next_sibling, ++count) {
    char input_hex[1024];
    str(field(item, "inputHex"), input_hex, sizeof(input_hex));
    check_wire("sanitizeSymbol", input_hex, field(item, "symbol"), SanitizeSymbol,
               kSanitizeSymbolMaxBytes);
    check_wire("sanitizeName", input_hex, field(item, "name"), SanitizeName,
               kSanitizeNameMaxBytes);
  }
  printf("sanitize: %zu cases\n", count);
}

void check_bounds() {
  char out[8];
  // A too-small buffer fails instead of truncating silently.
  if (SanitizeName("abcdefghij", 10, out, sizeof(out)) != kSanitizeFailed) fail("bound", "small");
  // Invalid UTF-8 is refused.
  if (SanitizeSymbol("\xff", 1, out, sizeof(out)) != kSanitizeFailed) fail("bound", "utf8");
  static TokenRef ref;
  if (ParseTokenRef(nullptr, "\xc3", 1, &ref)) fail("bound", "ref utf8");
  static char long_input[kTokenRefMaxInputBytes + 2];
  memset(long_input, 'a', sizeof(long_input));
  if (ParseTokenRef(nullptr, long_input, sizeof(long_input), &ref)) fail("bound", "ref length");
}

}  // namespace

int main(int argc, char** argv) try {
  const char* path = argc > 1 ? argv[1] : "tests/corpus/token_ref_corpus.json";
  FILE* file = fopen(path, "rb");
  if (!file) { perror(path); return 2; }
  static unsigned char input[1024 * 1024];
  const size_t length = fread(input, 1, sizeof(input), file);
  fclose(file);
  auto* tokens = static_cast<BoundedJsonToken*>(malloc(sizeof(BoundedJsonToken) * kTokens));
  BoundedJsonError error{};
  if (!tokens || !doc.ParseWithLimits(input, length, tokens, kTokens, kTokens,
                                      kBoundedJsonMaxExpandedInputBytes, &error)) {
    fprintf(stderr, "cannot parse %s (%d)\n", path, static_cast<int>(error));
    return 2;
  }
  check_networks(field(doc.root(), "networks"));
  check_refs(field(doc.root(), "refNetworks"), field(doc.root(), "refs"));
  check_sanitize(field(doc.root(), "sanitize"));
  check_bounds();
  free(tokens);
  if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
  printf("token_ref_test: ok\n");
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
