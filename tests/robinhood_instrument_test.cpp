#include "test_support.h"
#include "strict_unit_expectations.h"
// R12 unit test of GET /v1/networks/robinhood/stock-tokens/{address} below the
// socket: StockClient over a FetchFn, robinhood::FindInstrument, the quote and
// corporate-action reads and BuildInstrument + Stringify/EncodeInstrument,
// against the TypeScript oracle (robinhood_instrument_edges.json):
//  - the `main`, `quotes`, `quote-failures` and `actions` groups replayed in
//    order on one client per group with the oracle's frozen instants and
//    provider tables (HttpClient.json's three attempts emulated: 5xx, 408 and
//    a non-JSON 2xx retried, other statuses not): every instrument answer's
//    exact JSON or protobuf bytes (sha256 beyond 64 KiB), every 404 and 502,
//    and the provider documents each case requested;
//  - the caches: quote fresh to 14,999 ms and refetched at 15,000, a null
//    quote and a failure never cached, actions fresh to 3,599,999 ms;
//  - the issued generations: a read issued before a quote or corporate-action
//    request finished takes its failure (or its null quote) without a second
//    request; a later read asks again;
//  - one request for simultaneous quote and action reads;
//  - media records for the display logo (pending, never overwritten), the
//    maximum actions document's build time, an undersized protobuf buffer.
// The real-socket contract, Promise.all timing included, is
// tests/robinhood_instrument_edges_test.py.
// Usage: robinhood_instrument_test tests/corpus/robinhood_instrument_edges.json
#include "fake_seams.h"
#include "kv_store.h"
#include "robinhood_markets.h"

#include <mbedtls/sha256.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string_view>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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
const tj::Value* g_fixture = nullptr;

// A provider entry's body (inline, or from `bodies` by sha256).
const tj::Value* provider_body(const tj::Value* entry) {
  if (const tj::Value* body = tj::Get(entry, "body")) return body;
  const tj::Value* sha = tj::Get(entry, "bodySha256");
  return tj::IsString(sha) ? tj::Get(tj::Get(g_fixture, "bodies"), sha->text, sha->length) : nullptr;
}

// ---- the fake provider: a case's table, HttpClient.json per call -------------------------------

constexpr size_t kPaths = 8;
struct Fake final : rh::StockClient::Fetcher {
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  const tj::Value* table = nullptr;  // the case's providers
  char paths[kPaths][128]{};         // documents requested since Reset
  int path_count = 0;
  int consumed[16]{};                // per table key
  int calls = 0;
  useconds_t delay_us = 0;
  // Direct mode (no table): one body or one failure for every path.
  const char* body = nullptr;
  rh::Failure failure = rh::Failure::kNone;

  void Reset(const tj::Value* providers) {
    pthread_mutex_lock(&mutex);
    table = providers;
    path_count = 0;
    calls = 0;
    memset(consumed, 0, sizeof(consumed));
    pthread_mutex_unlock(&mutex);
  }
  int Calls() {
    pthread_mutex_lock(&mutex);
    const int n = calls;
    pthread_mutex_unlock(&mutex);
    return n;
  }
  rh::Outcome Fetch(tj::Arena& arena, uint64_t deadline_ms, const char* path, bool background,
                    tj::Value** body) override;
};

// The next recorded reply for `key` (a list consumed in order, the last
// repeating), or nullptr.
const tj::Value* next_reply(Fake& fake, const char* key) {
  const tj::Value* table = fake.table;
  for (uint32_t i = 0; tj::IsObject(table) && i < table->count && i < 16; ++i) {
    if (table->members[i].key_length != strlen(key) || memcmp(table->members[i].key, key, strlen(key)) != 0) continue;
    const tj::Value* list = table->members[i].value;
    if (!tj::IsArray(list) || !list->count) return nullptr;
    const uint32_t index = static_cast<uint32_t>(fake.consumed[i]) < list->count ? fake.consumed[i] : list->count - 1;
    ++fake.consumed[i];
    return list->items[index];
  }
  return nullptr;
}

