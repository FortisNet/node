#include "bittensor_route.h"

// POST /v1/bittensor/analytics. One route object for the normal image (behind
// FORTIS_NATIVE_PRICE_ROUTE_CANARY=1) and the test binaries; the test clock
// and jitter arrive only through the DiscoveryCache seams. The contract is the
// comment above `handle`.

#include "device_enrollment.h"
#include "price_history_batch_codec.h"
#include "proto_wire.h"
#include "threading.h"

#include <mbedtls/sha256.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

namespace fortis::discovery {

namespace {

using fortis::compat::js_json::OutBuffer;

constexpr uint32_t kCost = 10;
constexpr size_t kSlots = http::HttpResponseHandoff::kCapacity;
constexpr size_t kMaxQueryValueUnits = 160;
constexpr DiscoveryCacheTtl kTtl = kDiscoveryTtlTaostats;
constexpr int kRecursionLimit = 100;  // protobuf-es readMessage

struct AllowedPath {
  const char* path;
  const char* keys[7];
};

// The TypeScript TaostatsProxyFacet PATHS, in source order.
constexpr AllowedPath kPaths[] = {
  {"/api/account/latest/v1", {"address", "network"}},
  {"/api/extrinsic/v1", {"hash", "signer_address", "network", "limit", "page", "order"}},
  {"/api/call/v1", {"origin_address", "network", "limit", "page", "order"}},
  {"/api/proxy_call/v1", {"real_address", "network", "limit", "page", "order"}},
  {"/api/dtao/pool/latest/v1", {"network", "netuid", "limit", "page", "order"}},
  {"/api/dtao/validator/yield/latest/v1", {"network", "limit", "page", "order"}},
  {"/api/dtao/stake_balance/latest/v1", {"coldkey", "network", "limit", "page", "order"}},
  {"/api/subnet/owner/v1", {"owner", "limit", "page", "order"}},
  {"/api/transfer/v1", {"address", "network", "limit", "page", "order"}},
};


bool text_equals(const uint8_t* text, size_t length, const char* literal) {
  return strlen(literal) == length && memcmp(text, literal, length) == 0;
}

bool allows(const AllowedPath& path, const uint8_t* key, size_t length) {
  for (const char* allowed : path.keys)
    if (allowed && text_equals(key, length, allowed)) return true;
  return false;
}

// One in-flight provider read. A foreground slot is a cold miss answered
// through the handoff; a background slot is a stale entry's revalidation.
struct Slot {
  bool used = false, cancelled = false, background = false, protobuf = false, stored = false;
  unsigned references = 0;
  uint64_t handoff_token = 0, request_id = 0;
  DiscoveryRouteContext* route = nullptr;
  int rate_cost = -1;
  uint32_t rate_remaining = 0;
  char cache_key[48]{};
  char path[kDiscoveryMaxPathBytes]{};
};
Slot slots[kSlots]{};
fortis::base::Mutex slots_mutex;

void release(Slot* slot) {
  fortis::base::LockGuard slots_lock(slots_mutex);
  if (slot->references) --slot->references;
  if (!slot->references) slot->used = false;
}

// A free slot, or null. A background refresh is refused while another
// refresh of the same key is pending (SwrCache's `revalidating` set). A cold
// miss whose key a foreground slot is already fetching joins that flight
// (SwrCache's `swr:<key>` single flight): it submits the leader's provider
// path, which the owner shares as one provider request; the first completion
// of the flight stores the answer once.
Slot* acquire(DiscoveryRouteContext* route, bool background, const char* key, const char* path) {
  fortis::base::LockGuard slots_lock(slots_mutex);
  const Slot* leader = nullptr;
  for (Slot& slot : slots) {
    if (!slot.used || slot.background != background || strcmp(slot.cache_key, key) != 0) continue;
    if (background) return nullptr;
    if (!slot.cancelled && slot.path[0]) { leader = &slot; break; }
  }
  Slot* result = nullptr;
  for (Slot& slot : slots) if (!slot.used) {
    slot.used = true; slot.cancelled = false; slot.protobuf = false; slot.references = 1;
    slot.handoff_token = 0; slot.request_id = 0; slot.route = route; slot.background = background;
    slot.rate_cost = -1; slot.rate_remaining = 0; slot.stored = false;
    snprintf(slot.cache_key, sizeof(slot.cache_key), "%s", key);
    snprintf(slot.path, sizeof(slot.path), "%s", leader ? leader->path : path);
    result = &slot;
    break;
  }
  return result;
}

void handoff_terminal(void* opaque, bool cancelled) {
  auto* slot = static_cast<Slot*>(opaque);
  if (cancelled) {
    fortis::base::UniqueLock slots_lock(slots_mutex);
    slot->cancelled = true;
    const uint64_t request_id = slot->request_id;
    DiscoveryRouteContext* route = slot->route;
    slots_lock.Unlock();
    if (request_id && route && route->owner) route->owner->Cancel(request_id);
  }
  release(slot);
}

// ── Request decoding ──

struct Query {
  const uint8_t* path = nullptr;
  size_t path_length = 0;
  DiscoveryMember members[kDiscoveryMaxMembers];
  size_t member_count = 0;
};

// Decoded JSON strings live in the caller's arena while the request is
// validated.
struct Arena {
  uint8_t* data;
  size_t capacity;
  size_t used = 0;
  const uint8_t* Decode(const codec::BoundedJsonDocument& json, uint32_t token, size_t* length) {
    if (used >= capacity) return nullptr;
    size_t written = 0;
    if (!json.DecodeString(token, reinterpret_cast<char*>(data + used), capacity - used,
                           &written)) return nullptr;
    const uint8_t* text = data + used;
    used += written + 1;
    *length = written;
    return text;
  }
};

// zod `taostatsBody` over a parsed JSON object. False is the 400 without
// detail.
bool decode_json(const codec::BoundedJsonDocument& json, Arena& arena, Query* query) {
  const codec::BoundedJsonToken* root = json.token(json.root());
  if (!root || root->type != codec::BoundedJsonType::kObject) return false;
  uint32_t path = codec::kBoundedJsonNoToken, values = codec::kBoundedJsonNoToken;
  for (uint32_t key = root->first_child; key != codec::kBoundedJsonNoToken;) {
    const uint32_t value = json.token(key)->next_sibling;
    if (json.StringEquals(key, "path")) path = value;
    else if (json.StringEquals(key, "query")) values = value;
    else return false;  // .strict()
    key = json.token(value)->next_sibling;
  }
  const codec::BoundedJsonToken* path_token = path == codec::kBoundedJsonNoToken ? nullptr : json.token(path);
  if (!path_token || path_token->type != codec::BoundedJsonType::kString) return false;
  query->path = arena.Decode(json, path, &query->path_length);
  if (!query->path) return false;
  const size_t units = DiscoveryUtf16Length(query->path, query->path_length);
  if (units < 1 || units > 100) return false;
  if (values == codec::kBoundedJsonNoToken) return true;  // .default({})
  const codec::BoundedJsonToken* object = json.token(values);
  if (object->type != codec::BoundedJsonType::kObject) return false;
  for (uint32_t key = object->first_child; key != codec::kBoundedJsonNoToken;) {
    const uint32_t value = json.token(key)->next_sibling;
    DiscoveryMember member{};
    member.key = arena.Decode(json, key, &member.key_length);
    if (!member.key) return false;
    const codec::BoundedJsonToken* item = json.token(value);
    switch (item->type) {
      case codec::BoundedJsonType::kString:
        member.kind = DiscoveryValueKind::kString;
        member.text = arena.Decode(json, value, &member.text_length);
        if (!member.text) return false;
        break;
      case codec::BoundedJsonType::kNumber:
        member.kind = DiscoveryValueKind::kNumber;
        if (!json.Number(value, &member.number)) return false;
        break;
      case codec::BoundedJsonType::kTrue: member.kind = DiscoveryValueKind::kTrue; break;
      case codec::BoundedJsonType::kFalse: member.kind = DiscoveryValueKind::kFalse; break;
      default: member.kind = DiscoveryValueKind::kOther; break;
    }
    if (!DiscoveryUpsertMember(query->members, &query->member_count, member)) return false;
    key = item->next_sibling;
  }
  return true;
}

// fromBinary(BittensorAnalyticsRequestSchema), read exactly as protobuf-es
// 2.16 reads it (DiscoveryProtoReader): one cursor over the body, each
// message bounded by its declared length only in its loop condition.
bool es_value(DiscoveryProtoReader& r, uint32_t length, int depth, DiscoveryMember* member);

// google.protobuf.Struct: `map<string, Value> fields = 1`. A map entry is
// parity-only, see compat-decisions.md#protobuf-es-reader
// read as protobuf-es reads it: key (1) and value (2) at any wire type, any
// other tag consumed WITHOUT its value (2.16 does not skip it), a repeated
// value merged into the previous one; a missing key is "", a missing value
// has no kind. `members` null decodes and discards (a nested Struct).
bool es_struct(DiscoveryProtoReader& r, uint32_t length, int depth, DiscoveryMember* members,
               size_t* count) {
  if (++depth > kRecursionLimit) return false;
  const size_t end = r.pos() + length;
  DiscoveryProtoReader::Frame frame(r, end);
  if (!frame.ok()) return false;
  while (r.pos() < end) {
    uint32_t field = 0, wire = 0;
    if (!r.Tag(&field, &wire)) return false;
    if (field != 1) {
      if (!r.Skip(wire, field, kRecursionLimit - depth)) return false;
      continue;
    }
    uint32_t entry_length = 0;
    if (!r.MessageLength(&entry_length)) return false;
    const size_t entry_end = r.pos() + entry_length;
    DiscoveryProtoReader::Frame entry_frame(r, entry_end);
    if (!entry_frame.ok()) return false;
    DiscoveryMember member{};
    member.key = reinterpret_cast<const uint8_t*>("");
    member.kind = DiscoveryValueKind::kOther;
    while (r.pos() < entry_end) {
      uint32_t entry_field = 0, entry_wire = 0;
      if (!r.Tag(&entry_field, &entry_wire)) return false;
      if (entry_field == 1) {
        if (!r.String(&member.key, &member.key_length)) return false;
      } else if (entry_field == 2) {
        uint32_t value_length = 0;
        if (!r.MessageLength(&value_length) || !es_value(r, value_length, depth, &member)) return false;
      } else if (!r.Skip(entry_wire, entry_field, kRecursionLimit - depth)) return false;
    }
    if (members && !DiscoveryUpsertMember(members, count, member)) return false;
  }
  return true;
}

// google.protobuf.ListValue: `repeated Value values = 1` (decoded, discarded).
bool es_list(DiscoveryProtoReader& r, uint32_t length, int depth) {
  if (++depth > kRecursionLimit) return false;
  const size_t end = r.pos() + length;
  DiscoveryProtoReader::Frame frame(r, end);
  if (!frame.ok()) return false;
  while (r.pos() < end) {
    uint32_t field = 0, wire = 0;
    if (!r.Tag(&field, &wire)) return false;
    if (field != 1) {
      if (!r.Skip(wire, field, kRecursionLimit - depth)) return false;
      continue;
    }
    uint32_t value_length = 0;
    DiscoveryMember ignored{};
    if (!r.MessageLength(&value_length) || !es_value(r, value_length, depth, &ignored)) return false;
  }
  return true;
}

// google.protobuf.Value: the oneof's last field wins; a struct or list value
// is decoded (it must be well formed) and is not a zod scalar.
bool es_value(DiscoveryProtoReader& r, uint32_t length, int depth, DiscoveryMember* member) {
  if (++depth > kRecursionLimit) return false;
  const size_t end = r.pos() + length;
  DiscoveryProtoReader::Frame frame(r, end);
  if (!frame.ok()) return false;
  while (r.pos() < end) {
    uint32_t field = 0, wire = 0;
    if (!r.Tag(&field, &wire)) return false;
    switch (field) {
      case 1:  // null_value
        if (!r.Int32()) return false;
        member->kind = DiscoveryValueKind::kOther;
        break;
      case 2:  // number_value
        if (!r.Double(&member->number)) return false;
        member->kind = DiscoveryValueKind::kNumber;
        break;
      case 3:  // string_value
        if (!r.String(&member->text, &member->text_length)) return false;
        member->kind = DiscoveryValueKind::kString;
        break;
      case 4: {  // bool_value
        bool value = false;
        if (!r.Bool(&value)) return false;
        member->kind = value ? DiscoveryValueKind::kTrue : DiscoveryValueKind::kFalse;
        break;
      }
      case 5: case 6: {  // struct_value, list_value
        uint32_t nested = 0;
        if (!r.MessageLength(&nested)) return false;
        if (field == 5 ? !es_struct(r, nested, depth, nullptr, nullptr)
                       : !es_list(r, nested, depth)) return false;
        member->kind = DiscoveryValueKind::kOther;
        break;
      }
      default:
        if (!r.Skip(wire, field, kRecursionLimit - depth)) return false;
    }
  }
  return true;
}

// The request message, then `{path: value.path, query: value.query ?? {}}`
// through zod. False is the bare 400 either way.
bool decode_protobuf(const uint8_t* body, size_t length, Query* query) {
  DiscoveryProtoReader r(body, length);
  query->path = reinterpret_cast<const uint8_t*>("");
  const int depth = 1;
  while (r.pos() < length) {
    uint32_t field = 0, wire = 0;
    if (!r.Tag(&field, &wire)) return false;
    if (field == 1) {
      if (!r.String(&query->path, &query->path_length)) return false;
    } else if (field == 2) {
      // A repeated singular message field merges: entries accumulate. More
      // than kDiscoveryMaxMembers distinct keys is refused as invalid input.
      uint32_t fields = 0;
      if (!r.MessageLength(&fields) ||
          !es_struct(r, fields, depth, query->members, &query->member_count)) return false;
    } else if (!r.Skip(wire, field, kRecursionLimit - depth)) {
      return false;
    }
  }
  const size_t units = DiscoveryUtf16Length(query->path, query->path_length);
  return units >= 1 && units <= 100;
}

bool zod_values(const Query& query) {
  for (size_t i = 0; i < query.member_count; ++i) {
    const DiscoveryMember& member = query.members[i];
    if (member.kind == DiscoveryValueKind::kOther) return false;
    if (member.kind == DiscoveryValueKind::kNumber) {
      const double value = member.number;
      if (value != value || value - value != 0) return false;  // NaN or ±Infinity
    }
  }
  return true;
}

// String(value) of a validated query member.
bool member_text(const DiscoveryMember& member, uint8_t* storage, size_t capacity,
                 const uint8_t** text, size_t* length) {
  switch (member.kind) {
    case DiscoveryValueKind::kString: *text = member.text; *length = member.text_length; return true;
    case DiscoveryValueKind::kTrue: *text = reinterpret_cast<const uint8_t*>("true"); *length = 4; return true;
    case DiscoveryValueKind::kFalse: *text = reinterpret_cast<const uint8_t*>("false"); *length = 5; return true;
    case DiscoveryValueKind::kNumber: {
      OutBuffer out{storage, capacity};
      fortis::codec::history_batch::AppendJsNumber(out, member.number);
      *text = storage; *length = out.used;
      return out.ok;
    }
    case DiscoveryValueKind::kOther: return false;
  }
  return false;
}

// /^\d+$/ and Number(text) > limit.
bool digits_within(const uint8_t* text, size_t length, double limit) {
  if (!length) return false;
  double value = 0;
  for (size_t i = 0; i < length; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    value = value * 10 + (text[i] - '0');
  }
  // Number() rounds the whole literal once; repeated multiply-add can only
  // differ from it far above these small limits.
  return !(value > limit);
}

// URLSearchParams' application/x-www-form-urlencoded byte serializer.
void append_form(OutBuffer& out, const uint8_t* text, size_t length) {
  static const char kHex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < length; ++i) {
    const uint8_t c = text[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '*' || c == '-' || c == '.' || c == '_') {
      out.Byte(c);
    } else if (c == ' ') {
      out.Byte('+');
    } else {
      out.Byte('%'); out.Byte(static_cast<uint8_t>(kHex[c >> 4]));
      out.Byte(static_cast<uint8_t>(kHex[c & 15]));
    }
  }
}

struct Param {
  const uint8_t* key;
  size_t key_length;
  const uint8_t* value;
  size_t value_length;
};

// ── Responses ──

void complete_slot(Slot* slot, http::HttpResponse response) {
  fortis::base::UniqueLock slots_lock(slots_mutex);
  const bool cancelled = slot->cancelled;
  slots_lock.Unlock();
  if (cancelled || slot->background) return;
  response.rate_limit_cost = slot->rate_cost;
  response.rate_limit_remaining = slot->rate_remaining;
  slot->route->handoff->Complete(slot->handoff_token, response);
}

void provider_complete(void* opaque, const DiscoveryProviderResult& result,
                       const unsigned char* body, size_t body_length) {
  auto* slot = static_cast<Slot*>(opaque);
  DiscoveryWorkspace* workspace = slot->route->workspace;
  fortis::base::UniqueLock slots_lock(slots_mutex);
  const bool client_cancelled = slot->cancelled;
  slot->request_id = 0;
  slots_lock.Unlock();
  fortis::base::UniqueLock lock(workspace->mutex);
  DiscoveryCache* cache = workspace->cache;
  const int64_t now = cache->Now();
  size_t payload_length = 0;
  const bool ok = !result.transport_error && !result.cancelled &&
      result.http_status >= 200 && result.http_status < 300 &&
      BittensorNormalizePayload(workspace, body, body_length, &payload_length);
  if (ok) {
    // The flight's first completion stores the answer and marks every miss
    // of the key in flight (its joined requests) as stored. A failed KV write
    // is logged by TypeScript and the value still answers.
    slots_lock.Lock();
    const bool stored = slot->stored;
    slots_lock.Unlock();
    if (!stored) {
      cache->Write(slot->cache_key, kTtl, now, workspace->payload, payload_length, nullptr);
      if (!slot->background) {
        slots_lock.Lock();
        for (Slot& other : slots)
          if (other.used && !other.background && strcmp(other.cache_key, slot->cache_key) == 0)
            other.stored = true;
        slots_lock.Unlock();
      }
    }
    http::HttpResponse response{};
    if (!slot->background) {
      if (BittensorEncodeResponse(workspace, workspace->payload, payload_length, false, -1,
                                  slot->protobuf, &response)) complete_slot(slot, response);
      else complete_slot(slot, DiscoveryUpstreamUnavailableResponse());
    }
  } else if (!(result.cancelled && client_cancelled)) {
    if (slot->background) {
      cache->RecordRevalidateFailure(slot->cache_key, now, result.lane_rejected);
    } else {
      // The hold replays the original error: a LaneBusyError keeps its
      // retry-after, carried in the held status as 429 + 1000 * seconds.
      const uint32_t retry_ms = result.retry_after_ms ? result.retry_after_ms : 2000;
      const http::HttpResponse response = result.lane_rejected
          ? DiscoveryRateLimitedResponse(retry_ms) : DiscoveryUpstreamUnavailableResponse();
      cache->RecordMissFailure(slot->cache_key, now, result.lane_rejected,
          result.lane_rejected ? 429 + 1000 * static_cast<int>(response.retry_after) : 502);
      complete_slot(slot, response);
    }
  }
  lock.Unlock();
  release(slot);
}

// Starts the provider request for `slot`; the caller holds one reference for
// itself, and this adds the owner callback's. False when the owner refused.
bool submit(Slot* slot) {
  fortis::base::UniqueLock slots_lock(slots_mutex);
  ++slot->references;
  slots_lock.Unlock();
  uint64_t request_id = 0;
  // lanes.run('taostats', …) inside HttpClient.json: a cold read queues at
  // foreground priority; SwrCache.revalidate runs under the swr_revalidate
  // work reason, background priority (shed while the lane is busy). An
  // unparseable 2xx is a transport failure, retried like a 5xx.
  if (!slot->route->owner->Submit(DiscoveryUpstream::kTaostats, slot->path,
                                  DiscoveryMonotonicMs() + kDiscoveryOwnerDeadlineMs,
                                  slot->background, provider_complete, slot, &request_id,
                                  /*retry_unparseable=*/true)) {
    release(slot);
    return false;
  }
  slots_lock.Lock();
  slot->request_id = request_id;
  const bool cancelled = slot->cancelled;
  slots_lock.Unlock();
  if (cancelled) slot->route->owner->Cancel(request_id);
  return true;
}

void send_upstream_unavailable(int client, bool head, int cost, uint32_t remaining) {
  SendDiscoveryJson(client, 502, "Bad Gateway", "{\"error\":\"UPSTREAM_UNAVAILABLE\"}", head,
                    cost, remaining);
}

// ── POST /v1/bittensor/analytics ──────────────────────────────────────────
//
// TypeScript: getBittensorAnalytics over TaostatsProxyFacet.read; its
// recorded edge cases are tests/corpus/bittensor_analytics_edges.json,
// replayed by
// tests/bittensor_analytics_edges_test.py, unit tests/bittensor_analytics_test.cpp.
// Flutter: IntelligenceApi.bittensorAnalytics posts a protobuf
// BittensorAnalyticsRequest (the query map as a Struct) with Accept:
// application/protobuf and reads payload.toProto3Json() for the app's
// Taostats parser (account, extrinsic, call, proxy call, transfer, stake
// balance, dTAO pool/yield and subnet-owner pages).
//
// Method. POST only; GET/HEAD/PUT, a trailing slash, another case or segment
// are the router's 404 ({"error":"NOT_FOUND"}). A query string is ignored.
//
// Order. 1. The body parser, answered after step 2 (auth-first: an
// unauthenticated request is 401 whatever its body; a framing refusal is
// charged and carries the quota headers): content-type application/json
// (any case, parameters allowed) → JSON (one BOM dropped; empty body,
// invalid JSON, whitespace only, trailing bytes → 400 {"error":
// "BAD_REQUEST"}; `__proto__` is an ordinary key); application/protobuf →
// the raw bytes; text/plain, form bodies, a body without a content type,
// application/vnd.api+json, application/jsonx, application/x-protobuf,
// octet-stream or any other type → 415 {"error":"UNSUPPORTED_MEDIA_TYPE"}.
// No content type and no body is `undefined`.
// 2. preHandler: an Origin header → 403 BROWSER_ORIGIN_FORBIDDEN (before the
// bearer); `Bearer fintel_dev_…` of an unexpired, unrevoked device else 401
// UNAUTHORIZED / SESSION_EXPIRED; ATTESTATION_MODE=required refuses a
// development device (401 ATTESTATION_REQUIRED); then the per-minute device
// quota is charged 10 (`/bittensor/analytics`), whatever the body: over it →
// 429 {"error":"RATE_LIMITED"} with retry-after; x-rate-limit-cost and
// x-rate-limit-remaining on every answer from here on.
// 3. Validation, every refusal a bare 400 {"error":"BAD_REQUEST"}: a protobuf
// body decoded as protobuf-es 2.16 decodes BittensorAnalyticsRequest
// (DiscoveryProtoReader; see decode_protobuf — unknown fields skipped, map
// entries' unknown tags not skipped, a string's leading BOM dropped, a
// struct or list value decoded and refused, `__proto__` an ordinary key);
// then zod {path: string 1..100 UTF-16 units, query: record of
// string|finite number|boolean, default {}} .strict() (another top-level key
// refused; null, arrays, objects, a missing path
// refused). 4. The facet, in order: no TAOSTATS_API_KEY → 400 detail
// "bittensor analytics not configured"; a path not in PATHS → 400
// "unsupported bittensor analytics path" (exact bytes: no trimming, case
// folding or normalization, so traversal, encoded, absolute or padded paths
// are all unknown, Object.prototype member names included); then each query entry
// in Object.entries order (array-index keys ascending, then first insertion;
// a repeated JSON key keeps its first position and last value): a key the
// path does not allow → "unsupported bittensor query"; String(value) (JS
// number formatting, true/false) over 160 UTF-16 units → "bittensor query
// value too long"; limit not /^\d+$/ or > 200 → "bittensor limit exceeds
// 200"; page > 20 → "bittensor page exceeds 20"; netuid > 65535 → "invalid
// bittensor netuid" — the first failing entry wins.
//
// Upstream. `network` is set to finney on every path that allows it (in
// place when given, else appended); /api/subnet/owner/v1 has none. The URL is
// https://api.taostats.io<path>?<URLSearchParams> with `accept:
// application/json` and the key verbatim as Authorization; nothing from the
// client (cookies, user agent, device) is forwarded. The cache key is
// taostats:<first 24 hex of sha256(path + ":" + JSON.stringify(entries
// sorted by key))> (localeCompare and byte order agree on the allowlisted
// keys), so spellings differing only in order, number vs string, a given
// finney, JSON vs protobuf share one entry. One lane run (taostats: 30/min,
// burst 30, 4 concurrent) of HttpClient.json: 5xx, 408, transport failures
// and an unparseable 2xx (HTML, empty, truncated) are retried twice (250 ms,
// 1 s), a 429 or another 4xx is not; a 429 opens the lane breaker.
//
// Cache. SwrCache.read, 15 s fresh, 300 s stale (TTL jitter at the test
// seam's 0.5 is none): fresh → served (hit, the entry's asOf); stale →
// served, then one background revalidation per key (background lane
// priority: shed while the lane is busy, then a 5 s backoff; a failed
// revalidation backs off 30 s); a miss → the provider, stored as
// JSON.stringify(JSON.parse(body)), answered {hit:false, timestamp:null}; a
// failed miss → 502 UPSTREAM_UNAVAILABLE (429 RATE_LIMITED with retry-after
// when the lane refused) and held: the same key answers the same refusal for
// 10 s (5 s when the lane was busy) without provider work. Concurrent cold
// reads of one key share one provider request (the first request's URL).
//
// Response. 200 `{"payload":<the stored JSON>,"cache":{"hit":…,
// "timestamp":<ISO>|null}}` (application/json; charset=utf-8), or with
// Accept naming application/protobuf (q > 0; the router-wide parse)
// BittensorAnalyticsResponse {payload: google.protobuf.Value, cache:
// CacheInfo} (application/protobuf). Both carry Vary: Accept; errors do not.
// Every answer carries the API baseline headers (cache-control: no-store,
// nosniff, no-referrer, permissions-policy).
//
// Effects and bounds. One `taostats:` KV entry per key (the durable KV
// keeps it across restarts; miss holds and backoffs are in memory); no other
// state. At most 16 provider reads in flight (the handoff slots, joined
// misses included): a cold read beyond them → 503 UPSTREAM_UNAVAILABLE.
// Provider bodies ≤ 512 KiB; a payload or its encoding over the 1 MiB
// workspace buffers or 32,768 JSON tokens → 502. JSON request bodies over
// 256 KiB / 8,192 tokens / depth 16 → 413 BODY_TOO_LARGE; framing refuses
// bodies over 512 KiB.
//
// Named differences (asserted by the edge replay):
//  - a payload −0 is protobuf number_value −0 in TypeScript only while its
//    in-process L1 holds the parsed value (≤ 30 s); natively always 0 (the
//    stored JSON text), as TypeScript after the L1;
//  - after a restart the durable entry answers (hit, the first process's
//    timestamp) where TypeScript's in-memory Kv reads cold;
//  - the shared bounded JSON reader refuses a lone surrogate escape and a
//    number that overflows a double (400 BAD_REQUEST after the quota);
//    TypeScript reads them (a U+FFFD in the URL, Infinity → zod);
//  - a miss that arrives after its leader's provider reply but before the
//    leader's store asks the provider again (TypeScript's flight ends
//    exactly at the store).
void handle(int client, const http::HttpRequest& request, bool head, void* opaque) {
  auto* route = static_cast<DiscoveryRouteContext*>(opaque);
  DiscoveryWorkspace* workspace = route->workspace;
  fortis::base::UniqueLock lock(workspace->mutex);
  DiscoveryBody body{};
  const int framing = ParseDiscoveryBody(request, workspace->tokens, kDiscoveryTokens, &body);
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, kCost,
                                                           route->attestation_required);
  if (policy.status) {
    lock.Unlock();
    SendDiscoveryJson(client, policy.status, policy.reason,
        policy.error_body ? policy.error_body : "{\"error\":\"INTERNAL\"}", head,
        policy.rate_limit_cost, policy.rate_limit_remaining, policy.retry_after);
    return;
  }
  if (framing) {
    lock.Unlock();
    SendDiscoveryFraming(client, framing, head, policy.rate_limit_cost, policy.rate_limit_remaining);
    return;
  }
  const int cost = policy.rate_limit_cost;
  const uint32_t remaining = policy.rate_limit_remaining;

