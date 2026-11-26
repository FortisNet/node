#include "http_middleware.h"
#include "unit_test_support.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {
using namespace fortis::http;
namespace testing = fortis::testing;
using testing::Require;

void metadata_and_body_ownership() {
  for (const size_t length : {size_t{0}, size_t{512}, size_t{513}, size_t{2 * 1024 * 1024}}) {
    HttpReply reply;
    {
      std::vector<uint8_t> body(length, 0x7f);
      char reason[] = "Accepted";
      char type[] = "application/octet-stream";
      HttpResponse response{202, reason, type, body.data(), body.size()};
      response.vary_accept = true;
      response.rate_limit_cost = 7;
      response.rate_limit_remaining = 11;
      response.retry_after = 19;
      response.cache_control = "private, max-age=7";  // documented static storage
      response.extra_headers = "X-Unit: yes\r\n";    // documented static storage
      strcpy(response.etag, "W/\"unit\"");
      strcpy(response.www_authenticate, "Bearer realm=unit");
      strcpy(response.location, "/next");
      strcpy(response.next_cursor, "01ARZ3NDEKTSV4RRFFQ69G0000");
      strcpy(response.ai_reply_for, "01ARZ3NDEKTSV4RRFFQ69G0001");
      strcpy(response.ai_reply_status, "pending");
      Require(reply.Send(response), "capture boundary-sized response");
      memset(reason, 'x', sizeof(reason) - 1);
      memset(type, 'x', sizeof(type) - 1);
      std::fill(body.begin(), body.end(), 0);
      memset(response.etag, 0, sizeof(response.etag));
      memset(response.location, 0, sizeof(response.location));
    }
    const auto& captured = reply.response();
    Require(reply.sent() && captured.status == 202 && captured.body_length == length, "captured response metadata");
    Require(!strcmp(captured.reason, "Accepted") && !strcmp(captured.content_type, "application/octet-stream"),
            "reason/type outlive stack storage");
    for (size_t i = 0; i < length; ++i) Require(captured.body[i] == 0x7f, "captured body owns its bytes");
    Require(captured.vary_accept && captured.rate_limit_cost == 7 && captured.rate_limit_remaining == 11 &&
                captured.retry_after == 19, "quota and representation flags retained");
    Require(!strcmp(captured.etag, "W/\"unit\"") && !strcmp(captured.www_authenticate, "Bearer realm=unit") &&
                !strcmp(captured.location, "/next") && !strcmp(captured.next_cursor, "01ARZ3NDEKTSV4RRFFQ69G0000") &&
                !strcmp(captured.ai_reply_for, "01ARZ3NDEKTSV4RRFFQ69G0001") && !strcmp(captured.ai_reply_status, "pending"),
            "inline header arrays copied with response");
  }
}

void invalid_response_is_atomic() {
  const HttpResponse good = JsonResponse(200, "OK", "{}");
  std::vector<HttpResponse> invalid;
  auto bad = good;
  bad.reason = "OK\r\nInjected: yes";
  invalid.push_back(bad);
  bad = good;
  bad.content_type = "text/plain\nInjected: yes";
  invalid.push_back(bad);
  bad = good;
  strcpy(bad.etag, "bad\rvalue");
  invalid.push_back(bad);
  bad = good;
  memset(bad.location, 'x', sizeof(bad.location));
  invalid.push_back(bad);
  bad = good;
  bad.extra_headers = "server-timing: forged\r\n";
  invalid.push_back(bad);
  bad = good;
  bad.body = nullptr;
  invalid.push_back(bad);
  std::vector<uint8_t> oversized(2 * 1024 * 1024 + 1, 'x');
  bad = good;
  bad.body = oversized.data();
  bad.body_length = oversized.size();
  invalid.push_back(bad);
  for (const auto& response : invalid) {
    HttpReply reply;
    Require(!reply.Send(response) && !reply.sent(), "invalid response is not partially captured");
  }
  HttpReply retry;
  Require(!retry.Send(invalid.front()) && retry.Send(good), "header refusal leaves reply available for a valid response");
  Require(!retry.Send(JsonResponse(500, "Error", "bad")) && retry.response().status == 200 &&
              retry.response().body_length == 2 && !memcmp(retry.response().body, "{}", 2),
          "second send cannot replace the first captured answer");
}

