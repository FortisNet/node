// Test probes of the spot, history and discovery planes (and the coin
// popularity tracker they share with the token plane). Compiled empty
// without their flags.
#include "test_probes.h"


#if defined(FORTIS_PRICE_ROUTE_TEST_ONLY) || defined(FORTIS_HISTORY_ROUTE_TEST_ONLY) || \
    defined(FORTIS_DISCOVERY_ROUTE_TEST_ONLY) || defined(FORTIS_TOKEN_ROUTE_TEST_ONLY)
#include "app_support.h"
#include "device_auth.h"
#include "outbound_https.h"
#include "service_wiring.h"
#include "test_hook_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#endif
#ifdef FORTIS_HISTORY_ROUTE_TEST_ONLY
#include "price_series_store.h"
#include "kv_store.h"
#endif
#ifdef FORTIS_PRICE_ROUTE_TEST_ONLY
#include "kv_store.h"
#endif
#ifdef FORTIS_DISCOVERY_ROUTE_TEST_ONLY
#include "device_auth.h"
#include "discovery_cache.h"
#include "kv_runtime.h"
#include "kv_store.h"
#include "market_view.h"
#include "token_comparison_seed.h"
#include "token_json.h"
#include "token_search_route.h"
#endif

namespace fortis::app {
namespace {

#if defined(FORTIS_TOKEN_ROUTE_TEST_ONLY) || defined(FORTIS_DISCOVERY_ROUTE_TEST_ONLY)
// GET /probe/test-coin-popularity: every tracked coin in insertion order with
// its exact counters and refresh mark, and the tracker's bounds.
void handle_coin_popularity_test(int client, const http::HttpRequest&, bool head, void* opaque) {
  const auto* popularity = static_cast<const fortis::discovery::CoinPopularity*>(opaque);
  fortis::discovery::PopularityRowList rows;
  if (!popularity->Snapshot(&rows)) { respond(client, 500, "Internal Server Error", "{}", head); return; }
  const fortis::discovery::CoinPopularityStats stats = popularity->Stats();
  size_t capacity = 512 + rows.count * 256;
  for (size_t i = 0; i < rows.count; ++i) capacity += rows.items[i].id_length * 6;
  char* body = static_cast<char*>(malloc(capacity));
  if (!body) { rows.Release(); respond(client, 500, "Internal Server Error", "{}", head); return; }
  size_t used = static_cast<size_t>(snprintf(body, capacity,
      "{\"stats\":{\"coins\":%zu,\"seenPairs\":%zu,\"seenAllocated\":%s,\"dropped\":%zu,"
      "\"allocatedBytes\":%zu,\"peakAllocatedBytes\":%zu},\"coins\":[",
      stats.coins, stats.seen_pairs, stats.seen_allocated ? "true" : "false", stats.dropped,
      stats.allocated_bytes, stats.peak_allocated_bytes));
  for (size_t i = 0; i < rows.count && used < capacity; ++i) {
    const fortis::discovery::PopularityRow& row = rows.items[i];
    used += static_cast<size_t>(snprintf(body + used, capacity - used, "%s{\"id\":\"", i ? "," : ""));
    for (size_t k = 0; k < row.id_length && used + 7 < capacity; ++k) {
      const unsigned char c = static_cast<unsigned char>(row.id[k]);
      if (c == '"' || c == '\\') { body[used++] = '\\'; body[used++] = static_cast<char>(c); }
      else if (c < 0x20) used += static_cast<size_t>(snprintf(body + used, capacity - used, "\\u%04x", c));
      else body[used++] = static_cast<char>(c);
    }
    used += static_cast<size_t>(snprintf(body + used, capacity - used,
        "\",\"users\":%.17g,\"hits\":%.17g,\"updatedAt\":%lld,\"lastRequestedAt\":%lld,"
        "\"refreshedAt\":",
        row.state.users, row.state.hits, static_cast<long long>(row.state.updated_at_ms),
        static_cast<long long>(row.state.last_requested_at_ms)));
    if (row.has_refreshed_at)
      used += static_cast<size_t>(snprintf(body + used, capacity - used, "%lld}",
                                           static_cast<long long>(row.refreshed_at_ms)));
    else
      used += static_cast<size_t>(snprintf(body + used, capacity - used, "null}"));
  }
  if (used + 3 < capacity) { body[used++] = ']'; body[used++] = '}'; body[used] = '\0'; }
  else used = 0;
  rows.Release();
  if (!used) { free(body); respond(client, 500, "Internal Server Error", "{}", head); return; }
  respond(client, 200, "OK", body, head);
  free(body);
}
#endif

#if defined(FORTIS_PRICE_ROUTE_TEST_ONLY) || defined(FORTIS_HISTORY_ROUTE_TEST_ONLY)
bool allow_price_test_address(void*, uint32_t address) { return address == 0x7f000001U; }
#endif
#ifdef FORTIS_PRICE_ROUTE_TEST_ONLY
int64_t price_test_now_ms() {
  timeval now{}; gettimeofday(&now, nullptr);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
}

// Test-only owner start: the loopback fixture upstream and its generated CA
// replace the production host, port, trust store and address policy.
struct PriceTestUpstream final : prices::PriceSpotOwnerStarter {
  char port[6]{};
  const char* ca = nullptr;
  bool Start(prices::SpotProviderOwnerStartup& owner) override {
    return owner.StartRedirected("localhost", port, ca, allow_price_test_address, nullptr);
  }
};

void handle_price_owner_active(int client, const http::HttpRequest&, bool head, void* opaque) {
  const auto* route = static_cast<const prices::PriceSpotRouteContext*>(opaque);
  char body[64];
  snprintf(body, sizeof(body), "{\"active\":%zu}", route->owner->active_request_count());
  respond(client, 200, "OK", body, head);
}
#endif
#ifdef FORTIS_DISCOVERY_ROUTE_TEST_ONLY
bool allow_discovery_test_address(void*, uint32_t address) { return address == 0x7f000001U; }

// Test-only discovery owner start: every upstream (CoinGecko, Jupiter,
// Taostats, Robinhood) goes to the loopback fixture with its generated CA.
struct DiscoveryTestUpstream final : discovery::DiscoveryOwnerStarter {
  char port[6]{};
  const char* ca = nullptr;
  bool Start(discovery::DiscoveryProviderOwner& owner) override {
    return owner.StartRedirected("localhost", port, ca, allow_discovery_test_address, nullptr);
  }
};

// The discovery and comparison clock: a file lets one process move it between
// requests; the environment value fixes it; -1 keeps the wall clock.
int64_t discovery_test_now_ms() {
  const char* now_file = TestHookEnv("FORTIS_TEST_DISCOVERY_NOW_FILE");
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
  const char* fixed = TestHookEnv("FORTIS_TEST_DISCOVERY_NOW_MS");
  if (fixed && *fixed) {
    char* end = nullptr;
    const long long parsed = strtoll(fixed, &end, 10);
    if (end && !*end && parsed >= 0) return static_cast<int64_t>(parsed);
  }
  return -1;
}

// The KV expiry sweeper's clock: the discovery clock when one is set (the
// fixture clock lags the wall clock, and the routes expire by it), else wall
// time.
int64_t discovery_test_sweep_now_ms() {
  const int64_t fixed = discovery_test_now_ms();
  if (fixed >= 0) return fixed;
  timeval now{};
  gettimeofday(&now, nullptr);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
}

// The TypeScript fixtures run with coordinatorRand () => 0.5: no jitter.
bool discovery_test_jitter(double* sample) {
  const char* fixed = TestHookEnv("FORTIS_TEST_DISCOVERY_JITTER");
  *sample = fixed && *fixed ? strtod(fixed, nullptr) : 0.5;
  return true;
}

TestFnClock<discovery_test_now_ms> discovery_test_clock;
TestFnClock<discovery_test_sweep_now_ms> discovery_test_sweep_clock;
TestFnJitter<discovery_test_jitter> discovery_test_jitter_source;

// POST /probe/test-discovery-state {keys: [...], logos: [...]} (R25): each
// discovery cache envelope readable at the discovery clock (asOf,
// freshUntil, staleUntil and the value's JSON text; a `search:` answer's
// native record rendered as TypeScript's JSON.stringify(items)) and each logo
// URL's media record (`media/item/<key>`), null when absent.
void handle_discovery_test_state(int client, const http::HttpRequest& request, bool head, void* opaque) {
  namespace tj = fortis::codec::tj;
  const auto* seams = static_cast<const discovery::DiscoveryCacheSeams*>(opaque);
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
  tj::Arena arena = tj::GrowingArena(16 * 1024 * 1024);
  const tj::Value* body = request.body && request.body_length
                              ? tj::Parse(arena, reinterpret_cast<const uint8_t*>(request.body), request.body_length)
                              : nullptr;
  const tj::Value* keys = tj::Get(body, "keys");
  const tj::Value* logos = tj::Get(body, "logos");
  const size_t scratch_bytes = 2 * 1024 * 1024;
  unsigned char* scratch = static_cast<unsigned char*>(malloc(scratch_bytes));
  unsigned char* rendered = static_cast<unsigned char*>(malloc(scratch_bytes));
  discovery::DiscoveryCache cache(kv, scratch, scratch_bytes, seams);
  const int64_t now = cache.Now();
  tj::Value* out = tj::Obj(arena);
  tj::Value* envelopes = tj::Arr(arena);
  tj::Value* media = tj::Arr(arena);
  bool ok = scratch && rendered && out && envelopes && media && tj::IsArray(keys) && tj::IsArray(logos);
  for (uint32_t i = 0; ok && i < keys->count; ++i) {
    const tj::Value* key = keys->items[i];
    ok = tj::IsString(key);
    if (!ok) break;
    // TypeScript's `search:<id>:<term>` as the native KV key (a NUL or '%'
    // of the term escaped; no network id holds either).
    char native_key[2048];
    const bool search = strncmp(key->text, "search:", 7) == 0;
    size_t used = 0;
    for (uint32_t c = 0; ok && c < key->length; ++c) {
      const char byte = key->text[c];
      const char* escape = !search && byte == '\0' ? nullptr
                           : search && byte == '\0' ? "%00" : search && byte == '%' ? "%25" : "";
      ok = escape && used + 4 < sizeof(native_key);
      if (!ok) break;
      if (*escape) { memcpy(native_key + used, escape, 3); used += 3; }
      else native_key[used++] = byte;
    }
    native_key[ok ? used : 0] = '\0';
    if (!ok) break;
    const discovery::DiscoveryCacheEntry entry = cache.Read(native_key, now);
    tj::Value* envelope = tj::Null(arena);
    if (entry.state == discovery::DiscoveryCacheState::kFresh || entry.state == discovery::DiscoveryCacheState::kStale) {
      const char* d = reinterpret_cast<const char*>(entry.body);
      size_t d_length = entry.body_length;
      if (search) {
        ok = discovery::TokenSearchRecordJson(entry.body, entry.body_length, rendered, scratch_bytes, &d_length);
        d = reinterpret_cast<const char*>(rendered);
      }
      envelope = tj::Obj(arena);
      ok = ok && envelope &&
           tj::Set(arena, envelope, "asOf", tj::Num(arena, static_cast<double>(entry.as_of_ms))) &&
           tj::Set(arena, envelope, "freshUntil", tj::Num(arena, static_cast<double>(entry.fresh_until_ms))) &&
           tj::Set(arena, envelope, "staleUntil", tj::Num(arena, static_cast<double>(entry.stale_until_ms))) &&
           tj::Set(arena, envelope, "d", tj::Str(arena, d, d_length));
    }
    tj::Value* row = tj::Obj(arena);
    ok = ok && row && tj::Set(arena, row, "key", tj::Clone(arena, key)) && tj::Set(arena, row, "envelope", envelope) &&
         tj::Push(arena, envelopes, row);
  }
  for (uint32_t i = 0; ok && i < logos->count; ++i) {
    const tj::Value* url = logos->items[i];
    ok = tj::IsString(url);
    if (!ok) break;
    char key[fortis::discovery::market_view::kMediaKeyChars + 1];
    fortis::discovery::market_view::MediaKeyFor(url->text, url->length, key);
    char name[64];
    snprintf(name, sizeof(name), "media/item/%s", key);
    size_t size = 0;
    tj::Value* record = tj::Null(arena);
    if (kv->GetStatus(name, now, scratch, scratch_bytes, &size) == fortis::storage::KvReadStatus::Found) {
      record = tj::Parse(arena, scratch, size);
      ok = record != nullptr;
    }
    tj::Value* row = tj::Obj(arena);
    ok = ok && row && tj::Set(arena, row, "sourceUrl", tj::Clone(arena, url)) &&
         tj::Set(arena, row, "key", tj::Str(arena, key)) && tj::Set(arena, row, "record", record) &&
         tj::Push(arena, media, row);
  }
  free(scratch);
  free(rendered);
  ok = ok && tj::Set(arena, out, "envelopes", envelopes) && tj::Set(arena, out, "media", media);
  uint8_t* text = ok ? static_cast<uint8_t*>(malloc(8 * 1024 * 1024)) : nullptr;
  tj::OutBuffer buffer{text, 8 * 1024 * 1024, 0, true};
  ok = ok && text && tj::Stringify(out, buffer) && buffer.ok;
  if (ok) {
    http::HttpResponse response{200, "OK", "application/json", text, buffer.used};
    WriteHttpResponse(client, response, head);
  } else {
    respond(client, 500, "Internal Server Error", "{\"error\":\"STATE\"}", head);
  }
  free(text);
  tj::FreeArena(arena);
}

// Development-tier fixture devices: the comparison replay's H token and one
// token per letter for the discovery replays, which rotate tokens to stay
// inside the 240/min development quota.
bool seed_discovery_test_devices(fortis::storage::KvStore* kv) {
  fortis::auth::DeviceAuthStore auth(kv);
  timeval now{}; gettimeofday(&now, nullptr);
  const int64_t now_ms = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
  const char letters[] = "HABCDEFGIJKLMNOPQRSTUVWXYZ";
  for (size_t i = 0; letters[i]; ++i) {
    char token[64];
    char id[27];
    snprintf(token, sizeof(token), "fintel_dev_");
    for (size_t j = 0; j < 43; ++j) token[11 + j] = letters[i];
    token[54] = '\0';
    // The H token keeps the history fixture's device id; the others take
    // Crockford base32 suffixes (device ids exclude I, L, O and U).
    static const char kCrockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    snprintf(id, sizeof(id), "01ARZ3NDEKTSV4RRFFQ69G5F%c%c", letters[i] == 'H' ? 'A' : 'B',
             letters[i] == 'H' ? 'W' : kCrockford[i]);
    if (!auth.SeedPriceRouteFixture(token, id, now_ms)) return false;
  }
  return true;
}
#endif
#ifdef FORTIS_HISTORY_ROUTE_TEST_ONLY
// Test-only history owner start: both owners use the loopback fixture
// upstream and its generated CA instead of the production hosts, trust store
// and address policy. FORTIS_TEST_HISTORY_OWNER_START_FAIL=backfill fails
// before either owner starts; =ohlc fails after the backfill owner started,
// so a test can watch the composition stop it.
struct HistoryTestUpstream final : prices::PriceHistoryOwnersStarter {
  char port[6]{};
  const char* ca = nullptr;
  const char* fail = nullptr;
  bool Start(prices::PriceHistoryBackfillOwner& backfill, prices::OhlcProviderOwner& ohlc) override;
};

bool HistoryTestUpstream::Start(prices::PriceHistoryBackfillOwner& backfill, prices::OhlcProviderOwner& ohlc) {
  const char* failing = fail ? fail : "";
  if (strcmp(failing, "backfill") == 0) return false;
  if (!backfill.StartRedirected("localhost", port, ca, allow_price_test_address, nullptr)) return false;
  if (strcmp(failing, "ohlc") == 0) return false;
  return ohlc.StartRedirected("localhost", port, ca, allow_price_test_address, nullptr);
}

// The OHLC cache clock: a file lets one process move it between requests; the
// environment value fixes it for the process lifetime; -1 keeps the wall clock.
int64_t history_test_ohlc_now_ms() {
  const char* now_file = TestHookEnv("FORTIS_TEST_OHLC_NOW_FILE");
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
  const char* fixed = TestHookEnv("FORTIS_TEST_OHLC_NOW_MS");
  if (fixed && *fixed) {
    char* end = nullptr;
    const long long parsed = strtoll(fixed, &end, 10);
    if (end && !*end && parsed >= 0) return static_cast<int64_t>(parsed);
  }
  return -1;
}

// Pins the OHLC freshness jitter sample so its boundaries are exact.
bool history_test_ohlc_jitter(double* sample) {
  const char* fixed = TestHookEnv("FORTIS_TEST_OHLC_RAND");
  if (!fixed || !*fixed) return false;
  char* end = nullptr;
  const double parsed = strtod(fixed, &end);
  if (!end || *end || !(parsed >= 0 && parsed <= 1)) return false;
  *sample = parsed;
  return true;
}

TestFnClock<history_test_ohlc_now_ms> history_test_ohlc_clock;
TestFnJitter<history_test_ohlc_jitter> history_test_ohlc_jitter_source;

// A source ID whose batch-route series reads report an I/O failure.
bool history_test_fail_source(void*, const char* source_id) {
  const char* failing = TestHookEnv("FORTIS_TEST_HISTORY_FAIL_SOURCE");
  return failing && *failing && source_id && strcmp(failing, source_id) == 0;
}

// The fixture candles and development token the history replays expect.
bool seed_history_test_fixtures(fortis::storage::KvStore* kv) {
  fortis::prices::PriceSeriesStore series(*kv);
  const int64_t first = 1767225600000LL;
  fortis::prices::PriceSeriesCandleInput hourly[6]{};
  fortis::prices::PriceSeriesCandleInput daily[6]{};
  for (size_t i = 0; i < 6; ++i) {
    hourly[i] = {first + static_cast<int64_t>(i) * 3600000LL,
        100.0 + i, 102.0 + i, 99.0 + i, 101.0 + i, 0.5 + i, true};
    daily[i] = {first + static_cast<int64_t>(i) * 86400000LL,
        200.0 + i, 205.0 + i, 198.0 + i, 203.0 + i, 10.0 + i, true};
  }
  const int64_t now_ms = static_cast<int64_t>(time(nullptr)) * 1000;
  if (series.UpsertCandles(fortis::prices::PriceSeriesInterval::k1h, "usd",
        "ethereum", hourly, 6, now_ms) != fortis::prices::PriceSeriesStoreStatus::kStored ||
      series.UpsertCandles(fortis::prices::PriceSeriesInterval::k1d, "usd",
        "ethereum", daily, 6, now_ms) != fortis::prices::PriceSeriesStoreStatus::kStored) {
    fprintf(stderr, "Failed to seed test history route fixture candles\n"); return false;
  }
  // The batch route fixture's 50-ID case: two cached hourly candles each.
  const char* seed_batch = TestHookEnv("FORTIS_TEST_HISTORY_BATCH_SEED");
  if (seed_batch && strcmp(seed_batch, "1") == 0) {
    for (int coin = 0; coin < 50; ++coin) {
      char source_id[16];
      snprintf(source_id, sizeof(source_id), "coin-%02d", coin);
      fortis::prices::PriceSeriesCandleInput rows[2]{};
      for (int j = 0; j < 2; ++j)
        rows[j] = {first + j * 3600000LL, 10.0 + coin, 11.0 + coin, 9.0 + coin,
                   10.0 + coin + 0.5 * j, 1.0 + coin, true};
      if (series.UpsertCandles(fortis::prices::PriceSeriesInterval::k1h, "usd",
            source_id, rows, 2, now_ms) != fortis::prices::PriceSeriesStoreStatus::kStored) {
        fprintf(stderr, "Failed to seed test history batch fixture candles\n"); return false;
      }
    }
  }
  // The ten-year daily fixture: 365 cached daily candles ending
  // the day before FIRST for two coins, as its TypeScript app seeds them.
  const char* seed_decade = TestHookEnv("FORTIS_TEST_HISTORY_DECADE_SEED");
  if (seed_decade && strcmp(seed_decade, "1") == 0) {
    const char* decade_coins[] = {"seeded-coin", "locked-coin"};
    const int64_t seed_from = first - 365 * 86400000LL;
    for (const char* source_id : decade_coins) {
      for (int offset = 0; offset < 365; offset += 200) {
        const int rows_count = 365 - offset < 200 ? 365 - offset : 200;
        fortis::prices::PriceSeriesCandleInput rows[200]{};
        for (int j = 0; j < rows_count; ++j) {
          const int d = offset + j;
          rows[j] = {seed_from + d * 86400000LL, 50.0 + d, 51.0 + d, 49.0 + d,
                     50.5 + d, 7.0 + d, true};
        }
        if (series.UpsertCandles(fortis::prices::PriceSeriesInterval::k1d, "usd",
              source_id, rows, static_cast<size_t>(rows_count), now_ms) !=
            fortis::prices::PriceSeriesStoreStatus::kStored) {
          fprintf(stderr, "Failed to seed test history decade fixture candles\n"); return false;
        }
      }
    }
  }
  // The other-ID GET fixture: the candles its TypeScript app seeds
  // for recognized non-CoinGecko IDs, under their canonical source IDs, plus
  // one undecodable open row so a store read reports real KV corruption.
  const char* seed_other_ids = TestHookEnv("FORTIS_TEST_HISTORY_OTHER_IDS_SEED");
  if (seed_other_ids && strcmp(seed_other_ids, "1") == 0) {
    struct OtherIdSeed {
      const char* source_id;
      fortis::prices::PriceSeriesInterval interval;
      int count;
      double open, high, low, close, volume;
      bool has_volume, odd_volume_null;
    };
    const OtherIdSeed seeds[] = {
      {"spl:So11111111111111111111111111111111111111112", fortis::prices::PriceSeriesInterval::k1h,
       6, 1, 1.5, 0.5, 1.25, 100, true, false},
      {"spl:So11111111111111111111111111111111111111112", fortis::prices::PriceSeriesInterval::k1d,
       6, 2, 3, 1, 2.5, 200, true, true},
      {"tron:trc10:1002000", fortis::prices::PriceSeriesInterval::k1d,
       4, 0.125, 0.25, 0.0625, 0.1875, 5000, true, false},
      {"aptos:0x1::aptos_coin::AptosCoin", fortis::prices::PriceSeriesInterval::k1h,
       3, 8, 8.5, 7.5, 8.25, 0, false, false},
      {"eth-mainnet:erc20:0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48",
       fortis::prices::PriceSeriesInterval::k1h, 4, 0.75, 1, 0.5, 0.875, 42, true, false},
    };
    for (const auto& seed : seeds) {
      const int64_t width = seed.interval == fortis::prices::PriceSeriesInterval::k1h
          ? 3600000LL : 86400000LL;
      fortis::prices::PriceSeriesCandleInput rows[6]{};
      for (int i = 0; i < seed.count; ++i) {
        const bool has_volume = seed.has_volume && !(seed.odd_volume_null && i % 2);
        rows[i] = {first + i * width, seed.open + i, seed.high + i, seed.low + i,
                   seed.close + i, has_volume ? seed.volume + i : 0, has_volume};
      }
      if (series.UpsertCandles(seed.interval, "usd", seed.source_id, rows,
            static_cast<size_t>(seed.count), now_ms) != fortis::prices::PriceSeriesStoreStatus::kStored) {
        fprintf(stderr, "Failed to seed test history other-ID fixture candles\n"); return false;
      }
    }
    // PriceSeriesStore's open-row key layout (price_series_store.cpp OpenKey).
    char corrupt_key[160];
    snprintf(corrupt_key, sizeof(corrupt_key),
             "price/open/v1/1h/usd/spl:CorruptMint11111111111111111111111111111/%019llu",
             static_cast<unsigned long long>(first));
    const char garbage[] = "not a candle";
    fortis::storage::KvMutation corrupt{corrupt_key, garbage, sizeof(garbage),
                               now_ms + 86400000LL, false};
    if (!kv->Write(&corrupt, 1)) {
      fprintf(stderr, "Failed to seed test history corrupt row\n"); return false;
    }
  }
  const char* seed_ohlc = TestHookEnv("FORTIS_TEST_OHLC_SEED");
  if (seed_ohlc && strcmp(seed_ohlc, "1") == 0) {
    constexpr int64_t ohlc_now = 1767323045000LL;
    const char* source_id = "spl:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    fortis::prices::PriceSeriesCandleInput ohlc_hourly[] = {
      {1767319200000LL, 100, 108, 99, 106, 12.5, true},
      {1759665600000LL, 70, 75, 68, 74, 4.25, true},
    };
    fortis::prices::PriceSeriesCandleInput ohlc_daily[] = {
      {1766448000000LL, 200, 215, 195, 210, 300, true},
      {1750032000000LL, 80, 90, 78, 88, 70, true},
    };
    if (series.UpsertCandles(fortis::prices::PriceSeriesInterval::k1h, "usd",
          source_id, ohlc_hourly, 2, ohlc_now) != fortis::prices::PriceSeriesStoreStatus::kStored ||
        series.UpsertCandles(fortis::prices::PriceSeriesInterval::k1d, "usd",
          source_id, ohlc_daily, 2, ohlc_now) != fortis::prices::PriceSeriesStoreStatus::kStored) {
      fprintf(stderr, "Failed to seed test OHLC route fixture candles\n"); return false;
    }
  }
  // The R33 OHLC fixture's largest stored windows: 2,161 hourly candles
  // (FIRST … FIRST + 90 days) and 366 daily candles (FIRST … FIRST + 365
  // days), as the recorded fixture seeds them.
  const char* seed_ohlc_query = TestHookEnv("FORTIS_TEST_OHLC_QUERY_SEED");
  if (seed_ohlc_query && strcmp(seed_ohlc_query, "1") == 0) {
    // The store refuses candles after `now`; seed as of the last daily one.
    const int64_t seed_now = first + 365 * 86400000LL;
    const char* source_id = "spl:BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
    const struct { fortis::prices::PriceSeriesInterval interval; int64_t width; int count; } sets[] = {
      {fortis::prices::PriceSeriesInterval::k1h, 3600000LL, 90 * 24 + 1},
      {fortis::prices::PriceSeriesInterval::k1d, 86400000LL, 366},
    };
    for (const auto& set : sets) {
      for (int offset = 0; offset < set.count; offset += 200) {
        const int rows_count = set.count - offset < 200 ? set.count - offset : 200;
        fortis::prices::PriceSeriesCandleInput rows[200]{};
        for (int j = 0; j < rows_count; ++j) {
          const int i = offset + j;
          rows[j] = {first + i * set.width, 10 + i / 4.0, 11 + i / 4.0, 9 + i / 4.0,
                     10.5 + i / 4.0, static_cast<double>(i), true};
        }
        if (series.UpsertCandles(set.interval, "usd", source_id, rows,
              static_cast<size_t>(rows_count), seed_now) != fortis::prices::PriceSeriesStoreStatus::kStored) {
          fprintf(stderr, "Failed to seed test OHLC query fixture candles\n"); return false;
        }
      }
    }
  }
  const char* fixture_token = "fintel_dev_HHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHH";
  const char* fixture_id = "01ARZ3NDEKTSV4RRFFQ69G5FAW";
  fortis::auth::DeviceAuthStore auth(kv);
  timeval auth_now{}; gettimeofday(&auth_now, nullptr);
  const int64_t auth_ms = static_cast<int64_t>(auth_now.tv_sec) * 1000 + auth_now.tv_usec / 1000;
  if (!auth.SeedPriceRouteFixture(fixture_token, fixture_id, auth_ms)) {
    fprintf(stderr, "Failed to seed test history route fixture auth\n"); return false;
  }
  return true;
}
#endif


}  // namespace

