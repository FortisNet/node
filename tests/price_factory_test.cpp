#include "native_config.h"
#include <string_view>
#include "price_spot_service.h"
#include "price_series_store.h"
#include "factory_test_support.h"
#include <atomic>
#include <chrono>
#include <thread>

namespace {
using fortis::testing::Require;
void spot_factory_clock_roles() {
  static_assert(!std::is_default_constructible_v<fortis::prices::SpotProviderOwner>);
  static_assert(std::is_nothrow_move_constructible_v<fortis::prices::SpotProviderOwner>);
  struct Wall final : fortis::base::Clock {
    int64_t NowMs() override { return 123456; }
    uint64_t MonotonicMs() override { abort(); }
  } wall;
  struct Monotonic final : fortis::base::Clock {
    int64_t NowMs() override { abort(); }
    uint64_t MonotonicMs() override { return 7000; }
  } monotonic;
  fortis::testing::TemporaryKv fixture;
  fortis::prices::PriceSpotCache cache(fixture.kv);
  fortis::prices::PriceSeriesStore series(fixture.kv);
  fortis::config::NativeConfig config;
  config.ca_bundle_configured = true;
  strcpy(config.ca_bundle_path, "/unused-without-outbound-jobs");
  fortis::prices::SpotProviderOwnerOptions options;
  options.wall_clock = &wall;
  options.monotonic_clock = &monotonic;
  std::atomic<uint64_t> completed{0};
  auto made = fortis::prices::SpotProviderOwner::Create(cache, series, config, options);
  Require(made.ok(), "spot factory uses distinct clock roles");
  auto original = made.TakeValue();
  auto owner = std::move(original);
  const std::string_view id{};
  const auto submitted = owner.Submit(&id, 1, {"usd", 3}, fortis::prices::SpotProviderOwnerFormat::kJson, 8000,
      [](void* raw, const fortis::prices::SpotProviderOwnerResult& result) {
        static_cast<std::atomic<uint64_t>*>(raw)->store(result.request_id);
      }, &completed);
  Require(submitted.ticket(), "owner validates deadlines with monotonic clock, not wall/system time");
  for (size_t attempt = 0; attempt < 2000 && !completed.load(); ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  owner.Stop();
  Require(completed.load() == submitted.ticket()->request_id, "moved initialized owner retains callback identity");
}
}
int main() {
  const int lifecycle = fortis::testing::FactoryLifecycle("price_factory_test", fortis::config::NativeConfig{},
      [](auto& config) {
        config.ca_bundle_configured = true;
        strcpy(config.ca_bundle_path, "/unused-without-outbound-jobs");
      },
      [](const auto& config, auto& kv, auto& coordinator) { return fortis::prices::PriceSpotService::Create(config, {kv, coordinator}); });
  fortis::testing::UnitSuite suite;
  suite.Run("spot owner clock roles and handle moves", spot_factory_clock_roles);
  return lifecycle | suite.Finish("spot_factory_test");
}
