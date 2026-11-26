#pragma once

#include "device_enrollment.h"
#include "http_middleware.h"

#include <stdint.h>
#include <string.h>

// The device hook as shared middleware for HttpHandler routes
// (http_middleware.h): TypeScript's /v1 onRequest chain. Any Origin header →
// 403 BROWSER_ORIGIN_FORBIDDEN; a Bearer device token that authenticates,
// else 401; then the device quota at the route's cost → 429 RATE_LIMITED
// with retry-after. AuthorizeDeviceRequest decides; this only turns its
// refusal into the response.
//
// The refusal is always application/json; charset=utf-8 whatever Accept
// says, carries the quota headers the policy filled and no vary header, and
// follows the request's HEAD handling. Routes whose refusals differ in any
// byte (another content type, extra headers) must keep their own reply.

namespace fortis::auth {

// True when `policy` refused the request and the refusal has been sent.
inline bool ReplyDevicePolicy(http::HttpReply& reply, const DevicePolicyResult& policy) {
  if (!policy.status) return false;
  const char* body = policy.error_body ? policy.error_body : "{\"error\":\"INTERNAL\"}";
  http::HttpResponse response{policy.status, policy.reason, http::kHttpJsonUtf8, reinterpret_cast<const uint8_t*>(body),
                        strlen(body)};
  response.rate_limit_cost = policy.rate_limit_cost;
  response.rate_limit_remaining = policy.rate_limit_remaining;
  response.retry_after = policy.retry_after;
  reply.Send(response);
  return true;
}

// The quota headers of an authorized request, for the route's own answers.
struct DeviceQuota {
  int cost = -1;
  uint32_t remaining = 0;
};
inline DeviceQuota DeviceQuotaOf(const DevicePolicyResult& policy) {
  return DeviceQuota{policy.rate_limit_cost, policy.rate_limit_remaining};
}

// A step for RunHttpSteps: authorizes at quota cost `kCost`. State provides
//   bool attestation_required;   (in: the service's configuration)
//   DevicePolicyResult device;   (out: the authorized device and its quota)
template <typename State, uint32_t kCost>
bool RequireDevice(const http::HttpRequest& request, http::HttpReply& reply, State& state) {
  state.device = AuthorizeDeviceRequest(request, kCost, state.attestation_required);
  return !ReplyDevicePolicy(reply, state.device);
}

// The State of a route whose only shared step is the device hook.
struct DeviceStepState {
  bool attestation_required = false;
  DevicePolicyResult device{};
};

}  // namespace fortis::auth
