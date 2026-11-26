// R16 token-holdings units against
// tests/corpus/tron_token_holdings_edges.json (`units`, written by
// the earlier TypeScript implementation's tests):
//  - address::ValidTronAddress: the route's /^T[1-9A-HJ-NP-Za-km-z]{33}$/
//    gate, and address::TronHoldingsCacheKey: keys.tronTokenHoldings
//    (tronhold:{address} verbatim);
//  - chains::TronWalletHoldings: TronscanClient.walletTokens' projection of a
//    parsed /api/account/wallet body (TRC-10/TRC-20 rows by String(
//    token_type), the eight display fields in order), compared as
//    JSON.stringify text, or the failure TypeScript throws (always an
//    upstream failure: 502 at the route).
// Usage: tron_token_holdings_test <tron_token_holdings_edges.json>
#include "strict_unit_expectations.h"
#include "../src/wallet/address_facets.h"
#include "../src/chains/chain_core.h"
#include "../src/chains/chain_driver.h"
#include "../src/codec/token_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace fortis;
namespace tj = fortis::codec::tj;

namespace {

int failures = 0;

void fail(const char* what, const char* detail) {
  ++failures;
  fprintf(stderr, "FAIL: %s: %s\n", what, detail);
}

const tj::Value* member(const tj::Value* object, const char* key) { return tj::Get(object, key); }

char* read_file(const char* path, size_t* length) {
  FILE* file = fopen(path, "rb");
  if (!file) return nullptr;
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  char* data = static_cast<char*>(malloc(static_cast<size_t>(size) + 1));
  if (data && fread(data, 1, static_cast<size_t>(size), file) != static_cast<size_t>(size)) {
    free(data);
    data = nullptr;
  }
  fclose(file);
  if (data) data[size] = '\0';
  *length = static_cast<size_t>(size);
  return data;
}

void addresses(const tj::Value* rows) {
  size_t checked = 0;
  for (uint32_t i = 0; i < rows->count; ++i) {
    const tj::Value* row = rows->items[i];
    const tj::Value* address = member(row, "address");
    const tj::Value* key = member(row, "key");
    const bool expected = member(row, "valid")->type == tj::Type::kTrue;
    // A C string ends at a NUL: the route refuses a decoded NUL before this
    // gate, so such a value only has to fail it.
    const bool has_nul = strlen(address->text) != address->length;
    const bool valid = !has_nul && wallet::address::ValidTronAddress(address->text);
    char got[256] = "";
    const bool keyed = wallet::address::TronHoldingsCacheKey(address->text, got, sizeof(got));
    char detail[512];
    if (valid != expected) {
      snprintf(detail, sizeof(detail), "%.80s: %s, expected %s", address->text, valid ? "valid" : "invalid",
               expected ? "valid" : "invalid");
      fail("address", detail);
    }
    if (!has_nul && (!keyed || strcmp(got, key->text) != 0)) {
      snprintf(detail, sizeof(detail), "%.80s: %s != %s", address->text, keyed ? got : "(failed)", key->text);
      fail("cache key", detail);
    }
    ++checked;
  }
  printf("addresses: %zu checked\n", checked);
}

void projections(const tj::Value* rows) {
  size_t checked = 0;
  for (uint32_t i = 0; i < rows->count; ++i) {
    const tj::Value* row = rows->items[i];
    const tj::Value* body_text = member(row, "body");
    tj::Arena arena = tj::GrowingArena(16 * 1024 * 1024);
    const tj::Value* body = tj::Parse(arena, reinterpret_cast<const uint8_t*>(body_text->text), body_text->length);
    tj::Value* holdings = nullptr;
    chains::Error error;
    const bool ok = body && chains::TronWalletHoldings(arena, body, &holdings, &error);
    const tj::Value* output = member(row, "output");
    char detail[2600];
    if (output) {
      static uint8_t text[1 << 20];
      tj::OutBuffer out{text, sizeof(text), 0, true};
      const bool written = ok && tj::Stringify(holdings, out) && out.ok;
      if (!written || out.used != output->length || memcmp(text, output->text, out.used) != 0) {
        snprintf(detail, sizeof(detail), "%.60s: %s\n  got      %.900s\n  expected %.900s", body_text->text,
                 ok ? "projected" : error.message, written ? reinterpret_cast<const char*>(text) : "",
                 output->text);
        if (written) text[out.used < sizeof(text) ? out.used : sizeof(text) - 1] = '\0';
        fail("projection", detail);
      }
    } else if (ok || error.kind != chains::ErrorKind::kUpstream) {
      snprintf(detail, sizeof(detail), "%.80s: %s, expected the upstream failure %s", body_text->text,
               ok ? "projected" : error.message, member(row, "message")->text);
      fail("projection failure", detail);
    }
    tj::FreeArena(arena);
    ++checked;
  }
  printf("projections: %zu checked\n", checked);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <tron_token_holdings_edges.json>\n", argv[0]);
    return 2;
  }
  size_t length = 0;
  char* data = read_file(argv[1], &length);
  if (!data) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  tj::Arena arena = tj::GrowingArena(64 * 1024 * 1024);
  const tj::Value* fixture = tj::Parse(arena, reinterpret_cast<const uint8_t*>(data), length);
  const auto* text_changes = strict::UnitExpectations(arena, "tests/fixtures/strict_text_expectations.json");
  strict::PatchIndexRows(arena, tj::Get(tj::Get(fixture, "units"), "projections"), tj::Get(text_changes, "tron_projections"));
  const tj::Value* units = tj::Get(fixture, "units");
  if (!units) {
    fprintf(stderr, "no units in %s\n", argv[1]);
    return 2;
  }
  addresses(tj::Get(units, "addresses"));
  projections(tj::Get(units, "projections"));
  tj::FreeArena(arena);
  free(data);
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf("tron token holdings units: all passed\n");
  return 0;
}
