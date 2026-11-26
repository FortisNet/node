#include <memory>
#include "test_support.h"
// R28–R30 news reads: unit checks of the handlers' pure rules against the
// TypeScript oracle's `units` (tests/corpus/news_reads_edges.json):
//   ids          String trim + toUpperCase + the route's ULID pattern
//                (news_read_rules::NormalizeId, R29/R30)
//   feedQuery    the real route's 400 body or selected SWR key per query
//                (news_read_rules::FeedQueryOf, R28)
//   reactions    cleanSamples + measureReaction (MeasureReaction, R30)
//   pricesValue  marketChartRange's `prices ?? []` as stored (PricesValue, R30)
// plus two native-only properties:
//   - the msgpack round trip of lone surrogates the news store applies on
//     write (shorter than 64 units: three U+FFFD each; longer: one);
//   - FeedSnapshot under concurrent Persist: every feed snapshot's newest
//     story and lastIngestAt come from the same side of each persist.
// Usage: news_reads_test <fixture>
#include "strict_unit_expectations.h"
#include "kv_store.h"
#include "news_routes.h"
#include "news_store.h"
#include "token_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <array>
#include <barrier>
#include <exception>
#include <mutex>
#include <thread>

using namespace fortis::news;

namespace tj = fortis::codec::tj;
namespace news = fortis::news;

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const char* group, const char* label, const char* detail = "") {
  ++checks;
  if (ok) return;
  ++failures;
  if (failures <= 40) fprintf(stderr, "FAIL %s %s %s\n", group, label, detail);
}

const char* text(const tj::Value* value) { return tj::IsString(value) ? value->text : nullptr; }

// UTF-16 code units -> WTF-8 (a lone surrogate keeps its three bytes, which
// no URL path can carry and ValidUtf8 refuses).
size_t wtf8(const uint16_t* units, size_t count, char* out) {
  size_t used = 0;
  for (size_t i = 0; i < count; ++i) {
    uint32_t cp = units[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < count && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF)
      cp = 0x10000 + ((cp - 0xD800) << 10) + (units[++i] - 0xDC00);
    if (cp < 0x80) out[used++] = static_cast<char>(cp);
    else if (cp < 0x800) { out[used++] = static_cast<char>(0xC0 | cp >> 6); out[used++] = static_cast<char>(0x80 | (cp & 63)); }
    else if (cp < 0x10000) {
      out[used++] = static_cast<char>(0xE0 | cp >> 12);
      out[used++] = static_cast<char>(0x80 | ((cp >> 6) & 63));
      out[used++] = static_cast<char>(0x80 | (cp & 63));
    } else {
      out[used++] = static_cast<char>(0xF0 | cp >> 18);
      out[used++] = static_cast<char>(0x80 | ((cp >> 12) & 63));
      out[used++] = static_cast<char>(0x80 | ((cp >> 6) & 63));
      out[used++] = static_cast<char>(0x80 | (cp & 63));
    }
  }
  return used;
}

