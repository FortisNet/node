#include <string_view>
#include "env_clock.h"
#include "kv_store.h"
#include "price_spot_cache.h"
#include "price_series_store.h"
#include "spot_provider_owner.h"
#include "spot_owner_factory_support.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

using namespace fortis::config;
using namespace fortis::prices;

namespace {
uint64_t AcceptedId(const fortis::prices::SpotProviderOwnerSubmission& submitted) {
  assert(submitted.status() == fortis::prices::SpotProviderOwnerSubmitStatus::kAccepted);
  assert(submitted.ticket() && submitted.ticket()->request_id);
  return submitted.ticket()->request_id;
}

constexpr size_t kBodyCapacity = 128 * 1024;
struct CompletionState {
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
  bool done = false;
  size_t callback_count = 0;
  SpotProviderOwnerResultStatus status = SpotProviderOwnerResultStatus::kInternalFailure;
  SpotResolutionCompletionStatus resolution = SpotResolutionCompletionStatus::kNotNeeded;
  char content_type[64]{};
  uint8_t body[kBodyCapacity]{};
  size_t body_length = 0;
};
CompletionState completion;

uint64_t monotonic_ms() {
  timespec now{};
  assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return static_cast<uint64_t>(now.tv_sec) * 1000ULL +
         static_cast<uint64_t>(now.tv_nsec / 1000000L);
}

uint64_t process_cpu_ms() {
  timespec now{};
  assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now) == 0);
  return static_cast<uint64_t>(now.tv_sec) * 1000ULL +
         static_cast<uint64_t>(now.tv_nsec / 1000000L);
}

bool allow_loopback(void*, uint32_t address) { return address == 0x7f000001U; }

void on_completion(void* context, const SpotProviderOwnerResult& result) {
  auto* state = static_cast<CompletionState*>(context);
  pthread_mutex_lock(&state->mutex);
  state->status = result.status;
  ++state->callback_count;
  state->resolution = result.resolution;
  if (result.content_type)
    snprintf(state->content_type, sizeof(state->content_type), "%s", result.content_type);
  if (result.body_length) {
    assert(result.body_length <= sizeof(state->body));
    memcpy(state->body, result.body, result.body_length);
    state->body_length = result.body_length;
  }
  state->done = true;
  pthread_cond_broadcast(&state->changed);
  pthread_mutex_unlock(&state->mutex);
}

void wait_state(CompletionState& state) {
  pthread_mutex_lock(&state.mutex);
  timespec deadline{};
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 25;
  while (!state.done) {
    const int rc = pthread_cond_timedwait(&state.changed, &state.mutex,
                                          &deadline);
    assert(rc == 0);
  }
  pthread_mutex_unlock(&state.mutex);
}

void wait_completion() { wait_state(completion); }

void reset_completion() {
  pthread_mutex_lock(&completion.mutex);
  completion.done = false;
  completion.callback_count = 0;
  completion.status = SpotProviderOwnerResultStatus::kInternalFailure;
  completion.resolution = SpotResolutionCompletionStatus::kNotNeeded;
  completion.content_type[0] = '\0';
  completion.body_length = 0;
  pthread_mutex_unlock(&completion.mutex);
}

struct IndexedCompletion {
  size_t callback_count = 0;
  SpotProviderOwnerResultStatus status = SpotProviderOwnerResultStatus::kInternalFailure;
};
IndexedCompletion stop_completions[kSpotProviderOwnerMaxRequests];

void on_indexed_completion(void* context, const SpotProviderOwnerResult& result) {
  auto* saved = static_cast<IndexedCompletion*>(context);
  ++saved->callback_count;
  saved->status = result.status;
}

void* stop_owner_thread(void* context) {
  static_cast<SpotProviderOwner*>(context)->Stop();
  return nullptr;
}

void pause_ms(long milliseconds) {
  timespec delay{milliseconds / 1000, (milliseconds % 1000) * 1000000L};
  while (nanosleep(&delay, &delay) != 0) {}
}

void remove_store(const char* directory) {
  char path[768];
  snprintf(path, sizeof(path), "%s/fortis.kv", directory);
  assert(unlink(path) == 0);
  snprintf(path, sizeof(path), "%s/fortis.kv.lock", directory);
  assert(unlink(path) == 0);
  assert(rmdir(directory) == 0);
}

size_t kv_frame_count(const char* directory) {
  char path[768];
  snprintf(path, sizeof(path), "%s/fortis.kv", directory);
  FILE* file = fopen(path, "rb");
  assert(file);
  size_t count = 0;
  unsigned char header[12];
  while (fread(header, 1, sizeof(header), file) == sizeof(header)) {
    assert(memcmp(header, "FKV1", 4) == 0);
    const uint32_t length = static_cast<uint32_t>(header[4]) |
        (static_cast<uint32_t>(header[5]) << 8) |
        (static_cast<uint32_t>(header[6]) << 16) |
        (static_cast<uint32_t>(header[7]) << 24);
    assert(fseek(file, static_cast<long>(length), SEEK_CUR) == 0);
    ++count;
  }
  assert(feof(file));
  fclose(file);
  return count;
}

