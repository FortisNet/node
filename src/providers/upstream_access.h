#pragma once

#include "provider_runtime_config.h"

namespace fortis::providers {

// How this deployment reaches one keyed REST upstream: the host its key (or
// tier) selects and the credential header it sends. Each rule is written
// once here; the provider owners' upstream tables (token_provider.cpp,
// discovery_provider_owner.cpp) name the rule in the upstream's row.
struct UpstreamAccess {
  // The host the deployment's key selects; left unchanged when the rule does
  // not choose one (the row's fixed host, or the request's own endpoint).
  const char* host = nullptr;
  // The credential header; nullptr: the request is keyless.
  const char* header = nullptr;
  const char* value = nullptr;
};

// False: the upstream is refused for this deployment (it needs a key the
// deployment does not carry).
using UpstreamAccessFn = bool (*)(const config::ProviderRuntimeConfig& config, UpstreamAccess* access);

// CoinGecko: pro-api.coingecko.com with x-cg-pro-api-key for a pro-tier key,
// else api.coingecko.com with x-cg-demo-api-key when a key is configured.
inline bool CoinGeckoAccess(const config::ProviderRuntimeConfig& config, UpstreamAccess* access) {
  const bool pro = config.coingecko_tier == config::CoinGeckoTier::kPro && config.coingecko_api_key_configured;
  access->host = pro ? "pro-api.coingecko.com" : "api.coingecko.com";
  if (config.coingecko_api_key_configured) {
    access->header = pro ? "x-cg-pro-api-key" : "x-cg-demo-api-key";
    access->value = config.coingecko_api_key;
  }
  return true;
}

// Jupiter: api.jup.ag with x-api-key, or the keyless lite-api.jup.ag.
inline bool JupiterAccess(const config::ProviderRuntimeConfig& config, UpstreamAccess* access) {
  access->host = config.jupiter_api_key_configured ? "api.jup.ag" : "lite-api.jup.ag";
  if (config.jupiter_api_key_configured) {
    access->header = "x-api-key";
    access->value = config.jupiter_api_key;
  }
  return true;
}

// Taostats: the key as Authorization; refused without one.
inline bool TaostatsAccess(const config::ProviderRuntimeConfig& config, UpstreamAccess* access) {
  if (!config.taostats_api_key_configured) return false;
  access->header = "Authorization";
  access->value = config.taostats_api_key;
  return true;
}

// TronScan and TronGrid: TRON-PRO-API-KEY when the deployment has the key.
inline bool TronScanAccess(const config::ProviderRuntimeConfig& config, UpstreamAccess* access) {
  if (config.tronscan_api_key_configured) {
    access->header = "TRON-PRO-API-KEY";
    access->value = config.tronscan_api_key;
  }
  return true;
}
inline bool TronGridAccess(const config::ProviderRuntimeConfig& config, UpstreamAccess* access) {
  if (config.trongrid_api_key_configured) {
    access->header = "TRON-PRO-API-KEY";
    access->value = config.trongrid_api_key;
  }
  return true;
}

// SaucerSwap (both networks): x-api-key when the deployment has the key.
inline bool SaucerSwapAccess(const config::ProviderRuntimeConfig& config, UpstreamAccess* access) {
  if (config.saucerswap_api_key_configured) {
    access->header = "x-api-key";
    access->value = config.saucerswap_api_key;
  }
  return true;
}

}  // namespace fortis::providers
