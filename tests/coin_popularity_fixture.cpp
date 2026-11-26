// Native replay driver for the frozen TypeScript coin popularity contract
// (tests/corpus/coin_popularity.json). tests/coin_popularity_test.py
// writes one command per line on stdin and reads one JSON line per command.
// Ids and device ids travel hex-encoded ("~" is the empty id, "-" no device);
// floats as shortest round-trip text.
//
//   domain:   decayed V FROM TO | record_request USERS|- HITS UPDATED LAST NOW NEW
//             score USERS HITS UPDATED LAST NOW | expired ... NOW | bucket RANK
//             interval BUCKET
//   tracker:  new MAX|- SEEN|- | record AT DEV IDS... | repeat AT DEV TIMES ID
//             due AT BUDGET | mark AT IDS... | dirty AT | states AT | size AT
//             prune AT MAX|- | restore AT ID:USERS:HITS:UPDATED:LAST ...
//   runs:     open DIR PORT CA (KV, coordinator, token owner on the loopback
//             CoinGecko) | close | run AT BATCH CALLS [STOP_AFTER] | run_busy AT
//             inspect AT ID | persisted AT | physical | cold_restore AT
//             cprune AT | due_ids AT | stats | save AT | kv_close | kv_open DIR
//   load:     measure DIR COINS DEVICES — 20,000 coins and the capped daily
//             pair set: timings, bytes and frames written, peak RSS.
#include "coin_popularity.h"
#include "coin_price_scheduler.h"
#include "discovery_cache.h"
#include "fake_seams.h"
#include "kv_store.h"
#include "native_config.h"
#include "price_series_store.h"
#include "price_spot_cache.h"
#include "provider_execution_coordinator.h"
#include "token_provider.h"

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>

using namespace fortis::config;
using namespace fortis::discovery;
using namespace fortis::http;
using namespace fortis::providers;

namespace {

using fortis::discovery::CoinIdView;
using fortis::discovery::CoinPopularity;
using fortis::discovery::CoinPopularityState;
namespace cp = fortis::discovery::coin_popularity;

int64_t g_now = 0;
fortis::testing::PointerClock g_clock(&g_now);
fortis::testing::FixedJitter g_jitter;
bool allow_loopback(void*, uint32_t address) { return address == 0x7f000001U; }

size_t g_stop_checks = 0, g_stop_after = SIZE_MAX;
bool stop_after(void*) { return ++g_stop_checks > g_stop_after; }

uint64_t monotonic_us() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000000ULL + static_cast<uint64_t>(now.tv_nsec / 1000);
}

long long peak_rss_bytes() {
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
  return static_cast<long long>(usage.ru_maxrss);
#else
  return static_cast<long long>(usage.ru_maxrss) * 1024;
#endif
}

long long file_bytes(const char* directory) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/fortis.kv", directory);
  struct stat info{};
  return stat(path, &info) == 0 ? static_cast<long long>(info.st_size) : -1;
}

// ---- text helpers ----------------------------------------------------------------

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Decodes one hex token into `out` (NUL-terminated); "~" is the empty id.
size_t unhex(const char* token, char* out, size_t capacity) {
  if (strcmp(token, "~") == 0) { out[0] = '\0'; return 0; }
  const size_t n = strlen(token) / 2;
  assert(n < capacity);
  for (size_t i = 0; i < n; ++i) out[i] = static_cast<char>(hex_digit(token[2 * i]) << 4 | hex_digit(token[2 * i + 1]));
  out[n] = '\0';
  return n;
}

void print_hex(const char* text, size_t length) {
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    putchar(kHex[c >> 4]);
    putchar(kHex[c & 15]);
  }
}

void print_number(double value) {
  // Shortest round-trip text: %.17g always round-trips; Python parses it.
  if (value == floor(value) && fabs(value) < 9e15) printf("%lld", static_cast<long long>(value));
  else printf("%.17g", value);
}

void print_state(const CoinPopularityState& state) {
  printf("\"users\":"); print_number(state.users);
  printf(",\"hits\":"); print_number(state.hits);
  printf(",\"updatedAt\":%lld,\"lastRequestedAt\":%lld", static_cast<long long>(state.updated_at_ms),
         static_cast<long long>(state.last_requested_at_ms));
}

