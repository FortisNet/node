#include <memory>
#include <new>
#include <filesystem>
#include "native_config.h"
#include <string_view>
// Native replay of the frozen TypeScript price-jobs contract
// (tests/corpus/price_jobs.json) and the bounded seal checks.
// Driven by tests/price_jobs_test.py, which serves the fake CoinGecko and
// compares the JSON printed here with the fixture.
//
//   price_jobs_fixture replay <port> <ca> <fixture> <root> <clock-file>
//     Every refresh scenario through the real composition: a started
//     ProviderExecutionCoordinator, PriceSpotCache + PriceSeriesStore on a
//     fresh KV, a SpotProviderOwner leased on the coordinator (test target),
//     TokenDataStore demand rows and price_jobs::RefreshActive with the
//     owner in background mode. The clock file is FORTIS_TEST_PRICE_NOW_FILE
//     (the owner's cache clock); <root>/scenario and <root>/phase tell the
//     fake upstream what it is answering.
//   price_jobs_fixture lane <port> <ca> <root> <clock-file>
//     Background shedding on the shared CoinGecko lane (see lane()).
//   price_jobs_fixture seal <fixture> <root>
//     The TypeScript seal scenario, then 1,024 due rows in one 10 s pass:
//     frames, KV bytes, peak RSS, reader latency during the pass against an
//     idle baseline, an interrupted pass and a SIGKILLed pass that resume
//     without losing or duplicating candles.
#include "env_clock.h"
#include "kv_store.h"
#include "price_jobs.h"
#include "price_series_store.h"
#include "price_spot_cache.h"
#include "provider_execution_coordinator.h"
#include "spot_provider_owner.h"
#include "spot_owner_factory_support.h"
#include "token_data.h"
#include "token_json.h"

#include <assert.h>
#include <atomic>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

using namespace fortis::config;
using namespace fortis::prices;
using namespace fortis::providers;

namespace tj = fortis::codec::tj;
using tj::Value;

namespace {

constexpr int64_t kMinute = 60000;
constexpr int64_t kHour = 60 * kMinute;
constexpr int64_t kDay = 24 * kHour;

bool allow_loopback(void*, uint32_t address) { return address == 0x7f000001U; }

uint64_t monotonic_us() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000000ULL + static_cast<uint64_t>(now.tv_nsec / 1000);
}

void pause_ms(long ms) {
  timespec delay{ms / 1000, (ms % 1000) * 1000000L};
  while (nanosleep(&delay, &delay) != 0) {}
}

void write_file(const char* path, const char* text) {
  char temp[1100];
  snprintf(temp, sizeof(temp), "%s.tmp", path);
  FILE* file = fopen(temp, "w");
  assert(file);
  fputs(text, file);
  fclose(file);
  assert(rename(temp, path) == 0);
}

void write_marker(const char* root, const char* name, const char* value) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", root, name);
  write_file(path, value);
}

void set_clock(const char* clock_file, int64_t now) {
  char text[32];
  snprintf(text, sizeof(text), "%lld\n", static_cast<long long>(now));
  write_file(clock_file, text);
}

uint8_t* read_all(const char* path, size_t* size) {
  FILE* file = fopen(path, "rb");
  assert(file);
  fseek(file, 0, SEEK_END);
  const long length = ftell(file);
  fseek(file, 0, SEEK_SET);
  auto* bytes = static_cast<uint8_t*>(malloc(static_cast<size_t>(length) + 1));
  assert(bytes && fread(bytes, 1, static_cast<size_t>(length), file) == static_cast<size_t>(length));
  fclose(file);
  *size = static_cast<size_t>(length);
  return bytes;
}

long long kv_bytes(const char* directory) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/fortis.kv", directory);
  struct stat info{};
  return stat(path, &info) == 0 ? static_cast<long long>(info.st_size) : -1;
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

// ---- JSON output -------------------------------------------------------------

struct Out {
  char* data;
  size_t used, capacity;
};
Out out{};

