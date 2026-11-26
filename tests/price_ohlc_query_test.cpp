// R33 GET /v1/prices/ohlc: the query contract (fortis::prices::ohlc_query), driven
// by the frozen TypeScript fixture
// tests/corpus/price_ohlc_query_route.json
// (recorded by the earlier TypeScript implementation's tests).
//
// - Every fixture request answered by validation (the bare 400
//   {"error":"BAD_REQUEST"}) must be refused by the parser.
// - A 400 "unsupported price id" must pass the parser as a non-slug and
//   fail ParsePriceId.
// - Every other answer (200, or the slug path's 502) must pass the parser,
//   dispatched as the group's path: a CoinGecko slug in the slug groups,
//   a recognized price id elsewhere. When the request reached the provider,
//   the parsed slug and days must be the ones in the provider path.
// Then buffer and grammar edges the fixture cannot reach through a Node
// request head.
//
// usage: price_ohlc_query_test tests/corpus/price_ohlc_query_route.json

#include "strict_price_query_cases.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "price_id_parser.h"
#include "price_ohlc_query.h"
#include "token_json.h"

using namespace fortis::providers;

namespace {

using fortis::prices::ohlc_query::IsCoinGeckoSlug;
using fortis::prices::ohlc_query::Query;
namespace tj = fortis::codec::tj;

int g_failures = 0;
int g_checked_400 = 0;
int g_checked_unsupported = 0;
int g_checked_store = 0;
int g_checked_slug = 0;
int g_checked_edges = 0;

#define CHECK(condition, ...)                                   \
  do {                                                          \
    if (!(condition)) {                                         \
      ++g_failures;                                             \
      fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);           \
      fprintf(stderr, __VA_ARGS__);                             \
      fprintf(stderr, "\n");                                    \
    }                                                           \
  } while (0)

#define EDGE(condition, ...)  \
  do {                        \
    ++g_checked_edges;        \
    CHECK(condition, __VA_ARGS__); \
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
  fortis::prices::ohlc_query::ParseQuery(mark ? mark + 1 : nullptr, mark ? strlen(mark + 1) : 0,
                                 &g_query);
  return g_query;
}

bool slug_group(const char* group) {
  static const char* const kSlugGroups[] = {"slug", "shapes", "failing", "text", "missing",
                                            "stale", "concurrent"};
  for (const char* name : kSlugGroups) if (strcmp(group, name) == 0) return true;
  return false;
}

void check_case_url(tj::Arena& arena, const char* group, const char* name, const char* url,
                    bool head, const tj::Value* response, const tj::Value* calls) {
  const int status = static_cast<int>(tj::Get(response, "status")->number);
  const tj::Value* text = tj::Get(response, "body");
  const tj::Value* headers = tj::Get(response, "headers");
  // Auth and quota answer before validation; they are not the parser's.
  if (strcmp(group, "auth") == 0 || status == 429) return;
  const Query& query = parse(url);
  if (const auto* change = strict::Price("ohlc", group, name)) {
    CHECK(query.ok == change->accept, "%s/%s: task 17 query verdict", group, name);
    if (change->accept) {
      CHECK(!strcmp(query.id, change->id), "%s/%s: first decoded id", group, name);
      CHECK(query.days == change->days, "%s/%s: first days", group, name);
    }
    return;
  }
  if (status == 400) {
    static const char kBare[] = "{\"error\":\"BAD_REQUEST\"}";
    static const char kUnsupported[] =
        "{\"error\":\"BAD_REQUEST\",\"detail\":\"unsupported price id\"}";
    const size_t length = head ? strtoul(tj::Get(headers, "content-length")->text, nullptr, 10)
                               : text->length;
    const bool bare = length == strlen(kBare) &&
        (head || memcmp(text->text, kBare, length) == 0);
    const bool unsupported = length == strlen(kUnsupported) &&
        (head || memcmp(text->text, kUnsupported, length) == 0);
    CHECK(bare || unsupported, "%s/%s: unexpected oracle 400", group, name);
    if (bare) {
      CHECK(!query.ok, "%s/%s: parser accepted a TypeScript 400", group, name);
      ++g_checked_400;
    } else {
      CHECK(query.ok && !query.slug, "%s/%s: unsupported id must parse as a non-slug", group, name);
      CHECK(!(ParsePriceId(query.id, query.id_length, &g_parsed) &&
              g_parsed.kind != PriceIdKind::kInvalid),
            "%s/%s: ParsePriceId accepted a TypeScript unsupported id", group, name);
      ++g_checked_unsupported;
    }
    return;
  }
  CHECK(status == 200 || status == 502, "%s/%s: unexpected oracle status %d", group, name, status);
  CHECK(query.ok, "%s/%s: parser refused a TypeScript %d", group, name, status);
  if (!query.ok) return;
  // js-case: U+212A is not 'k' under ASCII lower-casing.
  if (strcmp(name, "id Kelvin sign lowers to k") == 0) {
    CHECK(!(ParsePriceId(query.id, query.id_length, &g_parsed) && g_parsed.kind != PriceIdKind::kInvalid),
          "%s/%s: U+212A must not lower to k", group, name);
    return;
  }
  if (slug_group(group)) {
    CHECK(query.slug, "%s/%s: not dispatched as a slug", group, name);
    for (uint32_t i = 0; calls && i < calls->count; ++i) {
      char expected[1024];
      snprintf(expected, sizeof(expected), "/api/v3/coins/%s/ohlc?vs_currency=usd&days=%d",
               query.id, query.days);
      // Together cases ask for several slugs; one of the calls is this one.
      bool found = false;
      for (uint32_t j = 0; j < calls->count; ++j)
        found |= tj::StringEquals(calls->items[j], expected);
      CHECK(found, "%s/%s: no provider call %s", group, name, expected);
    }
    ++g_checked_slug;
  } else {
    CHECK(!query.slug, "%s/%s: dispatched as a slug", group, name);
    CHECK(ParsePriceId(query.id, query.id_length, &g_parsed) &&
              g_parsed.kind != PriceIdKind::kInvalid,
          "%s/%s: ParsePriceId refused a TypeScript store read", group, name);
    CHECK(!calls || calls->count == 0, "%s/%s: store read asked the provider", group, name);
    ++g_checked_store;
  }
  static_cast<void>(arena);
}

