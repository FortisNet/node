#include <vector>
#include "test_support.h"
// R13 balances units against tests/corpus/address_balances_edges.json
// (`units`, written by the earlier TypeScript implementation's tests):
//  - BalancesCacheKey: the TypeScript cacheKey, i.e. sha1 of
//    [...tokens].sort().join(',') — Array.prototype.sort compares UTF-16
//    code units, so astral characters sort before U+E000..U+FFFF;
//  - SplitTrackedTokens: value.split(',').map(t => t.trim()).filter(Boolean)
//    with String.prototype.trim's whitespace set;
//  - Store::PutBalance: BalanceStore.put's merge (previous tokens by id,
//    requested-but-omitted ids removed, incoming entries set in place),
//    replayed step by step on one account and compared with every snapshot.
// Usage: address_balances_test <address_balances_edges.json>
#include "../src/wallet/address_facets.h"
#include "../src/wallet/address_store.h"
#include "../src/storage/kv_store.h"
#include "../src/codec/token_json.h"
#include "../src/compat/token_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

using namespace fortis::compat;
using namespace fortis::wallet;

using namespace fortis::storage;

using namespace fortis;
namespace tj = fortis::codec::tj;

namespace {

int failures = 0;

void fail(const char* what, const char* detail) {
  ++failures;
  fprintf(stderr, "FAIL: %s: %s\n", what, detail);
}

const tj::Value* member(const tj::Value* object, const char* key) { return tj::Get(object, key); }

bool stringify(const tj::Value* value, char* out, size_t capacity) {
  tj::OutBuffer buffer{reinterpret_cast<uint8_t*>(out), capacity - 1, 0, true};
  if (!tj::Stringify(value, buffer) || !buffer.ok) return false;
  out[buffer.used] = '\0';
  return true;
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

void cache_keys(const tj::Value* keys) {
  size_t checked = 0;
  for (uint32_t i = 0; i < keys->count; ++i) {
    const tj::Value* row = keys->items[i];
    const tj::Value* tokens = member(row, "tokens");
    std::vector<const char*> list(tokens->count + 1);
    for (uint32_t t = 0; t < tokens->count; ++t) list[t] = tokens->items[t]->text;
    char key[1024];
    const bool ok = address::BalancesCacheKey(member(row, "network")->text, member(row, "address")->text, list.data(),
                                              tokens->count, key, sizeof(key));
    if (!ok || strcmp(key, member(row, "key")->text) != 0) {
      char detail[2200];
      snprintf(detail, sizeof(detail), "case %u: %s != %s", i, ok ? key : "(failed)", member(row, "key")->text);
      fail("cache key", detail);
    }
    ++checked;
  }
  printf("cache keys: %zu checked\n", checked);
}

void trims(const tj::Value* rows) {
  for (uint32_t i = 0; i < rows->count; ++i) {
    const tj::Value* value = member(rows->items[i], "value");
    const tj::Value* trimmed = member(rows->items[i], "trimmed");
    size_t b = 0, e = 0;
    token_text::Trim(value->text, value->length, &b, &e);
    if (e - b != trimmed->length || memcmp(value->text + b, trimmed->text, trimmed->length) != 0) {
      char detail[128];
      snprintf(detail, sizeof(detail), "case %u (%u bytes): kept [%zu, %zu)", i, value->length, b, e);
      fail("trim", detail);
    }
  }
  printf("trims: %u checked\n", rows->count);
}

void splits(const tj::Value* rows) {
  for (uint32_t i = 0; i < rows->count; ++i) {
    const tj::Value* value = member(rows->items[i], "value");
    const tj::Value* want = member(rows->items[i], "tokens");
    std::unique_ptr<char*[], fortis::base::CStringArrayDeleter> items;
    size_t count = 0;
    const bool ok = address::SplitTrackedTokens(value->text, value->length, items, &count);
    bool same = ok && count == want->count;
    for (size_t t = 0; same && t < count; ++t) same = strcmp(items[t], want->items[t]->text) == 0;
    if (!same) {
      char detail[96];
      snprintf(detail, sizeof(detail), "case %u: %zu tokens, expected %u", i, count, want->count);
      fail("split", detail);
    }

  }
  printf("splits: %u checked\n", rows->count);
}

void merges(const tj::Value* steps) {
  char directory[] = "/tmp/fortis-balances-unit-XXXXXX";
  if (!mkdtemp(directory)) { fail("merge", "mkdtemp"); return; }
  KvStore kv;
  if (!kv.Open(directory)) { fail("merge", "KvStore::Open"); return; }
  address::Store store(&kv);
  const char* account = "01ARZ3NDEKTSV4RRFFQ69G5FZZ";
  char* got = static_cast<char*>(malloc(1 << 20));
  char* want = static_cast<char*>(malloc(1 << 20));
  for (uint32_t i = 0; i < steps->count; ++i) {
    tj::Arena arena = tj::GrowingArena(4 * 1024 * 1024);
    const tj::Value* step = steps->items[i];
    const tj::Value* requested = member(step, "requested");
    std::vector<const char*> list(requested->count + 1);
    for (uint32_t r = 0; r < requested->count; ++r) list[r] = requested->items[r]->text;
    const tj::Value* snapshot = member(step, "snapshot");
    const int64_t as_of = static_cast<int64_t>(member(snapshot, "asOf")->number);
    if (!store.PutBalance(account, member(step, "incoming"), list.data(), requested->count, as_of, arena)) {
      fail("merge", "PutBalance failed");
    } else {
      tj::Value* payload = nullptr;
      int64_t stored_at = 0;
      if (store.GetBalance(account, as_of, arena, &payload, &stored_at) != address::ReadStatus::kFound ||
          !stringify(payload, got, 1 << 20) || !stringify(member(snapshot, "payload"), want, 1 << 20) ||
          strcmp(got, want) != 0 || stored_at != as_of) {
        char detail[4200];
        snprintf(detail, sizeof(detail), "step %u\n  expected %.2000s\n  actual   %.2000s", i, want, got);
        fail("merge", detail);
      }
    }
    tj::FreeArena(arena);
  }
  free(got);
  free(want);
  printf("merges: %u steps checked\n", steps->count);
  fortis::test::RemoveTree(directory);
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <address_balances_edges.json>\n", argv[0]);
    return 2;
  }
  size_t length = 0;
  char* text = read_file(argv[1], &length);
  if (!text) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
  tj::Arena arena = tj::GrowingArena(256 * 1024 * 1024);
  const tj::Value* fixture = tj::Parse(arena, reinterpret_cast<const uint8_t*>(text), length);
  const tj::Value* units = member(fixture, "units");
  if (!tj::IsObject(units)) { fprintf(stderr, "fixture has no units\n"); return 2; }
  cache_keys(member(units, "keys"));
  trims(member(units, "trims"));
  splits(member(units, "splits"));
  merges(member(units, "merges"));
  tj::FreeArena(arena);
  free(text);
  if (failures) {
    fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  printf("address balances units: all checks passed\n");
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
