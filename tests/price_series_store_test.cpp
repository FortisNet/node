#include <cstdio>
#include "../src/prices/price_series_store.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace fortis::prices;
using namespace fortis::storage;

namespace {
using namespace fortis;
constexpr int64_t kT0 = 1'792'058'699'999LL; // 2026-10-15 10:04:59.999 UTC
PriceSpotQuoteInput quote(const char *id, double price, int64_t at,
                          double volume = 0, bool has_volume = false) {
  return {id, price, 0, volume, false, has_volume, at, 0};
}
void remove_store(const char *dir) {
  char path[512];
  snprintf(path, sizeof(path), "%s/fortis.kv", dir);
  assert(unlink(path) == 0);
  snprintf(path, sizeof(path), "%s/fortis.kv.lock", dir);
  assert(unlink(path) == 0);
  assert(rmdir(dir) == 0);
}
bool count_orphan_marker(const char *key, const void *, size_t, void *context) {
  if (strstr(key, "/empty-marker/"))
    ++*static_cast<size_t *>(context);
  return true;
}
bool count_upsert_marker(const char *key, const void *, size_t, void *context) {
  if (strstr(key, "/5m/usd/upsert-seal/"))
    ++*static_cast<size_t *>(context);
  return true;
}
struct KeyCollector {
  char (*keys)[600];
  size_t count;
  size_t capacity;
  const char *needle;
};
bool collect_matching_key(const char *key, const void *, size_t,
                          void *context) {
  auto *collector = static_cast<KeyCollector *>(context);
  if (collector->needle && !strstr(key, collector->needle))
    return true;
  if (collector->count == collector->capacity)
    return false;
  std::snprintf(collector->keys[collector->count++], sizeof(collector->keys[0]), "%s", key);
  return true;
}
void expect_range(PriceSeriesStore &store, PriceSeriesInterval interval,
                  int64_t from, int64_t to, const int64_t *timestamps,
                  const double *closes, size_t expected, int64_t now) {
  PriceSeriesCandle candles[8]{};
  size_t count = 0;
  assert(store.Range(interval, "usd", "ethereum", from, to, now, candles, 8,
                     &count) == PriceSeriesReadStatus::kFound);
  assert(count == expected);
  for (size_t i = 0; i < expected; ++i) {
    assert(candles[i].ts == timestamps[i]);
    assert(candles[i].close == closes[i]);
  }
}
} // namespace

