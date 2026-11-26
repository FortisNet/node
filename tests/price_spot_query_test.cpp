// R31 GET /v1/prices: the query contract (fortis::prices::spot_query) and the price
// id grammar changes (JavaScript trim and lower-casing), driven by the frozen
// TypeScript fixture tests/corpus/price_spot_query_route.json
// (recorded by the earlier TypeScript implementation's tests).
//
// For every fixture request answered by query validation (400 with a
// BAD_REQUEST body) the parser must produce the same body byte for byte; for
// every 200 the parsed fiat and id pieces must be exactly the ids the
// TypeScript answer lists under prices, missing and invalid. Requests the
// HTTP layer refuses natively (targets over 12,500 bytes) still run here, so
// the pure function is held to TypeScript beyond the socket limit.
//
// usage: price_spot_query_test tests/corpus/price_spot_query_route.json

#include "strict_price_query_cases.h"
#include <initializer_list>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "price_id_parser.h"
#include "price_spot_query.h"
#include "token_json.h"

using namespace fortis::providers;

namespace {

using fortis::prices::spot_query::Query;
using fortis::prices::spot_query::SpotQueryOutcome;
namespace tj = fortis::codec::tj;

int g_failures = 0;
int g_checked_400 = 0;
int g_checked_200 = 0;
int g_checked_bound = 0;

#define CHECK(condition, ...)                                   \
  do {                                                          \
    if (!(condition)) {                                         \
      ++g_failures;                                             \
      fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);           \
      fprintf(stderr, __VA_ARGS__);                             \
      fprintf(stderr, "\n");                                    \
    }                                                           \
  } while (0)

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

Query g_query;  // ~13 KiB: keep it off the test's stack frames

const Query& parse(const char* url) {
  const char* mark = strchr(url, '?');
  fortis::prices::spot_query::ParseQuery(mark ? mark + 1 : nullptr, mark ? strlen(mark + 1) : 0, &g_query);
  return g_query;
}

bool piece_is(const Query& query, size_t index, const char* text, size_t length) {
  return query.id_length[index] == length &&
         memcmp(query.ids + query.id_offset[index], text, length) == 0;
}

bool listed(const tj::Value* body, const Query& query, size_t index) {
  const tj::Value* prices = tj::Get(body, "prices");
  for (uint32_t i = 0; prices && i < prices->count; ++i)
    if (piece_is(query, index, prices->members[i].key, prices->members[i].key_length)) return true;
  for (const char* name : {"missing", "invalid"}) {
    const tj::Value* list = tj::Get(body, name);
    for (uint32_t i = 0; list && i < list->count; ++i)
      if (piece_is(query, index, list->items[i]->text, list->items[i]->length)) return true;
  }
  return false;
}

bool piece_present(const Query& query, const char* text, size_t length) {
  for (size_t i = 0; i < query.id_count; ++i)
    if (piece_is(query, i, text, length)) return true;
  return false;
}

