#include "native_config.h"
#include "strict_unit_expectations.h"
// R26 transaction-status units against tests/corpus/transaction_edges.json
// (written by the earlier TypeScript implementation's tests):
//  - chains::EvmTxStatusOf: EvmDriver.txStatus over recorded JSON-RPC answers
//    (`units.evm`): the TxDetail JSON byte for byte, or the thrown error —
//    RangeError (kRange, V8's message), SyntaxError from
//    Number(BigInt(String(blockNumber))) (kUpstream, V8's message),
//    TypeError/SyntaxError from the tip (kUpstream). A transport failure
//    ("<throw>") is the driver's call error, outside the pure function: the
//    socket replay covers it (counted here, not asserted);
//  - chains::SolanaTxStatusOf over getSignatureStatuses answers
//    (`units.solana`), including res.value?.[0] for a list, an object and a
//    string;
//  - address::TxRecordKey / TxPendingKey: txDetailRecordKey and txPendingKey
//    of (network as given, hash) spellings (`units.keys`);
//  - stored records (lmdb-msgpack): records are stored as their
//    JSON text. A cached answer the oracle recorded holds exactly what the
//    an earlier version stored (its msgpack copy: U+FFFD for lone
//    surrogates, "__proto_" keys); such a record must still parse and
//    serialize back byte for byte.
// Usage: transaction_test <transaction_edges.json>
#include "../src/wallet/address_facets.h"
#include "../src/chains/chain_core.h"
#include "../src/chains/chain_driver.h"
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

bool is_throw(const tj::Value* v) { return tj::IsString(v) && strcmp(v->text, "<throw>") == 0; }

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

// JSON.stringify of a native result (or "null"), copied into `arena`.
const char* stringify(tj::Arena& arena, const tj::Value* value) {
  if (!value) return "null";
  const size_t capacity = 4 * 1024 * 1024;
  uint8_t* scratch = static_cast<uint8_t*>(malloc(capacity));
  if (!scratch) return "(oom)";
  tj::OutBuffer buffer{scratch, capacity, 0, true};
  const bool ok = tj::Stringify(value, buffer) && buffer.ok;
  const char* out = ok ? arena.CopyText(reinterpret_cast<const char*>(scratch), buffer.used) : nullptr;
  free(scratch);
  return out ? out : "(unserializable)";
}

// The expected outcome against the native one: a result's JSON text, or the
// error class (RangeError: kRange and V8's message; SyntaxError/TypeError:
// kUpstream, the message compared where native writes V8's).
void expect(const char* group, const tj::Value* row, bool ok, const tj::Value* result, const chains::Error& error,
            tj::Arena& arena) {
  const char* name = text(row, "name");
  const char* want = text(row, "result");
  const tj::Value* want_null = tj::Get(row, "result");
  char detail[3000];
  if (want || (want_null && want_null->type == tj::Type::kNull)) {
    const char* got = ok ? stringify(arena, result) : nullptr;
    const char* wanted = want ? want : "null";
    if (!ok || strcmp(got, wanted) != 0) {
      snprintf(detail, sizeof(detail), "%s: %.1200s\n  expected %.1200s", name, ok ? got : error.message, wanted);
      fail(group, detail);
    }
    return;
  }
  const char* kind = text(row, "error");
  const char* message = text(row, "message");
  const bool range = kind && strcmp(kind, "RangeError") == 0;
  bool good = !ok && (range ? error.kind == chains::ErrorKind::kRange : error.kind == chains::ErrorKind::kUpstream);
  if (good && range) good = strcmp(error.message, message) == 0;
  if (good && !range && strncmp(error.message, "Cannot convert ", 15) == 0) good = strcmp(error.message, message) == 0;
  if (!good) {
    snprintf(detail, sizeof(detail), "%s: %s kind %d \"%s\", expected %s \"%s\"", name, ok ? "ok" : "error",
             static_cast<int>(error.kind), error.message, kind, message);
    fail(group, detail);
  }
}

void evm(const tj::Value* rows, chains::Catalog& catalog) {
  size_t checked = 0, transport = 0;
  for (uint32_t i = 0; i < rows->count; ++i) {
    const tj::Value* row = rows->items[i];
    const tj::Value* answers = tj::Get(row, "answers");
    const char* network_id = text(row, "network");
    chains::Network network;
    if (!catalog.Resolve(network_id, strlen(network_id), &network)) {
      fail("evm", network_id);
      continue;
    }
    tj::Arena arena = tj::GrowingArena(16 * 1024 * 1024);
    tj::Value* tx = tj::Clone(arena, tj::Get(answers, "eth_getTransactionByHash"));
    const tj::Value* receipt = tj::Get(answers, "eth_getTransactionReceipt");
    const tj::Value* tip = tj::Get(answers, "eth_blockNumber");
    const tj::Value* block = tj::Get(tx, "blockNumber");
    const bool mined = tx && tx->type != tj::Type::kNull && block && block->type != tj::Type::kNull;
    if (is_throw(tx) || (mined && (is_throw(receipt) || is_throw(tip)))) {
      // The call's transport error (an Error, kUpstream): driver orchestration.
      if (!text(row, "error") || strcmp(text(row, "error"), "Error") != 0) fail("evm", text(row, "name"));
      ++transport;
      tj::FreeArena(arena);
      continue;
    }
    chains::Error error;
    tj::Value* result = nullptr;
    bool ok = true;
    if (tx && tx->type != tj::Type::kNull)
      ok = chains::EvmTxStatusOf(arena, network, tx, tj::Clone(arena, receipt), tip, &result, &error);
    expect("evm", row, ok, result, error, arena);
    tj::FreeArena(arena);
    ++checked;
  }
  printf("evm txStatus: %zu checked, %zu transport failures left to the replay\n", checked, transport);
}

