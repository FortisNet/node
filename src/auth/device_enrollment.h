#pragma once
#include "device_auth.h"
#include "http_request.h"
#include <stdint.h>
namespace fortis::http { class HttpRouter; }

namespace fortis::auth {

// The device routes' slice of the startup configuration (the NativeConfig
// fields of the same names, validated by LoadNativeConfig). The composition
// root copies it once at startup; the routes keep a pointer to it.
struct DeviceEnrollmentConfig {
  bool attestation_required = false;
  // APPLE_APP_ATTEST_ENVIRONMENT=development: expect development App Attest keys.
  bool apple_attest_development = false;
  int64_t device_token_ttl_ms = 3600000;
};
// Set once at startup before HTTP workers start.
void ConfigureAndroidAttestation(bool required);
bool RegisterDeviceEnrollmentRoute(http::HttpRouter& router, DeviceEnrollmentConfig* config);
bool RegisterDeviceRotationRoute(http::HttpRouter& router, DeviceEnrollmentConfig* config);
bool RegisterDeviceRevocationRoute(http::HttpRouter& router, DeviceEnrollmentConfig* config);
struct DevicePolicyResult {
  fortis::auth::DeviceRecord device{};
  int status = 0;
  const char* reason = "OK";
  const char* error_body = nullptr;
  unsigned retry_after = 0;
  int rate_limit_cost = -1;
  uint32_t rate_limit_remaining = 0;
};
// Applied only by handlers for matched protected routes. Public enrollment
// and challenge handlers deliberately do not invoke this policy.
DevicePolicyResult AuthorizeDeviceRequest(const http::HttpRequest& request, uint32_t cost,
                                          bool attestation_required = false);
DevicePolicyResult AuthorizeSnapshotDeviceRequest(const http::HttpRequest& request,
                                                  bool attestation_required);

bool IsDevelopmentAddress(const char* address);
}
