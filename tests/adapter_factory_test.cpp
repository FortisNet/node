#include "factory.h"
#include "provider_execution.h"
#include "unit_test_support.h"

#include <type_traits>
#include <utility>

namespace {
using fortis::testing::Require;
using fortis::providers::ProviderExecutionAdapter;
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
}
int main() {
  fortis::testing::UnitSuite suite;
  suite.Run("adapter refusal and moved worker ownership", adapter_failure_and_move);
  suite.Run("owned storage and rollback", owned_factory_storage);
  return suite.Finish("adapter_factory_test");
}