rh::Outcome Fake::Fetch(tj::Arena& arena, uint64_t, const char* path, bool, tj::Value** body) {
  Fake* fake = this;
  rh::Outcome outcome;
  pthread_mutex_lock(&fake->mutex);
  ++fake->calls;
  if (fake->path_count < static_cast<int>(kPaths))
    snprintf(fake->paths[fake->path_count++], sizeof(fake->paths[0]), "%s", path);
  const useconds_t delay = fake->delay_us;
  if (!fake->table) {
    const char* text = fake->body;
    const rh::Failure failure = fake->failure;
    pthread_mutex_unlock(&fake->mutex);
    if (delay) usleep(delay);
    if (failure != rh::Failure::kNone || !text) {
      outcome.failure = failure != rh::Failure::kNone ? failure : rh::Failure::kError;
      return outcome;
    }
    *body = tj::Parse(arena, reinterpret_cast<const uint8_t*>(text), strlen(text));
    if (!*body) outcome.failure = rh::Failure::kError;
    return outcome;
  }
  char key[192];
  snprintf(key, sizeof(key), "GET api.robinhood.com%s", path);
  // HttpClient.json: three attempts; 5xx, 408 and an unparseable 2xx retry.
  outcome.failure = rh::Failure::kError;
  for (int attempt = 0; attempt < 3; ++attempt) {
    const tj::Value* reply = next_reply(*fake, key);
    if (!reply) break;
    const int status = static_cast<int>(tj::Get(reply, "status")->number);
    const tj::Value* text = provider_body(reply);
    if (status >= 200 && status < 300) {
      *body = tj::IsString(text) ? tj::Parse(arena, reinterpret_cast<const uint8_t*>(text->text), text->length)
                                 : nullptr;
      if (*body) {
        outcome.failure = rh::Failure::kNone;
        break;
      }
      continue;
    }
    if (status < 500 && status != 408) break;
  }
  pthread_mutex_unlock(&fake->mutex);
  if (delay) usleep(delay);
  return outcome;
}

// ---- encodings ----------------------------------------------------------------------------------

struct Encoded {
  uint8_t* bytes = nullptr;
  size_t length = 0;
  bool ok = false;
};

Encoded encode(const tj::Value* value, bool protobuf) {
  Encoded e;
  tj::OutBuffer measure{nullptr, 0, 0, true};
  if (!(protobuf ? rh::EncodeInstrument(measure, value) : tj::Stringify(value, measure)) || !measure.ok) return e;
  e.bytes = static_cast<uint8_t*>(malloc(measure.used ? measure.used : 1));
  tj::OutBuffer out{e.bytes, measure.used, 0, e.bytes != nullptr};
  e.ok = e.bytes && (protobuf ? rh::EncodeInstrument(out, value) : tj::Stringify(value, out)) && out.ok &&
         out.used == measure.used;
  e.length = out.used;
  return e;
}

void sha256_hex(const uint8_t* bytes, size_t length, char out[65]) {
  uint8_t digest[32];
  mbedtls_sha256(bytes, length, digest, 0);
  for (int i = 0; i < 32; ++i) snprintf(out + 2 * i, 3, "%02x", digest[i]);
}

