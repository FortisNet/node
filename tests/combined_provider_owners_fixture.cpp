#include "native_config.h"
#include <string_view>
#include "env_clock.h"
#include "kv_store.h"
#include "price_history_backfill_owner.h"
#include "price_series_store.h"
#include "price_spot_cache.h"
#include "provider_execution_coordinator.h"
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
using namespace fortis::providers;

namespace {
uint64_t AcceptedId(const fortis::prices::SpotProviderOwnerSubmission& submitted) {
  assert(submitted.status() == fortis::prices::SpotProviderOwnerSubmitStatus::kAccepted);
  assert(submitted.ticket() && submitted.ticket()->request_id);
  return submitted.ticket()->request_id;
}

struct State {
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
  bool done = false;
  SpotProviderOwnerResultStatus spot = SpotProviderOwnerResultStatus::kInternalFailure;
  PriceHistoryOwnerResultStatus history = PriceHistoryOwnerResultStatus::kUpstreamFailure;
};

uint64_t monotonic_ms() {
  timespec now{}; assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}
bool allow_loopback(void*, uint32_t address) { return address == 0x7f000001U; }

void spot_done(void* context, const SpotProviderOwnerResult& result) {
  auto* state = static_cast<State*>(context);
  pthread_mutex_lock(&state->mutex);
  state->spot = result.status; state->done = true;
  pthread_cond_broadcast(&state->changed); pthread_mutex_unlock(&state->mutex);
}
void history_done(void* context, const PriceHistoryOwnerResult& result) {
  auto* state = static_cast<State*>(context);
  pthread_mutex_lock(&state->mutex);
  state->history = result.status; state->done = true;
  pthread_cond_broadcast(&state->changed); pthread_mutex_unlock(&state->mutex);
}
void wait(State& state, long seconds = 20) {
  pthread_mutex_lock(&state.mutex);
  timespec deadline{}; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += seconds;
  while (!state.done) assert(pthread_cond_timedwait(&state.changed, &state.mutex,
                                                    &deadline) == 0);
  pthread_mutex_unlock(&state.mutex);
}
void wait_file(const char* root, const char* marker) {
  char path[512]; snprintf(path, sizeof(path), "%s/%s", root, marker);
  const uint64_t deadline = monotonic_ms() + 10000;
  while (access(path, F_OK) != 0 && monotonic_ms() < deadline) usleep(10000);
  assert(access(path, F_OK) == 0);
}
void reset_marker(const char* root, const char* marker) {
  char path[512]; snprintf(path, sizeof(path), "%s/%s", root, marker); unlink(path);
}
void signal_marker(const char* root, const char* marker) {
  char path[512]; snprintf(path, sizeof(path), "%s/%s", root, marker);
  FILE* file = fopen(path, "wb"); assert(file); fclose(file);
}
void wait_for_jobs(ProviderExecutionCoordinator& coordinator, size_t count) {
  const uint64_t deadline = monotonic_ms() + 5000;
  while (coordinator.in_flight_count() < count && monotonic_ms() < deadline) usleep(1000);
  assert(coordinator.in_flight_count() >= count);
}

uint64_t submit_spot(SpotProviderOwner& owner, const char* coin, const char* fiat,
                     State& state) {
  const std::string_view id{coin, strlen(coin)};
  uint64_t request = 0;
  request = AcceptedId(owner.Submit(&id, 1, {fiat, strlen(fiat)}, SpotProviderOwnerFormat::kJson, monotonic_ms() + 25000, spot_done, &state));
  return request;
}
void submit_history(PriceHistoryBackfillOwner& owner, const char* coin, State& state) {
  uint64_t request = 0;
  assert(owner.Submit(coin, coin, "usd", fortis::prices::PriceSeriesInterval::k1h,
      1767225600, 1767229200,
      monotonic_ms() + 25000, history_done, &state, &request) ==
      PriceHistoryOwnerSubmitStatus::kAccepted);
  assert(request);
}


void* stop_spot(void* context) {
  static_cast<SpotProviderOwner*>(context)->Stop(); return nullptr;
}
}