void detached_head_static_body_and_defer() {
  static const uint8_t body[] = {0, 1, 0xff};
  HttpReply head(true);
  uint64_t token = 123;
  Require(!head.Reserve(1000, &token) && token == 123 && !head.reserved(), "detached reserve has no side effects");
  Require(!head.Defer(0), "cannot defer before send");
  HttpResponse response{200, "OK", "application/octet-stream", body, sizeof(body)};
  response.static_body = true;
  Require(head.Send(response) && head.head() && head.head_only(), "HEAD retains representation metadata");
  Require(head.response().body == body && head.response().body_length == sizeof(body), "static body remains borrowed");
  Require(!head.Defer(10001) && !head.deferred(), "excessive delay is refused without poisoning reply");
  Require(head.Defer(10000) && !head.Defer(0), "defer maximum accepted exactly once");
  HttpReply empty;
  HttpResponse no_content{204, "No Content", nullptr, nullptr, 0};
  no_content.omit_content_type = true;
  Require(empty.Send(no_content) && !empty.response().content_type && !empty.head_only(), "empty response needs no type or body");
}

struct Steps {
  bool authorized = false;
  unsigned parser_calls = 0;
  unsigned provider_calls = 0;
};
bool authorize(const HttpRequest&, HttpReply& reply, Steps& state) {
  if (state.authorized) return true;
  SendRefusal(reply, 401, "Unauthorized", "UNAUTHORIZED");
  return false;
}
bool parse(const HttpRequest& request, HttpReply& reply, Steps& state) {
  ++state.parser_calls;
  HttpBodyParser parser;
  return SelectBodyParser(request, reply, {7, 11}, &parser);
}
bool provider(const HttpRequest&, HttpReply& reply, Steps& state) {
  ++state.provider_calls;
  return reply.Send(JsonResponse(200, "OK", "{}"));
}
void route_steps_stop_before_side_effects() {
  constexpr HttpStep<Steps> steps[] = {authorize, parse, provider};
  HttpRequest malformed{};
  malformed.has_content_type = true;
  malformed.json_content_type = true;
  HttpReply refused;
  Steps unauthorized;
  Require(!RunHttpSteps(steps, malformed, refused, unauthorized), "authentication refuses chain");
  Require(refused.response().status == 401 && unauthorized.parser_calls == 0 && unauthorized.provider_calls == 0,
          "auth refusal cannot parse a body or call a provider");
  HttpReply invalid(true);
  Steps authorized;
  authorized.authorized = true;
  Require(!RunHttpSteps(steps, malformed, invalid, authorized), "authorized invalid JSON framing refuses chain");
  Require(invalid.response().status == 400 && invalid.head_only() && invalid.response().rate_limit_cost == 7 &&
              invalid.response().rate_limit_remaining == 11 && authorized.parser_calls == 1 && authorized.provider_calls == 0,
          "charged parser refusal preserves HEAD and prevents provider work");
  malformed.json_content_type = false;
  malformed.protobuf_content_type = true;  // empty protobuf is a valid empty message
  HttpReply success;
  Require(RunHttpSteps(steps, malformed, success, authorized) && authorized.provider_calls == 1 &&
              success.response().status == 200, "accepted framing reaches provider once");
}

}  // namespace

int main() {
  testing::UnitSuite suite;
  suite.Run("metadata/body ownership", metadata_and_body_ownership);
  suite.Run("atomic refusal/one answer", invalid_response_is_atomic);
  suite.Run("detached HEAD/static/defer", detached_head_static_body_and_defer);
  suite.Run("route step refusal ordering", route_steps_stop_before_side_effects);
  return suite.Finish("http_reply_unit_test");
}