#ifdef FORTIS_HISTORY_ROUTE_TEST_ONLY
namespace {
bool history_test_route = false;
// Static: the service holds the seams until its destructor, which runs
// after main's later locals are gone.
HistoryTestUpstream history_test_upstream{};
const prices::PriceHistoryRouteSeams history_test_seams{
    &history_test_ohlc_clock, &history_test_ohlc_jitter_source, history_test_fail_source, nullptr};
HistoryTestUpstream loopback_history_upstream{};
}  // namespace

bool ConfigureHistoryTest(const TestPlaneConfig& config, PlaneOptions& options) {
  const char* history_route_flag = TestHookEnv("FORTIS_ENABLE_TEST_PRICE_HISTORY_ROUTE");
  history_test_route = history_route_flag && strcmp(history_route_flag, "1") == 0;
  const char* history_upstream_port = TestHookEnv("FORTIS_TEST_HISTORY_UPSTREAM_PORT");
  const char* history_upstream_ca = TestHookEnv("FORTIS_TEST_HISTORY_UPSTREAM_CA");
  if (history_test_route && config.native_price_route_canary) {
    fprintf(stderr, "FORTIS_ENABLE_TEST_PRICE_HISTORY_ROUTE and "
                    "FORTIS_NATIVE_PRICE_ROUTE_CANARY are mutually exclusive\n");
    return false;
  }
  if (history_test_route && (parse_port(history_upstream_port) < 0 ||
      !history_upstream_ca || !*history_upstream_ca ||
      !http::OutboundHttpsValidateCaBundle(history_upstream_ca))) {
    fprintf(stderr, "Invalid test-only price history upstream configuration\n");
    return false;
  }
  if (history_test_route) {
    snprintf(history_test_upstream.port, sizeof(history_test_upstream.port), "%d",
             parse_port(history_upstream_port));
    history_test_upstream.ca = history_upstream_ca;
    history_test_upstream.fail = TestHookEnv("FORTIS_TEST_HISTORY_OWNER_START_FAIL");
    options.history_owners = &history_test_upstream;
    options.history_seams = &history_test_seams;
    options.history = true;
  }
  return true;
}

