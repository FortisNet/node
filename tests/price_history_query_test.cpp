// R32 GET /v1/prices/history: the query contract (fortis::prices::history_query),
// driven by the frozen TypeScript fixture
// tests/corpus/price_history_query_route.json
// (recorded by the earlier TypeScript implementation's tests).
//
// - Every fixture request answered by validation (a 400 with a zod or range
//   body) must produce the same body byte for byte; HEAD 400s the same
//   Content-Length.
// - A 400 "unrecognized id" must pass the parser and fail ParsePriceId.
// - Every 200 must pass both, and a JSON 200's `id` and `interval` must be
//   the canonical source id and interval the parser and ParsePriceId give.
// - The fixture's `numbers` table (String(Number(input)) from Node) must be
//   reproduced by JsStringToNumber, whole and fed one byte at a time.
// Requests the HTTP layer refuses natively (targets over 12,500 bytes)
// still run here, so the pure function is held to TypeScript beyond them.
//
// usage: price_history_query_test tests/corpus/price_history_query_route.json

#include "strict_price_query_cases.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "price_history_query.h"
#include "price_id_parser.h"
#include "token_json.h"

using namespace fortis::providers;

namespace {

using fortis::prices::history_query::JsNumberParser;
using fortis::prices::history_query::JsStringToNumber;
using fortis::prices::history_query::Query;
using fortis::prices::history_query::HistoryQueryOutcome;
namespace tj = fortis::codec::tj;

int g_failures = 0;
int g_checked_400 = 0;
int g_checked_unrecognized = 0;
int g_checked_200 = 0;
int g_checked_numbers = 0;

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

Query g_query;
ParsedPriceId g_parsed;

const Query& parse(const char* url) {
  const char* mark = strchr(url, '?');
  fortis::prices::history_query::ParseQuery(mark ? mark + 1 : nullptr, mark ? strlen(mark + 1) : 0,
                                    &g_query);
  return g_query;
}

bool same_double(double a, double b) {
  if (isnan(a) || isnan(b)) return isnan(a) && isnan(b);
  return a == b && signbit(a) == signbit(b);
}

// The oracle's String(Number(input)) as a double.
double oracle_number(const char* text) {
  if (strcmp(text, "NaN") == 0) return NAN;
  if (strcmp(text, "Infinity") == 0) return INFINITY;
  if (strcmp(text, "-Infinity") == 0) return -INFINITY;
  return strtod(text, nullptr);  // shortest round-trip text; "-0" → -0.0
}

void check_case_url(tj::Arena& arena, const char* group, const char* name, const char* url,
                    bool head, const tj::Value* response) {
  const int status = static_cast<int>(tj::Get(response, "status")->number);
  const tj::Value* text = tj::Get(response, "body");
  const tj::Value* headers = tj::Get(response, "headers");
  // Auth and quota answer before validation; they are not the parser's.
  if (strcmp(group, "auth") == 0 || status == 429) return;
  const Query& query = parse(url);
  if (const auto* change = strict::Price("history", group, name)) {
    CHECK((query.status == HistoryQueryOutcome::kOk) == change->accept, "%s/%s: task 17 query verdict", group, name);
    if (change->accept) {
      CHECK(!strcmp(query.id, change->id), "%s/%s: first decoded id", group, name);
      CHECK(query.from == change->from && query.to == change->to && query.daily == (!strcmp(change->interval, "1d")), "%s/%s: first bounds/interval", group, name);
    }
    return;
  }
  const bool recognized = query.status == HistoryQueryOutcome::kOk &&
      ParsePriceId(query.id, query.id_length, &g_parsed) &&
      g_parsed.kind != PriceIdKind::kInvalid;
  // js-case: ids and networks are ASCII lower-cased, so the
  // oracle's Unicode case-folding answers for these two do not apply.
  if (strcmp(name, "id Kelvin sign lowers to k") == 0) {
    CHECK(status == 200 && !recognized, "%s/%s: U+212A must not lower to k", group, name);
    return;
  }
  if (strcmp(name, "id non-ASCII network lower-cased") == 0) {
    CHECK(status == 200 && recognized && strncmp(g_parsed.source_id, "\xc3\x89th:", 5) == 0,
          "%s/%s: only A-Z lower-cased: %s", group, name, recognized ? g_parsed.source_id : "-");
    return;
  }
  if (status == 400) {
    const size_t expected_length = head
        ? strtoul(tj::Get(headers, "content-length")->text, nullptr, 10) : text->length;
    static const char kUnrecognized[] = "{\"error\":\"BAD_REQUEST\",\"detail\":\"unrecognized id\"}";
    const bool unrecognized = head ? expected_length == strlen(kUnrecognized)
                                   : text->length == strlen(kUnrecognized) &&
                                     memcmp(text->text, kUnrecognized, text->length) == 0;
    if (unrecognized && query.status == HistoryQueryOutcome::kOk) {
      CHECK(!recognized, "%s/%s: ParsePriceId accepted a TypeScript unrecognized id", group, name);
      ++g_checked_unrecognized;
      return;
    }
    CHECK(query.status != HistoryQueryOutcome::kOk, "%s/%s: parser accepted a TypeScript 400", group, name);
    // compat-decisions.md#zod-issues: a schema refusal is the plain
    // BAD_REQUEST body where the oracle listed zod issues.
    static const char kZodPrefix[] = "{\"error\":\"BAD_REQUEST\",\"detail\":[";
    if (query.status == HistoryQueryOutcome::kSchema) {
      CHECK(head || (text->length > strlen(kZodPrefix) &&
                     memcmp(text->text, kZodPrefix, strlen(kZodPrefix)) == 0),
            "%s/%s: oracle schema refusal without zod issues", group, name);
      CHECK(strcmp(query.error_body, "{\"error\":\"BAD_REQUEST\"}") == 0,
            "%s/%s: schema body %s", group, name, query.error_body);
    } else if (head) {
      CHECK(query.error_body_length == expected_length, "%s/%s: HEAD 400 length %zu != %zu",
            group, name, query.error_body_length, expected_length);
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
  CHECK(query.status == HistoryQueryOutcome::kOk, "%s/%s: parser refused a TypeScript 200 (%s)", group, name,
        query.error_body);
  CHECK(recognized, "%s/%s: ParsePriceId refused a TypeScript 200", group, name);
  ++g_checked_200;
  if (!recognized || head || !text) return;  // HEAD/protobuf/large: the route test checks bytes
  const tj::Value* body = tj::Parse(arena, reinterpret_cast<const uint8_t*>(text->text),
                                    text->length);
  CHECK(body, "%s/%s: oracle body does not parse", group, name);
  if (!body) return;
  CHECK(tj::StringEquals(tj::Get(body, "id"), g_parsed.source_id), "%s/%s: id %s", group, name,
        g_parsed.source_id);
  CHECK(tj::StringEquals(tj::Get(body, "interval"), query.daily ? "1d" : "1h"),
        "%s/%s: interval", group, name);
}

void check_fixture(tj::Arena& arena, const tj::Value* fixture) {
  const tj::Value* groups = tj::Get(fixture, "groups");
  CHECK(tj::IsArray(groups) && groups->count == 9, "fixture groups");
  for (uint32_t g = 0; groups && g < groups->count; ++g) {
    const char* group = tj::CString(arena, tj::Get(groups->items[g], "name"));
    const tj::Value* cases = tj::Get(groups->items[g], "cases");
    for (uint32_t i = 0; i < cases->count; ++i) {
      const tj::Value* item = cases->items[i];
      const char* name = tj::CString(arena, tj::Get(item, "name"));
      const bool head = tj::StringEquals(tj::Get(item, "method"), "HEAD");
      const tj::Value* urls = tj::Get(item, "urls");
      const tj::Value* single = tj::Get(item, "response");
      const tj::Value* several = tj::Get(item, "responses");
      const bool parallel = tj::Get(item, "parallel") != nullptr;
      for (uint32_t u = 0; u < urls->count; ++u) {
        const char* url = tj::CString(arena, urls->items[u]);
        if (single) check_case_url(arena, group, name, url, head, single);
        else if (parallel)
          for (uint32_t r = 0; r < several->count; ++r)
            check_case_url(arena, group, name, url, head, several->items[r]);
        else check_case_url(arena, group, name, url, head, several->items[u]);
      }
    }
  }
  const tj::Value* numbers = tj::Get(fixture, "numbers");
  CHECK(tj::IsArray(numbers) && numbers->count > 90, "fixture numbers");
  for (uint32_t i = 0; numbers && i < numbers->count; ++i) {
    const tj::Value* input = tj::Get(numbers->items[i], "input");
    const char* output = tj::CString(arena, tj::Get(numbers->items[i], "output"));
    const double expected = oracle_number(output);
    const double whole = JsStringToNumber(input->text, input->length);
    JsNumberParser bytes;
    for (size_t b = 0; b < input->length; ++b)
      bytes.FeedByte(static_cast<unsigned char>(input->text[b]));
    const double streamed = bytes.Finish();
    CHECK(same_double(whole, expected) && same_double(streamed, expected),
          "Number(%.60s…) = %s, native %.17g / %.17g", input->text, output, whole, streamed);
    ++g_checked_numbers;
  }
}

// Bounds the fixture cannot reach through a 16 KiB Node request head, plus
// buffer edges of the native parser.
void check_edges() {
  // id: exactly kIdBufferBytes (200 three-byte units) is kept; one more
  // byte overflows the buffer and is necessarily over 200 units.
  char url[4096];
  size_t used = static_cast<size_t>(snprintf(url, sizeof(url), "/h?from=1&to=2&id="));
  for (int i = 0; i < 200; ++i) used += static_cast<size_t>(snprintf(url + used, sizeof(url) - used, "%%E2%%82%%AC"));
  const Query& euro = parse(url);
  CHECK(euro.status == HistoryQueryOutcome::kOk && euro.id_length == 600, "200 euro signs kept");
  snprintf(url + used, sizeof(url) - used, "a");
  const Query& over = parse(url);
  CHECK(over.status == HistoryQueryOutcome::kSchema, "601-byte id is too big: %s", over.error_body);

  // Null and empty query strings: every required field is missing.
  fortis::prices::history_query::ParseQuery(nullptr, 0, &g_query);
  CHECK(g_query.status == HistoryQueryOutcome::kSchema &&
            strcmp(g_query.error_body, "{\"error\":\"BAD_REQUEST\"}") == 0,
        "null query: %s", g_query.error_body);
  const Query& worst = parse("/h?id=a&id=b&from=-1e20&to=-1e20&interval=x");
  CHECK(worst.status == HistoryQueryOutcome::kSchema &&
            strcmp(worst.error_body, "{\"error\":\"BAD_REQUEST\"}") == 0,
        "every field invalid: %s", worst.error_body);

  // Number values far beyond the request-target limit stream without a
  // buffer: 100,000 leading zeros, then the value.
  {
    static char big[100064];
    size_t n = static_cast<size_t>(snprintf(big, sizeof(big), "from="));
    memset(big + n, '0', 100000);
    n += 100000;
    n += static_cast<size_t>(snprintf(big + n, sizeof(big) - n, "1767225600&to=1767229200&id=x"));
    fortis::prices::history_query::ParseQuery(big, n, &g_query);
    CHECK(g_query.status == HistoryQueryOutcome::kOk && g_query.from == 1767225600, "long zero padding");
  }

  // Range boundaries and the detail text (en dash).
  CHECK(parse("/h?id=x&from=1&to=7776001").status == HistoryQueryOutcome::kOk, "1h 90 days");
  const Query& wide = parse("/h?id=x&from=1&to=7776002");
  CHECK(wide.status == HistoryQueryOutcome::kRange &&
            strcmp(wide.error_body, "{\"error\":\"BAD_REQUEST\",\"detail\":\"range for 1h must be "
                                    "1s\xe2\x80\x93" "7776000s\"}") == 0,
        "1h range detail: %s", wide.error_body);
  CHECK(parse("/h?id=x&from=1&to=316224001&interval=1d").status == HistoryQueryOutcome::kOk, "1d decade");
  CHECK(parse("/h?id=x&from=1&to=316224002&interval=1d").status == HistoryQueryOutcome::kRange, "1d +1");
  CHECK(parse("/h?id=x&from=5&to=5").status == HistoryQueryOutcome::kRange, "empty range");
  const Query& safe = parse("/h?id=x&from=9007199254740990&to=9007199254740991");
  CHECK(safe.status == HistoryQueryOutcome::kOk && safe.to == 9007199254740991LL, "max safe integer");

  // Interval spelling and default.
  CHECK(!parse("/h?id=x&from=1&to=2").daily, "default interval is 1h");
  CHECK(parse("/h?id=x&from=1&to=2&interval=1%64").daily, "encoded 1d");
  CHECK(parse("/h?id=x&from=1&to=2&interval=1dd").status == HistoryQueryOutcome::kSchema, "1dd");
  CHECK(parse("/h?id=x&from=1&to=2&interval=1d&interval=1d").status == HistoryQueryOutcome::kOk,
        "repeated interval");
  CHECK(parse("/h?id=x&from=1&to=2&interval").status == HistoryQueryOutcome::kSchema, "interval flag");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s price_history_query_route.json\n", argv[0]);
    return 2;
  }
  size_t length = 0;
  char* text = read_file(argv[1], &length);
  if (!text) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  tj::Arena arena = tj::GrowingArena(64u << 20);
  const tj::Value* fixture = tj::Parse(arena, reinterpret_cast<const uint8_t*>(text), length);
  CHECK(fixture, "fixture does not parse");
  if (fixture) check_fixture(arena, fixture);
  check_edges();
  tj::FreeArena(arena);
  free(text);
  if (g_failures) {
    fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  printf("price_history_query_test: %d validation bodies, %d unrecognized ids, %d accepted "
         "queries, %d Number() cases and edges OK\n",
         g_checked_400, g_checked_unrecognized, g_checked_200, g_checked_numbers);
  return 0;
}
