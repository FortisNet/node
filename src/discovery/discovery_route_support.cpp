#include "discovery_route_support.h"

#include "price_history_batch_codec.h"
#include "proto_wire.h"

#include <stdio.h>
#include <string.h>

namespace fortis::discovery {

namespace {



}  // namespace

int ParseDiscoveryBody(const http::HttpRequest& request, codec::BoundedJsonToken* tokens, size_t token_capacity,
                       DiscoveryBody* body) {
  body->bytes = reinterpret_cast<const uint8_t*>(request.body);
  body->length = request.body_length;
  if (!request.has_content_type) {
    if (request.body_length) return 415;
    body->kind = DiscoveryBodyKind::kUndefined;
    return 0;
  }
  if (request.json_content_type) {
    if (!request.body_length) return 400;
    const unsigned char* json = reinterpret_cast<const unsigned char*>(request.body);
    size_t json_length = request.body_length;
    // One leading byte-order mark is dropped.
    if (json_length >= 3 && json[0] == 0xef && json[1] == 0xbb && json[2] == 0xbf) {
      json += 3; json_length -= 3;
    }
    codec::BoundedJsonError error = codec::BoundedJsonError::kNone;
    if (!body->json.ParseWithLimit(json, json_length, tokens, token_capacity,
                                   codec::kBoundedJsonMaxTokens, &error)) {
      // Invalid JSON is 400; the native bound (256 KiB, 8,192 tokens, depth
      // 16) is 413.
      return error == codec::BoundedJsonError::kInvalidInput ? 400 : 413;
    }
    body->kind = DiscoveryBodyKind::kJson;
    return 0;
  }
  if (request.protobuf_content_type) { body->kind = DiscoveryBodyKind::kProtobuf; return 0; }
  return 415;
}

void SendDiscoveryFraming(int client, int status, bool head, int rate_cost, uint32_t remaining) {
  SendDiscoveryJson(client, status,
                    status == 415 ? "Unsupported Media Type" : status == 413 ? "Payload Too Large" : "Bad Request",
                    status == 415   ? "{\"error\":\"UNSUPPORTED_MEDIA_TYPE\"}"
                    : status == 413 ? "{\"error\":\"BODY_TOO_LARGE\"}"
                                    : "{\"error\":\"BAD_REQUEST\"}",
                    head, rate_cost, remaining);
}

void SendDiscoveryJson(int client, int status, const char* reason, const char* body, bool head,
                       int rate_cost, uint32_t remaining, unsigned retry_after,
                       bool vary_accept) {
  http::HttpResponse response{status, reason, "application/json; charset=utf-8",
      reinterpret_cast<const uint8_t*>(body), strlen(body)};
  response.rate_limit_cost = rate_cost;
  response.rate_limit_remaining = remaining;
  response.retry_after = retry_after;
  response.vary_accept = vary_accept;
  WriteHttpResponse(client, response, head);
}

void SendDiscoveryBadRequest(int client, const char* detail, bool head, int rate_cost,
                             uint32_t remaining) {
  char body[256];
  fortis::codec::json_document_writer::OutBuffer out{reinterpret_cast<uint8_t*>(body), sizeof(body) - 1};
  out.Text("{\"error\":\"BAD_REQUEST\"");
  if (detail) {
    out.Text(",\"detail\":");
    fortis::codec::history_batch::AppendJsonString(out, detail, strlen(detail));
  }
  out.Byte('}');
  body[out.ok ? out.used : 0] = '\0';
  SendDiscoveryJson(client, 400, "Bad Request", body, head, rate_cost, remaining);
}

http::HttpResponse DiscoveryRateLimitedResponse(uint32_t retry_after_ms) {
  http::HttpResponse response = http::JsonResponse(429, "Too Many Requests",
      "{\"error\":\"RATE_LIMITED\",\"detail\":"
      "\"Upstream data is temporarily busy; retained data remains available.\"}");
  response.content_type = "application/json; charset=utf-8";
  const uint32_t seconds = (retry_after_ms + 999) / 1000;
  response.retry_after = seconds < 1 ? 1 : seconds;
  return response;
}

http::HttpResponse DiscoveryUpstreamUnavailableResponse() {
  http::HttpResponse response = http::JsonResponse(502, "Bad Gateway",
                                       "{\"error\":\"UPSTREAM_UNAVAILABLE\"}");
  response.content_type = "application/json; charset=utf-8";
  return response;
}

void AppendDiscoveryCacheJson(fortis::codec::json_document_writer::OutBuffer& out, bool hit, int64_t as_of_ms,
                              bool partial_key, bool partial) {
  out.Text(hit ? "{\"hit\":true,\"timestamp\":" : "{\"hit\":false,\"timestamp\":");
  char timestamp[32];
  if (as_of_ms >= 0 && DiscoveryIsoTimestamp(as_of_ms, timestamp)) {
    out.Byte('"'); out.Text(timestamp); out.Byte('"');
  } else {
    out.Text("null");
  }
  if (partial_key) out.Text(partial ? ",\"partial\":true" : ",\"partial\":false");
  out.Byte('}');
}

void AppendDiscoveryCacheProto(fortis::codec::json_document_writer::OutBuffer& out, bool hit, int64_t as_of_ms,
                               bool partial) {
  if (hit) { fortis::codec::proto_wire::PutTag(out, 1, 0); out.Byte(1); }
  char timestamp[32];
  if (as_of_ms >= 0 && DiscoveryIsoTimestamp(as_of_ms, timestamp))
    fortis::codec::proto_wire::PutBytes(out, 2, timestamp, strlen(timestamp));
  if (partial) { fortis::codec::proto_wire::PutTag(out, 3, 0); out.Byte(1); }
}

bool DiscoveryUpsertMember(DiscoveryMember* members, size_t* count,
                           const DiscoveryMember& member) {
  for (size_t i = 0; i < *count; ++i) {
    if (members[i].key_length == member.key_length &&
        memcmp(members[i].key, member.key, member.key_length) == 0) {
      const uint8_t* key = members[i].key;
      members[i] = member;
      members[i].key = key;
      return true;
    }
  }
  if (*count >= kDiscoveryMaxMembers) return false;
  members[(*count)++] = member;
  return true;
}

size_t DiscoveryUtf16Length(const uint8_t* text, size_t length) {
  return fortis::codec::history_batch::Utf16Units(reinterpret_cast<const char*>(text), length);
}

}  // namespace fortis::discovery
