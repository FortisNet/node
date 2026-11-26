#include "native_config.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fortis::config {

namespace {
void set_error(char* error, size_t capacity, const char* message) {
  if (error && capacity) snprintf(error, capacity, "%s", message);
}

bool copy_value(char* target, size_t capacity, const char* source) {
  if (!target || !capacity || !source) return false;
  const size_t length = strnlen(source, capacity);
  if (length >= capacity) return false;
  memcpy(target, source, length + 1);
  return true;
}

bool integral_number(const char* value, double* output) {
  if (!value || !output) return false;
  while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') ++value;
  char* end = nullptr;
  const double number = strtod(value, &end);
  if (end == value || !end || !isfinite(number) || floor(number) != number) return false;
  while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
  if (*end) return false;
  *output = number;
  return true;
}

bool port_number(const char* value, uint16_t* port) {
  double number = 0;
  if (!integral_number(value, &number) || number < 1 || number > 65535) return false;
  *port = static_cast<uint16_t>(number);
  return true;
}

bool read_coingecko_key(const char* path, char* output, size_t capacity) {
  const int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat info{};
  const bool private_regular_file = fstat(fd, &info) == 0 &&
      S_ISREG(info.st_mode) && info.st_uid == geteuid() &&
      (info.st_mode & 077) == 0 && info.st_size > 0 &&
      static_cast<uint64_t>(info.st_size) < capacity;
  size_t length = 0;
  bool ok = private_regular_file;
  while (ok && length < capacity) {
    const ssize_t read_count = read(fd, output + length, capacity - length);
    if (read_count < 0 && errno == EINTR) continue;
    if (read_count < 0) ok = false;
    if (read_count <= 0) break;
    length += static_cast<size_t>(read_count);
  }
  close(fd);
  if (!ok || length != static_cast<size_t>(info.st_size) ||
      length >= capacity || memchr(output, '\0', length)) return false;
  if (length && output[length - 1] == '\n') --length;
  if (length && output[length - 1] == '\r') --length;
  output[length] = '\0';
  return length != 0;
}

}  // namespace

const char* ProcessEnvironment(void*, const char* name) { return getenv(name); }

