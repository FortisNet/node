#pragma once

#include "discovery_route_support.h"

namespace fortis::discovery {

// POST /v1/bittensor/analytics: the port of the TypeScript
// TaostatsProxyFacet behind its discovery route. A fixed path and query
// allowlist, network-scoped resources forced to finney, bounded pagination,
// a 15 s / 300 s stale-while-revalidate cache keyed by a digest of the
// canonical query, and the Taostats lane with the configured key. The full
// contract is the comment above `handle` in bittensor_route.cpp.
bool RegisterBittensorAnalyticsRoute(http::HttpRouter& router, DiscoveryRouteContext* context);

// The request plan below the HTTP layer (shared with the unit test): the
// decoded body through zod's `taostatsBody` and TaostatsProxyFacet.read's
// checks, then the provider path (`<path>?<URLSearchParams>`) and the cache
// key (`taostats:<sha256(path:JSON.stringify(sorted entries))[:24]>`).
enum class BittensorPlanVerdict : uint8_t {
  kFetch,                // read `cache_key`, fetch `provider_path` on a miss
  kBadRequest,           // 400 {"error":"BAD_REQUEST"[,"detail":detail]}
  kUpstreamUnavailable,  // 502: `PATHS[path]` found an Object.prototype member
};
struct BittensorAnalyticsPlan {
  BittensorPlanVerdict verdict = BittensorPlanVerdict::kBadRequest;
  const char* detail = nullptr;  // the facet's RangeError message, or null
  char provider_path[kDiscoveryMaxPathBytes];
  size_t provider_path_length = 0;
  char cache_key[48];
};
// `body` as ParseDiscoveryBody left it; decoded JSON strings go to `arena`.
// Uses static scratch: the caller holds the discovery workspace mutex (or is
// single-threaded).
void PlanBittensorAnalytics(const DiscoveryBody& body, bool configured, uint8_t* arena,
                            size_t arena_capacity, BittensorAnalyticsPlan* plan);
// res.json() then the SwrCache value: the provider body (one BOM dropped)
// through JSON.parse and JSON.stringify into workspace->payload.
bool BittensorNormalizePayload(DiscoveryWorkspace* workspace, const unsigned char* body,
                               size_t length, size_t* payload_length);
// The 200 for a stored payload, JSON or BittensorAnalyticsResponse protobuf,
// written into the workspace output buffers.
bool BittensorEncodeResponse(DiscoveryWorkspace* workspace, const uint8_t* payload,
                             size_t payload_length, bool hit, int64_t as_of_ms, bool protobuf,
                             http::HttpResponse* response);

}  // namespace fortis::discovery
