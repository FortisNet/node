#include <memory>
#include "test_support.h"
#include "native_config.h"
// R27 transaction-view units against tests/corpus/transaction_view_edges.json
// (written by the earlier TypeScript implementation's tests):
//  - chains::ViewFor(family)->is_final: every TypeScript view's isFinal (the
//    TxDetailViewCache shouldStore) over the same detail shapes and network
//    (`units.isFinal`): EVM blockNumber != null, the UTXO confirmed-and-deep
//    rule with the catalogue's requiredConfirmations (zcash 10), and the
//    always-final ledger views;
//  - chains::ProjectInclusionOf: feeRanges + projectInclusion over a
//    recorded /v1/fees/mempool-blocks body (`units.projection`);
//  - address::TxViewKey: keys.txDetail(network, hash) (`units.keys`);
//  - the family split the route answers: every recorded 400 UNSUPPORTED
//    names a family with no native view (its "no detail view for {family}"
//    spelled by chains::FamilyName), and every network a recorded 200 view
//    came from has one.
// Usage: transaction_view_test <transaction_view_edges.json>
#include "../src/wallet/address_facets.h"
#include "../src/chains/chain_core.h"
#include "../src/chains/chain_driver.h"
#include "../src/config/native_config.h"
#include "../src/codec/token_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

using namespace fortis::wallet;

using namespace fortis::config;

using namespace fortis;
namespace tj = fortis::codec::tj;

