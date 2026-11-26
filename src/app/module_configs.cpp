#include "module_configs.h"

#include "native_config.h"

#include <string.h>

namespace fortis::app {
namespace {

// Byte-exact copy of a same-sized configuration array (secrets may hold any
// byte, so no string copy).
template <typename T, size_t N>
void copy_array(T (&target)[N], const T (&source)[N]) {
  memcpy(target, source, sizeof(target));
}

void copy_transport(const config::NativeConfig& config, fortis::chains::TransportConfig* out) {
  out->ca_bundle_configured = config.ca_bundle_configured;
  copy_array(out->ca_bundle_path, config.ca_bundle_path);
}

}  // namespace

void BuildAiServiceConfig(const config::NativeConfig& config, ai::AiServiceConfig* out) {
  out->native_ai_canary = config.native_ai_canary;
  out->attestation_required = config.attestation_required;
  copy_array(out->ca_bundle_path, config.ca_bundle_path);
  out->support_auth_secret_configured = config.support_auth_secret_configured;
  out->support_auth_secret_length = config.support_auth_secret_length;
  copy_array(out->support_auth_secret, config.support_auth_secret);
  out->openai_api_key_configured = config.openai_api_key_configured;
  out->openai_model_configured = config.openai_model_configured;
  copy_array(out->openai_api_key, config.openai_api_key);
  copy_array(out->openai_model, config.openai_model);
  copy_array(out->ai_provider, config.ai_provider);
  copy_array(out->ai_model, config.ai_model);
  copy_array(out->ai_effort, config.ai_effort);
  copy_array(out->ai_max_output_tokens, config.ai_max_output_tokens);
  copy_array(out->ai_timeout_ms, config.ai_timeout_ms);
  copy_array(out->ai_concurrency, config.ai_concurrency);
  copy_array(out->anthropic_api_key_path, config.anthropic_api_key_path);
  out->ai_provider_configured = config.ai_provider_configured;
  out->ai_model_configured = config.ai_model_configured;
  out->ai_effort_configured = config.ai_effort_configured;
  out->ai_max_output_tokens_configured = config.ai_max_output_tokens_configured;
  out->ai_timeout_ms_configured = config.ai_timeout_ms_configured;
  out->ai_concurrency_configured = config.ai_concurrency_configured;
  out->anthropic_api_key_path_configured = config.anthropic_api_key_path_configured;
}

void BuildDeviceEnrollmentConfig(const config::NativeConfig& config, auth::DeviceEnrollmentConfig* out) {
  out->attestation_required = config.attestation_required;
  out->apple_attest_development = config.apple_attest_development;
  out->device_token_ttl_ms = config.device_token_ttl_ms;
}

void BuildCrossChainConfig(const config::NativeConfig& config, exchange::CrossChainConfig* out) {
  out->attestation_required = config.attestation_required;
  copy_transport(config, &out->transport);
  out->lifi_api_key_configured = config.lifi_api_key_configured;
  out->squid_integrator_id_configured = config.squid_integrator_id_configured;
  out->simpleswap_api_key_configured = config.simpleswap_api_key_configured;
  out->near_intents_jwt_configured = config.near_intents_jwt_configured;
  copy_array(out->lifi_api_key, config.lifi_api_key);
  copy_array(out->squid_integrator_id, config.squid_integrator_id);
  copy_array(out->simpleswap_api_key, config.simpleswap_api_key);
  copy_array(out->near_intents_jwt, config.near_intents_jwt);
  out->maya_dash_execution_enabled = config.maya_dash_execution_enabled;
  out->maya_node_url_count = config.maya_node_url_count;
  copy_array(out->maya_node_urls, config.maya_node_urls);
}

void BuildRobinhoodConfig(const config::NativeConfig& config, exchange::RobinhoodConfig* out) {
  out->attestation_required = config.attestation_required;
  copy_transport(config, &out->transport);
  out->alchemy_api_key_configured = config.alchemy_api_key_configured;
  out->alchemy_gas_policy_id_configured = config.alchemy_gas_policy_id_configured;
  copy_array(out->alchemy_api_key, config.alchemy_api_key);
  copy_array(out->alchemy_gas_policy_id, config.alchemy_gas_policy_id);
}

void BuildDashPayConfig(const config::NativeConfig& config, exchange::DashPayConfig* out) {
  out->attestation_required = config.attestation_required;
  copy_transport(config, &out->transport);
}

void BuildProbeRoutesConfig(const config::NativeConfig& config, ProbeRoutesConfig* out) {
  out->diagnostic_probes_enabled = config.diagnostic_probes_enabled;
  out->tls_probe_configured = config.tls_probe_configured;
  copy_array(out->tls_probe_host, config.tls_probe_host);
  copy_array(out->tls_probe_port, config.tls_probe_port);
  copy_array(out->tls_probe_ca, config.tls_probe_ca);
  out->metrics_enabled = config.metrics_enabled;
  copy_array(out->metrics_token, config.metrics_token);
}

void BuildModuleConfigs(const config::NativeConfig& config, ModuleConfigs* out) {
  out->providers = config;
  BuildAiServiceConfig(config, &out->ai);
  BuildDeviceEnrollmentConfig(config, &out->devices);
  BuildCrossChainConfig(config, &out->cross_chain);
  BuildRobinhoodConfig(config, &out->robinhood);
  BuildDashPayConfig(config, &out->dashpay);
  BuildProbeRoutesConfig(config, &out->probes);
  out->probes.ai = &out->ai;
}

}  // namespace fortis::app
