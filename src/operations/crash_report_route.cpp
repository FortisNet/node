#include "ownership.h"
#include "crash_report_route.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bounded_json.h"
#include "crash_report_codec.h"
#include "device_auth.h"
#include "device_enrollment.h"
#include "diagnostic_redaction.h"
#include "kv_runtime.h"

namespace fortis::operations {

namespace cr = fortis::codec::crash_report;
using fortis::codec::history_batch::OutBuffer;

namespace {

constexpr char kJsonType[] = "application/json; charset=utf-8";
constexpr char kKeyPrefix[] = "crash/report/";
// The native request bound is 512 KiB.
constexpr size_t kMaxJsonBody = 512 * 1024;

int64_t wall_clock_ms() {
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) return 0;
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

struct Quota {
  int cost = -1;
  uint32_t remaining = 0;
};

void send(int client, int status, const char* reason, const char* content_type,
          const uint8_t* body, size_t length, const Quota& quota, bool vary = false) {
  http::HttpResponse response{status, reason, content_type, body, length};
  response.rate_limit_cost = quota.cost;
  response.rate_limit_remaining = quota.remaining;
  response.vary_accept = vary;
  WriteHttpResponse(client, response);
}

void send_json(int client, int status, const char* reason, const char* json,
               const Quota& quota = Quota{}) {
  send(client, status, reason, kJsonType, reinterpret_cast<const uint8_t*>(json),
       strlen(json), quota);
}

// Per-request scratch, released on every exit path.
struct Scratch {
  std::unique_ptr<codec::BoundedJsonToken[], base::StorageDeleter> tokens;
  std::unique_ptr<char[], base::StorageDeleter> arena;
  std::unique_ptr<cr::Slice[], base::StorageDeleter> keys;
  std::unique_ptr<uint8_t[], base::StorageDeleter> detail;
  std::unique_ptr<char[], base::StorageDeleter> error_type;
  std::unique_ptr<char[], base::StorageDeleter> message;
  std::unique_ptr<char[], base::StorageDeleter> stack;
  std::unique_ptr<uint8_t[], base::StorageDeleter> record;
  ~Scratch() = default;
};

enum class BodyKind : uint8_t { kUndefined, kJson, kProtobuf };

bool redact(cr::Slice input, std::unique_ptr<char[], base::StorageDeleter>& output, cr::Slice* redacted) {
  size_t length = 0;
  const bool ok = fortis::security::redaction::RedactDiagnosticText(input.data, input.length, &output, &length);
  if (!ok) return false;
  *redacted = {output.get(), length};
  return true;
}

void handle_crash_report(int client, const http::HttpRequest& request, bool, void* opaque) {
  const auto* context = static_cast<const CrashReportRouteContext*>(opaque);
  if (!context) { send_json(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}"); return; }
  Scratch scratch;
  codec::BoundedJsonDocument json;
  // 1. preHandler: Origin, device bearer, quota charged at cost 5.
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(
      request, cr::kQuotaCost, context->attestation_required);
  if (policy.status) {
    const char* body = policy.error_body ? policy.error_body : "{\"error\":\"INTERNAL\"}";
    http::HttpResponse response{policy.status, policy.reason, kJsonType,
                          reinterpret_cast<const uint8_t*>(body), strlen(body)};
    response.rate_limit_cost = policy.rate_limit_cost;
    response.rate_limit_remaining = policy.rate_limit_remaining;
    response.retry_after = policy.retry_after;
    WriteHttpResponse(client, response);
    return;
  }
  const Quota quota{policy.rate_limit_cost, policy.rate_limit_remaining};

  // 2. Content-type parsing (auth-first: after the device policy, charged).
  BodyKind kind = BodyKind::kUndefined;
  if (!request.has_content_type) {
    if (request.body_length) {
      send_json(client, 415, "Unsupported Media Type", "{\"error\":\"UNSUPPORTED_MEDIA_TYPE\"}", quota);
      return;
    }
  } else if (request.json_content_type) {
    if (!request.body_length) {
      send_json(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", quota);
      return;
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(request.body);
    size_t length = request.body_length;
    if (length >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf) {
      bytes += 3; length -= 3;  // one byte-order mark is dropped
    }
    scratch.tokens = base::AllocateStorage<codec::BoundedJsonToken[]>(codec::kBoundedJsonMaxTokens * sizeof(codec::BoundedJsonToken));
    if (!scratch.tokens) {
      send_json(client, 503, "Service Unavailable", "{\"error\":\"UNAVAILABLE\"}", quota);
      return;
    }
    codec::BoundedJsonError error = codec::BoundedJsonError::kNone;
    if (!json.ParseWithLimits(bytes, length, scratch.tokens.get(), codec::kBoundedJsonMaxTokens,
                              codec::kBoundedJsonMaxTokens, kMaxJsonBody, &error)) {
      if (error == codec::BoundedJsonError::kInvalidInput)
        send_json(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", quota);
      // Native bound (8,192 tokens, depth 16).
      else send_json(client, 413, "Payload Too Large", "{\"error\":\"BODY_TOO_LARGE\"}", quota);
      return;
    }
    kind = BodyKind::kJson;
  } else if (request.protobuf_content_type) {
    kind = BodyKind::kProtobuf;
  } else {
    send_json(client, 415, "Unsupported Media Type", "{\"error\":\"UNSUPPORTED_MEDIA_TYPE\"}", quota);
    return;
  }

  // 3. Handler: protobuf decode, then Zod.
  cr::RawReport raw{};
  if (kind == BodyKind::kProtobuf) {
    if (!cr::DecodeProtobuf(reinterpret_cast<const uint8_t*>(request.body),
                            request.body_length, &raw)) {
      send_json(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", quota);
      return;
    }
  } else if (kind == BodyKind::kJson) {
    const size_t arena_size = request.body_length + codec::kBoundedJsonMaxTokens + 1;
    scratch.arena = base::AllocateStorage<char[]>(arena_size);
    scratch.keys = base::AllocateStorage<cr::Slice[]>(codec::kBoundedJsonMaxTokens / 2 * sizeof(cr::Slice));
    raw.unrecognized = scratch.keys.get();
    raw.unrecognized_capacity = codec::kBoundedJsonMaxTokens / 2;
    if (!scratch.arena || !scratch.keys ||
        !cr::ReadJson(json, scratch.arena.get(), arena_size, &raw)) {
      send_json(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", quota);
      return;
    }
  } else {
    cr::ReadScalarRoot(cr::ValueKind::kMissing, &raw);
  }
  cr::ValidReport report{};
  OutBuffer counter{};
  if (!cr::Validate(raw, &report, counter)) {
    scratch.detail = base::AllocateStorage<uint8_t[]>(counter.used);
    OutBuffer detail{scratch.detail.get(), counter.used};
    if (!scratch.detail || cr::Validate(raw, &report, detail) || !detail.ok) {
      send_json(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", quota);
      return;
    }
    send(client, 400, "Bad Request", kJsonType, scratch.detail.get(), detail.used, quota);
    return;
  }

  // 4. CrashReportsFacet.submit: redact, fingerprint, store with expiry.
  cr::StoredReport stored{};
  stored.report = &report;
  if (!redact(report.error_type, scratch.error_type, &stored.error_type) ||
      !redact(report.message, scratch.message, &stored.message) ||
      !redact(report.stack, scratch.stack, &stored.stack)) {
    send_json(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", quota);
    return;
  }
  char fingerprint[65], id[cr::kUlidLength + 1];
  uint8_t random[10];
  const int64_t now = context->clock ? context->clock->NowMs() : wall_clock_ms();
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
  if (!kv || !cr::Fingerprint(stored.error_type, stored.message, stored.stack, fingerprint) ||
      !fortis::auth::SecureRandomBytes(random, sizeof(random)) || !cr::NewUlid(now, random, id)) {
    send_json(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", quota);
    return;
  }
  stored.id = id;
  stored.device_id = policy.device.id;
  stored.fingerprint = fingerprint;
  stored.created_at_ms = now;
  OutBuffer measure{};
  cr::AppendStoredJson(stored, measure);
  scratch.record = base::AllocateStorage<uint8_t[]>(measure.used ? measure.used : 1);
  OutBuffer record{scratch.record.get(), measure.used};
  char key[sizeof(kKeyPrefix) + cr::kUlidLength];
  snprintf(key, sizeof(key), "%s%s", kKeyPrefix, id);
  const fortis::storage::KvMutation write{key, scratch.record.get(), static_cast<uint32_t>(measure.used),
                                 now + cr::kRetentionMs, false};
  if (!scratch.record || !measure.ok || !cr::AppendStoredJson(stored, record) ||
      record.used != measure.used || !kv->Write(&write, 1)) {
    send_json(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", quota);
    return;
  }

  // 5. 202 { accepted, id } in the representation Accept selects.
  if (AcceptsProtobuf(request)) {
    uint8_t wire[64];
    const size_t length = cr::EncodeResponseProtobuf(id, wire, sizeof(wire));
    send(client, 202, "Accepted", "application/protobuf", wire, length, quota, true);
    return;
  }
  char body[64];
  const int length = snprintf(body, sizeof(body), "{\"accepted\":true,\"id\":\"%s\"}", id);
  send(client, 202, "Accepted", kJsonType, reinterpret_cast<const uint8_t*>(body),
       static_cast<size_t>(length), quota, true);
}

bool valid_id(const char* id) {
  if (!id || strlen(id) != cr::kUlidLength) return false;
  for (const char* p = id; *p; ++p)
    if (!strchr("0123456789ABCDEFGHJKMNPQRSTVWXYZ", *p)) return false;
  return true;
}

}  // namespace

bool RegisterCrashReportRoute(http::HttpRouter& router, CrashReportRouteContext* context) {
  return context &&
         router.Add("POST", "/v1/crash-reports", handle_crash_report, context);
}

bool FindCrashReport(fortis::storage::KvStore& kv, const char* id, int64_t now_ms, char* output,
                     size_t capacity, size_t* length) {
  if (!valid_id(id) || !output || !length) return false;
  char key[sizeof(kKeyPrefix) + cr::kUlidLength];
  snprintf(key, sizeof(key), "%s%s", kKeyPrefix, id);
  return kv.Get(key, now_ms, output, capacity, length);
}

}  // namespace fortis::operations
