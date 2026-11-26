#include "native_config.h"
#include <string_view>
#include "spot_resolution.h"

#include "kv_store.h"

#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace fortis::codec;
using namespace fortis::config;
using namespace fortis::prices;
using namespace fortis::providers;

namespace {
constexpr int64_t kNow = 1'700'000'000'000LL;
SpotResolutionWorkspace workspace;
SpotResolutionBatchAccumulator batch_accumulator;
SpotResolutionBatchAccumulator second_batch_accumulator;
CoinGeckoSpotBatchPlan batch_plan;
char batch_ids[100][101];
std::string_view batch_raw_ids[100];
BoundedJsonToken json_tokens[kBoundedJsonMaxTokens];
uint32_t key_index[kBoundedJsonMaxTokens];

std::string_view sv(const char* value) { return {value, strlen(value)}; }

uint32_t read32(const uint8_t* input) {
  return static_cast<uint32_t>(input[0]) |
         (static_cast<uint32_t>(input[1]) << 8) |
         (static_cast<uint32_t>(input[2]) << 16) |
         (static_cast<uint32_t>(input[3]) << 24);
}

size_t frame_count(const char* path) {
  const int fd = open(path, O_RDONLY);
  assert(fd >= 0);
  struct stat info{};
  assert(fstat(fd, &info) == 0);
  size_t frames = 0;
  off_t offset = 0;
  while (offset < info.st_size) {
    uint8_t header[12];
    assert(pread(fd, header, sizeof(header), offset) == static_cast<ssize_t>(sizeof(header)));
    assert(memcmp(header, "FKV1", 4) == 0);
    offset += 12 + read32(header + 4);
    ++frames;
  }
  close(fd);
  return frames;
}

void remove_store(const char* directory) {
  char path[768];
  snprintf(path, sizeof(path), "%s/fortis.kv", directory);
  assert(unlink(path) == 0);
  snprintf(path, sizeof(path), "%s/fortis.kv.lock", directory);
  assert(unlink(path) == 0);
  assert(rmdir(directory) == 0);
}

void store_quote(fortis::prices::PriceSpotCache& cache, const char* id, double price,
                 int64_t as_of, int64_t written_at) {
  const fortis::prices::PriceSpotQuoteInput quote{
      id, price, 2.5, 0, true, false, as_of, 0};
  assert(cache.StoreProviderBatch("usd", "coingecko", &quote, 1, nullptr, 0,
                                 true, written_at) == fortis::prices::PriceSpotStoreStatus::kStored);
}

void test_raw_alias_dedupe_fresh_negative_invalid() {
  char directory[] = "/tmp/fortis-spot-resolve-alias-XXXXXX";
  assert(mkdtemp(directory));
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    fortis::prices::PriceSpotCache cache(store);
    store_quote(cache, "bitcoin", 42.25, kNow, kNow);
    const char* unknown[] = {"unlisted"};
    assert(cache.StoreProviderBatch("usd", "coingecko", nullptr, 0, unknown, 1,
                                    true, kNow) == fortis::prices::PriceSpotStoreStatus::kStored);
    const std::string_view ids[] = {sv("Bitcoin"), sv("bitcoin"), sv("Bitcoin"),
                                  sv("unlisted"), sv("bad/id"), sv("bad/id")};
    assert(BeginSpotResolution(ids, 6, sv("usd"), cache, kNow, &workspace) ==
           fortis::base::StatusCode::kOk);
    assert(SpotResolutionUniqueIdCount(&workspace) == 2);  // cache read once per canonical id
    size_t fetch_count = 99;
    SpotResolutionFetchIds(&workspace, &fetch_count);
    assert(fetch_count == 0);
    bool ready = false;
    const SpotResponseInput* response = SpotResolutionResponse(&workspace, &ready);
    assert(response && ready && response->price_count == 2);
    assert(response->prices[0].raw_key.size() == 7 &&
           memcmp(response->prices[0].raw_key.data(), "Bitcoin", 7) == 0);
    assert(response->prices[1].raw_key.size() == 7 &&
           memcmp(response->prices[1].raw_key.data(), "bitcoin", 7) == 0);
    assert(response->prices[0].source_id.size() == 7 &&
           memcmp(response->prices[0].source_id.data(), "bitcoin", 7) == 0);
    assert(response->prices[0].cache_hit && response->prices[0].price == 42.25);
    assert(response->missing_count == 1 && response->invalid_count == 2);
    assert(memcmp(response->missing[0].data(), "unlisted", 8) == 0);
    assert(memcmp(response->invalid[0].data(), "bad/id", 6) == 0 &&
           memcmp(response->invalid[1].data(), "bad/id", 6) == 0);
    const std::string_view bad_request[] = {sv("bitcoin")};
    assert(BeginSpotResolution(bad_request, 1, sv("USD"), cache, kNow,
                               &workspace) == fortis::base::StatusCode::kInvalidArgument);
    response = SpotResolutionResponse(&workspace, &ready);
    assert(response == nullptr && !ready);
    size_t intents = 1, fetches = 1;
    SpotResolutionIntents(&workspace, &intents);
    SpotResolutionFetchIds(&workspace, &fetches);
    assert(intents == 0 && fetches == 0);
  }
  remove_store(directory);
}

