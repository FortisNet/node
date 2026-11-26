#include "test_support.h"
// Unit checks of GET /v1/networks/{network}/token/{tokenId} (R19) below the
// HTTP layer, against the TypeScript oracle token360_edges.json:
//  - every `units` scenario: the stub TokenFacts TypeScript's Token360Facet.get
//    projected, rebuilt as native Facts, gives TypeScript's JSON text and
//    StructuredReadResponse wire byte for byte through Token360Project and the
//    route's encoders (meta from a truthy coin id only, holders {count, floor},
//    the four cache blocks, errors in insertion order, no insight, contract or
//    chainMeta; escapes, astral text, extreme doubles, a 300 KB document);
//  - one byte short of a scenario's encoding is refused, never truncated;
//  - named rule `undefined_member`: TypeScript encodes the live object, so a
//    holders object without atLeast gives `floor: undefined`, which JSON drops
//    and protobuf-es writes as an empty Value (no kind; the Dart Struct reader
//    throws on it). Native omits the member in both: its wire is TypeScript's
//    wire of the JSON round trip (`jsonWireHex`, checked for every scenario),
//    and only that scenario's live wire differs. No holder source leaves
//    atLeast unset, so the route never reaches it.
// Usage: token360_test token360_edges.json
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

#include "token_json.h"
#include "token_page.h"