void solana(const tj::Value* rows) {
  size_t checked = 0, transport = 0;
  for (uint32_t i = 0; i < rows->count; ++i) {
    const tj::Value* row = rows->items[i];
    const tj::Value* answer = tj::Get(tj::Get(row, "answers"), "getSignatureStatuses");
    if (is_throw(answer)) {
      if (!text(row, "error") || strcmp(text(row, "error"), "Error") != 0) fail("solana", text(row, "name"));
      ++transport;
      continue;
    }
    tj::Arena arena = tj::GrowingArena(4 * 1024 * 1024);
    chains::Error error;
    tj::Value* result = nullptr;
    const bool ok = chains::SolanaTxStatusOf(arena, tj::Clone(arena, answer), &result, &error);
    expect("solana", row, ok, result, error, arena);
    tj::FreeArena(arena);
    ++checked;
  }
  printf("solana txStatus: %zu checked, %zu transport failures left to the replay\n", checked, transport);
}

void keys(const tj::Value* rows, double poll_until) {
  size_t checked = 0;
  for (uint32_t i = 0; i < rows->count; ++i) {
    const tj::Value* row = rows->items[i];
    const char* network = text(row, "network");
    const char* hash = text(row, "hash");
    char record[address::kTxKeyBytes], pending[address::kTxKeyBytes];
    const bool ok = address::TxRecordKey(network, hash, record, sizeof(record)) &&
                    address::TxPendingKey(static_cast<int64_t>(poll_until), network, hash, pending, sizeof(pending));
    if (!ok || strcmp(record, text(row, "record")) != 0 || strcmp(pending, text(row, "pending")) != 0) {
      char detail[9000];
      snprintf(detail, sizeof(detail), "%s %.300s: %s | %s", network, hash, ok ? record : "(failed)", ok ? pending : "");
      fail("keys", detail);
    }
    ++checked;
  }
  printf("record/pending keys: %zu checked\n", checked);
}

const tj::Value* find_case(const tj::Value* cases, const char* group, const char* name) {
  for (uint32_t i = 0; i < cases->count; ++i) {
    const char* g = text(cases->items[i], "group");
    const char* n = text(cases->items[i], "name");
    if (g && n && strcmp(g, group) == 0 && strcmp(n, name) == 0) return cases->items[i];
  }
  return nullptr;
}

// Each `{name}-again` JSON read answered from the record (cache.hit true):
// its detail and raw are records an earlier version wrote, still read as
// stored.
void stored_copies(const tj::Value* cases) {
  size_t checked = 0;
  for (uint32_t i = 0; i < cases->count; ++i) {
    const tj::Value* again = cases->items[i];
    const char* name = text(again, "name");
    const char* body = text(again, "responseText");
    const size_t length = name ? strlen(name) : 0;
    if (!body || length < 7 || strcmp(name + length - 6, "-again") != 0) continue;
    char live_name[200];
    snprintf(live_name, sizeof(live_name), "%.*s", static_cast<int>(length - 6), name);
    const tj::Value* live = find_case(cases, text(again, "group"), live_name);
    const char* live_body = live ? text(live, "responseText") : nullptr;
    if (!live_body) continue;
    tj::Arena arena = tj::GrowingArena(16 * 1024 * 1024);
    const tj::Value* cached = tj::Parse(arena, reinterpret_cast<const uint8_t*>(body), strlen(body));
    const tj::Value* fresh = tj::Parse(arena, reinterpret_cast<const uint8_t*>(live_body), strlen(live_body));
    const tj::Value* hit = tj::Get(tj::Get(cached, "cache"), "hit");
    if (!cached || !fresh || !hit || hit->type != tj::Type::kTrue) {
      tj::FreeArena(arena);
      continue;
    }
    static const char* const kFields[] = {"detail", "raw"};
    for (const char* field : kFields) {
      const char* want = stringify(arena, tj::Get(cached, field));
      const tj::Value* reread = tj::Parse(arena, reinterpret_cast<const uint8_t*>(want), strlen(want));
      const char* got = reread ? stringify(arena, reread) : "<unreadable>";
      if (strcmp(want, got) != 0) {
        char detail[3000];
        snprintf(detail, sizeof(detail), "%s.%s: %.1200s\n  expected %.1200s", name, field, got, want);
        fail("stored record", detail);
      }
    }
    tj::FreeArena(arena);
    ++checked;
  }
  if (checked < 5) fail("stored record", "fewer than 5 live/cached pairs in the fixture");
  printf("records stored before task 17: %zu cached answers read back as stored\n", checked);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <transaction_edges.json>\n", argv[0]);
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
  if (!strict::ContractExpectations(arena, fixture, "transaction_edges.json")) return 2;
  const tj::Value* units = tj::Get(fixture, "units");
  const tj::Value* poll_until = tj::Get(units, "pollUntil");
  if (!units || !tj::IsNumber(poll_until)) {
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
  evm(tj::Get(units, "evm"), *catalog);
  solana(tj::Get(units, "solana"));
  keys(tj::Get(units, "keys"), poll_until->number);
  stored_copies(tj::Get(fixture, "cases"));
  catalog->~Catalog();
  free(catalog);
  tj::FreeArena(arena);
  free(data);
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf("transaction units: all passed\n");
  return 0;
}
