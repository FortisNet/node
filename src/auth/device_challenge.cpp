#include "device_challenge.h"
#include "device_auth.h"
#include "http_server.h"
#include "kv_runtime.h"
#include <pb_encode.h>
#include "fortisnet/intelligence/v1/devices.pb.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace fortis::auth {

namespace {
constexpr int64_t kChallengeTtlMs = fortis::auth::kDeviceChallengeTtlMs;
constexpr char kBase64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

bool now_ms(int64_t* value) {
  timespec now{};
  if (!value || clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec < 0 ||
      now.tv_sec > (INT64_MAX - now.tv_nsec / 1000000) / 1000) return false;
  *value = static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
  return true;
}

bool mint_challenge(char challenge[44]) {
  uint8_t random[32];
  if (!fortis::auth::SecureRandomBytes(random, sizeof(random))) return false;
  size_t out = 0;
  for (size_t i = 0; i < 30; i += 3) {
    challenge[out++] = kBase64Url[random[i] >> 2];
    challenge[out++] = kBase64Url[((random[i] & 3) << 4) | (random[i + 1] >> 4)];
    challenge[out++] = kBase64Url[((random[i + 1] & 15) << 2) | (random[i + 2] >> 6)];
    challenge[out++] = kBase64Url[random[i + 2] & 63];
  }
  challenge[out++] = kBase64Url[random[30] >> 2];
  challenge[out++] = kBase64Url[((random[30] & 3) << 4) | (random[31] >> 4)];
  challenge[out++] = kBase64Url[(random[31] & 15) << 2];
  challenge[out] = '\0';
  return out == 43;
}

void send_json(int client, const char* content_type, const char* body, bool vary = false) {
  http::HttpResponse response{200, "OK", content_type,
      reinterpret_cast<const uint8_t*>(body), strlen(body)};
  response.vary_accept = vary;
  WriteHttpResponse(client, response);
}

http::HttpResponse device_error(int status, const char* reason, const char* body) {
  return {status, reason, "application/json; charset=utf-8",
      reinterpret_cast<const uint8_t*>(body), strlen(body)};
}

void handle_challenge(int client, const http::HttpRequest& request, bool, void*) {
  if (request.origin) {
    WriteHttpResponse(client, device_error(403, "Forbidden",
        "{\"error\":\"BROWSER_ORIGIN_FORBIDDEN\"}"));
    return;
  }
  int64_t now = 0;
  char key[128];
  uint64_t hits = 0;
  bool quota_ok = now_ms(&now) && request.remote_ip[0] &&
      snprintf(key, sizeof(key), "q:ip:%s:%lld", request.remote_ip,
               static_cast<long long>(now / 60000)) > 0 &&
      fortis::storage::RuntimeKvIncrementWindow(key, now, 60000, &hits);
  // Match the TypeScript quota's deliberate fail-open behavior on KV errors.
  if (quota_ok && hits > 10) {
    http::HttpResponse limited = device_error(429, "Too Many Requests", "{\"error\":\"RATE_LIMITED\"}");
    limited.retry_after = 60;
    WriteHttpResponse(client, limited);
    return;
  }
  char challenge[44];
  if (!mint_challenge(challenge) || !now_ms(&now) || !fortis::storage::RuntimeKvStore()) {
    WriteHttpResponse(client, device_error(500, "Internal Server Error", "{\"error\":\"INTERNAL\"}"));
    return;
  }
  if (!fortis::auth::PersistDeviceChallenge(fortis::storage::RuntimeKvStore(), challenge, now)) {
    WriteHttpResponse(client, device_error(500, "Internal Server Error", "{\"error\":\"INTERNAL\"}"));
    return;
  }
  if (AcceptsProtobuf(request)) {
    fortisnet_intelligence_v1_AttestationChallengeResponse message =
        fortisnet_intelligence_v1_AttestationChallengeResponse_init_zero;
    memcpy(message.challenge, challenge, sizeof(challenge));
    message.expires_in = 300;
    uint8_t encoded[64];
    pb_ostream_t stream = pb_ostream_from_buffer(encoded, sizeof(encoded));
    if (!pb_encode(&stream, fortisnet_intelligence_v1_AttestationChallengeResponse_fields, &message)) {
      WriteHttpResponse(client, device_error(500, "Internal Server Error", "{\"error\":\"INTERNAL\"}"));
      return;
    }
    http::HttpResponse response{200, "OK", "application/protobuf", encoded, stream.bytes_written};
    response.vary_accept = true;
    WriteHttpResponse(client, response);
  } else {
    char body[96];
    snprintf(body, sizeof(body), "{\"challenge\":\"%s\",\"expiresIn\":300}", challenge);
    send_json(client, "application/json; charset=utf-8", body, true);
  }
}
}

bool PersistDeviceChallenge(storage::KvStore* kv, const char* challenge, int64_t now) {
  if (!kv || !challenge || strlen(challenge) != 43 || now < 0 ||
      now > INT64_MAX - kChallengeTtlMs) return false;
  for (size_t i = 0; i < 43; ++i)
    if (!strchr(kBase64Url, challenge[i])) return false;
  char key[96];
  if (!DeviceChallengeKey(challenge, 43, key, sizeof(key))) return false;
  // EnrollDevelopmentWithChallenge and EnrollAttested expect this marker.
  const char value = '1';
  const storage::KvMutation mutation{key, &value, 1, now + kChallengeTtlMs, false};
  return kv->Write(&mutation, 1);
}
}

namespace fortis::auth {

bool RegisterDeviceChallengeRoute(http::HttpRouter& router) {
  return router.Add("POST", "/v1/devices/challenge", handle_challenge);
}

}  // namespace fortis::auth