void test_stale_is_served_and_revalidation_is_intent() {
  char directory[] = "/tmp/fortis-spot-resolve-stale-XXXXXX";
  assert(mkdtemp(directory));
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    fortis::prices::PriceSpotCache cache(store);
    store_quote(cache, "bitcoin", 17.0, kNow, kNow);
    const std::string_view ids[] = {sv("Bitcoin")};
    assert(BeginSpotResolution(ids, 1, sv("usd"), cache, kNow + 30'000,
                               &workspace) == fortis::base::StatusCode::kOk);
    bool ready = false;
    const SpotResponseInput* response = SpotResolutionResponse(&workspace, &ready);
    assert(response && ready && response->price_count == 1);
    assert(response->prices[0].stale && response->prices[0].cache_hit);
    size_t intent_count = 0;
    const SpotResolutionIntent* intents = SpotResolutionIntents(&workspace, &intent_count);
    assert(intent_count == 1 && intents[0].kind ==
           SpotResolutionIntentKind::kBackgroundRevalidation);
    assert(intents[0].id_kind == PriceIdKind::kCoinGecko && intents[0].stale_value_served &&
           intents[0].background_revalidation);
    size_t fetch_count = 0;
    SpotResolutionFetchIds(&workspace, &fetch_count);
    assert(fetch_count == 0);
    assert(BeginSpotResolution(ids, 1, sv("usd"), cache, kNow + 20'000,
                               &workspace, 15'000) == fortis::base::StatusCode::kOk);
    response = SpotResolutionResponse(&workspace, &ready);
    assert(response && ready && response->price_count == 1 && !response->prices[0].stale);
    intents = SpotResolutionIntents(&workspace, &intent_count);
    assert(intent_count == 1 && intents[0].kind == SpotResolutionIntentKind::kBackgroundRevalidation &&
           !intents[0].stale_value_served);
  }
  remove_store(directory);
}

void test_expired_floor_only_after_failed_attempt() {
  char directory[] = "/tmp/fortis-spot-resolve-floor-XXXXXX";
  assert(mkdtemp(directory));
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    fortis::prices::PriceSpotCache cache(store);
    store_quote(cache, "bitcoin", 11.5, kNow, kNow);
    const std::string_view ids[] = {sv("bitcoin")};
    assert(BeginSpotResolution(ids, 1, sv("usd"), cache,
                               kNow + 630'000, &workspace) == fortis::base::StatusCode::kOk);
    bool ready = true;
    const SpotResponseInput* response = SpotResolutionResponse(&workspace, &ready);
    assert(response && !ready && response->price_count == 0 && response->missing_count == 0);
    size_t fetch_count = 0;
    const char* const* fetch_ids = SpotResolutionFetchIds(&workspace, &fetch_count);
    assert(fetch_count == 1 && strcmp(fetch_ids[0], "bitcoin") == 0);
    SpotResolutionCompletion completed = CompleteSpotResolution(
        &workspace, cache, nullptr, kNow + 630'100);
    assert(completed.status == SpotResolutionCompletionStatus::kProviderFailed);
    response = SpotResolutionResponse(&workspace, &ready);
    assert(response && ready && response->price_count == 1 && response->missing_count == 0);
    assert(response->prices[0].stale && response->prices[0].cache_hit &&
           response->prices[0].price == 11.5 &&
           response->prices[0].cache_timestamp_ms == kNow);
    assert(cache.Lookup("usd", "bitcoin", kNow + 630'100).state ==
           fortis::prices::PriceSpotLookupState::kExpired);
  }
  remove_store(directory);
}

void test_floor_expiry_and_concurrent_cache_updates() {
  char directory[] = "/tmp/fortis-spot-resolve-floor-boundary-XXXXXX";
  assert(mkdtemp(directory));
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    fortis::prices::PriceSpotCache cache(store);
    store_quote(cache, "bitcoin", 11.5, kNow, kNow);
    const int64_t floor_expiry = kNow + fortis::prices::kPriceSpotFloorMs;
    const std::string_view ids[] = {sv("bitcoin")};
    assert(BeginSpotResolution(ids, 1, sv("usd"), cache, floor_expiry - 1,
                               &workspace) == fortis::base::StatusCode::kOk);
    assert(CompleteSpotResolution(&workspace, cache, nullptr, floor_expiry + 1).status ==
           SpotResolutionCompletionStatus::kProviderFailed);
    bool ready = false;
    const SpotResponseInput* response = SpotResolutionResponse(&workspace, &ready);
    assert(response && ready && response->price_count == 0 && response->missing_count == 1);

    // A concurrent live write during the provider attempt supersedes the
    // captured floor and is served fresh after the failed request.
    store_quote(cache, "ethereum", 8, kNow, kNow);
    const std::string_view ethereum[] = {sv("ethereum")};
    assert(BeginSpotResolution(ethereum, 1, sv("usd"), cache, kNow + 630'000,
                               &workspace) == fortis::base::StatusCode::kOk);
    store_quote(cache, "ethereum", 99, kNow + 630'020, kNow + 630'020);
    assert(CompleteSpotResolution(&workspace, cache, nullptr, kNow + 630'021).status ==
           SpotResolutionCompletionStatus::kProviderFailed);
    response = SpotResolutionResponse(&workspace, &ready);
    assert(response && ready && response->price_count == 1 && !response->prices[0].stale &&
           response->prices[0].price == 99);

    // A concurrent provider negative suppresses an older floor.
    store_quote(cache, "sol", 2, kNow, kNow);
    const std::string_view sol[] = {sv("sol")};
    assert(BeginSpotResolution(sol, 1, sv("usd"), cache, kNow + 630'000,
                               &workspace) == fortis::base::StatusCode::kOk);
    const char* unknown[] = {"sol"};
    const int64_t negative_at = kNow + 630'000;
    assert(cache.StoreProviderBatch("usd", "coingecko", nullptr, 0, unknown, 1,
                                    true, negative_at) == fortis::prices::PriceSpotStoreStatus::kStored);
    assert(CompleteSpotResolution(&workspace, cache, nullptr, negative_at + 1).status ==
           SpotResolutionCompletionStatus::kProviderFailed);
    response = SpotResolutionResponse(&workspace, &ready);
    assert(response && ready && response->price_count == 0 && response->missing_count == 1);
  }
  remove_store(directory);
}

void test_concurrent_fill_after_cold_miss_wins_on_failure() {
  char directory[] = "/tmp/fortis-spot-resolve-concurrent-fill-XXXXXX";
  assert(mkdtemp(directory));
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    fortis::prices::PriceSpotCache cache(store);
    const std::string_view ids[] = {sv("bitcoin")};
    assert(BeginSpotResolution(ids, 1, sv("usd"), cache, kNow, &workspace) ==
           fortis::base::StatusCode::kOk);
    store_quote(cache, "bitcoin", 77, kNow + 10, kNow + 10);
    assert(CompleteSpotResolution(&workspace, cache, nullptr, kNow + 11).status ==
           SpotResolutionCompletionStatus::kProviderFailed);
    bool ready = false;
    const SpotResponseInput* response = SpotResolutionResponse(&workspace, &ready);
    assert(response && ready && response->price_count == 1 &&
           response->prices[0].price == 77 && response->prices[0].cache_hit &&
           !response->prices[0].stale);
  }
  remove_store(directory);
}

void test_extracted_answer_store_batch_and_unknown_negative() {
  char directory[] = "/tmp/fortis-spot-resolve-answer-XXXXXX";
  assert(mkdtemp(directory));
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    fortis::prices::PriceSpotCache cache(store);
    const std::string_view ids[] = {sv("Bitcoin"), sv("bitcoin"), sv("ethereum"),
                                  sv("ethereum")};
    assert(BeginSpotResolution(ids, 4, sv("usd"), cache, kNow, &workspace) ==
           fortis::base::StatusCode::kOk);
    size_t fetch_count = 0;
    const char* const* fetch_ids = SpotResolutionFetchIds(&workspace, &fetch_count);
    assert(fetch_count == 2 && strcmp(fetch_ids[0], "bitcoin") == 0 &&
           strcmp(fetch_ids[1], "ethereum") == 0);
    static const unsigned char body[] = "{\"bitcoin\":{\"usd\":123.5,\"usd_24h_change\":1.25}}";
    CoinGeckoSpotResult extracted = ExtractCoinGeckoSimplePrice(
        body, sizeof(body) - 1, fetch_ids, fetch_count, "usd", json_tokens,
        kBoundedJsonMaxTokens, key_index, kBoundedJsonMaxTokens);
    assert(extracted.error == CoinGeckoSpotError::kNone && extracted.count == 1);
    SpotResolutionCompletion completed = CompleteSpotResolution(
        &workspace, cache, &extracted, kNow + 100);
    assert(completed.status == SpotResolutionCompletionStatus::kApplied);
    assert(completed.cache_store_status == fortis::prices::PriceSpotStoreStatus::kStored);
    bool ready = false;
    const SpotResponseInput* response = SpotResolutionResponse(&workspace, &ready);
    assert(response && ready && response->price_count == 2 && response->missing_count == 2);
    assert(memcmp(response->prices[0].raw_key.data(), "Bitcoin", 7) == 0 &&
           response->prices[0].price == 123.5 && !response->prices[0].cache_hit &&
           response->prices[0].has_change_24h_pct &&
           response->prices[0].change_24h_pct == 1.25);
    assert(memcmp(response->prices[1].raw_key.data(), "bitcoin", 7) == 0);
    assert(memcmp(response->missing[0].data(), "ethereum", 8) == 0 &&
           memcmp(response->missing[1].data(), "ethereum", 8) == 0);
    char json[4096]; size_t json_size = 0;
    assert(SerializeSpotResponseJson(*response, json, sizeof(json), &json_size) ==
           SpotSerializeResult::kOk);
    json[json_size] = '\0';
    assert(strstr(json, "\"fiat\":\"usd\"") != nullptr &&
           strstr(json, "\"Bitcoin\"") != nullptr &&
           strstr(json, "\"sourceId\":\"bitcoin\"") != nullptr &&
           strstr(json, "\"missing\":[\"ethereum\",\"ethereum\"]") != nullptr);
    assert(cache.Lookup("usd", "bitcoin", kNow + 100).state ==
           fortis::prices::PriceSpotLookupState::kFresh);
    assert(cache.Lookup("usd", "ethereum", kNow + 100).state ==
           fortis::prices::PriceSpotLookupState::kNegative);

    // One frame contains both the live quote and provider-answered unknown.
    char path[768]; snprintf(path, sizeof(path), "%s/fortis.kv", directory);
    const int fd = open(path, O_RDONLY);
    assert(fd >= 0);
    uint8_t header[16];
    assert(pread(fd, header, sizeof(header), 0) == static_cast<ssize_t>(sizeof(header)));
    assert(memcmp(header, "FKV1", 4) == 0 && read32(header + 12) == 2);
    close(fd);
  }
  remove_store(directory);
}

void test_parse_failure_does_not_negative_cache() {
  char directory[] = "/tmp/fortis-spot-resolve-parse-XXXXXX";
  assert(mkdtemp(directory));
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    fortis::prices::PriceSpotCache cache(store);
    const std::string_view ids[] = {sv("bitcoin"), sv("ethereum"),
                                  sv("bitcoin"), sv("ethereum")};
    assert(BeginSpotResolution(ids, 4, sv("usd"), cache, kNow, &workspace) ==
           fortis::base::StatusCode::kOk);
    CoinGeckoSpotResult malformed{};
    malformed.error = CoinGeckoSpotError::kInvalidJson;
    SpotResolutionCompletion completed = CompleteSpotResolution(
        &workspace, cache, &malformed, kNow + 10);
    assert(completed.status == SpotResolutionCompletionStatus::kProviderFailed &&
           completed.cache_store_status == fortis::prices::PriceSpotStoreStatus::kTransportFailureNoWrite);
    bool ready = false;
    const SpotResponseInput* response = SpotResolutionResponse(&workspace, &ready);
    assert(response && ready && response->price_count == 0 && response->missing_count == 4);
    assert(memcmp(response->missing[0].data(), "bitcoin", 7) == 0 &&
           memcmp(response->missing[1].data(), "bitcoin", 7) == 0 &&
           memcmp(response->missing[2].data(), "ethereum", 8) == 0 &&
           memcmp(response->missing[3].data(), "ethereum", 8) == 0);
    assert(cache.Lookup("usd", "bitcoin", kNow + 10).state ==
           fortis::prices::PriceSpotLookupState::kMissing);
    assert(cache.Lookup("usd", "ethereum", kNow + 10).state ==
           fortis::prices::PriceSpotLookupState::kMissing);
  }
  remove_store(directory);
}

void test_unindexed_contract_negative_does_not_abort_mixed_request() {
  char directory[] = "/tmp/fortis-spot-resolve-unindexed-XXXXXX";
  assert(mkdtemp(directory));
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    fortis::prices::PriceSpotCache cache(store);
    const std::string_view ids[] = {
        sv("unsupported-network:erc20:0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        sv("bitcoin")};
    assert(BeginSpotResolution(ids, 2, sv("usd"), cache, kNow, &workspace) ==
           fortis::base::StatusCode::kOk);
    size_t fetch_count = 0;
    const char* const* fetch_ids = SpotResolutionFetchIds(&workspace, &fetch_count);
    assert(fetch_count == 1 && strcmp(fetch_ids[0], "bitcoin") == 0);
    const char* unsupported =
        "unsupported-network:erc20:0xaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    ParsedPriceId parsed{};
    assert(ParsePriceId(unsupported, strlen(unsupported), &parsed));
    assert(cache.Lookup("usd", parsed.source_id, kNow).state ==
           fortis::prices::PriceSpotLookupState::kNegative);
    CoinGeckoSpotResult result{};
    strcpy(result.quotes[0].id, "bitcoin");
    result.quotes[0].price = 42.0;
    result.count = 1;
    assert(CompleteSpotResolution(&workspace, cache, &result, kNow + 1).status ==
           SpotResolutionCompletionStatus::kApplied);
    bool ready = false;
    const SpotResponseInput* response = SpotResolutionResponse(&workspace, &ready);
    assert(response && ready && response->price_count == 1 &&
           response->missing_count == 1);
    const char* stale_id =
        "another-unsupported-network:erc20:0xbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    store_quote(cache, stale_id, 8.0, kNow, kNow);
    const std::string_view stale_request[] = {sv(stale_id)};
    assert(BeginSpotResolution(stale_request, 1, sv("usd"), cache,
                               kNow + 30'000, &workspace) ==
           fortis::base::StatusCode::kOk);
    response = SpotResolutionResponse(&workspace, &ready);
    assert(response && ready && response->price_count == 1 &&
           response->prices[0].stale);
    assert(cache.Lookup("usd", stale_id, kNow + 30'001).state ==
           fortis::prices::PriceSpotLookupState::kNegative);
    size_t intent_count = 99;
    SpotResolutionIntents(&workspace, &intent_count);
    assert(intent_count == 0);
    fetch_ids = SpotResolutionFetchIds(&workspace, &fetch_count);
    assert(fetch_count == 0 && fetch_ids != nullptr);
  }
  remove_store(directory);
}

void test_live_quote_survives_cache_write_failure() {
  char directory[] = "/tmp/fortis-spot-resolve-cache-failure-XXXXXX";
  assert(mkdtemp(directory));
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    fortis::prices::PriceSpotCache cache(store);
    const std::string_view ids[] = {sv("bitcoin")};
    assert(BeginSpotResolution(ids, 1, sv("usd"), cache, kNow, &workspace) ==
           fortis::base::StatusCode::kOk);
    CoinGeckoSpotResult live{};
    live.count = 1;
    memcpy(live.quotes[0].id, "bitcoin", sizeof("bitcoin"));
    live.quotes[0].price = 123.75;
    store.Close();
    SpotResolutionCompletion completed = CompleteSpotResolution(
        &workspace, cache, &live, kNow + 5);
    assert(completed.status == SpotResolutionCompletionStatus::kApplied);
    assert(completed.cache_store_status == fortis::prices::PriceSpotStoreStatus::kReadFailure ||
           completed.cache_store_status == fortis::prices::PriceSpotStoreStatus::kWriteFailure);
    bool ready = false;
    const SpotResponseInput* response = SpotResolutionResponse(&workspace, &ready);
    assert(response && ready && response->price_count == 1 && response->missing_count == 0);
    assert(response->prices[0].price == 123.75 && !response->prices[0].cache_hit &&
           !response->prices[0].has_cache_timestamp);
  }
  remove_store(directory);
}

void test_jupiter_and_vestige_are_usd_fetches() {
  char directory[] = "/tmp/fortis-spot-resolve-usd-providers-XXXXXX";
  assert(mkdtemp(directory));
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    fortis::prices::PriceSpotCache cache(store);
    const std::string_view ids[] = {sv("spl:EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"),
                                  sv("asa:31566704")};
    assert(BeginSpotResolution(ids, 2, sv("usd"), cache, kNow, &workspace) ==
           fortis::base::StatusCode::kOk);
    size_t fetch_count = 0, intent_count = 0, kind_count = 0;
    const char* const* fetch_ids = SpotResolutionFetchIds(&workspace, &fetch_count);
    const PriceIdKind* fetch_kinds = SpotResolutionFetchKinds(&workspace, &kind_count);
    const SpotResolutionIntent* intents = SpotResolutionIntents(&workspace, &intent_count);
    assert(fetch_count == 2 && kind_count == 2 && intent_count == 2);
    assert(strcmp(fetch_ids[0], "spl:EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v") == 0 &&
           fetch_kinds[0] == PriceIdKind::kSpl);
    assert(intents[0].kind == SpotResolutionIntentKind::kFetch &&
           intents[0].id_kind == PriceIdKind::kSpl);
    assert(strcmp(fetch_ids[1], "asa:31566704") == 0 && fetch_kinds[1] == PriceIdKind::kAsa);
    // Canonical order follows parsed input order: both providers get fetch intents.
    assert(intents[1].kind == SpotResolutionIntentKind::kFetch &&
           intents[1].id_kind == PriceIdKind::kAsa);
    bool ready = false;
    const SpotResponseInput* response = SpotResolutionResponse(&workspace, &ready);
    assert(response && !ready && response->missing_count == 0 && response->price_count == 0);
    assert(SpotResolutionSingleIdPathLength(PriceIdKind::kSpl, fetch_ids[0], "usd") ==
           strlen("/price/v3?ids=") + 44);
    assert(SpotResolutionSingleIdPathLength(PriceIdKind::kAsa, "asa:31566704", "usd") ==
           strlen("/assets/price?network_id=0&asset_ids=31566704&denominating_asset_id=31566704"));
    assert(SpotResolutionSingleIdPathLength(PriceIdKind::kSpl, "spl:short", "usd") == 0);
    assert(SpotResolutionSingleIdPathLength(PriceIdKind::kCoinGecko, "bitcoin", "usd") ==
           strlen("/api/v3/simple/price?ids=bitcoin&vs_currencies=usd"
                  "&include_24hr_change=true&include_24hr_vol=true"));
  }
  remove_store(directory);
}

void test_stale_asa_value_remains_servable_with_revalidation() {
  char directory[] = "/tmp/fortis-spot-resolve-stale-unsupported-XXXXXX";
  assert(mkdtemp(directory));
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    fortis::prices::PriceSpotCache cache(store);
    const char* asa = "asa:31566704";
    store_quote(cache, asa, 1.5, kNow, kNow);
    const std::string_view ids[] = {sv(asa)};
    assert(BeginSpotResolution(ids, 1, sv("usd"), cache, kNow + 30'000,
                               &workspace) == fortis::base::StatusCode::kOk);
    bool ready = false;
    const SpotResponseInput* response = SpotResolutionResponse(&workspace, &ready);
    assert(response && ready && response->price_count == 1 && response->prices[0].stale);
    size_t count = 0;
    const SpotResolutionIntent* intents = SpotResolutionIntents(&workspace, &count);
    assert(count == 1 && intents[0].kind == SpotResolutionIntentKind::kBackgroundRevalidation &&
           intents[0].stale_value_served && intents[0].background_revalidation);

    // A stale SPL value in USD schedules a Jupiter revalidation instead.
    const char* spl = "spl:EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
    const fortis::prices::PriceSpotQuoteInput quote{spl, 1.25, 0, 0, false, false, kNow, 0};
    assert(cache.StoreProviderBatch("usd", "jupiter", &quote, 1, nullptr, 0,
                                    true, kNow) == fortis::prices::PriceSpotStoreStatus::kStored);
    const std::string_view spl_ids[] = {sv(spl)};
    assert(BeginSpotResolution(spl_ids, 1, sv("usd"), cache, kNow + 30'000,
                               &workspace) == fortis::base::StatusCode::kOk);
    response = SpotResolutionResponse(&workspace, &ready);
    assert(response && ready && response->price_count == 1 && response->prices[0].stale &&
           response->prices[0].source.size() == 7 &&
           memcmp(response->prices[0].source.data(), "jupiter", 7) == 0);
    intents = SpotResolutionIntents(&workspace, &count);
    assert(count == 1 && intents[0].kind == SpotResolutionIntentKind::kBackgroundRevalidation &&
           intents[0].id_kind == PriceIdKind::kSpl && intents[0].stale_value_served);
  }
  remove_store(directory);
}

void test_spl_non_usd_is_missing_without_provider_or_write() {
  char directory[] = "/tmp/fortis-spot-resolve-spl-fiat-XXXXXX";
  assert(mkdtemp(directory));
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    fortis::prices::PriceSpotCache cache(store);
    const char* spl = "spl:EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
    const char* asa = "asa:31566704";
    const std::string_view ids[] = {sv("ethereum"), sv(spl), sv(spl), sv(asa)};
    // PricesFacet skips USD-only providers for non-USD fiats and writes
    // nothing, while the CoinGecko slug still fetches in that fiat.
    assert(BeginSpotResolution(ids, 4, sv("eur"), cache, kNow, &workspace) ==
           fortis::base::StatusCode::kOk);
    size_t fetch_count = 0, intent_count = 0;
    const char* const* fetch_ids = SpotResolutionFetchIds(&workspace, &fetch_count);
    SpotResolutionIntents(&workspace, &intent_count);
    assert(fetch_count == 1 && strcmp(fetch_ids[0], "ethereum") == 0 && intent_count == 1);
    bool ready = true;
    const SpotResponseInput* response = SpotResolutionResponse(&workspace, &ready);
    assert(response && !ready && response->missing_count == 3 && response->price_count == 0);
    assert(CompleteSpotResolution(&workspace, cache, nullptr, kNow + 1).status ==
           SpotResolutionCompletionStatus::kProviderFailed);
    response = SpotResolutionResponse(&workspace, &ready);
    assert(response && ready && response->missing_count == 4);
    assert(cache.Lookup("eur", spl, kNow + 1).state == fortis::prices::PriceSpotLookupState::kMissing);
    assert(cache.Lookup("eur", asa, kNow + 1).state == fortis::prices::PriceSpotLookupState::kMissing);

    // A durable floor in that fiat is still served stale, exactly as
    // readFloorMany does after the provider group is skipped.
    const fortis::prices::PriceSpotQuoteInput quote{spl, 0.91, 0, 0, false, false, kNow, 0};
    assert(cache.StoreProviderBatch("eur", "jupiter", &quote, 1, nullptr, 0,
                                    true, kNow) == fortis::prices::PriceSpotStoreStatus::kStored);
    const fortis::prices::PriceSpotQuoteInput vestige_quote{asa, 0.5, 0, 0, false, false, kNow, 0};
    assert(cache.StoreProviderBatch("eur", "vestige", &vestige_quote, 1, nullptr, 0,
                                    true, kNow) == fortis::prices::PriceSpotStoreStatus::kStored);
    const std::string_view floor_ids[] = {sv(spl)};
    assert(BeginSpotResolution(floor_ids, 1, sv("eur"), cache, kNow + 630'000,
                               &workspace) == fortis::base::StatusCode::kOk);
    response = SpotResolutionResponse(&workspace, &ready);
    assert(response && ready && response->price_count == 1 && response->prices[0].stale &&
           response->prices[0].cache_hit && response->prices[0].price == 0.91);
    SpotResolutionIntents(&workspace, &intent_count);
    assert(intent_count == 0);
    const std::string_view asa_floor[] = {sv(asa)};
    assert(BeginSpotResolution(asa_floor, 1, sv("eur"), cache, kNow + 630'000,
                               &workspace) == fortis::base::StatusCode::kOk);
    response = SpotResolutionResponse(&workspace, &ready);
    assert(response && ready && response->price_count == 1 && response->prices[0].stale &&
           response->prices[0].price == 0.5);
    SpotResolutionIntents(&workspace, &intent_count);
    assert(intent_count == 0);
  }
  remove_store(directory);
}

