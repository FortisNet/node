#include "test_support.h"
#include "native_config.h"
#include "../src/config/native_config.h"
#include "native_config_fixtures.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

using namespace fortis::config;

namespace {
struct Entry { const char* name; const char* value; };
struct Environment { const Entry* entries; size_t count; };

const char* lookup(void* context, const char* name) {
  const auto* env = static_cast<const Environment*>(context);
  for (size_t i = 0; i < env->count; ++i)
    if (strcmp(env->entries[i].name, name) == 0) return env->entries[i].value;
  return nullptr;
}

bool load(const Entry* entries, size_t count, NativeConfig* config, char* error) {
  Environment env{entries, count};
  return LoadNativeConfig(lookup, &env, config, error, 256);
}
}

int main() try {
  using namespace native_config_fixtures;
  NativeConfig config{};
  char error[256]{};
  assert(load(nullptr, 0, &config, error));
  assert(config.port == kDefaultPort);
  assert(strcmp(config.kv_dir, kDefaultKvDirectory) == 0);
  assert(!config.attestation_required);
  assert(config.device_token_ttl_seconds == kDefaultDeviceTokenTtlSeconds);
  assert(!config.tls_probe_configured);
  assert(!config.metrics_enabled);
  // Diagnostic probes are off unless explicitly enabled, including in the
  // production attestation profile.
  assert(!config.diagnostic_probes_enabled);
  const Entry production_profile[] = {{"NODE_ENV", "production"}};
  assert(load(production_profile, 1, &config, error) && !config.diagnostic_probes_enabled);
  for (const char* off : {"", "0"}) {
    const Entry probes_off[] = {{"FORTIS_ENABLE_DIAGNOSTIC_PROBES", off}};
    assert(load(probes_off, 1, &config, error) && !config.diagnostic_probes_enabled);
  }
  const Entry probes_on[] = {{"FORTIS_ENABLE_DIAGNOSTIC_PROBES", "1"}};
  assert(load(probes_on, 1, &config, error) && config.diagnostic_probes_enabled);
  for (const char* invalid : {"true", "yes", " 1", "1 ", "2", "on"}) {
    const Entry probes_invalid[] = {{"FORTIS_ENABLE_DIAGNOSTIC_PROBES", invalid}};
    assert(!load(probes_invalid, 1, &config, error));
    assert(strstr(error, "FORTIS_ENABLE_DIAGNOSTIC_PROBES") != nullptr);
  }
  assert(config.coingecko_tier == CoinGeckoTier::kDemo &&
         !config.coingecko_api_key_configured && config.coingecko_api_key[0] == '\0');
  assert(!config.jupiter_api_key_configured);
  assert(!config.native_price_route_canary);
  assert(!config.price_snapshot_enabled);
  const Entry snapshot_only[] = {{"FORTIS_PRICE_SNAPSHOT_ENABLED", "1"}};
  assert(!load(snapshot_only, 1, &config, error));
  const Entry snapshot[] = {{"FORTIS_PRICE_SNAPSHOT_ENABLED", "1"},
      {"FORTIS_NATIVE_PRICE_ROUTE_CANARY", "1"}, {"FORTIS_CA_BUNDLE_PATH", "/data/ca-bundle.pem"}};
  assert(load(snapshot, 3, &config, error) && config.price_snapshot_enabled);


  char secret_path[] = "/tmp/fortis-support-secret-XXXXXX";
  const int secret_fd = mkstemp(secret_path);
  assert(secret_fd >= 0);
  const std::string test_secret(64, 's');
  assert(write(secret_fd, test_secret.data(), test_secret.size()) ==
         static_cast<ssize_t>(test_secret.size()));
  assert(close(secret_fd) == 0);
  const Entry ai_file[] = { {"FORTIS_NATIVE_AI_CANARY", "1"},
                            {"FORTIS_CA_BUNDLE_PATH", "/data/ca-bundle.pem"},
                            {"FORTIS_SUPPORT_AUTH_SECRET_FILE", secret_path} };
  assert(load(ai_file, 3, &config, error));
  assert(config.native_ai_canary && config.support_auth_secret_configured);
  assert(config.support_auth_secret_length == test_secret.size());
  assert(memcmp(config.support_auth_secret, test_secret.data(), test_secret.size()) == 0);
  assert(unlink(secret_path) == 0);
  assert(!load(ai_file, 3, &config, error));

  const Entry canary[] = {{"FORTIS_NATIVE_PRICE_ROUTE_CANARY", "1"},
                          {"FORTIS_CA_BUNDLE_PATH", "/data/ca-bundle.pem"}};
  assert(load(canary, 2, &config, error));
  assert(config.native_price_route_canary && config.ca_bundle_configured);
  assert(!config.split_price_store);
  const Entry split[] = {canary[0], canary[1], {"FORTIS_SPLIT_PRICE_STORE", "1"}};
  assert(load(split, 3, &config, error) && config.split_price_store);
  assert(!load(split + 2, 1, &config, error));
  const Entry bad_split[] = {canary[0], canary[1], {"FORTIS_SPLIT_PRICE_STORE", "yes"}};
  assert(!load(bad_split, 3, &config, error));
  for (const char* disabled : {"0", ""}) {
    const Entry off[] = {{"FORTIS_NATIVE_PRICE_ROUTE_CANARY", disabled}};
    assert(load(off, 1, &config, error));
    assert(!config.native_price_route_canary);
  }
  const Entry canary_blank_ca[] = {{"FORTIS_NATIVE_PRICE_ROUTE_CANARY", "1"},
                                   {"FORTIS_CA_BUNDLE_PATH", ""}};
  assert(!load(canary_blank_ca, 2, &config, error));
  assert(strstr(error, "requires FORTIS_CA_BUNDLE_PATH"));

  assert(load(nullptr, 0, &config, error) && !config.native_news_canary);
  const Entry news[] = {{"FORTIS_NATIVE_NEWS_CANARY", "1"},
                        {"FORTIS_CA_BUNDLE_PATH", "/data/ca-bundle.pem"}};
  assert(load(news, 2, &config, error));
  assert(config.native_news_canary && !config.native_price_route_canary);
  for (const char* disabled : {"0", ""}) {
    const Entry off[] = {{"FORTIS_NATIVE_NEWS_CANARY", disabled}};
    assert(load(off, 1, &config, error) && !config.native_news_canary);
  }
  const Entry news_no_ca[] = {{"FORTIS_NATIVE_NEWS_CANARY", "1"}};
  assert(!load(news_no_ca, 1, &config, error));
  assert(strstr(error, "FORTIS_NATIVE_NEWS_CANARY=1 requires FORTIS_CA_BUNDLE_PATH"));
  const Entry news_bad[] = {{"FORTIS_NATIVE_NEWS_CANARY", "yes"},
                            {"FORTIS_CA_BUNDLE_PATH", "/data/ca-bundle.pem"}};
  assert(!load(news_bad, 2, &config, error));
  assert(strstr(error, "FORTIS_NATIVE_NEWS_CANARY must be 0 or 1"));

  assert(load(nullptr, 0, &config, error) && !config.kv_commit_slots);
  const Entry commit_slots[] = {{"FORTIS_KV_DURABILITY", "commit-slots"}};
  assert(load(commit_slots, 1, &config, error) && config.kv_commit_slots);
  const Entry rollback_marker[] = {{"FORTIS_KV_DURABILITY", "rollback-marker"}};
  assert(load(rollback_marker, 1, &config, error) && !config.kv_commit_slots);
  const Entry durability_bad[] = {{"FORTIS_KV_DURABILITY", "slots"}};
  assert(!load(durability_bad, 1, &config, error));
  assert(strstr(error, "FORTIS_KV_DURABILITY must be rollback-marker or commit-slots"));

  // FORTIS_LOG_LEVEL: unset or blank is info (every pre-level line prints).
  assert(load(nullptr, 0, &config, error) && config.log_level == fortis::base::LogLevel::kInfo);
  const Entry log_blank[] = {{"FORTIS_LOG_LEVEL", ""}};
  assert(load(log_blank, 1, &config, error) && config.log_level == fortis::base::LogLevel::kInfo);
  const Entry log_error[] = {{"FORTIS_LOG_LEVEL", "error"}};
  assert(load(log_error, 1, &config, error) && config.log_level == fortis::base::LogLevel::kError);
  const Entry log_warn[] = {{"FORTIS_LOG_LEVEL", "warn"}};
  assert(load(log_warn, 1, &config, error) && config.log_level == fortis::base::LogLevel::kWarn);
  const Entry log_info[] = {{"FORTIS_LOG_LEVEL", "info"}};
  assert(load(log_info, 1, &config, error) && config.log_level == fortis::base::LogLevel::kInfo);
  const Entry log_debug[] = {{"FORTIS_LOG_LEVEL", "debug"}};
  assert(load(log_debug, 1, &config, error) && config.log_level == fortis::base::LogLevel::kDebug);
  for (const char* refused : {"DEBUG", "verbose", "3", " info"}) {
    const Entry log_bad[] = {{"FORTIS_LOG_LEVEL", refused}};
    assert(!load(log_bad, 1, &config, error));
    assert(strstr(error, "FORTIS_LOG_LEVEL must be error, warn, info or debug"));
  }
  // Loading the configuration never changes the process threshold itself.
  assert(fortis::base::LogThreshold() == fortis::base::kDefaultLogLevel);

  const Entry production[] = {{"NODE_ENV", "production"}};
  assert(load(production, 1, &config, error));
  assert(config.attestation_required);

  const Entry valid[] = {
      {"PORT", " 9000 "}, {"FORTIS_KV_DIR", "/tmp/fortis-kv"},
      {"ATTESTATION_MODE", "required"}, {"DEVICE_TOKEN_TTL_SECONDS", "600.0"},
      {"METRICS_TOKEN", "  metrics-test-token  "},
      {"FORTIS_TLS_PROBE_HOST", "localhost"}, {"FORTIS_TLS_PROBE_PORT", "9443"},
      {"FORTIS_TLS_PROBE_CA", "/tmp/test-ca.pem"},
      {"FORTIS_CA_BUNDLE_PATH", "/data/ca-bundle.pem"},
  };
  assert(load(valid, sizeof(valid) / sizeof(valid[0]), &config, error));
  assert(config.port == 9000 && strcmp(config.kv_dir, "/tmp/fortis-kv") == 0);
  assert(config.attestation_required && config.device_token_ttl_seconds == 600);
  assert(config.device_token_ttl_ms == 600000 && config.tls_probe_configured);
  assert(config.metrics_enabled && strcmp(config.metrics_token, kMetricsToken) == 0);
  assert(strcmp(config.tls_probe_host, "localhost") == 0);
  assert(strcmp(config.tls_probe_port, "9443") == 0);
  assert(strcmp(config.tls_probe_ca, "/tmp/test-ca.pem") == 0);
  assert(config.ca_bundle_configured);
  assert(strcmp(config.ca_bundle_path, "/data/ca-bundle.pem") == 0);

  const Entry ttl_min[] = {{"DEVICE_TOKEN_TTL_SECONDS", "300"}};
  assert(load(ttl_min, 1, &config, error));
  assert(config.device_token_ttl_seconds == kMinimumDeviceTokenTtlSeconds);
  const Entry ttl_max[] = {{"DEVICE_TOKEN_TTL_SECONDS", "3600"}};
  assert(load(ttl_max, 1, &config, error));
  assert(config.device_token_ttl_seconds == kMaximumDeviceTokenTtlSeconds);
  const Entry blank_metrics[] = {{"METRICS_TOKEN", " \t\r\n "}};
  assert(load(blank_metrics, 1, &config, error));
  assert(!config.metrics_enabled && config.metrics_token[0] == '\0');

  const Entry coingecko_pro[] = {
      {"COINGECKO_TIER", "pro"}, {"COINGECKO_API_KEY", "  secret-key-72  "},
  };
  assert(load(coingecko_pro, 2, &config, error));
  assert(config.coingecko_tier == CoinGeckoTier::kPro &&
         config.coingecko_api_key_configured &&
         strcmp(config.coingecko_api_key, "secret-key-72") == 0);
  const Entry pro_without_key[] = {{"COINGECKO_TIER", "pro"}};
  assert(load(pro_without_key, 1, &config, error));
  assert(config.coingecko_tier == CoinGeckoTier::kPro &&
         !config.coingecko_api_key_configured);
  const Entry jupiter_key[] = {{"JUPITER_API_KEY", " \t public-jupiter-key \r\n"}};
  assert(load(jupiter_key, 1, &config, error));
  assert(config.jupiter_api_key_configured &&
         strcmp(config.jupiter_api_key, "public-jupiter-key") == 0);
  const Entry blank_jupiter_key[] = {{"JUPITER_API_KEY", " \t\r\n "}};
  assert(load(blank_jupiter_key, 1, &config, error));
  assert(!config.jupiter_api_key_configured && config.jupiter_api_key[0] == '\0');
  const Entry injected_jupiter_key[] = {{"JUPITER_API_KEY", "secret\r\nInjected: yes"}};
  assert(!load(injected_jupiter_key, 1, &config, error));
  assert(strstr(error, "JUPITER_API_KEY") && !strstr(error, "secret"));
  const std::string maximum_jupiter_key(255, 'j');
  const Entry maximum_jupiter[] = {{"JUPITER_API_KEY", maximum_jupiter_key.c_str()}};
  assert(load(maximum_jupiter, 1, &config, error));
  assert(config.jupiter_api_key_configured &&
         strlen(config.jupiter_api_key) == maximum_jupiter_key.size());
  const std::string oversized_jupiter_key(256, 'j');
  const Entry oversized_jupiter[] = {{"JUPITER_API_KEY", oversized_jupiter_key.c_str()}};
  assert(!load(oversized_jupiter, 1, &config, error));
  assert(strstr(error, "JUPITER_API_KEY") && !strstr(error, oversized_jupiter_key.c_str()));

  const Entry tronscan_key[] = { {"TRONSCAN_API_KEY", " \t fixture-tronscan-key \r\n"} };
  assert(load(tronscan_key, 1, &config, error));
  assert(config.tronscan_api_key_configured &&
         strcmp(config.tronscan_api_key, "fixture-tronscan-key") == 0);
  const Entry blank_tronscan_key[] = { {"TRONSCAN_API_KEY", " \t\r\n "} };
  assert(load(blank_tronscan_key, 1, &config, error));
  assert(!config.tronscan_api_key_configured && config.tronscan_api_key[0] == '\0');
  const Entry injected_tronscan_key[] = { {"TRONSCAN_API_KEY", "secret\r\nInjected: yes"} };
  assert(!load(injected_tronscan_key, 1, &config, error));
  assert(strstr(error, "TRONSCAN_API_KEY") && !strstr(error, "secret"));
  const std::string maximum_tronscan_key(255, 't');
  const Entry maximum_tronscan[] = {{"TRONSCAN_API_KEY", maximum_tronscan_key.c_str()}};
  assert(load(maximum_tronscan, 1, &config, error));
  assert(config.tronscan_api_key_configured &&
         strlen(config.tronscan_api_key) == maximum_tronscan_key.size());
  const std::string oversized_tronscan_key(256, 't');
  const Entry oversized_tronscan[] = {{"TRONSCAN_API_KEY", oversized_tronscan_key.c_str()}};
  assert(!load(oversized_tronscan, 1, &config, error));
  assert(strstr(error, "TRONSCAN_API_KEY") && !strstr(error, oversized_tronscan_key.c_str()));

  const Entry taostats_key[] = { {"TAOSTATS_API_KEY", " \t fixture-taostats-key \r\n"} };
  assert(load(taostats_key, 1, &config, error));
  assert(config.taostats_api_key_configured &&
         strcmp(config.taostats_api_key, "fixture-taostats-key") == 0);
  const Entry blank_taostats_key[] = { {"TAOSTATS_API_KEY", " \t\r\n "} };
  assert(load(blank_taostats_key, 1, &config, error));
  assert(!config.taostats_api_key_configured && config.taostats_api_key[0] == '\0');
  const Entry injected_taostats_key[] = { {"TAOSTATS_API_KEY", "secret\r\nInjected: yes"} };
  assert(!load(injected_taostats_key, 1, &config, error));
  assert(strstr(error, "TAOSTATS_API_KEY") && !strstr(error, "secret"));
  const std::string maximum_taostats_key(255, 'b');
  const Entry maximum_taostats[] = {{"TAOSTATS_API_KEY", maximum_taostats_key.c_str()}};
  assert(load(maximum_taostats, 1, &config, error));
  assert(config.taostats_api_key_configured &&
         strlen(config.taostats_api_key) == maximum_taostats_key.size());
  const std::string oversized_taostats_key(256, 'b');
  const Entry oversized_taostats[] = {{"TAOSTATS_API_KEY", oversized_taostats_key.c_str()}};
  assert(!load(oversized_taostats, 1, &config, error));
  assert(strstr(error, "TAOSTATS_API_KEY") && !strstr(error, oversized_taostats_key.c_str()));

  const Entry blank_coingecko_key[] = {{"COINGECKO_API_KEY", "   "}};
  assert(load(blank_coingecko_key, 1, &config, error));
  assert(!config.coingecko_api_key_configured);
  const Entry bad_coingecko_tier[] = {{"COINGECKO_TIER", "platinum"}};
  assert(!load(bad_coingecko_tier, 1, &config, error));
  assert(strstr(error, "COINGECKO_TIER"));
  const Entry injected_coingecko_key[] = {{"COINGECKO_API_KEY", "secret\r\nInjected: yes"}};
  assert(!load(injected_coingecko_key, 1, &config, error));
  assert(strstr(error, "COINGECKO_API_KEY") && !strstr(error, "secret"));
  const std::string maximum_coingecko_key(255, 'm');
  const Entry maximum_coingecko[] = {{"COINGECKO_API_KEY", maximum_coingecko_key.c_str()}};
  assert(load(maximum_coingecko, 1, &config, error));
  assert(config.coingecko_api_key_configured &&
         strlen(config.coingecko_api_key) == maximum_coingecko_key.size());
  const std::string oversized_coingecko_key(256, 'k');
  const Entry oversized_coingecko[] = {{"COINGECKO_API_KEY", oversized_coingecko_key.c_str()}};
  assert(!load(oversized_coingecko, 1, &config, error));
  assert(strstr(error, "COINGECKO_API_KEY") && !strstr(error, oversized_coingecko_key.c_str()));
  char coingecko_key_path[] = "/tmp/fortis-coingecko-key-XXXXXX";
  const int coingecko_key_fd = mkstemp(coingecko_key_path);
  assert(coingecko_key_fd >= 0);
  constexpr char example_key[] = "file-fixture-key\r\n";
  assert(write(coingecko_key_fd, example_key, sizeof(example_key) - 1) ==
         static_cast<ssize_t>(sizeof(example_key) - 1));
  assert(close(coingecko_key_fd) == 0);
  const Entry coingecko_file[] = {
      {"FORTIS_COINGECKO_API_KEY_FILE", coingecko_key_path}, {"COINGECKO_TIER", "demo"}};
  assert(load(coingecko_file, 2, &config, error));
  assert(config.coingecko_api_key_configured &&
         strcmp(config.coingecko_api_key, "file-fixture-key") == 0);
  const Entry conflicting_key[] = {
      {"FORTIS_COINGECKO_API_KEY_FILE", coingecko_key_path}, {"COINGECKO_API_KEY", "env-key"}};
  assert(!load(conflicting_key, 2, &config, error));
  assert(chmod(coingecko_key_path, 0644) == 0);
  assert(!load(coingecko_file, 2, &config, error));
  assert(strstr(error, "FORTIS_COINGECKO_API_KEY_FILE") && !strstr(error, "file-fixture-key"));
  assert(unlink(coingecko_key_path) == 0);
  assert(!load(coingecko_file, 2, &config, error));

  // Per-platform attestation: not required by default (also in production),
  // "true" restores it per platform, anything but true/false stops startup.
  const Entry platform_default[] = {{"NODE_ENV", "production"}};
  assert(load(platform_default, 1, &config, error));
  assert(config.attestation_required);
  assert(!config.android_attestation_required && !config.apple_attestation_required &&
         !config.desktop_attestation_required);
  const Entry platform_on[] = {{"NODE_ENV", "production"},
                               {"FORTIS_ANDROID_ATTESTATION_REQUIRED", "true"},
                               {"FORTIS_APPLE_ATTESTATION_REQUIRED", "true"},
                               {"FORTIS_DESKTOP_ATTESTATION_REQUIRED", "true"}};
  assert(load(platform_on, 4, &config, error));
  assert(config.android_attestation_required && config.apple_attestation_required &&
         config.desktop_attestation_required);
  for (const char* name : {"FORTIS_APPLE_ATTESTATION_REQUIRED",
                           "FORTIS_DESKTOP_ATTESTATION_REQUIRED"}) {
    const Entry bad[] = {{name, "no"}};
    assert(!load(bad, 1, &config, error));
    assert(strstr(error, name));
  }

  const Entry prod_development[] = {{"NODE_ENV", "production"},
                                    {"ATTESTATION_MODE", "development"}};
  assert(!load(prod_development, 2, &config, error));
  assert(strstr(error, "mandatory in production"));

  for (const InvalidValue& invalid : kInvalidValues) {
    Entry entry{invalid.name, invalid.value};
    assert(!load(&entry, 1, &config, error));
    assert(strstr(error, invalid.error_fragment));
  }
  const Entry bad_tls_port[] = {
      {"FORTIS_TLS_PROBE_HOST", "localhost"}, {"FORTIS_TLS_PROBE_PORT", "70000"},
      {"FORTIS_TLS_PROBE_CA", "/tmp/ca.pem"},
  };
  assert(!load(bad_tls_port, 3, &config, error));
  assert(strstr(error, "TLS probe"));
  const std::string oversized_metrics_token(513, 't');
  const Entry oversized_metrics[] = {{"METRICS_TOKEN", oversized_metrics_token.c_str()}};
  assert(!load(oversized_metrics, 1, &config, error));
  assert(strstr(error, "METRICS_TOKEN"));

  puts("native startup config defaults, overrides, and invalid-value matrix passed");
} catch (...) {
  return fortis::test::ReportException();
}
