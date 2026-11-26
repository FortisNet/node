#include <memory>
#include "test_support.h"
// Replays tests/corpus/chain_core_corpus.json (written by
// the earlier TypeScript implementation's tests) against the
// shared helpers in chain_core.cpp and address_records.cpp.
#include "address_records.h"
#include "chain_core.h"
#include "token_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace fortis::chains;

namespace tj = fortis::codec::tj;
namespace chains = fortis::chains;
using tj::Value;

namespace {

int failures = 0;
int checked = 0;

void fail(const char* group, const char* detail, const char* expected, const char* actual) {
  ++failures;
  if (failures <= 40) fprintf(stderr, "%s %s: expected %s got %s\n", group, detail, expected, actual);
}

bool same(const char* a, const char* b) { return (!a && !b) || (a && b && strcmp(a, b) == 0); }

const char* text_of(const Value* v) { return tj::IsString(v) ? v->text : nullptr; }

void expect_text(const char* group, const char* detail, const Value* expected, const char* actual) {
  ++checked;
  const char* want = tj::IsString(expected) ? expected->text : nullptr;
  if (!same(want, actual)) fail(group, detail, want ? want : "null", actual ? actual : "null");
}

// The JSON text of a value (for messages and structural comparisons).
const char* json(tj::Arena& arena, const Value* v) {
  uint8_t* buffer = static_cast<uint8_t*>(arena.Alloc(1 << 16, 1));
  tj::OutBuffer out{buffer, 1 << 16, 0, true};
  if (!v || !tj::Stringify(v, out) || !out.ok) return "?";
  buffer[out.used] = '\0';
  return reinterpret_cast<const char*>(buffer);
}

// The growing arena behind the address task arenas (token_json.h).
void check_growing_arena() {
  auto check = [](bool ok, const char* detail) {
    if (!ok) fail("growingArena", detail, "true", "false");
  };
  tj::Arena arena = tj::GrowingArena(3 * tj::kArenaBlockBytes);
  check(arena.base == nullptr && arena.held == 0, "no block before the first Alloc");
  const tj::Mark empty = tj::MarkOf(arena);
  char* first = static_cast<char*>(arena.Alloc(100, 1));
  check(first && arena.held == tj::kArenaBlockBytes, "first Alloc chains one block");
  if (!first) { tj::FreeArena(arena); return; }
  memset(first, 'a', 100);
  const tj::Mark one = tj::MarkOf(arena);
  // Fill past the first block: a second block is chained.
  void* big = arena.Alloc(tj::kArenaBlockBytes - 64, 8);
  check(big && arena.held > tj::kArenaBlockBytes && arena.base != one.base, "overflow chains a second block");
  check(first[99] == 'a', "earlier allocations stay valid");
  // An allocation larger than a block gets a block of its own size.
  tj::Rewind(arena, one);
  check(arena.held == tj::kArenaBlockBytes && arena.ok, "rewind frees the blocks chained after the mark");
  void* oversized = arena.Alloc(tj::kArenaBlockBytes + 4096, 8);
  check(oversized && arena.held > 2 * tj::kArenaBlockBytes, "an oversized request gets its own block");
  // Past the limit the arena fails like a full fixed arena.
  void* refused = arena.Alloc(tj::kArenaBlockBytes, 8);
  check(!refused && !arena.ok, "a request past the limit fails");
  tj::Rewind(arena, empty);
  check(arena.base == nullptr && arena.held == 0, "rewind to the empty mark frees every block");
  tj::FreeArena(arena);
  check(arena.base == nullptr && arena.held == 0, "free of an empty arena is a no-op");
  tj::Arena again = tj::GrowingArena(2 * tj::kArenaBlockBytes);
  for (int i = 0; i < 3000; ++i) check(tj::Str(again, "0123456789abcdef0123456789abcdef") != nullptr || i > 2000, "many small");
  check(again.held <= 2 * tj::kArenaBlockBytes, "held stays within the limit");
  tj::FreeArena(again);
  check(again.base == nullptr && again.held == 0, "free releases every block");
  // A fixed arena keeps its old behaviour (no growth).
  uint8_t fixed_memory[64];
  tj::Arena fixed{fixed_memory, sizeof(fixed_memory), 0, true};
  check(fixed.Alloc(48, 8) && !fixed.Alloc(32, 8) && !fixed.ok, "a fixed arena never grows");
  tj::FreeArena(fixed);
}

}  // namespace