int main() {
  char directory[] = "/tmp/fortis-price-series-XXXXXX";
  assert(mkdtemp(directory));
  const int64_t boundary = kT0 + 1;
  const int64_t late = kT0 - 30'000;
  const int64_t next_hour = kT0 + 3'300'001;
  const int64_t now = next_hour + 10'000;
  {
    KvStore disk;
    assert(disk.Open(directory));
    PriceSeriesStore series(disk);
    PriceSeriesSpot absent{};
    assert(series.GetSpot("usd", "ethereum", now, &absent) ==
           PriceSeriesReadStatus::kMissing);
    const PriceSpotQuoteInput epoch = quote("epoch", 1.25, 0);
    assert(series.RecordSpots("usd", "coingecko", &epoch, 1, 0) ==
           PriceSeriesStoreStatus::kStored);
    PriceSeriesCandle epoch_candle{};
    size_t epoch_count = 0;
    assert(series.Range(PriceSeriesInterval::k5m, "usd", "epoch", 0, 0, 0,
                        &epoch_candle, 1,
                        &epoch_count) == PriceSeriesReadStatus::kFound);
    assert(epoch_count == 1 && epoch_candle.open == 1.25 &&
           epoch_candle.high == 1.25 && epoch_candle.low == 1.25 &&
           epoch_candle.close == 1.25);
    const PriceSpotQuoteInput a = quote("ethereum", 10.125, kT0, 100.5, true);
    assert(series.RecordSpots("usd", "coingecko", &a, 1, kT0 + 1) ==
           PriceSeriesStoreStatus::kStored);
    const PriceSpotQuoteInput b = quote("ethereum", 11.25, boundary);
    assert(series.RecordSpots("usd", "coingecko", &b, 1, boundary + 1) ==
           PriceSeriesStoreStatus::kStored);
    const PriceSpotQuoteInput old = quote("ethereum", 9.5, late, 90, true);
    assert(series.RecordSpots("usd", "coingecko", &old, 1, boundary + 2) ==
           PriceSeriesStoreStatus::kNoItems);
    const PriceSpotQuoteInput c = quote("ethereum", 12, next_hour, 120, true);
    assert(series.RecordSpots("usd", "coingecko", &c, 1, now) ==
           PriceSeriesStoreStatus::kStored);
    PriceSeriesSpot spot{};
    assert(series.GetSpot("usd", "ethereum", now, &spot) ==
           PriceSeriesReadStatus::kFound);
    assert(spot.price == 12 && spot.has_volume24h && spot.volume24h == 120 &&
           spot.as_of_ms == next_hour && strcmp(spot.source, "coingecko") == 0);
    const int64_t ts5[] = {1'792'058'400'000LL, 1'792'058'700'000LL,
                           1'792'062'000'000LL};
    const double close5[] = {10.125, 11.25, 12};
    expect_range(series, PriceSeriesInterval::k5m, ts5[0], ts5[2], ts5, close5,
                 3, now);
    PriceSeriesCandle too_small[2]{};
    size_t too_small_count = 0;
    assert(series.Range(PriceSeriesInterval::k5m, "usd", "ethereum", ts5[0],
                        ts5[2], now, too_small, 2, &too_small_count) ==
               PriceSeriesReadStatus::kCapacityExceeded &&
           too_small_count == 0);
    PriceSeriesCandle five_candles[4]{};
    size_t five_count = 0;
    assert(series.Range(PriceSeriesInterval::k5m, "usd", "ethereum", ts5[0],
                        ts5[2], now, five_candles, 4,
                        &five_count) == PriceSeriesReadStatus::kFound &&
           five_count == 3);
    assert(five_candles[0].open == 10.125 && five_candles[0].high == 10.125 &&
           five_candles[0].low == 10.125 && five_candles[0].close == 10.125 &&
           five_candles[0].volume == 100.5 && five_candles[0].has_volume &&
           five_candles[0].first_at_ms == kT0 &&
           five_candles[0].last_at_ms == kT0);
    assert(five_candles[1].open == 11.25 && five_candles[1].high == 11.25 &&
           five_candles[1].low == 11.25 && five_candles[1].close == 11.25 &&
           five_candles[1].volume == 100.5 && five_candles[1].has_volume &&
           five_candles[1].first_at_ms == boundary &&
           five_candles[1].last_at_ms == boundary);
    assert(five_candles[2].open == 12 && five_candles[2].high == 12 &&
           five_candles[2].low == 12 && five_candles[2].close == 12 &&
           five_candles[2].volume == 120 && five_candles[2].has_volume &&
           five_candles[2].first_at_ms == next_hour &&
           five_candles[2].last_at_ms == next_hour);
    const int64_t ts1h[] = {1'792'058'400'000LL, 1'792'062'000'000LL};
    const double close1h[] = {11.25, 12};
    expect_range(series, PriceSeriesInterval::k1h, ts1h[0], ts1h[1], ts1h,
                 close1h, 2, now);
    PriceSeriesCandle hour_candles[4]{};
    size_t hour_count = 0;
    assert(series.Range(PriceSeriesInterval::k1h, "usd", "ethereum", ts1h[0],
                        ts1h[1], now, hour_candles, 4,
                        &hour_count) == PriceSeriesReadStatus::kFound);
    assert(hour_count == 2 && hour_candles[0].open == 10.125 &&
           hour_candles[0].high == 11.25 && hour_candles[0].low == 10.125 &&
           hour_candles[0].close == 11.25 && hour_candles[0].volume == 100.5 &&
           hour_candles[0].has_volume && hour_candles[0].first_at_ms == kT0 &&
           hour_candles[0].last_at_ms == boundary);
    assert(hour_candles[1].open == 12 && hour_candles[1].high == 12 &&
           hour_candles[1].low == 12 && hour_candles[1].close == 12 &&
           hour_candles[1].volume == 120 &&
           hour_candles[1].first_at_ms == next_hour &&
           hour_candles[1].last_at_ms == next_hour);
    const int64_t ts1d[] = {1'792'022'400'000LL};
    const double close1d[] = {12};
    expect_range(series, PriceSeriesInterval::k1d, ts1d[0], ts1d[0], ts1d,
                 close1d, 1, now);
    PriceSeriesCandle day_candle{};
    size_t day_count = 0;
    assert(series.Range(PriceSeriesInterval::k1d, "usd", "ethereum", ts1d[0],
                        ts1d[0], now, &day_candle, 1,
                        &day_count) == PriceSeriesReadStatus::kFound &&
           day_count == 1);
    assert(day_candle.open == 10.125 && day_candle.high == 12 &&
           day_candle.low == 10.125 && day_candle.close == 12 &&
           day_candle.volume == 120 && day_candle.has_volume &&
           day_candle.first_at_ms == kT0 && day_candle.last_at_ms == next_hour);
    auto expect_candle_expiry = [&](PriceSeriesInterval interval, int64_t ts,
                                    int64_t expiry) {
      PriceSeriesCandle value{};
      size_t value_count = 0;
      assert(series.Range(interval, "usd", "ethereum", ts, ts, expiry - 1,
                          &value, 1,
                          &value_count) == PriceSeriesReadStatus::kFound &&
             value_count == 1);
      assert(series.Range(interval, "usd", "ethereum", ts, ts, expiry, &value,
                          1, &value_count) == PriceSeriesReadStatus::kMissing &&
             value_count == 0);
    };
    constexpr int64_t kWeek = 7 * 86'400'000LL;
    const int64_t weekly_end = ts1h[1] / kWeek * kWeek + kWeek;
    expect_candle_expiry(PriceSeriesInterval::k5m, ts5[2],
                         ts5[2] + 3'600'000 + 7 * 86'400'000LL);
    expect_candle_expiry(PriceSeriesInterval::k1h, ts1h[1],
                         weekly_end + 90 * 86'400'000LL);
    expect_candle_expiry(PriceSeriesInterval::k1d, ts1d[0],
                         1'793'491'200'000LL + 366 * 86'400'000LL);

    // Four small per-quote rows (spot + three candles) remain well below 2 KiB.
    struct stat before{}, after{};
    char path[512];
    snprintf(path, sizeof(path), "%s/fortis.kv", directory);
    assert(stat(path, &before) == 0);
    const PriceSpotQuoteInput bounded = quote("small-write", 1, now);
    assert(series.RecordSpots("usd", "coingecko", &bounded, 1, now + 1) ==
           PriceSeriesStoreStatus::kStored);
    assert(stat(path, &after) == 0 && after.st_size > before.st_size &&
           after.st_size - before.st_size < 2048);
    const int fd = open(path, O_RDONLY);
    assert(fd >= 0);
    uint8_t frame[12]{};
    assert(pread(fd, frame, sizeof(frame), before.st_size) == sizeof(frame));
    close(fd);
    assert(memcmp(frame, "FKV1", 4) == 0);
    const uint32_t body_bytes = static_cast<uint32_t>(frame[4]) |
                                (static_cast<uint32_t>(frame[5]) << 8) |
                                (static_cast<uint32_t>(frame[6]) << 16) |
                                (static_cast<uint32_t>(frame[7]) << 24);
    assert(after.st_size - before.st_size ==
           static_cast<off_t>(12 + body_bytes));
    const PriceSpotQuoteInput no_volume = quote("no-volume", 3, now, 0, false);
    assert(series.RecordSpots("usd", "coingecko", &no_volume, 1, now + 2) ==
           PriceSeriesStoreStatus::kStored);
    PriceSeriesCandle no_volume_candle{};
    size_t no_volume_count = 0;
    const int64_t no_volume_ts = now / 300'000 * 300'000;
    assert(series.Range(PriceSeriesInterval::k5m, "usd", "no-volume",
                        no_volume_ts, no_volume_ts, now + 2, &no_volume_candle,
                        1, &no_volume_count) == PriceSeriesReadStatus::kFound);
    assert(no_volume_count == 1 && !no_volume_candle.has_volume);

    const int64_t upsert_ts = (now - 2 * 3'600'000) / 300'000 * 300'000;
    const PriceSpotQuoteInput open_seed_a = quote("upsert-seal", 4, upsert_ts);
    const PriceSpotQuoteInput open_seed_b =
        quote("upsert-seal", 5, upsert_ts + 300'000);
    assert(series.RecordSpots("usd", "coingecko", &open_seed_a, 1, now) ==
           PriceSeriesStoreStatus::kStored);
    assert(series.RecordSpots("usd", "coingecko", &open_seed_b, 1, now) ==
           PriceSeriesStoreStatus::kStored);
    const PriceSeriesCandleInput seeded[] = {
        {upsert_ts, 4, 6, 3, 5, 42, true},
        {upsert_ts + 300'000, 5, 7, 4, 6, 43, true}};
    assert(series.UpsertCandles(PriceSeriesInterval::k5m, "usd", "upsert-seal",
                                seeded, 2,
                                now) == PriceSeriesStoreStatus::kStored);
    size_t upsert_markers = 0;
    assert(disk.Scan("price/seal/v1/", "price/seal/v1/", 500, now, [&, visitor_state = &upsert_markers](const char* key, const void* value, size_t size) { return (count_upsert_marker)(key, value, size, visitor_state); }) &&
           upsert_markers == 1);
    PriceSeriesAvailability availability{};
    assert(series.Availability(PriceSeriesInterval::k5m, "usd", "upsert-seal",
                               now,
                               &availability) == PriceSeriesReadStatus::kFound);
    assert(availability.oldest == upsert_ts &&
           availability.newest == upsert_ts + 300'000);
    const int64_t exact_segment_end = (now / 3'600'000) * 3'600'000;
    const PriceSeriesCandleInput exact_due{
        exact_segment_end - 3'600'000, 2, 2, 2, 2, 0, false};
    assert(series.UpsertCandles(PriceSeriesInterval::k5m, "usd", "exact-due",
                                &exact_due, 1, exact_segment_end) ==
           PriceSeriesStoreStatus::kStored);
    size_t exact_sealed = 0;
    assert(series.SealDue(exact_segment_end, 200, &exact_sealed) ==
               PriceSeriesStoreStatus::kStored &&
           exact_sealed > 0);
    PriceSeriesCandle exact_result{};
    size_t exact_count = 0;
    assert(series.Range(PriceSeriesInterval::k5m, "usd", "exact-due",
                        exact_due.ts, exact_due.ts, exact_segment_end,
                        &exact_result, 1,
                        &exact_count) == PriceSeriesReadStatus::kFound &&
           exact_count == 1);
    const PriceSeriesCandleInput orphan{
        exact_segment_end, 1, 1, 1, 1, 0, false};
    assert(series.UpsertCandles(PriceSeriesInterval::k5m, "usd", "empty-marker",
                                &orphan, 1, exact_segment_end) ==
           PriceSeriesStoreStatus::kStored);
    char orphan_key[256];
    snprintf(orphan_key, sizeof(orphan_key),
             "price/open/v1/5m/usd/empty-marker/%019lld",
             static_cast<long long>(orphan.ts));
    const KvMutation erase_open{orphan_key, nullptr, 0, 0, true};
    assert(disk.Write(&erase_open, 1));
    size_t orphan_sealed = 0;
    const PriceSeriesStoreStatus orphan_status =
        series.SealDue(exact_segment_end + 3'600'000, 200, &orphan_sealed);
    assert(orphan_status == PriceSeriesStoreStatus::kStored ||
           orphan_status == PriceSeriesStoreStatus::kNoItems);
    size_t orphan_markers = 0;
    assert(disk.Scan("price/seal/v1/", "price/seal/v1/", 500, exact_segment_end + 3'600'000, [&, visitor_state = &orphan_markers](const char* key, const void* value, size_t size) { return (count_orphan_marker)(key, value, size, visitor_state); }) &&
           orphan_markers == 0);
    size_t sealed = 0;
    assert(series.SealDue(now, 200, &sealed) ==
               PriceSeriesStoreStatus::kNoItems &&
           sealed == 0);
    PriceSeriesCandle second_seed{};
    size_t second_seed_count = 0;
    assert(series.Range(PriceSeriesInterval::k5m, "usd", "upsert-seal",
                        upsert_ts + 300'000, upsert_ts + 300'000, now,
                        &second_seed, 1,
                        &second_seed_count) == PriceSeriesReadStatus::kFound &&
           second_seed_count == 1 && second_seed.close == 6);
    PriceSeriesCandle packed_candle{};
    size_t packed_count = 0;
    assert(series.Range(PriceSeriesInterval::k5m, "usd", "upsert-seal",
                        upsert_ts, upsert_ts, now, &packed_candle, 1,
                        &packed_count) == PriceSeriesReadStatus::kFound &&
           packed_count == 1);
    assert(packed_candle.open == 4 && packed_candle.high == 6 &&
           packed_candle.low == 3 && packed_candle.close == 5 &&
           packed_candle.volume == 42 && packed_candle.has_volume);
    const PriceSeriesCandleInput packed_backfill{
        upsert_ts - 3'600'000, 7, 9, 6, 8, 0, false};
    assert(series.UpsertCandles(PriceSeriesInterval::k5m, "usd", "upsert-seal",
                                &packed_backfill, 1,
                                now) == PriceSeriesStoreStatus::kStored);
    PriceSeriesCandle backfill{};
    size_t backfill_count = 0;
    assert(series.Range(PriceSeriesInterval::k5m, "usd", "upsert-seal",
                        upsert_ts - 3'600'000, upsert_ts - 3'600'000, now,
                        &backfill, 1,
                        &backfill_count) == PriceSeriesReadStatus::kFound &&
           backfill_count == 1 && backfill.close == 8 && !backfill.has_volume);
    const PriceSeriesCandleInput replace_packed{upsert_ts, 4.5, 7,    2.5,
                                                6.5,       0,   false};
    assert(series.UpsertCandles(PriceSeriesInterval::k5m, "usd", "upsert-seal",
                                &replace_packed, 1,
                                now) == PriceSeriesStoreStatus::kStored);
    assert(series.Range(PriceSeriesInterval::k5m, "usd", "upsert-seal",
                        upsert_ts, upsert_ts, now, &packed_candle, 1,
                        &packed_count) == PriceSeriesReadStatus::kFound);
    assert(packed_candle.close == 6.5 && packed_candle.volume == 42 &&
           packed_candle.has_volume);

    PriceSpotQuoteInput batch[100]{};
    char batch_ids[100][20]{};
    for (size_t i = 0; i < 100; ++i) {
      snprintf(batch_ids[i], sizeof(batch_ids[i]), "batch-%03zu", i);
      batch[i] = quote(batch_ids[i], static_cast<double>(i + 1), now + 3);
    }
    assert(stat(path, &before) == 0);
    assert(series.RecordSpots("usd", "coingecko", batch, 100, now + 3) ==
           PriceSeriesStoreStatus::kStored);
    assert(stat(path, &after) == 0 && after.st_size > before.st_size);
    const int batch_fd = open(path, O_RDONLY);
    assert(batch_fd >= 0);
    uint8_t batch_frame[16]{};
    assert(pread(batch_fd, batch_frame, sizeof(batch_frame), before.st_size) ==
           sizeof(batch_frame));
    close(batch_fd);
    assert(memcmp(batch_frame, "FKV1", 4) == 0);
    const uint32_t mutations = static_cast<uint32_t>(batch_frame[12]) |
                               (static_cast<uint32_t>(batch_frame[13]) << 8) |
                               (static_cast<uint32_t>(batch_frame[14]) << 16) |
                               (static_cast<uint32_t>(batch_frame[15]) << 24);
    assert(mutations == 700);
    const uint32_t batch_bytes = static_cast<uint32_t>(batch_frame[4]) |
                                 (static_cast<uint32_t>(batch_frame[5]) << 8) |
                                 (static_cast<uint32_t>(batch_frame[6]) << 16) |
                                 (static_cast<uint32_t>(batch_frame[7]) << 24);
    assert(after.st_size - before.st_size ==
           static_cast<off_t>(12 + batch_bytes));

    constexpr int64_t kHour = 3'600'000;
    const int64_t history_start = kT0 - 48 * kHour;
    for (int64_t i = 0; i <= 48; ++i) {
      const int64_t at = history_start + i * kHour;
      const PriceSpotQuoteInput historical =
          quote("range-heavy", static_cast<double>(i + 1), at);
      assert(series.RecordSpots("usd", "coingecko", &historical, 1, at + 1) ==
             PriceSeriesStoreStatus::kStored);
    }
    const int64_t latest_hour = kT0 / kHour * kHour;
    PriceSeriesCandle narrow[2]{};
    size_t narrow_count = 0;
    assert(series.Range(PriceSeriesInterval::k1h, "usd", "range-heavy",
                        latest_hour, latest_hour, kT0 + 2, narrow, 2,
                        &narrow_count) == PriceSeriesReadStatus::kFound);
    assert(narrow_count == 1 && narrow[0].ts == latest_hour &&
           narrow[0].close == 49);

    constexpr int64_t kFixtureBackfillTs = 1'792'058'400'000LL;
    constexpr int64_t kFixtureBackfillNow = 1'792'062'010'000LL;
    const PriceSeriesCandleInput fixture_backfill{
        kFixtureBackfillTs, 10, 15, 9, 12, 500, true};
    assert(series.UpsertCandles(PriceSeriesInterval::k1h, "usd",
                                "fixture-backfill", &fixture_backfill, 1,
                                kFixtureBackfillNow) ==
           PriceSeriesStoreStatus::kStored);
    PriceSeriesCandle fixture_value{};
    size_t fixture_count = 0;
    assert(series.Range(PriceSeriesInterval::k1h, "usd", "fixture-backfill",
                        kFixtureBackfillTs, kFixtureBackfillTs,
                        kFixtureBackfillNow, &fixture_value, 1,
                        &fixture_count) == PriceSeriesReadStatus::kFound &&
           fixture_count == 1 && fixture_value.open == 10 &&
           fixture_value.high == 15 && fixture_value.low == 9 &&
           fixture_value.close == 12 && fixture_value.volume == 500 &&
           fixture_value.first_at_ms == kFixtureBackfillTs &&
           fixture_value.last_at_ms == kFixtureBackfillTs);
    PriceSeriesAvailability fixture_availability{};
    assert(series.Availability(PriceSeriesInterval::k1h, "usd",
                               "fixture-backfill", kFixtureBackfillNow,
                               &fixture_availability) ==
               PriceSeriesReadStatus::kFound &&
           fixture_availability.oldest == kFixtureBackfillTs &&
           fixture_availability.newest == kFixtureBackfillTs);
    const int64_t fixture_day = kFixtureBackfillTs / 86'400'000 * 86'400'000;
    const PriceSeriesCandleInput old_day{
        fixture_day - 200 * 86'400'000LL, 1, 1, 1, 1, 0, false};
    assert(series.UpsertCandles(
               PriceSeriesInterval::k1d, "usd", "availability-edges", &old_day,
               1, kFixtureBackfillNow) == PriceSeriesStoreStatus::kStored);
    const PriceSpotQuoteInput newest_day =
        quote("availability-edges", 2, fixture_day + 60'000);
    assert(series.RecordSpots("usd", "coingecko", &newest_day, 1,
                              kFixtureBackfillNow) ==
           PriceSeriesStoreStatus::kStored);
    PriceSeriesAvailability edge_availability{};
    assert(series.Availability(PriceSeriesInterval::k1d, "usd",
                               "availability-edges", kFixtureBackfillNow,
                               &edge_availability) ==
               PriceSeriesReadStatus::kFound &&
           edge_availability.oldest == old_day.ts &&
           edge_availability.newest == fixture_day);

    const PriceSpotQuoteInput seal_failure_quote =
        quote("seal-failed-write", 2, late);
    assert(series.RecordSpots("usd", "coingecko", &seal_failure_quote, 1,
                              now) == PriceSeriesStoreStatus::kStored);
    size_t failed_sealed = 0;
    assert(setenv("FORTIS_KV_TEST_FAIL", "write_after_header", 1) == 0);
    assert(series.SealDue(now, 200, &failed_sealed) ==
           PriceSeriesStoreStatus::kWriteFailure);
    assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
    assert(series.GetSpot("usd", "seal-failed-write", now, &spot) ==
           PriceSeriesReadStatus::kIoError);
    disk.Close();
    assert(disk.Open(directory));
    assert(series.GetSpot("usd", "seal-failed-write", now, &spot) ==
           PriceSeriesReadStatus::kFound);
    PriceSeriesCandle unsealed{};
    size_t unsealed_count = 0;
    const int64_t unsealed_ts = late / 300'000 * 300'000;
    assert(series.Range(PriceSeriesInterval::k5m, "usd", "seal-failed-write",
                        unsealed_ts, unsealed_ts, now, &unsealed, 1,
                        &unsealed_count) == PriceSeriesReadStatus::kFound &&
           unsealed_count == 1 && unsealed.close == 2);

    const PriceSpotQuoteInput failure = quote("failed-write", 2, now);
    assert(setenv("FORTIS_KV_TEST_FAIL", "write_after_header", 1) == 0);
    assert(series.RecordSpots("usd", "coingecko", &failure, 1, now + 2) ==
           PriceSeriesStoreStatus::kWriteFailure);
    assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
    assert(series.GetSpot("usd", "failed-write", now + 2, &spot) ==
           PriceSeriesReadStatus::kIoError);
    disk.Close();
    assert(disk.Open(directory));
    assert(series.GetSpot("usd", "ethereum", now, &spot) ==
               PriceSeriesReadStatus::kFound &&
           spot.price == 12);
    assert(series.GetSpot("usd", "failed-write", now + 2, &spot) ==
           PriceSeriesReadStatus::kMissing);
    expect_range(series, PriceSeriesInterval::k5m, ts5[0], ts5[2], ts5, close5,
                 3, now);
    expect_range(series, PriceSeriesInterval::k1h, ts1h[0], ts1h[1], ts1h,
                 close1h, 2, now);
    expect_range(series, PriceSeriesInterval::k1d, ts1d[0], ts1d[0], ts1d,
                 close1d, 1, now);
  }
  char partial_directory[] = "/tmp/fortis-price-series-partial-XXXXXX";
  assert(mkdtemp(partial_directory));
  {
    KvStore partial_disk;
    assert(partial_disk.Open(partial_directory));
    PriceSeriesStore partial_series(partial_disk);
    constexpr int64_t kHour = 3'600'000;
    constexpr int64_t kWeek = 7 * 86'400'000LL;
    const int64_t week_start = now / kWeek * kWeek;
    const int64_t first_ts = week_start - 3 * kWeek + 68 * kHour;
    for (size_t i = 0; i < 268; ++i) {
      const PriceSpotQuoteInput item =
          quote("seal-partial", static_cast<double>(i + 1),
                first_ts + static_cast<int64_t>(i) * kHour);
      assert(partial_series.RecordSpots("usd", "coingecko", &item, 1, now) ==
             PriceSeriesStoreStatus::kStored);
    }

    auto *keys = static_cast<char(*)[600]>(calloc(1000, 600));
    auto *mutations =
        static_cast<KvMutation *>(calloc(1000, sizeof(KvMutation)));
    assert(keys && mutations);
    KeyCollector collector{keys, 0, 1000, nullptr};
    const char hour_prefix[] = "price/open/v1/1h/usd/seal-partial/";
    assert(partial_disk.Scan(hour_prefix, hour_prefix, 500, now, [&, visitor_state = &collector](const char* key, const void* value, size_t size) { return (collect_matching_key)(key, value, size, visitor_state); }));
    assert(collector.count == 268);

    collector.count = 0;
    const char five_prefix[] = "price/open/v1/5m/usd/seal-partial/";
    assert(partial_disk.Scan(five_prefix, five_prefix, 500, now, [&, visitor_state = &collector](const char* key, const void* value, size_t size) { return (collect_matching_key)(key, value, size, visitor_state); }));
    const char daily_prefix[] = "price/open/v1/1d/usd/seal-partial/";
    assert(partial_disk.Scan(daily_prefix, daily_prefix, 500, now, [&, visitor_state = &collector](const char* key, const void* value, size_t size) { return (collect_matching_key)(key, value, size, visitor_state); }));
    collector.needle = "/5m/usd/seal-partial/";
    assert(partial_disk.Scan("price/seal/v1/", "price/seal/v1/", 1000, now, [&, visitor_state = &collector](const char* key, const void* value, size_t size) { return (collect_matching_key)(key, value, size, visitor_state); }));
    collector.needle = "/1d/usd/seal-partial/";
    assert(partial_disk.Scan("price/seal/v1/", "price/seal/v1/", 1000, now, [&, visitor_state = &collector](const char* key, const void* value, size_t size) { return (collect_matching_key)(key, value, size, visitor_state); }));
    assert(collector.count < 1000);
    for (size_t i = 0; i < collector.count; ++i)
      mutations[i] = {keys[i], nullptr, 0, 0, true};
    assert(partial_disk.Write(mutations, collector.count));
    free(mutations);
    free(keys);

    size_t first_sealed = 0;
    assert(partial_series.SealDue(now, 200, &first_sealed) ==
               PriceSeriesStoreStatus::kStored &&
           first_sealed == 200);
    PriceSeriesCandle partial_range[268]{};
    size_t partial_count = 0;
    assert(partial_series.Range(PriceSeriesInterval::k1h, "usd", "seal-partial",
                                first_ts, first_ts + 267 * kHour, now,
                                partial_range, 268, &partial_count) ==
               PriceSeriesReadStatus::kFound &&
           partial_count == 268);
    size_t remaining_sealed = 0;
    assert(partial_series.SealDue(now, 200, &remaining_sealed) ==
               PriceSeriesStoreStatus::kStored &&
           remaining_sealed == 68);
    auto *remaining_keys = static_cast<char(*)[600]>(calloc(1000, 600));
    assert(remaining_keys);
    KeyCollector remaining{remaining_keys, 0, 1000, nullptr};
    assert(partial_disk.Scan(hour_prefix, hour_prefix, 500, now, [&, visitor_state = &remaining](const char* key, const void* value, size_t size) { return (collect_matching_key)(key, value, size, visitor_state); }));
    assert(remaining.count == 0);
    free(remaining_keys);
    partial_disk.Close();
  }
  remove_store(partial_directory);
  remove_store(directory);
  puts("native bounded price-series open-bucket persistence, range, restart, "
       "write bound and failure tests passed");
}