void ids(const tj::Value* rows) {
  for (uint32_t r = 0; r < rows->count; ++r) {
    const tj::Value* row = rows->items[r];
    const char* hex = text(tj::Get(row, "rawHex"));
    const size_t bytes = strlen(hex) / 2;
    uint16_t units[256]{};
    if (bytes / 2 > 256) { check(false, "ids", "UTF-16 fixture capacity"); continue; }
    for (size_t i = 0; i + 1 < bytes && i / 2 < 256; i += 2) {
      unsigned lo = 0, hi = 0;
      if (!fortis::test::ParseHexByte(hex + i * 2, &lo)) throw std::runtime_error("invalid hex in test fixture");
      if (!fortis::test::ParseHexByte(hex + i * 2 + 2, &hi)) throw std::runtime_error("invalid hex in test fixture");
      units[i / 2] = static_cast<uint16_t>(lo | hi << 8);
    }
    char utf8[1024];
    const size_t length = wtf8(units, bytes / 2, utf8);
    char id[27] = {};
    const bool valid = news_read_rules::NormalizeId(utf8, length, id);
    // js-case: the id is ASCII upper-cased, so a non-ASCII
    // character that TypeScript's toUpperCase mapped into the ULID alphabet
    // (ß -> SS, ſ -> S, ﬀ -> FF) leaves the id malformed. Non-ASCII JS
    // whitespace is trimmed first and does not count.
    bool ascii = true;
    for (size_t i = 0; i < bytes / 2; ++i) {
      const uint16_t u = units[i];
      const bool space = u == 0xA0 || u == 0x1680 || (u >= 0x2000 && u <= 0x200A) || u == 0x2028 ||
                         u == 0x2029 || u == 0x202F || u == 0x205F || u == 0x3000 || u == 0xFEFF;
      ascii = ascii && (u < 0x80 || space);
    }
    const bool expected = tj::Get(row, "valid")->type == tj::Type::kTrue && ascii;
    char label[64];
    snprintf(label, sizeof(label), "row %u (%.40s)", r, hex);
    check(valid == expected, "ids", label, expected ? "expected valid" : "expected malformed");
    if (valid && expected) check(strcmp(id, text(tj::Get(row, "id"))) == 0, "ids", label, id);
  }
}

// compat-decisions.md#zod-issues: where the oracle listed zod
// issues this server answers the plain BAD_REQUEST body.
const char* expected_refusal(const char* oracle) {
  static const char kZodPrefix[] = "{\"error\":\"BAD_REQUEST\",\"detail\":[";
  if (oracle && strncmp(oracle, kZodPrefix, strlen(kZodPrefix)) == 0) return "{\"error\":\"BAD_REQUEST\"}";
  return oracle;
}

void feed_queries(const tj::Value* rows) {
  for (uint32_t r = 0; r < rows->count; ++r) {
    const tj::Value* row = rows->items[r];
    const char* query = text(tj::Get(row, "query"));
    const int status = static_cast<int>(tj::Get(row, "status")->number);
    char target[8192];
    snprintf(target, sizeof(target), "/v1/news%s", query);
    // A '#' stays in the query (fast-querystring reads it as value bytes);
    // over HTTP the native parser refuses it first (rule 'native parser 400').
    bool valid = false;
    char key[512] = {}, body[4096] = {};
    const bool parsed = news_read_rules::FeedQueryOf(target, &valid, key, sizeof(key), body, sizeof(body));
    char label[160];
    snprintf(label, sizeof(label), "%.120s", query);
    check(parsed, "feedQuery", label, "native refused");
    if (!parsed) continue;
    if (status == 200) {
      const char* expected = text(tj::Get(row, "key"));
      check(valid && expected && strcmp(key, expected) == 0, "feedQuery", label, key);
    } else {
      const char* expected = expected_refusal(text(tj::Get(row, "body")));
      check(!valid && expected && strcmp(body, expected) == 0, "feedQuery", label, body);
    }
  }
}

void cursor_query() {
  constexpr const char* kId = "01K2C8ZR4G0000000000000000";
  bool valid = false;
  char key[512] = {}, body[4096] = {};
  const bool parsed = news_read_rules::FeedQueryOf(
      "/v1/news?limit=50&from=1793790000000&beforeId=01K2C8ZR4G0000000000000000",
      &valid, key, sizeof(key), body, sizeof(body));
  check(parsed && valid && strcmp(key, "news:|50|1793790000000|01K2C8ZR4G0000000000000000") == 0,
        "feedQuery.cursor", "valid timestamp and ID", key);
  const bool invalid = news_read_rules::FeedQueryOf(
      "/v1/news?limit=50&beforeId=01K2C8ZR4G0000000000000000",
      &valid, key, sizeof(key), body, sizeof(body));
  check(invalid && !valid, "feedQuery.cursor", "beforeId requires from", kId);
}

// Every feedQuery row parsed by 8 threads at once, 200 passes each in a
// different row order: each answer must equal the row's own TypeScript
// answer, so parse scratch shared between concurrent requests shows up as
// a wrong key or a wrong 400 detail.
// libc++ 18 keeps jthread experimental; own and join the standard thread.
struct JoinThread {
  void operator()(std::thread* thread) const noexcept {
    const std::unique_ptr<std::thread> owned{thread};
    if (owned && owned->joinable()) owned->join();
  }
};

