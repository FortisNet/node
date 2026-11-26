#include "test_support.h"
#include "strict_unit_expectations.h"
// R11 unit test of GET /v1/networks/robinhood/stock-tokens below the socket:
// StockClient over a FetchFn + robinhood::Catalog + Stringify/EncodeCatalog
// against the TypeScript oracle (robinhood_stock_catalog_edges.json):
//  - every recorded catalog document (populated with duplicate, mixed-case
//    and invalid instruments, empty, all-invalid, extra members, the ASCII
//    and non-ASCII ordering tables, the 512 KiB maximum): exact JSON and
//    protobuf bytes (sha256 + length beyond 64 KiB; the ASCII and non-ASCII
//    ordering tables are compared as the same rows, now in
//    RobinhoodSymbolCompare order: locale-compare), and the 2 MiB-plus
//    expansion catalog's bytes, which the
//    route refuses only for size;
//  - every recorded shape failure fails parseAssets;
//  - the cache: fresh to 899,999 ms, a refetch at 900,000 ms, a failure is
//    never cached and never serves the expired catalog;
//  - one upstream request for simultaneous cold reads (success and failure),
//    and the issued-generation rule: a read queued while a request failed
//    takes that failure instead of a second request;
//  - a lane refusal keeps its retry-after; media records are written once per
//    display logo and an existing record is never overwritten;
//  - the symbol order pairs (ASCII case folding, then bytes) and an
//    undersized protobuf buffer.
// The real-socket contract is tests/robinhood_stock_catalog_edges_test.py.
// Usage: robinhood_stock_catalog_test tests/corpus/robinhood_stock_catalog_edges.json
#include "fake_seams.h"
#include "kv_store.h"
#include "robinhood_markets.h"
#include "token_search_route.h"

#include <mbedtls/sha256.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

using namespace fortis::discovery;

namespace rh = fortis::exchange::robinhood;
namespace tj = fortis::codec::tj;