int main(int argc, char** argv) try {
  if (argc != 2) {
    fprintf(stderr, "usage: chain_core_test <chain_core_corpus.json>\n");
    return 2;
  }
  std::vector<uint8_t> fixture_bytes;
  if (!fortis::test::ReadFixture(argv[1], fixture_bytes)) return 2;
  const size_t size = fixture_bytes.size();
  uint8_t* data = fixture_bytes.data();
  const size_t arena_bytes = 64 * 1024 * 1024;
  auto memory_owner = std::make_unique_for_overwrite<uint8_t[]>(arena_bytes);
  uint8_t* memory = memory_owner.get();
  tj::Arena arena{memory, arena_bytes, 0, true};
  Value* corpus = tj::Parse(arena, data, static_cast<size_t>(size));
  if (!tj::IsObject(corpus)) return 2;
  char buffer[4096];

  // decimalToRaw
  const Value* cases = tj::Get(corpus, "decimalToRaw");
  for (uint32_t i = 0; i < cases->count; ++i) {
    const Value* c = cases->items[i];
    const Value* value = tj::Get(c, "value");
    const Value* fixed = tj::Get(c, "fixed");
    const Value* result = tj::Get(c, "result");
    chains::BigInt raw;
    int decimals = 0;
    const bool ok = chains::DecimalToRaw(value->text, value->length, tj::IsNumber(fixed) ? static_cast<int>(fixed->number) : -1,
                                        &raw, &decimals);
    char actual[600] = "null";
    if (ok) {
      char digits[400];
      raw.ToDecimal(digits, sizeof(digits));
      snprintf(actual, sizeof(actual), "%s/%d", digits, decimals);
    }
    char expected[600] = "null";
    if (tj::IsObject(result))
      snprintf(expected, sizeof(expected), "%s/%d", text_of(tj::Get(result, "raw")),
               static_cast<int>(tj::Get(result, "decimals")->number));
    ++checked;
    snprintf(buffer, sizeof(buffer), "%s fixed=%s", value->text, json(arena, fixed));
    if (strcmp(actual, expected) != 0) fail("decimalToRaw", buffer, expected, actual);
  }
  // rawToDecimalString
  cases = tj::Get(corpus, "rawToDecimal");
  for (uint32_t i = 0; i < cases->count; ++i) {
    const Value* c = cases->items[i];
    const Value* raw = tj::Get(c, "raw");
    const int decimals = static_cast<int>(tj::Get(c, "decimals")->number);
    chains::BigInt parsed;
    const bool parses = chains::BigInt::Parse(raw->text, raw->length, &parsed);
    Value* out = parses ? chains::RawToDecimalString(arena, parsed, decimals) : tj::Null(arena);
    snprintf(buffer, sizeof(buffer), "%s/%d", raw->text, decimals);
    expect_text("rawToDecimal", buffer, tj::Get(c, "result"), text_of(out));
  }
  // toFixed(24)
  cases = tj::Get(corpus, "toFixed");
  for (uint32_t i = 0; i < cases->count; ++i) {
    const Value* c = cases->items[i];
    char actual[200];
    fortis::wallet::address::JsToFixed(tj::Get(c, "value")->number, 24, actual, sizeof(actual));
    snprintf(buffer, sizeof(buffer), "%.17g", tj::Get(c, "value")->number);
    expect_text("toFixed", buffer, tj::Get(c, "result"), actual);
  }
  // quoteLongIntegers
  cases = tj::Get(corpus, "quote");
  for (uint32_t i = 0; i < cases->count; ++i) {
    const Value* c = cases->items[i];
    const Value* text = tj::Get(c, "text");
    uint8_t out_bytes[4096];
    tj::OutBuffer out{out_bytes, sizeof(out_bytes) - 1, 0, true};
    chains::QuoteLongIntegers(reinterpret_cast<const uint8_t*>(text->text), text->length, out);
    out_bytes[out.used] = '\0';
    expect_text("quote", text->text, tj::Get(c, "result"), reinterpret_cast<const char*>(out_bytes));
  }
  // integerString (NaN cases are represented by the string "NaN").
  cases = tj::Get(corpus, "integerString");
  for (uint32_t i = 0; i < cases->count; ++i) {
    const Value* c = cases->items[i];
    const Value* value = tj::Get(c, "value");
    Value* number_nan = nullptr;
    if (tj::IsString(value) && !strcmp(value->text, "NaN")) number_nan = tj::Num(arena, NAN);
    Value* out = chains::IntegerString(arena, number_nan ? number_nan : value);
    expect_text("integerString", json(arena, value), tj::Get(c, "result"), text_of(out));
  }
  // coerce
  cases = tj::Get(corpus, "coerce");
  for (uint32_t i = 0; i < cases->count; ++i) {
    const Value* c = cases->items[i];
    const Value* value = tj::Get(c, "value");
    const char* label = json(arena, value);
    const auto integer = chains::IntOrNull(value);
    char actual[64] = "null";
    if (integer) chains::JsNumberText(*integer, actual, sizeof(actual));
    const Value* want = tj::Get(c, "intOrNull");
    char expected[64] = "null";
    if (tj::IsNumber(want)) chains::JsNumberText(want->number, expected, sizeof(expected));
    ++checked;
    if (strcmp(actual, expected) != 0) fail("intOrNull", label, expected, actual);
    expect_text("hexToDecimalString", label, tj::Get(c, "hexToDecimalString"),
                text_of(chains::HexToDecimalString(arena, value)));
    const auto hexadecimal = chains::HexToInt(value);
    snprintf(actual, sizeof(actual), "null");
    if (hexadecimal) chains::JsNumberText(*hexadecimal, actual, sizeof(actual));
    want = tj::Get(c, "hexToInt");
    snprintf(expected, sizeof(expected), "null");
    if (tj::IsNumber(want)) chains::JsNumberText(want->number, expected, sizeof(expected));
    ++checked;
    if (strcmp(actual, expected) != 0) fail("hexToInt", label, expected, actual);
    expect_text("amountString", label, tj::Get(c, "amountString"), text_of(chains::AmountString(arena, value)));
  }
  // encodeAggregate3
  {
    const Value* aggregate = tj::Get(corpus, "aggregate");
    const Value* calls = tj::Get(aggregate, "calls");
    chains::AbiCall3 list[8];
    for (uint32_t i = 0; i < calls->count; ++i)
      list[i] = chains::AbiCall3{text_of(tj::Get(calls->items[i], "target")), text_of(tj::Get(calls->items[i], "callData"))};
    expect_text("encodeAggregate3", "calls", tj::Get(aggregate, "encoded"), chains::AbiAggregate3(arena, list, calls->count));
  }
  // decodeAggregate3
  cases = tj::Get(corpus, "decodeAggregate3");
  for (uint32_t i = 0; i < cases->count; ++i) {
    const Value* c = cases->items[i];
    const Value* hex = tj::Get(c, "hex");
    chains::AbiResult* results = nullptr;
    size_t count = 0;
    const bool ok = chains::AbiDecodeAggregate3(arena, hex->text, hex->length, &results, &count);
    Value* actual = tj::Null(arena);
    if (ok) {
      actual = tj::Arr(arena);
      for (size_t r = 0; r < count; ++r) {
        Value* entry = tj::Obj(arena);
        tj::Set(arena, entry, "success", tj::Bool(arena, results[r].success));
        tj::Set(arena, entry, "returnData", tj::Str(arena, results[r].data, results[r].length));
        tj::Push(arena, actual, entry);
      }
    }
    const char* want = json(arena, tj::Get(c, "result"));
    const char* got = json(arena, actual);
    ++checked;
    if (strcmp(want, got) != 0) fail("decodeAggregate3", hex->text, want, got);
  }
  // decodeString ({value} | {throws})
  cases = tj::Get(corpus, "decodeString");
  for (uint32_t i = 0; i < cases->count; ++i) {
    const Value* c = cases->items[i];
    const Value* hex = tj::Get(c, "hex");
    const Value* result = tj::Get(c, "result");
    Value* actual = chains::AbiString(arena, hex->text, hex->length);
    const Value* want = tj::Get(result, "value");
    const char* expected = json(arena, want ? want : tj::Null(arena));
    const char* got = json(arena, actual);
    ++checked;
    if (strcmp(expected, got) != 0) fail("decodeString", hex->text, expected, got);
  }
  // decodeUint256
  cases = tj::Get(corpus, "decodeUint256");
  for (uint32_t i = 0; i < cases->count; ++i) {
    const Value* c = cases->items[i];
    const Value* hex = tj::Get(c, "hex");
    char digits[200];
    chains::AbiUint256(hex->text, hex->length).ToDecimal(digits, sizeof(digits));
    expect_text("decodeUint256", hex->text, tj::Get(c, "result"), digits);
  }
  // printableMemoFromBase64 / printableAsciiFromHex
  cases = tj::Get(corpus, "memos");
  for (uint32_t i = 0; i < cases->count; ++i) {
    const Value* c = cases->items[i];
    const Value* b64 = tj::Get(c, "b64");
    expect_text("memo", b64->text, tj::Get(c, "result"),
                text_of(chains::PrintableMemoFromBase64(arena, b64->text, b64->length)));
  }
  cases = tj::Get(corpus, "asciiHex");
  for (uint32_t i = 0; i < cases->count; ++i) {
    const Value* c = cases->items[i];
    const Value* hex = tj::Get(c, "hex");
    expect_text("asciiHex", hex->text, tj::Get(c, "result"),
                text_of(chains::PrintableAsciiFromHex(arena, hex->text, hex->length)));
  }
  // transferRecordId
  cases = tj::Get(corpus, "recordIds");
  for (uint32_t i = 0; i < cases->count; ++i) {
    const Value* c = cases->items[i];
    const Value* unit = tj::Get(c, "unit");
    Value* id = chains::TransferRecordId(arena, text_of(tj::Get(c, "chain")), text_of(tj::Get(c, "hash")),
                                        tj::IsString(unit) ? unit->text : nullptr);
    const Value* result = tj::Get(c, "result");
    expect_text("transferRecordId", json(arena, c), tj::Get(result, "value"), id ? id->text : nullptr);
  }
  // encodeURIComponent
  cases = tj::Get(corpus, "uri");
  for (uint32_t i = 0; i < cases->count; ++i) {
    const Value* c = cases->items[i];
    const Value* text = tj::Get(c, "text");
    expect_text("encodeURIComponent", text->text, tj::Get(c, "result"), chains::EncodeUri(arena, text->text));
  }


  check_growing_arena();
  if (failures) {
    fprintf(stderr, "chain core corpus: %d of %d checks failed\n", failures, checked);
    return 1;
  }
  printf("chain core corpus: %d checks match the TypeScript helpers; growing arena checks passed\n", checked);
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