namespace {

int failures = 0;
int checked = 0;

void fail(const char* what, const char* detail) {
  ++failures;
  fprintf(stderr, "FAIL: %s: %s\n", what, detail);
}

double number(const tj::Value* v) { return tj::IsNumber(v) ? v->number : NAN; }

const char* text(const tj::Value* object, const char* key) {
  const tj::Value* value = tj::Get(object, key);
  return tj::IsString(value) ? value->text : nullptr;
}

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

void is_final(const tj::Value* rows, chains::Catalog& catalog) {
  if (!tj::IsArray(rows) || rows->count == 0) { fail("isFinal", "no rows"); return; }
  auto memory_owner = std::make_unique<uint8_t[]>(1024 * 1024);
  uint8_t* memory = memory_owner.get();
  for (uint32_t i = 0; i < rows->count; ++i) {
    const tj::Value* row = rows->items[i];
    const char* family = text(row, "family");
    const char* network_id = text(row, "network");
    const char* name = text(row, "name");
    const char* detail_text = text(row, "detail");
    const tj::Value* expected = tj::Get(row, "final");
    char label[256];
    snprintf(label, sizeof(label), "isFinal %s/%s/%s", family ? family : "?", network_id ? network_id : "?",
             name ? name : "?");
    chains::Network network;
    if (!family || !network_id || !detail_text || !expected ||
        !catalog.Resolve(network_id, strlen(network_id), &network)) {
      fail(label, "unresolvable row");
      continue;
    }
    const chains::TransactionView* view = chains::ViewFor(network.family);
    if (!view || strcmp(chains::FamilyName(network.family), family) != 0) {
      fail(label, "no native view for the TypeScript family");
      continue;
    }
    tj::Arena arena{memory, 1024 * 1024, 0, true};
    const tj::Value* detail =
        tj::Parse(arena, reinterpret_cast<const uint8_t*>(detail_text), strlen(detail_text));
    if (!detail) { fail(label, "unparsable detail"); continue; }
    const bool want = expected->type == tj::Type::kTrue;
    const bool got = view->IsFinal(detail, network);
    ++checked;
    if (got != want) fail(label, want ? "native false, TypeScript true" : "native true, TypeScript false");
  }

}

void projection(const tj::Value* rows) {
  if (!tj::IsArray(rows) || rows->count == 0) { fail("projection", "no rows"); return; }
  auto memory_owner = std::make_unique<uint8_t[]>(256 * 1024);
  uint8_t* memory = memory_owner.get();
  for (uint32_t i = 0; i < rows->count; ++i) {
    const tj::Value* row = rows->items[i];
    tj::Arena arena{memory, 256 * 1024, 0, true};
    const tj::Value* rate = tj::Get(row, "feeRate");
    const double fee_rate = tj::IsString(rate) && strcmp(rate->text, "NaN") == 0 ? NAN : number(rate);
    const char* body_text = text(row, "body");
    const tj::Value* body =
        body_text ? tj::Parse(arena, reinterpret_cast<const uint8_t*>(body_text), strlen(body_text)) : nullptr;
    char label[64];
    snprintf(label, sizeof(label), "projection[%u]", i);
    if (!body) { fail(label, "unparsable body"); continue; }
    bool has_blocks = false, beyond = false;
    double blocks = 0;
    chains::ProjectInclusionOf(fee_rate, body, &has_blocks, &blocks, &beyond);
    const tj::Value* want_blocks = tj::Get(row, "blocks");
    const bool want_has = want_blocks && want_blocks->type != tj::Type::kNull;
    const bool want_beyond = tj::Get(row, "beyond") && tj::Get(row, "beyond")->type == tj::Type::kTrue;
    ++checked;
    if (has_blocks != want_has || (want_has && blocks != number(want_blocks)) || beyond != want_beyond) {
      char detail[160];
      snprintf(detail, sizeof(detail), "native {blocks: %s%g, beyond: %d}", has_blocks ? "" : "null/", blocks,
               beyond);
      fail(label, detail);
    }
  }

}

void keys(const tj::Value* rows) {
  if (!tj::IsArray(rows) || rows->count == 0) { fail("keys", "no rows"); return; }
  for (uint32_t i = 0; i < rows->count; ++i) {
    const tj::Value* row = rows->items[i];
    const char* network = text(row, "network");
    const char* hash = text(row, "hash");
    const char* want = text(row, "key");
    char key[1024];
    ++checked;
    if (!network || !hash || !want || !address::TxViewKey(network, hash, key, sizeof(key)) || strcmp(key, want) != 0) {
      fail("TxViewKey", want ? want : "?");
    }
  }
}

// The {network} segment of a recorded target, percent-decoded (ASCII ids).
bool network_of(const char* target_hex, char* out, size_t capacity) {
  char target[1024]{};
  size_t length = strlen(target_hex) / 2;
  if (length >= sizeof(target)) return false;
  for (size_t i = 0; i < length; ++i) {
    unsigned value = 0;
    if (fortis::test::ParseHexByte(target_hex + 2 * i, &value) != 1) return false;
    target[i] = static_cast<char>(value);
  }
  target[length] = '\0';
  static const char kPrefix[] = "/v1/networks/";
  if (strncmp(target, kPrefix, sizeof(kPrefix) - 1) != 0) return false;
  const char* start = target + sizeof(kPrefix) - 1;
  const char* end = strchr(start, '/');
  if (!end) return false;
  size_t used = 0;
  for (const char* p = start; p < end && used + 1 < capacity; ++p) {
    if (*p == '%' && p + 2 < end) {
      unsigned value = 0;
      if (fortis::test::ParseHexByte(p + 1, &value) != 1) return false;
      out[used++] = static_cast<char>(value);
      p += 2;
    } else {
      out[used++] = *p;
    }
  }
  out[used] = '\0';
  return true;
}

void family_split(const tj::Value* cases, chains::Catalog& catalog) {
  size_t unsupported = 0, served = 0;
  for (uint32_t i = 0; tj::IsArray(cases) && i < cases->count; ++i) {
    const tj::Value* c = cases->items[i];
    const char* response = text(c, "responseText");
    const char* target = text(c, "targetHex");
    const tj::Value* status = tj::Get(c, "status");
    if (!response || !target || !status) continue;
    char network_id[256];
    if (!network_of(target, network_id, sizeof(network_id))) continue;
    chains::Network network;
    const bool resolved = catalog.Resolve(network_id, strlen(network_id), &network);
    if (strstr(response, "\"error\":\"UNSUPPORTED\"")) {
      char want[160];
      snprintf(want, sizeof(want), "no detail view for %s",
               resolved ? chains::FamilyName(network.family) : "?");
      ++unsupported;
      ++checked;
      if (!resolved || chains::ViewFor(network.family) || !strstr(response, want)) fail("UNSUPPORTED", network_id);
    } else if (number(status) == 200 && strstr(response, "\"family\":")) {
      ++served;
      ++checked;
      if (!resolved || !chains::ViewFor(network.family)) fail("served view", network_id);
    }
  }
  if (!unsupported || !served) fail("family split", "the fixture has no UNSUPPORTED or no served view");
  printf("family split: %zu UNSUPPORTED and %zu served view cases checked\n", unsupported, served);
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc != 2) {
    fprintf(stderr, "usage: transaction_view_test <transaction_view_edges.json>\n");
    return 2;
  }
  size_t length = 0;
  char* data = read_file(argv[1], &length);
  if (!data) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  tj::Arena arena = tj::GrowingArena(256 * 1024 * 1024);
  const tj::Value* fixture = tj::Parse(arena, reinterpret_cast<const uint8_t*>(data), length);
  const tj::Value* units = tj::Get(fixture, "units");
  if (!units) {
    fprintf(stderr, "no units in %s\n", argv[1]);
    return 2;
  }
  NativeConfig config{};
  chains::Catalog* catalog = static_cast<chains::Catalog*>(calloc(1, sizeof(chains::Catalog)));
  new (catalog) chains::Catalog();
  if (!catalog->Init(config)) {
    fprintf(stderr, "catalog Init failed\n");
    return 2;
  }
  is_final(tj::Get(units, "isFinal"), *catalog);
  projection(tj::Get(units, "projection"));
  keys(tj::Get(units, "keys"));
  family_split(tj::Get(fixture, "cases"), *catalog);
  catalog->~Catalog();
  free(catalog);
  tj::FreeArena(arena);
  free(data);
  printf("transaction view units: %d checked, %d failed\n", checked, failures);
  return failures ? 1 : 0;
} catch (...) {
  return fortis::test::ReportException();
}