void test_mixed_kinds_finalize_with_provider_sources() {
  char directory[] = "/tmp/fortis-spot-resolve-mixed-kinds-XXXXXX";
  assert(mkdtemp(directory));
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    fortis::prices::PriceSpotCache cache(store);
    const char* spl = "spl:EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v";
    const char* unknown_spl = "spl:So11111111111111111111111111111111111111112";
    const std::string_view ids[] = {sv("Ethereum"), sv(spl), sv(unknown_spl)};
    assert(BeginSpotResolution(ids, 3, sv("usd"), cache, kNow, &workspace) ==
           fortis::base::StatusCode::kOk);
    size_t fetch_count = 0, kind_count = 0;
    const char* const* fetch_ids = SpotResolutionFetchIds(&workspace, &fetch_count);
    const PriceIdKind* kinds = SpotResolutionFetchKinds(&workspace, &kind_count);
    assert(fetch_count == 3 && kind_count == 3 && kinds[0] == PriceIdKind::kCoinGecko &&
           kinds[1] == PriceIdKind::kSpl && kinds[2] == PriceIdKind::kSpl);
    // The owner's plan: one batch per canonical ID with its provider path.
    CoinGeckoSpotBatchPlan plan{};
    plan.id_count = plan.batch_count = 3;
    for (size_t i = 0; i < 3; ++i) {
      memcpy(plan.ids[i], fetch_ids[i], strlen(fetch_ids[i]) + 1);
      plan.batches[i] = {i, 1, SpotResolutionSingleIdPathLength(kinds[i], fetch_ids[i], "usd")};
    }
    CoinGeckoSpotBatchPlan forged = plan;
    ++forged.batches[1].path_length;
    assert(!InitializeSpotResolutionBatchAccumulator(&workspace, &forged, &batch_accumulator));
    assert(InitializeSpotResolutionBatchAccumulator(&workspace, &plan, &batch_accumulator));
    CoinGeckoSpotResult ethereum{};
    ethereum.count = 1;
    memcpy(ethereum.quotes[0].id, "ethereum", sizeof("ethereum"));
    ethereum.quotes[0].price = 3012.44;
    ethereum.quotes[0].hasVolume24h = true;
    ethereum.quotes[0].volume24h = 5;
    CoinGeckoSpotResult jupiter{};
    jupiter.count = 1;
    memcpy(jupiter.quotes[0].id, spl, strlen(spl) + 1);
    jupiter.quotes[0].price = 0.9998;
    jupiter.quotes[0].hasChange24hPct = true;
    jupiter.quotes[0].change24hPct = -0.0125;
    CoinGeckoSpotResult empty{};
    assert(RecordSpotResolutionBatchResult(&batch_accumulator, 0, &ethereum, kNow + 5) ==
           SpotResolutionBatchReceipt::kRecordedAnswer);
    assert(RecordSpotResolutionBatchResult(&batch_accumulator, 1, &jupiter, kNow + 6) ==
           SpotResolutionBatchReceipt::kRecordedAnswer);
    assert(RecordSpotResolutionBatchResult(&batch_accumulator, 2, &empty, kNow + 7) ==
           SpotResolutionBatchReceipt::kRecordedAnswer);
    const auto completed = FinalizeSpotResolutionBatches(
        &workspace, cache, &batch_accumulator, kNow + 8);
    assert(completed.status == SpotResolutionCompletionStatus::kApplied &&
           completed.cache_store_status == fortis::prices::PriceSpotStoreStatus::kStored);
    bool ready = false;
    const SpotResponseInput* response = SpotResolutionResponse(&workspace, &ready);
    assert(response && ready && response->price_count == 2 && response->missing_count == 1);
    assert(memcmp(response->prices[0].source.data(), "coingecko", 9) == 0 &&
           memcmp(response->prices[1].source.data(), "jupiter", 7) == 0 &&
           response->prices[1].has_change_24h_pct &&
           response->prices[1].change_24h_pct == -0.0125 &&
           memcmp(response->missing[0].data(), unknown_spl, strlen(unknown_spl)) == 0);
    const auto spl_lookup = cache.Lookup("usd", spl, kNow + 8);
    assert(spl_lookup.state == fortis::prices::PriceSpotLookupState::kFresh &&
           strcmp(spl_lookup.value.provider, "jupiter") == 0 &&
           !spl_lookup.value.has_volume24h);
    assert(strcmp(cache.Lookup("usd", "ethereum", kNow + 8).value.provider, "coingecko") == 0);
    assert(cache.Lookup("usd", unknown_spl, kNow + 8).state ==
           fortis::prices::PriceSpotLookupState::kNegative);
    char path[768]; snprintf(path, sizeof(path), "%s/fortis.kv", directory);
    assert(frame_count(path) == 2);  // One atomic frame per provider.
  }
  remove_store(directory);
}

