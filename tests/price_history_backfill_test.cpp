#include <cassert>
#include "price_history_backfill.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

using namespace fortis::codec;
using namespace fortis::prices;

namespace {
void verify_atomic_store(const PriceHistoryCandles& candles,
                         const char* failure_stage, bool committed) {
  char directory[] = "/tmp/fortis-history-atomic-XXXXXX";
  assert(mkdtemp(directory));
  constexpr int64_t now = 1767484800000LL;
  {
    fortis::storage::KvStore disk;
    assert(disk.Open(directory));
    fortis::prices::PriceSeriesStore series(disk);
    assert(setenv("FORTIS_KV_TEST_FAIL", failure_stage, 1) == 0);
    assert(StorePriceHistoryCandles(series, "usd", "ethereum", candles, now) ==
           fortis::prices::PriceSeriesStoreStatus::kWriteFailure);
    assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
  }
  {
    fortis::storage::KvStore recovered;
    assert(recovered.Open(directory));
    fortis::prices::PriceSeriesStore series(recovered);
    fortis::prices::PriceSeriesCandle hourly[8]{}, daily[4]{};
    size_t hourly_count = 0, daily_count = 0;
    const auto hour = series.Range(fortis::prices::PriceSeriesInterval::k1h,
        "usd", "ethereum", 1767225600000LL, 1767398400000LL, now,
        hourly, 8, &hourly_count);
    const auto day = series.Range(fortis::prices::PriceSeriesInterval::k1d,
        "usd", "ethereum", 1767225600000LL, 1767398400000LL, now,
        daily, 4, &daily_count);
    if (committed) {
      assert(hour == fortis::prices::PriceSeriesReadStatus::kFound && hourly_count == 7);
      assert(day == fortis::prices::PriceSeriesReadStatus::kFound && daily_count == 3);
    } else {
      assert(hour == fortis::prices::PriceSeriesReadStatus::kMissing && hourly_count == 0);
      assert(day == fortis::prices::PriceSeriesReadStatus::kMissing && daily_count == 0);
    }
  }
  char path[256];
  snprintf(path, sizeof(path), "%s/fortis.kv", directory); assert(unlink(path) == 0);
  snprintf(path, sizeof(path), "%s/fortis.kv.lock", directory); assert(unlink(path) == 0);
  assert(rmdir(directory) == 0);
}

using fortis::prices::HistoryCoverage;
using fortis::prices::PriceSeriesReadStatus;
using fortis::prices::PriceSeriesStoreStatus;
constexpr fortis::prices::PriceSeriesInterval kHour = fortis::prices::PriceSeriesInterval::k1h;
constexpr fortis::prices::PriceSeriesInterval kDay = fortis::prices::PriceSeriesInterval::k1d;

PriceSeriesReadStatus read_coverage(const fortis::prices::PriceSeriesStore& series,
    const char* id, int64_t now, HistoryCoverage* coverage,
    fortis::prices::PriceSeriesInterval interval = kHour) {
  return series.ReadHistoryCoverage(interval, "usd", id, now, coverage);
}

bool covers(const fortis::prices::PriceSeriesStore& series, const char* id,
    int64_t from, int64_t to, int64_t now, fortis::prices::PriceSeriesInterval interval = kHour) {
  HistoryCoverage coverage{};
  return read_coverage(series, id, now, &coverage, interval) == PriceSeriesReadStatus::kFound &&
         fortis::prices::HistoryCoverageContains(coverage, from, to);
}

void verify_coverage_store() {
  char directory[] = "/tmp/fortis-history-coverage-XXXXXX"; assert(mkdtemp(directory));
  constexpr int64_t now = 1'800'000'000'000LL;
  constexpr int64_t ttl = 6 * 60 * 60 * 1000LL;
  constexpr int64_t hour_ms = 60 * 60 * 1000LL;
  constexpr int64_t h = 3600;
  {
    fortis::storage::KvStore disk; assert(disk.Open(directory));
    fortis::prices::PriceSeriesStore series(disk);
    HistoryCoverage coverage{};
    assert(read_coverage(series, "ethereum", now, &coverage) == PriceSeriesReadStatus::kMissing);
    assert(series.MarkHistoryCoverageRange(kHour, "usd", "ethereum", 10 * h, 20 * h, now) ==
           PriceSeriesStoreStatus::kStored);
    // Keyed by interval, like candles:cov:<source>:<fiat>:<interval>.
    assert(read_coverage(series, "ethereum", now, &coverage, kDay) == PriceSeriesReadStatus::kMissing);
    assert(covers(series, "ethereum", 10 * h, 20 * h, now + ttl - 1));
    assert(covers(series, "ethereum", 12 * h, 13 * h, now));
    assert(!covers(series, "ethereum", 10 * h, 21 * h, now));
    assert(!covers(series, "ethereum", 10 * h, 20 * h, now + ttl));
    // markCoveredRange rewrites the whole list with a fresh TTL, so an older
    // range outlives its own six hours while the record keeps being marked.
    assert(series.MarkHistoryCoverageRange(kHour, "usd", "ethereum", 30 * h, 40 * h,
        now + 5 * hour_ms) == PriceSeriesStoreStatus::kStored);
    assert(covers(series, "ethereum", 10 * h, 20 * h, now + 7 * hour_ms));
    // Touching ranges merge; the record stays one range.
    assert(series.MarkHistoryCoverageRange(kHour, "usd", "ethereum", 20 * h, 30 * h,
        now + 5 * hour_ms) == PriceSeriesStoreStatus::kStored);
    assert(read_coverage(series, "ethereum", now + 5 * hour_ms, &coverage) ==
           PriceSeriesReadStatus::kFound);
    assert(coverage.count == 1 && coverage.ranges[0].from == 10 * h &&
           coverage.ranges[0].to == 40 * h && coverage.expires_ms == now + 5 * hour_ms + ttl);
    // An expired record is replaced, never merged with.
    assert(series.MarkHistoryCoverageRange(kHour, "usd", "expired", 0, h, now) ==
           PriceSeriesStoreStatus::kStored);
    assert(series.MarkHistoryCoverageRange(kHour, "usd", "expired", 2 * h, 3 * h, now + ttl) ==
           PriceSeriesStoreStatus::kStored);
    assert(read_coverage(series, "expired", now + ttl, &coverage) == PriceSeriesReadStatus::kFound);
    assert(coverage.count == 1 && coverage.ranges[0].from == 2 * h);
    // Invalid arguments write nothing.
    assert(series.MarkHistoryCoverageRange(fortis::prices::PriceSeriesInterval::k5m, "usd", "ethereum",
        0, h, now) == PriceSeriesStoreStatus::kInvalidArguments);
    assert(series.MarkHistoryCoverageRange(kHour, "usd", "ethereum", h, h, now) ==
           PriceSeriesStoreStatus::kInvalidArguments);
    assert(series.MarkHistoryCoverageRange(kHour, "usd", "bad/id", 0, h, now) ==
           PriceSeriesStoreStatus::kInvalidArguments);
    assert(series.ReadHistoryCoverage(fortis::prices::PriceSeriesInterval::k5m, "usd", "ethereum", now,
        &coverage) == PriceSeriesReadStatus::kInvalidKey);

    // Corrupt values read as kCorrupt (never covered) and are replaced.
    const char garbage_key[] = "price/history-coverage/v2/1h/usd/garbage";
    const uint8_t garbage[] = {1, 2, 3};
    fortis::storage::KvMutation corrupt{garbage_key, garbage, sizeof(garbage), 0, false};
    assert(disk.Write(&corrupt, 1));
    assert(read_coverage(series, "garbage", now, &coverage) == PriceSeriesReadStatus::kCorrupt);
    assert(series.MarkHistoryCoverageRange(kHour, "usd", "garbage", 0, h, now) ==
           PriceSeriesStoreStatus::kStored);
    assert(covers(series, "garbage", 0, h, now));
    // A valid record copied under another key fails the key binding.
    HistoryCoverage foreign{};
    foreign.interval = static_cast<uint8_t>(kHour); foreign.count = 1;
    foreign.expires_ms = now + ttl; foreign.ranges[0].from = 0; foreign.ranges[0].to = h;
    const char other_key[] = "price/history-coverage/v2/1h/usd/other";
    uint8_t encoded[fortis::prices::kHistoryCoverageMaxBytes]{};
    const size_t size = fortis::prices::EncodeHistoryCoverage(foreign,
        fortis::prices::HistoryCoverageCrc32(other_key, strlen(other_key)), encoded, sizeof(encoded));
    assert(size == fortis::prices::kHistoryCoverageHeaderBytes + fortis::prices::kHistoryCoverageRangeBytes + 4);
    const char copied_key[] = "price/history-coverage/v2/1h/usd/copied";
    fortis::storage::KvMutation copied{copied_key, encoded, static_cast<uint32_t>(size), now + ttl, false};
    assert(disk.Write(&copied, 1));
    assert(read_coverage(series, "copied", now, &coverage) == PriceSeriesReadStatus::kCorrupt);
    // A 1d request cannot read a 1h record stored under the 1d key.
    const char daily_key[] = "price/history-coverage/v2/1d/usd/daily-mismatch";
    const size_t daily_size = fortis::prices::EncodeHistoryCoverage(foreign,
        fortis::prices::HistoryCoverageCrc32(daily_key, strlen(daily_key)), encoded, sizeof(encoded));
    fortis::storage::KvMutation mismatch{daily_key, encoded, static_cast<uint32_t>(daily_size), now + ttl, false};
    assert(disk.Write(&mismatch, 1));
    assert(read_coverage(series, "daily-mismatch", now, &coverage, kDay) ==
           PriceSeriesReadStatus::kCorrupt);

    // Capacity: 33 disjoint ranges keep 32; the dropped one is the narrowest,
    // the new range survives, and no two ranges are joined across a hole.
    for (int i = 0; i < 32; ++i)
      assert(series.MarkHistoryCoverageRange(kHour, "usd", "capacity",
          i * 10 * h, i * 10 * h + (i == 7 ? 1 : 2) * h, now) == PriceSeriesStoreStatus::kStored);
    assert(read_coverage(series, "capacity", now, &coverage) == PriceSeriesReadStatus::kFound);
    assert(coverage.count == 32);
    assert(series.MarkHistoryCoverageRange(kHour, "usd", "capacity", 400 * h, 401 * h, now) ==
           PriceSeriesStoreStatus::kStored);
    assert(read_coverage(series, "capacity", now, &coverage) == PriceSeriesReadStatus::kFound);
    assert(coverage.count == 32);
    assert(!covers(series, "capacity", 70 * h, 71 * h, now));   // narrowest dropped
    assert(covers(series, "capacity", 400 * h, 401 * h, now));  // new range kept
    assert(covers(series, "capacity", 0, 2 * h, now));
    assert(!covers(series, "capacity", 0, 12 * h, now));        // hole still a hole
  }
  {
    // Restart: every committed record is read back from the log.
    fortis::storage::KvStore disk; assert(disk.Open(directory));
    fortis::prices::PriceSeriesStore series(disk);
    assert(covers(series, "ethereum", 10 * h, 40 * h, now + 5 * hour_ms));
    assert(covers(series, "garbage", 0, h, now));
    assert(covers(series, "capacity", 400 * h, 401 * h, now));
    // A torn coverage frame leaves the previous record untouched.
    assert(setenv("FORTIS_KV_TEST_FAIL", "write_after_header", 1) == 0);
    assert(series.MarkHistoryCoverageRange(kHour, "usd", "ethereum", 50 * h, 60 * h,
        now + 5 * hour_ms) == PriceSeriesStoreStatus::kWriteFailure);
    assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
  }
  {
    fortis::storage::KvStore disk; assert(disk.Open(directory));
    fortis::prices::PriceSeriesStore series(disk);
    HistoryCoverage coverage{};
    assert(read_coverage(series, "ethereum", now + 5 * hour_ms, &coverage) ==
           PriceSeriesReadStatus::kFound);
    assert(coverage.count == 1 && coverage.ranges[0].to == 40 * h);
    assert(!covers(series, "ethereum", 50 * h, 60 * h, now + 5 * hour_ms));
  }
  char path[256];
  snprintf(path,sizeof(path),"%s/fortis.kv",directory); assert(unlink(path)==0);
  snprintf(path,sizeof(path),"%s/fortis.kv.lock",directory); assert(unlink(path)==0);
  assert(rmdir(directory)==0);
}

// ── Ten-year daily history ──

constexpr int64_t kDayMs = 86400000LL;
// FIRST - 10 * 366 days, the TypeScript daily maximum's start.
constexpr int64_t kDecadeOrigin = 1451001600000LL;
constexpr size_t kDecadePoints = 10 * 366 + 1;  // midnights in [from, to]

struct BodyBuilder {
  char* out;
  size_t capacity;
  size_t used = 0;
  void text(const char* value) {
    const size_t n = strlen(value);
    assert(used + n < capacity);
    memcpy(out + used, value, n); used += n; out[used] = 0;
  }
  void pair(int64_t ms, double value, bool comma) {
    const int n = snprintf(out + used, capacity - used, "%s[%lld,%.3f]", comma ? "," : "",
                           static_cast<long long>(ms), value);
    assert(n > 0 && static_cast<size_t>(n) < capacity - used);
    used += static_cast<size_t>(n);
  }
};

// A CoinGecko daily feed shaped like the ten-year fixture model: one sample
// per midnight from kDecadeOrigin, three series unless `caps` is false, then
// optional extra token-only pairs and a string pad.
size_t build_daily_body(char* out, size_t capacity, size_t points, bool caps,
                        size_t extra_pairs = 0, size_t pad = 0) {
  BodyBuilder body{out, capacity};
  body.text("{\"prices\":[");
  for (size_t d = 0; d < points; ++d)
    body.pair(kDecadeOrigin + static_cast<int64_t>(d) * kDayMs, 100.0 + d * 0.375, d != 0);
  if (caps) {
    body.text("],\"market_caps\":[");
    for (size_t d = 0; d < points; ++d)
      body.pair(kDecadeOrigin + static_cast<int64_t>(d) * kDayMs,
                1e12 + 29628.0 * static_cast<double>(d) + 0.25, d != 0);
  }
  body.text("],\"total_volumes\":[");
  for (size_t d = 0; d < points; ++d)
    body.pair(kDecadeOrigin + static_cast<int64_t>(d) * kDayMs, 1000.0 + 1.5 * d, d != 0);
  body.text("]");
  if (extra_pairs) {
    body.text(",\"extra\":[");
    for (size_t i = 0; i < extra_pairs; ++i) body.pair(static_cast<int64_t>(i), 1.0, i != 0);
    body.text("]");
  }
  if (pad) {
    body.text(",\"pad\":\"");
    assert(body.used + pad + 3 < capacity);
    memset(out + body.used, 'x', pad); body.used += pad; out[body.used] = 0;
    body.text("\"");
  }
  body.text("}");
  return body.used;
}

PriceHistoryParseStatus parse(const char* body, size_t length, PriceHistoryWorkspace* workspace) {
  return ParseCoinGeckoMarketChartRange(reinterpret_cast<const unsigned char*>(body), length,
                                        workspace);
}

void verify_decade_parser_and_store() {
  constexpr size_t kCapacity = 640 * 1024;
  char* body = static_cast<char*>(malloc(kCapacity)); assert(body);
  auto* workspace = static_cast<PriceHistoryWorkspace*>(calloc(1, sizeof(PriceHistoryWorkspace)));
  assert(workspace);

  // The full decade: over the old 256 KiB input and 32,768-token bounds.
  size_t length = build_daily_body(body, kCapacity, kDecadePoints, true);
  assert(length > 256 * 1024 && length <= kPriceHistoryMaxProviderBodyBytes);
  assert(7 + 9 * kDecadePoints > 32768);
  assert(parse(body, length, workspace) == PriceHistoryParseStatus::kOk);
  // CandleHistory buckets the daily feed into one 1h and one 1d row per point.
  assert(workspace->hourly_count == kDecadePoints && workspace->daily_count == kDecadePoints);
  assert(workspace->daily[0].candle.ts == kDecadeOrigin);
  assert(workspace->daily[0].candle.close == 100.0 && workspace->daily[0].candle.volume == 1000.0);
  assert(workspace->daily[kDecadePoints - 1].candle.ts ==
         kDecadeOrigin + static_cast<int64_t>(kDecadePoints - 1) * kDayMs);
  assert(workspace->daily[kDecadePoints - 1].candle.close == 1472.5);
  assert(workspace->daily[kDecadePoints - 1].candle.volume == 6490.0);
  assert(workspace->hourly[kDecadePoints - 1].candle.has_volume);

  // Exactly the 3,700-point bound (with market caps: 33,307 tokens) parses;
  // one more point fails the whole response before anything is stored.
  PriceHistoryWorkspace* bound = static_cast<PriceHistoryWorkspace*>(
      calloc(1, sizeof(PriceHistoryWorkspace)));
  assert(bound);
  length = build_daily_body(body, kCapacity, kPriceHistoryMaxPointCount, true);
  assert(parse(body, length, bound) == PriceHistoryParseStatus::kOk);
  assert(bound->hourly_count == kPriceHistoryMaxPointCount &&
         bound->daily_count == kPriceHistoryMaxPointCount);
  length = build_daily_body(body, kCapacity, kPriceHistoryMaxPointCount + 1, true);
  assert(parse(body, length, bound) == PriceHistoryParseStatus::kTooManyCandles);
  // Over the token arena but not the point bound: invalid JSON, not success.
  length = build_daily_body(body, kCapacity, kPriceHistoryMaxPointCount, true, 100);
  assert(parse(body, length, bound) == PriceHistoryParseStatus::kInvalidJson);
  // The input bound is 512 KiB: a 300 KiB body parses, a 520 KiB one does not.
  length = build_daily_body(body, kCapacity, 2, false, 0, 300 * 1024);
  assert(length > kBoundedJsonMaxInputBytes);
  assert(parse(body, length, bound) == PriceHistoryParseStatus::kOk && bound->daily_count == 2);
  length = build_daily_body(body, kCapacity, 2, false, 0, 520 * 1024);
  assert(length > kPriceHistoryMaxProviderBodyBytes);
  assert(parse(body, length, bound) == PriceHistoryParseStatus::kInvalidJson);

  // Out-of-order samples take the full-scan path with TypeScript's bucket()
  // semantics: open/close in provider order, the last volume wins, and only
  // price-created buckets get a volume.
  const char unordered[] =
      "{\"prices\":[[1451088000000,2],[1451001600000,1],[1451088000000,3]],"
      "\"total_volumes\":[[1451088000000,5],[1451001600000,7],[1451088000000,9],"
      "[1451174400000,11]]}";
  assert(parse(unordered, sizeof(unordered) - 1, bound) == PriceHistoryParseStatus::kOk);
  assert(bound->daily_count == 2 && bound->hourly_count == 2);
  assert(bound->daily[0].candle.ts == kDecadeOrigin && bound->daily[0].candle.close == 1 &&
         bound->daily[0].candle.volume == 7);
  assert(bound->daily[1].candle.open == 2 && bound->daily[1].candle.close == 3 &&
         bound->daily[1].candle.high == 3 && bound->daily[1].candle.low == 2 &&
         bound->daily[1].candle.volume == 9);
  free(bound);

  // Store: the decade commits in 37 atomic frames of at most 200 mixed rows
  // and reads back through the 1d capacity; 1h and 5m keep 2,500.
  char directory[] = "/tmp/fortis-history-decade-XXXXXX"; assert(mkdtemp(directory));
  constexpr int64_t now = 1767484800000LL;
  auto* candles = static_cast<fortis::prices::PriceSeriesCandle*>(
      calloc(fortis::prices::kPriceSeriesMaxDailyRangeCandles + 1, sizeof(fortis::prices::PriceSeriesCandle)));
  assert(candles);
  const int64_t decade_to = kDecadeOrigin + static_cast<int64_t>(kDecadePoints - 1) * kDayMs;
  {
    fortis::storage::KvStore disk; assert(disk.Open(directory));
    fortis::prices::PriceSeriesStore series(disk);
    size_t h = 0, d = 0, chunks = 0;
    fortis::prices::PriceSeriesCandleBatchInput batch[kPriceHistoryMaxCandles]{};
    while (h < workspace->hourly_count || d < workspace->daily_count) {
      const size_t count = BuildPriceHistoryChunk(*workspace, &h, &d, batch);
      assert(count > 0 && count <= kPriceHistoryMaxCandles);
      assert(series.UpsertCandlesAtomic("usd", "decade", batch, count, now) ==
             PriceSeriesStoreStatus::kStored);
      ++chunks;
    }
    assert(chunks == 37);
    size_t count = 0;
    assert(series.Range(kDay, "usd", "decade", kDecadeOrigin, decade_to, now, candles,
                        kDecadePoints, &count) == PriceSeriesReadStatus::kFound);
    assert(count == kDecadePoints && candles[count - 1].ts == decade_to);
    assert(candles[count - 1].close == 1472.5 && candles[count - 1].has_volume);
    assert(series.Range(kDay, "usd", "decade", kDecadeOrigin, decade_to, now, candles,
                        fortis::prices::kPriceSeriesMaxDailyRangeCandles, &count) ==
           PriceSeriesReadStatus::kFound && count == kDecadePoints);
    assert(series.Range(kDay, "usd", "decade", kDecadeOrigin, decade_to, now, candles,
                        fortis::prices::kPriceSeriesMaxDailyRangeCandles + 1, &count) ==
           PriceSeriesReadStatus::kInvalidKey);
    assert(series.Range(kHour, "usd", "decade", kDecadeOrigin, decade_to, now, candles,
                        fortis::prices::kPriceSeriesMaxRangeCandles + 1, &count) ==
           PriceSeriesReadStatus::kInvalidKey);
    // The 3,661 hourly rows of the daily feed overflow the unchanged 1h read.
    assert(series.Range(kHour, "usd", "decade", kDecadeOrigin, decade_to, now, candles,
                        fortis::prices::kPriceSeriesMaxRangeCandles, &count) ==
           PriceSeriesReadStatus::kCapacityExceeded);
    assert(series.Range(fortis::prices::PriceSeriesInterval::k5m, "usd", "decade", kDecadeOrigin,
                        decade_to, now, candles, fortis::prices::kPriceSeriesMaxRangeCandles + 1,
                        &count) == PriceSeriesReadStatus::kInvalidKey);
    fortis::prices::PriceSeriesAvailability hourly{};
    assert(series.Availability(kHour, "usd", "decade", now, &hourly) ==
           PriceSeriesReadStatus::kFound);
    assert(hourly.oldest == kDecadeOrigin && hourly.newest == decade_to);
  }
  {
    // Restart: the decade is read back from the log.
    fortis::storage::KvStore disk; assert(disk.Open(directory));
    fortis::prices::PriceSeriesStore series(disk);
    size_t count = 0;
    assert(series.Range(kDay, "usd", "decade", kDecadeOrigin, decade_to, now, candles,
                        kDecadePoints, &count) == PriceSeriesReadStatus::kFound);
    assert(count == kDecadePoints && candles[0].ts == kDecadeOrigin);
  }
  char path[256];
  snprintf(path, sizeof(path), "%s/fortis.kv", directory); assert(unlink(path) == 0);
  snprintf(path, sizeof(path), "%s/fortis.kv.lock", directory); assert(unlink(path) == 0);
  assert(rmdir(directory) == 0);
  free(candles); free(workspace); free(body);
}
}  // namespace