void seed_stale(fortis::prices::PriceSpotCache& cache, const char* id, double price) {
  const int64_t old_now = static_cast<int64_t>(time(nullptr)) * 1000 - 31'000;
  const fortis::prices::PriceSpotQuoteInput quote{id, price, 0, 0, false, false, old_now, 0};
  assert(cache.StoreProviderBatch("usd", "coingecko", &quote, 1, nullptr, 0,
                                 true, old_now) == fortis::prices::PriceSpotStoreStatus::kStored);
}
}  // namespace

int main(int argc, char** argv) {
  assert(argc == 14);
  char directory[] = "/tmp/fortis-spot-owner-XXXXXX";
  assert(mkdtemp(directory));
  {
    fortis::storage::KvStore store;
    assert(store.Open(directory));
    fortis::prices::PriceSpotCache cache(store);
    fortis::prices::PriceSeriesStore series(store);
    NativeConfig config{};
    {
      NativeConfig invalid_config{};
      SpotProviderOwnerOptions options;
      options.wall_clock = &PriceOwnerEnvClock();
      auto failed = SpotProviderOwner::Create(cache, series, invalid_config, options);
      assert(!failed); // Failed construction drains the unpublished adapter.
    }
    config.ca_bundle_configured = true;
    snprintf(config.ca_bundle_path, sizeof(config.ca_bundle_path), "%s", argv[2]);
    auto owner = fortis::testing::StartTestSpotOwner(cache, series, config, nullptr,
        &PriceOwnerEnvClock(), [&](SpotProviderOwnerStartup& startup) {
          return startup.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr);
        });

    static char ids[100][101];
    static std::string_view views[100];
    for (size_t i = 0; i < 100; ++i) {
      memset(ids[i], static_cast<int>('a' + i % 20), 96);
      snprintf(ids[i] + 96, 5, "%03zu", i);
      ids[i][99] = 'z';
      ids[i][100] = '\0';
      views[i] = {ids[i], 100};
    }
    uint64_t request_id = 0;
    request_id = AcceptedId(owner.Submit(views, 100, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 25000, on_completion, &completion));
    assert(request_id != 0);
    wait_completion();
    assert(completion.status == SpotProviderOwnerResultStatus::kComplete);
    assert(!owner.Cancel(request_id)); // Completion has already been committed.
    assert(completion.resolution == SpotResolutionCompletionStatus::kPartiallyApplied);
    assert(strcmp(completion.content_type, "application/json") == 0);
    assert(completion.body_length < sizeof(completion.body));
    completion.body[completion.body_length] = '\0';
    assert(strstr(reinterpret_cast<char*>(completion.body), "\"prices\"") != nullptr);
    assert(strstr(reinterpret_cast<char*>(completion.body), ids[0]) != nullptr);
    const int64_t now = static_cast<int64_t>(time(nullptr)) * 1000;
    assert(cache.Lookup("usd", ids[0], now).state == fortis::prices::PriceSpotLookupState::kFresh);
    assert(cache.Lookup("usd", ids[1], now).state == fortis::prices::PriceSpotLookupState::kMissing);
    assert(cache.Lookup("usd", ids[20], now).state == fortis::prices::PriceSpotLookupState::kMissing);
    // The failed batch does not negative-cache its floorless IDs.
    assert(cache.Lookup("usd", ids[19], now).state == fortis::prices::PriceSpotLookupState::kMissing);

    // Two requests arrive inside the same 20 ms window with mixed ID sets and
    // raw aliases. They share the canonical flight, but retain independent keys.
    CompletionState burst_a{}, burst_b{};
    const size_t frames_before_burst = kv_frame_count(directory);
    const char* a_ids[] = {"Shared-Token", "shared-token", "unique-a"};
    const char* b_ids[] = {"SHARED-TOKEN", "unique-b"};
    std::string_view a_views[3], b_views[2];
    for (size_t i = 0; i < 3; ++i) a_views[i] = {a_ids[i], strlen(a_ids[i])};
    for (size_t i = 0; i < 2; ++i) b_views[i] = {b_ids[i], strlen(b_ids[i])};
    (void)AcceptedId(owner.Submit(a_views, 3, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &burst_a));
    pause_ms(10);
    (void)AcceptedId(owner.Submit(b_views, 2, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &burst_b));
    wait_state(burst_a);
    wait_state(burst_b);
    assert(burst_a.status == SpotProviderOwnerResultStatus::kComplete &&
           burst_b.status == SpotProviderOwnerResultStatus::kComplete);
    assert(burst_a.body_length < sizeof(burst_a.body) &&
           burst_b.body_length < sizeof(burst_b.body));
    burst_a.body[burst_a.body_length] = '\0';
    burst_b.body[burst_b.body_length] = '\0';
    assert(strstr(reinterpret_cast<char*>(burst_a.body), "Shared-Token") != nullptr &&
           strstr(reinterpret_cast<char*>(burst_a.body), "shared-token") != nullptr &&
           strstr(reinterpret_cast<char*>(burst_b.body), "SHARED-TOKEN") != nullptr);
    assert(kv_frame_count(directory) == frames_before_burst);

    // One canceled waiter leaves the shared HTTPS operation alive for its peer.
    CompletionState shared_cancelled{}, shared_survivor{};
    const char* shared_slow = "shared-slow";
    const std::string_view shared_slow_view{shared_slow, strlen(shared_slow)};
    const size_t shared_frames_before = kv_frame_count(directory);
    uint64_t canceled_waiter_id = 0;
    canceled_waiter_id = AcceptedId(owner.Submit(&shared_slow_view, 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &shared_cancelled));
    (void)AcceptedId(owner.Submit(&shared_slow_view, 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &shared_survivor));
    for (size_t i = 0; i < 800 && access(argv[5], F_OK) != 0; ++i) pause_ms(10);
    assert(access(argv[5], F_OK) == 0);
    assert(owner.Cancel(canceled_waiter_id));
    wait_state(shared_cancelled);
    wait_state(shared_survivor);
    assert(shared_cancelled.status == SpotProviderOwnerResultStatus::kCancelled &&
           shared_cancelled.callback_count == 1 &&
           shared_survivor.status == SpotProviderOwnerResultStatus::kComplete);
    fortis::prices::PriceSeriesSpot shared_series{};
    assert(series.GetSpot("usd", shared_slow, now, &shared_series) ==
           fortis::prices::PriceSeriesReadStatus::kFound);
    assert(shared_series.price == 4.0);
    // The shared provider batch commits cache and series rows in one frame.
    assert(kv_frame_count(directory) == shared_frames_before + 1);
    fortis::prices::PriceSeriesSpot unknown_series{};
    assert(series.GetSpot("usd", "shared-token", now, &unknown_series) ==
           fortis::prices::PriceSeriesReadStatus::kMissing);

    // A malformed prior row in either plane aborts the shared atomic frame;
    // neither corruption changes the parsed provider reply.
    const uint8_t corrupt_value = 0xff;
    const fortis::storage::KvMutation corrupt_series{
        "price/series/v1/usd/series-fails", &corrupt_value, 1, 0, false};
    const fortis::storage::KvMutation corrupt_cache{
        "price/spot/v1/usd/cache-fails", &corrupt_value, 1, 0, false};
    assert(store.Write(&corrupt_series, 1));
    assert(store.Write(&corrupt_cache, 1));
    const char* failure_ids[] = {"series-fails", "cache-fails"};
    for (size_t i = 0; i < 2; ++i) {
      CompletionState failed_storage{};
      const std::string_view view{failure_ids[i], strlen(failure_ids[i])};
      (void)AcceptedId(owner.Submit(&view, 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &failed_storage));
      wait_state(failed_storage);
      assert(failed_storage.status == SpotProviderOwnerResultStatus::kComplete);
      failed_storage.body[failed_storage.body_length] = '\0';
      assert(strstr(reinterpret_cast<char*>(failed_storage.body), failure_ids[i]) != nullptr);
    }
    assert(cache.Lookup("usd", "series-fails", now).state ==
           fortis::prices::PriceSpotLookupState::kMissing);
    fortis::prices::PriceSeriesSpot failed_series{};
    assert(series.GetSpot("usd", "series-fails", now, &failed_series) ==
           fortis::prices::PriceSeriesReadStatus::kCorrupt);
    assert(cache.Lookup("usd", "cache-fails", now).state ==
           fortis::prices::PriceSpotLookupState::kCorrupt);
    assert(series.GetSpot("usd", "cache-fails", now, &failed_series) ==
           fortis::prices::PriceSeriesReadStatus::kMissing);
    const auto storage_stats = owner.storage_stats();
    assert(storage_stats.cache_write_failures >= 1);
    assert(storage_stats.series_write_failures >= 1);

    // With no remaining waiters the shared job is canceled. A new request for
    // the same key must form a fresh flight instead of joining the orphan.
    CompletionState all_cancelled{}, replacement{};
    const char* cancel_all_id = "cancel-all-slow";
    const std::string_view cancel_all_view{cancel_all_id, strlen(cancel_all_id)};
    uint64_t cancel_all_request = 0;
    cancel_all_request = AcceptedId(owner.Submit(&cancel_all_view, 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &all_cancelled));
    for (size_t i = 0; i < 800 && access(argv[6], F_OK) != 0; ++i) pause_ms(10);
    assert(access(argv[6], F_OK) == 0);
    assert(owner.Cancel(cancel_all_request));
    wait_state(all_cancelled);
    assert(all_cancelled.status == SpotProviderOwnerResultStatus::kCancelled &&
           all_cancelled.callback_count == 1);
    (void)AcceptedId(owner.Submit(&cancel_all_view, 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &replacement));
    wait_state(replacement);
    assert(replacement.status == SpotProviderOwnerResultStatus::kComplete &&
           cache.Lookup("usd", cancel_all_id, now).state == fortis::prices::PriceSpotLookupState::kFresh);

    // A failed shared provider response resolves both callers without writing
    // a negative entry, so the next request remains eligible to retry.
    CompletionState failed_a{}, failed_b{};
    const char* failed_id = "shared-failure";
    const std::string_view failed_view{failed_id, strlen(failed_id)};
    (void)AcceptedId(owner.Submit(&failed_view, 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &failed_a));
    (void)AcceptedId(owner.Submit(&failed_view, 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &failed_b));
    wait_state(failed_a);
    wait_state(failed_b);
    assert(failed_a.status == SpotProviderOwnerResultStatus::kComplete &&
           failed_b.status == SpotProviderOwnerResultStatus::kComplete);
    assert(cache.Lookup("usd", failed_id, now).state == fortis::prices::PriceSpotLookupState::kMissing);

    // Stale positives return immediately; refresh is detached from the
    // request slot and repeated stale reads join one background flight.
    auto stale_owner = fortis::testing::StartTestSpotOwner(cache, series, config, nullptr,
        &PriceOwnerEnvClock(), [&](SpotProviderOwnerStartup& startup) {
          return startup.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr);
        });
    seed_stale(cache, "stale-token", 7.0);
    assert(cache.Lookup("usd", "stale-token",
        static_cast<int64_t>(time(nullptr)) * 1000).state ==
        fortis::prices::PriceSpotLookupState::kStale);
    CompletionState stale_first{}, stale_second{}, cold_after_stale{};
    const char* stale_raw = "Stale-Token";
    const std::string_view stale_view{stale_raw, strlen(stale_raw)};
    const uint64_t stale_started = monotonic_ms();
    (void)AcceptedId(stale_owner.Submit(&stale_view, 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &stale_first));
    wait_state(stale_first);
    assert(stale_first.status == SpotProviderOwnerResultStatus::kComplete &&
           monotonic_ms() - stale_started < 500 && stale_owner.active_request_count() == 0);
    assert(stale_first.body_length < sizeof(stale_first.body));
    stale_first.body[stale_first.body_length] = '\0';
    assert(strstr(reinterpret_cast<char*>(stale_first.body), "Stale-Token") != nullptr &&
           strstr(reinterpret_cast<char*>(stale_first.body), "\"stale\":true") != nullptr);
    for (size_t i = 0; i < 800 && access(argv[7], F_OK) != 0; ++i) pause_ms(10);
    assert(access(argv[7], F_OK) == 0);

    const char* stale_alias = "STALE-TOKEN";
    const std::string_view stale_alias_view{stale_alias, strlen(stale_alias)};
    (void)AcceptedId(stale_owner.Submit(&stale_alias_view, 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &stale_second));
    wait_state(stale_second);
    assert(stale_second.status == SpotProviderOwnerResultStatus::kComplete &&
           stale_owner.active_request_count() == 0);

    // A cold foreground read joins an already-running background flight. A
    // separate cold miss still starts on the reserved foreground lane slot.
    const char* cold_id = "cold-after-stale";
    const std::string_view cold_view{cold_id, strlen(cold_id)};
    (void)AcceptedId(stale_owner.Submit(&cold_view, 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &cold_after_stale));
    for (size_t i = 0; i < 800 && access(argv[10], F_OK) != 0; ++i) pause_ms(10);
    assert(access(argv[10], F_OK) == 0);
    wait_state(cold_after_stale);
    assert(cold_after_stale.status == SpotProviderOwnerResultStatus::kComplete);

    // Move only the current envelope outside stale service while preserving its
    // durable floor. The foreground request must join the still-running refresh.
    const int64_t expired_write_time = static_cast<int64_t>(time(nullptr)) * 1000 -
        fortis::prices::kPriceSpotStaleMs - 40'000;
    const fortis::prices::PriceSpotQuoteInput expired_quote{
        "stale-token", 8.0, 0, 0, false, false, expired_write_time, 0};
    assert(cache.StoreProviderBatch("usd", "coingecko", &expired_quote, 1,
        nullptr, 0, true, expired_write_time) == fortis::prices::PriceSpotStoreStatus::kStored);
    assert(cache.Lookup("usd", "stale-token",
        static_cast<int64_t>(time(nullptr)) * 1000).state ==
        fortis::prices::PriceSpotLookupState::kExpired);
    const size_t frames_before_stale_result = kv_frame_count(directory);
    CompletionState stale_foreground_join{};
    const char* stale_canonical = "stale-token";
    const std::string_view stale_canonical_view{stale_canonical, strlen(stale_canonical)};
    (void)AcceptedId(stale_owner.Submit(&stale_canonical_view, 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &stale_foreground_join));
    wait_state(stale_foreground_join);
    assert(stale_foreground_join.status == SpotProviderOwnerResultStatus::kComplete &&
           cache.Lookup("usd", "stale-token",
               static_cast<int64_t>(time(nullptr)) * 1000).state ==
               fortis::prices::PriceSpotLookupState::kFresh &&
           cache.Lookup("usd", "stale-token",
               static_cast<int64_t>(time(nullptr)) * 1000).value.price == 42.0);
    assert(kv_frame_count(directory) == frames_before_stale_result + 1);

    // Background failure keeps the stale/floor record intact and writes no
    // provider negative.
    seed_stale(cache, "stale-fail", 13.0);
    CompletionState stale_failure{};
    const char* stale_fail_id = "stale-fail";
    const std::string_view stale_fail_view{stale_fail_id, strlen(stale_fail_id)};
    (void)AcceptedId(stale_owner.Submit(&stale_fail_view, 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &stale_failure));
    wait_state(stale_failure);
    assert(stale_failure.status == SpotProviderOwnerResultStatus::kComplete &&
           stale_owner.active_request_count() == 0);
    for (size_t i = 0; i < 800 && access(argv[11], F_OK) != 0; ++i) pause_ms(10);
    assert(access(argv[11], F_OK) == 0);
    pause_ms(100);
    const auto stale_after_failure = cache.Lookup("usd", "stale-fail",
        static_cast<int64_t>(time(nullptr)) * 1000);
    assert(stale_after_failure.state == fortis::prices::PriceSpotLookupState::kStale &&
           stale_after_failure.value.price == 13.0);

    // Saturate both CoinGecko lane slots with foreground work. Stale response
    // still completes from cache, and its background refresh is shed.
    CompletionState load_one{}, load_two{}, shed_response{};
    const char* load_one_id = "fg-load-one";
    const char* load_two_id = "fg-load-two";
    const std::string_view load_one_view{load_one_id, strlen(load_one_id)};
    const std::string_view load_two_view{load_two_id, strlen(load_two_id)};
    (void)AcceptedId(stale_owner.Submit(&load_one_view, 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &load_one));
    for (size_t i = 0; i < 800 && access(argv[8], F_OK) != 0; ++i) pause_ms(10);
    assert(access(argv[8], F_OK) == 0);
    (void)AcceptedId(stale_owner.Submit(&load_two_view, 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &load_two));
    for (size_t i = 0; i < 800 && access(argv[9], F_OK) != 0; ++i) pause_ms(10);
    assert(access(argv[9], F_OK) == 0);
    seed_stale(cache, "shed-stale", 21.0);
    const char* shed_id = "shed-stale";
    const std::string_view shed_view{shed_id, strlen(shed_id)};
    const size_t active_before_shed = stale_owner.active_request_count();
    assert(active_before_shed == 2);
    (void)AcceptedId(stale_owner.Submit(&shed_view, 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &shed_response));
    wait_state(shed_response);
    assert(shed_response.status == SpotProviderOwnerResultStatus::kComplete &&
           stale_owner.active_request_count() == active_before_shed);
    pause_ms(100);
    wait_state(load_one);
    wait_state(load_two);

    stale_owner.Stop();

    // A large stale burst is capped at 400 background-only flights, leaving
    // fixed foreground registry headroom for a cold miss.
    auto burst_owner = fortis::testing::StartTestSpotOwner(cache, series, config, nullptr,
        &PriceOwnerEnvClock(), [&](SpotProviderOwnerStartup& startup) {
          return startup.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr);
        });
    constexpr size_t kBurstIds = 400;
    constexpr size_t kBurstRequests = kBurstIds / kSpotResolutionMaxIds;
    char burst_ids[kBurstIds][16]{};
    std::string_view burst_views[kBurstIds]{};
    const int64_t burst_old_time = static_cast<int64_t>(time(nullptr)) * 1000 - 31'000;
    for (size_t base = 0; base < kBurstIds; base += kSpotResolutionMaxIds) {
      fortis::prices::PriceSpotQuoteInput quotes[kSpotResolutionMaxIds]{};
      for (size_t offset = 0; offset < kSpotResolutionMaxIds; ++offset) {
        const size_t i = base + offset;
        snprintf(burst_ids[i], sizeof(burst_ids[i]), "bg%03zu", i);
        burst_views[i] = {burst_ids[i], strlen(burst_ids[i])};
        quotes[offset] = {burst_ids[i], 1.0 + i, 0, 0, false, false,
                          burst_old_time, 0};
      }
      assert(cache.StoreProviderBatch("usd", "coingecko", quotes,
          kSpotResolutionMaxIds, nullptr, 0, true, burst_old_time) ==
          fortis::prices::PriceSpotStoreStatus::kStored);
    }
    CompletionState background_burst[kBurstRequests]{};
    for (size_t i = 0; i < kBurstRequests; ++i)
      (void)AcceptedId(burst_owner.Submit(burst_views + i * kSpotResolutionMaxIds, kSpotResolutionMaxIds, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &background_burst[i]));
    for (size_t i = 0; i < kBurstRequests; ++i) {
      wait_state(background_burst[i]);
      assert(background_burst[i].status == SpotProviderOwnerResultStatus::kComplete);
    }
    for (size_t i = 0; i < 800 && access(argv[12], F_OK) != 0; ++i) pause_ms(10);
    assert(access(argv[12], F_OK) == 0);
    // Due background flights wait for the active provider batch. They must
    // not spin the owner thread while the provider is still occupied.
    const uint64_t cpu_before_wait = process_cpu_ms();
    pause_ms(500);
    assert(process_cpu_ms() - cpu_before_wait < 300);

    seed_stale(cache, "burst-overflow", 3.0);
    CompletionState overflow_stale{};
    const char* overflow_id = "burst-overflow";
    const std::string_view overflow_view{overflow_id, strlen(overflow_id)};
    (void)AcceptedId(burst_owner.Submit(&overflow_view, 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &overflow_stale));
    wait_state(overflow_stale);
    assert(overflow_stale.status == SpotProviderOwnerResultStatus::kComplete &&
           burst_owner.active_request_count() == 0);

    CompletionState foreground_after_burst{};
    const char* burst_foreground_id = "burst-foreground";
    const std::string_view burst_foreground_view{
        burst_foreground_id, strlen(burst_foreground_id)};
    (void)AcceptedId(burst_owner.Submit(&burst_foreground_view, 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &foreground_after_burst));
    for (size_t i = 0; i < 800 && access(argv[13], F_OK) != 0; ++i) pause_ms(10);
    assert(access(argv[13], F_OK) == 0);
    wait_state(foreground_after_burst);
    assert(foreground_after_burst.status == SpotProviderOwnerResultStatus::kComplete);
    burst_owner.Stop();

    char cancel_id[] = "cancelme";
    std::string_view cancel_view{cancel_id, sizeof(cancel_id) - 1};
    reset_completion();
    uint64_t cancelled_request = 0;
    cancelled_request = AcceptedId(owner.Submit(&cancel_view, 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 10000, on_completion, &completion));
    // The fixture creates this marker only after the request is active in TLS.
    bool request_seen = false;
    for (size_t i = 0; i < 800 && !request_seen; ++i) {
      request_seen = access(argv[3], F_OK) == 0;
      if (!request_seen) pause_ms(10);
    }
    const bool cancel_accepted = owner.Cancel(cancelled_request);
    if (!request_seen || !cancel_accepted)
      fprintf(stderr, "cancel fixture seen=%d accepted=%d active=%zu id=%llu\n",
          request_seen, cancel_accepted, owner.active_request_count(),
          static_cast<unsigned long long>(cancelled_request));
    assert(request_seen && cancel_accepted);
    wait_completion();
    assert(completion.status == SpotProviderOwnerResultStatus::kCancelled &&
           completion.callback_count == 1);
    for (size_t i = 0; i < 300 && owner.active_request_count() != 0; ++i)
      pause_ms(10);
    assert(owner.active_request_count() == 0);

    // The canceled context is reclaimed only after its TLS result drains, and
    // the freed slot can then serve a cached response in protobuf form.
    reset_completion();
    const std::string_view cached_view{ids[0], 100};
    (void)AcceptedId(owner.Submit(&cached_view, 1, {"usd", 3}, SpotProviderOwnerFormat::kProtobuf, monotonic_ms() + 5000, on_completion, &completion));
    wait_completion();
    assert(completion.status == SpotProviderOwnerResultStatus::kComplete &&
           strcmp(completion.content_type, "application/protobuf") == 0 &&
           completion.body_length > 0);

    reset_completion();
    const char* asa = "asa:31566704";
    const std::string_view asa_view{asa, strlen(asa)};
    (void)AcceptedId(owner.Submit(&asa_view, 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 5000, on_completion, &completion));
    wait_completion();
    assert(completion.status == SpotProviderOwnerResultStatus::kComplete &&
           completion.callback_count == 1 && completion.body_length > 0);
    // A partial primary reply retains its good quote and falls back only for
    // mapped USD misses. Concurrent callers share flights; an HTTP-200 provider
    // error must not write a negative cache entry or suppress a later retry.
    const std::string_view native_ids[] = {"tron", "avalanche-2", "the-open-network"};
    CompletionState fallback_a, fallback_b;
    (void)AcceptedId(owner.Submit(native_ids, 3, "usd", SpotProviderOwnerFormat::kJson,
        monotonic_ms() + 15000, on_completion, &fallback_a));
    (void)AcceptedId(owner.Submit(native_ids, 3, "usd", SpotProviderOwnerFormat::kJson,
        monotonic_ms() + 15000, on_completion, &fallback_b));
    wait_state(fallback_a);
    wait_state(fallback_b);
    for (auto* answer : {&fallback_a, &fallback_b}) {
      assert(answer->status == SpotProviderOwnerResultStatus::kComplete && answer->callback_count == 1);
      assert(strstr(reinterpret_cast<const char*>(answer->body), "kraken"));
    }
    const int64_t fallback_now = static_cast<int64_t>(time(nullptr)) * 1000;
    auto primary = cache.Lookup("usd", "tron", fallback_now);
    assert(primary.state == PriceSpotLookupState::kFresh && primary.value.price == 0.25 &&
           strcmp(primary.value.provider, "coingecko") == 0);
    auto alternative = cache.Lookup("usd", "avalanche-2", fallback_now, true);
    assert(alternative.state == PriceSpotLookupState::kFresh && alternative.value.price == 2.5 &&
           strcmp(alternative.value.provider, "kraken") == 0 &&
           alternative.value.provider_observed_at_ms > fallback_now - 10000);
    assert(cache.Lookup("usd", "the-open-network", fallback_now).state == PriceSpotLookupState::kMissing);
    reset_completion();
    (void)AcceptedId(owner.Submit(&native_ids[2], 1, "usd", SpotProviderOwnerFormat::kJson,
        monotonic_ms() + 15000, on_completion, &completion));
    wait_completion();
    assert(cache.Lookup("usd", "the-open-network", static_cast<int64_t>(time(nullptr)) * 1000).state ==
           PriceSpotLookupState::kFresh);
    // Expire TRX completely, then prove a primary 429 produces a fresh, correctly
    // attributed fallback instead of relabeling the old durable floor.
    const int64_t expired = fallback_now - kPriceSpotStaleMs - 60000;
    const PriceSpotQuoteInput old_trx{"tron", 0.1, 0, 0, false, false, expired, 0};
    assert(cache.StoreProviderBatch("usd", "coingecko", &old_trx, 1, nullptr, 0, true, expired) ==
           PriceSpotStoreStatus::kStored);
    reset_completion();
    (void)AcceptedId(owner.Submit(native_ids, 3, "usd", SpotProviderOwnerFormat::kJson,
        monotonic_ms() + 15000, on_completion, &completion));
    wait_completion();
    primary = cache.Lookup("usd", "tron", static_cast<int64_t>(time(nullptr)) * 1000);
    assert(primary.state == PriceSpotLookupState::kFresh && primary.value.price == 2.5 &&
           strcmp(primary.value.provider, "kraken") == 0);
    reset_completion();
    (void)AcceptedId(owner.Submit(native_ids, 3, "usd", SpotProviderOwnerFormat::kProtobuf,
        monotonic_ms() + 5000, on_completion, &completion));
    wait_completion();
    assert(completion.status == SpotProviderOwnerResultStatus::kComplete && completion.body_length > 0);
    // There are no USD-as-EUR conversions: unsupported fiat uses the primary
    // policy and cannot reuse USD fallback prices.
    reset_completion();
    (void)AcceptedId(owner.Submit(native_ids, 3, "eur", SpotProviderOwnerFormat::kJson,
        monotonic_ms() + 10000, on_completion, &completion));
    wait_completion();
    assert(cache.Lookup("eur", "tron", fallback_now).state == PriceSpotLookupState::kMissing);
    const std::string_view broad_ids[] = {"bitcoin", "ethereum", "binancecoin", "ripple", "solana", "algorand",
        "eip155:1:erc20:0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48"};
    reset_completion();
    (void)AcceptedId(owner.Submit(broad_ids, 7, "usd", SpotProviderOwnerFormat::kJson,
        monotonic_ms() + 15000, on_completion, &completion));
    wait_completion();
    assert(completion.status == SpotProviderOwnerResultStatus::kComplete);
    for (const auto id : broad_ids) {
      const auto quote = cache.Lookup("usd", id.data(), static_cast<int64_t>(time(nullptr)) * 1000, true);
      assert(quote.state == PriceSpotLookupState::kFresh && quote.value.price == 61.25);
      assert(strcmp(quote.value.provider, "defillama") == 0);
      assert(quote.value.provider_observed_at_ms > 0);
    }
    const std::string_view wkc_id = "eip155:56:erc20:0x6ec90334d89dbdc89e08a133271be3d104128edb";
    reset_completion();
    (void)AcceptedId(owner.Submit(&wkc_id, 1, "usd", SpotProviderOwnerFormat::kJson,
        monotonic_ms() + 15000, on_completion, &completion));
    wait_completion();
    const auto converted = cache.Lookup("usd", wkc_id.data(), static_cast<int64_t>(time(nullptr)) * 1000, true);
    assert(completion.status == SpotProviderOwnerResultStatus::kComplete);
    assert(converted.state == PriceSpotLookupState::kFresh && converted.value.price == .04 * .98);
    assert(strcmp(converted.value.provider, "mexc-defillama") == 0);
    assert(converted.value.provider_observed_at_ms > 0);
    assert(converted.value.provider_observed_at_ms <= static_cast<int64_t>(time(nullptr)) * 1000 - 1000);
    const std::string_view wkc_slug = "wiki-cat";
    for (int attempt = 0; attempt < 3; ++attempt) {
      reset_completion();
      (void)AcceptedId(owner.Submit(&wkc_slug, 1, "usd", SpotProviderOwnerFormat::kJson,
          monotonic_ms() + 15000, on_completion, &completion));
      wait_completion();
      const auto value = cache.Lookup("usd", "wiki-cat", static_cast<int64_t>(time(nullptr)) * 1000, true);
      if (attempt < 2) assert(value.state == PriceSpotLookupState::kMissing);
      else assert(value.state == PriceSpotLookupState::kFresh && value.value.price == .04 * .98);
    }
    char llama_marker[1024]{};
    snprintf(llama_marker, sizeof(llama_marker), "%s.llama", argv[3]);
    const std::string_view llama_cancel = "llama-cancel";
    reset_completion();
    const auto llama_request = AcceptedId(owner.Submit(&llama_cancel, 1, "usd", SpotProviderOwnerFormat::kJson,
        monotonic_ms() + 10000, on_completion, &completion));
    bool llama_active = false;
    for (size_t i = 0; i < 800 && !llama_active; ++i) {
      llama_active = access(llama_marker, F_OK) == 0;
      if (!llama_active) pause_ms(10);
    }
    assert(llama_active && owner.Cancel(llama_request));
    wait_completion();
    assert(completion.status == SpotProviderOwnerResultStatus::kCancelled && completion.callback_count == 1);
    for (size_t i = 0; i < 300 && owner.active_request_count(); ++i) pause_ms(10);
    assert(owner.active_request_count() == 0);
    assert(cache.Lookup("usd", "llama-cancel", static_cast<int64_t>(time(nullptr)) * 1000).state == PriceSpotLookupState::kMissing);
    // Cancel while fallback TLS is active, then shut down a second flight.
    // Neither can publish a late quote or strand an orphaned shared flight.
    const PriceSpotQuoteInput old_avax{"avalanche-2", 0.1, 0, 0, false, false, expired, 0};
    assert(cache.StoreProviderBatch("usd", "coingecko", &old_avax, 1, nullptr, 0, true, expired) ==
           PriceSpotStoreStatus::kStored);
    char fallback_marker[1024]{};
    snprintf(fallback_marker, sizeof(fallback_marker), "%s.kraken", argv[3]);
    reset_completion();
    const auto cancel_fallback = AcceptedId(owner.Submit(&native_ids[1], 1, "usd", SpotProviderOwnerFormat::kJson,
        monotonic_ms() + 10000, on_completion, &completion));
    bool fallback_active = false;
    for (size_t i = 0; i < 800 && !fallback_active; ++i) {
      fallback_active = access(fallback_marker, F_OK) == 0;
      if (!fallback_active) pause_ms(10);
    }
    assert(fallback_active);
    assert(owner.Cancel(cancel_fallback));
    wait_completion();
    assert(completion.status == SpotProviderOwnerResultStatus::kCancelled && completion.callback_count == 1);
    for (size_t i = 0; i < 300 && owner.active_request_count(); ++i) pause_ms(10);
    assert(owner.active_request_count() == 0);
    assert(unlink(fallback_marker) == 0);
    reset_completion();
    (void)AcceptedId(owner.Submit(&native_ids[1], 1, "usd", SpotProviderOwnerFormat::kJson,
        monotonic_ms() + 10000, on_completion, &completion));
    fallback_active = false;
    for (size_t i = 0; i < 800 && !fallback_active; ++i) {
      fallback_active = access(fallback_marker, F_OK) == 0;
      if (!fallback_active) pause_ms(10);
    }
    assert(fallback_active);
    owner.Stop();
    assert(completion.status == SpotProviderOwnerResultStatus::kShutdown && completion.callback_count == 1);
    assert(cache.Lookup("usd", "avalanche-2", fallback_now).state == PriceSpotLookupState::kExpired);

    auto serialization_failure = fortis::testing::StartTestSpotOwner(cache, series, config, nullptr,
        &PriceOwnerEnvClock(), [&](SpotProviderOwnerStartup& startup) {
          return startup.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr, 1);
        });
    reset_completion();
    (void)AcceptedId(serialization_failure.Submit(&cached_view, 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 5000, on_completion, &completion));
    wait_completion();
    assert(completion.status == SpotProviderOwnerResultStatus::kInternalFailure &&
           completion.callback_count == 1 && completion.body_length == 0);
    serialization_failure.Stop();

    // The owner admits exactly eight fixed request contexts. Stop while a TLS
    // request is active and verify each accepted callback fires once.
    auto stopping_owner = fortis::testing::StartTestSpotOwner(cache, series, config, nullptr,
        &PriceOwnerEnvClock(), [&](SpotProviderOwnerStartup& startup) {
          return startup.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr);
        });
    static char capacity_ids[kSpotProviderOwnerMaxRequests][16];
    std::string_view capacity_views[kSpotProviderOwnerMaxRequests];
    for (size_t i = 0; i < kSpotProviderOwnerMaxRequests; ++i) {
      snprintf(capacity_ids[i], sizeof(capacity_ids[i]), "capacity%zu", i);
      capacity_views[i] = {capacity_ids[i], strlen(capacity_ids[i])};
      (void)AcceptedId(stopping_owner.Submit(&capacity_views[i], 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 20000, on_indexed_completion, &stop_completions[i]));
    }
    assert(stopping_owner.Submit(&capacity_views[0], 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 20000, on_completion, &completion).status() == SpotProviderOwnerSubmitStatus::kBusy);
    assert(stopping_owner.active_request_count() == kSpotProviderOwnerMaxRequests);
    bool active_tls = false;
    for (size_t i = 0; i < 300 && !active_tls; ++i) {
      active_tls = access(argv[4], F_OK) == 0;
      if (!active_tls) pause_ms(10);
    }
    assert(active_tls);
    pthread_t stop_thread{};
    assert(pthread_create(&stop_thread, nullptr, stop_owner_thread,
                          &stopping_owner) == 0);
    SpotProviderOwnerSubmitStatus during_stop = SpotProviderOwnerSubmitStatus::kBusy;
    for (size_t i = 0; i < 200 && during_stop == SpotProviderOwnerSubmitStatus::kBusy; ++i) {
      during_stop = stopping_owner.Submit(&capacity_views[0], 1, {"usd", 3}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 20000, on_completion, &completion).status();
      if (during_stop == SpotProviderOwnerSubmitStatus::kBusy) pause_ms(5);
    }
    assert(during_stop == SpotProviderOwnerSubmitStatus::kStopping ||
           during_stop == SpotProviderOwnerSubmitStatus::kNotRunning);
    assert(pthread_join(stop_thread, nullptr) == 0);
    assert(stopping_owner.active_request_count() == 0);
    for (const auto& saved : stop_completions)
      assert(saved.callback_count == 1 &&
             saved.status == SpotProviderOwnerResultStatus::kShutdown);

    // Reopen the same append-only store and confirm successful atomic batches
    // survive restart while failed preparations left no partial quote rows.
    store.Close();
    assert(store.Open(directory));
    fortis::prices::PriceSeriesSpot recovered_series{};
    assert(series.GetSpot("usd", shared_slow, now, &recovered_series) ==
           fortis::prices::PriceSeriesReadStatus::kFound);
    assert(recovered_series.price == 4.0);
    assert(cache.Lookup("usd", "series-fails", now).state ==
           fortis::prices::PriceSpotLookupState::kMissing);
    assert(series.GetSpot("usd", "series-fails", now, &recovered_series) ==
           fortis::prices::PriceSeriesReadStatus::kCorrupt);
    assert(series.GetSpot("usd", "cache-fails", now, &recovered_series) ==
           fortis::prices::PriceSeriesReadStatus::kMissing);
    assert(cache.Lookup("usd", "cache-fails", now).state ==
           fortis::prices::PriceSpotLookupState::kCorrupt);
  }
  remove_store(directory);
  puts("native CoinGecko spot provider owner TLS integration passed");
}