struct QueryRun {
  const tj::Value* rows;
  int offset;
  int mismatches;
};

void query_thread(QueryRun* run) noexcept {
  try {
    const uint32_t count = run->rows->count;
    for (int pass = 0; pass < 200; ++pass) {
      for (uint32_t k = 0; k < count; ++k) {
        const tj::Value* row = run->rows->items[(k * 7 + static_cast<uint32_t>(run->offset + pass)) % count];
        char target[8192];
        snprintf(target, sizeof(target), "/v1/news%s", text(tj::Get(row, "query")));
        bool valid = false;
        char key[512] = {}, body[4096] = {};
        if (!news_read_rules::FeedQueryOf(target, &valid, key, sizeof(key), body, sizeof(body))) { ++run->mismatches; continue; }
        const bool ok = static_cast<int>(tj::Get(row, "status")->number) == 200
            ? valid && strcmp(key, text(tj::Get(row, "key"))) == 0
            : !valid && strcmp(body, expected_refusal(text(tj::Get(row, "body")))) == 0;
        if (!ok) ++run->mismatches;
      }
    }
  } catch (...) {
    ++run->mismatches;
  }
}

void concurrent_queries(const tj::Value* rows) {
  std::array<QueryRun, 8> runs{};
  {
    std::array<std::unique_ptr<std::thread, JoinThread>, 8> threads{};
    for (size_t i = 0; i < runs.size(); ++i) {
      runs[i] = QueryRun{rows, static_cast<int>(i) * 11, 0};
      threads[i].reset(std::make_unique<std::thread>(query_thread, &runs[i]).release());
    }
  }
  int mismatches = 0;
  for (const auto& run : runs) mismatches += run.mismatches;
  char detail[64];
  snprintf(detail, sizeof(detail), "%d wrong answers", mismatches);
  check(mismatches == 0, "feedQuery.concurrent", "8 threads x 200 passes", detail);
  printf("feedQuery.concurrent: %u parses, %d wrong\n", rows->count * 8 * 200, mismatches);
}

double number_of(const tj::Value* value) {
  if (tj::IsNumber(value)) return value->number;
  const char* t = text(value);
  if (!t) return NAN;
  if (strcmp(t, "Infinity") == 0) return INFINITY;
  if (strcmp(t, "-Infinity") == 0) return -INFINITY;
  return NAN;
}

void reactions(const tj::Value* rows) {
  static const int kWindows[4] = {300, 900, 3600, 86400};
  for (uint32_t r = 0; r < rows->count; ++r) {
    const tj::Value* row = rows->items[r];
    const char* name = text(tj::Get(row, "name"));
    const tj::Value* points = tj::Get(row, "points");
    const int64_t now = static_cast<int64_t>(tj::Get(row, "now")->number);
    const int64_t published = static_cast<int64_t>(tj::Get(row, "published")->number);
    double* at = static_cast<double*>(calloc(points->count + 1, sizeof(double)));
    double* price = static_cast<double*>(calloc(points->count + 1, sizeof(double)));
    double* cleaned_at = static_cast<double*>(calloc(points->count + 1, sizeof(double)));
    double* cleaned_price = static_cast<double*>(calloc(points->count + 1, sizeof(double)));
    for (uint32_t i = 0; i < points->count; ++i) {
      at[i] = number_of(points->items[i]->items[0]);
      price[i] = number_of(points->items[i]->items[1]);
    }
    const tj::Value* cleaned = tj::Get(row, "cleaned");
    const tj::Value* windows = tj::Get(row, "windows");
    for (int w = 0; w < 4; ++w) {
      NewsReactionWindow window;
      const size_t count = news_read_rules::MeasureReaction(at, price, points->count, published, now,
                                                            kWindows[w], cleaned_at, cleaned_price, &window);
      char label[96];
      snprintf(label, sizeof(label), "%s/%d", name, kWindows[w]);
      if (w == 0) {
        bool same = count == cleaned->count;
        for (size_t i = 0; same && i < count; ++i)
          same = cleaned->items[i]->items[0]->number == cleaned_at[i] &&
                 cleaned->items[i]->items[1]->number == cleaned_price[i];
        check(same, "reactions.cleaned", label);
      }
      const tj::Value* expected = windows->items[w];
      const char* status = text(tj::Get(expected, "status"));
      const int want = strcmp(status, "pending") == 0 ? 0 : strcmp(status, "unavailable") == 0 ? 1 : 2;
      check(window.status == want, "reactions.status", label, status);
      if (want != 2 || window.status != 2) continue;
      const tj::Value* baseline = tj::Get(expected, "baseline");
      const tj::Value* endpoint = tj::Get(expected, "endpoint");
      check(window.return_pct == tj::Get(expected, "returnPct")->number, "reactions.returnPct", label);
      check(window.baseline[0] == baseline->items[0]->number && window.baseline[1] == baseline->items[1]->number &&
                window.endpoint[0] == endpoint->items[0]->number && window.endpoint[1] == endpoint->items[1]->number,
            "reactions.samples", label);
    }
    free(at); free(price); free(cleaned_at); free(cleaned_price);
  }
}

