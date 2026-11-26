#pragma once

#include "provider_execution_coordinator.h"
#include "native_config.h"
#include "unit_test_support.h"

#include <type_traits>
#include <utility>

namespace fortis::testing {

// Each creator is a real plane factory. Empty config must refuse startup;
// configured empty stores require no outbound work and exercise normal teardown.
template <typename Config, typename Configure, typename Create>
int FactoryLifecycle(const char* name, Config config, Configure configure, Create create) {
  UnitSuite suite;
  suite.Run("refusal, initialized move, teardown and borrowed KV lifetime", [&] {
    TemporaryKv fixture;
    providers::ProviderExecutionCoordinator coordinator;
    Require(coordinator.Start(), "start coordinator");
    auto refused = create(config, fixture.kv, coordinator);
    Require(!refused, "unconfigured plane publishes no service");
    configure(config);
    {
      auto made = create(config, fixture.kv, coordinator);
      Require(made.ok(), made.status().message());
      auto initialized = made.TakeValue();
      static_assert(!std::is_default_constructible_v<decltype(initialized)>);
      static_assert(std::is_nothrow_move_constructible_v<decltype(initialized)>);
      auto moved = std::move(initialized);
      (void)moved;
    }
    Require(coordinator.Stop(), "moved service destruction releases coordinator leases");
    size_t bytes = 0;
    Require(fixture.kv.GetStatus("never-written", 0, nullptr, 0, &bytes) == storage::KvReadStatus::Missing,
            "service retains borrowed store lifetime");
  });
  return suite.Finish(name);
}

} // namespace fortis::testing