  // Explicit constant initialization avoids a C++ local-static guard, which
  // the Unikraft link does not provide. The workspace mutex serializes use.
  static BittensorAnalyticsPlan plan{BittensorPlanVerdict::kBadRequest, nullptr, {}, 0, {}};
  PlanBittensorAnalytics(body, route->taostats_api_key_configured, workspace->payload,
                         sizeof(workspace->payload), &plan);
  if (plan.verdict != BittensorPlanVerdict::kFetch) {
    lock.Unlock();
    if (plan.verdict == BittensorPlanVerdict::kBadRequest)
      SendDiscoveryBadRequest(client, plan.detail, head, cost, remaining);
    else
      send_upstream_unavailable(client, head, cost, remaining);
    return;
  }

  // SwrCache.read
  const bool protobuf = AcceptsProtobuf(request);
  DiscoveryCache* cache = workspace->cache;
  const int64_t now = cache->Now();
  const DiscoveryCacheEntry entry = cache->Read(plan.cache_key, now);
  if (entry.state == DiscoveryCacheState::kFresh || entry.state == DiscoveryCacheState::kStale) {
    http::HttpResponse response{};
    if (BittensorEncodeResponse(workspace, entry.body, entry.body_length, true, entry.as_of_ms,
                                protobuf, &response)) {
      const bool refresh = entry.state == DiscoveryCacheState::kStale &&
          cache->AllowRevalidate(plan.cache_key, now);
      response.rate_limit_cost = cost;
      response.rate_limit_remaining = remaining;
      WriteHttpResponse(client, response, head);
      Slot* slot = refresh ? acquire(route, true, plan.cache_key, plan.provider_path) : nullptr;
      lock.Unlock();
      if (slot) {
        submit(slot);
        release(slot);
      }
      return;
    }
    // An unreadable cached value is treated as a miss.
  }
  int replay_status = 0;
  if (cache->MissReplay(plan.cache_key, now, &replay_status)) {
    lock.Unlock();
    http::HttpResponse response = replay_status % 1000 == 429
        ? DiscoveryRateLimitedResponse(static_cast<uint32_t>(replay_status / 1000) * 1000)
        : DiscoveryUpstreamUnavailableResponse();
    response.rate_limit_cost = cost;
    response.rate_limit_remaining = remaining;
    WriteHttpResponse(client, response, head);
    return;
  }
  // The slot is taken under the workspace mutex, so a concurrent miss of the
  // same key (which reads the cache under it too) always finds this flight.
  Slot* slot = acquire(route, false, plan.cache_key, plan.provider_path);
  lock.Unlock();
  if (!slot) {
    SendDiscoveryJson(client, 503, "Service Unavailable", "{\"error\":\"UPSTREAM_UNAVAILABLE\"}",
                      head, cost, remaining);
    return;
  }
  slot->protobuf = protobuf;
  slot->rate_cost = cost;
  slot->rate_remaining = remaining;
  fortis::base::UniqueLock slots_lock(slots_mutex);
  ++slot->references;  // handoff terminal
  slots_lock.Unlock();
  if (!http::ReserveHttpResponse(client, head, kDiscoveryHandoffTimeoutMs, &slot->handoff_token,
                           handoff_terminal, slot)) {
    release(slot);
    release(slot);
    SendDiscoveryJson(client, 503, "Service Unavailable", "{\"error\":\"UPSTREAM_UNAVAILABLE\"}",
                      head, cost, remaining);
    return;
  }
  if (!submit(slot)) {
    http::HttpResponse unavailable = http::JsonResponse(503, "Service Unavailable",
                                            "{\"error\":\"UPSTREAM_UNAVAILABLE\"}");
    unavailable.content_type = "application/json; charset=utf-8";
    unavailable.rate_limit_cost = cost;
    unavailable.rate_limit_remaining = remaining;
    route->handoff->Complete(slot->handoff_token, unavailable);
  }
  release(slot);  // this handler's reference
}

}  // namespace

