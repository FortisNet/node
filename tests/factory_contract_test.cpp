#include "native_config.h"
#include <string_view>
#include "ai_service.h"
#include "factory.h"
#include "service_wiring.h"
#include "provider_execution.h"
#include "token_provider.h"
#include "unit_test_support.h"

#include <optional>
#include <atomic>
#include <chrono>
#include <thread>
#include <type_traits>
#include <utility>

namespace {
using fortis::testing::Require;
using fortis::providers::ProviderExecutionAdapter;
using fortis::ai::AiConversationService;
void adapter_failure_and_move() {
  static_assert(!std::is_default_constructible_v<ProviderExecutionAdapter>);
  static_assert(std::is_nothrow_move_constructible_v<ProviderExecutionAdapter>);
  auto& clock = fortis::base::SystemClock();
  fortis::providers::ProviderLaneRegistry invalid(nullptr);
  auto rejected = ProviderExecutionAdapter::Create(invalid, clock);
  Require(!rejected && rejected.status().code() == fortis::base::StatusCode::kUnavailable, "failed adapter construction returns no object");
  fortis::providers::ProviderLaneRegistry lanes(&clock);
  auto made = ProviderExecutionAdapter::Create(lanes, clock);
  Require(made.ok(), "adapter factory");
  auto first = made.TakeValue();
  auto moved = std::move(first);
  moved.BeginShutdown();
  moved.JoinWorkers();
  Require(moved.workers_joined() && moved.live_job_count() == 0, "worker ownership survives handle moves");
}
void failed_planes_release_resources() {
  static_assert(!std::is_default_constructible_v<fortis::prices::PriceSpotService>);
  static_assert(!std::is_default_constructible_v<fortis::prices::PriceHistoryService>);
  static_assert(!std::is_default_constructible_v<fortis::discovery::DiscoveryService>);
  static_assert(!std::is_default_constructible_v<fortis::news::NewsService>);
  static_assert(!std::is_default_constructible_v<fortis::tokens::TokenService>);
  static_assert(!std::is_default_constructible_v<fortis::wallet::AddressService>);
  static_assert(!std::is_default_constructible_v<fortis::exchange::CrossChainService>);
  static_assert(!std::is_default_constructible_v<fortis::exchange::RobinhoodService>);
  static_assert(!std::is_default_constructible_v<fortis::exchange::DashPayService>);
  fortis::testing::TemporaryKv fixture;
  fortis::config::NativeConfig config;
  fortis::providers::ProviderExecutionCoordinator coordinator;
  Require(coordinator.Start(), "start dependency coordinator");
  Require(!fortis::prices::PriceSpotService::Create(config, {fixture.kv, coordinator}), "spot refusal is a factory error");
  auto history = fortis::prices::PriceHistoryService::Create(config, {fixture.kv, coordinator});
  Require(!history && !strcmp(history.status().message(), "backfill owner"), "history preserves refusal name");
  Require(!fortis::discovery::DiscoveryService::Create(config, {fixture.kv, coordinator}), "discovery refusal");
  Require(!fortis::news::NewsService::Create(config, {fixture.kv, coordinator}), "news refusal");
  Require(!fortis::tokens::TokenService::Create(config, {fixture.kv, coordinator, fixture.kv}), "token refusal");
  Require(!fortis::wallet::AddressService::Create(config, {fixture.kv, coordinator}), "address refusal");
  fortis::exchange::CrossChainConfig cross;
  fortis::exchange::RobinhoodConfig robinhood;
  fortis::exchange::DashPayConfig dash;
  Require(!fortis::exchange::CrossChainService::Create(cross, {coordinator}), "cross-chain refusal");
  Require(!fortis::exchange::RobinhoodService::Create(robinhood, {coordinator, &fixture.kv}), "Robinhood refusal");
  Require(!fortis::exchange::DashPayService::Create(dash, {coordinator}), "DashPay refusal");
  Require(coordinator.Stop(), "failed construction releases every coordinator lease");
}
void owned_factory_storage() {
  auto& clock = fortis::base::SystemClock();
  fortis::providers::ProviderLaneRegistry invalid(nullptr);
  auto failed = fortis::base::MakeOwned<ProviderExecutionAdapter>([&] {
    return ProviderExecutionAdapter::Create(invalid, clock);
  });
  Require(!failed, "factory failure releases outer handle storage");
  fortis::providers::ProviderLaneRegistry lanes(&clock);
  auto made = fortis::base::MakeOwned<ProviderExecutionAdapter>([&] {
    return ProviderExecutionAdapter::Create(lanes, clock);
  });
  Require(made.ok(), "retain separately allocated handle");
  auto adapter = made.TakeValue();
  adapter->BeginShutdown();
  adapter->JoinWorkers();
  Require(adapter->workers_joined(), "owned factory retains worker lifetime");
}
void allocation_failure_after_owner_start() {
  struct ThrowingDiscovery final : fortis::discovery::DiscoveryOwnerStarter {
    bool Start(fortis::discovery::DiscoveryProviderOwner& owner) override {
      Require(owner.Start(), "start discovery child before injected failure");
      throw std::bad_alloc();
    }
  } discovery;
  struct ThrowingToken final : fortis::tokens::TokenOwnerStarter {
    bool Start(fortis::providers::TokenProviderOwner& owner) override {
      Require(owner.Start(), "start token child before injected failure");
      throw std::bad_alloc();
    }
  } token;
  fortis::testing::TemporaryKv fixture;
  fortis::config::NativeConfig config;
  config.ca_bundle_configured = true;
  strcpy(config.ca_bundle_path, "/unused-without-outbound-jobs");
  fortis::providers::ProviderExecutionCoordinator coordinator;
  Require(coordinator.Start(), "start coordinator");
  auto failed_discovery = fortis::discovery::DiscoveryService::Create(config, {fixture.kv, coordinator}, {&discovery});
  Require(!failed_discovery && failed_discovery.status().code() == fortis::base::StatusCode::kResourceExhausted,
          "discovery allocation failure is a status after joining the started child");
  auto failed_token = fortis::tokens::TokenService::Create(config, {fixture.kv, coordinator, fixture.kv}, {&token});
  Require(!failed_token && failed_token.status().code() == fortis::base::StatusCode::kResourceExhausted,
          "token allocation failure joins the child without publishing its catalog");
  Require(coordinator.Stop(), "throwing starters leave no live leases or jobs");
}
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
void ai_failure_and_move() {
  static_assert(!std::is_default_constructible_v<AiConversationService>);
  static_assert(std::is_nothrow_move_constructible_v<AiConversationService>);
  fortis::testing::TemporaryKv store;
  fortis::ai::AiServiceConfig config;
  auto refused = AiConversationService::Create(config, {store.kv});
  Require(!refused && refused.status().code() == fortis::base::StatusCode::kInvalidArgument, "missing secret is factory failure");
  config.support_auth_secret_configured = true;
  memset(config.support_auth_secret, 's', 32);
  config.support_auth_secret_length = 32;
  auto made = AiConversationService::Create(config, {store.kv});
  Require(made.ok(), "AI factory with replies disabled");
  std::optional<AiConversationService> service(made.TakeValue());
  Require(service->WaitIdle(0), "no pending generation after creation");
  fortis::http::HttpRouter router;
  fortis::http::HttpResponseHandoff handoff;
  Require(service->Register(router, handoff), "moved service retains stable route target");
  service.reset();
  size_t length = 0;
  Require(store.kv.GetStatus("never-written", 0, nullptr, 0, &length) == fortis::storage::KvReadStatus::Missing,
          "service destruction retains borrowed KV");
  store.kv.Close();
  auto failed_store = AiConversationService::Create(config, {store.kv});
  Require(!failed_store, "failed initialization publishes no service");
}
}
int main() {
  fortis::testing::UnitSuite suite;
  suite.Run("adapter failure and moved worker ownership", adapter_failure_and_move);
  suite.Run("failed plane construction releases leases", failed_planes_release_resources);
  suite.Run("owned factory storage and rollback", owned_factory_storage);
  suite.Run("allocation failure after starting child owners", allocation_failure_after_owner_start);
  suite.Run("spot owner factory and separate clock roles", spot_factory_clock_roles);
  suite.Run("AI refusal, initialized handle and borrowed lifetime", ai_failure_and_move);
  return suite.Finish("factory_contract_test");
}