void prepare_long_batch_ids() {
  for (size_t i = 0; i < 100; ++i) {
    memset(batch_ids[i], 'a' + (i % 20), 100);
    char suffix[8];
    snprintf(suffix, sizeof(suffix), "%03zu", i);
    memcpy(batch_ids[i] + 96, suffix, 3);
    batch_ids[i][99] = 'z';
    batch_ids[i][100] = '\0';
    batch_raw_ids[i] = sv(batch_ids[i]);
  }
}

void prepare_long_batch_request(fortis::prices::PriceSpotCache& cache, int64_t begin_at) {
  prepare_long_batch_ids();
  assert(BeginSpotResolution(batch_raw_ids, 100, sv("usd"), cache, begin_at,
                             &workspace) == fortis::base::StatusCode::kOk);
  NativeConfig config{};
  config.ca_bundle_configured = true;
  snprintf(config.ca_bundle_path, sizeof(config.ca_bundle_path), "%s", "/tmp/test-ca.pem");
  const char* ids[100];
  for (size_t i = 0; i < 100; ++i) ids[i] = batch_ids[i];
  assert(BuildCoinGeckoSpotBatchPlan(config, ids, 100, "usd", &batch_plan) ==
         CoinGeckoSpotPlanError::kNone);
  assert(batch_plan.id_count == 100 && batch_plan.batch_count == 6);
  CoinGeckoSpotBatchPlan forged_plan = batch_plan;
  --forged_plan.batches[0].path_length;
  assert(!InitializeSpotResolutionBatchAccumulator(&workspace, &forged_plan,
                                                    &second_batch_accumulator));
  assert(InitializeSpotResolutionBatchAccumulator(&workspace, &batch_plan,
                                                  &batch_accumulator));
  assert(!InitializeSpotResolutionBatchAccumulator(&workspace, &batch_plan,
                                                    &second_batch_accumulator));
}

