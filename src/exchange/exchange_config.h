#pragma once

#include "transport_config.h"

#include <stddef.h>

// The exchange services' slices of the startup configuration: the
// NativeConfig fields of the same names, validated by LoadNativeConfig. The
// composition root (app/module_configs.cpp) copies them once at startup; a
// service keeps its own copy.

namespace fortis::exchange {

// Cross-chain exchange routes.
struct CrossChainConfig {
  bool attestation_required = false;
  fortis::chains::TransportConfig transport{};
  // Provider credentials, trimmed; each is sent only as its provider's header.
  bool lifi_api_key_configured = false;
  bool squid_integrator_id_configured = false;
  bool simpleswap_api_key_configured = false;
  bool near_intents_jwt_configured = false;
  char lifi_api_key[256]{};
  char squid_integrator_id[256]{};
  char simpleswap_api_key[256]{};
  char near_intents_jwt[249]{};
  // MAYA_DASH_EXECUTION_ENABLED and MAYA_NODE_URLS (https:// only).
  bool maya_dash_execution_enabled = false;
  size_t maya_node_url_count = 0;
  char maya_node_urls[5][512]{};
};

// Robinhood routes.
struct RobinhoodConfig {
  bool attestation_required = false;
  fortis::chains::TransportConfig transport{};
  // ALCHEMY_API_KEY (the Robinhood RPC) and ALCHEMY_GAS_POLICY_ID
  // (sponsored trades, sent only inside wallet_prepareCalls).
  bool alchemy_api_key_configured = false;
  bool alchemy_gas_policy_id_configured = false;
  char alchemy_api_key[256]{};
  char alchemy_gas_policy_id[256]{};
};

// The DashPay name resolver.
struct DashPayConfig {
  bool attestation_required = false;
  fortis::chains::TransportConfig transport{};
};

}  // namespace fortis::exchange