void prices(const tj::Value* rows) {
  const size_t capacity = 2 * 1024 * 1024;
  uint8_t* out = static_cast<uint8_t*>(malloc(capacity));
  for (uint32_t r = 0; r < rows->count; ++r) {
    const tj::Value* row = rows->items[r];
    const tj::Value* body = tj::Get(row, "body");
    size_t used = 0;
    const bool ok = news_read_rules::PricesValue(reinterpret_cast<const uint8_t*>(body->text), body->length, out,
                                                 capacity, &used);
    const char* value = text(tj::Get(row, "value"));
    char label[96];
    snprintf(label, sizeof(label), "row %u (%.50s)", r, body->text);
    if (!value) { check(!ok, "pricesValue", label, "expected the fetcher's failure"); continue; }
    check(ok && used == strlen(value) && memcmp(out, value, used) == 0, "pricesValue", label,
          ok ? reinterpret_cast<const char*>(out) : "native failure");
  }
  free(out);
}

// ---- the store ----------------------------------------------------------------------

struct Story {
  char json[1024];
};

void story_json(Story* story, const char* id, const char* headline_escaped, int64_t published) {
  snprintf(story->json, sizeof(story->json),
           "{\"id\":\"%s\",\"headline\":\"%s\",\"summary\":\"s\",\"topics\":[\"trend\"],\"impact\":\"normal\","
           "\"tickers\":[],\"publisher\":\"P\",\"url\":\"https://n.example/%s\",\"imageUrl\":null,"
           "\"imageMediaKey\":null,\"publishedAt\":%lld,\"articles\":[{\"sourceId\":\"s\",\"publisher\":\"P\","
           "\"externalId\":\"%s\",\"title\":\"t\",\"summary\":\"\",\"url\":\"https://n.example/%s\","
           "\"imageUrl\":null,\"publishedAt\":%lld}]}",
           id, headline_escaped, id, static_cast<long long>(published), id, id, static_cast<long long>(published));
}

int64_t g_clock = 0;
std::mutex g_clock_mutex;
struct TestClock final : fortis::base::Clock {
  int64_t NowMs() override;
} g_test_clock;
int64_t TestClock::NowMs() {
  const std::lock_guard lock(g_clock_mutex);
  return g_clock;
}

constexpr int64_t kBase = 1809928800000LL;  // 2027-05-10T06:00:00Z

