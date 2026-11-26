// What every address-plane route handler shares: query and path-parameter
// parsing, the immediate replies and refusals, and handing a task to the
// service.
#include "query_string.h"
#include "address_service_internal.h"
#include "http_middleware.h"
#include "ownership.h"

namespace hb = fortis::codec::history_batch;

namespace tj = fortis::codec::tj;

namespace fortis::wallet {

namespace {

// ---- replies --------------------------------------------------------------------

void send_body(int client, int status, const char* reason, const char* body, size_t length, bool head, int cost,
               uint32_t remaining) {
  http::HttpResponse response{status, reason, kJsonType, reinterpret_cast<const uint8_t*>(body), length};
  response.rate_limit_cost = cost;
  response.rate_limit_remaining = remaining;
  WriteHttpResponse(client, response, head);
}

}  // namespace

namespace address_service_internal {

bool query_value(const char* target, const char* name, QueryValue* out) {
  out->count = 0;
  out->storage.reset();
  out->value = nullptr;
  out->length = 0;
  out->too_long = false;
  const auto query = http::query_string::Query::FromTarget(target);
  if (!query.Valid()) return false;
  http::query_string::Component value;
  if (!query.First(name, &value)) return true;
  const size_t length = value.Decode(nullptr, 0);
  if (length >= kQueryBytes) { out->too_long = true; out->count = 1; return true; }
  out->storage = base::AllocateStorage<char[]>(length + 1);
  out->value = out->storage.get();
  if (!out->value) return false;
  value.Decode(out->value, length);
  out->value[length] = '\0';
  out->length = length;
  out->count = 1;
  return true;
}

// z.string().max(units): one valid string of at most `units` UTF-16 units.
bool string_value(const QueryValue& q, size_t max_units) {
  if (q.count != 1 || q.too_long || !q.value) return false;
  if (!hb::ValidUtf8(reinterpret_cast<const uint8_t*>(q.value), q.length)) return false;
  return hb::Utf16Units(q.value, q.length) <= max_units;
}

// Decimal request integer within the route's inclusive bounds.
bool coerce_int(const QueryValue& q, double lo, double hi, double* out) {
  if (q.count != 1 || q.too_long || !q.value) return false;
  uint64_t value = 0;
  if (!http::query_string::ParseUnsigned(q.value, q.length, static_cast<uint64_t>(hi), &value) ||
      static_cast<double>(value) < lo) return false;
  *out = static_cast<double>(value);
  return true;
}

void send_text(int client, int status, const char* reason, const char* body, bool head, int cost, uint32_t remaining) {
  send_body(client, status, reason, body, strlen(body), head, cost, remaining);
}

size_t error_body(const char* error, const char* detail, char* out, size_t capacity) {
  uint8_t arena_bytes[8192];
  tj::Arena a{arena_bytes, sizeof(arena_bytes), 0, true};
  Value* o = tj::Obj(a);
  if (!o || !tj::Set(a, o, "error", tj::Str(a, error)) || (detail && !tj::Set(a, o, "detail", tj::Str(a, detail))))
    return 0;
  tj::OutBuffer buffer{reinterpret_cast<uint8_t*>(out), capacity, 0, true};
  if (!tj::Stringify(o, buffer) || !buffer.ok) return 0;
  return buffer.used;
}

void send_error(int client, int status, const char* reason, const char* error, const char* detail, bool head, int cost,
                uint32_t remaining) {
  char body[4096];
  const size_t length = error_body(error, detail, body, sizeof(body));
  send_body(client, status, reason, body, length, head, cost, remaining);
}

bool reply_policy(int client, const auth::DevicePolicyResult& policy, bool head) {
  if (!policy.status) return false;
  const char* body = policy.error_body ? policy.error_body : "{\"error\":\"INTERNAL\"}";
  http::HttpResponse response{policy.status, policy.reason, kJsonType, reinterpret_cast<const uint8_t*>(body), strlen(body)};
  response.rate_limit_cost = policy.rate_limit_cost;
  response.rate_limit_remaining = policy.rate_limit_remaining;
  response.retry_after = policy.retry_after;
  WriteHttpResponse(client, response, head);
  return true;
}

// A parameter over 100 UTF-16 units once decoded: 414
// {"error":"URI_TOO_LONG"} before any hook (no auth, no quota).
bool reject_long_params(int client, const http::HttpRequest& request, bool head) {
  static const char* const kNames[] = {"network", "address", "hash"};
  bool too_long = false;
  for (const char* name : kNames) {
    const http::HttpPathParam* param = HttpPathParamFind(request, name);
    if (param && param->utf16_units > 100) too_long = true;
  }
  if (!too_long) return false;
  http::WriteRefusal(client, 414, "URI Too Long", "URI_TOO_LONG", head);
  return true;
}

// A raw byte >= 0x80 in the request target: 400 {"error":"BAD_REQUEST"}
// before any hook (no quota).
bool reject_raw_non_ascii(int client, const http::HttpRequest& request, bool head) {
  for (const char* p = request.raw_target ? request.raw_target : ""; *p; ++p) {
    if (static_cast<unsigned char>(*p) < 0x80) continue;
    http::WriteRefusal(client, 400, "Bad Request", "BAD_REQUEST", head);
    return true;
  }
  return false;
}

// One path parameter, percent-decoded by the router (which has refused an
// empty segment, a control character/DEL and a value over 512 bytes with
// 404). False when it is over `max_units` UTF-16 units or `capacity`.
bool path_param(const http::HttpRequest& request, const char* name, size_t max_units, char* out,
                size_t capacity) {
  const http::HttpPathParam* param = HttpPathParamFind(request, name);
  if (!param || param->utf16_units < 1 || param->utf16_units > max_units) return false;
  return snprintf(out, capacity, "%s", param->value) < static_cast<int>(capacity);
}


AddressTask* new_task(AddressTask::Kind kind, const http::HttpRequest& request, const auth::DevicePolicyResult& policy) {
  auto* task = fortis::base::New<AddressTask>();
  if (!task) return nullptr;
  task->kind = kind;
  task->protobuf = AcceptsProtobuf(request);
  if (request.if_none_match && strlen(request.if_none_match) < sizeof(task->if_none_match)) {
    task->has_if_none_match = true;
    snprintf(task->if_none_match, sizeof(task->if_none_match), "%s", request.if_none_match);
  }
  task->cost = policy.rate_limit_cost;
  task->remaining = policy.rate_limit_remaining;
  snprintf(task->device_id, sizeof(task->device_id), "%s", policy.device.id);
  return task;
}

void dispatch(AddressService::Impl& owner, int client, bool head, AddressTask* task) {
  AddressService::Impl* impl = &owner;
  if (!task) {
    send_text(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", head, -1, 0);
    return;
  }
  task->head = head;
  uint64_t token = 0;
  if (!http::ReserveHttpResponse(client, head, kHandoffTimeoutMs, &token)) {
    http::HttpResponse response{429, "Too Many Requests", kJsonType, reinterpret_cast<const uint8_t*>(kBusy), strlen(kBusy)};
    response.retry_after = 2;
    response.rate_limit_cost = task->cost;
    response.rate_limit_remaining = task->remaining;
    WriteHttpResponse(client, response, head);
    free_task(task);
    return;
  }
  task->token = token;
  if (!impl->Submit(task)) {
    http::HttpResponse response{429, "Too Many Requests", kJsonType, reinterpret_cast<const uint8_t*>(kBusy), strlen(kBusy)};
    response.retry_after = 2;
    response.rate_limit_cost = task->cost;
    response.rate_limit_remaining = task->remaining;
    impl->handoff->Complete(token, response);
    free_task(task);
  }
}

void bad_request(int client, bool head, const auth::DevicePolicyResult& policy, const char* detail) {
  send_error(client, 400, "Bad Request", "BAD_REQUEST", detail, head, policy.rate_limit_cost,
             policy.rate_limit_remaining);
}

bool copy_into(char* out, size_t capacity, const char* text) {
  const size_t length = strlen(text);
  if (length >= capacity) return false;
  memcpy(out, text, length + 1);
  return true;
}

}  // namespace address_service_internal

}  // namespace fortis::wallet
