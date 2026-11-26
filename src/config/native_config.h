#pragma once

#include "log.h"
#include "provider_runtime_config.h"
#include "config_environment.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::config {

struct NativeConfig : ProviderRuntimeConfig {
  // TypeScript PORT defaults to 8788. QEMU development launchers set PORT=8080
  // explicitly to preserve the repository's published probe port.
  uint16_t port = 8788;
  uint16_t health_port = 0;
  char kv_dir[1024]{};
  // Read-only controller-owned public snapshots; absent on unmanaged/native guest runs.
  char release_status_dir[1024]{"/deployment"};
  // FORTIS_LOG_LEVEL=error|warn|info|debug: the least severe log level still
  // written to stderr (base/log.h). Unset or blank keeps the default, info,
  // which prints every line the server printed before levels existed; any
  // other value stops startup. ServerMain applies it with SetLogThreshold.
  fortis::base::LogLevel log_level = fortis::base::kDefaultLogLevel;
  // Per-platform device attestation under required mode. Each defaults to
  // false (not required): devices of that platform enroll and authenticate
  // unverified, whatever proof they send. "true" restores the check:
  //   FORTIS_ANDROID_ATTESTATION_REQUIRED  Android (Play Integrity)
  //   FORTIS_APPLE_ATTESTATION_REQUIRED    iOS (App Attest) and macOS
  //   FORTIS_DESKTOP_ATTESTATION_REQUIRED  Windows and Linux
  // macOS, Windows and Linux have no accepted proof, so "true" refuses them.
  bool android_attestation_required = false;
  bool apple_attestation_required = false;
  bool desktop_attestation_required = false;
  // APPLE_APP_ATTEST_ENVIRONMENT=development makes required-mode App Attest
  // enrollment expect the development AAGUID and development-signed
  // executables; unset or "production" expects production keys from
  // TestFlight or App Store builds. NODE_ENV=production refuses development.
  bool apple_attest_development = false;
  uint32_t device_token_ttl_seconds = 3600;
  int64_t device_token_ttl_ms = 3600000;
  // FORTIS_ENABLE_DIAGNOSTIC_PROBES=1 registers the unauthenticated
  // diagnostic probes: GET|HEAD /probe/platform (outbound DNS and TCP),
  // /probe/tls (outbound HTTPS), /probe/evm (signature recovery) and GET|HEAD|
  // POST /probe/kv and /probe/storage (POST writes and fsyncs a counter).
  // Unset, blank or 0 leaves them unregistered (the router's generic 404);
  // any other value stops startup. /healthz, /probe, /probe/representation
  // and POST /probe/protobuf are request-local and always registered.
  bool diagnostic_probes_enabled = false;
  bool tls_probe_configured = false;
  char tls_probe_host[254]{};
  char tls_probe_port[6]{};
  char tls_probe_ca[1024]{};
  // FORTIS_NATIVE_PRICE_ROUTE_CANARY=1 registers the native GET /v1/prices,
  // GET and POST /v1/prices/history and GET /v1/prices/ohlc routes and
  // requires FORTIS_CA_BUNDLE_PATH. Unset, blank or 0
  // leaves them all absent. This is not a public production switch.
  bool native_price_route_canary = false;
  bool kv_group_commit = false;
  // FORTIS_KV_DURABILITY=commit-slots opts the KV store into the commit-slot
  // experiment and migrates a rollback-marker log on its first open; the
  // migrated log then refuses the default mode. Unset, blank or
  // rollback-marker keeps the default protocol (docs/kv-capacity.md).
  bool kv_commit_slots = false;
  // FORTIS_NATIVE_NEWS_CANARY=1 registers the native GET /v1/news,
  // /v1/news/{id}, /v1/news/{id}/reactions and /v1/media/{key} routes and
  // starts the news ingest and media prune jobs (which contact the six
  // publisher feeds and registered media hosts). Requires
  // FORTIS_CA_BUNDLE_PATH, validated at startup. Unset, blank or 0 leaves
  // them all absent. This is not a public production switch.
  bool native_news_canary = false;
  // FORTIS_NATIVE_ADDRESS_CANARY=1 registers the address, transaction
  // and network-overview routes (balances, insight, nfts, token-holdings,
  // transfers, tx detail and view, overview) and starts their jobs (pending
  // transaction polling, EVM change detection, transfer retention and
  // operation candle repair). Requires FORTIS_CA_BUNDLE_PATH. Unset, blank
  // or 0 leaves them all absent. This is not a public production switch.
  bool native_address_canary = false;
  // FORTIS_NATIVE_AI_CANARY=1 registers the AI conversation routes
  // (POST /v1/ai/conversations, GET and POST
  // /v1/ai/conversations/{id}/messages). Requires FORTIS_CA_BUNDLE_PATH (the
  // outbound model call) and SUPPORT_AUTH_SECRET (at least 32 characters; it keys the
  // pseudonymous conversation owner id, so it must stay stable). Unset,
  // blank or 0 leaves the routes absent. This is not a public production
  // switch.
  bool native_ai_canary = false;
  // SUPPORT_AUTH_SECRET keys AI conversation owner IDs and must stay stable.
  bool support_auth_secret_configured = false;
  size_t support_auth_secret_length = 0;
  char support_auth_secret[1024]{};
  // OPENAI_API_KEY and OPENAI_MODEL (trimmed optionalKey): both enable model
  // replies; otherwise every turn is saved and answers 503
  // REPLY_UNAVAILABLE, as in the TypeScript service. The key is sent as
  // "Bearer <key>", so it is at most 248 bytes.
  bool openai_api_key_configured = false;
  bool openai_model_configured = false;
  char openai_api_key[249]{};
  char openai_model[256]{};
  // Native async AI provider. Only the key's mounted guest path crosses
  // the launcher boundary; the key bytes are read inside the guest.
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
  // Blank/unset disables /metrics; configured values are trimmed and capped
  // at 512 bytes before handlers can compare an Authorization header.
  bool metrics_enabled = false;
  char metrics_token[513]{};
  // FORTIS_NATIVE_CROSS_CHAIN_CANARY=1 registers the cross-chain
  // exchange routes (GET /v1/exchange/cross-chain/providers, POST
  // /v1/exchange/cross-chain/quotes and /status). Requires
  // FORTIS_CA_BUNDLE_PATH. Unset, blank or 0 leaves them absent. This is not
  // a public production switch.
  bool native_cross_chain_canary = false;
  // Cross-chain provider credentials (trimmed optionalKey, sent only as the
  // provider's request header): LIFI_API_KEY (x-lifi-api-key),
  // SQUID_INTEGRATOR_ID (x-integrator-id), SIMPLESWAP_API_KEY (x-api-key)
  // and NEAR_INTENTS_JWT ("Bearer <jwt>", so at most 248 bytes).
  bool lifi_api_key_configured = false;
  bool squid_integrator_id_configured = false;
  bool simpleswap_api_key_configured = false;
  bool near_intents_jwt_configured = false;
  char lifi_api_key[256]{};
  char squid_integrator_id[256]{};
  char simpleswap_api_key[256]{};
  char near_intents_jwt[249]{};
  // MAYA_DASH_EXECUTION_ENABLED ('true'/'false', default false) and
  // MAYA_NODE_URLS (optionalUrlList; native accepts https:// URLs only).
  // Enabling execution requires two distinct https origins, as TypeScript.
  bool maya_dash_execution_enabled = false;
  size_t maya_node_url_count = 0;
  char maya_node_urls[5][512]{};
  // FORTIS_NATIVE_ROBINHOOD_CANARY=1 registers the Robinhood routes
  // (GET /v1/networks/robinhood/stock-tokens[/{address}] and
  // /sponsored-trades/capabilities, POST /transactions/diagnose and
  // /sponsored-trades/prepare|submit). Requires FORTIS_CA_BUNDLE_PATH. Unset,
  // blank or 0 leaves them absent. This is not a public production switch.
  bool native_robinhood_canary = false;
  // ALCHEMY_GAS_POLICY_ID (trimmed optionalKey): with ALCHEMY_API_KEY it
  // enables sponsored trades; it is sent only inside wallet_prepareCalls.
  bool alchemy_gas_policy_id_configured = false;
  char alchemy_gas_policy_id[256]{};
  // FORTIS_NATIVE_DASHPAY_CANARY=1 registers GET /v1/dashpay/names/resolve
  // (the proof-verified DPNS lookup). Requires FORTIS_CA_BUNDLE_PATH.
  // Unset, blank or 0 leaves it absent. This is not a public production
  // switch.
  bool native_dashpay_canary = false;
};

// This intentionally covers only settings used by current native routes. It
// does not implement other TypeScript provider secrets, the ephemeral
// SUPPORT_AUTH_SECRET default (the AI canary requires a configured one),
// or the TS EMBEDDED_STORE_PATH. Development attestation
// remains local/private-peer-only, but checks the direct TCP peer; native has
// no trusted-proxy forwarding model equivalent to TRUST_PROXY=loopback.

// Load once at startup. Returns false with a bounded, human-readable error for
// invalid supported settings; no environment reads occur in request handlers.
bool LoadNativeConfig(NativeEnvLookup lookup, void* context,
                      NativeConfig* config, char* error, size_t error_capacity);

}  // namespace fortis::config
