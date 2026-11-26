// R17 transfer history units against tests/corpus/address_transfers_edges.json
// (`units`, written by the earlier TypeScript implementation's tests):
//  - address::ParseTransfersQuery: the route's zod transfersQuery over
//    fast-querystring's parse, for 116 raw query strings (validity, then every
//    parsed field: since, before, refresh, limit, token, the trimmed and
//    lowercased priceId, from, to, flow);
//  - address::DecodeDisplayCursor: decodeDisplayCursor (Buffer base64url,
//    JSON.parse, Date.parse) for 42 cursors, and the page boundary the facet
//    builds from it (pad13(ts)/encodeURIComponent(id), or the URIError a lone
//    surrogate throws);
//  - address::EncodeDisplayCursor and the boundary of fixed (ms, id) pairs,
//    including negative and extreme instants.
// Usage: address_transfers_test <address_transfers_edges.json>
#include "strict_unit_expectations.h"
#include "../src/wallet/address_records.h"
#include "../src/wallet/address_service.h"
#include "../src/wallet/address_store.h"
#include "../src/chains/chain_core.h"
#include "../src/codec/token_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace fortis::wallet;

using namespace fortis;
namespace tj = fortis::codec::tj;

namespace {

int failures = 0;

void fail(const char* what, const char* detail) {
  ++failures;
  fprintf(stderr, "FAIL: %s: %.400s\n", what, detail);
}

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

bool same_string(const tj::Value* expected, bool present, const char* actual, size_t actual_length) {
  if (!expected) return !present;
  return present && tj::IsString(expected) && expected->length == actual_length &&
         memcmp(expected->text, actual, actual_length) == 0;
}

bool same_number(const tj::Value* expected, bool present, double actual) {
  if (!expected) return !present;
  // JSON carries no -0 (z.coerce.number() keeps "-0" as -0; the replay's
  // protobuf window case checks that sign on the wire).
  return present && tj::IsNumber(expected) && expected->number == actual;
}

// pad13(ms)/encodeURIComponent(id): false for a lone surrogate (URIError).
bool boundary(int64_t ms, const char* id, size_t id_length, char* out, size_t capacity) {
  char pad[24];
  address::Store::Pad13(ms, pad);
  const size_t pad_length = strlen(pad);
  if (pad_length + 2 >= capacity) return false;
  memcpy(out, pad, pad_length);
  out[pad_length] = '/';
  tj::OutBuffer encoded{reinterpret_cast<uint8_t*>(out + pad_length + 1), capacity - pad_length - 2, 0, true};
  if (!chains::EncodeUriComponent(id, id_length, encoded) || !encoded.ok) return false;
  out[pad_length + 1 + encoded.used] = '\0';
  return true;
}

size_t queries(const tj::Value* rows) {
  size_t checked = 0;
  static char target[65536];
  for (uint32_t i = 0; tj::IsArray(rows) && i < rows->count; ++i) {
    const tj::Value* row = rows->items[i];
    const char* query = text(row, "query");
    const tj::Value* valid = tj::Get(row, "valid");
    if (!query || !valid) { fail("query row", "malformed"); continue; }
    snprintf(target, sizeof(target), "/v1/networks/eth-mainnet/address/0x1/transfers?%s", query);
    auto* input = static_cast<address::TransfersQueryInput*>(calloc(1, sizeof(address::TransfersQueryInput)));
    if (!input) { fail("query", "out of memory"); return checked; }
    const bool parsed = address::ParseTransfersQuery(target, input);
    ++checked;
    if (parsed != (valid->type == tj::Type::kTrue)) {
      fail(parsed ? "query accepted, zod refuses" : "query refused, zod accepts", query);
      free(input);
      continue;
    }
    if (!parsed) { free(input); continue; }
    const tj::Value* value = tj::Get(row, "value");
    const tj::Value* refresh = tj::Get(value, "refresh");
    const tj::Value* flow = tj::Get(value, "flow");
    static const char* const kFlows[] = {"in", "out", "neutral"};
    const char* flow_text = input->flow >= 0 ? kFlows[input->flow] : nullptr;
    bool ok = same_string(tj::Get(value, "since"), input->has_since, input->since, input->since_length) &&
              same_string(tj::Get(value, "before"), input->has_before, input->before, input->before_length) &&
              ((refresh && refresh->type == tj::Type::kTrue) == input->refresh) &&
              same_number(tj::Get(value, "limit"), true, input->limit) &&
              same_string(tj::Get(value, "token"), input->has_token, input->token, input->token_length) &&
              same_string(tj::Get(value, "priceId"), input->has_price, input->price_id, strlen(input->price_id)) &&
              same_number(tj::Get(value, "from"), input->has_from, input->from) &&
              same_number(tj::Get(value, "to"), input->has_to, input->to) &&
              same_string(flow, flow_text != nullptr, flow_text ? flow_text : "", flow_text ? strlen(flow_text) : 0);
    if (!ok) fail("query fields", query);
    free(input);
  }
  return checked;
}

size_t cursors(const tj::Value* rows) {
  size_t checked = 0;
  for (uint32_t i = 0; tj::IsArray(rows) && i < rows->count; ++i) {
    const tj::Value* row = rows->items[i];
    const tj::Value* raw = tj::Get(row, "raw");
    const tj::Value* valid = tj::Get(row, "valid");
    if (!tj::IsString(raw) || !valid) { fail("cursor row", "malformed"); continue; }
    uint8_t bytes[65536];
    tj::Arena arena{bytes, sizeof(bytes), 0, true};
    int64_t ts = 0;
    const char* id = nullptr;
    size_t id_length = 0;
    const bool decoded = address::DecodeDisplayCursor(arena, raw->text, raw->length, &ts, &id, &id_length);
    ++checked;
    if (decoded != (valid->type == tj::Type::kTrue)) {
      fail(decoded ? "cursor accepted, TypeScript refuses" : "cursor refused, TypeScript accepts", raw->text);
      continue;
    }
    if (!decoded) continue;
    const tj::Value* ts_ms = tj::Get(row, "tsMs");
    const tj::Value* expected_id = tj::Get(row, "id");
    if (!tj::IsNumber(ts_ms) || static_cast<double>(ts) != ts_ms->number) fail("cursor ts", raw->text);
    if (!same_string(expected_id, true, id, id_length)) fail("cursor id", raw->text);
    char built[8192];
    const bool encoded = boundary(ts, id, id_length, built, sizeof(built));
    const char* expected_boundary = text(row, "boundary");
    if (text(row, "boundaryError")) {
      if (!encoded || strcmp(built, "1792929600000/%EF%BF%BD") != 0) fail("cursor boundary: normalized Unicode expected", raw->text);
    } else if (!encoded || !expected_boundary || strcmp(built, expected_boundary) != 0) {
      fail("cursor boundary", built);
    }
  }
  return checked;
}

size_t encodes(const tj::Value* rows) {
  size_t checked = 0;
  for (uint32_t i = 0; tj::IsArray(rows) && i < rows->count; ++i) {
    const tj::Value* row = rows->items[i];
    const tj::Value* ms = tj::Get(row, "ms");
    const char* id = text(row, "id");
    const char* expected = text(row, "encoded");
    char encoded[4096];
    ++checked;
    if (!tj::IsNumber(ms) || !id || !expected ||
        !address::EncodeDisplayCursor(static_cast<int64_t>(ms->number), id, encoded, sizeof(encoded)) ||
        strcmp(encoded, expected) != 0)
      fail("encodeDisplayCursor", expected ? expected : "?");
  }
  return checked;
}

size_t boundaries(const tj::Value* rows) {
  size_t checked = 0;
  for (uint32_t i = 0; tj::IsArray(rows) && i < rows->count; ++i) {
    const tj::Value* row = rows->items[i];
    const tj::Value* ms = tj::Get(row, "ms");
    const tj::Value* id = tj::Get(row, "id");
    const char* expected = text(row, "boundary");
    char built[4096];
    ++checked;
    if (!tj::IsNumber(ms) || !tj::IsString(id) || !expected ||
        !boundary(static_cast<int64_t>(ms->number), id->text, id->length, built, sizeof(built)) ||
        strcmp(built, expected) != 0)
      fail("transferTimeBoundary", expected ? expected : "?");
  }
  return checked;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <address_transfers_edges.json>\n", argv[0]);
    return 2;
  }
  size_t length = 0;
  char* data = read_file(argv[1], &length);
  if (!data) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  tj::Arena arena = tj::GrowingArena(512 * 1024 * 1024);
  const tj::Value* fixture = tj::Parse(arena, reinterpret_cast<const uint8_t*>(data), length);
  strict::PatchRows(arena, tj::Get(tj::Get(fixture, "units"), "query"), tj::Get(strict::UnitExpectations(arena), "address"), "query");
  const tj::Value* units = tj::Get(fixture, "units");
  if (!tj::IsObject(units)) {
    fprintf(stderr, "no units in %s\n", argv[1]);
    return 2;
  }
  const size_t q = queries(tj::Get(units, "query"));
  const size_t c = cursors(tj::Get(units, "cursors"));
  const size_t e = encodes(tj::Get(units, "encodes"));
  const size_t b = boundaries(tj::Get(units, "boundaries"));
  printf("queries %zu, cursors %zu, encodes %zu, boundaries %zu: %s\n", q, c, e, b,
         failures ? "FAILED" : "all passed");
  tj::FreeArena(arena);
  free(data);
  return failures ? 1 : 0;
}