void print_rows(const fortis::discovery::PopularityRowList& rows, bool refresh) {
  putchar('[');
  for (size_t i = 0; i < rows.count; ++i) {
    const fortis::discovery::PopularityRow& row = rows.items[i];
    printf("%s{\"idHex\":\"", i ? "," : "");
    print_hex(row.id, row.id_length);
    printf("\",");
    print_state(row.state);
    if (refresh) {
      if (row.has_refreshed_at) printf(",\"refreshedAt\":%lld", static_cast<long long>(row.refreshed_at_ms));
      else printf(",\"refreshedAt\":null");
    }
    putchar('}');
  }
  putchar(']');
}

CoinPopularityState parse_state(char** fields) {
  CoinPopularityState state;
  state.users = strtod(fields[0], nullptr);
  state.hits = strtod(fields[1], nullptr);
  state.updated_at_ms = strtoll(fields[2], nullptr, 10);
  state.last_requested_at_ms = strtoll(fields[3], nullptr, 10);
  return state;
}

// ---- the run composition -------------------------------------------------------

struct Composition {
  fortis::storage::KvStore kv;
  ProviderExecutionCoordinator* coordinator = nullptr;
  fortis::prices::PriceSeriesStore* series = nullptr;
  TokenProviderOwner* owner = nullptr;
  NativeConfig config{};
  char port[8]{};
  bool open = false;
};
Composition g_run;
CoinPopularity* g_popularity = nullptr;

void new_tracker(size_t max_coins, size_t max_seen) {
  CoinPopularity::Destroy(g_popularity);
  fortis::discovery::CoinPopularityOptions options;
  if (max_coins) options.max_coins = max_coins;
  if (max_seen) options.max_seen_per_day = max_seen;
  options.clock = &g_clock;
  g_popularity = CoinPopularity::Create(options).ValueOr(nullptr).release();
  assert(g_popularity);
}

void open_run(const char* directory, const char* port, const char* ca) {
  assert(!g_run.open);
  assert(g_run.kv.Open(directory));
  g_run.coordinator = new ProviderExecutionCoordinator();
  assert(g_run.coordinator->Start());
  g_run.series = new fortis::prices::PriceSeriesStore(g_run.kv);
  g_run.config = NativeConfig{};
  g_run.config.ca_bundle_configured = true;
  snprintf(g_run.config.ca_bundle_path, sizeof(g_run.config.ca_bundle_path), "%s", ca);
  snprintf(g_run.port, sizeof(g_run.port), "%s", port);
  g_run.owner = new TokenProviderOwner(g_run.config, g_run.coordinator);
  // The replays list a failing endpoint first for some networks.
  const char* endpoints = getenv("FORTIS_TEST_TOKEN_NETWORK_ENDPOINTS");
  if (endpoints && *endpoints) assert(g_run.owner->SetNetworkEndpoints(endpoints));
  assert(g_run.owner->StartRedirected("localhost", port, ca, allow_loopback, nullptr,
                                      "x-fortis-test-host"));
  g_run.open = true;
}

void close_run() {
  if (!g_run.open) return;
  g_run.owner->Stop();
  delete g_run.owner;
  assert(g_run.coordinator->Stop());
  delete g_run.coordinator;
  delete g_run.series;
  g_run.kv.Close();
  g_run.open = false;
}

void print_run(const fortis::prices::coin_prices::RunResult& r) {
  printf("{\"due\":%zu,\"deferred\":%zu,\"calls\":%zu,\"failedCalls\":%zu,\"laneRejected\":%zu,"
         "\"refreshed\":%zu,\"listed\":%zu,\"quotes\":%zu,\"marketWriteFailures\":%zu,"
         "\"spotWriteFailures\":%zu,\"saved\":%zu,\"saveFrames\":%zu,\"saveBytes\":%zu,"
         "\"aborted\":%s,\"stopped\":%s,\"failed\":%s,\"elapsedUs\":%llu}",
         r.due, r.deferred, r.calls, r.failed_calls, r.lane_rejected, r.refreshed, r.listed, r.quotes,
         r.market_write_failures, r.spot_write_failures, r.saved, r.save_frames, r.save_bytes,
         r.aborted ? "true" : "false", r.stopped ? "true" : "false", r.failed ? "true" : "false",
         static_cast<unsigned long long>(r.elapsed_us));
}