int main(int argc, char** argv) {
  assert(argc == 4);
  // FORTIS_TEST_HISTORY_NOW_FILE / _MS move the history owner's clock.
  SetPriceHistoryCoverageClock(&HistoryCoverageEnvClock());
  char directory[] = "/tmp/fortis-combined-owner-XXXXXX";
  assert(mkdtemp(directory));
  NativeConfig config{}; config.ca_bundle_configured = true;
  snprintf(config.ca_bundle_path, sizeof(config.ca_bundle_path), "%s", argv[2]);
  {
    fortis::storage::KvStore store; assert(store.Open(directory));
    fortis::prices::PriceSpotCache cache(store); fortis::prices::PriceSeriesStore series(store);

    // Both owner leases share one CoinGecko budget: max concurrency two and
    // burst four across the aggregate eight requests.
    {
      ProviderExecutionCoordinator coordinator;
      assert(coordinator.valid() && coordinator.Start());
      auto spot = fortis::testing::StartTestSpotOwner(cache, series, config, &coordinator,
          &PriceOwnerEnvClock(), [&](SpotProviderOwnerStartup& startup) {
            return startup.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr);
          });
      PriceHistoryBackfillOwner history(series, config, &coordinator);
      assert(history.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr));
      State spot_states[4]{}, history_states[4]{};
      const char* spot_fiats[] = {"usd", "eur", "gbp", "cad"};
      char spot_ids[4][24]{}; char history_ids[4][24]{};
      for (size_t i = 0; i < 4; ++i) {
        snprintf(spot_ids[i], sizeof(spot_ids[i]), "spot-burst-%zu", i);
        snprintf(history_ids[i], sizeof(history_ids[i]), "hist-burst-%zu", i);
        submit_spot(spot, spot_ids[i], spot_fiats[i], spot_states[i]);
        submit_history(history, history_ids[i], history_states[i]);
      }
      for (size_t i = 0; i < 4; ++i) {
        wait(spot_states[i]); wait(history_states[i]);
        assert(spot_states[i].spot == SpotProviderOwnerResultStatus::kComplete);
        assert(history_states[i].history == PriceHistoryOwnerResultStatus::kComplete);
      }
      history.Stop(); spot.Stop();
      assert(coordinator.in_flight_count() == 0 && coordinator.Stop());
    }

    // Cancellation and Stop on the spot owner must not cancel or drop an
    // unrelated history owner's work on the shared coordinator.
    {
      ProviderExecutionCoordinator coordinator;
      assert(coordinator.valid() && coordinator.Start());
      auto spot = fortis::testing::StartTestSpotOwner(cache, series, config, &coordinator,
          &PriceOwnerEnvClock(), [&](SpotProviderOwnerStartup& startup) {
            return startup.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr);
          });
      PriceHistoryBackfillOwner history(series, config, &coordinator);
      assert(history.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr));
      State cancelled_spot{}, surviving_history{};
      const uint64_t cancel_id = submit_spot(spot, "spot-cancel", "usd", cancelled_spot);
      submit_history(history, "hist-cancel-peer", surviving_history);
      wait_file(argv[3], "spot_cancel-active");
      wait_file(argv[3], "hist_cancel_peer-active");
      assert(spot.Cancel(cancel_id)); wait(cancelled_spot);
      assert(cancelled_spot.spot == SpotProviderOwnerResultStatus::kCancelled);
      wait(surviving_history);
      assert(surviving_history.history == PriceHistoryOwnerResultStatus::kComplete);
      wait_file(argv[3], "spot_cancel-done");

      reset_marker(argv[3], "spot_stop-active");
      reset_marker(argv[3], "hist_survivor-active");
      reset_marker(argv[3], "spot_stop-done");
      State stopped_spot{}, survivor{};
      submit_spot(spot, "spot-stop", "eur", stopped_spot);
      submit_history(history, "hist-survivor", survivor);
      wait_file(argv[3], "spot_stop-active");
      wait_file(argv[3], "hist_survivor-active");
      pthread_t stopper{};
      assert(pthread_create(&stopper, nullptr, stop_spot, &spot) == 0);
      wait(survivor);
      assert(survivor.history == PriceHistoryOwnerResultStatus::kComplete);
      pthread_join(stopper, nullptr);
      wait(stopped_spot);
      assert(stopped_spot.spot == SpotProviderOwnerResultStatus::kShutdown);
      history.Stop();
      assert(coordinator.in_flight_count() == 0 && coordinator.Stop());
      // Stop cancelled the in-flight batch; let the server finish the
      // abandoned handler before the next section counts concurrency (as
      // for spot-cancel above).
      wait_file(argv[3], "spot_stop-done");
    }

    // A 429 opens the shared CoinGecko breaker for requests from either owner.
    {
      ProviderExecutionCoordinator coordinator;
      assert(coordinator.valid() && coordinator.Start());
      auto spot = fortis::testing::StartTestSpotOwner(cache, series, config, &coordinator,
          &PriceOwnerEnvClock(), [&](SpotProviderOwnerStartup& startup) {
            return startup.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr);
          });
      PriceHistoryBackfillOwner history(series, config, &coordinator);
      assert(history.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr));
      State limited{}, blocked{}, independent{};
      submit_spot(spot, "spot-429", "usd", limited); wait(limited);
      submit_spot(spot, "spot-after-429", "usd", blocked);
      submit_history(history, "hist-after-spot-429", independent);
      wait(blocked); wait(independent);
      history.Stop(); spot.Stop();
      assert(coordinator.in_flight_count() == 0 && coordinator.Stop());
    }

    // Saturate all eight coordinator records from one owner, then submit work
    // from its peer. Repeat in the reverse direction. The server holds the
    // first network wave until the peer has had time to receive temporary Busy.
    {
      ProviderExecutionCoordinator coordinator;
      assert(coordinator.valid() && coordinator.Start());
      auto spot = fortis::testing::StartTestSpotOwner(cache, series, config, &coordinator,
          &PriceOwnerEnvClock(), [&](SpotProviderOwnerStartup& startup) {
            return startup.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr);
          });
      PriceHistoryBackfillOwner history(series, config, &coordinator);
      assert(history.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr));
      State history_states[8]{}, spot_peer{};
      char coin[32]{};
      for (size_t i = 0; i < 8; ++i) {
        snprintf(coin, sizeof(coin), "hist-full-%zu", i);
        submit_history(history, coin, history_states[i]);
      }
      wait_for_jobs(coordinator, 8);
      submit_spot(spot, "spot-after-history-full", "usd", spot_peer);
      usleep(150000);
      signal_marker(argv[3], "history_saturation_release");
      for (State& state : history_states) {
        wait(state);
        assert(state.history == PriceHistoryOwnerResultStatus::kComplete);
      }
      wait(spot_peer);
      assert(spot_peer.spot == SpotProviderOwnerResultStatus::kComplete);
      history.Stop(); spot.Stop();
      assert(coordinator.in_flight_count() == 0 && coordinator.Stop());
    }
    {
      ProviderExecutionCoordinator coordinator;
      assert(coordinator.valid() && coordinator.Start());
      auto spot = fortis::testing::StartTestSpotOwner(cache, series, config, &coordinator,
          &PriceOwnerEnvClock(), [&](SpotProviderOwnerStartup& startup) {
            return startup.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr);
          });
      PriceHistoryBackfillOwner history(series, config, &coordinator);
      assert(history.StartRedirected("localhost", argv[1], argv[2], allow_loopback, nullptr));
      const char* fiats[8] = {"usd", "eur", "gbp", "cad", "aud", "chf", "cny", "jpy"};
      State spot_states[8]{}, history_peer{};
      uint64_t spot_ids[8]{};
      char coin[32]{};
      for (size_t i = 0; i < 8; ++i) {
        snprintf(coin, sizeof(coin), "spot-full-%zu", i);
        spot_ids[i] = submit_spot(spot, coin, fiats[i], spot_states[i]);
      }
      wait_for_jobs(coordinator, 8);
      submit_history(history, "hist-after-spot-full", history_peer);
      usleep(150000);
      assert(spot.Cancel(spot_ids[7]));
      wait(spot_states[7]);
      assert(spot_states[7].spot == SpotProviderOwnerResultStatus::kCancelled);
      signal_marker(argv[3], "spot_saturation_release");
      for (size_t i = 0; i < 7; ++i) {
        wait(spot_states[i]);
        assert(spot_states[i].spot == SpotProviderOwnerResultStatus::kComplete);
      }
      wait(history_peer);
      assert(history_peer.history == PriceHistoryOwnerResultStatus::kComplete);
      history.Stop(); spot.Stop();
      assert(coordinator.in_flight_count() == 0 && coordinator.Stop());
    }
  }
  char path[512]; snprintf(path, sizeof(path), "%s/fortis.kv", directory); unlink(path);
  snprintf(path, sizeof(path), "%s/fortis.kv.lock", directory); unlink(path);
  assert(rmdir(directory) == 0);
  puts("combined owners share the CoinGecko budget, breaker, cancellation, and shutdown safely");
  return 0;
}