void surrogate_rule(fortis::storage::KvStore* kv) {
  const news::NewsStore store{kv, nullptr};
  const size_t bytes = 8 * 1024 * 1024;
  auto memory_owner = std::make_unique<uint8_t[]>(bytes);
  uint8_t* memory = memory_owner.get();
  news::Arena arena{memory, bytes};
  // Short (< 64 units): a lone high and a lone low surrogate; long: one.
  char long_headline[400];
  snprintf(long_headline, sizeof(long_headline), "%s\\udc00%s", "x x x x x x x x x x x x x x x x x x x x x",
           " y y y y y y y y y y y y y y y y y y y y");
  Story short_story, long_story;
  story_json(&short_story, "01KQSURR0GATE000000000000S", "a\\ud800b\\udc00c\\ud83d\\ude00", kBase);
  story_json(&long_story, "01KQSURR0GATE000000000000L", long_headline, kBase);
  news::Cluster clusters[2];
  news::Cluster* pointers[2] = {&clusters[0], &clusters[1]};
  bool ok = news::DecodeCluster(reinterpret_cast<const uint8_t*>(short_story.json), strlen(short_story.json),
                                &arena, &clusters[0]) &&
            news::DecodeCluster(reinterpret_cast<const uint8_t*>(long_story.json), strlen(long_story.json), &arena,
                                &clusters[1]);
  g_clock = kBase;
  ok = ok && news::Persist(store, pointers, 2, g_test_clock, &arena) == news::StoreStatus::kOk;
  check(ok, "store.surrogates", "persist");
  news::Cluster read;
  bool found = false;
  ok = ok && news::Item(store, "01KQSURR0GATE000000000000S", kBase, &arena, &read, &found) == news::StoreStatus::kOk &&
       found;
  // a, FFFD x3, b, FFFD x3, c, U+1F600 (a pair is not lone).
  static const uint16_t kShort[] = {'a', 0xFFFD, 'b', 0xFFFD, 'c', 0xD83D, 0xDE00};
  check(ok && read.headline.length == 7 && memcmp(read.headline.data, kShort, sizeof(kShort)) == 0,
        "store.surrogates", "short string: one U+FFFD per lone surrogate");
  ok = news::Item(store, "01KQSURR0GATE000000000000L", kBase, &arena, &read, &found) == news::StoreStatus::kOk && found;
  size_t replacement = 0;
  for (size_t i = 0; ok && i < read.headline.length; ++i) replacement += read.headline.data[i] == 0xFFFD ? 1 : 0;
  check(ok && replacement == 1 && read.headline.length == 41 + 1 + 40, "store.surrogates",
        "long string: one U+FFFD per lone surrogate");
  // Legacy stored replacement characters are data, not a pattern to
  // collapse. Reading an older record must preserve all three U+FFFDs.
  Story legacy;
  story_json(&legacy, "01KQSURR0GATE000000000000S", "a\\ufffd\\ufffd\\ufffdb", kBase);
  const fortis::storage::KvMutation old_record{"news/item/01KQSURR0GATE000000000000S", legacy.json,
      static_cast<uint32_t>(strlen(legacy.json)), 0, false};
  ok = kv->Write(&old_record, 1) &&
      news::Item(store, "01KQSURR0GATE000000000000S", kBase, &arena, &read, &found) == news::StoreStatus::kOk && found;
  const uint16_t old_text[] = {'a', 0xfffd, 0xfffd, 0xfffd, 'b'};
  check(ok && read.headline.length == 5 && !memcmp(read.headline.data, old_text, sizeof(old_text)),
        "store.surrogates", "legacy replacement text preserved");

}

bool id_equals(news::U16 actual, const char* expected) {
  if (actual.length != strlen(expected)) return false;
  for (size_t i = 0; i < actual.length; ++i)
    if (actual.data[i] != static_cast<uint16_t>(expected[i])) return false;
  return true;
}