fortis::prices::coin_prices::RunResult run_once(size_t batch, size_t calls) {
  fortis::prices::coin_prices::Deps deps;
  deps.kv = &g_run.kv;
  deps.series = g_run.series;
  deps.owner = g_run.owner;
  deps.popularity = g_popularity;
  deps.clock = &g_clock;
  deps.jitter = &g_jitter;
  deps.stop = stop_after;
  deps.pinned_core = getenv("FORTIS_TEST_PINNED_PRICE_CORE") != nullptr;
  deps.snapshot_owns_core_quotes = getenv("FORTIS_TEST_SNAPSHOT_OWNS_CORE") != nullptr;
  deps.batch_ids = batch;
  deps.max_calls = calls;
  deps.deadline_ms = TokenMonotonicMs() + 60000;
  return fortis::prices::coin_prices::Run(deps);
}

fortis::prices::coin_prices::RunResult run_with_lane_busy() {
  auto& coordinator = *g_run.coordinator;
  const auto owner = coordinator.RegisterOwner([](void*) {}, nullptr);
  assert(owner);
  OutboundRequest request{};
  request.host = "localhost";
  request.port = g_run.port;
  request.path = "/hold-lane";
  request.ca_file = g_run.config.ca_bundle_path;
  request.allow_address = allow_loopback;
  request.max_response_bytes = 4096;
  ProviderExecutionOptions options{};
  options.deadline_ms = TokenMonotonicMs() + 5000;
  const auto held = coordinator.Submit(owner, "coingecko", request, nullptr, options);
  assert(held.status == ProviderExecutionSubmitStatus::kAccepted);
  const auto result = run_once(6, 2);
  coordinator.Cancel(owner, held.job_id);
  ProviderExecutionResult ignored{};
  unsigned char body[4096];
  while (coordinator.TryTakeResult(owner, &ignored, body, sizeof(body)) ==
         ProviderExecutionTakeStatus::kNone) {
    const timespec pause{0, 1000000};
    nanosleep(&pause, nullptr);
  }
  assert(coordinator.UnregisterOwner(owner));
  return result;
}

void inspect(const char* id, size_t id_length) {
  printf("{\"idHex\":\"");
  print_hex(id, id_length);
  printf("\",\"market\":");
  unsigned char scratch[96 * 1024];
  const DiscoveryCacheSeams seams{&g_clock, &g_jitter};
  DiscoveryCache cache(&g_run.kv, scratch, sizeof(scratch), &seams);
  char key[4096];
  snprintf(key, sizeof(key), "mkt:usd:%s", id);
  const DiscoveryCacheEntry entry = cache.Read(key, g_now);
  if (entry.state == DiscoveryCacheState::kFresh || entry.state == DiscoveryCacheState::kStale) {
    printf("{\"d\":");
    fwrite(entry.body, 1, entry.body_length, stdout);
    printf(",\"asOf\":%lld,\"freshMs\":%lld,\"staleMs\":%lld}", static_cast<long long>(entry.as_of_ms),
           static_cast<long long>(entry.fresh_until_ms - entry.as_of_ms),
           static_cast<long long>(entry.stale_until_ms - entry.fresh_until_ms));
  } else {
    printf("null");
  }
  fortis::prices::PriceSpotCache spot(g_run.kv);
  printf(",\"spot\":");
  const fortis::prices::PriceSpotLookup lookup = spot.Lookup("usd", id, g_now);
  if (lookup.state == fortis::prices::PriceSpotLookupState::kFresh || lookup.state == fortis::prices::PriceSpotLookupState::kStale) {
    printf("{\"price\":"); print_number(lookup.value.price);
    printf(",\"change24hPct\":");
    if (lookup.value.has_change24h_pct) print_number(lookup.value.change24h_pct); else printf("null");
    printf(",\"source\":\"%s\",\"asOf\":%lld,\"freshMs\":%lld,\"staleMs\":%lld}", lookup.value.provider,
           static_cast<long long>(lookup.value.as_of_ms),
           static_cast<long long>(lookup.value.fresh_until_ms - lookup.value.as_of_ms),
           static_cast<long long>(lookup.value.stale_until_ms - lookup.value.fresh_until_ms));
  } else {
    printf("null");
  }
  const fortis::prices::PriceSpotLookup eur = spot.Lookup("eur", id, g_now);
  printf(",\"eurSpot\":%s", eur.state == fortis::prices::PriceSpotLookupState::kFresh ||
                                    eur.state == fortis::prices::PriceSpotLookupState::kStale ? "true" : "false");
  fortis::prices::PriceSeriesSpot series;
  printf(",\"series\":");
  if (g_run.series->GetSpot("usd", id, g_now, &series) == fortis::prices::PriceSeriesReadStatus::kFound) {
    printf("{\"price\":"); print_number(series.price);
    printf(",\"change24hPct\":");
    if (series.has_change24h_pct) print_number(series.change24h_pct); else printf("null");
    printf(",\"volume24h\":");
    if (series.has_volume24h) print_number(series.volume24h); else printf("null");
    printf(",\"source\":\"%s\",\"asOf\":%lld}", series.source, static_cast<long long>(series.as_of_ms));
  } else {
    printf("null");
  }
  printf("}");
}