void emit(const char* format, ...) __attribute__((format(printf, 1, 2)));
void emit(const char* format, ...) {
  va_list args;
  for (;;) {
    va_start(args, format);
    const int n = vsnprintf(out.data + out.used, out.capacity - out.used, format, args);
    va_end(args);
    assert(n >= 0);
    if (out.used + static_cast<size_t>(n) < out.capacity) { out.used += static_cast<size_t>(n); return; }
    out.capacity = out.capacity * 2 + static_cast<size_t>(n);
    auto* replacement = static_cast<char*>(realloc(out.data, out.capacity));
    if (!replacement) throw std::bad_alloc();
    out.data = replacement;
  }
}

void emit_string(const char* text) {
  emit("\"");
  for (const char* c = text; *c; ++c) {
    if (*c == '"' || *c == '\\') emit("\\%c", *c);
    else emit("%c", *c);
  }
  emit("\"");
}

// ---- the composition -------------------------------------------------------------

struct Composition {
  fortis::storage::KvStore kv;
  ProviderExecutionCoordinator coordinator;
  fortis::prices::PriceSpotCache* cache = nullptr;
  fortis::prices::PriceSeriesStore* series = nullptr;
  SpotProviderOwner* owner = nullptr;
  fortis::tokens::token_data::TokenDataStore* data = nullptr;
  NativeConfig config{};
  fortis::tokens::price_jobs::SpotOwnerFetchContext fetch{};

  void Open(const char* directory, const char* port, const char* ca) {
    assert(kv.Open(directory));
    assert(coordinator.Start());
    cache = new fortis::prices::PriceSpotCache(kv);
    series = new fortis::prices::PriceSeriesStore(kv);
    config.ca_bundle_configured = true;
    snprintf(config.ca_bundle_path, sizeof(config.ca_bundle_path), "%s", ca);
    auto made = fortis::testing::StartTestSpotOwner(*cache, *series, config, &coordinator,
        &PriceOwnerEnvClock(), [&](SpotProviderOwnerStartup& startup) {
          return startup.StartRedirected("localhost", port, ca, allow_loopback, nullptr);
        });
    owner = new SpotProviderOwner(std::move(made));
    fetch.owner = owner;
    data = new fortis::tokens::token_data::TokenDataStore(&kv);
    assert(data->valid());
  }
  void Close() {
    owner->Stop();
    delete owner;
    assert(coordinator.Stop());
    delete data;
    delete series;
    delete cache;
    kv.Close();
  }
};

struct Plan {
  struct Call { char fiat[6]; size_t first, count; } calls[64];
  size_t call_count = 0;
  char ids[512][kPriceIdMaxSourceIdBytes + 1];
  size_t id_count = 0;
};

void observe(void* context, const char* fiat, const char* const* ids, size_t count) {
  auto* plan = static_cast<Plan*>(context);
  assert(plan->call_count < 64 && plan->id_count + count <= 512);
  Plan::Call& call = plan->calls[plan->call_count++];
  snprintf(call.fiat, sizeof(call.fiat), "%s", fiat);
  call.first = plan->id_count;
  call.count = count;
  for (size_t i = 0; i < count; ++i) snprintf(plan->ids[plan->id_count++], sizeof(plan->ids[0]), "%s", ids[i]);
}

void emit_plan(const Plan& plan) {
  emit("[");
  for (size_t c = 0; c < plan.call_count; ++c) {
    const Plan::Call& call = plan.calls[c];
    emit("%s{\"fiat\":", c ? "," : "");
    emit_string(call.fiat);
    emit(",\"ids\":[");
    for (size_t i = 0; i < call.count; ++i) {
      if (i) emit(",");
      emit_string(plan.ids[call.first + i]);
    }
    emit("]}");
  }
  emit("]");
}

struct Wait {
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t done = PTHREAD_COND_INITIALIZER;
  bool finished = false;
};
void on_done(void* context, const SpotProviderOwnerResult&) {
  auto* wait = static_cast<Wait*>(context);
  pthread_mutex_lock(&wait->mutex);
  wait->finished = true;
  pthread_cond_signal(&wait->done);
  pthread_mutex_unlock(&wait->mutex);
}