namespace {

int failures = 0;
int checks = 0;
bool check(bool condition, const char* what) {
  ++checks;
  if (!condition) {
    ++failures;
    fprintf(stderr, "FAIL: %s\n", what);
  }
  return condition;
}

int64_t g_now = 0;
fortis::testing::PointerClock g_clock(&g_now);
uint64_t far_deadline() { return static_cast<uint64_t>(-1) / 2; }

struct Fake final : rh::StockClient::Fetcher {
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  const char* body = nullptr;
  size_t length = 0;
  rh::Failure failure = rh::Failure::kNone;
  uint32_t retry_after_ms = 0;
  useconds_t delay_us = 0;
  int calls = 0;
  bool wrong_path = false;
  rh::Outcome Fetch(tj::Arena& arena, uint64_t deadline_ms, const char* path, bool background,
                    tj::Value** body) override;
};

void serve(Fake& fake, const char* body, size_t length) {
  pthread_mutex_lock(&fake.mutex);
  fake.body = body;
  fake.length = length;
  fake.failure = rh::Failure::kNone;
  pthread_mutex_unlock(&fake.mutex);
}
void serve(Fake& fake, const char* body) { serve(fake, body, strlen(body)); }
void refuse(Fake& fake, rh::Failure failure, uint32_t retry_after_ms = 0) {
  pthread_mutex_lock(&fake.mutex);
  fake.failure = failure;
  fake.retry_after_ms = retry_after_ms;
  pthread_mutex_unlock(&fake.mutex);
}
int calls(Fake& fake) {
  pthread_mutex_lock(&fake.mutex);
  const int n = fake.calls;
  pthread_mutex_unlock(&fake.mutex);
  return n;
}

// HttpClient.json: a refusal, or the 2xx body parsed (unparseable fails).
rh::Outcome Fake::Fetch(tj::Arena& arena, uint64_t, const char* path, bool, tj::Value** body) {
  Fake* fake = this;
  pthread_mutex_lock(&fake->mutex);
  ++fake->calls;
  if (strcmp(path, "/rhj/assets") != 0) fake->wrong_path = true;
  const char* text = fake->body;
  const size_t length = fake->length;
  const rh::Failure failure = fake->failure;
  const uint32_t retry = fake->retry_after_ms;
  const useconds_t delay = fake->delay_us;
  pthread_mutex_unlock(&fake->mutex);
  if (delay) usleep(delay);
  rh::Outcome outcome;
  if (failure != rh::Failure::kNone) {
    outcome.failure = failure;
    outcome.retry_after_ms = retry;
    return outcome;
  }
  *body = tj::Parse(arena, reinterpret_cast<const uint8_t*>(text), length);
  if (!*body) outcome.failure = rh::Failure::kError;
  return outcome;
}

struct Encoded {
  uint8_t* bytes = nullptr;
  size_t length = 0;
  bool ok = false;
};

Encoded encode(const tj::Value* value, bool protobuf) {
  Encoded e;
  tj::OutBuffer measure{nullptr, 0, 0, true};
  if (!(protobuf ? rh::EncodeCatalog(measure, value) : tj::Stringify(value, measure)) || !measure.ok) return e;
  e.bytes = static_cast<uint8_t*>(malloc(measure.used ? measure.used : 1));
  tj::OutBuffer out{e.bytes, measure.used, 0, e.bytes != nullptr};
  e.ok = e.bytes && (protobuf ? rh::EncodeCatalog(out, value) : tj::Stringify(value, out)) && out.ok &&
         out.used == measure.used;
  e.length = out.used;
  return e;
}

// One catalog read: the outcome, and both encodings on success.
struct Read {
  rh::Outcome outcome;
  Encoded json;
  Encoded protobuf;
  void Free() {
    free(json.bytes);
    free(protobuf.bytes);
  }
};

Read read(rh::StockClient& client, fortis::storage::KvStore* kv, uint64_t issued = rh::StockClient::kNotIssued) {
  Read r;
  tj::Arena arena = tj::GrowingArena(64u << 20);
  rh::MarketsRequest request;
  request.client = &client;
  request.kv = kv;
  request.now_ms = g_now;
  request.deadline_ms = far_deadline();
  request.assets_issued = issued;
  tj::Value* result = nullptr;
  std::shared_ptr<const rh::StockClient::Snapshot> held = nullptr;
  r.outcome = rh::Catalog(arena, request, &result, &held);
  if (r.outcome.ok()) {
    r.json = encode(result, false);
    r.protobuf = encode(result, true);
  }
  held.reset();
  tj::FreeArena(arena);
  return r;
}

void sha256_hex(const uint8_t* bytes, size_t length, char out[65]) {
  uint8_t digest[32];
  mbedtls_sha256(bytes, length, digest, 0);
  for (int i = 0; i < 32; ++i) snprintf(out + 2 * i, 3, "%02x", digest[i]);
}

bool same_hex(const Encoded& e, const tj::Value* hex) {
  if (!tj::IsString(hex) || hex->length != e.length * 2) return false;
  char pair[3];
  for (size_t i = 0; i < e.length; ++i) {
    snprintf(pair, sizeof(pair), "%02x", e.bytes[i]);
    if (memcmp(pair, hex->text + 2 * i, 2) != 0) return false;
  }
  return true;
}

// {text}|{hex}|{sha256,length} of the oracle against an encoding.
bool matches(const Encoded& e, const tj::Value* expected) {
  if (!e.ok || !expected) return false;
  if (const tj::Value* text = tj::Get(expected, "text"))
    return tj::IsString(text) && text->length == e.length && memcmp(text->text, e.bytes, e.length) == 0;
  if (const tj::Value* hex = tj::Get(expected, "hex")) return same_hex(e, hex);
  const tj::Value* sha = tj::Get(expected, "sha256");
  const tj::Value* length = tj::Get(expected, "length");
  char digest[65];
  sha256_hex(e.bytes, e.length, digest);
  return tj::IsString(sha) && tj::IsNumber(length) && static_cast<size_t>(length->number) == e.length &&
         memcmp(sha->text, digest, 64) == 0;
}

const tj::Value* g_fixture = nullptr;

// A provider entry's body (inline, or from `bodies` by sha256).
const tj::Value* provider_body(const tj::Value* entry) {
  if (const tj::Value* body = tj::Get(entry, "body")) return body;
  const tj::Value* sha = tj::Get(entry, "bodySha256");
  return tj::IsString(sha) ? tj::Get(tj::Get(g_fixture, "bodies"), sha->text, sha->length) : nullptr;
}

const tj::Value* find_case(const char* name) {
  const tj::Value* cases = tj::Get(g_fixture, "cases");
  for (uint32_t i = 0; tj::IsArray(cases) && i < cases->count; ++i) {
    const tj::Value* c = cases->items[i];
    const tj::Value* n = tj::Get(c, "name");
    if (tj::IsString(n) && strcmp(n->text, name) == 0) return c;
  }
  return nullptr;
}

const tj::Value* first_assets_reply(const tj::Value* c) {
  const tj::Value* list = tj::Get(tj::Get(c, "providers"), "GET api.robinhood.com/rhj/assets");
  return tj::IsArray(list) && list->count ? list->items[0] : nullptr;
}

// The protobuf field-1 rows of a catalog, as (offset, length) pairs.
size_t rows_of(const uint8_t* bytes, size_t length, size_t* starts, size_t* lengths, size_t capacity) {
  size_t count = 0, position = 0;
  while (position < length && count < capacity) {
    const uint8_t tag = bytes[position++];
    size_t size = 0;
    for (int shift = 0;; shift += 7) {
      const uint8_t b = bytes[position++];
      size |= static_cast<size_t>(b & 0x7F) << shift;
      if (!(b & 0x80)) break;
    }
    if (tag == 0x0A) {
      starts[count] = position;
      lengths[count] = size;
      ++count;
    }
    position += size;
  }
  return count;
}

// The same rows, in any order (the non-ASCII collation difference).
bool same_rows(const Encoded& mine, const uint8_t* theirs, size_t theirs_length) {
  size_t a_start[128], a_len[128], b_start[128], b_len[128];
  const size_t a = rows_of(mine.bytes, mine.length, a_start, a_len, 128);
  const size_t b = rows_of(theirs, theirs_length, b_start, b_len, 128);
  if (a != b || mine.length != theirs_length) return false;
  bool used[128] = {};
  for (size_t i = 0; i < a; ++i) {
    bool found = false;
    for (size_t k = 0; k < b && !found; ++k)
      if (!used[k] && a_len[i] == b_len[k] && memcmp(mine.bytes + a_start[i], theirs + b_start[k], a_len[i]) == 0)
        used[k] = found = true;
    if (!found) return false;
  }
  return true;
}

// The JSON catalog's rows are in RobinhoodSymbolCompare order.
bool sorted_by_symbol(const Encoded& json) {
  tj::Arena arena = tj::GrowingArena(1u << 20);
  const tj::Value* assets = tj::Get(tj::Parse(arena, json.bytes, json.length), "assets");
  bool ok = tj::IsArray(assets) && assets->count > 1;
  for (uint32_t i = 1; ok && i < assets->count; ++i) {
    const tj::Value* a = tj::Get(assets->items[i - 1], "tokenSymbol");
    const tj::Value* b = tj::Get(assets->items[i], "tokenSymbol");
    ok = tj::IsString(a) && tj::IsString(b) && RobinhoodSymbolCompare(a->text, a->length, b->text, b->length) <= 0;
  }
  tj::FreeArena(arena);
  return ok;
}

void documents() {
  const tj::Value* docs = tj::Get(g_fixture, "documents");
  int compared = 0;
  for (uint32_t i = 0; tj::IsArray(docs) && i < docs->count; ++i) {
    const tj::Value* doc = docs->items[i];
    const tj::Value* name = tj::Get(doc, "name");
    const tj::Value* body = provider_body(doc);
    char what[160];
    if (!check(tj::IsString(name) && tj::IsString(body), "document has a name and a provider body")) continue;
    Fake fake;
    serve(fake, body->text, body->length);
    rh::StockClient client;
    client.Configure(&fake, &g_clock);
    Read r = read(client, nullptr);
    snprintf(what, sizeof(what), "%s: catalog read succeeds", name->text);
    check(r.outcome.ok() && r.json.ok && r.protobuf.ok, what);
    // locale-compare: the ordering tables hold the oracle's rows
    // in ICU order; this server sorts them by RobinhoodSymbolCompare.
    const bool reordered = strcmp(name->text, "unicode") == 0 || strcmp(name->text, "ascii") == 0;
    if (reordered) {
      // Same rows (byte-identical per instrument), another order.
      const tj::Value* hex = tj::Get(tj::Get(doc, "protobuf"), "hex");
      uint8_t* theirs = static_cast<uint8_t*>(malloc(hex->length / 2));
      for (size_t k = 0; k < hex->length / 2; ++k) {
        unsigned value = 0;
        if (!fortis::test::ParseHexByte(hex->text + 2 * k, &value)) throw std::runtime_error("invalid hex in test fixture");
        theirs[k] = static_cast<uint8_t>(value);
      }
      snprintf(what, sizeof(what), "%s: the same protobuf rows in another order", name->text);
      check(same_rows(r.protobuf, theirs, hex->length / 2), what);
      const tj::Value* text = tj::Get(tj::Get(doc, "json"), "text");
      snprintf(what, sizeof(what), "%s: the JSON has the oracle's length", name->text);
      check(tj::IsString(text) && text->length == r.json.length, what);
      snprintf(what, sizeof(what), "%s: rows sorted by RobinhoodSymbolCompare", name->text);
      check(sorted_by_symbol(r.json), what);
      free(theirs);
    } else {
      snprintf(what, sizeof(what), "%s: JSON bytes equal the oracle", name->text);
      check(matches(r.json, tj::Get(doc, "json")), what);
      if (tj::Get(doc, "protobuf")) {
        snprintf(what, sizeof(what), "%s: protobuf bytes equal the oracle", name->text);
        check(matches(r.protobuf, tj::Get(doc, "protobuf")), what);
      }
    }
    snprintf(what, sizeof(what), "%s: one request to /rhj/assets", name->text);
    check(calls(fake) == 1 && !fake.wrong_path, what);
    r.Free();
    ++compared;
  }
  check(compared == 8, "eight oracle documents compared");

  // The expansion catalog: the native bytes are TypeScript's; only the
  // route's 2 MiB response bound refuses them.
  const tj::Value* json_case = find_case("expansion-json");
  const tj::Value* proto_case = find_case("expansion-protobuf");
  const tj::Value* body = provider_body(first_assets_reply(json_case));
  if (check(tj::IsString(body), "expansion provider body")) {
    Fake fake;
    serve(fake, body->text, body->length);
    rh::StockClient client;
    client.Configure(&fake, &g_clock);
    Read r = read(client, nullptr);
    char digest[65];
    sha256_hex(r.json.bytes, r.json.length, digest);
    const tj::Value* want = tj::Get(json_case, "bodySha256");
    check(r.json.ok && r.json.length == static_cast<size_t>(tj::Get(json_case, "bodyLength")->number) &&
              memcmp(digest, want->text, 64) == 0 && r.json.length > 2u * 1024 * 1024,
          "expansion: JSON equals the oracle's 2.76 MB answer");
    sha256_hex(r.protobuf.bytes, r.protobuf.length, digest);
    want = tj::Get(proto_case, "bodySha256");
    check(r.protobuf.ok && r.protobuf.length == static_cast<size_t>(tj::Get(proto_case, "bodyLength")->number) &&
              memcmp(digest, want->text, 64) == 0 && r.protobuf.length > 2u * 1024 * 1024,
          "expansion: protobuf equals the oracle's answer");
    r.Free();
  }
}

void shapes() {
  const tj::Value* cases = tj::Get(g_fixture, "cases");
  int seen = 0;
  for (uint32_t i = 0; i < cases->count; ++i) {
    const tj::Value* c = cases->items[i];
    const tj::Value* name = tj::Get(c, "name");
    if (strncmp(name->text, "shape-", 6) != 0 || strstr(name->text, "recovered")) continue;
    const tj::Value* body = provider_body(first_assets_reply(c));
    tj::Arena arena = tj::GrowingArena(1u << 20);
    const tj::Value* parsed = tj::Parse(arena, reinterpret_cast<const uint8_t*>(body->text), body->length);
    char what[128];
    snprintf(what, sizeof(what), "%s: parseAssets throws (oracle 502)", name->text);
    check(parsed && !rh::ParseAssets(arena, parsed) && tj::Get(c, "status")->number == 502, what);
    Fake fake;
    serve(fake, body->text, body->length);
    rh::StockClient client;
    client.Configure(&fake, &g_clock);
    Read r = read(client, nullptr);
    snprintf(what, sizeof(what), "%s: the catalog read fails", name->text);
    check(r.outcome.failure == rh::Failure::kError, what);
    r.Free();
    tj::FreeArena(arena);
    ++seen;
  }
  check(seen == 5, "five shape failures");
}

const char kOne[] =
    "{\"assets\":[{\"id\":\"0x00000000000000000000000000000000000000000000000000000000000000aa\","
    "\"tokenSymbol\":\"TSLA\",\"tokenName\":\"Tesla\",\"deployments\":[{\"chainId\":4663,"
    "\"contractAddress\":\"0x00000000000000000000000000000000000000aa\"}],\"currentMultiplier\":\"1\","
    "\"status\":\"ASSET_STATUS_ACTIVE\"}]}";
const char kTwo[] =
    "{\"assets\":[{\"id\":\"0x00000000000000000000000000000000000000000000000000000000000000bb\","
    "\"tokenSymbol\":\"NVDA\",\"tokenName\":\"Nvidia\",\"deployments\":[{\"chainId\":4663,"
    "\"contractAddress\":\"0x00000000000000000000000000000000000000bb\"}],\"currentMultiplier\":\"2\"}]}";

bool contains(const Encoded& e, const char* text) {
  const size_t n = strlen(text);
  for (size_t i = 0; e.ok && i + n <= e.length; ++i)
    if (memcmp(e.bytes + i, text, n) == 0) return true;
  return false;
}

void cache() {
  Fake fake;
  rh::StockClient client;
  client.Configure(&fake, &g_clock);
  const int64_t t0 = 1'793'000'000'000;
  g_now = t0;
  serve(fake, kOne);
  Read r = read(client, nullptr);
  check(r.outcome.ok() && calls(fake) == 1 && contains(r.json, "TSLA"), "cold: one request");
  r.Free();
  serve(fake, kTwo);
  g_now = t0 + 899'999;
  r = read(client, nullptr);
  check(r.outcome.ok() && calls(fake) == 1 && contains(r.json, "TSLA"), "fresh at 899,999 ms: no request");
  r.Free();
  g_now = t0 + 900'000;
  r = read(client, nullptr);
  check(r.outcome.ok() && calls(fake) == 2 && contains(r.json, "NVDA"), "stale at 900,000 ms: refetched");
  r.Free();
  // An outage after expiry: the expired catalog is not served, the failure
  // is not cached.
  g_now = t0 + 1'800'000;
  refuse(fake, rh::Failure::kError);
  r = read(client, nullptr);
  check(r.outcome.failure == rh::Failure::kError && calls(fake) == 3, "stale outage: failure, no stale catalog");
  r.Free();
  r = read(client, nullptr);
  check(r.outcome.failure == rh::Failure::kError && calls(fake) == 4, "the failure is not cached");
  r.Free();
  serve(fake, kOne);
  r = read(client, nullptr);
  check(r.outcome.ok() && calls(fake) == 5 && contains(r.json, "TSLA"), "recovered at the same instant");
  r.Free();
  // A lane refusal keeps its retry-after (runFacet's typed 429).
  g_now = t0 + 2'700'000;
  refuse(fake, rh::Failure::kLaneBusy, 7000);
  r = read(client, nullptr);
  check(r.outcome.failure == rh::Failure::kLaneBusy && r.outcome.retry_after_ms == 7000, "lane busy passes through");
  r.Free();
}

struct Thread {
  rh::StockClient* client = nullptr;
  uint64_t issued = rh::StockClient::kNotIssued;
  Read result;
  pthread_t thread{};
};

void* run_read(void* opaque) {
  auto* t = static_cast<Thread*>(opaque);
  t->result = read(*t->client, nullptr, t->issued);
  return nullptr;
}

void concurrency() {
  for (int failing = 0; failing < 2; ++failing) {
    Fake fake;
    fake.delay_us = 200 * 1000;
    rh::StockClient client;
    client.Configure(&fake, &g_clock);
    g_now = 1'793'000'000'000;
    serve(fake, kOne);
    if (failing) refuse(fake, rh::Failure::kError);
    Thread threads[8];
    for (Thread& t : threads) {
      t.client = &client;
      pthread_create(&t.thread, nullptr, run_read, &t);
    }
    bool same = true;
    for (Thread& t : threads) {
      pthread_join(t.thread, nullptr);
      if (failing) {
        same = same && t.result.outcome.failure == rh::Failure::kError;
      } else {
        same = same && t.result.outcome.ok() && t.result.json.length == threads[0].result.json.length &&
               memcmp(t.result.json.bytes, threads[0].result.json.bytes, t.result.json.length) == 0;
      }
    }
    check(calls(fake) == 1, failing ? "8 simultaneous reads of a failing provider: one request"
                                     : "8 simultaneous cold reads: one request");
    check(same, failing ? "every simultaneous read fails" : "every simultaneous read is the same document");
    for (Thread& t : threads) t.result.Free();
  }

  // Issued generations: reads issued before a request finished share its
  // failure; a read issued after it starts its own.
  Fake fake;
  rh::StockClient client;
  client.Configure(&fake, &g_clock);
  g_now = 1'793'000'000'000;
  const uint64_t before = client.AssetsGeneration();
  refuse(fake, rh::Failure::kError);
  Read r = read(client, nullptr, before);
  check(r.outcome.failure == rh::Failure::kError && calls(fake) == 1, "the first queued read makes the request");
  r.Free();
  serve(fake, kOne);
  r = read(client, nullptr, before);
  check(r.outcome.failure == rh::Failure::kError && calls(fake) == 1,
        "a read issued before that failed request finished takes its failure");
  r.Free();
  r = read(client, nullptr, client.AssetsGeneration());
  check(r.outcome.ok() && calls(fake) == 2, "a read issued after it makes a new request");
  r.Free();
  const uint64_t after_success = client.AssetsGeneration();
  check(after_success == before + 2, "each finished request advances the generation");
  g_now += 900'000;
  r = read(client, nullptr, before);
  check(r.outcome.ok() && calls(fake) == 3, "an old issue after a success refetches an expired catalog");
  r.Free();
  r = read(client, nullptr);
  check(r.outcome.ok() && calls(fake) == 3, "kNotIssued reads the fresh catalog");
  r.Free();
}

void media(const char* directory) {
  const tj::Value* docs = tj::Get(g_fixture, "documents");
  const tj::Value* doc = docs->items[0];
  check(strcmp(tj::Get(doc, "name")->text, "A") == 0, "document A first");
  fortis::storage::KvStore kv;
  if (!check(kv.Open(directory), "KV opens")) return;
  g_now = 1'793'000'000'000;
  // An existing record (another route registered it) is never overwritten.
  const char* sentinel_key = "media/item/";
  char existing[64];
  // The TSLA issuer logo's key, from the oracle's document.
  tj::Arena arena = tj::GrowingArena(4u << 20);
  const tj::Value* text = tj::Get(tj::Get(doc, "json"), "text");
  const tj::Value* expected = tj::Parse(arena, reinterpret_cast<const uint8_t*>(text->text), text->length);
  const tj::Value* rows = tj::Get(expected, "assets");
  const tj::Value* first_key = nullptr;
  for (uint32_t i = 0; i < rows->count && !first_key; ++i)
    if (tj::IsString(tj::Get(rows->items[i], "logoMediaKey"))) first_key = tj::Get(rows->items[i], "logoMediaKey");
  if (!first_key) { check(false, "fixture has an issuer logo key"); tj::FreeArena(arena); return; }
  snprintf(existing, sizeof(existing), "%s%s", sentinel_key, first_key->text);
  const fortis::storage::KvMutation mutation{existing, "sentinel", 8, 0, false};
  check(kv.Write(&mutation, 1), "sentinel written");

  Fake fake;
  const tj::Value* body = provider_body(doc);
  serve(fake, body->text, body->length);
  rh::StockClient client;
  client.Configure(&fake, &g_clock);
  Read r = read(client, &kv);
  check(r.outcome.ok() && matches(r.json, tj::Get(doc, "json")), "with KV: the same document");
  int records = 0, keyed = 0;
  for (uint32_t i = 0; i < rows->count; ++i) {
    const tj::Value* key = tj::Get(rows->items[i], "logoMediaKey");
    const tj::Value* logo = tj::Get(rows->items[i], "logoUrl");
    if (!tj::IsString(key)) {
      check(!tj::IsString(logo), "no media key without a display logo");
      continue;
    }
    ++keyed;
    char record_key[64];
    snprintf(record_key, sizeof(record_key), "media/item/%s", key->text);
    char value[4096];
    size_t size = 0;
    const bool found = kv.Get(record_key, g_now, value, sizeof(value) - 1, &size);
    value[found ? size : 0] = '\0';
    if (strcmp(record_key, existing) == 0) {
      check(found && strcmp(value, "sentinel") == 0, "an existing media record is not overwritten");
      continue;
    }
    char want[96];
    snprintf(want, sizeof(want), "{\"key\":\"%s\",\"sourceUrl\":", key->text);
    check(found && strncmp(value, want, strlen(want)) == 0 && strstr(value, "\"status\":\"pending\""),
          "a pending media record per display logo");
    ++records;
  }
  check(keyed == 9 && records >= 4, "media records for the catalog's display logos");
  r.Free();
  tj::FreeArena(arena);
  kv.Close();
}

void collation() {
  struct Pair {
    const char* a;
    size_t a_length;
    const char* b;
    size_t b_length;
    int sign;
    const char* what;
  } pairs[] = {
      {"A\x01" "B", 3, "AB", 2, -1, "a control character is an ordinary byte"},
      {"A\x7f" "B", 3, "AB", 2, 1, "DEL is an ordinary byte"},
      {"A\0B", 3, "AB", 2, -1, "NUL is an ordinary byte"},
      {"A\tB", 3, "A B", 3, -1, "TAB before space (byte order)"},
      {"A B", 3, "A_B", 3, -1, "space before underscore"},
      {"A$B", 3, "A1", 2, -1, "symbols before digits"},
      {"A1", 2, "AA", 2, -1, "digits before letters"},
      {"A_", 2, "AA", 2, -1, "underscore before the letters once folded"},
      {"AA", 2, "Ab", 2, -1, "case-insensitive first"},
      {"A", 1, "a", 1, -1, "then the own bytes: upper case first"},
      {"AAPl", 4, "Aapl", 4, -1, "the first byte difference decides a case tie"},
      {"aapl", 4, "aapl", 4, 0, "equal symbols"},
      {"A", 1, "AA", 2, -1, "a prefix first"},
      {"BRK-A", 5, "BRK.A", 5, -1, "hyphen before full stop"},
      {"\xc3\x84", 2, "Z", 1, 1, "non-ASCII after ASCII (byte order)"},
  };
  for (const Pair& p : pairs) {
    const int got = RobinhoodSymbolCompare(p.a, p.a_length, p.b, p.b_length);
    const int back = RobinhoodSymbolCompare(p.b, p.b_length, p.a, p.a_length);
    check((got > 0) - (got < 0) == p.sign && (back > 0) - (back < 0) == -p.sign, p.what);
  }
}

void bounds() {
  Fake fake;
  serve(fake, kOne);
  rh::StockClient client;
  client.Configure(&fake, &g_clock);
  tj::Arena arena = tj::GrowingArena(1u << 20);
  rh::MarketsRequest request;
  request.client = &client;
  request.now_ms = g_now;
  request.deadline_ms = far_deadline();
  tj::Value* result = nullptr;
  std::shared_ptr<const rh::StockClient::Snapshot> held = nullptr;
  check(rh::Catalog(arena, request, &result, &held).ok(), "catalog for the bounds check");
  tj::OutBuffer measure{nullptr, 0, 0, true};
  check(rh::EncodeCatalog(measure, result), "measure");
  uint8_t buffer[1024];
  tj::OutBuffer small{buffer, measure.used - 1, 0, true};
  check(measure.used <= sizeof(buffer) && !rh::EncodeCatalog(small, result), "an undersized buffer is refused");
  tj::OutBuffer exact{buffer, measure.used, 0, true};
  check(rh::EncodeCatalog(exact, result) && exact.used == measure.used, "the exact size is accepted");
  held.reset();
  tj::FreeArena(arena);
}

void snapshot_lifetime() {
  std::shared_ptr<const rh::StockClient::Snapshot> held;
  {
    Fake fake;
    serve(fake, kOne);
    rh::StockClient client;
    client.Configure(&fake, &g_clock);
    check(client.Assets(far_deadline(), &held).ok() && held,
          "snapshot owns its document");
  }
  const auto* document = rh::StockClient::Document(held.get());
  check(document && tj::IsArray(document) && document->count == 1,
        "held snapshot survives the client and cache lifetime");
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc != 2) {
    fprintf(stderr, "usage: %s robinhood_stock_catalog_edges.json\n", argv[0]);
    return 2;
  }
  std::vector<uint8_t> fixture_bytes;
  if (!fortis::test::ReadFixture(argv[1], fixture_bytes)) return 2;
  const size_t size = fixture_bytes.size();
  uint8_t* text = fixture_bytes.data();
  tj::Arena arena = tj::GrowingArena(256u << 20);
  g_fixture = tj::Parse(arena, text, static_cast<size_t>(size));
  if (!strict::ContractExpectations(arena, g_fixture, "robinhood_stock_catalog_edges.json")) return 2;
  if (!g_fixture) {
    fprintf(stderr, "fixture does not parse\n");
    return 2;
  }
  char directory[] = "/tmp/robinhood-stock-catalog-XXXXXX";
  if (!mkdtemp(directory)) return 2;

  documents();
  shapes();
  cache();
  concurrency();
  media(directory);
  collation();
  bounds();
  snapshot_lifetime();

  fortis::test::RemoveTree(directory);
  tj::FreeArena(arena);

  printf("robinhood stock catalog unit: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
} catch (...) {
  return fortis::test::ReportException();
}
