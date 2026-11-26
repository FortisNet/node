#include "test_support.h"
#include "../src/auth/device_enrollment.h"
#include "../src/auth/device_challenge.h"
#include "../src/http/http_server.h"
#include "../src/storage/kv_runtime.h"
#include <assert.h>
#include <atomic>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <vector>

using namespace fortis::auth;
using namespace fortis::http;

namespace {

// One request through HttpRouter::Dispatch over a socketpair, as an HTTP
// worker runs it, from `remote_ip`.
std::string dispatch(HttpRouter& router, const std::string& raw, const char* remote_ip) {
  int pair[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  assert(send(pair[1], raw.data(), raw.size(), 0) == static_cast<ssize_t>(raw.size()));
  shutdown(pair[1], SHUT_WR);
  router.Dispatch(pair[0], remote_ip);
  close(pair[0]);
  std::string response;
  char bytes[1024];
  for (;;) {
    const ssize_t count = recv(pair[1], bytes, sizeof(bytes), 0);
    if (count == 0) break;
    assert(count > 0);
    response.append(bytes, static_cast<size_t>(count));
  }
  close(pair[1]);
  return response;
}

std::string post(const char* path, const std::string& body, const char* extra = "",
                 const char* type = "application/json") {
  return std::string("POST ") + path + " HTTP/1.1\r\nHost: localhost\r\nContent-Type: " + type +
         "\r\n" + extra + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

std::string body_of(const std::string& response) {
  const size_t split = response.find("\r\n\r\n");
  assert(split != std::string::npos);
  return response.substr(split + 4);
}

int status_of(const std::string& response) {
  assert(response.compare(0, 9, "HTTP/1.1 ") == 0);
  return fortis::test::ParseInteger<int>(response.c_str() + 9);
}

std::string json_field(const std::string& body, const char* name) {
  const std::string key = std::string("\"") + name + "\":\"";
  const size_t start = body.find(key);
  assert(start != std::string::npos);
  const size_t end = body.find('"', start + key.size());
  return body.substr(start + key.size(), end - start - key.size());
}

std::string mint(HttpRouter& router, const char* ip) {
  const std::string response = dispatch(router,
      "POST /v1/devices/challenge HTTP/1.1\r\nHost: localhost\r\n\r\n", ip);
  assert(status_of(response) == 200);
  const std::string value = json_field(body_of(response), "challenge");
  assert(value.size() == 43);
  return value;
}

std::string enrollment_json(const char* platform, const std::string& challenge,
                            const char* provider, const char* token = nullptr) {
  std::string body = std::string("{\"platform\":\"") + platform + "\",\"appVersion\":\"1.4.2\"";
  if (provider) {
    body += std::string(",\"attestation\":{\"provider\":\"") + provider +
            "\",\"challenge\":\"" + challenge + "\"";
    if (token) body += std::string(",\"token\":\"") + token + "\"";
    body += "}";
  }
  return body + "}";
}

std::string field(int tag, const std::string& data) {
  assert(data.size() < 128);
  return std::string(1, static_cast<char>(tag << 3 | 2)) +
         std::string(1, static_cast<char>(data.size())) + data;
}

std::string protobuf_development(const std::string& challenge) {
  return field(1, "android") + field(2, "1.4.2") +
         field(3, field(1, "development") + field(2, challenge));
}

// Dispatches every request on its own thread, released together, the way
// concurrent HTTP workers run POST /v1/devices without the route lock.
struct Contender {
  HttpRouter* router = nullptr;
  std::string request;
  const char* ip = nullptr;
  std::atomic<bool>* go = nullptr;
  std::string response;
};

void* contend(void* opaque) {
  auto* contender = static_cast<Contender*>(opaque);
  while (!contender->go->load()) {}
  contender->response = dispatch(*contender->router, contender->request, contender->ip);
  return nullptr;
}

void run_together(std::vector<Contender>& contenders) {
  std::atomic<bool> go{false};
  std::vector<pthread_t> threads(contenders.size());
  for (size_t i = 0; i < contenders.size(); ++i) {
    contenders[i].go = &go;
    assert(pthread_create(&threads[i], nullptr, contend, &contenders[i]) == 0);
  }
  go.store(true);
  for (pthread_t thread : threads) pthread_join(thread, nullptr);
}

int64_t minute_now() {
  timespec now{};
  clock_gettime(CLOCK_REALTIME, &now);
  return static_cast<int64_t>(now.tv_sec) / 60;
}

// Concurrent development enrollments of one challenge (JSON and protobuf):
// one 201 in the requested representation, every other 401
// ATTESTATION_EXPIRED; the winner's bearer then rotates.
void test_one_challenge_race(HttpRouter& router) {
  const char* const ips[] = {"10.20.0.1", "10.20.0.2", "10.20.0.3", "10.20.0.4"};
  for (const char* ip : ips) {
    const std::string value = mint(router, ip);
    std::vector<Contender> contenders(9);
    for (size_t i = 0; i < contenders.size(); ++i) {
      contenders[i].router = &router;
      contenders[i].ip = ip;
      contenders[i].request = i % 2
          ? post("/v1/devices", protobuf_development(value), "Accept: application/protobuf\r\n",
                 "application/protobuf")
          : post("/v1/devices", enrollment_json("android", value, "development"));
    }
    run_together(contenders);
    size_t winners = 0;
    std::string bearer;
    for (size_t i = 0; i < contenders.size(); ++i) {
      const std::string& response = contenders[i].response;
      if (status_of(response) == 201) {
        ++winners;
        assert(response.find("Vary: Accept\r\n") != std::string::npos);
        const std::string body = body_of(response);
        if (i % 2) {
          assert(response.find("Content-Type: application/protobuf\r\n") != std::string::npos);
          assert(body.size() == 110 && body.compare(30, 11, "fintel_dev_") == 0);
          bearer = body.substr(30, 54);
        } else {
          bearer = json_field(body, "deviceToken");
        }
        continue;
      }
      assert(status_of(response) == 401);
      assert(body_of(response) == "{\"error\":\"ATTESTATION_EXPIRED\"}");
    }
    assert(winners == 1 && bearer.size() == 54);
    const std::string rotated = dispatch(router, "POST /v1/devices/rotate HTTP/1.1\r\n"
        "Host: localhost\r\nAuthorization: Bearer " + bearer + "\r\n\r\n", ip);
    assert(status_of(rotated) == 200);
  }
  puts("concurrent development enrollments of one challenge: one 201 per round");
}

// The per-IP quota (10 per minute, shared with the challenge route) stays
// exact under concurrency; Origin refusals come first and are not counted.
void test_quota_and_origin(HttpRouter& router) {
  for (int attempt = 0;; ++attempt) {
    assert(attempt < 3);
    char ip[32];
    snprintf(ip, sizeof(ip), "10.21.0.%d", attempt + 1);
    const int64_t minute = minute_now();
    std::vector<Contender> contenders(17);
    for (size_t i = 0; i < contenders.size(); ++i) {
      contenders[i].router = &router;
      contenders[i].ip = strdup(ip);
      contenders[i].request = post("/v1/devices", enrollment_json("linux", "", nullptr),
                                   i < 3 ? "Origin: https://example.com\r\n" : "");
    }
    run_together(contenders);
    size_t created = 0, limited = 0, forbidden = 0;
    for (size_t i = 0; i < contenders.size(); ++i) {
      const std::string& response = contenders[i].response;
      free(const_cast<char*>(contenders[i].ip));
      switch (status_of(response)) {
        case 201: ++created; break;
        case 429:
          assert(body_of(response) == "{\"error\":\"RATE_LIMITED\"}");
          assert(response.find("Retry-After: 60\r\n") != std::string::npos);
          ++limited; break;
        case 403:
          assert(i < 3 && body_of(response) == "{\"error\":\"BROWSER_ORIGIN_FORBIDDEN\"}");
          ++forbidden; break;
        default: assert(false);
      }
    }
    // A window that rolled over mid-round splits the count; run it again.
    if (minute != minute_now()) continue;
    assert(created == 10 && limited == 4 && forbidden == 3);
    break;
  }
  puts("concurrent enrollment quota: 10 created, 4 RATE_LIMITED, 3 Origin refusals uncounted");
}

// Required mode: concurrent refusals on every provider branch; App Attest
// may also answer BUSY (two verification slots); the challenge survives.
void test_required_refusals(HttpRouter& router) {
  const char* ip = "10.22.0.1";
  const std::string value = mint(router, ip);
  std::vector<Contender> contenders(8);
  for (size_t i = 0; i < contenders.size(); ++i) {
    contenders[i].router = &router;
    contenders[i].ip = ip;
    contenders[i].request = post("/v1/devices",
        i < 3 ? enrollment_json("ios", value, "apple-app-attest", "AAAAAAAAAAAAAAAAAAAAAAAA")
        : i < 6 ? enrollment_json("android", value, "play-integrity", "AAAAAAAAAAAAAAAAAAAAAAAA")
                : enrollment_json("android", value, "development"));
  }
  run_together(contenders);
  for (size_t i = 0; i < contenders.size(); ++i) {
    const std::string& response = contenders[i].response;
    const std::string body = body_of(response);
    if (i < 3) {
      assert((status_of(response) == 401 && body == "{\"error\":\"ATTESTATION_INVALID\"}") ||
             (status_of(response) == 503 && body == "{\"error\":\"ATTESTATION_BUSY\"}" &&
              response.find("Retry-After: 1\r\n") != std::string::npos));
    } else {
      // This binary has no Play configuration: the unconfigured answer.
      assert(status_of(response) == 401 && body == "{\"error\":\"ATTESTATION_REQUIRED\"}");
    }
  }
  fortis::storage::KvStore* kv = fortis::storage::RuntimeKvStore();
  assert(kv);
  timespec now{};
  clock_gettime(CLOCK_REALTIME, &now);
  assert(fortis::auth::DeviceAuthStore(kv).PeekChallenge(value.c_str(), value.size(),
      static_cast<int64_t>(now.tv_sec) * 1000) == fortis::storage::KvReadStatus::Found);
  puts("concurrent required-mode refusals leave the challenge live");
}

}  // namespace

int main() try {
  const char* allowed[] = {"127.0.0.1", "::1", "10.1.2.3", "172.16.0.1",
      "172.31.255.254", "192.168.1.2", "fc00::1", "fd12::1", "fe80::1",
      "::ffff:192.168.1.2"};
  for (const char* address : allowed) assert(fortis::auth::IsDevelopmentAddress(address));
  const char* denied[] = {"8.8.8.8", "172.32.0.1", "192.167.1.1",
      "2001:4860:4860::8888", "fe90::1", "not-an-ip", ""};
  for (const char* address : denied) assert(!fortis::auth::IsDevelopmentAddress(address));
  puts("development enrollment address policy test passed");

  char directory[] = "/tmp/fortis-enrollment-route-XXXXXX";
  assert(mkdtemp(directory));
  fortis::storage::RuntimeKvSetDirectory(directory);
  assert(fortis::storage::RuntimeKvStore());
  DeviceEnrollmentConfig development{};
  HttpRouter router;
  assert(RegisterDeviceChallengeRoute(router) && RegisterDeviceEnrollmentRoute(router, &development) &&
         RegisterDeviceRotationRoute(router, &development));
  test_one_challenge_race(router);
  test_quota_and_origin(router);
  DeviceEnrollmentConfig required{};
  required.attestation_required = true;
  HttpRouter required_router;
  assert(RegisterDeviceChallengeRoute(required_router) &&
         RegisterDeviceEnrollmentRoute(required_router, &required));
  test_required_refusals(required_router);
  fortis::test::RemoveTree(directory);
  puts("device enrollment route concurrency tests passed");
} catch (...) {
  return fortis::test::ReportException();
}