void prewarm(Composition& c, const Value* warm) {
  const char* fiat = tj::Get(warm, "fiat")->text;
  const Value* ids = tj::Get(warm, "ids");
  std::string_view views[16];
  assert(ids->count <= 16);
  for (uint32_t i = 0; i < ids->count; ++i) views[i] = {ids->items[i]->text, ids->items[i]->length};
  Wait wait;
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  const uint64_t deadline = static_cast<uint64_t>(now.tv_sec) * 1000ULL + now.tv_nsec / 1000000 + 20000;
  assert(c.owner->Submit(views, ids->count, {fiat, strlen(fiat)}, SpotProviderOwnerFormat::kJson, deadline, on_done,
                         &wait).ticket() != nullptr);
  pthread_mutex_lock(&wait.mutex);
  while (!wait.finished) pthread_cond_wait(&wait.done, &wait.mutex);
  pthread_mutex_unlock(&wait.mutex);
}

void seed(Composition& c, const Value* demand, const Value* spots, int64_t now) {
  uint8_t memory[4096]{};
  for (uint32_t i = 0; demand && i < demand->count; ++i) {
    const Value* d = demand->items[i];
    tj::Arena a{memory, sizeof(memory), 0, true};
    Value* fields = tj::Obj(a);
    assert(tj::Set(a, fields, "sourceId", tj::Clone(a, tj::Get(d, "sourceId"))));
    assert(tj::Set(a, fields, "priceTier", tj::Clone(a, tj::Get(d, "priceTier"))));
    assert(tj::Set(a, fields, "priceScore", tj::Clone(a, tj::Get(d, "priceScore"))));
    assert(tj::Set(a, fields, "computedAt", tj::Num(a, static_cast<double>(now))));
    const Value* last = tj::Get(d, "lastRefreshedAt");
    if (tj::IsNumber(last)) assert(tj::Set(a, fields, "lastRefreshedAt", tj::Num(a, now + last->number)));
    assert(c.data->PatchDemand(tj::Get(d, "network")->text, tj::Get(d, "tokenId")->text, fields, now));
  }
  for (uint32_t i = 0; spots && i < spots->count; ++i) {
    const Value* s = spots->items[i];
    const fortis::prices::PriceSpotQuoteInput quote{tj::Get(s, "sourceId")->text, tj::Get(s, "price")->number, 0, 0,
                                            false, false, now + static_cast<int64_t>(tj::Get(s, "asOf")->number), 0};
    assert(c.series->RecordSpots(tj::Get(s, "fiat")->text, "coingecko", &quote, 1, now) ==
           fortis::prices::PriceSeriesStoreStatus::kStored);
  }
}

// lastRefreshedAt relative to the fixture base instant, as TypeScript records it.
void emit_demand(Composition& c, const Value* demand, int64_t now, int64_t base) {
  emit("[");
  auto memory_owner = std::make_unique<uint8_t[]>(1 << 20);
  uint8_t* memory = memory_owner.get();
  for (uint32_t i = 0; demand && i < demand->count; ++i) {
    const Value* d = demand->items[i];
    tj::Arena a{memory, 1 << 20, 0, true};
    Value* row = nullptr;
    const char* network = tj::Get(d, "network")->text;
    const char* token = tj::Get(d, "tokenId")->text;
    assert(c.data->ReadDemand(network, token, now, a, &row) == fortis::tokens::token_data::DataOutcome::kOk);
    const Value* last = tj::Get(row, "lastRefreshedAt");
    emit("%s{\"network\":", i ? "," : "");
    emit_string(network);
    emit(",\"tokenId\":");
    emit_string(token);
    if (tj::IsNumber(last)) emit(",\"lastRefreshedAt\":%lld}", static_cast<long long>(last->number) - base);
    else emit(",\"lastRefreshedAt\":null}");
  }

  emit("]");
}

void emit_result(const fortis::tokens::price_jobs::RefreshResult& r) {
  emit("{\"ranked\":%zu,\"chosen\":%zu,\"fromRecent\":%zu,\"groups\":%zu,\"touched\":%zu,\"touchFrames\":%zu,"
       "\"recentScanned\":%zu}",
       r.ranked, r.chosen, r.from_recent, r.groups, r.touched, r.touch_frames, r.recent_scanned);
}

