#include "../src/prices/price_series_store.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

using namespace fortis::prices;
using namespace fortis::storage;

using namespace fortis;
namespace {
constexpr int64_t kDay = 86'400'000LL;
int64_t floor_to(int64_t value, int64_t width) { return value / width * width; }
int64_t month_start(int year, int month) {
  struct tm t{};
  t.tm_year = year - 1900;
  t.tm_mon = month;
  t.tm_mday = 1;
  return static_cast<int64_t>(timegm(&t)) * 1000;
}
} // namespace
int main(int argc, char **argv) {
  const size_t sources =
      argc > 1 ? static_cast<size_t>(strtoul(argv[1], nullptr, 10)) : 10;
  if (!sources || sources > 1000) {
    fprintf(stderr, "source count must be 1..1000\n");
    return 2;
  }
  char directory[] = "/tmp/fortis-series-bench-XXXXXX";
  if (!mkdtemp(directory))
    return 2;
  KvStore disk;
  if (!disk.Open(directory))
    return 2;
  PriceSeriesStore series(disk);
  const int64_t wall_now = static_cast<int64_t>(time(nullptr)) * 1000;
  const int64_t now = floor_to(wall_now, 7 * kDay) + 7 * kDay;
  PriceSeriesCandleInput five[2016], hour[2160], day[366];
  const int64_t recent5 = floor_to(now, 3'600'000) - 300'000;
  for (size_t i = 0; i < 2016; ++i) {
    int64_t ts = recent5 - static_cast<int64_t>(i) * 300'000;
    five[i] = {ts, 100, 101, 99, 100, 0, false};
  }
  const int64_t recent1h = floor_to(now - 3'600'000, 3'600'000);
  for (size_t i = 0; i < 2160; ++i) {
    int64_t ts = recent1h - static_cast<int64_t>(i) * 3'600'000;
    hour[i] = {ts, 100, 101, 99, 100, 0, false};
  }
  struct tm current{};
  time_t current_sec = static_cast<time_t>(now / 1000);
  gmtime_r(&current_sec, &current);
  const int64_t recent_day =
      month_start(current.tm_year + 1900, current.tm_mon);
  for (size_t i = 0; i < 366; ++i) {
    int64_t ts = recent_day - static_cast<int64_t>(i) * kDay;
    day[i] = {ts, 100, 101, 99, 100, 0, false};
  }
  for (size_t s = 0; s < sources; ++s) {
    char id[32];
    snprintf(id, sizeof(id), "bench-%04zu", s);
    for (size_t offset = 0; offset < 2016;
         offset += kPriceSeriesMaxUpsertItems) {
      const size_t n = 2016 - offset < kPriceSeriesMaxUpsertItems
                           ? 2016 - offset
                           : kPriceSeriesMaxUpsertItems;
      if (series.UpsertCandles(PriceSeriesInterval::k5m, "usd", id,
                               five + offset, n,
                               now) != PriceSeriesStoreStatus::kStored) {
        fprintf(stderr, "5m upsert failed for %s\n", id);
        return 1;
      }
    }
    for (size_t offset = 0; offset < 2160;
         offset += kPriceSeriesMaxUpsertItems) {
      const size_t n = 2160 - offset < kPriceSeriesMaxUpsertItems
                           ? 2160 - offset
                           : kPriceSeriesMaxUpsertItems;
      if (series.UpsertCandles(PriceSeriesInterval::k1h, "usd", id,
                               hour + offset, n,
                               now) != PriceSeriesStoreStatus::kStored) {
        fprintf(stderr, "1h upsert failed for %s\n", id);
        return 1;
      }
    }
    for (size_t offset = 0; offset < 366;
         offset += kPriceSeriesMaxUpsertItems) {
      const size_t n = 366 - offset < kPriceSeriesMaxUpsertItems
                           ? 366 - offset
                           : kPriceSeriesMaxUpsertItems;
      if (series.UpsertCandles(PriceSeriesInterval::k1d, "usd", id,
                               day + offset, n,
                               now) != PriceSeriesStoreStatus::kStored) {
        fprintf(stderr, "1d upsert failed for %s\n", id);
        return 1;
      }
    }
  }
  char path[512];
  snprintf(path, sizeof(path), "%s/fortis.kv", directory);
  struct stat st{};
  if (stat(path, &st))
    return 2;
  struct rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
  const long rss_kib = usage.ru_maxrss / 1024;
#else
  const long rss_kib = usage.ru_maxrss;
#endif
  printf("sources=%zu loose_rows_per_source=4542 segments_per_source=194 "
         "keys=%zu kv_log_bytes=%lld max_rss_kib=%ld\n",
         sources, disk.Size(), static_cast<long long>(st.st_size), rss_kib);
  disk.Close();
  unlink(path);
  snprintf(path, sizeof(path), "%s/fortis.kv.lock", directory);
  unlink(path);
  rmdir(directory);
  return 0;
}