void check_fixture_case(tj::Arena& arena, const char* group, const tj::Value* item) {
  const char* name = tj::CString(arena, tj::Get(item, "name"));
  const char* url = tj::CString(arena, tj::Get(item, "url"));
  const tj::Value* response = tj::Get(item, "response");
  const int status = static_cast<int>(tj::Get(response, "status")->number);
  const tj::Value* text = tj::Get(response, "body");
  const tj::Value* headers = tj::Get(response, "headers");
  const bool head = tj::StringEquals(tj::Get(item, "method"), "HEAD");
  // Auth and quota answer before validation; they are not the parser's.
  if (strcmp(group, "auth") == 0 || status == 429) return;
  const Query& query = parse(url);
  if (const auto* change = strict::Price("spot", group, name)) {
    CHECK((query.status == SpotQueryOutcome::kOk) == change->accept, "%s/%s: task 17 query verdict", group, name);
    if (change->accept) {
      CHECK(query.id_count == 1 && piece_is(query, 0, change->id, strlen(change->id)), "%s/%s: first decoded id", group, name);
      CHECK(!strcmp(query.fiat, change->fiat), "%s/%s: first fiat", group, name);
    }
    return;
  }
  if (strcmp(name, "id of 600 bytes") == 0 || strcmp(name, "contract on a 600-byte network") == 0) {
    CHECK(status == 200 && query.status == SpotQueryOutcome::kIdTooLong,
          "%s: the 512-byte native bound must answer kIdTooLong", name);
    ++g_checked_bound;
    return;
  }
  if (status == 400) {
    CHECK(query.status != SpotQueryOutcome::kOk, "%s/%s: parser accepted a TypeScript 400", group, name);
    // compat-decisions.md#zod-issues: a schema refusal is the plain
    // BAD_REQUEST body where the oracle listed zod issues.
    static const char kZodPrefix[] = "{\"error\":\"BAD_REQUEST\",\"detail\":[";
    if (query.status == SpotQueryOutcome::kSchema) {
      CHECK(head || (text->length > strlen(kZodPrefix) &&
                     memcmp(text->text, kZodPrefix, strlen(kZodPrefix)) == 0),
            "%s/%s: oracle schema refusal without zod issues", group, name);
      CHECK(strcmp(query.error_body, "{\"error\":\"BAD_REQUEST\"}") == 0,
            "%s/%s: schema body %s", group, name, query.error_body);
    } else if (head) {
      const size_t expected = strtoul(tj::Get(headers, "content-length")->text, nullptr, 10);
      CHECK(query.error_body_length == expected, "%s/%s: HEAD 400 length %zu != %zu", group,
            name, query.error_body_length, expected);
    } else {
      CHECK(query.error_body_length == text->length &&
                memcmp(query.error_body, text->text, text->length) == 0,
            "%s/%s: body\n  native %.*s\n  oracle %.*s", group, name,
            static_cast<int>(query.error_body_length), query.error_body,
            static_cast<int>(text->length), text->text);
    }
    ++g_checked_400;
    return;
  }
  CHECK(status == 200, "%s/%s: unexpected oracle status %d", group, name, status);
  CHECK(query.status == SpotQueryOutcome::kOk, "%s/%s: parser refused a TypeScript 200 (%s)", group, name,
        query.error_body);
  if (query.status != SpotQueryOutcome::kOk || head || !text) return;  // HEAD/protobuf: shape checked by the route test
  const tj::Value* body = tj::Parse(arena, reinterpret_cast<const uint8_t*>(text->text), text->length);
  CHECK(body, "%s/%s: oracle body does not parse", group, name);
  if (!body) return;
  CHECK(tj::StringEquals(tj::Get(body, "fiat"), query.fiat), "%s/%s: fiat %s", group, name,
        query.fiat);
  for (size_t i = 0; i < query.id_count; ++i)
    CHECK(listed(body, query, i), "%s/%s: native piece %.*s is not in the oracle answer", group,
          name, static_cast<int>(query.id_length[i]), query.ids + query.id_offset[i]);
  const tj::Value* prices = tj::Get(body, "prices");
  for (uint32_t i = 0; i < prices->count; ++i)
    CHECK(piece_present(query, prices->members[i].key, prices->members[i].key_length),
          "%s/%s: oracle price key missing natively", group, name);
  size_t listed_count = 0;
  for (const char* list_name : {"missing", "invalid"}) {
    const tj::Value* list = tj::Get(body, list_name);
    listed_count += list->count;
    for (uint32_t i = 0; i < list->count; ++i)
      CHECK(piece_present(query, list->items[i]->text, list->items[i]->length),
            "%s/%s: oracle %s entry missing natively", group, name, list_name);
  }
  // Duplicates in missing/invalid are kept by TypeScript, so a count check
  // proves the pieces are not collapsed or split.
  CHECK(query.id_count >= listed_count, "%s/%s: %zu pieces < %zu listed", group, name,
        query.id_count, listed_count);
  ++g_checked_200;
}

void check_fixture(tj::Arena& arena, const tj::Value* fixture) {
  const tj::Value* groups = tj::Get(fixture, "groups");
  CHECK(tj::IsArray(groups) && groups->count == 4, "fixture groups");
  for (uint32_t g = 0; groups && g < groups->count; ++g) {
    const char* group = tj::CString(arena, tj::Get(groups->items[g], "name"));
    const tj::Value* cases = tj::Get(groups->items[g], "cases");
    for (uint32_t c = 0; c < cases->count; ++c) check_fixture_case(arena, group, cases->items[c]);
  }
}