int replay(const char* port, const char* ca, const char* fixture_path, const char* root, const char* clock) {
  size_t size = 0;
  uint8_t* text = read_all(fixture_path, &size);
  tj::Arena arena = tj::GrowingArena(64 << 20);
  const Value* fixture = tj::Parse(arena, text, size);
  assert(fixture);
  const int64_t now = 1767323045000;  // 2026-01-02T03:04:05.000Z
  assert(strcmp(tj::Get(tj::Get(fixture, "source"), "fixtureTime")->text, "2026-01-02T03:04:05.000Z") == 0);
  const Value* scenarios = tj::Get(fixture, "scenarios");
  emit("{\"scenarios\":[");
  bool first = true;
  for (uint32_t s = 0; s < scenarios->count; ++s) {
    const Value* scenario = scenarios->items[s];
    const char* name = tj::Get(scenario, "name")->text;
    if (strcmp(name, "seal") == 0) continue;
    char directory[1024];
    snprintf(directory, sizeof(directory), "%s/kv-%s", root, name);
    assert(mkdir(directory, 0700) == 0);
    write_marker(root, "scenario", name);
    write_marker(root, "phase", "prewarm");
    set_clock(clock, now);
    const Value* seed_value = tj::Get(scenario, "seed");
    emit("%s{\"name\":", first ? "" : ",");
    first = false;
    emit_string(name);
    if (strcmp(name, "restart") == 0) {
      emit(",\"phases\":[");
      const Value* demand = tj::Get(seed_value, "demand");
      for (int phase = 0; phase < 2; ++phase) {
        const int64_t at = now + (phase ? 16 * kMinute : 0);
        set_clock(clock, at);
        write_marker(root, "phase", phase ? "boot" : "run");
        Composition c;
        c.Open(directory, port, ca);
        if (!phase) seed(c, demand, nullptr, at);
        // Nothing runs at boot: give the owner time to show any request.
        pause_ms(300);
        write_marker(root, "phase", "run");
        Plan* plan = new Plan();
        fortis::tokens::price_jobs::RefreshOptions options{};
        options.observe = observe;
        options.observe_context = plan;
        const auto result =
            fortis::tokens::price_jobs::RefreshActive(*c.data, *c.series, at, fortis::tokens::price_jobs::SpotOwnerFetch, &c.fetch,
                                              options);
        emit("%s{\"at\":%lld,\"plan\":", phase ? "," : "", static_cast<long long>(at - now));
        emit_plan(*plan);
        emit(",\"refreshed\":%zu,\"demand\":", result.refreshed);
        emit_demand(c, demand, at, now);
        emit(",\"native\":");
        emit_result(result);
        emit("}");
        delete plan;
        c.Close();
      }
      emit("]}");
      continue;
    }
    Composition c;
    c.Open(directory, port, ca);
    seed(c, tj::Get(seed_value, "demand"), tj::Get(seed_value, "spots"), now);
    const Value* warm = tj::Get(seed_value, "prewarm");
    for (uint32_t w = 0; warm && w < warm->count; ++w) prewarm(c, warm->items[w]);
    write_marker(root, "phase", "run");
    Plan* plan = new Plan();
    fortis::tokens::price_jobs::RefreshOptions options{};
    options.cap = static_cast<size_t>(tj::Get(scenario, "cap")->number);
    options.observe = observe;
    options.observe_context = plan;
    const uint64_t started = monotonic_us();
    const auto result = fortis::tokens::price_jobs::RefreshActive(*c.data, *c.series, now,
                                                          fortis::tokens::price_jobs::SpotOwnerFetch, &c.fetch, options);
    const uint64_t elapsed = monotonic_us() - started;
    emit(",\"plan\":");
    emit_plan(*plan);
    emit(",\"refreshed\":%zu,\"demand\":", result.refreshed);
    emit_demand(c, tj::Get(seed_value, "demand"), now, now);
    emit(",\"native\":");
    emit_result(result);
    emit(",\"elapsedMs\":%.1f}", static_cast<double>(elapsed) / 1000.0);
    delete plan;
    c.Close();
  }
  emit("]}\n");
  fwrite(out.data, 1, out.used, stdout);
  tj::FreeArena(arena);
  free(text);
  return 0;
}

