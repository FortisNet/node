#include "native_config.h"
#include "token_service.h"
#include "factory_test_support.h"

int main() {
  return fortis::testing::FactoryLifecycle("token_factory_test", fortis::config::NativeConfig{},
      [](auto& config) {
        config.ca_bundle_configured = true;
        strcpy(config.ca_bundle_path, "/unused-without-outbound-jobs");
      },
      [](const auto& config, auto& kv, auto& coordinator) { return fortis::tokens::TokenService::Create(config, {kv, coordinator, kv}); });
}
