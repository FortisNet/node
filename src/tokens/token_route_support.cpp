#include <string_view>
// What every token route handler shares: query and path-parameter parsing,
// the immediate replies and refusals, and handing a task to the service.
#include "query_string.h"
#include "token_service_internal.h"
#include "http_middleware.h"
#include "ownership.h"

namespace hb = fortis::codec::history_batch;

namespace tj = fortis::codec::tj;

namespace fortis::tokens {

namespace {

// ---- replies ----------------------------------------------------------------

void send_body(int client, int status, const char* reason, const char* body, size_t length, bool head, int cost,
               uint32_t remaining, const char* cache_control = nullptr) {
  http::HttpResponse response{status, reason, kJsonType, reinterpret_cast<const uint8_t*>(body), length};
  response.rate_limit_cost = cost;
  response.rate_limit_remaining = remaining;
  response.cache_control = cache_control;
  WriteHttpResponse(client, response, head);
}

}  // namespace

namespace token_service_internal {

void send_text(int client, int status, const char* reason, const char* body, bool head, int cost,
               uint32_t remaining) {
  send_body(client, status, reason, body, strlen(body), head, cost, remaining);
}

// {"error":"BAD_REQUEST","detail":<message>} with JSON.stringify escaping.
size_t bad_request_body(const char* detail, char* out, size_t capacity) {
  uint8_t arena_bytes[8192];
  tj::Arena a{arena_bytes, sizeof(arena_bytes), 0, true};
  Value* o = tj::Obj(a);
  if (!o || !tj::Set(a, o, "error", tj::Str(a, "BAD_REQUEST")) || !tj::Set(a, o, "detail", tj::Str(a, detail)))
    return 0;
  tj::OutBuffer buffer{reinterpret_cast<uint8_t*>(out), capacity, 0, true};
  if (!tj::Stringify(o, buffer) || !buffer.ok) return 0;
  return buffer.used;
}

void send_bad_request(int client, const char* detail, bool head, int cost, uint32_t remaining) {
  char body[4096];
  const size_t length = bad_request_body(detail, body, sizeof(body));
  if (!length) { send_text(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", head, cost, remaining); return; }
  send_body(client, 400, "Bad Request", body, length, head, cost, remaining);
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


TokenTask* new_task(TokenTask::Kind kind, const http::HttpRequest& request, const auth::DevicePolicyResult& policy) {
  auto* task = fortis::base::New<TokenTask>();
  if (!task) return nullptr;
  task->kind = kind;
  task->protobuf = AcceptsProtobuf(request);
  if (request.if_none_match && strlen(request.if_none_match) < task->if_none_match.capacity()) {
    task->has_if_none_match = true;
    task->if_none_match.Assign(request.if_none_match);
  }  // a longer If-None-Match exceeds every validator, so it can never match
  task->cost = policy.rate_limit_cost;
  task->remaining = policy.rate_limit_remaining;
  return task;
}

// Queues a provider-backed task and transfers the socket to the handoff.
void dispatch(TokenRequestWorker& owner, int client, bool head, TokenTask* task) {
  TokenRequestWorker* impl = &owner;
  if (!task) {
    send_text(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", head, -1, 0);
    return;
  }
  task->head = head;
  uint64_t token = 0;
  if (!http::ReserveHttpResponse(client, head, kHandoffTimeoutMs, &token)) {
    // Every response handoff slot is taken: the same back-pressure contract.
    http::HttpResponse response{429, "Too Many Requests", kJsonType, reinterpret_cast<const uint8_t*>(kBusy), strlen(kBusy)};
    response.retry_after = 2;
    response.rate_limit_cost = task->cost;
    response.rate_limit_remaining = task->remaining;
    WriteHttpResponse(client, response, head);
    fortis::base::Delete(task);
    return;
  }
  task->token = token;
  if (!impl->Submit(task)) {
    // A full token queue is back-pressure (runFacet's LaneBusy contract).
    http::HttpResponse response{429, "Too Many Requests", kJsonType, reinterpret_cast<const uint8_t*>(kBusy), strlen(kBusy)};
    response.retry_after = 2;
    response.rate_limit_cost = task->cost;
    response.rate_limit_remaining = task->remaining;
    impl->Complete(*task, response);
    fortis::base::Delete(task);
  }
}

bool copy_params(TokenTask* task, const char* network, size_t network_length, const char* token, size_t token_length) {
  if (network_length >= task->network.capacity() || token_length >= task->ref.capacity()) return false;
  memcpy(task->network.data(), network, network_length + 1);
  task->network_length = network_length;
  memcpy(task->ref.data(), token, token_length + 1);
  task->ref_length = token_length;
  return true;
}

// A parameter over 100 UTF-16 units once decoded: 414
// {"error":"URI_TOO_LONG"} before any hook (no auth, no quota).
bool reject_long_params(int client, const http::HttpRequest& request, bool head) {
  static const char* const kNames[] = {"network", "tokenId"};
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

// One path parameter (`name`), percent-decoded by the router, which has
// refused an empty segment, a control character/DEL and a value over 512
// bytes with 404. SIZE_MAX when the value does not fit `capacity`.
size_t path_param(const http::HttpRequest& request, const char* name, char* out, size_t capacity) {
  const http::HttpPathParam* param = HttpPathParamFind(request, name);
  if (!param) return SIZE_MAX;
  const size_t used = strlen(param->value);
  if (used + 1 > capacity) return SIZE_MAX;
  memcpy(out, param->value, used + 1);
  return used;
}

// {"error":"BAD_REQUEST","detail":"<prefix><text>"} with `text` taken by
// length (a NUL is escaped as \u0000, as JSON.stringify writes it).
void send_detail(int client, const char* prefix, const char* text, size_t length, bool head, int cost,
                 uint32_t remaining) {
  uint8_t arena_bytes[8192];
  tj::Arena a{arena_bytes, sizeof(arena_bytes), 0, true};
  const size_t prefix_length = strlen(prefix);
  char* detail = static_cast<char*>(a.Alloc(prefix_length + length + 1, 1));
  Value* o = tj::Obj(a);
  char body[4096];
  tj::OutBuffer out{reinterpret_cast<uint8_t*>(body), sizeof(body), 0, true};
  bool ok = detail && o;
  if (ok) {
    memcpy(detail, prefix, prefix_length);
    memcpy(detail + prefix_length, text, length);
    detail[prefix_length + length] = '\0';
    ok = tj::Set(a, o, "error", tj::Str(a, "BAD_REQUEST")) &&
         tj::Set(a, o, "detail", tj::Str(a, detail, prefix_length + length)) && tj::Stringify(o, out) && out.ok;
  }
  if (!ok) { send_text(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", head, cost, remaining); return; }
  send_body(client, 400, "Bad Request", body, out.used, head, cost, remaining);
}

// networkTokenParams for /page and the KV-only reads: both values as the
// router decoded them, 1..64 / 1..160 UTF-16 units.
bool history_params(const http::HttpRequest& request, char (&network)[520], size_t* network_length, char (&token)[520],
                    size_t* token_length) {
  *network_length = path_param(request, "network", network, sizeof(network));
  *token_length = path_param(request, "tokenId", token, sizeof(token));
  if (*network_length == SIZE_MAX || *token_length == SIZE_MAX) return false;
  const size_t n = hb::Utf16Units(network, *network_length);
  const size_t t = hb::Utf16Units(token, *token_length);
  return n >= 1 && n <= 64 && t >= 1 && t <= 160;
}

TargetQuery::TargetQuery(const char* target, const char* const* names, size_t count) : count_(count) {
  capacity_ = strlen(target) + 1;
  memory_ = base::AllocateStorage<char[]>(capacity_ * count);
  const auto query = http::query_string::Query::FromTarget(target);
  valid_ = query.Valid();
  for (size_t i = 0; memory_ && i < count && i < kMax; ++i) {
    // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
    // cppcheck-suppress arithOperationsOnVoidPointer
    values_[i] = memory_.get() + capacity_ * i;
    values_[i][0] = '\0';
    http::query_string::Component value;
    if (valid_ && query.First(names[i], &value)) {
      lengths_[i] = value.Decode(values_[i], capacity_ - 1);
      values_[i][lengths_[i]] = '\0';
      counts_[i] = 1;
    }
  }
}

// facts()' synchronous resolve over the values the router decoded:
// registry.network, then parseTokenRef over the JS-trimmed ref; a
// RangeError 400 with the value as decoded ("unknown network: …" /
// "unsupported token ref: …", JSON-escaped). False once answered.
bool facet_resolve(int client, bool head, int cost, uint32_t remaining, const char* network, size_t network_length,
                   const char* token, size_t token_length) {
  networks::TokenNetwork resolved;
  const networks::NativeNetworkConfig* seed = nullptr;
  networks::TokenRef ref;
  const FactsOutcome status =
      fortis::tokens::token_facts::Resolve(std::string_view{network, network_length}, std::string_view{token, token_length}, &resolved, &seed, &ref);
  if (status == FactsOutcome::kUnknownNetwork) {
    send_detail(client, "unknown network: ", network, network_length, head, cost, remaining);
    return false;
  }
  if (status != FactsOutcome::kOk) {
    send_detail(client, "unsupported token ref: ", token, token_length, head, cost, remaining);
    return false;
  }
  return true;
}

}  // namespace token_service_internal

}  // namespace fortis::tokens