// Background mode on the shared lane: while a slow foreground CoinGecko call
// is in flight the scheduled refresh is shed (no request, no touch); once
// the lane is idle the same run refreshes.
void* submit_slow(void* opaque) {
  auto* c = static_cast<Composition*>(opaque);
  uint8_t memory[512];
  tj::Arena a{memory, sizeof(memory), 0, true};
  Value* warm = tj::Obj(a);
  Value* ids = tj::Arr(a);
  tj::Push(a, ids, tj::Str(a, "slow-fg"));
  tj::Set(a, warm, "fiat", tj::Str(a, "usd"));
  tj::Set(a, warm, "ids", ids);
  prewarm(*c, warm);
  return nullptr;
}

int lane(const char* port, const char* ca, const char* root, const char* clock) {
  const int64_t now = 1767323045000;
  char directory[1024];
  snprintf(directory, sizeof(directory), "%s/kv-lane", root);
  assert(mkdir(directory, 0700) == 0);
  write_marker(root, "scenario", "lane");
  write_marker(root, "phase", "busy");
  set_clock(clock, now);
  Composition c;
  c.Open(directory, port, ca);
  uint8_t memory[1024];
  tj::Arena a{memory, sizeof(memory), 0, true};
  Value* fields = tj::Obj(a);
  tj::Set(a, fields, "sourceId", tj::Str(a, "bitcoin"));
  tj::Set(a, fields, "priceTier", tj::Num(a, 0));
  tj::Set(a, fields, "priceScore", tj::Str(a, "1"));
  assert(c.data->PatchDemand("bitcoin", "native", fields, now));
  pthread_t slow;
  assert(pthread_create(&slow, nullptr, submit_slow, &c) == 0);
  char started[1100];
  snprintf(started, sizeof(started), "%s/slow-started", root);
  for (int i = 0; i < 500 && access(started, F_OK) != 0; ++i) pause_ms(10);
  assert(access(started, F_OK) == 0);
  const uint64_t begin = monotonic_us();
  const auto busy = fortis::tokens::price_jobs::RefreshActive(*c.data, *c.series, now, fortis::tokens::price_jobs::SpotOwnerFetch,
                                                      &c.fetch);
  const uint64_t busy_us = monotonic_us() - begin;
  pthread_join(slow, nullptr);
  write_marker(root, "phase", "idle");
  const auto idle = fortis::tokens::price_jobs::RefreshActive(*c.data, *c.series, now, fortis::tokens::price_jobs::SpotOwnerFetch,
                                                      &c.fetch);
  emit("{\"busy\":{\"refreshed\":%zu,\"touched\":%zu,\"ms\":%.1f},\"idle\":{\"refreshed\":%zu,\"touched\":%zu}}\n",
       busy.refreshed, busy.touched, static_cast<double>(busy_us) / 1000.0, idle.refreshed, idle.touched);
  c.Close();
  fwrite(out.data, 1, out.used, stdout);
  return 0;
}

// ---- seal ---------------------------------------------------------------------------

const char* const kIntervalNames[] = {"5m", "1h", "1d"};
const fortis::prices::PriceSeriesInterval kIntervals[] = {fortis::prices::PriceSeriesInterval::k5m,
                                                  fortis::prices::PriceSeriesInterval::k1h,
                                                  fortis::prices::PriceSeriesInterval::k1d};

void emit_candles(fortis::prices::PriceSeriesStore& series, const char* id, int64_t from, int64_t to, int64_t now) {
  emit("[");
  static fortis::prices::PriceSeriesCandle candles[64];
  for (size_t i = 0; i < 3; ++i) {
    size_t count = 0;
    const auto status = series.Range(kIntervals[i], "usd", id, from, to, now, candles, 64, &count);
    assert(status == fortis::prices::PriceSeriesReadStatus::kFound || status == fortis::prices::PriceSeriesReadStatus::kMissing);
    emit("%s{\"interval\":\"%s\",\"candles\":[", i ? "," : "", kIntervalNames[i]);
    for (size_t k = 0; k < count; ++k) {
      const auto& c = candles[k];
      emit("%s{\"ts\":%lld,\"open\":%.17g,\"high\":%.17g,\"low\":%.17g,\"close\":%.17g,\"volume\":", k ? "," : "",
           static_cast<long long>(c.ts), c.open, c.high, c.low, c.close);
      if (c.has_volume) emit("%.17g}", c.volume);
      else emit("null}");
    }
    emit("]}");
  }
  emit("]");
}