struct PersistedRow { char key[1100]; CoinPopularityState state; };
struct PersistedScan { PersistedRow* rows; size_t count, capacity; };

bool persisted_visit(const char* key, const void* value, size_t size, PersistedScan* scan) {
  CoinPopularityState state;
  if (scan->count == scan->capacity || !fortis::discovery::coin_popularity_store::Decode(value, size, &state)) return true;
  snprintf(scan->rows[scan->count].key, sizeof(scan->rows[0].key), "%s", key);
  scan->rows[scan->count++].state = state;
  return true;
}

// load(at) and each row's expiry boundary (visible at last + 7 d, gone 1 ms later).
void persisted(int64_t at) {
  CoinPopularity* scratch = nullptr;
  fortis::discovery::CoinPopularityOptions options;
  options.clock = &g_clock;
  options.max_coins = 100000;
  scratch = CoinPopularity::Create(options).ValueOr(nullptr).release();
  const int64_t saved = g_now;
  g_now = at;
  const size_t loaded = fortis::discovery::coin_popularity_store::LoadInto(g_run.kv, at, *scratch);
  fortis::discovery::PopularityRowList rows;
  assert(scratch->Snapshot(&rows));
  printf("{\"loaded\":%zu,\"rows\":[", loaded);
  for (size_t i = 0; i < rows.count; ++i) {
    const fortis::discovery::PopularityRow& row = rows.items[i];
    char key[1100];
    assert(fortis::discovery::coin_popularity_store::Key(row.id, row.id_length, key, sizeof(key)));
    uint8_t value[64];
    size_t size = 0;
    const int64_t boundary = row.state.last_requested_at_ms + fortis::discovery::kCoinPopularityExpireAfterMs;
    const bool at_expiry = g_run.kv.Get(key, boundary, value, sizeof(value), &size);
    const bool after_expiry = g_run.kv.Get(key, boundary + 1, value, sizeof(value), &size);
    printf("%s{\"idHex\":\"", i ? "," : "");
    print_hex(row.id, row.id_length);
    printf("\",");
    print_state(row.state);
    printf(",\"visibleAtExpiry\":%s,\"visibleAfterExpiry\":%s}", at_expiry ? "true" : "false",
           after_expiry ? "true" : "false");
  }
  printf("]}");
  rows.Release();
  CoinPopularity::Destroy(scratch);
  g_now = saved;
}

size_t physical_rows() {
  PersistedScan scan{static_cast<PersistedRow*>(malloc(sizeof(PersistedRow) * 4096)), 0, 4096};
  fortis::storage::KvPageResult page;
  assert(g_run.kv.Page(fortis::discovery::coin_popularity_store::kPrefix, nullptr, 4096, 0, false, [&, visitor_state = &scan](const char* key, const void* value, size_t size) { return (persisted_visit)(key, value, size, visitor_state); }, &page));
  free(scan.rows);
  return scan.count;
}

// ---- load measurement -------------------------------------------------------