void UseLoopbackHistoryOwners(int port, const char* ca, bool test_seams, PlaneOptions& options) {
  if (history_test_route) return;
  snprintf(loopback_history_upstream.port, sizeof(loopback_history_upstream.port), "%d", port);
  loopback_history_upstream.ca = ca;
  options.history_owners = &loopback_history_upstream;
  if (test_seams) options.history_seams = &history_test_seams;
  options.history = true;
}

bool SeedHistoryTest(fortis::storage::KvStore* kv) {
  return !history_test_route || seed_history_test_fixtures(kv);
}
#endif

#ifdef FORTIS_PRICE_ROUTE_TEST_ONLY
namespace {
bool price_test_route = false;
PriceTestUpstream price_test_upstream{};
}  // namespace

bool ConfigurePriceTest(const TestPlaneConfig& config, PlaneOptions& options) {
  const char* price_route_flag = TestHookEnv("FORTIS_ENABLE_TEST_PRICE_ROUTE");
  price_test_route = price_route_flag && strcmp(price_route_flag, "1") == 0;
  const char* price_upstream_port = TestHookEnv("FORTIS_TEST_PRICE_UPSTREAM_PORT");
  const char* price_upstream_ca = TestHookEnv("FORTIS_TEST_PRICE_UPSTREAM_CA");
  if (price_test_route && config.native_price_route_canary) {
    fprintf(stderr, "FORTIS_ENABLE_TEST_PRICE_ROUTE and "
                    "FORTIS_NATIVE_PRICE_ROUTE_CANARY are mutually exclusive\n");
    return false;
  }
  if (price_test_route && (parse_port(price_upstream_port) < 0 || !price_upstream_ca ||
      !*price_upstream_ca || !http::OutboundHttpsValidateCaBundle(price_upstream_ca))) {
    fprintf(stderr, "Invalid test-only price route upstream configuration\n");
    return false;
  }
  if (price_test_route) {
    snprintf(price_test_upstream.port, sizeof(price_test_upstream.port), "%d",
             parse_port(price_upstream_port));
    price_test_upstream.ca = price_upstream_ca;
    options.price_owner = &price_test_upstream;
    options.price = true;
    // Loopback fixture servers do not share CoinGecko's anonymous quota.
    options.coingecko_loopback = true;
  }
  return true;
}