void expect_pieces(const char* query_text, const char* const* pieces, size_t count, const char* fiat) {
  char url[256];
  snprintf(url, sizeof(url), "/v1/prices?%s", query_text);
  const Query& query = parse(url);
  CHECK(query.status == SpotQueryOutcome::kOk, "%s: status %d (%s)", query_text,
        static_cast<int>(query.status), query.error_body);
  CHECK(query.id_count == count, "%s: %zu pieces, expected %zu", query_text, query.id_count, count);
  for (size_t i = 0; i < count && i < query.id_count; ++i)
    CHECK(piece_is(query, i, pieces[i], strlen(pieces[i])), "%s: piece %zu is %.*s", query_text,
          i, static_cast<int>(query.id_length[i]), query.ids + query.id_offset[i]);
  CHECK(strcmp(query.fiat, fiat) == 0, "%s: fiat %s", query_text, query.fiat);
}

void check_query_edges() {
  {
    const char* p[] = {"ethereum"};
    expect_pieces("ids=ethereum", p, 1, "usd");
    expect_pieces("fiat=usd&ids=ethereum", p, 1, "usd");
    expect_pieces("%66iat=usd&ids=ethereum", p, 1, "usd");  // encoded key
    expect_pieces("ids=ethereum&idsx=1&xids=2&ID=3", p, 1, "usd");
    CHECK(parse("/?ids=ethereum&fiat%=eur").status == SpotQueryOutcome::kSchema, "malformed unknown key");    // key "fiat%" (raw kept)
  }
  {
    const char* p[] = {"a=b"};
    expect_pieces("ids=a=b", p, 1, "usd");  // only the first '=' splits
  }
  {
    const char* p[] = {"btc", "eth"};
    expect_pieces("ids=btc,,%20,eth,&fiat=abcde", p, 2, "abcde");
    expect_pieces("ids=btc%2C%2Ceth&fiat=abc", p, 2, "abc");
  }
  {
    const char* p[] = {"\xc3\xa9t"};  // raw Latin-1 0xE9 is the character U+00E9
    expect_pieces("ids=\xc3\xa9t", p, 1, "usd");
  }
  {
    const char* p[] = {"\xc3\xa9"};  // an escape interrupted by a raw byte keeps the raw text
    expect_pieces("ids=%C3\xa9", p, 1, "usd");
  }
  {
    CHECK(parse("/?ids=%ED%A0%80").status == SpotQueryOutcome::kSchema, "invalid UTF-8 refused");     // surrogate
    CHECK(parse("/?ids=%C0%80").status == SpotQueryOutcome::kSchema, "invalid UTF-8 refused");    // overlong
    CHECK(parse("/?ids=%F4%90%80%80").status == SpotQueryOutcome::kSchema, "invalid UTF-8 refused");  // past U+10FFFF
    CHECK(parse("/?ids=%E0%80%80").status == SpotQueryOutcome::kSchema, "invalid UTF-8 refused");  // overlong three-byte
  }
  {
    const char* p[] = {"\xf0\x9f\x98\x80"};
    expect_pieces("ids=%F0%9F%98%80", p, 1, "usd");
  }
  // fiat: 3–5 lower-case ASCII letters after decoding.
  CHECK(parse("/v1/prices?ids=a&fiat=ab").status == SpotQueryOutcome::kSchema, "fiat ab");
  CHECK(parse("/v1/prices?ids=a&fiat=abcdef").status == SpotQueryOutcome::kSchema, "fiat abcdef");
  CHECK(parse("/v1/prices?ids=a&fiat=ab%C3%A9").status == SpotQueryOutcome::kSchema, "fiat non-ASCII");
  CHECK(parse("/v1/prices?ids=a&fiat=usd%0A").status == SpotQueryOutcome::kSchema, "fiat newline");
  CHECK(parse("/v1/prices?ids=a&fiat=%75sd").status == SpotQueryOutcome::kOk, "fiat escape");
  CHECK(parse("/v1/prices?ids=a&fiat").status == SpotQueryOutcome::kSchema, "fiat flag");
  // Unit boundaries: 4000 code units pass, 4001 fail; an astral character is
  // two units; values beyond the buffer are over the limit.
  static char url[40000];
  memcpy(url, "/v1/prices?ids=", 15);
  memset(url + 15, 'a', 4000); url[15 + 4000] = '\0';
  CHECK(parse(url).status == SpotQueryOutcome::kIdTooLong, "4000 units of one id: native bound");
  for (size_t i = 0; i < 4000; ++i) url[15 + i] = (i % 40 == 39) ? ',' : 'a';
  CHECK(parse(url).status == SpotQueryOutcome::kOk && g_query.id_count == 100, "4000 units, 100 ids");
  url[15 + 4000] = 'a'; url[15 + 4001] = '\0';
  CHECK(parse(url).status == SpotQueryOutcome::kSchema &&
            strcmp(g_query.error_body, "{\"error\":\"BAD_REQUEST\"}") == 0, "4001 units");
  size_t cursor = 15;
  for (size_t i = 0; i < 1999; ++i) { memcpy(url + cursor, "%F0%9F%98%80", 12); cursor += 12; }
  memcpy(url + cursor, "aa", 3);  // 3998 + 2 = 4000 units in one id
  CHECK(parse(url).status == SpotQueryOutcome::kIdTooLong, "1999 astral + 2: within 4000 units");
  memcpy(url + cursor, "aaa", 4);
  CHECK(parse(url).status == SpotQueryOutcome::kSchema, "1999 astral + 3: 4001 units");
  cursor = 15;
  for (size_t i = 0; i < 4001; ++i) { memcpy(url + cursor, "%E2%82%AC", 9); cursor += 9; }
  url[cursor] = '\0';
  CHECK(parse(url).status == SpotQueryOutcome::kSchema,
        "4001 three-byte units overflow the buffer and stay refused");
  // Pieces: exactly 100 accepted, 101 refused with the count detail.
  cursor = 15;
  for (size_t i = 0; i < 101; ++i) { memcpy(url + cursor, "b,", 2); cursor += 2; }
  url[cursor - 1] = '\0';
  CHECK(parse(url).status == SpotQueryOutcome::kIdCount, "101 pieces");
  url[cursor - 3] = '\0';
  CHECK(parse(url).status == SpotQueryOutcome::kOk && g_query.id_count == 100, "100 pieces");
  // 512 bytes per id pass; 513 hit the native bound only after the count check.
  memcpy(url, "/v1/prices?ids=", 15);
  memset(url + 15, 'c', 512); url[15 + 512] = '\0';
  CHECK(parse(url).status == SpotQueryOutcome::kOk, "512-byte id");
  url[15 + 512] = 'c'; url[15 + 513] = '\0';
  CHECK(parse(url).status == SpotQueryOutcome::kIdTooLong, "513-byte id");
  // Trim set: every JavaScript whitespace code point, and two that are not.
  const char* spaces[] = {"%09", "%0A", "%0B", "%0C", "%0D", "%20", "%C2%A0", "%E1%9A%80",
                          "%E2%80%80", "%E2%80%8A", "%E2%80%A8", "%E2%80%A9", "%E2%80%AF",
                          "%E2%81%9F", "%E3%80%80", "%EF%BB%BF", "+"};
  for (const char* space : spaces) {
    char query_text[64];
    snprintf(query_text, sizeof(query_text), "ids=%sx%s", space, space);
    const char* p[] = {"x"};
    expect_pieces(query_text, p, 1, "usd");
  }
  for (const char* other : {"%C2%85", "%E2%80%8B", "%E1%A0%8E", "%00"}) {
    char query_text[64];
    snprintf(query_text, sizeof(query_text), "/v1/prices?ids=%sx", other);
    const Query& query = parse(query_text);
    CHECK(query.status == SpotQueryOutcome::kOk && query.id_count == 1 && query.id_length[0] > 1,
          "%s must not be trimmed", other);
  }
  CHECK(parse("/v1/prices?ids=%20").status == SpotQueryOutcome::kIdCount, "whitespace only");
  CHECK(parse("/v1/prices").status == SpotQueryOutcome::kSchema, "no query");
  CHECK(parse("/v1/prices?ids&ids").status == SpotQueryOutcome::kSchema, "two flags are refused");
}

