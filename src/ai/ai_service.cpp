#include "provider_diagnostics.h"
#include "exception_boundary.h"
#include "ai_service.h"
#include "conversation_components_internal.h"
#include "utf8.h"
#include "json_lex.h"
#include "ownership.h"
#include "query_string.h"

#include "ai_proof.h"
#include "ai_reply.h"
#include "ai_snapshot.h"
#include "ai_store.h"
#include "device_enrollment.h"
#include "hourly_windows.h"
#include "kv_store.h"
#include "outbound_https.h"
#include "price_history_batch_codec.h"
#include "proto_wire.h"
#include "threading.h"
#include "token_json.h"
#include "token_text.h"

#include <mbedtls/platform_util.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::ai {

namespace {

namespace tj = fortis::codec::tj;
namespace pw = fortis::codec::proto_wire;
namespace hb = fortis::codec::history_batch;
using tj::OutBuffer;
using tj::Value;

constexpr const char* kJsonType = "application/json; charset=utf-8";
constexpr const char* kProtobufType = "application/protobuf";
constexpr size_t kProviderResponseBytes = 2 * 1024 * 1024;
constexpr size_t kResponseLimit = 2 * 1024 * 1024;  // http_server.cpp kCapturedResponseLimit
constexpr int64_t kQuotaWindowMs = 60 * 60 * 1000;
constexpr uint64_t kOpenQuota = 20;
constexpr uint64_t kMessageQuota = 120;
constexpr size_t kRequestArenaBytes = 16 * 1024 * 1024;
constexpr size_t kWorkArenaBytes = 64 * 1024 * 1024;

uint64_t monotonic_ms() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1000000;
}

// ---- replies --------------------------------------------------------------------------



http::HttpResponse json_response(int status, const char* reason, const char* body, const Policy& policy) {
  http::HttpResponse response{status, reason, kJsonType, reinterpret_cast<const uint8_t*>(body), strlen(body)};
  response.rate_limit_cost = policy.cost;
  response.rate_limit_remaining = policy.remaining;
  return response;
}

void send(int client, bool head, int status, const char* reason, const char* body, const Policy& policy,
          unsigned retry_after = 0) {
  http::HttpResponse response = json_response(status, reason, body, policy);
  response.retry_after = retry_after;
  WriteHttpResponse(client, response, head);
}

