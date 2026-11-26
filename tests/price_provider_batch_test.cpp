#include "../src/storage/kv_store.h"
#include "../src/prices/price_series_store.h"
#include "../src/prices/price_spot_cache.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

using namespace fortis::prices;
using namespace fortis::storage;

namespace {
using namespace fortis;

void remove_store(const char* directory) {
  char path[512];
  snprintf(path, sizeof(path), "%s/fortis.kv", directory);
  assert(unlink(path) == 0);
  snprintf(path, sizeof(path), "%s/fortis.kv.lock", directory);
  assert(unlink(path) == 0);
  assert(rmdir(directory) == 0);
}

size_t frame_count(const char* directory, uint32_t* mutations = nullptr) {
  char path[512];
  snprintf(path, sizeof(path), "%s/fortis.kv", directory);
  FILE* file = fopen(path, "rb");
  assert(file);
  size_t frames = 0;
  unsigned char header[12];
  while (fread(header, 1, sizeof(header), file) == sizeof(header)) {
    assert(memcmp(header, "FKV1", 4) == 0);
    const uint32_t body_size = static_cast<uint32_t>(header[4]) |
        (static_cast<uint32_t>(header[5]) << 8) |
        (static_cast<uint32_t>(header[6]) << 16) |
        (static_cast<uint32_t>(header[7]) << 24);
    unsigned char count_bytes[4]{};
    assert(body_size >= sizeof(count_bytes));
    assert(fread(count_bytes, 1, sizeof(count_bytes), file) == sizeof(count_bytes));
    const uint32_t count = static_cast<uint32_t>(count_bytes[0]) |
        (static_cast<uint32_t>(count_bytes[1]) << 8) |
        (static_cast<uint32_t>(count_bytes[2]) << 16) |
        (static_cast<uint32_t>(count_bytes[3]) << 24);
    if (mutations) *mutations = count;
    assert(fseek(file, static_cast<long>(body_size - sizeof(count_bytes)), SEEK_CUR) == 0);
    ++frames;
  }
  assert(feof(file));
  fclose(file);
  return frames;
}

PriceSpotQuoteInput make_quote(const char* id, double price) {
  return {id, price, 1.0, 12.0, true, true, 9'000'000, 0};
}

void test_maximum_frame_bound() {
  char directory[] = "/tmp/fortis-provider-batch-max-XXXXXX";
  assert(mkdtemp(directory));
  {
    KvStore store;
    assert(store.Open(directory));
    PriceSpotCache cache(store);
    PriceSeriesStore series(store);
    static char ids[kPriceSpotMaxBatchItems][24];
    PriceSpotQuoteInput quotes[kPriceSpotMaxBatchItems]{};
    for (size_t i = 0; i < kPriceSpotMaxBatchItems; ++i) {
      snprintf(ids[i], sizeof(ids[i]), "coin-%03zu", i);
      quotes[i] = make_quote(ids[i], static_cast<double>(i + 1));
      quotes[i].provider_observed_at_ms = 8'000'000;
    }
    PriceSeriesStoreStatus series_status = PriceSeriesStoreStatus::kWriteFailure;
    assert(cache.StoreProviderBatchWithSeries(series, "usd", "coingecko",
        quotes, kPriceSpotMaxBatchItems, nullptr, 0, true, 10'000'000,
        &series_status) == PriceSpotStoreStatus::kStored);
    assert(series_status == PriceSeriesStoreStatus::kStored);
    uint32_t mutation_count = 0;
    assert(frame_count(directory, &mutation_count) == 1);
    assert(mutation_count == kPriceProviderBatchMaxMutations);
    assert(mutation_count == 900);
  }
  remove_store(directory);
}

void test_torn_and_uncertain_commit(const char* stage, bool expect_committed) {
  char directory[] = "/tmp/fortis-provider-batch-fault-XXXXXX";
  assert(mkdtemp(directory));
  {
    KvStore store;
    assert(store.Open(directory));
    PriceSpotCache cache(store);
    PriceSeriesStore series(store);
    PriceSpotQuoteInput quote = make_quote("fault-coin", 7.5);
    const char* unknown[] = {"unknown-coin"};
    PriceSeriesStoreStatus series_status = PriceSeriesStoreStatus::kNoItems;
    assert(setenv("FORTIS_KV_TEST_FAIL", stage, 1) == 0);
    const PriceSpotStoreStatus status = cache.StoreProviderBatchWithSeries(
        series, "usd", "coingecko", &quote, 1, unknown, 1, true,
        10'000'000, &series_status);
    assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
    assert(status == PriceSpotStoreStatus::kWriteFailure);
    assert(series_status == PriceSeriesStoreStatus::kWriteFailure);
  }
  {
    KvStore recovered;
    assert(recovered.Open(directory));
    PriceSpotCache cache(recovered);
    PriceSeriesStore series(recovered);
    PriceSeriesSpot spot{};
    const auto lookup = cache.Lookup("usd", "fault-coin", 10'000'001);
    const auto series_status = series.GetSpot("usd", "fault-coin", 10'000'001, &spot);
    const auto unknown_lookup = cache.Lookup("usd", "unknown-coin", 10'000'001);
    const bool all_present = lookup.state == PriceSpotLookupState::kFresh &&
        series_status == PriceSeriesReadStatus::kFound && spot.price == 7.5 &&
        unknown_lookup.state == PriceSpotLookupState::kNegative;
    const bool all_absent = lookup.state == PriceSpotLookupState::kMissing &&
        series_status == PriceSeriesReadStatus::kMissing &&
        unknown_lookup.state == PriceSpotLookupState::kMissing;
    assert(expect_committed ? all_present : all_absent);
    assert(frame_count(directory) == (expect_committed ? 1u : 0u));
  }
  remove_store(directory);
}

void test_unknown_has_no_series_row() {
  char directory[] = "/tmp/fortis-provider-batch-unknown-XXXXXX";
  assert(mkdtemp(directory));
  {
    KvStore store;
    assert(store.Open(directory));
    PriceSpotCache cache(store);
    PriceSeriesStore series(store);
    const char* unknown[] = {"unknown-only"};
    PriceSeriesStoreStatus series_status = PriceSeriesStoreStatus::kWriteFailure;
    assert(cache.StoreProviderBatchWithSeries(series, "usd", "coingecko",
        nullptr, 0, unknown, 1, true, 10'000'000, &series_status) ==
        PriceSpotStoreStatus::kStored);
    assert(series_status == PriceSeriesStoreStatus::kStored);
    assert(cache.Lookup("usd", "unknown-only", 10'000'001).state ==
           PriceSpotLookupState::kNegative);
    PriceSeriesSpot spot{};
    assert(series.GetSpot("usd", "unknown-only", 10'000'001, &spot) ==
           PriceSeriesReadStatus::kMissing);
    assert(frame_count(directory) == 1);
  }
  remove_store(directory);
}

void test_100_ids_with_negatives() {
  char directory[] = "/tmp/fortis-provider-batch-mixed-XXXXXX";
  assert(mkdtemp(directory));
  {
    KvStore store;
    assert(store.Open(directory));
    PriceSpotCache cache(store);
    PriceSeriesStore series(store);
    static char ids[kPriceSpotMaxBatchItems][24];
    PriceSpotQuoteInput quotes[kPriceSpotMaxBatchItems / 2]{};
    const char* unknown[kPriceSpotMaxBatchItems / 2]{};
    for (size_t i = 0; i < kPriceSpotMaxBatchItems / 2; ++i) {
      snprintf(ids[i], sizeof(ids[i]), "known-%03zu", i);
      quotes[i] = make_quote(ids[i], static_cast<double>(i + 1));
      snprintf(ids[i + kPriceSpotMaxBatchItems / 2], sizeof(ids[0]),
               "unknown-%03zu", i);
      unknown[i] = ids[i + kPriceSpotMaxBatchItems / 2];
    }
    PriceSeriesStoreStatus series_status = PriceSeriesStoreStatus::kWriteFailure;
    assert(cache.StoreProviderBatchWithSeries(series, "usd", "coingecko",
        quotes, kPriceSpotMaxBatchItems / 2, unknown,
        kPriceSpotMaxBatchItems / 2, true, 10'000'000,
        &series_status) == PriceSpotStoreStatus::kStored);
    assert(series_status == PriceSeriesStoreStatus::kStored);
    uint32_t mutation_count = 0;
    assert(frame_count(directory, &mutation_count) == 1);
    assert(mutation_count == 450);
    PriceSeriesSpot spot{};
    assert(series.GetSpot("usd", unknown[0], 10'000'001, &spot) ==
           PriceSeriesReadStatus::kMissing);
  }
  remove_store(directory);
}
}  // namespace

int main() {
  static_assert(kPriceProviderBatchMaxMutations == 900,
                "provider frame mutation budget changed");
  test_maximum_frame_bound();
  test_100_ids_with_negatives();
  test_unknown_has_no_series_row();
  test_torn_and_uncertain_commit("write_after_header", false);
  test_torn_and_uncertain_commit("write_after_fsync", true);
  puts("native atomic provider cache/series frame, fault recovery and bounds passed");
}
