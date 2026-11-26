#pragma once

#include "http_server.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Shared middleware for HttpHandler routes (http_server.h).
//
// A step is one cross-cutting concern run before a route's own work: it
// reads the request, may record what it learned in the route's per-request
// State, and returns true to let the chain continue. A step that refuses the
// request answers through the reply and returns false; the chain stops and
// the handler returns without doing anything else. A route declares its
// chain as a constant array, in the order the route's contract lists its
// checks (authentication first), and runs it with RunHttpSteps:
//
//   struct WriteState { Body body; DevicePolicyResult device; ... };
//   constexpr HttpStep<WriteState> kWriteSteps[] = {ParseBody, RequireDevice<WriteState, 20>};
//   ...
//   WriteState state;
//   if (!RunHttpSteps(kWriteSteps, request, reply, state)) return;
//
// Only behaviour that is byte-identical across routes belongs here. Route
// families differ on purpose (which media types a body may have, whether a
// form body is an object or just "some other value", whether a parameter is
// checked before or after authentication): those choices stay in the
// family's own steps and are not harmonised.
//
// This header is in the http layer: it must not include auth or anything
// above it. The device-policy step lives in src/auth/device_policy_step.h.
// Guest-compiled: no C++ standard library.

namespace fortis::http {

template <typename State>
using HttpStep = bool (*)(const HttpRequest& request, HttpReply& reply, State& state);

template <typename State, size_t N>
inline bool RunHttpSteps(const HttpStep<State> (&steps)[N], const HttpRequest& request, HttpReply& reply,
                         State& state) {
  for (size_t i = 0; i < N; ++i)
    if (!steps[i](request, reply, state)) return false;
  return true;
}

inline constexpr char kHttpJsonUtf8[] = "application/json; charset=utf-8";

// The quota headers of an authorized request (cost -1: none).
struct HttpQuota {
  int cost = -1;
  uint32_t remaining = 0;
};

// A refusal: {"error":"<code>"} as application/json; charset=utf-8, with the
// quota headers of `quota`. `code` is a trusted upper-snake constant (no
// escaping is applied). A HEAD gets the headers without the body.
inline bool SendRefusal(HttpReply& reply, int status, const char* reason, const char* code, HttpQuota quota = {}) {
  char body[96];
  snprintf(body, sizeof(body), "{\"error\":\"%s\"}", code);
  HttpResponse response{status, reason, kHttpJsonUtf8, reinterpret_cast<const uint8_t*>(body), strlen(body)};
  response.rate_limit_cost = quota.cost;
  response.rate_limit_remaining = quota.remaining;
  return reply.Send(response);
}

// The same refusal for a raw-fd route (HttpRouteHandler), written at once.
inline void WriteRefusal(int client, int status, const char* reason, const char* code, bool head,
                         HttpQuota quota = {}) {
  char body[96];
  snprintf(body, sizeof(body), "{\"error\":\"%s\"}", code);
  HttpResponse response{status, reason, kHttpJsonUtf8, reinterpret_cast<const uint8_t*>(body), strlen(body)};
  response.rate_limit_cost = quota.cost;
  response.rate_limit_remaining = quota.remaining;
  WriteHttpResponse(client, response, head);
}

// 400 BAD_REQUEST: an application/json body that does not parse. The route
// owns the parser.
inline bool SendInvalidJsonBody(HttpReply& reply, HttpQuota quota) {
  return SendRefusal(reply, 400, "Bad Request", "BAD_REQUEST", quota);
}

// The request body kinds of the /v1 routes: JSON and protobuf only.
enum class HttpBodyParser : uint8_t {
  kNone,      // no Content-Type and no body: request.body is undefined
  kJson,      // application/json with a non-empty body (not yet parsed)
  kProtobuf,  // application/protobuf
};

// Selects the parser, or refuses the body's framing. A protected route runs
// it after authentication and the quota (auth-first), so `quota` is the
// authorized request's:
//   - a body without a Content-Type          → 415 UNSUPPORTED_MEDIA_TYPE
//   - application/json with an empty body    → 400 BAD_REQUEST
//   - any other media type (text/plain and form bodies included)
//                                            → 415 UNSUPPORTED_MEDIA_TYPE
// False after answering. What a route does with each parser's body is the
// route's own.
inline bool SelectBodyParser(const HttpRequest& request, HttpReply& reply, HttpQuota quota, HttpBodyParser* parser) {
  if (!request.has_content_type) {
    if (request.body_length) {
      SendRefusal(reply, 415, "Unsupported Media Type", "UNSUPPORTED_MEDIA_TYPE", quota);
      return false;
    }
    *parser = HttpBodyParser::kNone;
    return true;
  }
  if (request.json_content_type) {
    if (!request.body_length) {
      SendRefusal(reply, 400, "Bad Request", "BAD_REQUEST", quota);
      return false;
    }
    *parser = HttpBodyParser::kJson;
    return true;
  }
  if (request.protobuf_content_type) {
    *parser = HttpBodyParser::kProtobuf;
    return true;
  }
  SendRefusal(reply, 415, "Unsupported Media Type", "UNSUPPORTED_MEDIA_TYPE", quota);
  return false;
}

}  // namespace fortis::http
