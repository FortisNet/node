#include "cross_chain_service.h"
#include "factory_test_support.h"

int main() {
  return fortis::testing::FactoryLifecycle("cross_chain_factory_test", fortis::exchange::CrossChainConfig{},
      [](auto& config) {
        config.transport.ca_bundle_configured = true;
        strcpy(config.transport.ca_bundle_path, "/unused-without-outbound-jobs");
      },
      [](const auto& config, auto&, auto& coordinator) { return fortis::exchange::CrossChainService::Create(config, {coordinator}); });
}