void PlanBittensorAnalytics(const DiscoveryBody& body, bool configured, uint8_t* arena_data,
                            size_t arena_capacity, BittensorAnalyticsPlan* plan) {
  plan->verdict = BittensorPlanVerdict::kBadRequest;
  plan->detail = nullptr;
  plan->provider_path[0] = '\0';
  plan->provider_path_length = 0;
  plan->cache_key[0] = '\0';
  static Query query;
  query = Query{};
  Arena arena{arena_data, arena_capacity};
  bool valid = false;
  if (body.kind == DiscoveryBodyKind::kProtobuf)
    valid = decode_protobuf(body.bytes, body.length, &query);
  else if (body.kind == DiscoveryBodyKind::kJson)
    valid = decode_json(body.json, arena, &query);
  if (!valid || !zod_values(query)) return;

  // TaostatsProxyFacet.read
  if (!configured) { plan->detail = "bittensor analytics not configured"; return; }
  const AllowedPath* allowed = nullptr;
  for (const AllowedPath& candidate : kPaths)
    if (text_equals(query.path, query.path_length, candidate.path)) { allowed = &candidate; break; }
  if (!allowed) {
    plan->detail = "unsupported bittensor analytics path";
    return;
  }
  static uint8_t numbers[kDiscoveryMaxMembers][32];
  Param params[8]{};
  size_t param_count = 0;
  for (size_t i = 0; i < query.member_count; ++i) {
    const DiscoveryMember& member = query.members[i];
    if (!allows(*allowed, member.key, member.key_length)) {
      plan->detail = "unsupported bittensor query";
      return;
    }
    const uint8_t* text = nullptr; size_t length = 0;
    if (!member_text(member, numbers[i], sizeof(numbers[i]), &text, &length)) return;
    if (DiscoveryUtf16Length(text, length) > kMaxQueryValueUnits) {
      plan->detail = "bittensor query value too long";
      return;
    }
    if (text_equals(member.key, member.key_length, "limit") && !digits_within(text, length, 200)) {
      plan->detail = "bittensor limit exceeds 200";
      return;
    }
    if (text_equals(member.key, member.key_length, "page") && !digits_within(text, length, 20)) {
      plan->detail = "bittensor page exceeds 20";
      return;
    }
    if (text_equals(member.key, member.key_length, "netuid") &&
        !digits_within(text, length, 65535)) {
      plan->detail = "invalid bittensor netuid";
      return;
    }
    // Allowed keys are distinct, so at most seven parameters reach here.
    params[param_count++] = Param{member.key, member.key_length, text, length};
  }
  // Network-scoped resources are forced to finney; set() replaces in place.
  if (allows(*allowed, reinterpret_cast<const uint8_t*>("network"), 7)) {
    size_t i = 0;
    while (i < param_count && !text_equals(params[i].key, params[i].key_length, "network")) ++i;
    if (i == param_count) ++param_count;
    params[i] = Param{reinterpret_cast<const uint8_t*>("network"), 7,
                      reinterpret_cast<const uint8_t*>("finney"), 6};
  }

  // Upstream path, then `taostats:<sha256(path:JSON.stringify(sorted))[:24]>`.
  OutBuffer path_out{reinterpret_cast<uint8_t*>(plan->provider_path),
                     sizeof(plan->provider_path) - 1};
  path_out.Append(query.path, query.path_length);
  path_out.Byte('?');
  for (size_t i = 0; i < param_count; ++i) {
    if (i) path_out.Byte('&');
    append_form(path_out, params[i].key, params[i].key_length);
    path_out.Byte('=');
    append_form(path_out, params[i].value, params[i].value_length);
  }
  // Seven bounded parameters of the allowlisted paths always fit the
  // transport's 8 KiB path bound; 502 if not.
  plan->verdict = BittensorPlanVerdict::kUpstreamUnavailable;
  if (!path_out.ok) return;
  plan->provider_path[path_out.used] = '\0';
  plan->provider_path_length = path_out.used;
  size_t order[8];
  for (size_t i = 0; i < param_count; ++i) order[i] = i;
  for (size_t i = 1; i < param_count; ++i) {
    const size_t moving = order[i];
    size_t j = i;
    while (j > 0) {
      const Param& a = params[order[j - 1]];
      const Param& b = params[moving];
      const size_t common = a.key_length < b.key_length ? a.key_length : b.key_length;
      const int compared = memcmp(a.key, b.key, common);
      if (compared < 0 || (compared == 0 && a.key_length <= b.key_length)) break;
      order[j] = order[j - 1];
      --j;
    }
    order[j] = moving;
  }
  static uint8_t canonical[4096];
  OutBuffer digest_input{canonical, sizeof(canonical)};
  digest_input.Append(query.path, query.path_length);
  digest_input.Text(":[");
  for (size_t i = 0; i < param_count; ++i) {
    const Param& param = params[order[i]];
    if (i) digest_input.Byte(',');
    digest_input.Byte('[');
    fortis::codec::history_batch::AppendJsonString(digest_input,
        reinterpret_cast<const char*>(param.key), param.key_length);
    digest_input.Byte(',');
    fortis::codec::history_batch::AppendJsonString(digest_input,
        reinterpret_cast<const char*>(param.value), param.value_length);
    digest_input.Byte(']');
  }
  digest_input.Byte(']');
  if (!digest_input.ok) return;
  unsigned char digest[32];
  mbedtls_sha256(canonical, digest_input.used, digest, 0);
  static const char kHex[] = "0123456789abcdef";
  memcpy(plan->cache_key, "taostats:", 9);
  for (size_t i = 0; i < 12; ++i) {
    plan->cache_key[9 + i * 2] = kHex[digest[i] >> 4];
    plan->cache_key[10 + i * 2] = kHex[digest[i] & 15];
  }
  plan->cache_key[33] = '\0';
  plan->verdict = BittensorPlanVerdict::kFetch;
}