bool SeedPriceTest(fortis::storage::KvStore* kv) {
  if (price_test_route) {
    const char* fixture_token = "fintel_dev_PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP";
    const char* fixture_id = "01ARZ3NDEKTSV4RRFFQ69G5FAV";
    fortis::auth::DeviceAuthStore auth(kv);
    if (!auth.SeedPriceRouteFixture(fixture_token, fixture_id, price_test_now_ms())) {
      fprintf(stderr, "Failed to seed test price route fixture auth\n");
      return false;
    }
  }
  return true;
}

bool RegisterPriceTestRoutes(http::HttpRouter& router, Planes& planes) {
  return !price_test_route ||
         router.Add("GET", "/probe/test-price-owner", handle_price_owner_active, planes.price ? planes.price->route_context() : nullptr);
}
#endif

#ifdef FORTIS_DISCOVERY_ROUTE_TEST_ONLY
namespace {
bool discovery_test_route = false;
DiscoveryTestUpstream discovery_test_upstream{};
const discovery::DiscoveryCacheSeams discovery_test_seams{&discovery_test_clock, &discovery_test_jitter_source};
}  // namespace

bool ConfigureDiscoveryTest(const TestPlaneConfig& config, PlaneOptions& options) {
  const char* discovery_flag = TestHookEnv("FORTIS_ENABLE_TEST_DISCOVERY_ROUTES");
  discovery_test_route = discovery_flag && strcmp(discovery_flag, "1") == 0;
  const char* discovery_upstream_port = TestHookEnv("FORTIS_TEST_DISCOVERY_UPSTREAM_PORT");
  const char* discovery_upstream_ca = TestHookEnv("FORTIS_TEST_DISCOVERY_UPSTREAM_CA");
  if (discovery_test_route && config.native_price_route_canary) {
    fprintf(stderr, "FORTIS_ENABLE_TEST_DISCOVERY_ROUTES and "
                    "FORTIS_NATIVE_PRICE_ROUTE_CANARY are mutually exclusive\n");
    return false;
  }
  // The comparison replay needs no upstream; a discovery replay names one.
  const bool discovery_test_upstream_set = discovery_upstream_port && *discovery_upstream_port;
  if (discovery_test_route && discovery_test_upstream_set &&
      (parse_port(discovery_upstream_port) < 0 || !discovery_upstream_ca ||
       !*discovery_upstream_ca || !http::OutboundHttpsValidateCaBundle(discovery_upstream_ca))) {
    fprintf(stderr, "Invalid test-only discovery upstream configuration\n");
    return false;
  }
  if (discovery_test_route) {
    if (discovery_test_upstream_set) {
      snprintf(discovery_test_upstream.port, sizeof(discovery_test_upstream.port), "%d",
               parse_port(discovery_upstream_port));
      discovery_test_upstream.ca = discovery_upstream_ca;
    } else {
      // No reachable upstream: every provider request fails at connect.
      snprintf(discovery_test_upstream.port, sizeof(discovery_test_upstream.port), "1");
      // The owner never connects in such a replay; any CA path starts it.
      const char* fallback_ca = TestHookEnv("FORTIS_TEST_DISCOVERY_FALLBACK_CA");
      discovery_test_upstream.ca = fallback_ca && *fallback_ca ? fallback_ca : "/dev/null";
    }
    options.discovery_owner = &discovery_test_upstream;
    options.discovery_seams = &discovery_test_seams;
    options.comparison_clock = &discovery_test_clock;
    options.popularity_clock = &discovery_test_clock;
    options.discovery = true;
    // Match the replay's normal provider budget, as ConfigurePriceTest does.
    // This validated loopback transport does not share anonymous egress quota.
    if (discovery_test_upstream_set) options.coingecko_loopback = true;
  }
  return true;
}

