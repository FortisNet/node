#include "dashpay_service.h"
#include "factory_test_support.h"

int main() {
  return fortis::testing::FactoryLifecycle("dashpay_factory_test", fortis::exchange::DashPayConfig{},
      [](auto& config) {
        config.transport.ca_bundle_configured = true;
        strcpy(config.transport.ca_bundle_path, "/unused-without-outbound-jobs");
      },
      [](const auto& config, auto&, auto& coordinator) { return fortis::exchange::DashPayService::Create(config, {coordinator}); });
}