static PriceHistoryCandles parse_fixture(const char* path,
    BoundedJsonToken* tokens, size_t token_capacity) {
  FILE* file = fopen(path, "rb"); assert(file);
  fseek(file, 0, SEEK_END); const long size = ftell(file); assert(fseek(file, 0, SEEK_SET) == 0);
  assert(size > 0 && size < 256 * 1024);
  char* json = static_cast<char*>(malloc(static_cast<size_t>(size) + 1));
  assert(json && fread(json, 1, static_cast<size_t>(size), file) == static_cast<size_t>(size));
  fclose(file); json[size] = 0;
  const char* marker = strstr(json, "\"wireResponse\": {"); assert(marker);
  const char* begin = strchr(marker, '{'); assert(begin);
  const char* end = strstr(begin, "\n  },\n  \"response\""); assert(end);
  const size_t body_length = static_cast<size_t>(end - begin + 4);
  PriceHistoryCandles candles{};
  const PriceHistoryParseStatus status = ParseCoinGeckoMarketChartRange(
      reinterpret_cast<const unsigned char*>(begin), body_length,
      tokens, token_capacity, &candles);
  if (status != PriceHistoryParseStatus::kOk)
    fprintf(stderr, "fixture parse failed %s status=%d bytes=%zu capacity=%zu\n",
        path, static_cast<int>(status), body_length, token_capacity);
  assert(status == PriceHistoryParseStatus::kOk);
  free(json);
  return candles;
}

