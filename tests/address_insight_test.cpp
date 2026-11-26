#include "native_config.h"
// R14 insight units against tests/corpus/address_insight_edges.json
// (`units`, written by the earlier TypeScript implementation's tests):
//  - chains::BigIntNumber: the EVM driver's txCount, Number(BigInt(nonce)),
//    for strings (StringToBigInt, no bit limit), numbers, booleans, arrays
//    and objects: the double (as String(n), never -0) or the error class —
//    RangeError (kRange, V8's message), TypeError/SyntaxError (kUpstream);
//  - address::InsightCacheKey: ins:{registry.network(id).id}:
//    {canonicalAddress} through chains::Catalog::Resolve;
//  - chains::FirstSettledFailure: Promise.all rejects with the call that
//    failed first, whatever its index.
// Usage: address_insight_test <address_insight_edges.json>
#include "../src/wallet/address_facets.h"
#include "../src/chains/chain_core.h"
#include "../src/chains/chain_transport.h"
#include "../src/config/native_config.h"
#include "../src/codec/token_json.h"

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

void fail(const char* what, const char* detail) {
  ++failures;
  fprintf(stderr, "FAIL: %s: %s\n", what, detail);
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

void nonces(const tj::Value* rows) {
  size_t checked = 0;
  for (uint32_t i = 0; i < rows->count; ++i) {
    const tj::Value* row = rows->items[i];
    const char* json = text(row, "json");
    tj::Arena arena = tj::GrowingArena(4 * 1024 * 1024);
    const tj::Value* value = tj::Parse(arena, reinterpret_cast<const uint8_t*>(json), strlen(json));
    double number = 0;
    chains::Error error;
    const bool ok = value && chains::BigIntNumber(arena, value, &number, &error);
    char detail[1400];
    const char* expected_error = text(row, "error");
    if (!expected_error) {
      char got[64] = "";
      if (ok) chains::JsNumberText(number, got, sizeof(got));
      const bool negative_zero = ok && number == 0 && __builtin_signbit(number);
      if (!ok || strcmp(got, text(row, "number")) != 0 || negative_zero) {
        snprintf(detail, sizeof(detail), "%.80s: %s (%s) != %s", json, ok ? got : "error",
                 negative_zero ? "-0" : error.message, text(row, "number"));
        fail("nonce", detail);
      }
    } else {
      const bool range = strcmp(expected_error, "RangeError") == 0;
      const bool kind_ok = !ok && (range ? error.kind == chains::ErrorKind::kRange
                                         : error.kind == chains::ErrorKind::kUpstream);
      if (!kind_ok || (range && strcmp(error.message, text(row, "message")) != 0)) {
        snprintf(detail, sizeof(detail), "%.80s: %s kind %d \"%s\", expected %s \"%s\"", json, ok ? "ok" : "error",
                 static_cast<int>(error.kind), error.message, expected_error, text(row, "message"));
        fail("nonce", detail);
      }
    }
    tj::FreeArena(arena);
    ++checked;
  }
  printf("nonces: %zu checked\n", checked);
}

void keys(const tj::Value* rows) {
  NativeConfig config{};
  chains::Catalog* catalog = static_cast<chains::Catalog*>(calloc(1, sizeof(chains::Catalog)));
  new (catalog) chains::Catalog();
  if (!catalog->Init(config)) {
    fail("catalog", "Init failed");
    return;
  }
  size_t checked = 0;
  for (uint32_t i = 0; i < rows->count; ++i) {
    const tj::Value* row = rows->items[i];
    const char* network_id = text(row, "network");
    chains::Network network;
    tj::Arena arena = tj::GrowingArena(4 * 1024 * 1024);
    char key[1024] = "";
    const bool ok = catalog->Resolve(network_id, strlen(network_id), &network) &&
                    address::InsightCacheKey(arena, network, text(row, "address"), key, sizeof(key));
    if (!ok || strcmp(key, text(row, "key")) != 0) {
      char detail[2400];
      snprintf(detail, sizeof(detail), "%s %s: %s != %s", network_id, text(row, "address"), ok ? key : "(failed)",
               text(row, "key"));
      fail("cache key", detail);
    }
    tj::FreeArena(arena);
    ++checked;
  }
  catalog->~Catalog();
  free(catalog);
  printf("cache keys: %zu checked\n", checked);
}

void first_failure() {
  chains::Call calls[3];
  calls[0].failure = chains::CallFailure::kTransport;
  calls[0].settled_rank = 3;
  calls[1].failure = chains::CallFailure::kNone;
  calls[1].settled_rank = 1;
  calls[2].failure = chains::CallFailure::kLaneBusy;
  calls[2].settled_rank = 2;
  if (chains::FirstSettledFailure(calls, 3) != &calls[2]) fail("first failure", "not the earliest failed call");
  calls[2].failure = chains::CallFailure::kNone;
  if (chains::FirstSettledFailure(calls, 3) != &calls[0]) fail("first failure", "the only failure was skipped");
  calls[0].failure = chains::CallFailure::kNone;
  if (chains::FirstSettledFailure(calls, 3) != nullptr) fail("first failure", "a failure without one");
  printf("first settled failure: 3 checked\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <address_insight_edges.json>\n", argv[0]);
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
  const tj::Value* units = tj::Get(fixture, "units");
  if (!units) {
    fprintf(stderr, "no units in %s\n", argv[1]);
    return 2;
  }
  nonces(tj::Get(units, "nonces"));
  keys(tj::Get(units, "keys"));
  first_failure();
  tj::FreeArena(arena);
  free(data);
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf("address insight units: all passed\n");
  return 0;
}