namespace {

namespace tj = fortis::codec::tj;
namespace tf = fortis::tokens::token_facts;
namespace tp = fortis::tokens::token_page;

int g_failures = 0;
int g_checks = 0;
void check(bool condition, const char* what, const char* detail = "") {
  ++g_checks;
  if (condition) return;
  ++g_failures;
  fprintf(stderr, "FAIL: %s %s\n", what, detail);
}

char* read_file(const char* path, size_t* length) {
  FILE* file = fopen(path, "rb");
  if (!file) return nullptr;
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  char* data = static_cast<char*>(malloc(static_cast<size_t>(size) + 1));
  if (!data || fread(data, 1, static_cast<size_t>(size), file) != static_cast<size_t>(size)) {
    fclose(file);
    free(data);
    return nullptr;
  }
  fclose(file);
  data[size] = '\0';
  *length = static_cast<size_t>(size);
  return data;
}

void to_hex(const uint8_t* bytes, size_t length, char* out) {
  static const char kDigits[] = "0123456789abcdef";
  for (size_t i = 0; i < length; ++i) {
    out[2 * i] = kDigits[bytes[i] >> 4];
    out[2 * i + 1] = kDigits[bytes[i] & 15];
  }
  out[2 * length] = '\0';
}

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// "2026-01-02T03:04:05.000Z" → epoch ms (the stub timestamps are UTC ISO).
bool parse_iso(const tj::Value* text, int64_t* out) {
  int year, month, day, hour, minute, second, millis;
  if (!tj::IsString(text) ||
      !fortis::test::ParseTimestampParts(text->text, &year, &month, &day, &hour, &minute, &second, &millis))
    return false;
  *out = ((days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 24 + hour) * 60 + minute) *
             60000 + second * 1000 + millis;
  return true;
}

tj::Value* member(const tj::Value* object, const char* key) {
  tj::Value* value = tj::Get(object, key);
  return value && value->type != tj::Type::kNull ? value : nullptr;
}

// The stub TokenFacts as native Facts (the fields Token360Project reads).
bool build_facts(tj::Arena& a, const tj::Value* stub, tf::Facts* facts) {
  new (facts) tf::Facts();
  const tj::Value* network = tj::Get(stub, "network");
  const tj::Value* ref = tj::Get(stub, "ref");
  const tj::Value* id = tj::Get(network, "id");
  const tj::Value* token_id = tj::Get(ref, "tokenId");
  if (!tj::IsString(id) || !tj::IsString(token_id) || token_id->length >= sizeof(facts->ref.token_id)) return false;
  facts->network.id = tj::CString(a, id);
  memcpy(facts->ref.token_id, token_id->text, token_id->length + 1);
  const tj::Value* coin = tj::Get(tj::Get(stub, "resolution"), "coinId");
  if (tj::IsString(coin)) {
    if (coin->length >= sizeof(facts->resolution.coin_id)) return false;
    facts->resolution.has_coin = true;
    memcpy(facts->resolution.coin_id, coin->text, coin->length + 1);
  }
  facts->market = member(stub, "market");
  facts->profile = member(stub, "profile");
  facts->liquidity = member(stub, "liquidity");
  facts->holders = member(stub, "holders");
  facts->chain_meta = member(stub, "chainMeta");
  facts->insight = member(stub, "insight");
  facts->contract = member(stub, "contract");
  facts->safety = member(stub, "safety");
  facts->errors = tj::Get(stub, "errors");
  static const struct { const char* name; tf::Facet facet; } kCache[] = {
      {"market", tf::kMarket}, {"profile", tf::kProfile}, {"liquidity", tf::kLiquidity},
      {"holders", tf::kHolders}, {"chainMeta", tf::kChainMeta}, {"insight", tf::kInsight},
      {"contract", tf::kContract}};
  const tj::Value* cache = tj::Get(stub, "cache");
  for (const auto& entry : kCache) {
    const tj::Value* p = tj::Get(cache, entry.name);
    if (!tj::IsObject(p)) return false;
    tf::Provenance& provenance = facts->cache[entry.facet];
    provenance.hit = tj::Get(p, "hit") && tj::Get(p, "hit")->type == tj::Type::kTrue;
    provenance.partial = tj::Get(p, "partial") && tj::Get(p, "partial")->type == tj::Type::kTrue;
    provenance.has_timestamp = parse_iso(tj::Get(p, "timestamp"), &provenance.timestamp);
  }
  return true;
}

bool encode(const tj::Value* document, bool protobuf, uint8_t* out, size_t capacity, size_t* used) {
  tj::OutBuffer buffer{out, capacity, 0, true};
  const bool ok = protobuf ? tj::AppendStructuredReadResponse(document, buffer) : tj::Stringify(document, buffer);
  *used = buffer.used;
  return ok && buffer.ok;
}

constexpr size_t kBytes = 4 * 1024 * 1024;

void run_unit(const tj::Value* unit, uint8_t* out, char* hex) {
  const tj::Value* name = tj::Get(unit, "name");
  const tj::Value* facts_text = tj::Get(unit, "factsText");
  const tj::Value* response = tj::Get(unit, "responseText");
  const tj::Value* wire = tj::Get(unit, "wireHex");
  char label[160];
  snprintf(label, sizeof(label), "unit %s", tj::IsString(name) ? name->text : "?");
  tj::Arena a = tj::GrowingArena(16 * 1024 * 1024);
  const tj::Value* stub = tj::Parse(a, reinterpret_cast<const uint8_t*>(facts_text->text), facts_text->length);
  auto* facts = static_cast<tf::Facts*>(a.Alloc(sizeof(tf::Facts), alignof(tf::Facts)));
  tj::Value* document = nullptr;
  const bool built = stub && facts && build_facts(a, stub, facts);
  check(built, label, "stub facts");
  if (!built) { tj::FreeArena(a); return; }
  check(tp::Token360Project(a, *facts, &document) && document, label, "projection");
  size_t used = 0;
  const bool json_ok = document && encode(document, false, out, kBytes, &used);
  check(json_ok && tj::IsString(response) && used == response->length && memcmp(out, response->text, used) == 0,
        label, "JSON text");
  if (json_ok && used > 1) {
    size_t short_used = 0;
    check(!encode(document, false, out, used - 1, &short_used), label, "JSON one byte short is refused");
  }
  const tj::Value* json_wire = tj::Get(unit, "jsonWireHex");
  const bool wire_ok = document && encode(document, true, out, kBytes, &used);
  if (wire_ok) to_hex(out, used, hex);
  const bool same_json_wire = wire_ok && tj::IsString(json_wire) && used * 2 == json_wire->length &&
                              memcmp(hex, json_wire->text, json_wire->length) == 0;
  const bool same_live_wire = wire_ok && tj::IsString(wire) && used * 2 == wire->length &&
                              memcmp(hex, wire->text, wire->length) == 0;
  check(same_json_wire, label, "protobuf wire of the JSON document");
  // undefined_member: only the holders object without atLeast differs, by
  // TypeScript's empty `floor` Value (0a 09 0a 05 "floor" 12 00).
  const bool undefined_member = tj::IsString(name) && strcmp(name->text, "holders-without-at-least") == 0;
  check(undefined_member ? !same_live_wire && strstr(wire->text, "0a090a05666c6f6f721200") != nullptr
                         : same_live_wire,
        label, undefined_member ? "TypeScript's live wire has the empty floor Value" : "protobuf wire");
  if (wire_ok && used > 1) {
    size_t short_used = 0;
    check(!encode(document, true, out, used - 1, &short_used), label, "protobuf one byte short is refused");
  }
  tj::FreeArena(a);
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc != 2) {
    fprintf(stderr, "usage: %s token360_edges.json\n", argv[0]);
    return 2;
  }
  size_t length = 0;
  char* text = read_file(argv[1], &length);
  if (!text) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  tj::Arena arena = tj::GrowingArena(64 * 1024 * 1024);
  const tj::Value* fixture = tj::Parse(arena, reinterpret_cast<const uint8_t*>(text), length);
  const tj::Value* units = tj::Get(fixture, "units");
  if (!tj::IsArray(units)) {
    fprintf(stderr, "fixture without units/cases\n");
    return 2;
  }
  auto* out = static_cast<uint8_t*>(malloc(kBytes));
  auto* hex = static_cast<char*>(malloc(2 * kBytes + 1));
  for (uint32_t i = 0; i < units->count; ++i) run_unit(units->items[i], out, hex);
  printf("token360: %u units, %d checks, %d failures\n", units->count, g_checks, g_failures);
  free(out);
  free(hex);
  tj::FreeArena(arena);
  free(text);
  return g_failures ? 1 : 0;
} catch (...) {
  return fortis::test::ReportException();
}
