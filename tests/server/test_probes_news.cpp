#include "fixed_string.h"
// Test probes of the news and media plane. Compiled empty without
// FORTIS_NEWS_ROUTE_TEST_ONLY.
#include "test_probes.h"

#ifdef FORTIS_NEWS_ROUTE_TEST_ONLY
#include "app_support.h"
#include "device_auth.h"
#include "kv_store.h"
#include "market_view.h"
#include "media_registry.h"
#include "news_json.h"
#include "outbound_https.h"
#include "service_wiring.h"
#include "test_hook_env.h"
#include "token_json.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef FORTIS_UNIKRAFT
// This target has no C++ standard-library headers; provider_execution.cpp
// defines the placement-new primitive for malloc-owned objects.
void* operator new(size_t, void* storage) noexcept;
#else
#include <new>
#endif

namespace fortis::app {
namespace {

bool allow_news_test_address(void*, uint32_t address) { return address == 0x7f000001U; }

// The news, media, cache and ingest clock: a file lets one process move it
// between requests; the environment value fixes it; otherwise the wall clock.
int64_t news_test_now_ms() {
  const char* now_file = TestHookEnv("FORTIS_TEST_NEWS_NOW_FILE");
  if (now_file && *now_file) {
    if (FILE* handle = fopen(now_file, "r")) {
      char buffer[32]{};
      const size_t length = fread(buffer, 1, sizeof(buffer) - 1, handle);
      fclose(handle);
      buffer[length] = '\0';
      char* end = nullptr;
      const long long parsed = strtoll(buffer, &end, 10);
      if (end && end != buffer && parsed >= 0 && (!*end || *end == '\n'))
        return static_cast<int64_t>(parsed);
    }
  }
  const char* fixed = TestHookEnv("FORTIS_TEST_NEWS_NOW_MS");
  if (fixed && *fixed) {
    char* end = nullptr;
    const long long parsed = strtoll(fixed, &end, 10);
    if (end && !*end && parsed >= 0) return static_cast<int64_t>(parsed);
  }
  timeval now{};
  gettimeofday(&now, nullptr);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
}

// The TypeScript fixtures run with coordinatorRand () => 0.5: no jitter.
bool news_test_jitter(double* sample) {
  *sample = 0.5;
  return true;
}

// The contract fixture's ulid mock: "01JZ" + a 22-digit counter. The next
// value lives in FORTIS_TEST_NEWS_ID_FILE when set, so a restarted process
// continues the sequence.
pthread_mutex_t g_news_test_id_mutex = PTHREAD_MUTEX_INITIALIZER;
unsigned long long g_news_test_id_next = 0;
bool g_news_test_id_loaded = false;
bool news_test_new_id(char* output) {
  pthread_mutex_lock(&g_news_test_id_mutex);
  const char* file = TestHookEnv("FORTIS_TEST_NEWS_ID_FILE");
  if (!g_news_test_id_loaded) {
    if (file && *file) {
      if (FILE* handle = fopen(file, "r")) {
        if (fscanf(handle, "%llu", &g_news_test_id_next) != 1) g_news_test_id_next = 0;
        fclose(handle);
      }
    }
    g_news_test_id_loaded = true;
  }
  const unsigned long long id = g_news_test_id_next++;
  if (file && *file) {
    if (FILE* handle = fopen(file, "w")) {
      fprintf(handle, "%llu\n", g_news_test_id_next);
      fclose(handle);
    }
  }
  pthread_mutex_unlock(&g_news_test_id_mutex);
  snprintf(output, 27, "01JZ%022llu", id);
  return true;
}

// The news clock; its feed retry backoff sleeps return at once.
class NewsTestClock final : public fortis::base::Clock {
 public:
  int64_t NowMs() override { return news_test_now_ms(); }
  void SleepMs(uint32_t) override {}
};
NewsTestClock news_test_clock;
TestFnJitter<news_test_jitter> news_test_jitter_source;
TestFnIds<news_test_new_id> news_test_ids;

// FORTIS_TEST_MEDIA_DNS="host=a.b.c.d|a.b.c.d,host2=...,*=a.b.c.d" answers
// the media guard's lookups ("*" for any other host); an unlisted host
// without a "*" entry is a DNS failure.
bool news_test_resolve_in(const char* host, uint32_t* addresses, size_t* count) {
  const char* table = TestHookEnv("FORTIS_TEST_MEDIA_DNS");
  if (!table || !host) return false;
  const size_t host_length = strlen(host);
  for (const char* entry = table; *entry;) {
    const char* end = strchr(entry, ',');
    const size_t length = end ? static_cast<size_t>(end - entry) : strlen(entry);
    const char* equals = static_cast<const char*>(memchr(entry, '=', length));
    if (equals && static_cast<size_t>(equals - entry) == host_length &&
        memcmp(entry, host, host_length) == 0) {
      const char* cursor = equals + 1;
      while (cursor < entry + length && *count < 8) {
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (sscanf(cursor, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
        addresses[(*count)++] = a << 24 | b << 16 | c << 8 | d;
        const char* bar = static_cast<const char*>(memchr(cursor, '|', entry + length - cursor));
        if (!bar) break;
        cursor = bar + 1;
      }
      return *count > 0;
    }
    if (!end) break;
    entry = end + 1;
  }
  return false;
}

bool news_test_resolve(void*, const char* host, uint32_t* addresses, size_t* count) {
  *count = 0;
  return news_test_resolve_in(host, addresses, count) ||
         (*count == 0 && news_test_resolve_in("*", addresses, count));
}

struct NewsTestUpstream final : news::NewsOwnerStarter {
  char port[6]{};
  char plain_port[6]{};
  const char* ca = nullptr;
  bool Start(discovery::DiscoveryProviderOwner& owner) override {
    return owner.StartRedirected("localhost", port, ca, allow_news_test_address, nullptr);
  }
};

void handle_news_test_ingest(int client, const http::HttpRequest&, bool head, void* opaque) {
  auto* service = static_cast<news::NewsService*>(opaque);
  fortis::news::IngestResult result;
  const bool ok = service->RunIngestNow(&result);
  char body[320];
  snprintf(body, sizeof(body),
           "{\"ok\":%s,\"fetched\":%zu,\"fresh\":%zu,\"stored\":%zu,\"pruned\":%zu,"
           "\"feedsFailed\":%zu,\"persistFailed\":%s,\"pruneFailed\":%s,"
           "\"arenaUsed\":%zu,\"scratchPeak\":%zu}",
           ok ? "true" : "false", result.fetched, result.fresh, result.stored, result.pruned,
           result.feeds_failed, result.persist_failed ? "true" : "false",
           result.prune_failed ? "true" : "false", result.run_arena_used, result.scratch_peak);
  respond(client, 200, "OK", body, head);
}

void handle_news_test_prune(int client, const http::HttpRequest&, bool head, void* opaque) {
  auto* service = static_cast<news::NewsService*>(opaque);
  size_t orphans = 0;
  const size_t freed = service->RunMediaPruneNow(news_test_now_ms(), &orphans);
  char body[96];
  snprintf(body, sizeof(body), "{\"freed\":%zu,\"orphans\":%zu}", freed, orphans);
  respond(client, 200, "OK", body, head);
}

// GET /probe/test-news-seed-samples?coin=&day=&from=&step=&count=&base=
// stores one news-samples:v1:<coin>:<day> SWR entry (the reaction sample
// cache) at the test clock: `count` points every `step` ms from `from`, so a
// test can hold dense sample arrays without a provider round trip each.
fortis::storage::KvStore* g_news_test_kv = nullptr;
void handle_news_test_seed_samples(int client, const http::HttpRequest& request, bool head, void*) {
  const char* query = request.raw_target ? strchr(request.raw_target, '?') : nullptr;
  char coin[64] = {};
  long long day = 0, from = 0, step = 0, count = 0, base = 1;
  for (const char* p = query ? query + 1 : ""; *p;) {
    const char* end = strchr(p, '&');
    const size_t length = end ? static_cast<size_t>(end - p) : strlen(p);
    char pair[128] = {};
    if (length < sizeof(pair)) memcpy(pair, p, length);
    if (strncmp(pair, "coin=", 5) == 0) fortis::base::CopyCString(coin, pair + 5);
    else if (strncmp(pair, "day=", 4) == 0) day = strtoll(pair + 4, nullptr, 10);
    else if (strncmp(pair, "from=", 5) == 0) from = strtoll(pair + 5, nullptr, 10);
    else if (strncmp(pair, "step=", 5) == 0) step = strtoll(pair + 5, nullptr, 10);
    else if (strncmp(pair, "count=", 6) == 0) count = strtoll(pair + 6, nullptr, 10);
    else if (strncmp(pair, "base=", 5) == 0) base = strtoll(pair + 5, nullptr, 10);
    if (!end) break;
    p = end + 1;
  }
  const size_t capacity = 900 * 1024;
  unsigned char* body = static_cast<unsigned char*>(malloc(capacity));
  unsigned char* scratch = static_cast<unsigned char*>(malloc(1024 * 1024));
  bool ok = body && scratch && coin[0] && count >= 0 && step > 0 && g_news_test_kv;
  size_t used = 0;
  if (ok) {
    body[used++] = '[';
    for (long long i = 0; ok && i < count; ++i) {
      const int written = snprintf(reinterpret_cast<char*>(body) + used, capacity - used,
                                   "%s[%lld,%lld.25]", i ? "," : "", from + i * step, base + i % 97);
      ok = written > 0 && static_cast<size_t>(written) < capacity - used - 1;
      if (ok) used += static_cast<size_t>(written);
    }
    body[used++] = ']';
  }
  if (ok) {
    static const discovery::DiscoveryCacheSeams seams{&news_test_clock, &news_test_jitter_source};
    discovery::DiscoveryCache cache(g_news_test_kv, scratch, 1024 * 1024, &seams);
    char key[160];
    snprintf(key, sizeof(key), "news-samples:v1:%s:%lld", coin, day);
    int64_t as_of = 0;
    ok = cache.Write(key, discovery::DiscoveryCacheTtl{60000, 300000}, news_test_now_ms(), body, used,
                     &as_of);
  }
  free(body);
  free(scratch);
  char reply[96];
  snprintf(reply, sizeof(reply), "{\"ok\":%s,\"bytes\":%zu}", ok ? "true" : "false", used);
  respond(client, ok ? 200 : 400, ok ? "OK" : "Bad Request", reply, head);
}

void handle_news_test_jobs(int client, const http::HttpRequest&, bool head, void* opaque) {
  auto* service = static_cast<news::NewsService*>(opaque);
  char body[96];
  snprintf(body, sizeof(body), "{\"ingestRuns\":%llu,\"pruneRuns\":%llu}",
           static_cast<unsigned long long>(service->ingest_runs()),
           static_cast<unsigned long long>(service->prune_runs()));
  respond(client, 200, "OK", body, head);
}

// The value of `name` in the request query, percent-decoded ("+" kept).
bool news_test_query_value(const http::HttpRequest& request, const char* name, char* output,
                           size_t capacity, size_t* length) {
  const char* query = request.raw_target ? strchr(request.raw_target, '?') : nullptr;
  const size_t name_length = strlen(name);
  for (const char* p = query ? query + 1 : ""; *p;) {
    const char* end = strchr(p, '&');
    const size_t pair = end ? static_cast<size_t>(end - p) : strlen(p);
    if (pair > name_length && strncmp(p, name, name_length) == 0 && p[name_length] == '=') {
      size_t used = 0;
      for (size_t i = name_length + 1; i < pair; ++i) {
        if (used + 1 >= capacity) return false;
        unsigned value = 0;
        if (p[i] == '%' && i + 2 < pair && sscanf(p + i + 1, "%2x", &value) == 1) {
          output[used++] = static_cast<char>(value);
          i += 2;
        } else {
          output[used++] = p[i];
        }
      }
      output[used] = '\0';
      *length = used;
      return true;
    }
    if (!end) break;
    p = end + 1;
  }
  return false;
}

// GET /probe/test-media-register?url=<percent-encoded> registers one source
// URL at the news test clock, as a token, market, news or NFT read does
// (MediaFacet.register): {"key":"<32 hex>"} or {"key":null}.
void handle_media_test_register(int client, const http::HttpRequest& request, bool head, void*) {
  char* url = static_cast<char*>(malloc(16384));
  size_t length = 0;
  char key[fortis::discovery::market_view::kMediaKeyChars + 1] = {};
  const bool ok = url && news_test_query_value(request, "url", url, 16384, &length) &&
                  discovery::RegisterMediaSource(g_news_test_kv, url, length, news_test_now_ms(), key);
  free(url);
  char body[96];
  if (ok) snprintf(body, sizeof(body), "{\"key\":\"%s\"}", key);
  else snprintf(body, sizeof(body), "{\"key\":null}");
  respond(client, 200, "OK", body, head);
}

// GET /probe/test-media-record?key=<32 hex>: the stored record at the news
// test clock and the live size of each blob chunk:
// {"record":<record JSON>|null,"chunks":[size|null, ...4]}.
void handle_media_test_record(int client, const http::HttpRequest& request, bool head, void*) {
  char key[64] = {};
  size_t key_length = 0;
  if (!g_news_test_kv || !news_test_query_value(request, "key", key, 33, &key_length) ||
      key_length != 32) {
    respond(client, 400, "Bad Request", "{\"error\":\"key\"}", head);
    return;
  }
  const int64_t now = news_test_now_ms();
  const size_t record_capacity = 64 * 1024;
  char* body = static_cast<char*>(malloc(record_capacity + 256));
  unsigned char* scratch = static_cast<unsigned char*>(malloc(512 * 1024));
  if (!body || !scratch) {
    free(body); free(scratch);
    respond(client, 500, "Internal Server Error", "{\"error\":\"memory\"}", head);
    return;
  }
  char name[64];
  snprintf(name, sizeof(name), "media/item/%s", key);
  size_t size = 0;
  size_t used = static_cast<size_t>(snprintf(body, 16, "{\"record\":"));
  if (g_news_test_kv->GetStatus(name, now, body + used, record_capacity, &size) ==
      fortis::storage::KvReadStatus::Found) used += size;
  else used += static_cast<size_t>(snprintf(body + used, 8, "null"));
  used += static_cast<size_t>(snprintf(body + used, 16, ",\"chunks\":["));
  for (int i = 0; i < 4; ++i) {
    snprintf(name, sizeof(name), "media/blob/%s/%d", key, i);
    const bool found = g_news_test_kv->GetStatus(name, now, scratch, 512 * 1024, &size) ==
                       fortis::storage::KvReadStatus::Found;
    used += static_cast<size_t>(found ? snprintf(body + used, 24, "%s%zu", i ? "," : "", size)
                                      : snprintf(body + used, 24, "%snull", i ? "," : ""));
  }
  used += static_cast<size_t>(snprintf(body + used, 4, "]}"));
  respond_bytes(client, 200, "OK", "application/json", reinterpret_cast<const uint8_t*>(body), used,
                head);
  free(body);
  free(scratch);
}

// ---- R28–R30 news read probes (tests/news_reads_edges_test.py) -----------------
// Each mirrors one store operation the TypeScript oracle performs directly
// (KvNewsStore, PriceSeriesStore, EmbeddedStore) at the news test clock.

const fortis::codec::tj::Value* news_test_body(fortis::codec::tj::Arena& arena, const http::HttpRequest& request) {
  if (!request.body || !request.body_length) return nullptr;
  const fortis::codec::tj::Value* body =
      fortis::codec::tj::Parse(arena, reinterpret_cast<const uint8_t*>(request.body), request.body_length);
  return fortis::codec::tj::IsObject(body) ? body : nullptr;
}


// POST /probe/test-news-persist {"stories": [<news_store.h story JSON>, …]}:
// KvNewsStore.persist of those stories at the news test clock.
void handle_news_test_persist_stories(int client, const http::HttpRequest& request, bool head, void*) {
  namespace tj = fortis::codec::tj;
  tj::Arena arena = tj::GrowingArena(16 * 1024 * 1024);
  const tj::Value* stories = tj::Get(news_test_body(arena, request), "stories");
  const size_t news_bytes = 32 * 1024 * 1024;
  const size_t text_bytes = 1024 * 1024;
  uint8_t* memory = static_cast<uint8_t*>(malloc(news_bytes));
  uint8_t* text = static_cast<uint8_t*>(malloc(text_bytes));
  bool ok = g_news_test_kv && memory && text && tj::IsArray(stories);
  fortis::news::Arena news_arena{memory, news_bytes};
  fortis::news::Cluster** clusters = nullptr;
  if (ok) {
    clusters = static_cast<fortis::news::Cluster**>(
        news_arena.Alloc(sizeof(fortis::news::Cluster*) * (stories->count + 1), alignof(fortis::news::Cluster*)));
    ok = clusters != nullptr;
  }
  for (uint32_t i = 0; ok && i < stories->count; ++i) {
    tj::OutBuffer out{text, text_bytes, 0, true};
    auto* cluster = static_cast<fortis::news::Cluster*>(
        news_arena.Alloc(sizeof(fortis::news::Cluster), alignof(fortis::news::Cluster)));
    ok = cluster && tj::Stringify(stories->items[i], out) && out.ok;
    if (ok) {
      new (cluster) fortis::news::Cluster();
      ok = fortis::news::DecodeCluster(text, out.used, &news_arena, cluster);
      clusters[i] = cluster;
    }
  }
  if (ok) {
    const fortis::news::NewsStore news{g_news_test_kv, nullptr};
    ok = fortis::news::Persist(news, clusters, stories->count, news_test_clock, &news_arena) ==
         fortis::news::StoreStatus::kOk;
  }
  free(memory);
  free(text);
  tj::FreeArena(arena);
  if (ok) respond(client, 200, "OK", "{\"persisted\":true}", head);
  else respond(client, 500, "Internal Server Error", "{\"error\":\"NEWS\"}", head);
}

// POST /probe/test-news-raw {"key", "value": <raw text> | null, "expiresAt"}:
// EmbeddedStore put/delete of one record (the oracle's corrupt and drop steps).
void handle_news_test_raw(int client, const http::HttpRequest& request, bool head, void*) {
  namespace tj = fortis::codec::tj;
  tj::Arena arena = tj::GrowingArena(1024 * 1024);
  const tj::Value* body = news_test_body(arena, request);
  const tj::Value* key = tj::Get(body, "key");
  const tj::Value* value = tj::Get(body, "value");
  const tj::Value* expires = tj::Get(body, "expiresAt");
  bool ok = g_news_test_kv && tj::IsString(key);
  if (ok) {
    const bool put = tj::IsString(value) && tj::IsNumber(expires);
    const fortis::storage::KvMutation mutation{key->text, put ? value->text : nullptr, put ? value->length : 0,
                                      put ? static_cast<int64_t>(expires->number) : 0, !put};
    ok = g_news_test_kv->Write(&mutation, 1);
  }
  tj::FreeArena(arena);
  respond(client, ok ? 200 : 400, ok ? "OK" : "Bad Request", ok ? "{\"ok\":true}" : "{\"ok\":false}", head);
}

// GET /probe/test-news-prune-stories?before=<ms>: KvNewsStore.prune(before).
void handle_news_test_prune_stories(int client, const http::HttpRequest& request, bool head, void*) {
  char value[32] = {};
  size_t length = 0;
  const size_t arena_bytes = 32 * 1024 * 1024;
  uint8_t* memory = static_cast<uint8_t*>(malloc(arena_bytes));
  size_t removed = 0;
  bool ok = g_news_test_kv && memory && news_test_query_value(request, "before", value, sizeof(value), &length);
  if (ok) {
    fortis::news::Arena arena{memory, arena_bytes};
    const fortis::news::NewsStore news{g_news_test_kv, nullptr};
    ok = fortis::news::Prune(news, strtoll(value, nullptr, 10), &arena, &removed) == fortis::news::StoreStatus::kOk;
  }
  free(memory);
  char body[64];
  snprintf(body, sizeof(body), "{\"removed\":%zu}", removed);
  respond(client, ok ? 200 : 500, ok ? "OK" : "Internal Server Error", body, head);
}

// POST /probe/test-news-spots {"spots": [{"coinId", "price", "change24hPct"}]}:
// PriceSeriesStore.recordSpots of usd rows observed at the news test clock.
void handle_news_test_spots(int client, const http::HttpRequest& request, bool head, void*) {
  namespace tj = fortis::codec::tj;
  tj::Arena arena = tj::GrowingArena(1024 * 1024);
  const tj::Value* spots = tj::Get(news_test_body(arena, request), "spots");
  bool ok = g_news_test_kv && tj::IsArray(spots) && spots->count <= 32;
  fortis::prices::PriceSpotQuoteInput quotes[32]{};
  const int64_t now = news_test_now_ms();
  for (uint32_t i = 0; ok && i < spots->count; ++i) {
    const tj::Value* id = tj::Get(spots->items[i], "coinId");
    const tj::Value* price = tj::Get(spots->items[i], "price");
    const tj::Value* change = tj::Get(spots->items[i], "change24hPct");
    ok = tj::IsString(id) && tj::IsString(price);
    if (!ok) break;
    quotes[i].source_id = id->text;
    quotes[i].price = strtod(price->text, nullptr);
    quotes[i].has_change24h_pct = tj::IsString(change);
    quotes[i].change24h_pct = tj::IsString(change) ? strtod(change->text, nullptr) : 0;
    quotes[i].as_of_ms = now;
  }
  if (ok) {
    fortis::prices::PriceSeriesStore prices(*g_news_test_kv);
    ok = prices.RecordSpots("usd", "coingecko", quotes, spots->count, now) ==
         fortis::prices::PriceSeriesStoreStatus::kStored;
  }
  tj::FreeArena(arena);
  respond(client, ok ? 200 : 500, ok ? "OK" : "Internal Server Error", ok ? "{\"ok\":true}" : "{\"ok\":false}",
          head);
}

// POST /probe/test-news-state {"ids": [...], "keys": [...]} at the news test
// clock: {"recent": unexpired story ids newest first (recentClusters(0)),
// "lastIngestAt": ms|null, "items": {id: live?}, "swr": {key: {"d", "asOf",
// "freshUntil", "staleUntil"} | null}} — the oracle's state record.
void handle_news_test_state(int client, const http::HttpRequest& request, bool head, void*) {
  namespace tj = fortis::codec::tj;
  tj::Arena arena = tj::GrowingArena(1024 * 1024);
  const tj::Value* body = news_test_body(arena, request);
  const tj::Value* ids = tj::Get(body, "ids");
  const tj::Value* keys = tj::Get(body, "keys");
  const size_t news_bytes = 16 * 1024 * 1024;
  const size_t out_bytes = 8 * 1024 * 1024;
  uint8_t* memory = static_cast<uint8_t*>(malloc(news_bytes));
  uint8_t* output = static_cast<uint8_t*>(malloc(out_bytes));
  unsigned char* scratch = static_cast<unsigned char*>(malloc(2 * 1024 * 1024));
  bool ok = g_news_test_kv && memory && output && scratch && tj::IsArray(ids) && tj::IsArray(keys);
  const int64_t now = news_test_now_ms();
  fortis::news::OutBuffer out{output, out_bytes};
  if (ok) {
    fortis::news::Arena news_arena{memory, news_bytes};
    const fortis::news::NewsStore news{g_news_test_kv, nullptr};
    fortis::news::Cluster* clusters = nullptr;
    size_t count = 0;
    bool found = false;
    int64_t last = 0;
    ok = fortis::news::RecentClusters(news, 0, now, &news_arena, &clusters, &count) ==
             fortis::news::StoreStatus::kOk &&
         fortis::news::LastIngestAt(news, now, &found, &last) == fortis::news::StoreStatus::kOk;
    out.Text("{\"recent\":[");
    for (size_t i = 0; ok && i < count; ++i) {
      if (i) out.Byte(',');
      fortis::news::AppendJsonString(out, clusters[i].id);
    }
    char number[64];
    if (found) snprintf(number, sizeof(number), "],\"lastIngestAt\":%lld,\"items\":{", static_cast<long long>(last));
    else snprintf(number, sizeof(number), "],\"lastIngestAt\":null,\"items\":{");
    out.Text(number);
    for (uint32_t i = 0; ok && i < ids->count; ++i) {
      if (!tj::IsString(ids->items[i])) { ok = false; break; }
      char name[160];
      snprintf(name, sizeof(name), "news/item/%s", ids->items[i]->text);
      uint8_t probe[1];
      size_t size = 0;
      const fortis::storage::KvReadStatus status = g_news_test_kv->GetStatus(name, now, probe, sizeof(probe), &size);
      const bool live = status == fortis::storage::KvReadStatus::Found || status == fortis::storage::KvReadStatus::BufferTooSmall;
      out.Text(i ? ",\"" : "\"");
      out.Text(ids->items[i]->text);
      out.Text(live ? "\":true" : "\":false");
    }
    out.Text("},\"swr\":{");
    static const discovery::DiscoveryCacheSeams seams{&news_test_clock, &news_test_jitter_source};
    discovery::DiscoveryCache cache(g_news_test_kv, scratch, 2 * 1024 * 1024, &seams);
    for (uint32_t i = 0; ok && i < keys->count; ++i) {
      if (!tj::IsString(keys->items[i])) { ok = false; break; }
      out.Text(i ? ",\"" : "\"");
      out.Text(keys->items[i]->text);
      out.Text("\":");
      const discovery::DiscoveryCacheEntry entry = cache.Read(keys->items[i]->text, now);
      if (entry.state != discovery::DiscoveryCacheState::kFresh && entry.state != discovery::DiscoveryCacheState::kStale) {
        out.Text("null");
        continue;
      }
      char stamp[32];
      if (!discovery::DiscoveryIsoTimestamp(entry.as_of_ms, stamp)) { ok = false; break; }
      snprintf(number, sizeof(number), "{\"asOf\":\"%s\",\"freshUntil\":%lld,", stamp,
               static_cast<long long>(entry.fresh_until_ms));
      out.Text(number);
      snprintf(number, sizeof(number), "\"staleUntil\":%lld,\"d\":", static_cast<long long>(entry.stale_until_ms));
      out.Text(number);
      out.Append(entry.body, entry.body_length);
      out.Byte('}');
    }
    out.Text("}}");
    ok = ok && out.ok;
  }
  if (ok) respond_bytes(client, 200, "OK", "application/json", out.data, out.used, head);
  else respond(client, 500, "Internal Server Error", "{\"error\":\"NEWS\"}", head);
  free(memory);
  free(output);
  free(scratch);
  tj::FreeArena(arena);
}

// Development-tier fixture devices, one token per letter (the replays rotate
// tokens to stay inside the 240/min development quota).
bool seed_news_test_devices(fortis::storage::KvStore* kv) {
  fortis::auth::DeviceAuthStore auth(kv);
  // The news replay advances its clock independently of wall time. Seed
  // credentials on that clock and keep them valid across the retention-day
  // jumps in the fixture. This lifetime is test-only.
  const int64_t now_ms = news_test_now_ms();
  const char letters[] = "ABCDEFGHJKMNPQRSTVWXYZ";
  static const char kCrockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
  for (size_t i = 0; letters[i]; ++i) {
    char token[64];
    char id[27];
    snprintf(token, sizeof(token), "fintel_dev_");
    for (size_t j = 0; j < 43; ++j) token[11 + j] = letters[i];
    token[54] = '\0';
    snprintf(id, sizeof(id), "01ARZ3NDEKTSV4RRFFQ69G5FC%c", kCrockford[i]);
    if (!auth.SeedPriceRouteFixture(token, id, now_ms, 400LL * 86400000)) return false;
  }
  return true;
}

bool news_test_route = false;
NewsTestUpstream news_test_upstream{};
news::NewsServiceSeams news_test_seams{};

}  // namespace

bool ConfigureNewsTest(const TestPlaneConfig& config, PlaneOptions& options) {
  const char* news_flag = TestHookEnv("FORTIS_ENABLE_TEST_NEWS_ROUTES");
  news_test_route = news_flag && strcmp(news_flag, "1") == 0;
  const char* news_upstream_port = TestHookEnv("FORTIS_TEST_NEWS_UPSTREAM_PORT");
  const char* news_upstream_plain_port = TestHookEnv("FORTIS_TEST_NEWS_UPSTREAM_PLAIN_PORT");
  const char* news_upstream_ca = TestHookEnv("FORTIS_TEST_NEWS_UPSTREAM_CA");
  if (news_test_route && config.native_news_canary) {
    fprintf(stderr, "FORTIS_ENABLE_TEST_NEWS_ROUTES and "
                    "FORTIS_NATIVE_NEWS_CANARY are mutually exclusive\n");
    return false;
  }
  if (news_test_route && (parse_port(news_upstream_port) < 0 ||
      parse_port(news_upstream_plain_port) < 0 || !news_upstream_ca || !*news_upstream_ca ||
      !http::OutboundHttpsValidateCaBundle(news_upstream_ca))) {
    fprintf(stderr, "Invalid test-only news upstream configuration\n");
    return false;
  }
  if (news_test_route) {
    snprintf(news_test_upstream.port, sizeof(news_test_upstream.port), "%d",
             parse_port(news_upstream_port));
    snprintf(news_test_upstream.plain_port, sizeof(news_test_upstream.plain_port), "%d",
             parse_port(news_upstream_plain_port));
    news_test_upstream.ca = news_upstream_ca;
    news_test_seams.clock = &news_test_clock;
    news_test_seams.jitter = &news_test_jitter_source;
    news_test_seams.new_id = &news_test_ids;
    news_test_seams.feed_host = "localhost";
    news_test_seams.feed_host_header = "x-fortis-test-host";
    news_test_seams.feed_port = news_test_upstream.port;
    news_test_seams.feed_plain_port = news_test_upstream.plain_port;
    news_test_seams.feed_ca = news_upstream_ca;
    news_test_seams.allow_feed_address = allow_news_test_address;
    const char* manual = TestHookEnv("FORTIS_TEST_NEWS_MANUAL_JOBS");
    news_test_seams.manual_jobs = !(manual && strcmp(manual, "0") == 0);
    news_test_seams.media.clock = &news_test_clock;
    news_test_seams.media.resolve = news_test_resolve;
    news_test_seams.media.dial_host = "localhost";
    news_test_seams.media.dial_host_header = "x-fortis-test-host";
    news_test_seams.media.dial_port = news_test_upstream.port;
    news_test_seams.media.dial_plain_port = news_test_upstream.plain_port;
    news_test_seams.media.dial_ca = news_upstream_ca;
    options.news_seams = &news_test_seams;
    options.news_owner = &news_test_upstream;
    options.news = true;
  }
  return true;
}

bool SeedNewsTest(fortis::storage::KvStore* kv) {
  if (news_test_route) g_news_test_kv = kv;
  if (news_test_route && !seed_news_test_devices(kv)) {
    fprintf(stderr, "Failed to seed test news fixtures\n");
    return false;
  }
  return true;
}

bool RegisterNewsTestRoutes(http::HttpRouter& router, Planes& planes) {
  return !news_test_route ||
         (router.Add("GET", "/probe/test-news-ingest", handle_news_test_ingest, planes.news ? &*planes.news : nullptr) &&
          router.Add("GET", "/probe/test-media-prune", handle_news_test_prune, planes.news ? &*planes.news : nullptr) &&
          router.Add("GET", "/probe/test-news-jobs", handle_news_test_jobs, planes.news ? &*planes.news : nullptr) &&
          router.Add("GET", "/probe/test-news-seed-samples", handle_news_test_seed_samples) &&
          router.Add("GET", "/probe/test-media-register", handle_media_test_register) &&
          router.Add("GET", "/probe/test-media-record", handle_media_test_record) &&
          router.Add("POST", "/probe/test-news-persist", handle_news_test_persist_stories) &&
          router.Add("POST", "/probe/test-news-raw", handle_news_test_raw) &&
          router.Add("GET", "/probe/test-news-prune-stories", handle_news_test_prune_stories) &&
          router.Add("POST", "/probe/test-news-spots", handle_news_test_spots) &&
          router.Add("POST", "/probe/test-news-state", handle_news_test_state));
}

void NewsTestSweepClock(fortis::base::Clock** clock) {
  if (news_test_route) *clock = &news_test_clock;
}

}  // namespace fortis::app
#endif