void expect_id(const char* raw, PriceIdKind kind, const char* source_id) {
  ParsedPriceId parsed{};
  const bool ok = ParsePriceId(raw, strlen(raw), &parsed);
  if (kind == PriceIdKind::kInvalid) {
    CHECK(!ok, "%s: expected invalid, got %s", raw, parsed.source_id);
    return;
  }
  CHECK(ok && parsed.kind == kind && strcmp(parsed.source_id, source_id) == 0,
        "%s: got ok=%d %s", raw, ok, parsed.source_id);
}

void check_price_ids() {
  const char* evm = "0xABABABABABABABABABABABABABABABABABABABAB";
  char raw[700];
  // js-case: ids and networks are ASCII lower-cased, so U+212A
  // KELVIN SIGN is not 'k' and a non-ASCII letter keeps its case.
  expect_id("\xe2\x84\xaa" "aspa", PriceIdKind::kInvalid, "");
  expect_id("\xe2\x84\xaa", PriceIdKind::kInvalid, "");
  expect_id("-\xe2\x84\xaa", PriceIdKind::kInvalid, "");
  expect_id("\xc4\xb0" "d", PriceIdKind::kInvalid, "");
  expect_id("\xc2\xa0" "Ethereum\xe3\x80\x80", PriceIdKind::kCoinGecko, "ethereum");  // JS trim
  expect_id("\xc2\x85" "ethereum", PriceIdKind::kInvalid, "");                 // NEL is not whitespace
  memset(raw, 'a', 99); memcpy(raw + 99, "\xe2\x84\xaa", 4);
  expect_id(raw, PriceIdKind::kInvalid, "");
  memset(raw, 'A', 100); raw[100] = '\0';
  {
    ParsedPriceId parsed{};
    CHECK(ParsePriceId(raw, strlen(raw), &parsed) && strlen(parsed.source_id) == 100 &&
              parsed.source_id[99] == 'a', "100 A lower-cased");
  }
  // Contract networks lower-case A-Z only.
  snprintf(raw, sizeof(raw), "\xc3\x89TH:erc20:%s", evm);
  {
    char expected[200];
    snprintf(expected, sizeof(expected), "\xc3\x89th:erc20:%s",
             "0xabababababababababababababababababababab");
    expect_id(raw, PriceIdKind::kContract, expected);
    snprintf(raw, sizeof(raw), "\xc3\xa9Th:erc20:%s", evm);
    snprintf(expected, sizeof(expected), "\xc3\xa9th:erc20:%s",
             "0xabababababababababababababababababababab");
    expect_id(raw, PriceIdKind::kContract, expected);  // a different id now: É is not lowered
  }
  snprintf(raw, sizeof(raw), "\xce\xa3\xce\xa3:erc20:%s", evm);  // ΣΣ stays (no Final_Sigma)
  {
    char expected[200];
    snprintf(expected, sizeof(expected), "\xce\xa3\xce\xa3:erc20:%s",
             "0xabababababababababababababababababababab");
    expect_id(raw, PriceIdKind::kContract, expected);
  }
  snprintf(raw, sizeof(raw), "a\xe2\x80\xa8" "b:erc20:%s", evm);  // `.` excludes U+2028
  expect_id(raw, PriceIdKind::kInvalid, "");
  snprintf(raw, sizeof(raw), "a\xe2\x80\xa9" "b:erc20:%s", evm);
  expect_id(raw, PriceIdKind::kInvalid, "");
  snprintf(raw, sizeof(raw), "a\xe2\x80\xaa" "b:erc20:%s", evm);  // U+202A is fine
  {
    ParsedPriceId parsed{};
    CHECK(ParsePriceId(raw, strlen(raw), &parsed) && parsed.kind == PriceIdKind::kContract,
          "U+202A network");
  }
  snprintf(raw, sizeof(raw), "TRON:trc20:T%s", "LyqzVGLV1srkB7dToTAEqgDSfPtXRJZYH");
  expect_id(raw, PriceIdKind::kContract, "tron:trc20:TLyqzVGLV1srkB7dToTAEqgDSfPtXRJZYH");
  snprintf(raw, sizeof(raw), "\xe2\x84\xaa" "tron:trc20:T%s", "LyqzVGLV1srkB7dToTAEqgDSfPtXRJZYH");
  expect_id(raw, PriceIdKind::kInvalid, "");
  // ASCII lower-casing never grows a network: 230 x İ stays within 512 bytes.
  size_t cursor = 0;
  for (size_t i = 0; i < 230; ++i) { memcpy(raw + cursor, "\xc4\xb0", 2); cursor += 2; }
  snprintf(raw + cursor, sizeof(raw) - cursor, ":erc20:%s", evm);
  {
    ParsedPriceId parsed{};
    CHECK(ParsePriceId(raw, strlen(raw), &parsed) && parsed.kind == PriceIdKind::kContract &&
              strncmp(parsed.source_id, raw, 460) == 0, "230 x U+0130 network kept as written");
  }
  expect_id("aptos:\xc2\xa0" "0x1", PriceIdKind::kContract, "aptos:0x1");
  expect_id("ethereum\x01", PriceIdKind::kInvalid, "");
}

