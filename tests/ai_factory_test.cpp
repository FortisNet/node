#include "ai_service.h"
#include "factory_test_support.h"

int main() {
  return fortis::testing::FactoryLifecycle("ai_factory_test", fortis::ai::AiServiceConfig{},
      [](auto& config) {
        config.support_auth_secret_configured = true;
        memset(config.support_auth_secret, 's', 32);
        config.support_auth_secret_length = 32;
      },
      [](const auto& config, auto& kv, auto&) { return fortis::ai::AiConversationService::Create(config, {kv}); });
}