static PriceHistoryWorkspace* parse_workspace_fixture(const char* path) {
  FILE* file = fopen(path, "rb"); assert(file);
  fseek(file, 0, SEEK_END); const long size = ftell(file); assert(fseek(file, 0, SEEK_SET) == 0);
  assert(size > 0 && size < 256 * 1024);
  char* json = static_cast<char*>(malloc(static_cast<size_t>(size) + 1));
  assert(json && fread(json, 1, static_cast<size_t>(size), file) == static_cast<size_t>(size));
  fclose(file); json[size] = 0;
  const char* marker = strstr(json, "\"wireResponse\": {"); assert(marker);
  const char* begin = strchr(marker, '{'); assert(begin);
  const char* end = strstr(begin, "\n  },\n  \"response\""); assert(end);
  const size_t body_length = static_cast<size_t>(end - begin + 4);
  auto* workspace = static_cast<PriceHistoryWorkspace*>(calloc(1, sizeof(PriceHistoryWorkspace)));
  assert(workspace);
  assert(ParseCoinGeckoMarketChartRange(reinterpret_cast<const unsigned char*>(begin),
      body_length, workspace) == PriceHistoryParseStatus::kOk);
  free(json);
  return workspace;
}

int main(int argc, char** argv) {
  assert(argc == 3);
  verify_coverage_store();
  BoundedJsonToken* tokens = static_cast<BoundedJsonToken*>(calloc(kPriceHistoryMaxJsonTokens, sizeof(BoundedJsonToken)));
  assert(tokens);
  PriceHistoryCandles candles = parse_fixture(argv[1], tokens, kPriceHistoryMaxJsonTokens);
  assert(candles.count == 10);
  const int64_t ts[] = {1767225600000LL,1767225600000LL,1767229200000LL,
    1767308400000LL,1767312000000LL,1767312000000LL,1767355200000LL,
    1767384000000LL,1767398400000LL,1767398400000LL};
  const double open[] = {10,10,11,15,17,17,16,18,20,20};
  const double close[] = {9,15,13,15,17,16,16,18,20,20};
  const double volume[] = {100,300,300,0,0,500,500,0,0,0};
  for (size_t i = 0; i < candles.count; ++i) {
    assert(candles.items[i].candle.ts == ts[i]);
    assert(candles.items[i].candle.open == open[i]);
    assert(candles.items[i].candle.close == close[i]);
    assert(candles.items[i].candle.has_volume == (volume[i] != 0));
    if (volume[i]) assert(candles.items[i].candle.volume == volume[i]);
    const bool daily[] = {false,true,false,false,false,true,false,false,false,true};
    assert(candles.items[i].interval == (daily[i] ? fortis::prices::PriceSeriesInterval::k1d : fortis::prices::PriceSeriesInterval::k1h));
  }
  verify_atomic_store(candles, "write_after_header", false);
  verify_atomic_store(candles, "write_after_fsync", true);
  PriceHistoryWorkspace* daily365 = parse_workspace_fixture(argv[2]);
  assert(daily365->hourly_count == 365 && daily365->daily_count == 365);
  for (size_t i = 0; i < daily365->daily_count; ++i) {
    assert(daily365->daily[i].candle.open == 100.0 + i / 10.0);
    assert(daily365->daily[i].candle.has_volume);
    assert(daily365->daily[i].candle.volume == 1000.0 + i);
  }
  size_t daily_h = 0, daily_d = 0, daily_chunks = 0;
  fortis::prices::PriceSeriesCandleBatchInput daily_batch[kPriceHistoryMaxCandles]{};
  while (daily_h < daily365->hourly_count || daily_d < daily365->daily_count) {
    const size_t count = BuildPriceHistoryChunk(*daily365, &daily_h, &daily_d, daily_batch);
    assert(count > 0 && count <= kPriceHistoryMaxCandles); ++daily_chunks;
  }
  assert(daily_chunks == 4);
  free(daily365);
  const char malformed[] = "{\"prices\":[[1,\"bad\"]],\"total_volumes\":[]}";
  assert(ParseCoinGeckoMarketChartRange(reinterpret_cast<const unsigned char*>(malformed),
      sizeof(malformed)-1, tokens, kBoundedJsonMaxTokens, &candles) == PriceHistoryParseStatus::kInvalidPoint);
  const char empty[] = "{}";
  assert(ParseCoinGeckoMarketChartRange(reinterpret_cast<const unsigned char*>(empty),
      sizeof(empty)-1, tokens, kBoundedJsonMaxTokens, &candles) == PriceHistoryParseStatus::kOk);
  assert(candles.count == 0);
  const char null_arrays[] = "{\"prices\":null,\"total_volumes\":null}";
  assert(ParseCoinGeckoMarketChartRange(reinterpret_cast<const unsigned char*>(null_arrays),
      sizeof(null_arrays)-1, tokens, kBoundedJsonMaxTokens, &candles) == PriceHistoryParseStatus::kOk);
  assert(candles.count == 0);
  const char negative[] = "{\"prices\":[[1767225600000,-1]],\"total_volumes\":[[1767225600000,-2]]}";
  assert(ParseCoinGeckoMarketChartRange(reinterpret_cast<const unsigned char*>(negative),
      sizeof(negative)-1, tokens, kBoundedJsonMaxTokens, &candles) == PriceHistoryParseStatus::kOk);
  assert(candles.count == 2 && candles.items[0].candle.close == -1 &&
         candles.items[0].candle.volume == -2 && candles.items[0].candle.has_volume);
  free(tokens);

  char* large = static_cast<char*>(malloc(220 * 1024)); assert(large);
  size_t used = static_cast<size_t>(snprintf(large, 220 * 1024, "{\"prices\":["));
  const int64_t origin = 1767225600000LL;
  for (int i = 0; i < 2160; ++i) {
    const int n = snprintf(large + used, 220 * 1024 - used, "%s[%lld,%d]",
        i ? "," : "", static_cast<long long>(origin + i * 3600000LL), 100 + i);
    assert(n > 0); used += static_cast<size_t>(n);
  }
  int n = snprintf(large + used, 220 * 1024 - used, "],\"total_volumes\":[");
  assert(n > 0); used += static_cast<size_t>(n);
  for (int i = 0; i < 2160; ++i) {
    n = snprintf(large + used, 220 * 1024 - used, "%s[%lld,-%d]",
        i ? "," : "", static_cast<long long>(origin + i * 3600000LL), i);
    assert(n > 0); used += static_cast<size_t>(n);
  }
  n = snprintf(large + used, 220 * 1024 - used, "]}");
  assert(n > 0); used += static_cast<size_t>(n);
  auto* workspace = static_cast<PriceHistoryWorkspace*>(calloc(1, sizeof(PriceHistoryWorkspace)));
  assert(workspace && sizeof(PriceHistoryWorkspace) <= 1280 * 1024);
  assert(ParseCoinGeckoMarketChartRange(reinterpret_cast<unsigned char*>(large), used,
      workspace) == PriceHistoryParseStatus::kOk);
  assert(workspace->hourly_count == 2160 && workspace->daily_count == 90);
  assert(workspace->hourly[2].candle.has_volume && workspace->hourly[2].candle.volume == -2);
  size_t h = 0, d = 0, chunks = 0;
  fortis::prices::PriceSeriesCandleBatchInput batch[kPriceHistoryMaxCandles]{};
  while (h < workspace->hourly_count || d < workspace->daily_count) {
    const size_t count = BuildPriceHistoryChunk(*workspace, &h, &d, batch);
    assert(count > 0 && count <= 200); ++chunks;
  }
  assert(chunks == 12);
  free(workspace); free(large);
  verify_decade_parser_and_store();
  puts("native CoinGecko range bucketing fixture passed");
}