void measure(const char* directory, size_t coins, size_t devices) {
  new_tracker(0, 0);
  const long long rss_before = peak_rss_bytes();
  g_now = 1767323045000LL;  // the fixture instant
  // Every coin asked by `devices` devices (coins · devices pairs, capped).
  char ids[100][32];
  CoinIdView views[100];
  char device[32];
  uint64_t started = monotonic_us();
  size_t records = 0;
  for (size_t d = 0; d < devices; ++d) {
    snprintf(device, sizeof(device), "01ARZ3NDEKTSV4RRFFQ%07zu", d);
    for (size_t first = 0; first < coins; first += 100) {
      size_t n = 0;
      for (size_t c = first; c < coins && n < 100; ++c, ++n) {
        snprintf(ids[n], sizeof(ids[n]), "coin-%06zu", c);
        views[n] = CoinIdView{ids[n], strlen(ids[n])};
      }
      g_popularity->Record(views, n, device);
      ++records;
    }
  }
  const uint64_t record_us = monotonic_us() - started;
  fortis::discovery::CoinPopularityStats stats = g_popularity->Stats();
  // One minute's ranking at the cap.
  started = monotonic_us();
  fortis::discovery::RankedCoinList due;
  assert(g_popularity->Due(fortis::prices::coin_prices::kBudgetPerRun, &due));
  const uint64_t due_us = monotonic_us() - started;
  const size_t due_count = due.count;
  due.Release();
  // Persist every dirty counter (the first run after the flood).
  fortis::storage::KvStore kv;
  assert(kv.Open(directory));
  const long long bytes_before = file_bytes(directory);
  started = monotonic_us();
  fortis::discovery::PopularityRowList dirty;
  assert(g_popularity->TakeDirty(&dirty));
  const fortis::discovery::coin_popularity_store::SaveResult saved =
      fortis::discovery::coin_popularity_store::Save(kv, dirty.items.get(), dirty.count);
  const uint64_t save_us = monotonic_us() - started;
  dirty.Release();
  const long long bytes_after = file_bytes(directory);
  // A second save with nothing dirty writes nothing (idle minute).
  assert(g_popularity->TakeDirty(&dirty));
  const size_t idle_dirty = dirty.count;
  dirty.Release();
  const long long bytes_idle = file_bytes(directory);
  // Restore into a fresh tracker (boot).
  fortis::discovery::CoinPopularityOptions options;
  options.clock = &g_clock;
  CoinPopularity* restored = CoinPopularity::Create(options).ValueOr(nullptr).release();
  started = monotonic_us();
  const size_t loaded = fortis::discovery::coin_popularity_store::LoadInto(kv, g_now, *restored);
  const uint64_t restore_us = monotonic_us() - started;
  const fortis::discovery::CoinPopularityStats restored_stats = restored->Stats();
  CoinPopularity::Destroy(restored);
  // Daily prune at the cap (nothing expired) and eight days later.
  started = monotonic_us();
  fortis::prices::coin_prices::PruneResult prune = fortis::prices::coin_prices::Prune(kv, *g_popularity, g_now);
  const uint64_t prune_us = monotonic_us() - started;
  g_now += 8LL * 86400000;
  started = monotonic_us();
  fortis::prices::coin_prices::PruneResult expired = fortis::prices::coin_prices::Prune(kv, *g_popularity, g_now);
  const uint64_t expired_us = monotonic_us() - started;
  const fortis::discovery::CoinPopularityStats after = g_popularity->Stats();
  kv.Close();
  printf("{\"coins\":%zu,\"devices\":%zu,\"records\":%zu,\"recordUs\":%llu,\"tracked\":%zu,"
         "\"seenPairs\":%zu,\"dropped\":%zu,\"allocatedBytes\":%zu,\"peakAllocatedBytes\":%zu,"
         "\"rowCapacity\":%zu,\"idBytes\":%zu,\"indexSlots\":%zu,"
         "\"dueCount\":%zu,\"dueUs\":%llu,\"saved\":%zu,\"saveFrames\":%zu,\"saveBytes\":%zu,"
         "\"saveUs\":%llu,\"kvFileBytes\":%lld,\"idleDirty\":%zu,\"idleFileDelta\":%lld,"
         "\"restored\":%zu,\"restoreUs\":%llu,\"restoredAllocatedBytes\":%zu,"
         "\"pruneBefore\":%zu,\"pruneAfter\":%zu,\"pruneUs\":%llu,"
         "\"expiredAfter\":%zu,\"expiredSwept\":%zu,\"expiredUs\":%llu,\"afterAllocatedBytes\":%zu,"
         "\"seenAllocatedAfterExpiry\":%s,\"rssBeforeBytes\":%lld,\"peakRssBytes\":%lld}\n",
         coins, devices, records, static_cast<unsigned long long>(record_us), stats.coins, stats.seen_pairs,
         stats.dropped, stats.allocated_bytes, stats.peak_allocated_bytes, stats.row_capacity,
         stats.id_bytes, stats.index_slots, due_count, static_cast<unsigned long long>(due_us), saved.rows,
         saved.frames, saved.bytes, static_cast<unsigned long long>(save_us), bytes_after - bytes_before,
         idle_dirty, bytes_idle - bytes_after, loaded, static_cast<unsigned long long>(restore_us),
         restored_stats.allocated_bytes, prune.before, prune.after, static_cast<unsigned long long>(prune_us),
         expired.after, expired.swept, static_cast<unsigned long long>(expired_us), after.allocated_bytes,
         after.seen_allocated ? "true" : "false", rss_before, peak_rss_bytes());
}

}  // namespace