bool LoadNativeConfig(NativeEnvLookup lookup, void* context,
                      NativeConfig* config, char* error, size_t error_capacity) {
  if (!config || !lookup) {
    set_error(error, error_capacity, "Configuration loader is unavailable");
    return false;
  }
  *config = NativeConfig{};
  set_error(error, error_capacity, "");

  const char* value = lookup(context, "PORT");
  if (value && !port_number(value, &config->port)) {
    set_error(error, error_capacity, "Invalid environment: PORT must be a positive integer port");
    return false;
  }
  value = lookup(context, "FORTIS_HEALTH_PORT");
  if (value && *value &&
      (!port_number(value, &config->health_port) || config->health_port == config->port)) {
    set_error(error, error_capacity, "Invalid environment: FORTIS_HEALTH_PORT must differ from PORT");
    return false;
  }

  const char* kv_dir = lookup(context, "FORTIS_KV_DIR");
  if (!kv_dir || !*kv_dir) kv_dir = "/data";
  if (!copy_value(config->kv_dir, sizeof(config->kv_dir), kv_dir)) {
    set_error(error, error_capacity, "Invalid environment: FORTIS_KV_DIR is too long");
    return false;
  }

  const char* release_dir = lookup(context, "FORTIS_RELEASE_STATUS_DIR");
  if (release_dir && *release_dir && (release_dir[0] != '/' ||
      !copy_value(config->release_status_dir, sizeof(config->release_status_dir), release_dir))) {
    set_error(error, error_capacity, "Invalid environment: FORTIS_RELEASE_STATUS_DIR must be a bounded absolute directory");
    return false;
  }

  const char* log_level = lookup(context, "FORTIS_LOG_LEVEL");
  if (log_level && *log_level) {
    const fortis::base::Result<fortis::base::LogLevel> parsed = fortis::base::ParseLogLevel(log_level);
    if (!parsed.ok()) {
      set_error(error, error_capacity,
                "Invalid environment: FORTIS_LOG_LEVEL must be error, warn, info or debug");
      return false;
    }
    config->log_level = parsed.value();
  }

  const char* group_commit = lookup(context, "FORTIS_KV_GROUP_COMMIT");
  if (group_commit && strcmp(group_commit, "0") != 0 && strcmp(group_commit, "1") != 0) {
    set_error(error, error_capacity, "FORTIS_KV_GROUP_COMMIT must be 0 or 1");
    return false;
  }
  config->kv_group_commit = group_commit && strcmp(group_commit, "1") == 0;

  const char* durability = lookup(context, "FORTIS_KV_DURABILITY");
  if (durability && *durability && strcmp(durability, "rollback-marker") != 0 &&
      strcmp(durability, "commit-slots") != 0) {
    set_error(error, error_capacity,
              "FORTIS_KV_DURABILITY must be rollback-marker or commit-slots");
    return false;
  }
  config->kv_commit_slots = durability && strcmp(durability, "commit-slots") == 0;

  const char* node_env = lookup(context, "NODE_ENV");
  const bool production = node_env && strcmp(node_env, "production") == 0;
  const char* mode = lookup(context, "ATTESTATION_MODE");
  if (!mode) mode = production ? "required" : "development";
  if (strcmp(mode, "required") != 0 && strcmp(mode, "development") != 0) {
    set_error(error, error_capacity, "Invalid environment: ATTESTATION_MODE must be development or required");
    return false;
  }
  if (production && strcmp(mode, "required") != 0) {
    set_error(error, error_capacity, "ATTESTATION_MODE=required is mandatory in production");
    return false;
  }
  config->attestation_required = strcmp(mode, "required") == 0;
  const char* android_attestation = lookup(context, "FORTIS_ANDROID_ATTESTATION_REQUIRED");
  if (android_attestation) {
    if (strcmp(android_attestation, "true") != 0 && strcmp(android_attestation, "false") != 0) {
      set_error(error, error_capacity,
                "Invalid environment: FORTIS_ANDROID_ATTESTATION_REQUIRED must be true or false");
      return false;
    }
    config->android_attestation_required = strcmp(android_attestation, "true") == 0;
  }
  const char* apple_attestation = lookup(context, "FORTIS_APPLE_ATTESTATION_REQUIRED");
  if (apple_attestation) {
    if (strcmp(apple_attestation, "true") != 0 && strcmp(apple_attestation, "false") != 0) {
      set_error(error, error_capacity,
                "Invalid environment: FORTIS_APPLE_ATTESTATION_REQUIRED must be true or false");
      return false;
    }
    config->apple_attestation_required = strcmp(apple_attestation, "true") == 0;
  }
  const char* desktop_attestation = lookup(context, "FORTIS_DESKTOP_ATTESTATION_REQUIRED");
  if (desktop_attestation) {
    if (strcmp(desktop_attestation, "true") != 0 && strcmp(desktop_attestation, "false") != 0) {
      set_error(error, error_capacity,
                "Invalid environment: FORTIS_DESKTOP_ATTESTATION_REQUIRED must be true or false");
      return false;
    }
    config->desktop_attestation_required = strcmp(desktop_attestation, "true") == 0;
  }
  const char* apple_environment = lookup(context, "APPLE_APP_ATTEST_ENVIRONMENT");
  if (apple_environment && strcmp(apple_environment, "production") != 0 &&
      strcmp(apple_environment, "development") != 0) {
    set_error(error, error_capacity,
              "Invalid environment: APPLE_APP_ATTEST_ENVIRONMENT must be production or development");
    return false;
  }
  config->apple_attest_development =
      apple_environment && strcmp(apple_environment, "development") == 0;
  if (production && config->apple_attest_development) {
    set_error(error, error_capacity,
              "APPLE_APP_ATTEST_ENVIRONMENT=development is not allowed in production");
    return false;
  }

  value = lookup(context, "DEVICE_TOKEN_TTL_SECONDS");
  if (value) {
    double seconds = 0;
    if (!integral_number(value, &seconds) || seconds < 300 || seconds > 3600) {
      set_error(error, error_capacity,
                "Invalid environment: DEVICE_TOKEN_TTL_SECONDS must be an integer from 300 to 3600");
      return false;
    }
    config->device_token_ttl_seconds = static_cast<uint32_t>(seconds);
    config->device_token_ttl_ms = static_cast<int64_t>(seconds) * 1000;
  }

  const char* diagnostic_probes = lookup(context, "FORTIS_ENABLE_DIAGNOSTIC_PROBES");
  if (diagnostic_probes && *diagnostic_probes && strcmp(diagnostic_probes, "0") != 0 &&
      strcmp(diagnostic_probes, "1") != 0) {
    set_error(error, error_capacity,
              "Invalid environment: FORTIS_ENABLE_DIAGNOSTIC_PROBES must be 0 or 1");
    return false;
  }
  config->diagnostic_probes_enabled = diagnostic_probes && strcmp(diagnostic_probes, "1") == 0;

  const char* host = lookup(context, "FORTIS_TLS_PROBE_HOST");
  const char* tls_port_value = lookup(context, "FORTIS_TLS_PROBE_PORT");
  const char* ca = lookup(context, "FORTIS_TLS_PROBE_CA");
  const bool has_host = host && *host;
  const bool has_port = tls_port_value && *tls_port_value;
  const bool has_ca = ca && *ca;
  if (has_host || has_port || has_ca) {
    uint16_t normalized_tls_port = 0;
    if (!has_host || !has_port || !has_ca || !port_number(tls_port_value, &normalized_tls_port) ||
        strlen(host) > 253 || strlen(ca) >= sizeof(config->tls_probe_ca)) {
      set_error(error, error_capacity,
                "Invalid environment: TLS probe host, port, and CA must be valid and configured together");
      return false;
    }
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(host); *p; ++p) {
      if (*p <= 0x20 || *p == 0x7f) {
        set_error(error, error_capacity, "Invalid environment: FORTIS_TLS_PROBE_HOST is invalid");
        return false;
      }
    }
    if (!copy_value(config->tls_probe_host, sizeof(config->tls_probe_host), host) ||
        !copy_value(config->tls_probe_ca, sizeof(config->tls_probe_ca), ca)) {
      set_error(error, error_capacity, "Invalid environment: TLS probe settings are too long");
      return false;
    }
    snprintf(config->tls_probe_port, sizeof(config->tls_probe_port), "%u",
             static_cast<unsigned>(normalized_tls_port));
    config->tls_probe_configured = true;
  }

  const char* ca_bundle = lookup(context, "FORTIS_CA_BUNDLE_PATH");
  if (ca_bundle && *ca_bundle) {
    if (!copy_value(config->ca_bundle_path, sizeof(config->ca_bundle_path), ca_bundle)) {
      set_error(error, error_capacity, "Invalid environment: FORTIS_CA_BUNDLE_PATH is too long");
      return false;
    }
    config->ca_bundle_configured = true;
  }

  const char* metrics_token = lookup(context, "METRICS_TOKEN");
  if (metrics_token) {
    const size_t raw_length = strnlen(metrics_token, 4097);
    if (raw_length > 4096) {
      set_error(error, error_capacity, "Invalid environment: METRICS_TOKEN is too long");
      return false;
    }
    size_t begin = 0;
    size_t end = raw_length;
    while (begin < end && (metrics_token[begin] == ' ' || metrics_token[begin] == '\t' ||
           metrics_token[begin] == '\r' || metrics_token[begin] == '\n')) ++begin;
    while (end > begin && (metrics_token[end - 1] == ' ' || metrics_token[end - 1] == '\t' ||
           metrics_token[end - 1] == '\r' || metrics_token[end - 1] == '\n')) --end;
    const size_t token_length = end - begin;
    if (token_length > sizeof(config->metrics_token) - 1) {
      set_error(error, error_capacity, "Invalid environment: METRICS_TOKEN is too long");
      return false;
    }
    if (token_length) {
      memcpy(config->metrics_token, metrics_token + begin, token_length);
      config->metrics_token[token_length] = '\0';
      config->metrics_enabled = true;
    }
  }

  const char* coingecko_tier = lookup(context, "COINGECKO_TIER");
  if (coingecko_tier) {
    if (strcmp(coingecko_tier, "demo") == 0)
      config->coingecko_tier = CoinGeckoTier::kDemo;
    else if (strcmp(coingecko_tier, "pro") == 0)
      config->coingecko_tier = CoinGeckoTier::kPro;
    else {
      set_error(error, error_capacity,
                "Invalid environment: COINGECKO_TIER must be demo or pro");
      return false;
    }
  }

  const char* coingecko_key = lookup(context, "COINGECKO_API_KEY");
  const char* coingecko_key_file = lookup(context, "FORTIS_COINGECKO_API_KEY_FILE");
  char file_key[sizeof(config->coingecko_api_key) + 2]{};
  if (coingecko_key_file) {
    if (coingecko_key || coingecko_key_file[0] != '/' ||
        !read_coingecko_key(coingecko_key_file, file_key, sizeof(file_key))) {
      set_error(error, error_capacity,
                "Invalid environment: FORTIS_COINGECKO_API_KEY_FILE requires a private regular key file and no COINGECKO_API_KEY");
      return false;
    }
    coingecko_key = file_key;
  }
  if (coingecko_key) {
    // Bound scanning even when the environment value is unexpectedly large.
    // Never include the secret itself in diagnostics.
    const size_t raw_length = strnlen(coingecko_key, 4097);
    if (raw_length > 4096) {
      set_error(error, error_capacity,
                "Invalid environment: COINGECKO_API_KEY is too long");
      return false;
    }
    for (size_t i = 0; i < raw_length; ++i) {
      const unsigned char c = static_cast<unsigned char>(coingecko_key[i]);
      if (c < 0x20 || c == 0x7f || c > 0x7e) {
        set_error(error, error_capacity,
                  "Invalid environment: COINGECKO_API_KEY contains invalid characters");
        return false;
      }
    }
    size_t begin = 0;
    size_t end = raw_length;
    while (begin < end && coingecko_key[begin] == ' ') ++begin;
    while (end > begin && coingecko_key[end - 1] == ' ') --end;
    const size_t key_length = end - begin;
    if (!key_length && coingecko_key_file) {
      set_error(error, error_capacity, "Invalid environment: FORTIS_COINGECKO_API_KEY_FILE is blank");
      return false;
    }
    if (key_length > sizeof(config->coingecko_api_key) - 1) {
      set_error(error, error_capacity,
                "Invalid environment: COINGECKO_API_KEY is too long");
      return false;
    }
    if (key_length) {
      memcpy(config->coingecko_api_key, coingecko_key + begin, key_length);
      config->coingecko_api_key[key_length] = '\0';
      config->coingecko_api_key_configured = true;
    }
  }
  const char* jupiter_key = lookup(context, "JUPITER_API_KEY");
  if (jupiter_key) {
    const size_t length = strnlen(jupiter_key, 4097);
    if (length > 4096) {
      set_error(error, error_capacity, "Invalid environment: JUPITER_API_KEY is too long");
      return false;
    }
    size_t begin = 0, end = length;
    while (begin < end && (jupiter_key[begin] == ' ' || jupiter_key[begin] == '\t' ||
           jupiter_key[begin] == '\r' || jupiter_key[begin] == '\n')) ++begin;
    while (end > begin && (jupiter_key[end - 1] == ' ' || jupiter_key[end - 1] == '\t' ||
           jupiter_key[end - 1] == '\r' || jupiter_key[end - 1] == '\n')) --end;
    // TypeScript trims and forwards the value as the x-api-key header. The
    // native outbound client rejects control bytes in header values, so fail
    // at startup instead of on the first provider request. The secret itself
    // never appears in diagnostics.
    for (size_t i = begin; i < end; ++i) {
      const unsigned char c = static_cast<unsigned char>(jupiter_key[i]);
      if (c < 0x20 || c == 0x7f || c > 0x7e) {
        set_error(error, error_capacity,
                  "Invalid environment: JUPITER_API_KEY contains invalid characters");
        return false;
      }
    }
    if (end - begin > sizeof(config->jupiter_api_key) - 1) {
      set_error(error, error_capacity, "Invalid environment: JUPITER_API_KEY is too long");
      return false;
    }
    if (end != begin) {
      memcpy(config->jupiter_api_key, jupiter_key + begin, end - begin);
      config->jupiter_api_key[end - begin] = '\0';
      config->jupiter_api_key_configured = true;
    }
  }
  const char* tronscan_key = lookup(context, "TRONSCAN_API_KEY");
  if (tronscan_key) {
    const size_t length = strnlen(tronscan_key, 4097);
    if (length > 4096) {
      set_error(error, error_capacity, "Invalid environment: TRONSCAN_API_KEY is too long");
      return false;
    }
    size_t begin = 0, end = length;
    while (begin < end && (tronscan_key[begin] == ' ' || tronscan_key[begin] == '\t' ||
           tronscan_key[begin] == '\r' || tronscan_key[begin] == '\n')) ++begin;
    while (end > begin && (tronscan_key[end - 1] == ' ' || tronscan_key[end - 1] == '\t' ||
           tronscan_key[end - 1] == '\r' || tronscan_key[end - 1] == '\n')) --end;
    for (size_t i = begin; i < end; ++i) {
      const unsigned char c = static_cast<unsigned char>(tronscan_key[i]);
      if (c < 0x20 || c == 0x7f || c > 0x7e) {
        set_error(error, error_capacity,
                  "Invalid environment: TRONSCAN_API_KEY contains invalid characters");
        return false;
      }
    }
    if (end - begin > sizeof(config->tronscan_api_key) - 1) {
      set_error(error, error_capacity, "Invalid environment: TRONSCAN_API_KEY is too long");
      return false;
    }
    if (end != begin) {
      memcpy(config->tronscan_api_key, tronscan_key + begin, end - begin);
      config->tronscan_api_key[end - begin] = '\0';
      config->tronscan_api_key_configured = true;
    }
  }

  const char* taostats_key = lookup(context, "TAOSTATS_API_KEY");
  if (taostats_key) {
    const size_t length = strnlen(taostats_key, 4097);
    if (length > 4096) {
      set_error(error, error_capacity, "Invalid environment: TAOSTATS_API_KEY is too long");
      return false;
    }
    size_t begin = 0, end = length;
    while (begin < end && (taostats_key[begin] == ' ' || taostats_key[begin] == '\t' ||
           taostats_key[begin] == '\r' || taostats_key[begin] == '\n')) ++begin;
    while (end > begin && (taostats_key[end - 1] == ' ' || taostats_key[end - 1] == '\t' ||
           taostats_key[end - 1] == '\r' || taostats_key[end - 1] == '\n')) --end;
    for (size_t i = begin; i < end; ++i) {
      const unsigned char c = static_cast<unsigned char>(taostats_key[i]);
      if (c < 0x20 || c == 0x7f || c > 0x7e) {
        set_error(error, error_capacity,
                  "Invalid environment: TAOSTATS_API_KEY contains invalid characters");
        return false;
      }
    }
    if (end - begin > sizeof(config->taostats_api_key) - 1) {
      set_error(error, error_capacity, "Invalid environment: TAOSTATS_API_KEY is too long");
      return false;
    }
    if (end != begin) {
      memcpy(config->taostats_api_key, taostats_key + begin, end - begin);
      config->taostats_api_key[end - begin] = '\0';
      config->taostats_api_key_configured = true;
    }
  }

  // Optional provider keys, trimmed like the TypeScript optionalKey.
  struct OptionalKey { const char* name; char* value; size_t capacity; bool* configured; };
  const OptionalKey optional_keys[] = {
      {"ETHERSCAN_API_KEY", config->etherscan_api_key, sizeof(config->etherscan_api_key),
       &config->etherscan_api_key_configured},
      {"ETHPLORER_API_KEY", config->ethplorer_api_key, sizeof(config->ethplorer_api_key),
       &config->ethplorer_api_key_configured},
      {"ALCHEMY_API_KEY", config->alchemy_api_key, sizeof(config->alchemy_api_key),
       &config->alchemy_api_key_configured},
      {"HELIUS_API_KEY", config->helius_api_key, sizeof(config->helius_api_key),
       &config->helius_api_key_configured},
      {"TRONGRID_API_KEY", config->trongrid_api_key, sizeof(config->trongrid_api_key),
       &config->trongrid_api_key_configured},
      {"NEARBLOCKS_API_KEY", config->nearblocks_api_key, sizeof(config->nearblocks_api_key),
       &config->nearblocks_api_key_configured},
      {"GETBLOCK_DOGE_TOKEN", config->getblock_doge_token, sizeof(config->getblock_doge_token),
       &config->getblock_doge_token_configured},
      {"GETBLOCK_LTC_TOKEN", config->getblock_ltc_token, sizeof(config->getblock_ltc_token),
       &config->getblock_ltc_token_configured},
      {"GETBLOCK_ZEC_TOKEN", config->getblock_zec_token, sizeof(config->getblock_zec_token),
       &config->getblock_zec_token_configured},
      // Server-side AI replies.
      {"OPENAI_API_KEY", config->openai_api_key, sizeof(config->openai_api_key),
       &config->openai_api_key_configured},
      {"OPENAI_MODEL", config->openai_model, sizeof(config->openai_model), &config->openai_model_configured},
      {"AI_PROVIDER", config->ai_provider, sizeof(config->ai_provider), &config->ai_provider_configured},
      {"AI_MODEL", config->ai_model, sizeof(config->ai_model), &config->ai_model_configured},
      {"AI_EFFORT", config->ai_effort, sizeof(config->ai_effort), &config->ai_effort_configured},
      {"AI_MAX_OUTPUT_TOKENS", config->ai_max_output_tokens, sizeof(config->ai_max_output_tokens),
       &config->ai_max_output_tokens_configured},
      {"AI_TIMEOUT_MS", config->ai_timeout_ms, sizeof(config->ai_timeout_ms), &config->ai_timeout_ms_configured},
      {"AI_CONCURRENCY", config->ai_concurrency, sizeof(config->ai_concurrency), &config->ai_concurrency_configured},
      {"ANTHROPIC_API_KEY_PATH", config->anthropic_api_key_path, sizeof(config->anthropic_api_key_path),
       &config->anthropic_api_key_path_configured},
      // Cross-chain providers.
      {"LIFI_API_KEY", config->lifi_api_key, sizeof(config->lifi_api_key), &config->lifi_api_key_configured},
      {"SQUID_INTEGRATOR_ID", config->squid_integrator_id, sizeof(config->squid_integrator_id),
       &config->squid_integrator_id_configured},
      {"SIMPLESWAP_API_KEY", config->simpleswap_api_key, sizeof(config->simpleswap_api_key),
       &config->simpleswap_api_key_configured},
      {"NEAR_INTENTS_JWT", config->near_intents_jwt, sizeof(config->near_intents_jwt),
       &config->near_intents_jwt_configured},
      // Sponsored Robinhood trades.
      {"ALCHEMY_GAS_POLICY_ID", config->alchemy_gas_policy_id, sizeof(config->alchemy_gas_policy_id),
       &config->alchemy_gas_policy_id_configured},
      // Hedera SaucerSwap token reads (x-api-key; keyless works).
      {"SAUCERSWAP_API_KEY", config->saucerswap_api_key, sizeof(config->saucerswap_api_key),
       &config->saucerswap_api_key_configured},
  };
  for (const OptionalKey& key : optional_keys) {
    const char* raw = lookup(context, key.name);
    if (!raw) continue;
    char message[96];
    const size_t length = strnlen(raw, 4097);
    if (length > 4096) {
      snprintf(message, sizeof(message), "Invalid environment: %s is too long", key.name);
      set_error(error, error_capacity, message);
      return false;
    }
    size_t begin = 0, end = length;
    while (begin < end && (raw[begin] == ' ' || raw[begin] == '\t' || raw[begin] == '\r' ||
           raw[begin] == '\n')) ++begin;
    while (end > begin && (raw[end - 1] == ' ' || raw[end - 1] == '\t' || raw[end - 1] == '\r' ||
           raw[end - 1] == '\n')) --end;
    for (size_t i = begin; i < end; ++i) {
      const unsigned char c = static_cast<unsigned char>(raw[i]);
      if (c <= 0x20 || c >= 0x7f) {
        snprintf(message, sizeof(message), "Invalid environment: %s contains invalid characters",
                 key.name);
        set_error(error, error_capacity, message);
        return false;
      }
    }
    if (end - begin > key.capacity - 1) {
      snprintf(message, sizeof(message), "Invalid environment: %s is too long", key.name);
      set_error(error, error_capacity, message);
      return false;
    }
    if (end != begin) {
      memcpy(key.value, raw + begin, end - begin);
      key.value[end - begin] = '\0';
      *key.configured = true;
    }
  }

  // The canary owners must start with a verified trust store: there is no
  // system-trust fallback, so reject the opt-in before any route is bound.
  const char* price_canary = lookup(context, "FORTIS_NATIVE_PRICE_ROUTE_CANARY");
  if (price_canary && *price_canary && strcmp(price_canary, "0") != 0) {
    if (strcmp(price_canary, "1") != 0) {
      set_error(error, error_capacity,
                "Invalid environment: FORTIS_NATIVE_PRICE_ROUTE_CANARY must be 0 or 1");
      return false;
    }
    if (!config->ca_bundle_configured) {
      set_error(error, error_capacity,
                "Invalid environment: FORTIS_NATIVE_PRICE_ROUTE_CANARY=1 requires FORTIS_CA_BUNDLE_PATH");
      return false;
    }
    config->native_price_route_canary = true;
  }
  const char* snapshot = lookup(context, "FORTIS_PRICE_SNAPSHOT_ENABLED");
  if (snapshot && *snapshot && strcmp(snapshot, "0") != 0) {
    if (strcmp(snapshot, "1") != 0 || !config->native_price_route_canary) {
      set_error(error, error_capacity, "FORTIS_PRICE_SNAPSHOT_ENABLED requires 0 or 1 and the native price canary");
      return false;
    }
    config->price_snapshot_enabled = true;
  }
  const char* split_prices = lookup(context, "FORTIS_SPLIT_PRICE_STORE");
  if (split_prices && strcmp(split_prices, "0") != 0) {
    if (strcmp(split_prices, "1") != 0 || !config->native_price_route_canary) {
      set_error(error, error_capacity, "FORTIS_SPLIT_PRICE_STORE requires 0 or 1 and the native price canary");
      return false;
    }
    config->split_price_store = true;
  }
  const char* news_canary = lookup(context, "FORTIS_NATIVE_NEWS_CANARY");
  if (news_canary && *news_canary && strcmp(news_canary, "0") != 0) {
    if (strcmp(news_canary, "1") != 0) {
      set_error(error, error_capacity,
                "Invalid environment: FORTIS_NATIVE_NEWS_CANARY must be 0 or 1");
      return false;
    }
    if (!config->ca_bundle_configured) {
      set_error(error, error_capacity,
                "Invalid environment: FORTIS_NATIVE_NEWS_CANARY=1 requires FORTIS_CA_BUNDLE_PATH");
      return false;
    }
    config->native_news_canary = true;
  }
  // DASH_BLOCKBOOK_URLS (optionalUrlList): split on ",", trim, drop blanks,
  // at most five, each a URL. Only https:// is usable natively.
  const char* dash_urls = lookup(context, "DASH_BLOCKBOOK_URLS");
  if (dash_urls) {
    const size_t total = strnlen(dash_urls, 8193);
    if (total > 8192) {
      set_error(error, error_capacity, "Invalid environment: DASH_BLOCKBOOK_URLS is too long");
      return false;
    }
    size_t start = 0;
    while (start <= total) {
      size_t stop = start;
      while (stop < total && dash_urls[stop] != ',') ++stop;
      size_t begin = start, end = stop;
      while (begin < end && (dash_urls[begin] == ' ' || dash_urls[begin] == '\t' ||
             dash_urls[begin] == '\r' || dash_urls[begin] == '\n')) ++begin;
      while (end > begin && (dash_urls[end - 1] == ' ' || dash_urls[end - 1] == '\t' ||
             dash_urls[end - 1] == '\r' || dash_urls[end - 1] == '\n')) --end;
      if (end > begin) {
        if (config->dash_blockbook_url_count == 5) {
          set_error(error, error_capacity,
                    "Invalid environment: DASH_BLOCKBOOK_URLS allows at most 5 URLs");
          return false;
        }
        const size_t length = end - begin;
        bool valid = length > 8 && length < sizeof(config->dash_blockbook_urls[0]) &&
                     strncmp(dash_urls + begin, "https://", 8) == 0;
        for (size_t i = begin; valid && i < end; ++i) {
          const unsigned char c = static_cast<unsigned char>(dash_urls[i]);
          if (c <= 0x20 || c >= 0x7f) valid = false;
        }
        if (!valid) {
          set_error(error, error_capacity,
                    "Invalid environment: DASH_BLOCKBOOK_URLS must list https:// URLs");
          return false;
        }
        memcpy(config->dash_blockbook_urls[config->dash_blockbook_url_count], dash_urls + begin,
               length);
        config->dash_blockbook_urls[config->dash_blockbook_url_count][length] = '\0';
        ++config->dash_blockbook_url_count;
      }
      start = stop + 1;
    }
  }
  const char* address_canary = lookup(context, "FORTIS_NATIVE_ADDRESS_CANARY");
  if (address_canary && *address_canary && strcmp(address_canary, "0") != 0) {
    if (strcmp(address_canary, "1") != 0) {
      set_error(error, error_capacity,
                "Invalid environment: FORTIS_NATIVE_ADDRESS_CANARY must be 0 or 1");
      return false;
    }
    if (!config->ca_bundle_configured) {
      set_error(error, error_capacity,
                "Invalid environment: FORTIS_NATIVE_ADDRESS_CANARY=1 requires FORTIS_CA_BUNDLE_PATH");
      return false;
    }
    config->native_address_canary = true;
  }
  // z.string().min(32): at least 32 UTF-16 units, not trimmed.
  auto load_support_secret = [&](const char* canary) -> bool {
    if (config->support_auth_secret_configured) return true;
    const char* secret = lookup(context, "SUPPORT_AUTH_SECRET");
    char file_secret[sizeof(config->support_auth_secret)]{};
    if (!secret || !*secret) {
      const char* path = lookup(context, "FORTIS_SUPPORT_AUTH_SECRET_FILE");
      if (path && *path) {
        FILE* file = fopen(path, "rb");
        if (file) {
          const size_t count = fread(file_secret, 1, sizeof(file_secret), file);
          const bool valid = !ferror(file) && count > 0 &&
                             count < sizeof(file_secret) &&
                             memchr(file_secret, '\0', count) == nullptr;
          fclose(file);
          if (valid) secret = file_secret;
        }
      }
    }
    const size_t length = secret ? strnlen(secret, sizeof(config->support_auth_secret)) : 0;
    size_t units = 0;
    for (size_t i = 0; i < length; ++i) {
      const unsigned char c = static_cast<unsigned char>(secret[i]);
      if ((c & 0xC0) != 0x80) units += c >= 0xF0 ? 2 : 1;
    }
    if (!secret || units < 32 || length >= sizeof(config->support_auth_secret)) {
      char message[160];
      snprintf(message, sizeof(message),
               "Invalid environment: %s=1 requires SUPPORT_AUTH_SECRET or FORTIS_SUPPORT_AUTH_SECRET_FILE (32 to 1023 bytes)", canary);
      set_error(error, error_capacity, message);
      return false;
    }
    memcpy(config->support_auth_secret, secret, length);
    config->support_auth_secret[length] = '\0';
    config->support_auth_secret_length = length;
    config->support_auth_secret_configured = true;
    return true;
  };
  const char* ai_canary = lookup(context, "FORTIS_NATIVE_AI_CANARY");
  if (ai_canary && *ai_canary && strcmp(ai_canary, "0") != 0) {
    if (strcmp(ai_canary, "1") != 0) {
      set_error(error, error_capacity, "Invalid environment: FORTIS_NATIVE_AI_CANARY must be 0 or 1");
      return false;
    }
    if (!config->ca_bundle_configured) {
      set_error(error, error_capacity,
                "Invalid environment: FORTIS_NATIVE_AI_CANARY=1 requires FORTIS_CA_BUNDLE_PATH");
      return false;
    }
    if (!load_support_secret("FORTIS_NATIVE_AI_CANARY")) return false;
    config->native_ai_canary = true;
  }
  // MAYA_DASH_EXECUTION_ENABLED: z.enum(['true', 'false']).default('false').
  const char* maya_enabled = lookup(context, "MAYA_DASH_EXECUTION_ENABLED");
  if (maya_enabled) {
    if (strcmp(maya_enabled, "true") == 0) {
      config->maya_dash_execution_enabled = true;
    } else if (strcmp(maya_enabled, "false") != 0) {
      set_error(error, error_capacity, "Invalid environment: MAYA_DASH_EXECUTION_ENABLED must be true or false");
      return false;
    }
  }
  // MAYA_NODE_URLS (optionalUrlList): as DASH_BLOCKBOOK_URLS, https:// only.
  const char* maya_urls = lookup(context, "MAYA_NODE_URLS");
  if (maya_urls) {
    const size_t total = strnlen(maya_urls, 8193);
    if (total > 8192) {
      set_error(error, error_capacity, "Invalid environment: MAYA_NODE_URLS is too long");
      return false;
    }
    size_t start = 0;
    while (start <= total) {
      size_t stop = start;
      while (stop < total && maya_urls[stop] != ',') ++stop;
      size_t begin = start, end = stop;
      while (begin < end && (maya_urls[begin] == ' ' || maya_urls[begin] == '\t' || maya_urls[begin] == '\r' ||
             maya_urls[begin] == '\n')) ++begin;
      while (end > begin && (maya_urls[end - 1] == ' ' || maya_urls[end - 1] == '\t' ||
             maya_urls[end - 1] == '\r' || maya_urls[end - 1] == '\n')) --end;
      if (end > begin) {
        if (config->maya_node_url_count == 5) {
          set_error(error, error_capacity, "Invalid environment: MAYA_NODE_URLS allows at most 5 URLs");
          return false;
        }
        const size_t length = end - begin;
        bool valid = length > 8 && length < sizeof(config->maya_node_urls[0]) &&
                     strncmp(maya_urls + begin, "https://", 8) == 0;
        for (size_t i = begin; valid && i < end; ++i) {
          const unsigned char c = static_cast<unsigned char>(maya_urls[i]);
          if (c <= 0x20 || c >= 0x7f || c == '?' || c == '#' || c == '@') valid = false;
        }
        if (!valid) {
          set_error(error, error_capacity,
                    "Invalid environment: MAYA_NODE_URLS must list https:// URLs without query, fragment or "
                    "credentials");
          return false;
        }
        memcpy(config->maya_node_urls[config->maya_node_url_count], maya_urls + begin, length);
        config->maya_node_urls[config->maya_node_url_count][length] = '\0';
        ++config->maya_node_url_count;
      }
      start = stop + 1;
    }
  }
  if (config->maya_dash_execution_enabled) {
    // At least two distinct origins (scheme + lowercase host[:port]).
    char origins[5][512];
    size_t distinct = 0;
    for (size_t i = 0; i < config->maya_node_url_count; ++i) {
      const char* url = config->maya_node_urls[i];
      const size_t authority = strcspn(url + 8, "/");
      char origin[512];
      size_t used = 0;
      for (size_t k = 0; k < 8 + authority && used + 1 < sizeof(origin); ++k) {
        const char c = url[k];
        origin[used++] = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
      }
      origin[used] = '\0';
      if (used > 4 && strcmp(origin + used - 4, ":443") == 0) origin[used - 4] = '\0';
      bool seen = false;
      for (size_t k = 0; k < distinct; ++k) seen = seen || strcmp(origins[k], origin) == 0;
      if (!seen) snprintf(origins[distinct++], sizeof(origins[0]), "%s", origin);
    }
    if (distinct < 2) {
      set_error(error, error_capacity,
                "Invalid environment: MAYA_DASH_EXECUTION_ENABLED requires at least two independent HTTPS "
                "MAYA_NODE_URLS");
      return false;
    }
  }
  const char* cross_chain_canary = lookup(context, "FORTIS_NATIVE_CROSS_CHAIN_CANARY");
  if (cross_chain_canary && *cross_chain_canary && strcmp(cross_chain_canary, "0") != 0) {
    if (strcmp(cross_chain_canary, "1") != 0) {
      set_error(error, error_capacity, "Invalid environment: FORTIS_NATIVE_CROSS_CHAIN_CANARY must be 0 or 1");
      return false;
    }
    if (!config->ca_bundle_configured) {
      set_error(error, error_capacity,
                "Invalid environment: FORTIS_NATIVE_CROSS_CHAIN_CANARY=1 requires FORTIS_CA_BUNDLE_PATH");
      return false;
    }
    config->native_cross_chain_canary = true;
  }
  const char* robinhood_canary = lookup(context, "FORTIS_NATIVE_ROBINHOOD_CANARY");
  if (robinhood_canary && *robinhood_canary && strcmp(robinhood_canary, "0") != 0) {
    if (strcmp(robinhood_canary, "1") != 0) {
      set_error(error, error_capacity, "Invalid environment: FORTIS_NATIVE_ROBINHOOD_CANARY must be 0 or 1");
      return false;
    }
    if (!config->ca_bundle_configured) {
      set_error(error, error_capacity,
                "Invalid environment: FORTIS_NATIVE_ROBINHOOD_CANARY=1 requires FORTIS_CA_BUNDLE_PATH");
      return false;
    }
    config->native_robinhood_canary = true;
  }
  const char* dashpay_canary = lookup(context, "FORTIS_NATIVE_DASHPAY_CANARY");
  if (dashpay_canary && *dashpay_canary && strcmp(dashpay_canary, "0") != 0) {
    if (strcmp(dashpay_canary, "1") != 0) {
      set_error(error, error_capacity, "Invalid environment: FORTIS_NATIVE_DASHPAY_CANARY must be 0 or 1");
      return false;
    }
    if (!config->ca_bundle_configured) {
      set_error(error, error_capacity,
                "Invalid environment: FORTIS_NATIVE_DASHPAY_CANARY=1 requires FORTIS_CA_BUNDLE_PATH");
      return false;
    }
    config->native_dashpay_canary = true;
  }
  return true;
}

}  // namespace fortis::config
