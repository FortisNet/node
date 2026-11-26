#include "robinhood_service.h"
#include "factory_test_support.h"

int main() {
  return fortis::testing::FactoryLifecycle("robinhood_factory_test", fortis::exchange::RobinhoodConfig{},
      [](auto& config) {
        config.transport.ca_bundle_configured = true;
        strcpy(config.transport.ca_bundle_path, "/unused-without-outbound-jobs");
      },
      [](const auto& config, auto& kv, auto& coordinator) { return fortis::exchange::RobinhoodService::Create(config, {coordinator, &kv}); });
}