void source_name(char* out, size_t capacity, const char* prefix, size_t i) {
  snprintf(out, capacity, "%s-%03zu", prefix, i);
}

void record(fortis::prices::PriceSeriesStore& series, const char* prefix, size_t count, int64_t as_of, int64_t now,
            bool volume) {
  static char ids[100][32];
  fortis::prices::PriceSpotQuoteInput quotes[100];
  for (size_t i = 0; i < count;) {
    size_t n = 0;
    for (; n < 100 && i < count; ++n, ++i) {
      source_name(ids[n], sizeof(ids[n]), prefix, i);
      quotes[n] = fortis::prices::PriceSpotQuoteInput{ids[n], 100.0 + static_cast<double>(i), 0, 10.0 + static_cast<double>(i),
                                              false, volume, as_of, 0};
    }
    assert(series.RecordSpots("usd", "coingecko", quotes, n, now) == fortis::prices::PriceSeriesStoreStatus::kStored);
  }
}

// Every source has exactly one 5m candle with its recorded price.
void check_five_minute(fortis::prices::PriceSeriesStore& series, size_t count, int64_t as_of, int64_t now) {
  fortis::prices::PriceSeriesCandle candles[4];
  char id[32];
  for (size_t i = 0; i < count; ++i) {
    source_name(id, sizeof(id), "load", i);
    size_t found = 0;
    assert(series.Range(fortis::prices::PriceSeriesInterval::k5m, "usd", id, as_of - kHour, now, now, candles, 4, &found) ==
           fortis::prices::PriceSeriesReadStatus::kFound);
    assert(found == 1);
    assert(candles[0].ts == as_of / 300000 * 300000);
    assert(candles[0].open == 100.0 + static_cast<double>(i) && candles[0].close == candles[0].open);
    assert(candles[0].has_volume && candles[0].volume == 10.0 + static_cast<double>(i));
  }
}

struct Reader {
  fortis::prices::PriceSeriesStore* series;
  std::atomic<bool> stop{false};  // set by the main thread, polled by the reader
  uint64_t reads = 0, max_us = 0, total_us = 0;
  uint64_t samples[200000];
  size_t sample_count = 0;
};
void* reader_main(void* opaque) {
  auto* r = static_cast<Reader*>(opaque);
  fortis::prices::PriceSeriesCandle candles[8];
  fortis::prices::PriceSeriesSpot spot{};
  char id[32];
  const int64_t now = 1767323045000;
  for (size_t i = 0; !r->stop; ++i) {
    source_name(id, sizeof(id), "load", i % 1024);
    const uint64_t start = monotonic_us();
    size_t found = 0;
    r->series->Range(fortis::prices::PriceSeriesInterval::k1h, "usd", id, now - kDay, now, now, candles, 8, &found);
    r->series->GetSpot("usd", id, now, &spot);
    const uint64_t took = monotonic_us() - start;
    ++r->reads;
    r->total_us += took;
    if (took > r->max_us) r->max_us = took;
    if (r->sample_count < 200000) r->samples[r->sample_count++] = took;
  }
  return nullptr;
}
int compare_u64(const void* a, const void* b) {
  const uint64_t x = *static_cast<const uint64_t*>(a), y = *static_cast<const uint64_t*>(b);
  return x < y ? -1 : x > y;
}
void emit_reader(const char* name, Reader& r) {
  qsort(r.samples, r.sample_count, sizeof(uint64_t), compare_u64);
  const uint64_t p99 = r.sample_count ? r.samples[(r.sample_count * 99) / 100] : 0;
  emit("\"%s\":{\"reads\":%llu,\"meanUs\":%.1f,\"p99Us\":%llu,\"maxUs\":%llu}", name,
       static_cast<unsigned long long>(r.reads), r.reads ? static_cast<double>(r.total_us) / r.reads : 0.0,
       static_cast<unsigned long long>(p99), static_cast<unsigned long long>(r.max_us));
}