void cursor_pagination(fortis::storage::KvStore* kv) {
  const news::NewsStore store{kv, nullptr};
  auto memory_owner = std::make_unique<uint8_t[]>(4 * 1024 * 1024);
  uint8_t* memory = memory_owner.get();
  news::Arena arena{memory, 4 * 1024 * 1024};
  Story stories[3];
  news::Cluster clusters[3];
  news::Cluster* pointers[3] = {&clusters[0], &clusters[1], &clusters[2]};
  char ids[3][27];
  bool ok = memory != nullptr;
  for (int i = 0; ok && i < 3; ++i) {
    snprintf(ids[i], sizeof(ids[i]), "01KQ%022d", i + 1);
    story_json(&stories[i], ids[i], "page", kBase + 60000);
    ok = news::DecodeCluster(reinterpret_cast<const uint8_t*>(stories[i].json),
                             strlen(stories[i].json), &arena, &clusters[i]);
  }
  g_clock = kBase + 60000;
  ok = ok && news::Persist(store, pointers, 3, g_test_clock, &arena) == news::StoreStatus::kOk;
  check(ok, "store.cursor", "persist");
  if (ok) {
    news::Arena read_arena{memory, 4 * 1024 * 1024};
    news::Cluster* page = nullptr;
    size_t count = 0;
    bool found = false;
    int64_t last = 0;
    ok = news::FeedSnapshot(store, nullptr, 0, 2, false, 0, nullptr,
                            kBase + 60000, &read_arena, &page, &count, &found, &last) == news::StoreStatus::kOk;
    check(ok && count == 2 && id_equals(page[0].id, ids[2]) && id_equals(page[1].id, ids[1]),
          "store.cursor", "first page uses descending ID at equal timestamp");
    read_arena = news::Arena{memory, 4 * 1024 * 1024};
    ok = news::FeedSnapshot(store, nullptr, 0, 2, true, kBase + 60000, ids[1],
                            kBase + 60000, &read_arena, &page, &count, &found, &last) == news::StoreStatus::kOk;
    check(ok && count == 2 && id_equals(page[0].id, ids[0]),
          "store.cursor", "second page excludes boundary ID and keeps equal timestamp");
  }

}

struct Race {
  fortis::storage::KvStore* kv;
  int stories;
  std::barrier<> phase{2};
  int persisted = 0;
  std::exception_ptr failure{};
};

// Drop participation even during unwinding so the peer can finish and join.
class RaceParticipant {
 public:
  explicit RaceParticipant(std::barrier<>& phase) : phase_(phase) {}
  ~RaceParticipant() { phase_.arrive_and_drop(); }
  RaceParticipant(const RaceParticipant&) = delete;
  RaceParticipant& operator=(const RaceParticipant&) = delete;
 private:
  std::barrier<>& phase_;
};

void race_writer(Race* race) noexcept {
  const RaceParticipant participant(race->phase);
  try {
    const news::NewsStore store{race->kv, nullptr};
    const size_t bytes = 4 * 1024 * 1024;
    auto memory_owner = std::make_unique<uint8_t[]>(bytes);
    uint8_t* memory = memory_owner.get();
    for (int i = 1; i <= race->stories; ++i) {
      news::Arena arena{memory, bytes};
      char id[27];
      snprintf(id, sizeof(id), "01KR%022d", i);
      Story story;
      story_json(&story, id, "race", kBase + 3600000LL + i);
      news::Cluster cluster;
      news::Cluster* pointer = &cluster;
      if (!news::DecodeCluster(reinterpret_cast<const uint8_t*>(story.json), strlen(story.json), &arena, &cluster))
        throw std::runtime_error("cannot decode snapshot race fixture");
      {
        const std::lock_guard lock(g_clock_mutex);
        g_clock = kBase + 7200000LL + i;
      }
      race->phase.arrive_and_wait();
      if (news::Persist(store, &pointer, 1, g_test_clock, &arena) != news::StoreStatus::kOk)
        throw std::runtime_error("snapshot race persist failed");
      race->persisted = i;
      race->phase.arrive_and_wait();
    }
  } catch (...) {
    race->failure = std::current_exception();
  }
}