void send_bad_request(int client, bool head, const Policy& policy) {
  send(client, head, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", policy);
}
void send_not_found(int client, bool head, const Policy& policy) {
  send(client, head, 404, "Not Found", "{\"error\":\"NOT_FOUND\"}", policy);
}
void send_internal(int client, bool head, const Policy& policy) {
  send(client, head, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", policy);
}

bool reply_policy(int client, const auth::DevicePolicyResult& policy, bool head) {
  if (!policy.status) return false;
  const char* body = policy.error_body ? policy.error_body : "{\"error\":\"INTERNAL\"}";
  http::HttpResponse response{policy.status, policy.reason, kJsonType, reinterpret_cast<const uint8_t*>(body),
                        strlen(body)};
  response.rate_limit_cost = policy.rate_limit_cost;
  response.rate_limit_remaining = policy.rate_limit_remaining;
  response.retry_after = policy.retry_after;
  WriteHttpResponse(client, response, head);
  return true;
}

// An id over 100 UTF-16 units once decoded: 414 URI_TOO_LONG before any hook.
bool reject_long_id(int client, const http::HttpRequest& request, bool head) {
  const char* value = HttpPathParamValue(request, "id");
  if (!value || hb::Utf16Units(value, strlen(value)) <= 100) return false;
  send(client, head, 414, "URI Too Long", "{\"error\":\"URI_TOO_LONG\"}", Policy{});
  return true;
}

// The no-query read remains a 100-message tail for older clients. Mobile
// history reads request 20 rows and use an exclusive cursor.
bool message_page_query(const char* target, char before[ai::kIdBytes], size_t* limit) {
  before[0] = '\0';
  *limit = ai::kMaxHistory;
  const auto query = http::query_string::Query::FromTarget(target);
  if (!query.Valid()) return false;
  http::query_string::Component value;
  if (query.First("before", &value)) {
    if (value.Decode(before, ai::kIdBytes - 1) != ai::kIdBytes - 1) return false;
    before[ai::kIdBytes - 1] = '\0';
    if (!ai::IsConversationId(before)) return false;
    *limit = 20;
  }
  if (query.First("limit", &value)) {
    uint64_t parsed = 0;
    if (!value.Unsigned(ai::kMaxHistory, &parsed) || !parsed) return false;
    *limit = static_cast<size_t>(parsed);
  }
  return true;
}

// ---- request bodies (content-type parsing, after the device hook) ---------------------

enum class BodyKind : uint8_t { kUndefined, kJson, kProtobuf };
struct Body {
  BodyKind kind = BodyKind::kUndefined;
  const Value* json = nullptr;
  const uint8_t* bytes = nullptr;
  size_t length = 0;
};

// The body parser, after the device policy (auth-first; its refusals carry
// the quota headers): the UTF-8 text of a JSON body (bounded by the 512 KiB
// request limit), one leading byte order mark dropped, JSON.parse
// (`__proto__` is an ordinary key).
bool parse_body(int client, const http::HttpRequest& request, const Policy& policy, tj::Arena& arena, Body* body) {
  body->bytes = reinterpret_cast<const uint8_t*>(request.body);
  body->length = request.body_length;
  if (!request.has_content_type) {
    if (request.body_length) {
      send(client, false, 415, "Unsupported Media Type", "{\"error\":\"UNSUPPORTED_MEDIA_TYPE\"}", policy);
      return false;
    }
    body->kind = BodyKind::kUndefined;
    return true;
  }
  if (request.json_content_type) {
    body->json = request.body_length ? tj::Parse(arena, body->bytes, body->length) : nullptr;
    if (!body->json) {
      send_bad_request(client, false, policy);
      return false;
    }
    body->kind = BodyKind::kJson;
    return true;
  }
  if (request.protobuf_content_type) { body->kind = BodyKind::kProtobuf; return true; }
  send(client, false, 415, "Unsupported Media Type", "{\"error\":\"UNSUPPORTED_MEDIA_TYPE\"}", policy);
  return false;
}

// ---- request schemas -------------------------------------------------------------------

struct Field {
  const char* text = nullptr;  // NUL-terminated (WTF-8)
  size_t length = 0;
  bool present = false;
};

bool hex_string(const Field& f, size_t digits, bool upper_allowed) {
  if (!f.present || f.length != digits) return false;
  for (size_t i = 0; i < digits; ++i) {
    const char c = f.text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (upper_allowed && c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}
// /^0x[0-9a-fA-F]{n}$/
bool prefixed_hex(const Field& f, size_t digits) {
  if (!f.present || f.length != digits + 2 || f.text[0] != '0' || f.text[1] != 'x') return false;
  Field rest{f.text + 2, digits, true};
  return hex_string(rest, digits, true);
}
bool chat_token(const Field& f) {
  static const char kPrefix[] = "fintel_chat_";
  if (!f.present || f.length != sizeof(kPrefix) - 1 + 43 || memcmp(f.text, kPrefix, sizeof(kPrefix) - 1) != 0)
    return false;
  for (size_t i = sizeof(kPrefix) - 1; i < f.length; ++i) {
    const char c = f.text[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
      return false;
  }
  return true;
}
bool snapshot_text(const Field& f) {
  if (!f.present) return false;
  const size_t units = hb::Utf16Units(f.text, f.length);
  return units >= 2 && units <= ai::kSnapshotJsonMaxUnits;
}
// .trim().min(1).max(8_000): the field becomes its trimmed value.
bool trimmed_body(Field* f) {
  if (!f->present) return false;
  size_t begin = 0, end = 0;
  if (!ai::ZodTrimmedBody(f->text, f->length, &begin, &end)) return false;
  f->text += begin;
  f->length = end - begin;
  return true;
}

// JSON members of a strict object, by name; false for a non-string value or
// an unknown key.
bool json_fields(const Value* object, const char* const* names, size_t count, Field* fields) {
  if (!tj::IsObject(object)) return false;
  for (uint32_t i = 0; i < object->count; ++i) {
    const tj::Member& member = object->members[i];
    size_t k = 0;
    while (k < count && !(strlen(names[k]) == member.key_length && memcmp(names[k], member.key, member.key_length) == 0))
      ++k;
    if (k == count || !tj::IsString(member.value)) return false;
    fields[k] = {member.value->text, member.value->length, true};
  }
  return true;
}

// protobuf-es fromBinary of a message whose fields 1..count are strings:
// read by their declared type whatever the wire type, invalid UTF-8 and an
// end-group tag throw, unknown fields are skipped, the last value wins.
bool proto_fields(const uint8_t* body, size_t length, tj::Arena& arena, Field* fields, size_t count) {
  size_t position = 0;
  while (position < length) {
    uint32_t field = 0, wire = 0;
    if (!fortis::codec::proto_wire::ReadTag(body, length, &position, &field, &wire) || wire == 4) return false;
    if (field >= 1 && field <= count) {
      if (wire != 2) return false;
      const uint8_t* data = nullptr;
      size_t size = 0;
      if (!fortis::codec::proto_wire::ReadBytes(body, length, &position, &data, &size) || !hb::ValidUtf8(data, size)) return false;
      char* copy = arena.CopyText(reinterpret_cast<const char*>(data), size);
      if (!copy) return false;
      fields[field - 1] = {copy, size, true};
    } else if (!fortis::codec::proto_wire::SkipField(body, length, &position, wire, field)) {
      return false;
    }
  }
  return true;
}

// A copy with each lone surrogate as U+FFFD: Node's UTF-8 encoder (hashing)
// writes it so, and the native secret scanner reads only valid UTF-8.
Field well_formed(const Field& f, tj::Arena& arena) {
  const size_t size = codec::utf8::Normalize(reinterpret_cast<const uint8_t*>(f.text), f.length, nullptr, 0);
  char* out = static_cast<char*>(arena.Alloc(size + 1, 1));
  if (!out) return Field{};
  codec::utf8::Normalize(reinterpret_cast<const uint8_t*>(f.text), f.length, out, size);
  out[size] = '\0';
  return Field{out, size, f.present};
}

enum OpenField { kAddress, kNonce, kTimestamp, kSnapshotHash, kAccessToken, kAccessTokenHash, kSnapshotJson,
                 kSignature, kFirstMessage, kClientMessageId, kOpenFieldCount };
const char* const kOpenNames[kOpenFieldCount] = {"address", "nonce", "timestamp", "snapshotHash", "accessToken",
                                                 "accessTokenHash", "snapshotJson", "signature", "firstMessage",
                                                 "clientMessageId"};
// openSchema (strict).
bool open_schema(Field* f) {
  for (size_t i = 0; i < kOpenFieldCount; ++i)
    if (!f[i].present) return false;
  return prefixed_hex(f[kAddress], 40) && hex_string(f[kNonce], 32, false) &&
         ai::IsZodDatetime(f[kTimestamp].text, f[kTimestamp].length) && hex_string(f[kSnapshotHash], 64, false) &&
         chat_token(f[kAccessToken]) && hex_string(f[kAccessTokenHash], 64, false) && snapshot_text(f[kSnapshotJson]) &&
         prefixed_hex(f[kSignature], 130) && trimmed_body(&f[kFirstMessage]) && hex_string(f[kClientMessageId], 32, false);
}

enum MessageField { kMessageClientId, kMessageBody, kMessageSnapshotJson, kMessageSnapshotHash, kMessageFieldCount };
const char* const kMessageNames[kMessageFieldCount] = {"clientMessageId", "body", "snapshotJson", "snapshotHash"};
// messageSchema (strict; the snapshot pair optional).
bool message_schema(Field* f) {
  return hex_string(f[kMessageClientId], 32, false) && trimmed_body(&f[kMessageBody]) &&
         (!f[kMessageSnapshotJson].present || snapshot_text(f[kMessageSnapshotJson])) &&
         (!f[kMessageSnapshotHash].present || hex_string(f[kMessageSnapshotHash], 64, false));
}

// /^[0-9A-HJKMNP-TV-Z]{26}$/
bool conversation_id(const char* id) {
  if (!id || strlen(id) != 26) return false;
  for (size_t i = 0; i < 26; ++i) {
    const char c = id[i];
    const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z' && c != 'I' && c != 'L' && c != 'O' && c != 'U');
    if (!ok) return false;
  }
  return true;
}

// ---- response encoding --------------------------------------------------------------



bool stringify(const Value* value, Encoded* out) {
  OutBuffer counter{};
  if (!tj::Stringify(value, counter) || !counter.ok) return false;
  out->data = base::AllocateStorage<uint8_t[]>(counter.used + 1);
  if (!out->data) return false;
  OutBuffer buffer{out->data.get(), counter.used + 1};
  if (!tj::Stringify(value, buffer) || !buffer.ok) return false;
  out->length = buffer.used;
  return true;
}

// A proto3 string field, with valid Unicode scalar values on the wire.
size_t string_size(size_t length) { return length ? 1 + pw::VarintSize(length) + length : 0; }
void string_field(OutBuffer& out, uint8_t tag, const char* text, size_t length) {
  if (!length) return;
  out.Byte(tag);
  const auto* bytes = reinterpret_cast<const uint8_t*>(text);
  const size_t normalized = codec::utf8::Normalize(bytes, length, nullptr, 0);
  pw::PutVarint(out, normalized);
  for (size_t at=0; at<length;) {
    uint8_t encoded[4];
    const uint32_t cp = codec::utf8::Next(bytes, length, &at);
    out.Append(encoded, codec::json_lex::EncodeUtf8(cp, encoded));
  }
}

struct Wire {
  const char* id;
  const char* role;
  const char* body;
  size_t body_length;
  char created_at[32];
};
Wire wire_of(const ai::MessageView& row, const char* role) {
  Wire w{row.id, role, row.body, row.body_length, {}};
  fortis::compat::token_text::IsoString(row.created_at_ms, w.created_at);
  return w;
}
size_t message_size(const Wire& w) {
  return string_size(strlen(w.id)) + string_size(strlen(w.role)) + string_size(codec::utf8::Normalize(reinterpret_cast<const uint8_t*>(w.body), w.body_length, nullptr, 0)) +
         string_size(strlen(w.created_at));
}
void message_fields(OutBuffer& out, const Wire& w) {
  string_field(out, 0x0a, w.id, strlen(w.id));
  string_field(out, 0x12, w.role, strlen(w.role));
  string_field(out, 0x1a, w.body, w.body_length);
  string_field(out, 0x22, w.created_at, strlen(w.created_at));
}
void message_submessage(OutBuffer& out, uint8_t tag, const Wire& w) {
  out.Byte(tag);
  pw::PutVarint(out, message_size(w));
  message_fields(out, w);
}

// {id, role, body, createdAt} (role omitted for the open reply's user turn).
Value* message_json(tj::Arena& a, const Wire& w, bool with_role) {
  Value* o = tj::Obj(a);
  if (!o || !tj::Set(a, o, "id", tj::Str(a, w.id)) || (with_role && !tj::Set(a, o, "role", tj::Str(a, w.role))) ||
      !tj::Set(a, o, "body", tj::Str(a, w.body, w.body_length)) ||
      !tj::Set(a, o, "createdAt", tj::Str(a, w.created_at)))
    return nullptr;
  return o;
}

// The hourly per-device quotas (kv.incrWindow): see hourly_windows.h.


// The turn a request answers.


}  // namespace

}  // namespace fortis::ai

// ---- GET .../messages: id, bearer and body (ai_service.h) ----------------------------

namespace fortis::ai {

bool IsConversationId(const char* id) { return conversation_id(id); }

const char* ChatBearerToken(const char* header) {
  if (!header || strncmp(header, "Bearer ", 7) != 0) return "";
  return header + 7;
}

bool EncodeMessagesBody(const MessageView* rows, size_t count, bool protobuf, size_t limit, std::unique_ptr<uint8_t[], base::StorageDeleter>* body,
                        size_t* length, size_t* first) {
  // Input views may point into the previous output; keep it alive while reading.
  const auto previous_output = std::move(*body);
  *length = 0;
  *first = count;
  tj::Arena arena = tj::GrowingArena(kWorkArenaBytes);
  std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
  const size_t slots = count ? count : 1;
  Wire* wires = static_cast<Wire*>(arena.Alloc(sizeof(Wire) * slots));
  size_t* sizes = static_cast<size_t*>(arena.Alloc(sizeof(size_t) * slots));
  Value** json = static_cast<Value**>(arena.Alloc(sizeof(Value*) * slots));
  if (!wires || !sizes || !json) return false;
  // Each row's exact encoded size (a protobuf row includes its tag and
  // length prefix); JSON rows are joined by one comma.
  size_t total = protobuf ? 0 : 15;  // {"messages":[]}
  for (size_t i = 0; i < count; ++i) {
    wires[i] = wire_of(rows[i], rows[i].role);
    if (protobuf) {
      const size_t inner = message_size(wires[i]);
      sizes[i] = 1 + pw::VarintSize(inner) + inner;
    } else {
      json[i] = message_json(arena, wires[i], true);
      OutBuffer counter{};
      if (!json[i] || !tj::Stringify(json[i], counter) || !counter.ok) return false;
      sizes[i] = counter.used;
    }
    total += sizes[i] + (!protobuf && i ? 1 : 0);
  }
  // The newest rows that fit: drop from the oldest end.
  size_t kept = 0;
  while (total > limit && kept < count) {
    total -= sizes[kept];
    ++kept;
    if (!protobuf && kept < count) total -= 1;  // one comma fewer
  }
  auto data_storage = base::AllocateStorage<uint8_t[]>(total ? total : 1);
  auto* data = data_storage.get();
  if (!data) return false;
  OutBuffer out{data, total ? total : 1};
  if (protobuf) {
    for (size_t i = kept; i < count; ++i) message_submessage(out, 0x0a, wires[i]);
  } else {
    out.Text("{\"messages\":[");
    for (size_t i = kept; i < count && out.ok; ++i) {
      if (i > kept) out.Byte(',');
      if (!tj::Stringify(json[i], out)) out.ok = false;
    }
    out.Text("]}");
  }
  if (!out.ok || out.used != total) {
    data_storage.reset();
    return false;
  }
  *body = std::move(data_storage);
  *length = total;
  *first = kept;
  return true;
}

}  // namespace fortis::ai

// ---- the service ----------------------------------------------------------------------

namespace fortis::ai {

void ConversationRoutes::HandleStats(int client, const http::HttpRequest&, bool head, void* opaque) {
    auto* self = static_cast<Impl*>(opaque);
    char page[16 * 1024];
    const size_t length = self->kv ? ai::RenderAiStatsPage(*self->kv, self->Now(), page, sizeof(page)) : 0;
    if (!length) {
      WriteHttpResponse(client, http::JsonResponse(503, "Service Unavailable", "{\"error\":\"STATS_UNAVAILABLE\"}"), head);
      return;
    }
    http::HttpResponse response{200, "OK", "text/html; charset=utf-8",
                          reinterpret_cast<const uint8_t*>(page), length};
    response.cache_control = "no-store";
    WriteHttpResponse(client, response, head);
  }

bool ReplyGenerator::ParseNumber(const char* text, int minimum, int maximum, int* out) {
    if (!text || !*text) return false;
    int value = 0;
    for (const char* p = text; *p; ++p) {
      if (*p < '0' || *p > '9' || value > maximum) return false;
      value = value * 10 + (*p - '0');
    }
    if (value < minimum || value > maximum) return false;
    *out = value;
    return true;
  }

bool ReplyGenerator::ConfigureAnthropic() {
    auto& config = root_.config;
    auto& have_seams = root_.have_seams;
    if (!config.ai_provider_configured && !config.ai_model_configured &&
        !config.ai_effort_configured && !config.ai_max_output_tokens_configured &&
        !config.ai_timeout_ms_configured && !config.ai_concurrency_configured &&
        !config.anthropic_api_key_path_configured) return true;
    if (!config.ai_provider_configured || strcmp(config.ai_provider, "anthropic") != 0 || !config.ai_model_configured ||
        strncmp(config.ai_model, "claude-opus-", 12) != 0 || !config.ai_effort_configured ||
        (strcmp(config.ai_effort, "low") != 0 && strcmp(config.ai_effort, "medium") != 0 &&
         strcmp(config.ai_effort, "high") != 0 && strcmp(config.ai_effort, "max") != 0) ||
        !config.ai_max_output_tokens_configured || !config.ai_timeout_ms_configured ||
        !config.ai_concurrency_configured || !config.anthropic_api_key_path_configured ||
        (!have_seams && strncmp(config.anthropic_api_key_path, "/data/", 6) != 0))
      return false;
    int tokens = 0, timeout = 0, concurrency = 0;
    if (!ParseNumber(config.ai_max_output_tokens, 256, 8192, &tokens) ||
        !ParseNumber(config.ai_timeout_ms, 1000, 120000, &timeout) ||
        !ParseNumber(config.ai_concurrency, 1, static_cast<int>(AiConversationService::kWorkers), &concurrency)) return false;
    struct stat info{};
    if (stat(config.anthropic_api_key_path, &info) != 0 || !S_ISREG(info.st_mode) ||
        (info.st_mode & 077) != 0 || info.st_uid != geteuid() || info.st_size < 10 || info.st_size > 255) return false;
    FILE* file = fopen(config.anthropic_api_key_path, "rb");
    if (!file) return false;
    const size_t size = fread(anthropic_key, 1, sizeof(anthropic_key) - 1, file);
    const bool read_ok = !ferror(file);
    const bool close_ok = fclose(file) == 0;
    const bool good = read_ok && close_ok && size >= 10 && size <= 255 &&
                      memchr(anthropic_key, '\0', size) == nullptr;
    if (!good) { memset(anthropic_key, 0, sizeof(anthropic_key)); return false; }
    size_t key_length = strlen(anthropic_key);
    if (key_length && anthropic_key[key_length - 1] == '\n')
      anthropic_key[key_length - 1] = '\0';
    if (!*anthropic_key || strpbrk(anthropic_key, " \t\r\n")) {
      memset(anthropic_key, 0, sizeof(anthropic_key));
      return false;
    }
    ai_max_tokens = tokens;
    ai_timeout_ms = static_cast<uint32_t>(timeout);
    ai_concurrency = static_cast<size_t>(concurrency);
    anthropic_ready = true;
    return true;
  }

void ReplyGenerator::GenerateReply(const Value* snapshot, const Turn* turns, size_t turn_count,
    tj::Arena& arena, GeneratedReply* output) {
    auto& config = root_.config;
    auto& seams = root_.seams;
    auto& have_seams = root_.have_seams;
    *output = GeneratedReply{};
    const uint8_t* body = nullptr;
    size_t length = 0;
    if (!ai::BuildAnthropicRequest(config.ai_model, config.ai_effort, ai_max_tokens,
                                   snapshot, turns, turn_count, arena, &body, &length)) {
      output->failure = "failed";
      return;
    }
    http::OutboundRequest request{};
    request.method = http::OutboundMethod::kPostJson;
    request.host = ai::kAnthropicHost;
    request.path = ai::kAnthropicPath;
    request.ca_file = config.ca_bundle_path;
    request.body = body;
    request.body_length = length;
    request.timeout_ms = ai_timeout_ms;
    request.max_response_bytes = kProviderResponseBytes;
    request.max_request_bytes = http::kOutboundMaxRequestBytes;
    snprintf(request.headers[0].name, sizeof(request.headers[0].name), "x-api-key");
    snprintf(request.headers[0].value, sizeof(request.headers[0].value), "%s", anthropic_key);
    snprintf(request.headers[1].name, sizeof(request.headers[1].name), "anthropic-version");
    snprintf(request.headers[1].value, sizeof(request.headers[1].value), "2023-06-01");
    request.header_count = 2;
    const auto wipe_key = [](http::OutboundRequest* value) noexcept {
      mbedtls_platform_zeroize(value->headers[0].value, sizeof(value->headers[0].value));
    };
    // Borrow the stack request solely to wipe its credential on every exit.
    const std::unique_ptr<http::OutboundRequest, decltype(wipe_key)> key_guard(&request, wipe_key);
    if (have_seams && seams.upstream_host) {
      request.host = seams.upstream_host;
      request.port = seams.upstream_port;
      request.ca_file = seams.upstream_ca;
      request.allow_address = seams.allow_address;
      request.policy_context = seams.context;
    }
    auto response_storage = base::AllocateStorage<uint8_t[]>(kProviderResponseBytes);
  auto* response = response_storage.get();
    if (!response) { output->failure = "failed"; return; }
    const http::OutboundResponse answer = (have_seams && seams.transport ? seams.transport->Execute(request, response, kProviderResponseBytes) : OutboundHttpsRequest(request, response, kProviderResponseBytes));
    mbedtls_platform_zeroize(request.headers[0].value, sizeof(request.headers[0].value));
    const char* text = nullptr;
    size_t text_length = 0;
    ai::TokenUsage usage{};
    const bool parsed = answer.error == http::OutboundError::kNone &&
        ai::ParseAnthropicReply(answer.status, response, answer.body_length, arena, &text, &text_length, &usage);
    if (!parsed) {
      providers::ProviderExecutionResult observed{};
      observed.attempts = 1;
      observed.response = answer;
      const auto failure = providers::DescribeProviderFailure("anthropic", observed);
      output->failure_details = failure.details;
      if (!failure.code) output->failure_details.origin = base::ErrorOrigin::kUnclassified;
      const char* status = answer.error == http::OutboundError::kNone
          ? ai::ClassifyAnthropicFailure(answer.status, response, answer.body_length, arena)
          : "unavailable";
      response_storage.reset();
      output->failure = answer.status == 200 && strcmp(status, "failed") == 0 ? "invalid_reply" : status;
      return;
    }
    response_storage.reset();
    if (ai::OutboundSuppressed(text, text_length)) {
      text = ai::kSafeReply;
      text_length = strlen(ai::kSafeReply);
    }
    output->text = text;
    output->length = text_length;
    output->usage = usage;
}

void ConversationService::GenerateJob(const char* id, const char* user_message) {
    auto& store = root_.store;
    char reply_id[ai::kIdBytes];
    if (store.FindReply(id, user_message, reply_id)) return;
    if (!root_.generator.anthropic_ready) { FailJob(id, user_message, "unavailable", "provider_not_configured"); return; }
    tj::Arena arena = tj::GrowingArena(kWorkArenaBytes);
    std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
    const Value* snapshot = nullptr;
    ai::Turn* turns = nullptr;
    size_t turn_count = 0;
    if (!store.LoadReplyContext(id, user_message, arena, &snapshot, &turns, &turn_count)) {
      FailJob(id, user_message, "failed", "reply_context_unavailable");
      return;
    }
    GeneratedReply reply;
    root_.generator.GenerateReply(snapshot, turns, turn_count, arena, &reply);
    if (reply.failure) { FailJob(id, user_message, reply.failure, reply.failure, reply.failure_details); return; }
    bool created = false;
    if (!store.SaveReply(id, user_message, reply.text, reply.length, reply_id, &created, reply.usage))
      FailJob(id, user_message, "failed", "reply_write_failed");
}


void ConversationService::FailJob(const char* id, const char* message,
    const char* status, const char* code, const base::ErrorDetails& details) noexcept {
  if (root_.errors) root_.errors->Report("ai", code, {}, details);
  try {
    if (!root_.store.SetJobStatus(id, message, status) && root_.errors)
      root_.errors->Report("ai", "job_status_write_failed");
  } catch (...) {
    if (root_.errors) root_.errors->Report("ai", "job_status_write_failed");
  }
}

bool ReplyJobQueue::ProcessPending() noexcept {
  char id[ai::kIdBytes], message[ai::kIdBytes];
  bool claimed = false;
  bool claim_failed = true;
  try {
    claimed = root_.store.NextPendingJob(id, message);
    claim_failed = false;
  } catch (const std::bad_alloc&) {
    base::LogBoundaryException("AI job claim", "std::bad_alloc");
  } catch (const std::exception&) {
    base::LogBoundaryException("AI job claim", "std::exception");
  } catch (...) {
    base::LogBoundaryException("AI job claim", "unknown exception");
  }
  if (claim_failed && root_.errors) root_.errors->Report("ai", "job_queue_read_failed");
  if (!claimed) return false;
  fortis::base::UniqueLock lock(mutex);
  ++active_jobs;
  lock.Unlock();
  try {
    root_.conversations.GenerateJob(id, message);
  } catch (const std::bad_alloc&) {
    base::LogBoundaryException("AI reply job", "std::bad_alloc");
    root_.conversations.FailJob(id, message, "failed", "provider_exception");
  } catch (const std::exception&) {
    base::LogBoundaryException("AI reply job", "std::exception");
    root_.conversations.FailJob(id, message, "failed", "provider_exception");
  } catch (...) {
    base::LogBoundaryException("AI reply job", "unknown exception");
    root_.conversations.FailJob(id, message, "failed", "provider_exception");
  }
  lock.Lock();
  --active_jobs;
  return true;
}

void* ReplyJobQueue::AsyncWorkerMain(void* opaque) {
    auto* self = static_cast<Impl*>(opaque);
    for (;;) {
      fortis::base::UniqueLock lock(self->queue.mutex);
      const bool stop = self->queue.stopping;
      lock.Unlock();
      if (stop) break;
      const bool processed = self->queue.ProcessPending();
      if (!processed) {
        timespec pause{0, 300 * 1000 * 1000};
        nanosleep(&pause, nullptr);
      }
    }
    return nullptr;
  }

bool ConversationService::BuildResponse(const TurnRequest& turn, const char* reply_id, const char* fresh, size_t fresh_length,
                     http::HttpResponse* response, Encoded* encoded) {
    auto& store = root_.store;
    tj::Arena arena = tj::GrowingArena(kWorkArenaBytes);
    std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
    ai::MessageView user, reply;
    const char* snapshot = nullptr;
    size_t snapshot_length = 0;
    if (!store.LoadMessage(turn.conversation, turn.user_message, arena, &user, turn.open ? nullptr : &snapshot,
                           turn.open ? nullptr : &snapshot_length) ||
        (reply_id && !store.LoadMessage(turn.conversation, reply_id, arena, &reply)))
      return false;
    if (reply_id && fresh) {
      reply.body = fresh;
      reply.body_length = fresh_length;
    }
    const Wire user_wire = wire_of(user, "user");
    const Wire reply_wire = reply_id ? wire_of(reply, reply.role) : Wire{};
    if (turn.protobuf) {
      char expires[32] = "";
      if (turn.open && turn.expires_at_ms && !fortis::compat::token_text::IsoString(turn.expires_at_ms, expires)) return false;
      const size_t size = turn.open
          ? string_size(strlen(turn.conversation)) + string_size(strlen(turn.access_token)) +
                (turn.expires_at_ms ? string_size(strlen(expires)) : 0) +
                1 + pw::VarintSize(message_size(user_wire)) + message_size(user_wire) +
                (reply_id ? 1 + pw::VarintSize(message_size(reply_wire)) + message_size(reply_wire) : 0)
          : 1 + pw::VarintSize(message_size(user_wire)) + message_size(user_wire) +
                (reply_id ? 1 + pw::VarintSize(message_size(reply_wire)) + message_size(reply_wire) : 0);
      encoded->data = base::AllocateStorage<uint8_t[]>(size ? size : 1);
      if (!encoded->data) return false;
      OutBuffer out{encoded->data.get(), size};
      if (turn.open) {
        string_field(out, 0x0a, turn.conversation, strlen(turn.conversation));
        string_field(out, 0x12, turn.access_token, strlen(turn.access_token));
        if (turn.expires_at_ms) string_field(out, 0x1a, expires, strlen(expires));
        message_submessage(out, 0x22, user_wire);
        if (reply_id) message_submessage(out, 0x2a, reply_wire);
      } else {
        message_submessage(out, 0x0a, user_wire);
        if (reply_id) message_submessage(out, 0x12, reply_wire);
      }
      if (!out.ok || out.used != size) return false;
      encoded->length = out.used;
    } else {
      Value* root = tj::Obj(arena);
      if (!root) return false;
      if (turn.open) {
        char expires[32];
        Value* message = message_json(arena, user_wire, false);
        Value* assistant = reply_id ? message_json(arena, reply_wire, true) : nullptr;
        if ((turn.expires_at_ms && !fortis::compat::token_text::IsoString(turn.expires_at_ms, expires)) || !message ||
            !tj::Set(arena, root, "id", tj::Str(arena, turn.conversation)) ||
            !tj::Set(arena, root, "accessToken", tj::Str(arena, turn.access_token)) ||
            (turn.expires_at_ms && !tj::Set(arena, root, "expiresAt", tj::Str(arena, expires))) ||
            !tj::Set(arena, root, "message", message) ||
            (reply_id && (!assistant || !tj::Set(arena, root, "assistant", assistant))))
          return false;
      } else {
        // publicMessage(row): every stored field, createdAt as ISO text.
        Value* message = tj::Obj(arena);
        Value* portfolio = snapshot ? tj::Parse(arena, reinterpret_cast<const uint8_t*>(snapshot), snapshot_length)
                                    : tj::Null(arena);
        Value* assistant = reply_id ? message_json(arena, reply_wire, true) : nullptr;
        if (!message || !portfolio || !tj::Set(arena, message, "id", tj::Str(arena, user.id)) ||
            !tj::Set(arena, message, "conversationId", tj::Str(arena, turn.conversation)) ||
            !tj::Set(arena, message, "clientMessageId", tj::Str(arena, user.client_message_id)) ||
            !tj::Set(arena, message, "role", tj::Str(arena, user.role)) ||
            !tj::Set(arena, message, "body", tj::Str(arena, user.body, user.body_length)) ||
            !tj::Set(arena, message, "portfolioSnapshot", portfolio) ||
            !tj::Set(arena, message, "createdAt", tj::Str(arena, user_wire.created_at)) ||
            !tj::Set(arena, root, "message", message) ||
            (reply_id && (!assistant || !tj::Set(arena, root, "assistant", assistant))))
          return false;
      }
      if (!stringify(root, encoded)) return false;
    }
    if (encoded->length > kResponseLimit) return false;
    *response = http::HttpResponse{201, "Created", turn.protobuf ? kProtobufType : kJsonType, encoded->data.get(),
                             encoded->length};
    response->vary_accept = true;
    response->rate_limit_cost = turn.policy.cost;
    response->rate_limit_remaining = turn.policy.remaining;
    return true;
  }

Policy ConversationRoutes::policy_of(const auth::DevicePolicyResult& result) { return Policy{result.rate_limit_cost, result.rate_limit_remaining}; }

bool ConversationRoutes::OverQuota(char scope, const char* device, uint64_t limit) {
    uint64_t count = 0;
    if (!quotas.Increment(scope, device, root_.Now(), kQuotaWindowMs, &count)) return false;
    return count > limit;
  }

void ConversationRoutes::Open(int client, const http::HttpRequest& request, bool head) {
    auto& config = root_.config;
    auto& store = root_.store;
    tj::Arena arena = tj::GrowingArena(kRequestArenaBytes);
    std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
    const auth::DevicePolicyResult authorized = auth::AuthorizeDeviceRequest(request, 1, config.attestation_required);
    if (reply_policy(client, authorized, head)) return;
    const Policy policy = policy_of(authorized);
    Body body;
    if (!parse_body(client, request, policy, arena, &body)) return;
    Field f[kOpenFieldCount];
    bool decoded = true;
    if (body.kind == BodyKind::kProtobuf) {
      if (!proto_fields(body.bytes, body.length, arena, f, kOpenFieldCount)) {
        send_bad_request(client, head, policy);
        return;
      }
      // Every field of the decoded message is defined ("" by default).
      for (Field& field : f)
        if (!field.present) field = Field{"", 0, true};
    } else {
      decoded = body.kind == BodyKind::kJson && json_fields(body.json, kOpenNames, kOpenFieldCount, f);
    }
    if (!decoded || !open_schema(f)) {
      send_bad_request(client, head, policy);
      return;
    }
    if (OverQuota('o', authorized.device.id, kOpenQuota)) {
      send(client, head, 429, "Too Many Requests", "{\"error\":\"RATE_LIMITED\"}", policy, 3600);
      return;
    }
    const Field snapshot = well_formed(f[kSnapshotJson], arena);
    const Field first = well_formed(f[kFirstMessage], arena);
    if (!snapshot.text || !first.text) { send_internal(client, head, policy); return; }
    ai::OpenInput input{authorized.device.id, f[kAddress].text, f[kNonce].text, f[kTimestamp].text,
                        f[kSnapshotHash].text, snapshot.text, snapshot.length, f[kAccessToken].text,
                        f[kAccessTokenHash].text, f[kSignature].text, first.text, first.length,
                        f[kClientMessageId].text};
    ai::Opened opened;
    bool storage_error = false;
    if (!store.Open(input, &opened, &storage_error)) {
      if (storage_error) send_internal(client, head, policy);
      else send(client, head, 401, "Unauthorized", "{\"error\":\"PROOF_INVALID\"}", policy);
      return;
    }
    TurnRequest turn;
    turn.open = true;
    memcpy(turn.conversation, opened.id, ai::kIdBytes);
    memcpy(turn.user_message, opened.message_id, ai::kIdBytes);
    snprintf(turn.access_token, sizeof(turn.access_token), "%s", f[kAccessToken].text);
    turn.expires_at_ms = opened.expires_at_ms;
    turn.protobuf = AcceptsProtobuf(request);
    turn.head = head;
    turn.policy = policy;
    http::HttpResponse response;
    Encoded encoded;
    if (root_.conversations.BuildResponse(turn, nullptr, nullptr, 0, &response, &encoded)) WriteHttpResponse(client, response, head);
    else send_internal(client, head, policy);
  }

bool ConversationRoutes::ChatAuthorized(const http::HttpRequest& request, const char* id, const char* device,
                      bool* storage_error) {
    auto& store = root_.store;
    const char* token =
        ai::ChatBearerToken(request.intelligence_authorization);
    return store.Authorize(id, device, token, strlen(token), storage_error);
  }

void ConversationRoutes::Messages(int client, const http::HttpRequest& request, bool head) {
    auto& config = root_.config;
    auto& store = root_.store;
    if (reject_long_id(client, request, head)) return;
    const auth::DevicePolicyResult authorized = auth::AuthorizeDeviceRequest(request, 1, config.attestation_required);
    if (reply_policy(client, authorized, head)) return;
    const Policy policy = policy_of(authorized);
    const char* id = HttpPathParamValue(request, "id");
    bool storage_error = false;
    if (!ai::IsConversationId(id) || !ChatAuthorized(request, id, authorized.device.id, &storage_error)) {
      if (storage_error) { send_internal(client, head, policy); return; }
      send_not_found(client, head, policy);
      return;
    }
    tj::Arena arena = tj::GrowingArena(kWorkArenaBytes);
    std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
    char before[ai::kIdBytes];
    size_t page_limit = ai::kMaxHistory;
    if (!message_page_query(request.raw_target, before, &page_limit)) {
      send_bad_request(client, head, policy);
      return;
    }
    ai::MessageView* rows = nullptr;
    size_t count = 0;
    bool has_more = false;
    if (!store.MessagePage(id, before[0] ? before : nullptr, page_limit,
                           arena, &rows, &count, &has_more)) {
      send_internal(client, head, policy);
      return;
    }
    const bool protobuf = AcceptsProtobuf(request);
    Encoded encoded;
    size_t first = 0;
    const bool encoded_ok = ai::EncodeMessagesBody(rows, count, protobuf, kResponseLimit, &encoded.data, &encoded.length, &first);
    if (!encoded_ok) {
      send_internal(client, head, policy);
      return;
    }
    http::HttpResponse response{200, "OK", protobuf ? kProtobufType : kJsonType, encoded.data.get(), encoded.length};
    response.cache_control = "private, no-store";
    response.vary_accept = true;
    response.rate_limit_cost = policy.cost;
    response.rate_limit_remaining = policy.remaining;
    if (has_more && count && first == 0) {
      snprintf(response.next_cursor, sizeof(response.next_cursor), "%s", rows[0].id);
    }
    if (!before[0]) {
      for (size_t i = count; i > 0; --i) {
        if (strcmp(rows[i - 1].role, "user") != 0) continue;
        char status[16]{};
        if (store.JobStatus(id, rows[i - 1].id, status, sizeof(status)) &&
            (strcmp(status, "pending") == 0 || strcmp(status, "running") == 0 ||
             ai::IsFailedReplyStatus(status) || strcmp(status, "complete") == 0)) {
          snprintf(response.ai_reply_for, sizeof(response.ai_reply_for), "%s", rows[i - 1].id);
          snprintf(response.ai_reply_status, sizeof(response.ai_reply_status), "%s", status);
        }
        break;
      }
    }
    WriteHttpResponse(client, response, head);
  }

void ConversationRoutes::Retry(int client, const http::HttpRequest& request, bool head) {
    auto& config = root_.config;
    auto& store = root_.store;
    const auth::DevicePolicyResult authorized = auth::AuthorizeDeviceRequest(request, 1, config.attestation_required);
    if (reply_policy(client, authorized, head)) return;
    const Policy policy = policy_of(authorized);
    const char* id = HttpPathParamValue(request, "id");
    const char* message = HttpPathParamValue(request, "messageId");
    bool storage_error = false;
    if (!ai::IsConversationId(id) || !ai::IsConversationId(message) ||
        !ChatAuthorized(request, id, authorized.device.id, &storage_error)) {
      if (storage_error) send_internal(client, head, policy);
      else send_not_found(client, head, policy);
      return;
    }
    tj::Arena arena = tj::GrowingArena(1024 * 1024);
    std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
    ai::MessageView row;
    char status[16]{};
    if (!store.LoadMessage(id, message, arena, &row) || strcmp(row.role, "user") != 0 ||
        !store.JobStatus(id, message, status, sizeof(status)) || !ai::IsFailedReplyStatus(status)) {
      send_not_found(client, head, policy);
      return;
    }
    if (!store.SetJobStatus(id, message, "pending")) { send_internal(client, head, policy); return; }
    send(client, head, 202, "Accepted", "{\"status\":\"pending\"}", policy);
  }

void ConversationRoutes::PostMessage(int client, const http::HttpRequest& request, bool head) {
    auto& config = root_.config;
    auto& store = root_.store;
    if (reject_long_id(client, request, head)) return;
    tj::Arena arena = tj::GrowingArena(kRequestArenaBytes);
    std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
    const auth::DevicePolicyResult authorized = auth::AuthorizeDeviceRequest(request, 1, config.attestation_required);
    if (reply_policy(client, authorized, head)) return;
    const Policy policy = policy_of(authorized);
    Body body;
    if (!parse_body(client, request, policy, arena, &body)) return;
    const char* id = HttpPathParamValue(request, "id");
    bool storage_error = false;
    if (!conversation_id(id) || !ChatAuthorized(request, id, authorized.device.id, &storage_error)) {
      if (storage_error) { send_internal(client, head, policy); return; }
      send_not_found(client, head, policy);
      return;
    }
    Field f[kMessageFieldCount];
    bool decoded = true;
    if (body.kind == BodyKind::kProtobuf) {
      if (!proto_fields(body.bytes, body.length, arena, f, kMessageFieldCount)) {
        send_bad_request(client, head, policy);
        return;
      }
      // Implicit-presence fields default to ""; the optional pair keeps its presence.
      for (size_t i = 0; i < 2; ++i)
        if (!f[i].present) f[i] = Field{"", 0, true};
    } else {
      decoded = body.kind == BodyKind::kJson &&
                json_fields(body.json, kMessageNames, kMessageFieldCount, f);
    }
    if (!decoded || !message_schema(f)) {
      send_bad_request(client, head, policy);
      return;
    }
    if (f[kMessageSnapshotJson].present != f[kMessageSnapshotHash].present) {
      send(client, head, 400, "Bad Request", "{\"error\":\"SNAPSHOT_INVALID\"}", policy);
      return;
    }
    if (OverQuota('m', authorized.device.id, kMessageQuota)) {
      send(client, head, 429, "Too Many Requests", "{\"error\":\"RATE_LIMITED\"}", policy, 3600);
      return;
    }
    const Field text = well_formed(f[kMessageBody], arena);
    const Field snapshot = f[kMessageSnapshotJson].present ? well_formed(f[kMessageSnapshotJson], arena) : Field{};
    if (!text.text || (f[kMessageSnapshotJson].present && !snapshot.text)) {
      send_internal(client, head, policy);
      return;
    }
    TurnRequest turn;
    const ai::AddStatus status = store.AddMessage(id, f[kMessageClientId].text, text.text, text.length,
                                                  snapshot.present ? snapshot.text : nullptr, snapshot.length,
                                                  f[kMessageSnapshotHash].text, turn.user_message);
    if (status == ai::AddStatus::kInvalid) {
      send(client, head, 400, "Bad Request", "{\"error\":\"SNAPSHOT_INVALID\"}", policy);
      return;
    }
    if (status != ai::AddStatus::kAdded) {
      send_internal(client, head, policy);
      return;
    }
    snprintf(turn.conversation, sizeof(turn.conversation), "%s", id);
    turn.protobuf = AcceptsProtobuf(request);
    turn.head = head;
    turn.policy = policy;
    http::HttpResponse response;
    Encoded encoded;
    if (root_.conversations.BuildResponse(turn, nullptr, nullptr, 0, &response, &encoded)) WriteHttpResponse(client, response, head);
    else send_internal(client, head, policy);
  }

void ConversationRoutes::HandleRetry(int client, const http::HttpRequest& request, bool head, void* context) {
    static_cast<Impl*>(context)->routes.Retry(client, request, head);
  }

void ConversationRoutes::HandleOpen(int client, const http::HttpRequest& request, bool head, void* context) {
    static_cast<Impl*>(context)->routes.Open(client, request, head);
  }

void ConversationRoutes::HandleMessages(int client, const http::HttpRequest& request, bool head, void* context) {
    static_cast<Impl*>(context)->routes.Messages(client, request, head);
  }

void ConversationRoutes::HandlePost(int client, const http::HttpRequest& request, bool head, void* context) {
    static_cast<Impl*>(context)->routes.PostMessage(client, request, head);
  }

void AiConversationService::ImplDeleter::operator()(Impl* impl) const { fortis::base::Delete(impl); }

AiConversationService::~AiConversationService() { Stop(); }

AiConversationService::AiConversationService(Impl* impl) : impl_(impl) {}
AiConversationService::AiConversationService(AiConversationService&&) noexcept = default;

base::Result<AiConversationService> AiConversationService::Create(
    const AiServiceConfig& config, AiServiceDeps deps, const AiServiceSeams* seams) try {
  auto& kv = deps.kv;
  if (!config.support_auth_secret_configured || !ai::OutboundPatternsReady())
    return base::Status(base::StatusCode::kInvalidArgument, "AI configuration");
  Impl* impl = fortis::base::New<Impl>();
  if (!impl) return base::Status(base::StatusCode::kResourceExhausted, "AI allocation");
  AiConversationService service(impl);
  impl->config = config;
  impl->kv = &kv;
  impl->errors = deps.errors;
  if (seams) {
    impl->seams = *seams;
    impl->have_seams = true;
  }
  fortis::base::IdSource* test_ids = impl->have_seams ? impl->seams.new_id : nullptr;
  if (!impl->stored.Start(&kv, config.support_auth_secret, config.support_auth_secret_length, &impl->Clock(),
                         test_ids)) {
    return base::Status(base::StatusCode::kUnavailable, "AI initialization");
  }
  if (!impl->generator.ConfigureAnthropic() || !impl->store.ResetRunningJobs()) {
    return base::Status(base::StatusCode::kUnavailable, "AI initialization");
  }
  pthread_attr_t attributes;
  const bool attributes_ready = pthread_attr_init(&attributes) == 0;
  bool ok = attributes_ready;
  if (ok) ok = pthread_attr_setstacksize(&attributes, 256 * 1024) == 0;
  for (size_t i = 0; ok && i < impl->generator.ai_concurrency; ++i) {
    ok = (impl->queue.workers[i].StartEntry<ReplyJobQueue::AsyncWorkerMain>(impl, &attributes) ? 0 : -1) == 0;
    if (ok) ++impl->queue.started_workers;
  }
  if (attributes_ready) pthread_attr_destroy(&attributes);
  if (!ok) {
    return base::Status(base::StatusCode::kUnavailable, "AI initialization");
  }
  return service;
} catch (const std::bad_alloc&) {
  return base::Status(base::StatusCode::kResourceExhausted, "ai allocation");
}

bool AiConversationService::Register(http::HttpRouter& router, http::HttpResponseHandoff& handoff) {
  (void)handoff;  // The async contract never reserves an HTTP response.
  return router.Add("POST", "/v1/ai/conversations", ConversationRoutes::HandleOpen, impl_.get()) &&
         router.Add("GET", "/ai/stats", ConversationRoutes::HandleStats, impl_.get()) &&
         router.Add("GET", "/v1/ai/conversations/:id/messages", ConversationRoutes::HandleMessages, impl_.get()) &&
         router.Add("POST", "/v1/ai/conversations/:id/messages", ConversationRoutes::HandlePost, impl_.get()) &&
         router.Add("POST", "/v1/ai/conversations/:id/messages/:messageId/retry", ConversationRoutes::HandleRetry, impl_.get());
}

bool AiConversationService::WaitIdle(uint32_t timeout_ms) {
  const uint64_t deadline = monotonic_ms() + timeout_ms;
  for (;;) {
    fortis::base::UniqueLock lock(impl_->queue.mutex);
    bool idle = impl_->queue.active_jobs == 0;
    lock.Unlock();
    if (idle) return true;
    if (monotonic_ms() >= deadline) return false;
    timespec pause{0, 10 * 1000 * 1000};
    nanosleep(&pause, nullptr);
  }
}

void AiConversationService::Stop() {
  if (!impl_) return;
  fortis::base::UniqueLock lock(impl_->queue.mutex);
  impl_->queue.stopping = true;
  lock.Unlock();
  for (size_t i = 0; i < impl_->queue.started_workers; ++i) impl_->queue.workers[i].Join();
  impl_->stored.Stop();
  mbedtls_platform_zeroize(impl_->generator.anthropic_key, sizeof(impl_->generator.anthropic_key));
  impl_.reset();
}

}  // namespace fortis::ai