void test_batch_partial_success_failure_and_floor() {
  char directory[] = "/tmp/fortis-spot-resolve-batches-XXXXXX";
  assert(mkdtemp(directory));
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    fortis::prices::PriceSpotCache cache(store);
    prepare_long_batch_ids();
    store_quote(cache, batch_ids[19], 19.5, kNow, kNow);
    prepare_long_batch_request(cache, kNow + 630'000);
    CoinGeckoSpotResult success{};
    success.count = 1;
    memcpy(success.quotes[0].id, batch_ids[0], sizeof(success.quotes[0].id));
    success.quotes[0].price = 123.0;
    assert(RecordSpotResolutionBatchResult(&batch_accumulator, 0, &success,
        kNow + 630'010) == SpotResolutionBatchReceipt::kRecordedAnswer);
    assert(RecordSpotResolutionBatchResult(&batch_accumulator, 0, &success,
        kNow + 630'010) == SpotResolutionBatchReceipt::kDuplicateBatch);
    assert(RecordSpotResolutionBatchFailure(&batch_accumulator, 1, false) ==
           SpotResolutionBatchReceipt::kRecordedFailure);
    for (size_t b = 2; b < 6; ++b)
      assert(RecordSpotResolutionBatchFailure(&batch_accumulator, b, true) ==
             SpotResolutionBatchReceipt::kRecordedFailure);
    // The failed batch must use its current floor; failed cold IDs stay missing
    // and are never written as provider negatives.
    const auto completed = FinalizeSpotResolutionBatches(
        &workspace, cache, &batch_accumulator, kNow + 630'100);
    assert(completed.status == SpotResolutionCompletionStatus::kPartiallyApplied &&
           completed.cache_store_status == fortis::prices::PriceSpotStoreStatus::kStored);
    const auto* response = SpotResolutionResponse(&workspace, nullptr);
    assert(response && response->price_count == 2 && response->missing_count == 98);
    assert(response->prices[0].price == 123.0 && !response->prices[0].stale);
    assert(response->prices[1].price == 19.5 && response->prices[1].stale &&
           response->prices[1].cache_hit);
    assert(cache.Lookup("usd", batch_ids[1], kNow + 630'100).state ==
           fortis::prices::PriceSpotLookupState::kNegative);
    assert(cache.Lookup("usd", batch_ids[20], kNow + 630'100).state ==
           fortis::prices::PriceSpotLookupState::kMissing);
    assert(cache.Lookup("usd", batch_ids[19], kNow + 630'100).state ==
           fortis::prices::PriceSpotLookupState::kExpired);
    assert(FinalizeSpotResolutionBatches(&workspace, cache, &batch_accumulator,
        kNow + 630'101).status == SpotResolutionCompletionStatus::kInvalidWorkspace);
    assert(CompleteSpotResolution(&workspace, cache, nullptr, kNow + 630'101).status ==
           SpotResolutionCompletionStatus::kInvalidWorkspace);
    char path[768]; snprintf(path, sizeof(path), "%s/fortis.kv", directory);
    assert(frame_count(path) == 2); // Floor seed plus one answered-batch frame.
  }
  remove_store(directory);
}

void test_batch_all_success_combined_frame_and_incomplete_guard() {
  char directory[] = "/tmp/fortis-spot-resolve-batch-all-XXXXXX";
  assert(mkdtemp(directory));
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    fortis::prices::PriceSpotCache cache(store);
    prepare_long_batch_request(cache, kNow);
    assert(FinalizeSpotResolutionBatches(&workspace, cache, &batch_accumulator,
        kNow + 1).status == SpotResolutionCompletionStatus::kInvalidWorkspace);
    CoinGeckoSpotResult empty{};
    for (size_t b = 0; b < 6; ++b)
      assert(RecordSpotResolutionBatchResult(&batch_accumulator, b, &empty,
          kNow + 1) == SpotResolutionBatchReceipt::kRecordedAnswer);
    const auto completed = FinalizeSpotResolutionBatches(
        &workspace, cache, &batch_accumulator, kNow + 2);
    assert(completed.status == SpotResolutionCompletionStatus::kApplied &&
           completed.cache_store_status == fortis::prices::PriceSpotStoreStatus::kStored);
    const auto* response = SpotResolutionResponse(&workspace, nullptr);
    assert(response && response->price_count == 0 && response->missing_count == 100);
    assert(cache.Lookup("usd", batch_ids[99], kNow + 2).state ==
           fortis::prices::PriceSpotLookupState::kNegative);
    char path[768]; snprintf(path, sizeof(path), "%s/fortis.kv", directory);
    assert(frame_count(path) == 1); // Six successes are committed in one frame.
  }
  remove_store(directory);
}
}  // namespace

int main() {
  static_assert(sizeof(SpotResolutionWorkspace) < 160 * 1024,
                "coordinator workspace should stay caller-owned and bounded");
  static_assert(sizeof(SpotResolutionBatchAccumulator) < 128 * 1024,
                "batch accumulator must remain bounded for canonical contract IDs");
  test_raw_alias_dedupe_fresh_negative_invalid();
  test_stale_is_served_and_revalidation_is_intent();
  test_expired_floor_only_after_failed_attempt();
  test_floor_expiry_and_concurrent_cache_updates();
  test_concurrent_fill_after_cold_miss_wins_on_failure();
  test_extracted_answer_store_batch_and_unknown_negative();
  test_parse_failure_does_not_negative_cache();
  test_unindexed_contract_negative_does_not_abort_mixed_request();
  test_live_quote_survives_cache_write_failure();
  test_jupiter_and_vestige_are_usd_fetches();
  test_stale_asa_value_remains_servable_with_revalidation();
  test_spl_non_usd_is_missing_without_provider_or_write();
  test_mixed_kinds_finalize_with_provider_sources();
  test_batch_partial_success_failure_and_floor();
  test_batch_all_success_combined_frame_and_incomplete_guard();
  puts("native bounded spot resolution tests passed");
}
