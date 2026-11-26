#pragma once

#include <stddef.h>

namespace fortis::ai {

// The AI plane's slice of the startup configuration (NativeConfig fields of
// the same names, validated by LoadNativeConfig). The composition root
// (app/module_configs.cpp) copies it once at startup; the AI service, the
// /healthz supportedModels list and the /ai operations page read only this.
struct AiServiceConfig {
  // FORTIS_NATIVE_AI_CANARY=1: the conversation routes are composed.
  bool native_ai_canary = false;
  bool attestation_required = false;
  // FORTIS_CA_BUNDLE_PATH (required by the canary): the model call's trust store.
  char ca_bundle_path[1024]{};
  // SUPPORT_AUTH_SECRET keys the pseudonymous owner ids and must stay stable.
  bool support_auth_secret_configured = false;
  size_t support_auth_secret_length = 0;
  char support_auth_secret[1024]{};
  // OPENAI_API_KEY / OPENAI_MODEL: both enable OpenAI replies.
  bool openai_api_key_configured = false;
  bool openai_model_configured = false;
  char openai_api_key[249]{};
  char openai_model[256]{};
  // The native Anthropic provider (AI_* and ANTHROPIC_API_KEY_PATH).
  char ai_provider[32]{};
  char ai_model[128]{};
  char ai_effort[16]{};
  char ai_max_output_tokens[16]{};
  char ai_timeout_ms[16]{};
  char ai_concurrency[16]{};
  char anthropic_api_key_path[256]{};
  bool ai_provider_configured = false;
  bool ai_model_configured = false;
  bool ai_effort_configured = false;
  bool ai_max_output_tokens_configured = false;
  bool ai_timeout_ms_configured = false;
  bool ai_concurrency_configured = false;
  bool anthropic_api_key_path_configured = false;
};

}  // namespace fortis::ai