void check_fixture(tj::Arena& arena, const tj::Value* fixture) {
  const tj::Value* groups = tj::Get(fixture, "groups");
  CHECK(tj::IsArray(groups) && groups->count == 13, "fixture groups");
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
      const tj::Value* calls = tj::Get(item, "upstreamCalls");
      const bool parallel = tj::Get(item, "parallel") != nullptr;
      for (uint32_t u = 0; u < urls->count; ++u) {
        const char* url = tj::CString(arena, urls->items[u]);
        if (single) check_case_url(arena, group, name, url, head, single, calls);
        else if (parallel)
          for (uint32_t r = 0; r < several->count; ++r)
            check_case_url(arena, group, name, url, head, several->items[r], calls);
        else check_case_url(arena, group, name, url, head, several->items[u], calls);
      }
    }
  }
}

void check_edges() {
  static char url[16384];
  size_t used = 0;
  auto start = [&](const char* text) {
    used = static_cast<size_t>(snprintf(url, sizeof(url), "%s", text));
  };
  auto append = [&](const char* text, int times) {
    for (int i = 0; i < times; ++i)
      used += static_cast<size_t>(snprintf(url + used, sizeof(url) - used, "%s", text));
  };

  // id: exactly 600 bytes (200 three-byte units) are kept; one more
  // non-whitespace byte overflows the buffer and is over 200 units.
  start("/o?days=7&id=");
  append("%E2%82%AC", 200);
  const Query& euro = parse(url);
  EDGE(euro.ok && euro.id_length == 600 && !euro.slug, "200 euro signs kept");
  append("a", 1);
  EDGE(!parse(url).ok, "601-byte id refused");
  // Whitespace past the buffer is trailing and trimmed.
  start("/o?days=7&id=");
  append("%E2%82%AC", 200);
  append("%E3%80%80", 500);
  const Query& padded = parse(url);
  EDGE(padded.ok && padded.id_length == 600, "trailing whitespace past the buffer trimmed");
  // …unless something follows it: interior whitespace counts.
  append("x", 1);
  EDGE(!parse(url).ok, "whitespace then a character past the buffer refused");
  start("/o?days=7&id=");
  append("%20", 2000);
  append("bitcoin", 1);
  append("%09", 2000);
  const Query& slug = parse(url);
  EDGE(slug.ok && slug.slug && strcmp(slug.id, "bitcoin") == 0, "2000-space padded slug");
  // 200 units of 'x' (one byte each) and 201.
  start("/o?days=7&id=");
  append("x", 199);
  append(":", 1);
  EDGE(parse(url).ok, "200 units");
  append("y", 1);
  EDGE(!parse(url).ok, "201 units");
  // Astral characters: two units each.
  start("/o?days=7&id=");
  append("%F0%9F%98%80", 100);
  EDGE(parse(url).ok, "100 astral characters");
  append("%F0%9F%98%80", 1);
  EDGE(!parse(url).ok, "101 astral characters");

  // A NUL survives decoding as id text (ParsePriceId refuses it).
  const Query& nul = parse("/o?days=7&id=spl%3A%00");
  EDGE(nul.ok && nul.id_length == 5 && nul.id[4] == '\0' && !nul.slug, "NUL kept");
  EDGE(!ParsePriceId(nul.id, nul.id_length, &g_parsed) || g_parsed.kind == PriceIdKind::kInvalid,
       "NUL id unsupported");

  // Null and empty query strings.
  fortis::prices::ohlc_query::ParseQuery(nullptr, 0, &g_query);
  EDGE(!g_query.ok, "null query");
  fortis::prices::ohlc_query::ParseQuery("", 0, &g_query);
  EDGE(!g_query.ok, "empty query");

  // days: every supported value and its neighbours; JavaScript Number().
  const int supported[] = {1, 7, 14, 30, 90, 180, 365};
  for (const int days : supported) {
    char text[64];
    snprintf(text, sizeof(text), "/o?id=bitcoin&days=%d", days);
    EDGE(parse(text).ok && g_query.days == days, "days %d", days);
    snprintf(text, sizeof(text), "/o?id=bitcoin&days=%d", days + 1);
    EDGE(!parse(text).ok, "days %d", days + 1);
  }
  EDGE(!parse("/o?id=bitcoin&days=-0").ok, "days -0");
  EDGE(!parse("/o?id=bitcoin&days=0x16d").ok, "days 0x16d");
  EDGE(!parse("/o?id=bitcoin&days=%E2%80%A8+1+%C2%A0").ok, "days whitespace");
  EDGE(parse("/o?id=bitcoin&days=1&days=1").ok, "days repeated");
  EDGE(!parse("/o?id=bitcoin&days").ok, "days flag");
  // A number value far beyond the request-target limit streams without a
  // buffer: 100,000 leading zeros.
  {
    static char big[100064];
    size_t n = static_cast<size_t>(snprintf(big, sizeof(big), "id=bitcoin&days="));
    memset(big + n, '0', 100000);
    n += 100000;
    n += static_cast<size_t>(snprintf(big + n, sizeof(big) - n, "30"));
    fortis::prices::ohlc_query::ParseQuery(big, n, &g_query);
    EDGE(g_query.ok && g_query.days == 30, "100,000 leading zeros");
  }

  // fiat.
  EDGE(parse("/o?id=bitcoin&days=1&fiat=usd").ok, "fiat usd");
  EDGE(!parse("/o?id=bitcoin&days=1&fiat=usdusd").ok, "fiat usdusd");
  EDGE(!parse("/o?id=bitcoin&days=1&fiat=us").ok, "fiat us");
  EDGE(parse("/o?id=bitcoin&days=1&fiat=usd&fiat=usd").ok, "fiat repeated");

  // Keys: decoded, exact, and unknown keys of any length are ignored.
  EDGE(parse("/o?%69%64=bitcoin&%64ays=1").ok, "encoded keys");
  EDGE(!parse("/o?idx=bitcoin&days=1").ok, "idx is not id");
  start("/o?id=bitcoin&days=1&");
  append("k", 3000);
  append("=", 1);
  append("v", 9000);
  EDGE(parse(url).ok, "long unknown pair ignored");

  // The dispatch regex.
  EDGE(IsCoinGeckoSlug("a", 1) && IsCoinGeckoSlug("0", 1) && IsCoinGeckoSlug("a-", 2),
       "short slugs");
  char hundred[102];
  memset(hundred, 'a', sizeof(hundred));
  EDGE(IsCoinGeckoSlug(hundred, 100) && !IsCoinGeckoSlug(hundred, 101), "100-character bound");
  EDGE(!IsCoinGeckoSlug("-a", 2) && !IsCoinGeckoSlug("A", 1) && !IsCoinGeckoSlug("a_b", 3) &&
           !IsCoinGeckoSlug("a:b", 3) && !IsCoinGeckoSlug("", 0) && !IsCoinGeckoSlug("a b", 3),
       "non-slugs");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s price_ohlc_query_route.json\n", argv[0]);
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
  printf("price_ohlc_query_test: %d validation 400s, %d unsupported ids, %d store reads, "
         "%d slug reads, %d edges OK\n",
         g_checked_400, g_checked_unsupported, g_checked_store, g_checked_slug, g_checked_edges);
  return 0;
}
