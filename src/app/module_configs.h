#pragma once

// The composition root's configuration fan-out: each module receives only
// its value-owned slice of NativeConfig, copied once at startup. Provider-backed
// modules share only provider credentials, transport and module policy fields.

#include "ai_config.h"
#include "provider_runtime_config.h"
#include "device_enrollment.h"
#include "exchange_config.h"
#include "probe_routes.h"

namespace fortis::config { struct NativeConfig; }

namespace fortis::app {

// Every slice, in process-lifetime static storage (main's g_module_configs):
// together they are several KiB, kept off the 64 KiB guest boot stack.
struct ModuleConfigs {
  config::ProviderRuntimeConfig providers;
  ai::AiServiceConfig ai;
  auth::DeviceEnrollmentConfig devices;
  exchange::CrossChainConfig cross_chain;
  exchange::RobinhoodConfig robinhood;
  exchange::DashPayConfig dashpay;
  ProbeRoutesConfig probes;
};

// Fills every slice of `out` from `config`; probes.ai points at out->ai.
// Each builder assigns every field of its slice (no stack temporary).
void BuildModuleConfigs(const config::NativeConfig& config, ModuleConfigs* out);

// The individual slices (also used by unit tests that load a NativeConfig).
void BuildAiServiceConfig(const config::NativeConfig& config, ai::AiServiceConfig* out);
void BuildDeviceEnrollmentConfig(const config::NativeConfig& config, auth::DeviceEnrollmentConfig* out);
void BuildCrossChainConfig(const config::NativeConfig& config, exchange::CrossChainConfig* out);
void BuildRobinhoodConfig(const config::NativeConfig& config, exchange::RobinhoodConfig* out);
void BuildDashPayConfig(const config::NativeConfig& config, exchange::DashPayConfig* out);
void BuildProbeRoutesConfig(const config::NativeConfig& config, ProbeRoutesConfig* out);

}  // namespace fortis::app