struct StopAfter {
  size_t calls = 0, limit = 0;
};
bool stop_after(void* opaque) {
  auto* s = static_cast<StopAfter*>(opaque);
  return ++s->calls >= s->limit;
}

int seal(const char* fixture_path, const char* root) {
  (void)fixture_path;
  const int64_t now = 1767323045000;
  emit("{");
  // 1. The TypeScript scenario: 400 sources, one spot each two days ago.
  {
    char directory[1024];
    snprintf(directory, sizeof(directory), "%s/kv-seal", root);
    assert(mkdir(directory, 0700) == 0);
    fortis::storage::KvStore kv;
    assert(kv.Open(directory));
    fortis::prices::PriceSeriesStore series(kv);
    const int64_t as_of = now - 2 * kDay;
    record(series, "seal", 400, as_of, now, true);
    emit("\"seal\":{\"before\":");
    emit_candles(series, "seal-007", as_of - 2 * kDay, now, now);
    emit(",\"passes\":[");
    for (int pass = 0; pass < 3; ++pass) {
      const auto result = fortis::tokens::price_jobs::SealDueYielding(series, now, 1024, 128, nullptr, nullptr, 0);
      assert(!result.failed);
      emit("%s%zu", pass ? "," : "", result.sealed);
    }
    emit("],\"after\":");
    emit_candles(series, "seal-007", as_of - 2 * kDay, now, now);
    const fortis::prices::PriceSpotQuoteInput open{"seal-open", 1, 0, 0, false, false, now - 1000, 0};
    assert(series.RecordSpots("usd", "coingecko", &open, 1, now) == fortis::prices::PriceSeriesStoreStatus::kStored);
    const auto open_pass = fortis::tokens::price_jobs::SealDueYielding(series, now, 1024, 128, nullptr, nullptr, 0);
    emit(",\"openPass\":%zu}", open_pass.sealed);
    kv.Close();
  }
  // 2. 1,024 due 5m rows (one source each, two hours ago: their hour segment
  // closed; the week and month segments are still open).
  const int64_t as_of = now - 2 * kHour;
  char directory[1024];
  snprintf(directory, sizeof(directory), "%s/kv-load", root);
  assert(mkdir(directory, 0700) == 0);
  {
    fortis::storage::KvStore kv;
    assert(kv.Open(directory));
    fortis::prices::PriceSeriesStore series(kv);
    record(series, "load", 1024, as_of, now, true);
    kv.Close();
  }
  // Copies for the interrupted and killed passes.
  char interrupted[1024], killed[1024];
  snprintf(interrupted, sizeof(interrupted), "%s/kv-interrupted", root);
  snprintf(killed, sizeof(killed), "%s/kv-killed", root);
  const char* const copies[] = {interrupted, killed};
  for (const char* copy : copies) {
    std::filesystem::copy(directory, copy, std::filesystem::copy_options::recursive);
    std::filesystem::remove(std::filesystem::path(copy) / "fortis.kv.lock");
  }
  {
    fortis::storage::KvStore kv;
    assert(kv.Open(directory));
    fortis::prices::PriceSeriesStore series(kv);
    check_five_minute(series, 1024, as_of, now);
    Reader* idle = new Reader();
    idle->series = &series;
    pthread_t thread;
    assert(pthread_create(&thread, nullptr, reader_main, idle) == 0);
    pause_ms(400);
    idle->stop = true;
    pthread_join(thread, nullptr);

    Reader* busy = new Reader();
    busy->series = &series;
    const long long bytes_before = kv_bytes(directory);
    const long long rss_before = peak_rss_bytes();
    assert(pthread_create(&thread, nullptr, reader_main, busy) == 0);
    const uint64_t started = monotonic_us();
    const auto pass = fortis::tokens::price_jobs::SealDueYielding(series, now, fortis::tokens::price_jobs::kTimerSealLimit,
                                                          fortis::tokens::price_jobs::kSealBatch);
    const uint64_t elapsed = monotonic_us() - started;
    busy->stop = true;
    pthread_join(thread, nullptr);
    assert(!pass.failed && pass.sealed == 1024);
    const auto again = fortis::tokens::price_jobs::SealDueYielding(series, now, 1024, 128);
    assert(!again.failed && again.sealed == 0 && again.frames == 0);
    check_five_minute(series, 1024, as_of, now);
    emit(",\"load\":{\"due\":1024,\"sealed\":%zu,\"frames\":%zu,\"passMs\":%.1f,\"kvBytesWritten\":%lld,"
         "\"peakRssGrowthBytes\":%lld,\"secondPass\":%zu,",
         pass.sealed, pass.frames, static_cast<double>(elapsed) / 1000.0, kv_bytes(directory) - bytes_before,
         peak_rss_bytes() - rss_before, again.sealed);
    emit_reader("idleReader", *idle);
    emit(",");
    emit_reader("readerDuringSeal", *busy);
    emit("}");
    delete idle;
    delete busy;
    kv.Close();
  }
  // 3. An interrupted pass (stopped after ten frames) and its resume.
  {
    fortis::storage::KvStore kv;
    assert(kv.Open(interrupted));
    fortis::prices::PriceSeriesStore series(kv);
    StopAfter stop{0, 10};
    const auto partial = fortis::tokens::price_jobs::SealDueYielding(series, now, 1024, 128, stop_after, &stop, 0);
    assert(partial.stopped && !partial.failed && partial.frames == 10);
    check_five_minute(series, 1024, as_of, now);
    const auto rest = fortis::tokens::price_jobs::SealDueYielding(series, now, 1024, 128, nullptr, nullptr, 0);
    assert(!rest.failed && partial.sealed + rest.sealed == 1024);
    check_five_minute(series, 1024, as_of, now);
    emit(",\"interrupted\":{\"firstSealed\":%zu,\"firstFrames\":%zu,\"resumeSealed\":%zu}", partial.sealed,
         partial.frames, rest.sealed);
    kv.Close();
  }
  // 4. SIGKILL in the middle of a slow pass, then reopen and resume.
  {
    char progress[1100];
    snprintf(progress, sizeof(progress), "%s/killed-progress", root);
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
      fortis::storage::KvStore kv;
      if (!kv.Open(killed)) _exit(3);
      fortis::prices::PriceSeriesStore series(kv);
      write_file(progress, "started\n");
      fortis::tokens::price_jobs::SealDueYielding(series, now, 1024, 128, nullptr, nullptr, 50000);
      _exit(0);
    }
    for (int i = 0; i < 200 && access(progress, F_OK) != 0; ++i) pause_ms(10);
    pause_ms(400);  // about eight 50 ms-spaced frames
    assert(kill(child, SIGKILL) == 0);
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
    fortis::storage::KvStore kv;
    assert(kv.Open(killed));
    fortis::prices::PriceSeriesStore series(kv);
    check_five_minute(series, 1024, as_of, now);
    const auto rest = fortis::tokens::price_jobs::SealDueYielding(series, now, 1024, 128, nullptr, nullptr, 0);
    assert(!rest.failed && rest.sealed > 0 && rest.sealed < 1024);
    check_five_minute(series, 1024, as_of, now);
    const auto after = fortis::tokens::price_jobs::SealDueYielding(series, now, 1024, 128, nullptr, nullptr, 0);
    assert(after.sealed == 0);
    emit(",\"killed\":{\"sealedBeforeKill\":%zu,\"resumeSealed\":%zu}", 1024 - rest.sealed, rest.sealed);
    kv.Close();
  }
  emit("}\n");
  fwrite(out.data, 1, out.used, stdout);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  out.capacity = 1 << 16;
  out.data = static_cast<char*>(malloc(out.capacity));
  if (argc == 7 && strcmp(argv[1], "replay") == 0) return replay(argv[2], argv[3], argv[4], argv[5], argv[6]);
  if (argc == 4 && strcmp(argv[1], "seal") == 0) return seal(argv[2], argv[3]);
  if (argc == 6 && strcmp(argv[1], "lane") == 0) return lane(argv[2], argv[3], argv[4], argv[5]);
  fprintf(stderr, "usage: %s replay <port> <ca> <fixture> <root> <clock-file> | seal <fixture> <root>\n", argv[0]);
  return 2;
}
