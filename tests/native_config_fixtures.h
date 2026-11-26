#pragma once

// Frozen startup configuration contract for native settings that overlap the
// TypeScript API config, plus the native KV/TLS deployment knobs.
namespace native_config_fixtures {
constexpr uint16_t kDefaultPort = 8788;
constexpr char kDefaultKvDirectory[] = "/data";
constexpr uint32_t kDefaultDeviceTokenTtlSeconds = 3600;
constexpr uint32_t kMinimumDeviceTokenTtlSeconds = 300;
constexpr uint32_t kMaximumDeviceTokenTtlSeconds = 3600;
constexpr char kDefaultAttestationMode[] = "development";
constexpr char kProductionAttestationMode[] = "required";
constexpr char kMetricsToken[] = "metrics-test-token";

struct InvalidValue {
  const char* name;
  const char* value;
  const char* error_fragment;
};
constexpr InvalidValue kInvalidValues[] = {
    {"PORT", "0", "PORT"},
    {"PORT", "8788.5", "PORT"},
    {"PORT", "65536", "PORT"},
    {"ATTESTATION_MODE", "unknown", "ATTESTATION_MODE"},
    {"DEVICE_TOKEN_TTL_SECONDS", "299", "DEVICE_TOKEN_TTL_SECONDS"},
    {"DEVICE_TOKEN_TTL_SECONDS", "2592001", "DEVICE_TOKEN_TTL_SECONDS"},
    {"DEVICE_TOKEN_TTL_SECONDS", "", "DEVICE_TOKEN_TTL_SECONDS"},
    {"FORTIS_TLS_PROBE_HOST", "localhost", "TLS probe"},
    {"FORTIS_NATIVE_PRICE_ROUTE_CANARY", "true", "FORTIS_NATIVE_PRICE_ROUTE_CANARY"},
    {"FORTIS_NATIVE_PRICE_ROUTE_CANARY", " 1", "FORTIS_NATIVE_PRICE_ROUTE_CANARY"},
    {"FORTIS_NATIVE_PRICE_ROUTE_CANARY", "1", "requires FORTIS_CA_BUNDLE_PATH"},
};
}  // namespace native_config_fixtures