int main() {
  static char line[4 * 1024 * 1024];
  static char* fields[300000];
  static char id_buffer[2048];
  new_tracker(0, 0);
  while (fgets(line, sizeof(line), stdin)) {
    size_t length = strlen(line);
    while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = '\0';
    size_t count = 0;
    for (char* token = strtok(line, " "); token && count < 300000; token = strtok(nullptr, " "))
      fields[count++] = token;
    if (!count) continue;
    const char* command = fields[0];
    if (strcmp(command, "decayed") == 0) {
      printf("{\"result\":");
      print_number(cp::Decayed(strtod(fields[1], nullptr), strtoll(fields[2], nullptr, 10), strtoll(fields[3], nullptr, 10)));
      printf("}\n");
    } else if (strcmp(command, "record_request") == 0) {
      CoinPopularityState state;
      const bool has_state = strcmp(fields[1], "-") != 0;
      if (has_state) state = parse_state(fields + 1);
      const CoinPopularityState out = cp::RecordRequest(has_state ? &state : nullptr,
          strtoll(fields[has_state ? 5 : 2], nullptr, 10), strcmp(fields[has_state ? 6 : 3], "1") == 0);
      printf("{");
      print_state(out);
      printf("}\n");
    } else if (strcmp(command, "score") == 0) {
      const CoinPopularityState state = parse_state(fields + 1);
      printf("{\"result\":");
      print_number(cp::Score(state, strtoll(fields[5], nullptr, 10)));
      printf("}\n");
    } else if (strcmp(command, "expired") == 0) {
      const CoinPopularityState state = parse_state(fields + 1);
      printf("{\"result\":%s}\n", cp::IsExpired(state, strtoll(fields[5], nullptr, 10)) ? "true" : "false");
    } else if (strcmp(command, "bucket") == 0) {
      printf("{\"result\":%u}\n", cp::BucketForRank(strtoll(fields[1], nullptr, 10)));
    } else if (strcmp(command, "interval") == 0) {
      printf("{\"result\":%lld}\n", static_cast<long long>(cp::IntervalForBucket(strtoll(fields[1], nullptr, 10))));
    } else if (strcmp(command, "new") == 0) {
      new_tracker(strcmp(fields[1], "-") ? strtoull(fields[1], nullptr, 10) : 0,
                  strcmp(fields[2], "-") ? strtoull(fields[2], nullptr, 10) : 0);
      printf("null\n");
    } else if (strcmp(command, "record") == 0 || strcmp(command, "repeat") == 0) {
      g_now = strtoll(fields[1], nullptr, 10);
      char device[256];
      const bool has_device = strcmp(fields[2], "-") != 0;
      if (has_device) unhex(fields[2], device, sizeof(device));
      const bool repeat = command[1] == 'e' && command[2] == 'p';
      static char texts[100][1100];
      CoinIdView views[100];
      size_t n = 0, times = 1;
      size_t first = 3;
      if (repeat) { times = strtoull(fields[3], nullptr, 10); first = 4; }
      for (size_t i = first; i < count && n < 100; ++i, ++n) {
        const size_t id_length = unhex(fields[i], texts[n], sizeof(texts[n]));
        views[n] = CoinIdView{texts[n], id_length};
      }
      for (size_t t = 0; t < times; ++t) g_popularity->Record(views, n, has_device ? device : nullptr);
      printf("null\n");
    } else if (strcmp(command, "due") == 0 || strcmp(command, "due_ids") == 0) {
      g_now = strtoll(fields[1], nullptr, 10);
      const bool ids_only = command[3] == '_';
      fortis::discovery::RankedCoinList due;
      assert(g_popularity->Due(ids_only ? fortis::prices::coin_prices::kBudgetPerRun : strtoull(fields[2], nullptr, 10), &due));
      putchar('[');
      for (size_t i = 0; i < due.count; ++i) {
        const fortis::discovery::RankedCoin& coin = due.items[i];
        printf("%s", i ? "," : "");
        if (ids_only) {
          putchar('"'); print_hex(coin.id, coin.id_length); putchar('"');
        } else {
          printf("{\"idHex\":\"");
          print_hex(coin.id, coin.id_length);
          printf("\",\"rank\":%zu,\"bucket\":%u,\"intervalMs\":%lld}", coin.rank, coin.bucket,
                 static_cast<long long>(coin.interval_ms));
        }
      }
      printf("]\n");
      due.Release();
    } else if (strcmp(command, "mark") == 0) {
      g_now = strtoll(fields[1], nullptr, 10);
      static char texts[1000][256];
      static CoinIdView views[1000];
      size_t n = 0;
      for (size_t i = 2; i < count && n < 1000; ++i, ++n)
        views[n] = CoinIdView{texts[n], unhex(fields[i], texts[n], sizeof(texts[n]))};
      g_popularity->MarkRefreshed(views, n);
      printf("null\n");
    } else if (strcmp(command, "dirty") == 0 || strcmp(command, "states") == 0) {
      g_now = strtoll(fields[1], nullptr, 10);
      fortis::discovery::PopularityRowList rows;
      const bool states = command[0] == 's';
      assert(states ? g_popularity->Snapshot(&rows) : g_popularity->TakeDirty(&rows));
      print_rows(rows, states);
      putchar('\n');
      rows.Release();
    } else if (strcmp(command, "size") == 0) {
      printf("%zu\n", g_popularity->Size());
    } else if (strcmp(command, "prune") == 0) {
      g_now = strtoll(fields[1], nullptr, 10);
      if (strcmp(fields[2], "-") == 0) g_popularity->Prune();
      else g_popularity->Prune(strtoull(fields[2], nullptr, 10));
      printf("null\n");
    } else if (strcmp(command, "restore") == 0) {
      g_now = strtoll(fields[1], nullptr, 10);
      static fortis::discovery::PopularityRow rows[64];
      static char texts[64][256];
      size_t n = 0;
      for (size_t i = 2; i < count && n < 64; ++i, ++n) {
        char* parts[5];
        size_t p = 0;
        for (char* cursor = fields[i]; p < 5;) {
          parts[p++] = cursor;
          char* colon = strchr(cursor, ':');
          if (!colon) break;
          *colon = '\0';
          cursor = colon + 1;
        }
        assert(p == 5);
        const size_t id_length = unhex(parts[0], texts[n], sizeof(texts[n]));
        rows[n] = fortis::discovery::PopularityRow{texts[n], id_length, parse_state(parts + 1), false, 0};
      }
      g_popularity->Restore(rows, n);
      printf("null\n");
    } else if (strcmp(command, "open") == 0) {
      open_run(fields[1], fields[2], fields[3]);
      printf("null\n");
    } else if (strcmp(command, "close") == 0) {
      close_run();
      printf("null\n");
    } else if (strcmp(command, "kv_fail") == 0) {
      if (strcmp(fields[1], "-") == 0) unsetenv("FORTIS_KV_TEST_FAIL");
      else setenv("FORTIS_KV_TEST_FAIL", fields[1], 1);
      unsetenv("FORTIS_KV_TEST_FAIL_ON_CALL");
      printf("null\n");
    } else if (strcmp(command, "snapshot_owns_core") == 0) {
      if (strcmp(fields[1], "1") == 0) setenv("FORTIS_TEST_SNAPSHOT_OWNS_CORE", "1", 1);
      else unsetenv("FORTIS_TEST_SNAPSHOT_OWNS_CORE");
      printf("null\n");
    } else if (strcmp(command, "run") == 0) {
      g_now = strtoll(fields[1], nullptr, 10);
      g_stop_checks = 0;
      g_stop_after = count > 4 ? strtoull(fields[4], nullptr, 10) : SIZE_MAX;
      const auto result = run_once(strtoull(fields[2], nullptr, 10), strtoull(fields[3], nullptr, 10));
      g_stop_after = SIZE_MAX;
      print_run(result);
      putchar('\n');
    } else if (strcmp(command, "run_busy") == 0) {
      g_now = strtoll(fields[1], nullptr, 10);
      print_run(run_with_lane_busy());
      putchar('\n');
    } else if (strcmp(command, "inspect") == 0) {
      g_now = strtoll(fields[1], nullptr, 10);
      const size_t id_length = unhex(fields[2], id_buffer, sizeof(id_buffer));
      inspect(id_buffer, id_length);
      putchar('\n');
    } else if (strcmp(command, "persisted") == 0) {
      persisted(strtoll(fields[1], nullptr, 10));
      putchar('\n');
    } else if (strcmp(command, "physical") == 0) {
      printf("%zu\n", physical_rows());
    } else if (strcmp(command, "cold_restore") == 0) {
      g_now = strtoll(fields[1], nullptr, 10);
      new_tracker(0, 0);
      const auto result = fortis::prices::coin_prices::Restore(g_run.kv, *g_popularity, g_now);
      printf("{\"rows\":%zu,\"failed\":%s}\n", result.rows, result.failed ? "true" : "false");
    } else if (strcmp(command, "cprune") == 0) {
      g_now = strtoll(fields[1], nullptr, 10);
      const auto result = fortis::prices::coin_prices::Prune(g_run.kv, *g_popularity, g_now);
      printf("{\"before\":%zu,\"after\":%zu,\"swept\":%zu}\n", result.before, result.after, result.swept);
    } else if (strcmp(command, "save") == 0) {
      // takeDirty + save outside a run (the partial-save check).
      g_now = strtoll(fields[1], nullptr, 10);
      fortis::discovery::PopularityRowList dirty;
      assert(g_popularity->TakeDirty(&dirty));
      const auto saved = fortis::discovery::coin_popularity_store::Save(g_run.kv, dirty.items.get(), dirty.count);
      dirty.Release();
      printf("{\"rows\":%zu,\"frames\":%zu,\"bytes\":%zu,\"failed\":%s}\n", saved.rows, saved.frames,
             saved.bytes, saved.failed ? "true" : "false");
    } else if (strcmp(command, "kv_close") == 0) {
      g_run.kv.Close();
      printf("null\n");
    } else if (strcmp(command, "kv_open") == 0) {
      printf("%s\n", g_run.kv.Open(fields[1]) ? "true" : "false");
    } else if (strcmp(command, "stats") == 0) {
      const fortis::discovery::CoinPopularityStats stats = g_popularity->Stats();
      printf("{\"coins\":%zu,\"seenPairs\":%zu,\"seenAllocated\":%s,\"dropped\":%zu,\"allocatedBytes\":%zu}\n",
             stats.coins, stats.seen_pairs, stats.seen_allocated ? "true" : "false", stats.dropped,
             stats.allocated_bytes);
    } else if (strcmp(command, "measure") == 0) {
      measure(fields[1], strtoull(fields[2], nullptr, 10), strtoull(fields[3], nullptr, 10));
    } else {
      fprintf(stderr, "unknown command %s\n", command);
      return 2;
    }
    fflush(stdout);
  }
  close_run();
  CoinPopularity::Destroy(g_popularity);
  return 0;
}
