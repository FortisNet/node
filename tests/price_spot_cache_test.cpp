#include "../src/prices/price_spot_cache.h"

#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace fortis::prices;
using namespace fortis::storage;

namespace {
using namespace fortis;

uint32_t read32(const uint8_t* bytes) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(bytes[i]) << (i * 8);
  return value;
}

PriceSpotQuoteInput quote(const char* id, double price, int64_t as_of,
                          uint32_t fresh_ms = 0) {
  return {id, price, -1.25, 0, true, false, as_of, fresh_ms};
}

void remove_store_files(const char* directory) {
  char path[768];
  snprintf(path, sizeof(path), "%s/fortis.kv", directory);
  assert(unlink(path) == 0);
  snprintf(path, sizeof(path), "%s/fortis.kv.lock", directory);
  assert(unlink(path) == 0);
  assert(rmdir(directory) == 0);
}

void test_fresh_stale_negative_floor_restart_and_bounds() {
  char directory[] = "/tmp/fortis-price-cache-XXXXXX";
  assert(mkdtemp(directory));
  const int64_t now = 1'700'000'000'000LL;
  {
    KvStore store;
    assert(store.Open(directory));
    PriceSpotCache cache(store);
    assert(cache.Lookup("usd", "bitcoin", now).state == PriceSpotLookupState::kMissing);
    assert(cache.Lookup("USD", "bitcoin", now).state == PriceSpotLookupState::kInvalidKey);
    assert(cache.Lookup("usd", "", now).state == PriceSpotLookupState::kInvalidKey);
    assert(cache.Lookup("usd", "bad/id", now).state == PriceSpotLookupState::kInvalidKey);
    char overlong_id[kPriceSpotMaxSourceIdBytes + 2];
    memset(overlong_id, 'z', sizeof(overlong_id) - 1);
    overlong_id[sizeof(overlong_id) - 1] = '\0';
    assert(cache.Lookup("usd", overlong_id, now).state == PriceSpotLookupState::kInvalidKey);

    const PriceSpotQuoteInput first = quote("bitcoin", 123.5, now);
    assert(cache.StoreProviderBatch("usd", "coingecko", &first, 1, nullptr, 0,
                                   true, now) == PriceSpotStoreStatus::kStored);
    PriceSpotLookup result = cache.Lookup("usd", "bitcoin", now);
    assert(result.state == PriceSpotLookupState::kFresh && result.cache_hit &&
           !result.stale && !result.revalidate_needed &&
           strcmp(result.value.provider, "coingecko") == 0 &&
           result.value.price == 123.5 && result.value.has_change24h_pct &&
           result.value.change24h_pct == -1.25 && !result.value.has_volume24h &&
           result.value.as_of_ms == now);
    result = cache.Lookup("usd", "bitcoin", now + 29'999);
    assert(result.state == PriceSpotLookupState::kFresh);
    result = cache.Lookup("usd", "bitcoin", now + 30'000);
    assert(result.state == PriceSpotLookupState::kStale && result.cache_hit &&
           result.stale && result.revalidate_needed && result.has_floor_candidate);
    result = cache.Lookup("usd", "bitcoin", now + 630'000);
    assert(result.state == PriceSpotLookupState::kExpired && !result.cache_hit &&
           result.revalidate_needed && result.has_floor_candidate &&
           result.value.price == 123.5 && result.value.as_of_ms == now);

    struct stat before {};
    char path[256];
    snprintf(path, sizeof(path), "%s/fortis.kv", directory);
    assert(stat(path, &before) == 0);
    const PriceSpotQuoteInput failed_quote = quote("bitcoin", 999, now + 1000);
    assert(cache.StoreProviderBatch("usd", "coingecko", &failed_quote, 1,
                                   nullptr, 0, false, now + 630'001) ==
           PriceSpotStoreStatus::kTransportFailureNoWrite);
    struct stat after_failure {};
    assert(stat(path, &after_failure) == 0 && before.st_size == after_failure.st_size);
    result = cache.Lookup("usd", "bitcoin", now + 630'001);
    assert(result.state == PriceSpotLookupState::kExpired && result.value.price == 123.5);

    store.Close();
    assert(store.Open(directory));
    result = cache.Lookup("usd", "bitcoin", now + 30'001);
    assert(result.state == PriceSpotLookupState::kStale && result.value.price == 123.5);

    const char* unknown[] = {"bitcoin"};
    const int64_t negative_at = now + 800'000;
    assert(cache.StoreProviderBatch("usd", "coingecko", nullptr, 0, unknown, 1,
                                   true, negative_at) == PriceSpotStoreStatus::kStored);
    result = cache.Lookup("usd", "bitcoin", negative_at);
    assert(result.state == PriceSpotLookupState::kNegative &&
           !result.has_floor_candidate && !result.revalidate_needed &&
           strcmp(result.value.provider, "coingecko") == 0 &&
           result.value.fresh_until_ms == negative_at + kPriceSpotNegativeMs);
    result = cache.Lookup("usd", "bitcoin", negative_at + kPriceSpotNegativeMs - 1);
    assert(result.state == PriceSpotLookupState::kNegative && !result.has_floor_candidate);
    result = cache.Lookup("usd", "bitcoin", negative_at + kPriceSpotNegativeMs);
    assert(result.state == PriceSpotLookupState::kExpired && !result.cache_hit &&
           result.has_floor_candidate && result.value.price == 123.5 &&
           result.value.as_of_ms == now);

    const char* new_unknown[] = {"unlisted"};
    assert(cache.StoreProviderBatch("usd", "coingecko", nullptr, 0, new_unknown, 1,
                                   true, now) == PriceSpotStoreStatus::kStored);
    assert(cache.Lookup("usd", "unlisted", now).state == PriceSpotLookupState::kNegative);
    assert(cache.Lookup("usd", "unlisted", now + kPriceSpotNegativeMs).state ==
           PriceSpotLookupState::kMissing);

    const PriceSpotQuoteInput different_fiat = quote("bitcoin", 124, now + 5);
    assert(cache.StoreProviderBatch("eur", "coingecko", &different_fiat, 1,
                                   nullptr, 0, true, now + 5) == PriceSpotStoreStatus::kStored);
    assert(cache.Lookup("eur", "bitcoin", now + 5).value.price == 124);
    assert(cache.Lookup("usd", "BITCOIN", now + 5).state == PriceSpotLookupState::kMissing);

    char max_id[kPriceSpotMaxSourceIdBytes + 1];
    memset(max_id, 'x', sizeof(max_id) - 1);
    max_id[sizeof(max_id) - 1] = '\0';
    const PriceSpotQuoteInput boundary = quote(max_id, 1, now, kPriceSpotMaxFreshMs);
    assert(cache.StoreProviderBatch("usd", "coingecko", &boundary, 1, nullptr, 0,
                                   true, now) == PriceSpotStoreStatus::kStored);
    result = cache.Lookup("usd", max_id, now);
    assert(result.state == PriceSpotLookupState::kFresh &&
           result.value.fresh_until_ms == now + kPriceSpotMaxFreshMs);
    const PriceSpotQuoteInput too_fresh = quote(max_id, 1, now, kPriceSpotMaxFreshMs + 1);
    assert(cache.StoreProviderBatch("usd", "coingecko", &too_fresh, 1, nullptr, 0,
                                   true, now) == PriceSpotStoreStatus::kInvalidArguments);
    const PriceSpotQuoteInput too_long_id = quote(overlong_id, 1, now);
    assert(cache.StoreProviderBatch("usd", "coingecko", &too_long_id, 1, nullptr, 0,
                                   true, now) == PriceSpotStoreStatus::kInvalidArguments);
    const PriceSpotQuoteInput bad_id = quote(overlong_id, 1, now);
    assert(cache.StoreProviderBatch("usd", "coingecko", &bad_id, 1, nullptr, 0,
                                   true, now) == PriceSpotStoreStatus::kInvalidArguments);
    const PriceSpotQuoteInput nonfinite = quote("nonfinite", NAN, now);
    assert(cache.StoreProviderBatch("usd", "coingecko", &nonfinite, 1, nullptr, 0,
                                   true, now) == PriceSpotStoreStatus::kInvalidArguments);
    const PriceSpotQuoteInput future = quote("future", 1, now + 1);
    assert(cache.StoreProviderBatch("usd", "coingecko", &future, 1, nullptr, 0,
                                   true, now) == PriceSpotStoreStatus::kInvalidArguments);

    result = cache.Lookup("usd", "bitcoin", now + kPriceSpotFloorMs);
    assert(result.state == PriceSpotLookupState::kMissing && !result.has_floor_candidate);
  }
  remove_store_files(directory);
}

void test_monotonic_floor_and_warm_ttl() {
  char directory[] = "/tmp/fortis-price-monotonic-XXXXXX";
  assert(mkdtemp(directory));
  {
    KvStore store;
    assert(store.Open(directory));
    PriceSpotCache cache(store);
    const int64_t t = 10'000'000;
    const PriceSpotQuoteInput newer = quote("coin", 10, t);
    assert(cache.StoreProviderBatch("usd", "coingecko", &newer, 1, nullptr, 0,
                                   true, t) == PriceSpotStoreStatus::kStored);
    const PriceSpotQuoteInput older = quote("coin", 9, t - 1);
    assert(cache.StoreProviderBatch("usd", "coingecko", &older, 1, nullptr, 0,
                                   true, t + 1) == PriceSpotStoreStatus::kStored);
    PriceSpotLookup result = cache.Lookup("usd", "coin", t + 1);
    assert(result.state == PriceSpotLookupState::kFresh && result.value.price == 9 &&
           result.value.as_of_ms == t - 1);
    result = cache.Lookup("usd", "coin", t + 1 + kPriceSpotMaxFreshMs +
                          kPriceSpotStaleMs);
    assert(result.state == PriceSpotLookupState::kExpired &&
           result.has_floor_candidate && result.value.price == 10 &&
           result.value.as_of_ms == t);

    // 60-minute warmer cadence plus the TypeScript coordinator's 10% jitter.
    const PriceSpotQuoteInput warm = quote("warm", 2, t, 3'960'000);
    assert(cache.StoreProviderBatch("usd", "coingecko", &warm, 1, nullptr, 0,
                                   true, t) == PriceSpotStoreStatus::kStored);
    result = cache.Lookup("usd", "warm", t);
    assert(result.state == PriceSpotLookupState::kFresh &&
           result.value.fresh_until_ms == t + 3'960'000);
  }
  remove_store_files(directory);
}

void test_one_frame_for_a_hundred_ids() {
  char directory[] = "/tmp/fortis-price-batch-XXXXXX";
  assert(mkdtemp(directory));
  {
    KvStore store;
    assert(store.Open(directory));
    PriceSpotCache cache(store);
    char ids[50][24]{};
    char unknown_storage[50][24]{};
    const char* unknown[50]{};
    PriceSpotQuoteInput quotes[50]{};
    const PriceSpotQuoteInput one_quote = quote("count-check", 1, 5000);
    const char* one_unknown[] = {"count-check-unknown"};
    assert(cache.StoreProviderBatch("usd", "coingecko", &one_quote,
                                   kPriceSpotMaxBatchItems + 1, nullptr, 0,
                                   true, 5000) == PriceSpotStoreStatus::kInvalidArguments);
    assert(cache.StoreProviderBatch("usd", "coingecko", nullptr, 0,
                                   one_unknown, kPriceSpotMaxBatchItems + 1,
                                   true, 5000) == PriceSpotStoreStatus::kInvalidArguments);
    // This checks the guarded subtraction when the positive count itself is
    // within range but the combined count would exceed the batch maximum.
    assert(cache.StoreProviderBatch("usd", "coingecko", &one_quote, 60,
                                   one_unknown, 41, true, 5000) ==
           PriceSpotStoreStatus::kInvalidArguments);
    for (unsigned i = 0; i < 50; ++i) {
      snprintf(ids[i], sizeof(ids[i]), "coin-%03u", i);
      snprintf(unknown_storage[i], sizeof(unknown_storage[i]), "missing-%03u", i);
      unknown[i] = unknown_storage[i];
      quotes[i] = quote(ids[i], i + 1, 5000, i == 0 ? 3'960'000 : 30'000);
    }
    assert(cache.StoreProviderBatch("usd", "coingecko", quotes, 50, unknown, 50,
                                   true, 5000) == PriceSpotStoreStatus::kStored);
    char path[256];
    snprintf(path, sizeof(path), "%s/fortis.kv", directory);
    const int fd = open(path, O_RDONLY);
    assert(fd >= 0);
    uint8_t header[12];
    assert(pread(fd, header, sizeof(header), 0) == static_cast<ssize_t>(sizeof(header)));
    assert(memcmp(header, "FKV1", 4) == 0);
    struct stat info {};
    assert(fstat(fd, &info) == 0);
    assert(info.st_size == static_cast<off_t>(12 + read32(header + 4)));
    uint8_t count[4];
    assert(pread(fd, count, sizeof(count), 12) == static_cast<ssize_t>(sizeof(count)));
    assert(read32(count) == 100);
    assert(close(fd) == 0);
    for (unsigned i = 0; i < 50; ++i) {
      assert(cache.Lookup("usd", ids[i], 5000).state == PriceSpotLookupState::kFresh);
      assert(cache.Lookup("usd", unknown_storage[i], 5000).state ==
             PriceSpotLookupState::kNegative);
    }
  }
  remove_store_files(directory);
}

void test_corrupt_and_oversized_rows_fail_closed() {
  char directory[] = "/tmp/fortis-price-corrupt-XXXXXX";
  assert(mkdtemp(directory));
  {
    KvStore store;
    assert(store.Open(directory));
    const uint8_t corrupt[kPriceSpotEncodedBytes]{};
    const char* ids[] = {"corrupt", "oversize"};
    const uint8_t oversized[kPriceSpotEncodedBytes + 1]{};
    const KvMutation rows[] = {
      {"price/spot/v1/usd/corrupt", corrupt, sizeof(corrupt), 0, false},
      {"price/spot/v1/usd/oversize", oversized, sizeof(oversized), 0, false},
    };
    assert(store.Write(rows, 2));
    PriceSpotCache cache(store);
    assert(cache.Lookup("usd", ids[0], 100).state == PriceSpotLookupState::kCorrupt);
    assert(cache.Lookup("usd", ids[1], 100).state == PriceSpotLookupState::kCorrupt);
    struct stat before {};
    char path[256];
    snprintf(path, sizeof(path), "%s/fortis.kv", directory);
    assert(stat(path, &before) == 0);
    const char* bad_ids[] = {"corrupt", "oversize"};
    assert(cache.StoreProviderBatch("usd", "coingecko", nullptr, 0, &bad_ids[0], 1,
                                   true, 100) == PriceSpotStoreStatus::kReadFailure);
    assert(cache.StoreProviderBatch("usd", "coingecko", nullptr, 0, &bad_ids[1], 1,
                                   true, 100) == PriceSpotStoreStatus::kReadFailure);
    struct stat after {};
    assert(stat(path, &after) == 0 && before.st_size == after.st_size);
  }
  remove_store_files(directory);
}

void test_partial_frame_failure_recovers_and_batch_retries() {
  char directory[] = "/tmp/fortis-price-recovery-XXXXXX";
  assert(mkdtemp(directory));
  const int64_t now = 20'000'000;
  {
    KvStore store;
    assert(store.Open(directory));
    PriceSpotCache cache(store);
    const PriceSpotQuoteInput quotes[] = {
      quote("recover-a", 1, now), quote("recover-b", 2, now),
    };
    const char* unknown[] = {"recover-c"};
    assert(setenv("FORTIS_KV_TEST_FAIL", "write_after_header", 1) == 0);
    assert(cache.StoreProviderBatch("usd", "coingecko", quotes, 2, unknown, 1,
                                   true, now) == PriceSpotStoreStatus::kWriteFailure);
    assert(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
  }
  {
    KvStore recovered;
    assert(recovered.Open(directory));
    PriceSpotCache cache(recovered);
    assert(cache.Lookup("usd", "recover-a", now).state == PriceSpotLookupState::kMissing);
    assert(cache.Lookup("usd", "recover-b", now).state == PriceSpotLookupState::kMissing);
    assert(cache.Lookup("usd", "recover-c", now).state == PriceSpotLookupState::kMissing);
    const PriceSpotQuoteInput retry[] = {quote("recover-a", 4, now)};
    assert(cache.StoreProviderBatch("usd", "coingecko", retry, 1, nullptr, 0,
                                   true, now) == PriceSpotStoreStatus::kStored);
    const PriceSpotLookup result = cache.Lookup("usd", "recover-a", now);
    assert(result.state == PriceSpotLookupState::kFresh && result.value.price == 4);
  }
  remove_store_files(directory);
}
}  // namespace

void test_memory_publication_and_reopen() {
  char directory[] = "/tmp/fortis-price-cache-memory-XXXXXX";
  assert(mkdtemp(directory));
  KvStore store;
  assert(store.Open(directory));
  PriceSpotCache cache(store);
  assert(cache.EnableMemory());
  constexpr int64_t now = 1700000000000;
  const char* id = "eip155:1:erc20:0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48";
  auto item = quote(id, 1.25, now);
  item.provider_observed_at_ms = now - 20;
  assert(cache.StoreProviderBatch("usd", "coingecko", &item, 1, nullptr, 0, true, now) ==
         PriceSpotStoreStatus::kStored);
  auto hit = cache.Lookup("usd", id, now, true);
  assert(hit.state == PriceSpotLookupState::kFresh && hit.value.price == 1.25);
  assert(hit.value.provider_observed_at_ms == now - 20 && cache.MemoryHits() == 1);
  assert(cache.Lookup("usd", id, now + kPriceSpotDefaultFreshMs).stale);
  const char* missing[] = {id};
  assert(cache.StoreProviderBatch("usd", "coingecko", nullptr, 0, missing, 1, true, now + 1) ==
         PriceSpotStoreStatus::kStored);
  assert(cache.Lookup("usd", id, now + 1).state == PriceSpotLookupState::kNegative);
  store.Close();
  assert(cache.Lookup("usd", id, now + 1).state == PriceSpotLookupState::kIoError);
  assert(store.Open(directory));
  assert(cache.Lookup("usd", id, now + 1).state == PriceSpotLookupState::kNegative);
  assert(cache.MemoryHits() == 3);
  assert(cache.Lookup("usd", id, now + 1).state == PriceSpotLookupState::kNegative);
  assert(cache.MemoryHits() == 4);
  store.Close();
  remove_store_files(directory);
}

int main() {
  test_memory_publication_and_reopen();
  test_fresh_stale_negative_floor_restart_and_bounds();
  test_monotonic_floor_and_warm_ttl();
  test_one_frame_for_a_hundred_ids();
  test_corrupt_and_oversized_rows_fail_closed();
  test_partial_frame_failure_recovers_and_batch_retries();
  puts("native bounded price spot cache policy and persistence tests passed");
}