bool BittensorNormalizePayload(DiscoveryWorkspace* workspace, const unsigned char* body,
                               size_t length, size_t* payload_length) {
  if (length >= 3 && body[0] == 0xef && body[1] == 0xbb && body[2] == 0xbf) {
    body += 3; length -= 3;
  }
  codec::BoundedJsonDocument document;
  codec::BoundedJsonError error{};
  if (!length || !document.ParseWithLimits(body, length, workspace->tokens, kDiscoveryTokens,
                                           kDiscoveryTokens, codec::kBoundedJsonMaxExpandedInputBytes,
                                           &error)) return false;
  OutBuffer out{workspace->payload, sizeof(workspace->payload)};
  if (!fortis::compat::js_json::Stringify(document, document.root(), out, DiscoveryScratch(workspace)))
    return false;
  *payload_length = out.used;
  return true;
}

bool BittensorEncodeResponse(DiscoveryWorkspace* workspace, const uint8_t* payload,
                             size_t payload_length, bool hit, int64_t as_of_ms, bool protobuf,
                             http::HttpResponse* response) {
  if (!protobuf) {
    OutBuffer out{workspace->json_out, sizeof(workspace->json_out)};
    out.Text("{\"payload\":");
    out.Append(payload, payload_length);
    out.Text(",\"cache\":");
    AppendDiscoveryCacheJson(out, hit, hit ? as_of_ms : -1, false, false);
    out.Byte('}');
    if (!out.ok) return false;
    *response = http::HttpResponse{200, "OK", "application/json; charset=utf-8",
                             workspace->json_out, out.used};
    response->vary_accept = true;
    return true;
  }
  codec::BoundedJsonDocument document;
  codec::BoundedJsonError error{};
  if (!document.ParseWithLimits(payload, payload_length, workspace->tokens, kDiscoveryTokens,
                                kDiscoveryTokens, codec::kBoundedJsonMaxExpandedInputBytes, &error))
    return false;
  const auto scratch = DiscoveryScratch(workspace);
  OutBuffer measure{};
  if (!fortis::compat::js_json::AppendProtoValue(document, document.root(), measure, scratch))
    return false;
  OutBuffer out{workspace->proto_out, sizeof(workspace->proto_out)};
  fortis::codec::proto_wire::PutTag(out, 1, 2);
  fortis::codec::proto_wire::PutVarint(out, measure.used);
  if (!fortis::compat::js_json::AppendProtoValue(document, document.root(), out, scratch)) return false;
  OutBuffer cache{};
  AppendDiscoveryCacheProto(cache, hit, hit ? as_of_ms : -1, false);
  fortis::codec::proto_wire::PutTag(out, 2, 2);
  fortis::codec::proto_wire::PutVarint(out, cache.used);
  AppendDiscoveryCacheProto(out, hit, hit ? as_of_ms : -1, false);
  if (!out.ok) return false;
  *response = http::HttpResponse{200, "OK", "application/protobuf", workspace->proto_out, out.used};
  response->vary_accept = true;
  return true;
}

bool RegisterBittensorAnalyticsRoute(http::HttpRouter& router, DiscoveryRouteContext* context) {
  return context && context->owner && context->workspace &&
         context->handoff && router.Add("POST", "/v1/bittensor/analytics", handle, context);
}

}  // namespace fortis::discovery