// The case's recorded body against an encoding.
bool matches(const Encoded& e, const tj::Value* c) {
  if (!e.ok) return false;
  if (const tj::Value* text = tj::Get(c, "responseText"))
    { const bool same = tj::IsString(text) && text->length == e.length && memcmp(text->text, e.bytes, e.length) == 0;
      if (!same && tj::IsString(text)) { size_t i=0; while(i<text->length && i<e.length && text->text[i]==static_cast<char>(e.bytes[i])) ++i;
        fprintf(stderr, "JSON difference at %zu: want %.100s got %.100s\n", i, text->text+i, e.bytes+i); } return same; }
  if (const tj::Value* hex = tj::Get(c, "wireHex")) {
    if (!tj::IsString(hex) || hex->length != e.length * 2) return false;
    char pair[3];
    for (size_t i = 0; i < e.length; ++i) {
      snprintf(pair, sizeof(pair), "%02x", e.bytes[i]);
      if (memcmp(pair, hex->text + 2 * i, 2) != 0) return false;
    }
    return true;
  }
  const tj::Value* sha = tj::Get(c, "bodySha256");
  const tj::Value* length = tj::Get(c, "bodyLength");
  char digest[65];
  sha256_hex(e.bytes, e.length, digest);
  return tj::IsString(sha) && tj::IsNumber(length) && static_cast<size_t>(length->number) == e.length &&
         memcmp(sha->text, digest, 64) == 0;
}

// ---- one instrument read, as the route runs it (sequentially here) ---------------------------------

struct Read {
  int status = 0;  // 200, 404, 502 or 429
  Encoded json;
  Encoded protobuf;
  double build_ms = 0;
  void Free() {
    free(json.bytes);
    free(protobuf.bytes);
  }
};