// Each snapshot's newest story (limit 1) is story i exactly when its
// lastIngestAt is persist i's stamp: a torn read pairs story i with i ± 1.
void snapshot_race(fortis::storage::KvStore* kv) {
  Race race{kv, 1500};
  const news::NewsStore store{kv, nullptr};
  const size_t bytes = 4 * 1024 * 1024;
  auto memory_owner = std::make_unique<uint8_t[]>(bytes);
  uint8_t* memory = memory_owner.get();
  size_t snapshots = 0, torn = 0;
  auto snapshot = [&] {
    news::Arena arena{memory, bytes};
    news::Cluster* clusters = nullptr;
    size_t count = 0;
    bool found = false;
    int64_t last = 0;
    if (news::FeedSnapshot(store, nullptr, 0, 1, false, 0, nullptr,
                           kBase + 8 * 3600000LL, &arena, &clusters, &count, &found, &last) !=
        news::StoreStatus::kOk) { ++torn; return; }
    ++snapshots;
    if (!count || !found) return;
    const int64_t newest = static_cast<int64_t>(clusters[0].published_ms) - (kBase + 3600000LL);
    if (newest <= 0) return;  // the surrogate stories precede the race
    if (last - (kBase + 7200000LL) != newest) ++torn;
  };
  {
    const std::unique_ptr<std::thread, JoinThread> writer{
        std::make_unique<std::thread>(race_writer, &race).release()};
    const RaceParticipant reader(race.phase);
    // Release both operations together, then finish the pair before the next.
    // Work is bounded without assuming that a hot reader yields a mutex fairly.
    for (int i = 0; i < race.stories; ++i) {
      race.phase.arrive_and_wait();
      snapshot();
      race.phase.arrive_and_wait();
    }
  }
  if (race.failure) std::rethrow_exception(race.failure);

  char detail[96];
  snprintf(detail, sizeof(detail), "%zu of %zu snapshots torn", torn, snapshots);
  const int persisted = race.persisted;
  check(torn == 0 && snapshots == static_cast<size_t>(race.stories) && persisted == race.stories, "store.snapshot",
        "feed and lastIngestAt from one side of each persist", detail);
  printf("store.snapshot: %zu snapshots during %d/%d persists, %zu torn\n",
         snapshots, persisted, race.stories, torn);
}

}  // namespace

int main(int argc, char** argv) try {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  puts("news reads: loading fixture");
  if (argc != 2) {
    fprintf(stderr, "usage: news_reads_test <news_reads_edges.json>\n");
    return 2;
  }
  std::vector<uint8_t> fixture_bytes;
  if (!fortis::test::ReadFixture(argv[1], fixture_bytes)) return 2;
  const size_t size = fixture_bytes.size();
  uint8_t* data = fixture_bytes.data();
  tj::Arena arena = tj::GrowingArena(512u * 1024 * 1024);
  const tj::Value* fixture = tj::Parse(arena, data, static_cast<size_t>(size));
  if (!strict::ContractExpectations(arena, fixture, "news_reads_edges.json")) return 2;
  strict::PatchRows(arena, tj::Get(tj::Get(fixture, "units"), "feedQuery"), tj::Get(strict::UnitExpectations(arena), "news"), "query");
  const tj::Value* units = tj::Get(fixture, "units");
  if (!units) { fprintf(stderr, "no units\n"); return 2; }
  ids(tj::Get(units, "ids"));
  feed_queries(tj::Get(units, "feedQuery"));
  cursor_query();
  puts("news reads: concurrent query parsing");
  concurrent_queries(tj::Get(units, "feedQuery"));
  reactions(tj::Get(units, "reactions"));
  prices(tj::Get(units, "pricesValue"));
  printf("units: %u ids, %u feed queries, %u reaction sets x 4 windows, %u price bodies\n",
         tj::Get(units, "ids")->count, tj::Get(units, "feedQuery")->count, tj::Get(units, "reactions")->count,
         tj::Get(units, "pricesValue")->count);

  char directory[] = "/tmp/news_reads_test_XXXXXX";
  if (!mkdtemp(directory)) return 2;
  {
    fortis::storage::KvStore kv;
    if (!kv.Open(directory)) { fprintf(stderr, "kv open failed\n"); return 2; }
    surrogate_rule(&kv);
    cursor_pagination(&kv);
    puts("news reads: concurrent snapshots and persists");
    snapshot_race(&kv);
  }
  fortis::test::RemoveTree(directory);
  tj::FreeArena(arena);

  printf("news reads units: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
} catch (...) {
  return fortis::test::ReportException();
}
