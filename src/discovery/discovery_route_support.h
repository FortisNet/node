#include "threading.h"
#pragma once

#include "proto_wire.h"
#include "bounded_json.h"
#include "discovery_cache.h"
#include "discovery_provider_owner.h"
#include "http_server.h"
#include "js_json.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

// Shared state and helpers of the discovery routes
// (POST /v1/bittensor/analytics, POST /v1/prices/markets and
// GET /v1/networks/{network}/tokens/search).

namespace fortis::discovery {

// Bounds of the shared workspace. One KV value is at most 1 MiB; a provider
// body at most kDiscoveryMaxResponseBytes; a normalized payload must fit the
// output buffers or the request fails as an upstream failure.
constexpr size_t kDiscoveryKvScratchBytes = 1024 * 1024;
constexpr size_t kDiscoveryTokens = 32768;
constexpr size_t kDiscoveryOutputBytes = 1024 * 1024;
constexpr uint32_t kDiscoveryHandoffTimeoutMs = 30000;
constexpr uint64_t kDiscoveryOwnerDeadlineMs = 25000;

// One allocated workspace per started discovery service, so a disabled image
// pays nothing. `mutex` owns every buffer and the cache (and so the KV
// scratch); routes hold it while they read, normalize, store and encode.
struct DiscoveryWorkspace {
  fortis::base::Mutex mutex;
  DiscoveryCache* cache;
  unsigned char kv_scratch[kDiscoveryKvScratchBytes];
  codec::BoundedJsonToken tokens[kDiscoveryTokens];
  uint32_t words[kDiscoveryTokens * fortis::compat::js_json::kScratchWordsPerToken];
  uint8_t json_out[kDiscoveryOutputBytes];
  uint8_t proto_out[kDiscoveryOutputBytes];
  uint8_t payload[kDiscoveryOutputBytes];
};

struct PriceMarketsState;
struct TokenSearchState;

}  // namespace fortis::discovery
namespace fortis::discovery { class CoinPopularity; }

namespace fortis::discovery {

struct DiscoveryRouteContext {
  // TAOSTATS_API_KEY is configured: POST /v1/bittensor/analytics forwards.
  bool taostats_api_key_configured = false;
  DiscoveryProviderOwner* owner = nullptr;
  DiscoveryWorkspace* workspace = nullptr;
  http::HttpResponseHandoff* handoff = nullptr;
  PriceMarketsState* markets = nullptr;
  TokenSearchState* search = nullptr;
  // The coin popularity tracker POST /v1/prices/markets records into (or
  // null): TokenDiscoveryFacet.markets' `popularity.record(ids, deviceId)`.
  fortis::discovery::CoinPopularity* popularity = nullptr;
  bool attestation_required = false;
};

inline fortis::compat::js_json::Scratch DiscoveryScratch(DiscoveryWorkspace* workspace) {
  return fortis::compat::js_json::Scratch{workspace->words,
      sizeof(workspace->words) / sizeof(workspace->words[0])};
}

// Content-type parsing. 0 leaves the parsed body; otherwise the framing
// refusal's status, which the route answers after the device policy
// (auth-first) with SendDiscoveryFraming: 415 for a missing content type with
// a body or an unsupported type (text/plain and form bodies included), 400
// for an empty or invalid JSON body, 413 over the native JSON bounds. Only
// application/json and application/protobuf bodies are accepted.
enum class DiscoveryBodyKind : uint8_t { kUndefined, kJson, kProtobuf };
struct DiscoveryBody {
  DiscoveryBodyKind kind = DiscoveryBodyKind::kUndefined;
  codec::BoundedJsonDocument json;
  const uint8_t* bytes = nullptr;
  size_t length = 0;
};
int ParseDiscoveryBody(const http::HttpRequest& request, codec::BoundedJsonToken* tokens, size_t token_capacity,
                       DiscoveryBody* body);
// {"error":"UNSUPPORTED_MEDIA_TYPE"} (415), {"error":"BAD_REQUEST"} (400) or
// {"error":"BODY_TOO_LARGE"} (413) with the request's quota headers.
void SendDiscoveryFraming(int client, int status, bool head, int rate_cost, uint32_t remaining);

// Shared typed reader: bounded nested messages and varints, strict UTF-8,
// preserved string BOMs, standard unknown-field skipping.
using DiscoveryProtoReader = fortis::codec::proto_wire::Reader;

// A JSON reply with the route's rate-limit headers.
void SendDiscoveryJson(int client, int status, const char* reason, const char* body,
                       bool head, int rate_cost, uint32_t remaining,
                       unsigned retry_after = 0, bool vary_accept = false);
// The facet RangeError reply: {"error":"BAD_REQUEST","detail":<message>}.
void SendDiscoveryBadRequest(int client, const char* detail, bool head, int rate_cost,
                             uint32_t remaining);
// replyBackPressure(): 429 RATE_LIMITED with retry-after (seconds, >= 1).
http::HttpResponse DiscoveryRateLimitedResponse(uint32_t retry_after_ms);
http::HttpResponse DiscoveryUpstreamUnavailableResponse();

// The appended CacheInfo provenance, JSON ({"hit":…,"timestamp":…}) and
// protobuf (field bodies only; omitted defaults as protobuf-es writes them).
void AppendDiscoveryCacheJson(fortis::compat::js_json::OutBuffer& out, bool hit, int64_t as_of_ms,
                              bool partial_key, bool partial);
void AppendDiscoveryCacheProto(fortis::compat::js_json::OutBuffer& out, bool hit, int64_t as_of_ms,
                               bool partial);

// One member of a request object (JSON object or decoded protobuf Struct) in
// source/insertion order; a repeated key keeps its first
// position and its last value.
enum class DiscoveryValueKind : uint8_t { kString, kNumber, kTrue, kFalse, kOther };
struct DiscoveryMember {
  const uint8_t* key = nullptr;   // decoded UTF-8 (points into caller storage)
  size_t key_length = 0;
  DiscoveryValueKind kind = DiscoveryValueKind::kOther;
  const uint8_t* text = nullptr;  // kString: decoded UTF-8
  size_t text_length = 0;
  double number = 0;              // kNumber
};
constexpr size_t kDiscoveryMaxMembers = 256;
// Adds or replaces `member` by key. False when more than
// kDiscoveryMaxMembers distinct keys are present.
bool DiscoveryUpsertMember(DiscoveryMember* members, size_t* count,
                           const DiscoveryMember& member);

// String.prototype.length of valid UTF-8.
size_t DiscoveryUtf16Length(const uint8_t* text, size_t length);

}  // namespace fortis::discovery