void check_helpers() {
  char out[32];
  CHECK(fortis::prices::spot_query::DecodeComponent("a+b%2B", 6, out, sizeof(out)) == 4 &&
            memcmp(out, "a b+", 4) == 0, "plus then decode");
  CHECK(fortis::prices::spot_query::DecodeComponent("%zz+", 4, out, sizeof(out)) == SIZE_MAX, "raw fallback keeps replaced plus");
  CHECK(fortis::prices::spot_query::DecodeComponent("%E2%82", 6, out, sizeof(out)) == SIZE_MAX, "truncated sequence");
  CHECK(fortis::prices::spot_query::DecodeComponent("%E2%82%AC", 9, out, 2) == 3, "overflow length");
  CHECK(fortis::prices::spot_query::DecodeComponent("%41%", 4, out, sizeof(out)) == SIZE_MAX, "trailing percent fails the whole value");
  CHECK(fortis::prices::spot_query::Utf16Units("a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80", 10) == 5,
        "UTF-16 units");
  CHECK(fortis::providers::JsWhitespaceAtEnd("x\xc2\xa0", 3) == 2, "NBSP at end");
  CHECK(fortis::providers::JsWhitespaceAtEnd("x\xef\xbb\xbf", 4) == 3, "BOM at end");
  CHECK(fortis::providers::JsWhitespaceAtEnd("\xe2\x80\x8b", 3) == 0, "ZWSP");
  CHECK(fortis::providers::JsWhitespaceAtStart("\xe2\x80\x8a", 3) == 3 &&
            fortis::providers::JsWhitespaceAtStart("\xe2\x80\x8b", 3) == 0, "U+200A vs U+200B");
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "tests/corpus/price_spot_query_route.json";
  size_t length = 0;
  char* text = read_file(path, &length);
  if (!text) {
    fprintf(stderr, "cannot read %s\n", path);
    return 2;
  }
  tj::Arena arena = tj::GrowingArena(64u << 20);
  const tj::Value* fixture = tj::Parse(arena, reinterpret_cast<const uint8_t*>(text), length);
  if (!fixture) {
    fprintf(stderr, "fixture does not parse\n");
    return 2;
  }
  check_fixture(arena, fixture);
  check_query_edges();
  check_price_ids();
  check_helpers();
  tj::FreeArena(arena);
  free(text);
  CHECK(g_checked_400 >= 250 && g_checked_200 >= 40 && g_checked_bound == 2,
        "fixture coverage: %d 400s, %d 200s, %d bounded", g_checked_400, g_checked_200,
        g_checked_bound);
  if (g_failures) {
    fprintf(stderr, "price_spot_query_test: %d failure(s)\n", g_failures);
    return 1;
  }
  printf("price_spot_query_test: %d validation bodies, %d accepted queries, %d bounded ids, "
         "edges and id grammar OK\n", g_checked_400, g_checked_200, g_checked_bound);
  return 0;
}