bool SeedDiscoveryTest(fortis::storage::KvStore* kv) {
  if (discovery_test_route) {
    const char* seed = TestHookEnv("FORTIS_TEST_COMPARISON_SEED");
    if (!seed_discovery_test_devices(kv) ||
        (seed && *seed && !LoadTokenComparisonSeedFile(*kv, seed))) {
      fprintf(stderr, "Failed to seed test discovery fixtures\n");
      return false;
    }
  }
  return true;
}

bool RegisterDiscoveryTestRoutes(http::HttpRouter& router) {
  return !discovery_test_route ||
         router.Add("POST", "/probe/test-discovery-state", handle_discovery_test_state,
                    const_cast<discovery::DiscoveryCacheSeams*>(&discovery_test_seams));
}

void DiscoveryTestSweepClock(fortis::base::Clock** clock) {
  if (discovery_test_route) *clock = &discovery_test_sweep_clock;
}
#endif

#if defined(FORTIS_TOKEN_ROUTE_TEST_ONLY) || defined(FORTIS_DISCOVERY_ROUTE_TEST_ONLY)
bool RegisterCoinPopularityTestRoute(http::HttpRouter& router, Planes& planes) {
  return !planes.coin_popularity.get() ||
         router.Add("GET", "/probe/test-coin-popularity", handle_coin_popularity_test,
                    planes.coin_popularity.get());
}
#endif

}  // namespace fortis::app