double now_ms() {
  timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

Read read(rh::StockClient& client, fortis::storage::KvStore* kv, const char* address) {
  Read r;
  tj::Arena arena = tj::GrowingArena(64u << 20);
  rh::MarketsRequest request;
  request.client = &client;
  request.kv = kv;
  request.now_ms = g_now;
  request.deadline_ms = far_deadline();
  const tj::Value* asset = nullptr;
  std::shared_ptr<const rh::StockClient::Snapshot> assets = nullptr;
  std::shared_ptr<const rh::StockClient::Snapshot> actions = nullptr;
  rh::Outcome outcome = rh::FindInstrument(request, address, &asset, &assets);
  if (outcome.ok() && !asset) r.status = 404;
  if (outcome.ok() && asset) {
    tj::Value* quote = nullptr;
    const rh::Outcome quoted = client.ReferenceQuote(arena, request.deadline_ms, asset, &quote);
    const rh::Outcome acted = client.CorporateActions(request.deadline_ms, &actions);
    outcome = !quoted.ok() ? quoted : acted;
    tj::Value* result = nullptr;
    const double started = now_ms();
    if (outcome.ok() && rh::BuildInstrument(arena, request, asset, quote, actions.get(), &result)) {
      r.json = encode(result, false);
      r.protobuf = encode(result, true);
      r.status = 200;
    }
    r.build_ms = now_ms() - started;
  }
  if (!outcome.ok()) r.status = outcome.failure == rh::Failure::kLaneBusy ? 429 : 502;
  actions.reset();
  assets.reset();
  tj::FreeArena(arena);
  return r;
}

// /^0x[0-9a-fA-F]{40}$/ of a decoded target's last segment.
bool address_of(const tj::Value* c, char out[43]) {
  const tj::Value* hex = tj::Get(c, "targetHex");
  char target[1200];
  if (!tj::IsString(hex) || hex->length / 2 >= sizeof(target)) return false;
  for (size_t i = 0; i < hex->length / 2; ++i) {
    unsigned value = 0;
    if (!fortis::test::ParseHexByte(hex->text + 2 * i, &value)) throw std::runtime_error("invalid hex in test fixture");
    target[i] = static_cast<char>(value);
  }
  target[hex->length / 2] = '\0';
  target[strcspn(target, "?")] = '\0';
  const char* slash = strrchr(target, '/');
  if (!slash) return false;
  const char* segment = slash + 1;
  // `%30x…` (the escaped-address case): decode the escapes.
  char decoded[64];
  size_t used = 0;
  for (const char* p = segment; *p && used < sizeof(decoded) - 1; ++p) {
    if (*p == '%' && p[1] && p[2]) {
      unsigned value = 0;
      if (!fortis::test::ParseHexByte(p + 1, &value)) throw std::runtime_error("invalid hex in test fixture");
      decoded[used++] = static_cast<char>(value);
      p += 2;
    } else {
      decoded[used++] = *p;
    }
  }
  decoded[used] = '\0';
  if (used != 42 || decoded[0] != '0' || decoded[1] != 'x') return false;
  for (size_t i = 2; i < 42; ++i)
    if (!strchr("0123456789abcdefABCDEF", decoded[i])) return false;
  memcpy(out, decoded, 43);
  return true;
}

// The documents a case requested, as a sorted "a|p|c" signature.
void requested(const tj::Value* upstream, char* out, size_t size) {
  bool assets = false, prices = false, actions = false;
  for (uint32_t i = 0; tj::IsArray(upstream) && i < upstream->count; ++i) {
    const tj::Value* url = tj::Get(upstream->items[i], "url");
    if (strstr(url->text, "/rhj/assets")) assets = true;
    if (strstr(url->text, "/rhj/prices/")) prices = true;
    if (strstr(url->text, "/rhj/corporate-actions")) actions = true;
  }
  snprintf(out, size, "%s%s%s", assets ? "a" : "-", prices ? "p" : "-", actions ? "c" : "-");
}
void fetched(Fake& fake, char* out, size_t size) {
  bool assets = false, prices = false, actions = false;
  for (int i = 0; i < fake.path_count; ++i) {
    if (strcmp(fake.paths[i], "/rhj/assets") == 0) assets = true;
    if (strncmp(fake.paths[i], "/rhj/prices/", 12) == 0) prices = true;
    if (strcmp(fake.paths[i], "/rhj/corporate-actions") == 0) actions = true;
  }
  snprintf(out, size, "%s%s%s", assets ? "a" : "-", prices ? "p" : "-", actions ? "c" : "-");
}

void replay_group(const char* group, fortis::storage::KvStore* kv, int* replayed) {
  Fake fake;
  rh::StockClient client;
  client.Configure(&fake, &g_clock);
  const tj::Value* cases = tj::Get(g_fixture, "cases");
  for (uint32_t i = 0; i < cases->count; ++i) {
    const tj::Value* c = cases->items[i];
    if (strcmp(tj::Get(c, "group")->text, group) != 0 || tj::Get(c, "concurrent")) continue;
    const tj::Value* status = tj::Get(c, "status");
    const tj::Value* method = tj::Get(c, "method");
    const char* name = tj::Get(c, "name")->text;
    char address[43];
    // Route-level answers (refusals, invalid addresses, router 404s, HEAD)
    // are the socket replay's.
    const int want = static_cast<int>(status->number);
    if (strcmp(method->text, "GET") != 0 || (want != 200 && want != 404 && want != 502) ||
        !tj::Get(c, "device") || !tj::IsString(tj::Get(c, "device")) || !tj::Get(c, "device")->length ||
        !address_of(c, address) || strncmp(name, "router-", 7) == 0)
      continue;
    g_now = static_cast<int64_t>(tj::Get(c, "at")->number);
    fake.Reset(tj::Get(c, "providers"));
    Read r = read(client, kv, address);
    char what[256];
    snprintf(what, sizeof(what), "%s/%s: status %d (TypeScript %d)", group, name, r.status, want);
    check(r.status == want, what);
    if (want == 200) {
      const bool protobuf = tj::Get(c, "wireHex") ||
                            (tj::Get(c, "bodySha256") && tj::Get(c, "protobuf") &&
                             tj::Get(c, "protobuf")->type == tj::Type::kTrue);
      snprintf(what, sizeof(what), "%s/%s: %s bytes equal the oracle", group, name, protobuf ? "protobuf" : "JSON");
      check(matches(protobuf ? r.protobuf : r.json, c), what);
      if (strcmp(name, "actions-max-json") == 0)
        printf("  actions-max-json: built in %.1f ms (%zu-byte JSON, %zu-byte protobuf)\n", r.build_ms, r.json.length,
               r.protobuf.length);
    }
    char mine[8], theirs[8];
    fetched(fake, mine, sizeof(mine));
    requested(tj::Get(c, "upstream"), theirs, sizeof(theirs));
    snprintf(what, sizeof(what), "%s/%s: provider documents %s (TypeScript %s)", group, name, mine, theirs);
    if (strcmp(name,"lone-surrogate-symbol")==0) {
      check(strcmp(mine,"-p-")==0 && strcmp(theirs,"---")==0 && fake.path_count==1 &&
            strcmp(fake.paths[0],"/rhj/prices/%EF%BF%BDX")==0, "normalized symbol reaches exact encoded provider path");
    } else check(strcmp(mine, theirs) == 0, what);
    r.Free();
    ++*replayed;
  }
}

// ---- caches, generations, concurrency ---------------------------------------------------------

const char* kAssets = nullptr;
const char* kActions = nullptr;
char g_quote_body[512];
constexpr const char* kTsla = "0x7e51a7e51a7e51a7e51a7e51a7e51a7e51a7e51a";

// A client whose every document is `body` (the catalog, a quote or the
// actions all parse from the same document in direct mode); the quote body
// carries `assets`, `quotes` and `corpActions` together.
const char* combined_body() {
  static char* combined = nullptr;
  if (combined) return combined;
  const size_t size = strlen(kAssets) + strlen(kActions) + strlen(g_quote_body) + 64;
  combined = static_cast<char*>(malloc(size));
  // {"assets":[...], "corpActions":[...], "quotes":[...]}
  snprintf(combined, size, "{\"assets\":%s,\"corpActions\":%s,\"quotes\":%s}", kAssets, kActions, g_quote_body);
  return combined;
}

struct Held {
  rh::StockClient* client;
  const tj::Value* asset = nullptr;
  std::shared_ptr<const rh::StockClient::Snapshot> assets = nullptr;
  tj::Arena arena = tj::GrowingArena(4u << 20);
  explicit Held(rh::StockClient* c) : client(c) {
    rh::MarketsRequest request;
    request.client = c;
    request.deadline_ms = far_deadline();
    rh::FindInstrument(request, kTsla, &asset, &assets);
  }
  ~Held() {
    assets.reset();
    tj::FreeArena(arena);
  }
  rh::Outcome Quote(tj::Value** quote, uint64_t issued = rh::StockClient::kNotIssued) {
    return client->ReferenceQuote(arena, far_deadline(), asset, quote, false, issued);
  }
};

void caches() {
  Fake fake;
  fake.body = combined_body();
  rh::StockClient client;
  client.Configure(&fake, &g_clock);
  g_now = 1'800'000'000'000;
  Held held(&client);
  check(held.asset != nullptr, "the TSLA asset is found");
  tj::Value* quote = nullptr;
  int before = fake.Calls();
  check(held.Quote(&quote).ok() && quote, "a quote");
  check(fake.Calls() == before + 1, "the quote is fetched");
  g_now += 14'999;
  check(held.Quote(&quote).ok() && quote && fake.Calls() == before + 1, "the quote is fresh to 14,999 ms");
  g_now += 1;
  check(held.Quote(&quote).ok() && quote && fake.Calls() == before + 2, "the quote is refetched at 15,000 ms");

  // A failure is not cached; nor is a null quote.
  g_now += 15'000;
  fake.failure = rh::Failure::kError;
  before = fake.Calls();
  check(!held.Quote(&quote).ok(), "a provider failure fails the quote");
  fake.failure = rh::Failure::kNone;
  check(held.Quote(&quote).ok() && quote && fake.Calls() == before + 2, "a failure is not cached");
  g_now += 15'000;
  fake.body = "{\"quotes\":[]}";
  before = fake.Calls();
  check(held.Quote(&quote).ok() && !quote, "an unusable body is a null quote");
  check(held.Quote(&quote).ok() && !quote && fake.Calls() == before + 2, "a null quote is not cached");

  // Issued generations: a read issued before a request finished takes its
  // result (failure or null) without a request of its own.
  uint64_t issued = client.QuoteGeneration();
  fake.failure = rh::Failure::kError;
  check(!held.Quote(&quote).ok(), "a failing quote request");
  fake.failure = rh::Failure::kNone;
  fake.body = combined_body();
  before = fake.Calls();
  check(!held.Quote(&quote, issued).ok() && fake.Calls() == before, "a read issued before that request takes its failure");
  check(held.Quote(&quote, client.QuoteGeneration()).ok() && quote && fake.Calls() == before + 1,
        "a read issued after it asks again");
  g_now += 15'000;
  issued = client.QuoteGeneration();
  fake.body = "{\"quotes\":[]}";
  check(held.Quote(&quote).ok() && !quote, "a null quote request");
  fake.body = combined_body();
  before = fake.Calls();
  check(held.Quote(&quote, issued).ok() && !quote && fake.Calls() == before,
        "a read issued before that request takes its null quote");
  check(held.Quote(&quote, rh::StockClient::kNotIssued).ok() && quote && fake.Calls() == before + 1,
        "kNotIssued asks again");

  // Corporate actions: one hour, and the same generation rule.
  std::shared_ptr<const rh::StockClient::Snapshot> actions = nullptr;
  before = fake.Calls();
  check(client.CorporateActions(far_deadline(), &actions).ok() && fake.Calls() == before + 1, "actions fetched");
  actions.reset();
  g_now += 3'599'999;
  check(client.CorporateActions(far_deadline(), &actions).ok() && fake.Calls() == before + 1,
        "actions fresh to 3,599,999 ms");
  actions.reset();
  g_now += 1;
  issued = client.ActionsGeneration();
  fake.failure = rh::Failure::kError;
  check(!client.CorporateActions(far_deadline(), &actions).ok() && fake.Calls() == before + 2,
        "actions refetched at 3,600,000 ms (failing)");
  fake.failure = rh::Failure::kNone;
  check(!client.CorporateActions(far_deadline(), &actions, issued).ok() && fake.Calls() == before + 2,
        "an actions read issued before that request takes its failure");
  check(client.CorporateActions(far_deadline(), &actions, client.ActionsGeneration()).ok() &&
            fake.Calls() == before + 3,
        "a later actions read asks again");
  actions.reset();
}

struct ThreadArg {
  rh::StockClient* client;
  const tj::Value* asset;
  bool quote_ok = false;
  bool actions_ok = false;
};

void* reader(void* opaque) {
  auto* arg = static_cast<ThreadArg*>(opaque);
  tj::Arena arena = tj::GrowingArena(1u << 20);
  tj::Value* quote = nullptr;
  arg->quote_ok = arg->client->ReferenceQuote(arena, far_deadline(), arg->asset, &quote).ok() && quote;
  std::shared_ptr<const rh::StockClient::Snapshot> actions = nullptr;
  arg->actions_ok = arg->client->CorporateActions(far_deadline(), &actions).ok();
  actions.reset();
  tj::FreeArena(arena);
  return nullptr;
}

void concurrency() {
  Fake fake;
  fake.body = combined_body();
  rh::StockClient client;
  client.Configure(&fake, &g_clock);
  g_now = 1'810'000'000'000;
  Held held(&client);
  fake.delay_us = 200'000;
  const int before = fake.Calls();
  ThreadArg args[8];
  pthread_t threads[8];
  for (int i = 0; i < 8; ++i) {
    args[i].client = &client;
    args[i].asset = held.asset;
    pthread_create(&threads[i], nullptr, reader, &args[i]);
  }
  bool all = true;
  for (int i = 0; i < 8; ++i) {
    pthread_join(threads[i], nullptr);
    all = all && args[i].quote_ok && args[i].actions_ok;
  }
  check(all, "eight simultaneous readers all read the quote and the actions");
  check(fake.Calls() == before + 2, "eight simultaneous readers make one quote and one actions request");
}

void media(const char* directory) {
  fortis::storage::KvStore kv;
  if (!check(kv.Open(directory), "KV opens")) return;
  g_now = 1'820'000'000'000;
  const char* key = "media/item/a882a98667d7f46d10c93951a4115684";  // the TSLA display logo
  Fake fake;
  fake.body = combined_body();
  rh::StockClient client;
  client.Configure(&fake, &g_clock);
  Read r = read(client, &kv, kTsla);
  // The encoded JSON is length-delimited, not NUL-terminated.
  const std::string_view json(reinterpret_cast<const char*>(r.json.bytes), r.json.length);
  check(r.status == 200 && json.find("\"logoMediaKey\":\"a882a98667d7f46d") != std::string_view::npos,
        "the instrument carries the display logo's media key");
  char value[2048];
  size_t size = 0;
  bool found = kv.Get(key, g_now, value, sizeof(value) - 1, &size);
  value[found ? size : 0] = '\0';
  check(found && strstr(value, "\"sourceUrl\":\"https://financialmodelingprep.com/image-stock/TSLA.png\"") &&
            strstr(value, "\"status\":\"pending\""),
        "a pending media record for the display logo");
  r.Free();
  // An existing record is never overwritten.
  const fortis::storage::KvMutation mutation{key, "sentinel", 8, 0, false};
  check(kv.Write(&mutation, 1), "sentinel written");
  r = read(client, &kv, kTsla);
  found = kv.Get(key, g_now, value, sizeof(value) - 1, &size);
  value[found ? size : 0] = '\0';
  check(r.status == 200 && found && strcmp(value, "sentinel") == 0, "an existing media record is not overwritten");
  r.Free();
  kv.Close();
}

void bounds() {
  Fake fake;
  fake.body = combined_body();
  rh::StockClient client;
  client.Configure(&fake, &g_clock);
  g_now = 1'830'000'000'000;
  tj::Arena arena = tj::GrowingArena(4u << 20);
  rh::MarketsRequest request;
  request.client = &client;
  request.deadline_ms = far_deadline();
  request.now_ms = g_now;
  const tj::Value* asset = nullptr;
  std::shared_ptr<const rh::StockClient::Snapshot> assets = nullptr;
  std::shared_ptr<const rh::StockClient::Snapshot> actions = nullptr;
  tj::Value* quote = nullptr;
  tj::Value* result = nullptr;
  const bool built = rh::FindInstrument(request, kTsla, &asset, &assets).ok() && asset &&
                     client.ReferenceQuote(arena, far_deadline(), asset, &quote).ok() &&
                     client.CorporateActions(far_deadline(), &actions).ok() &&
                     rh::BuildInstrument(arena, request, asset, quote, actions.get(), &result);
  check(built, "an instrument without media KV");
  // MediaFacet.register logs a failed write and still returns the key.
  const tj::Value* key = built ? tj::Get(tj::Get(result, "asset"), "logoMediaKey") : nullptr;
  check(tj::IsString(key) && strcmp(key->text, "a882a98667d7f46d10c93951a4115684") == 0,
        "without KV the media key is still the display logo's");
  tj::OutBuffer measure{nullptr, 0, 0, true};
  check(built && rh::EncodeInstrument(measure, result), "measured");
  if (!built || !measure.used) return;
  uint8_t* buffer = static_cast<uint8_t*>(malloc(measure.used));
  assert(buffer);
  tj::OutBuffer small{buffer, measure.used - 1, 0, true};
  check(built && !rh::EncodeInstrument(small, result), "an undersized buffer is refused");
  tj::OutBuffer exact{buffer, measure.used, 0, true};
  check(built && rh::EncodeInstrument(exact, result) && exact.used == measure.used, "the exact size is accepted");
  free(buffer);
  // An unknown contract: no quote or actions request.
  const int before = fake.Calls();
  const tj::Value* none = nullptr;
  std::shared_ptr<const rh::StockClient::Snapshot> again;
  check(rh::FindInstrument(request, "0x0000000000000000000000000000000000000333", &none, &again).ok() && !none &&
            fake.Calls() == before,
        "an unknown contract is absent without a request");
  again.reset();
  actions.reset();
  assets.reset();
  tj::FreeArena(arena);
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc != 2) {
    fprintf(stderr, "usage: %s robinhood_instrument_edges.json\n", argv[0]);
    return 2;
  }
  std::vector<uint8_t> fixture_bytes;
  if (!fortis::test::ReadFixture(argv[1], fixture_bytes)) return 2;
  const size_t size = fixture_bytes.size();
  uint8_t* text = fixture_bytes.data();
  tj::Arena arena = tj::GrowingArena(256u << 20);
  g_fixture = tj::Parse(arena, text, static_cast<size_t>(size));
  if (!strict::ContractExpectations(arena, g_fixture, "robinhood_instrument_edges.json")) return 2;
  if (!g_fixture) {
    fprintf(stderr, "fixture does not parse\n");
    return 2;
  }
  // The direct-mode documents: the catalog, the actions and one quote.
  const tj::Value* documents = tj::Get(g_fixture, "documents");
  const tj::Value* catalog = tj::Get(documents, "catalog");
  const tj::Value* actions = tj::Get(documents, "actions");
  const tj::Value* parsed_catalog = tj::Parse(arena, reinterpret_cast<const uint8_t*>(catalog->text), catalog->length);
  const tj::Value* parsed_actions = tj::Parse(arena, reinterpret_cast<const uint8_t*>(actions->text), actions->length);
  tj::OutBuffer a{nullptr, 0, 0, true}, c{nullptr, 0, 0, true};
  tj::Stringify(tj::Get(parsed_catalog, "assets"), a);
  tj::Stringify(tj::Get(parsed_actions, "corpActions"), c);
  char* assets_text = static_cast<char*>(calloc(1, a.used + 1));
  char* actions_text = static_cast<char*>(calloc(1, c.used + 1));
  tj::OutBuffer aw{reinterpret_cast<uint8_t*>(assets_text), a.used, 0, true};
  tj::OutBuffer cw{reinterpret_cast<uint8_t*>(actions_text), c.used, 0, true};
  tj::Stringify(tj::Get(parsed_catalog, "assets"), aw);
  tj::Stringify(tj::Get(parsed_actions, "corpActions"), cw);
  kAssets = assets_text;
  kActions = actions_text;
  snprintf(g_quote_body, sizeof(g_quote_body),
           "[{\"tokenSymbol\":\"TSLA\",\"deployments\":[{\"chainId\":4663,\"contractAddress\":\"%s\"}],"
           "\"bid\":\"100.1\",\"ask\":\"100.3\",\"currency\":\"USD\",\"generatedAt\":\"2026-10-26T11:59:58Z\"}]",
           kTsla);

  char directory[] = "/tmp/robinhood-instrument-XXXXXX";
  if (!mkdtemp(directory)) return 2;
  int replayed = 0;
  static const char* const kGroups[] = {"main", "quotes", "quote-failures", "actions"};
  for (const char* group : kGroups) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", directory, group);
    mkdir(path, 0700);
    fortis::storage::KvStore kv;
    if (!check(kv.Open(path), "group KV opens")) continue;
    replay_group(group, &kv, &replayed);
    kv.Close();
  }
  printf("  %d oracle cases replayed below the socket\n", replayed);
  check(replayed >= 120, "at least 120 oracle cases replayed");
  caches();
  concurrency();
  char media_path[128];
  snprintf(media_path, sizeof(media_path), "%s/media", directory);
  mkdir(media_path, 0700);
  media(media_path);
  bounds();

  fortis::test::RemoveTree(directory);
  free(assets_text);
  free(actions_text);
  tj::FreeArena(arena);

  printf("robinhood instrument unit: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
} catch (...) {
  return fortis::test::ReportException();
}
