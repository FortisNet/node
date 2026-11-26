#include <algorithm>
#include "test_support.h"
// Play Integrity: required-mode Android enrollment units. Built with
// FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT against the _test_seam objects, the
// real device_auth and the fault-injecting KV store.
//
//   play_integrity_enrollment_test unit FIXTURE_DIR
//
// FIXTURE_DIR is made by tests/play_integrity_enrollment_socket_test.py with
// openssl at test time (no key is stored in Git): rsa2048.pem (PKCS#8) and
// ca.pem. Groups:
//   config     LoadPlayIntegrityConfig: presence, every variable's syntax and
//              bounds, defaults, CA fallback, no value in any message
//   initialize InitializePlayIntegrityClient: CA, credential file refusals,
//              double initialization
//   runtime    Start/RuntimePlayIntegrityClient/Stop, seam env pairing
//   enroll     the Verified path: record, auth in required mode, consumed
//              challenge, exactly one OAuth exchange and decode
//   pre-I/O    refusals before any KV read or Google request
//   verdict    every refusal keeps the challenge live; a new token wins
//   upstream   Google/OAuth/transport failures map to Busy/Unavailable/
//              InvalidProof and keep the challenge live
//   clock/ttl  post-decode time, expiry during decode, TTL bounds
//   storage    an unacknowledged enrollment frame is StorageError
//   race       concurrent decodes of one challenge enroll exactly one device
//   restart    reopened store: device authenticates, challenge spent
//   admission  the route's PlayEnrollRouteSlot: three slots, a refusal never
//              waits or releases, every enrollment outcome returns its slot,
//              no over-admission under contention
//
// The client under test is a local PlayIntegrityGoogleClient with an
// injected transport; no network is used.

#include "../src/auth/play_integrity_enrollment.h"
#include "fake_seams.h"

#include "../src/auth/device_auth.h"
#include "../src/auth/device_challenge.h"
#include "../src/storage/kv_store.h"
#include "../src/auth/play_integrity_google.h"

#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace fortis::auth;
using namespace fortis::http;
using namespace fortis::storage;

#ifndef FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT
#error "built with the test transport only"
#endif

using namespace fortis;

static_assert(!std::is_default_constructible<VerifiedDeviceAttestation>::value,
              "only the friend issuers construct a verified proof");
static_assert(!std::is_constructible<PlayIntegrityDecodedResponse, const uint8_t*, size_t>::value,
              "only PlayIntegrityDecodeSource wraps a decoded response");

#define CHECK(condition)                                                            \
  do {                                                                              \
    if (!(condition)) {                                                             \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
      abort();                                                                      \
    }                                                                               \
  } while (0)

#define CHECK_ENROLL(actual, expected)                                                 \
  do {                                                                                 \
    const PlayEnrollOutcome got_ = (actual);                                           \
    if (got_.status != (expected)) {                                                   \
      fprintf(stderr, "%s:%d: got %s (decode %s, verdict %s), expected %s\n", __FILE__, \
              __LINE__, PlayEnrollStatusName(got_.status), got_.decode,                \
              PlayIntegrityStatusName(got_.verdict), PlayEnrollStatusName(expected));  \
      abort();                                                                         \
    }                                                                                  \
  } while (0)

namespace {

constexpr char kAppVersion[] = "1.4.2";
constexpr char kEmail[] = "play-verifier@fortis-test.iam.gserviceaccount.com";
constexpr char kKeyId[] = "0123456789abcdef0123456789abcdef01234567";
constexpr int64_t kTtl = 86400000;
constexpr int64_t kMinVersion = 40;

std::string g_fixture, g_scratch, g_credential, g_ca;
uint8_t g_certificate[32];
std::string g_certificate_b64;

int64_t Now() {
  timespec now{};
  CHECK(clock_gettime(CLOCK_REALTIME, &now) == 0);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

// ---- Files and encodings ---------------------------------------------------

std::string ReadFile(const std::string& path) {
  FILE* file = fopen(path.c_str(), "rb");
  CHECK(file);
  std::string data;
  char buffer[4096];
  size_t count;
  while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) data.append(buffer, count);
  fclose(file);
  return data;
}

std::string WriteFile(const std::string& name, const std::string& data, mode_t mode = 0600) {
  const std::string path = g_scratch + "/" + name;
  unlink(path.c_str());
  FILE* file = fopen(path.c_str(), "wb");
  CHECK(file);
  CHECK(fwrite(data.data(), 1, data.size(), file) == data.size());
  CHECK(fclose(file) == 0);
  CHECK(chmod(path.c_str(), mode) == 0);
  return path;
}

std::string JsonString(const std::string& text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '\n') out += "\\n";
    else if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else out += c;
  }
  return out + "\"";
}

std::string Credential() {
  return "{\"type\":\"service_account\",\"project_id\":\"fortis-test\",\"private_key_id\":" +
         JsonString(kKeyId) + ",\"private_key\":" +
         JsonString(ReadFile(g_fixture + "/rsa2048.pem")) + ",\"client_email\":" +
         JsonString(kEmail) +
         ",\"token_uri\":\"https://oauth2.googleapis.com/token\","
         "\"universe_domain\":\"googleapis.com\"}";
}

std::string B64(const uint8_t* bytes, size_t length) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  uint32_t buffer = 0;
  int bits = 0;
  for (size_t i = 0; i < length; ++i) {
    buffer = (buffer << 8) | bytes[i];
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out += alphabet[(buffer >> bits) & 63];
    }
  }
  if (bits) out += alphabet[(buffer << (6 - bits)) & 63];
  return out;
}

std::string Hex(const uint8_t* bytes, size_t length, bool colons, bool upper) {
  const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < length; ++i) {
    if (colons && i) out += ':';
    out += digits[bytes[i] >> 4];
    out += digits[bytes[i] & 15];
  }
  return out;
}

// A fresh server-shaped challenge: 32 random bytes, canonical base64url.
std::string RandomChallenge() {
  uint8_t bytes[32];
  CHECK(SecureRandomBytes(bytes, sizeof(bytes)));
  return B64(bytes, sizeof(bytes));
}

std::atomic<unsigned> g_token_counter{0};
// A unique standard-token-shaped string (the token alphabet has no '_').
std::string FreshToken(const char* tag = "ok") {
  return std::string("eyJhbGciOiJBMjU2S1ciLCJlbmMiOiJBMjU2R0NNIn0.c3-") + tag + "-" +
         std::to_string(++g_token_counter) + ".iv.cipher.tag";
}

// ---- Verdicts ---------------------------------------------------------------

std::string Hash(const std::string& challenge, const std::string& app_version) {
  char output[kPlayRequestHashChars + 1];
  CHECK(ComputePlayIntegrityRequestHash(challenge.data(), challenge.size(), "android", 7,
                                        kPlayIntegrityPackageName,
                                        strlen(kPlayIntegrityPackageName), app_version.data(),
                                        app_version.size(), output) == PlayRequestHashStatus::Ok);
  return output;
}

struct VerdictSpec {
  std::string hash;
  int64_t timestamp_ms = 0;  // 0: one second before now
  std::string package = kPlayIntegrityPackageName;
  std::string certificate = g_certificate_b64;
  std::string version_code = "42";
  std::string app_verdict = "PLAY_RECOGNIZED";
  std::string device = "\"MEETS_DEVICE_INTEGRITY\"";
  bool testing = false;
};

VerdictSpec Bound(const std::string& challenge, const std::string& version = kAppVersion) {
  VerdictSpec spec;
  spec.hash = Hash(challenge, version);
  return spec;
}

std::string Verdict(const VerdictSpec& spec) {
  const int64_t timestamp = spec.timestamp_ms ? spec.timestamp_ms : Now() - 1000;
  return "{\"tokenPayloadExternal\":{\"requestDetails\":{\"requestPackageName\":\"" +
         spec.package + "\",\"requestHash\":\"" + spec.hash + "\",\"timestampMillis\":\"" +
         std::to_string(timestamp) + "\"},\"appIntegrity\":{\"appRecognitionVerdict\":\"" +
         spec.app_verdict + "\",\"packageName\":\"" + spec.package +
         "\",\"certificateSha256Digest\":[\"" + spec.certificate + "\"],\"versionCode\":\"" +
         spec.version_code + "\"},\"deviceIntegrity\":{\"deviceRecognitionVerdict\":[" +
         spec.device + "]},\"accountDetails\":{\"appLicensingVerdict\":\"LICENSED\"}" +
         (spec.testing ? ",\"testingDetails\":{\"isTestingResponse\":true}" : "") + "}}";
}

// ---- Injected Google transport ----------------------------------------------

struct Reply {
  OutboundError error = OutboundError::kNone;
  int status = 200;
  std::string content_type = "application/json; charset=UTF-8";
  std::string body;
};

struct Fake {
  std::mutex mutex;
  std::vector<std::string> decoded_tokens;
  size_t oauth = 0;
  std::function<Reply()> on_oauth;
  // Given the integrity token from the decode body.
  std::function<Reply(const std::string& token)> on_decode;
};

OutboundResponse FakeTransport(void* context, const OutboundRequest& request,
                               unsigned char* body, size_t capacity) {
  Fake* fake = static_cast<Fake*>(context);
  const bool is_oauth = strcmp(request.host, kGoogleOAuthHost) == 0;
  Reply reply;
  if (is_oauth) {
    CHECK(strcmp(request.path, kGoogleOAuthPath) == 0 && !request.authorization_bearer);
    {
      std::lock_guard<std::mutex> lock(fake->mutex);
      ++fake->oauth;
    }
    reply = fake->on_oauth ? fake->on_oauth()
                           : Reply{OutboundError::kNone, 200, "application/json",
                                   "{\"access_token\":\"ya29.c3-unit\",\"expires_in\":3599,"
                                   "\"token_type\":\"Bearer\"}"};
  } else {
    CHECK(strcmp(request.host, kPlayIntegrityApiHost) == 0);
    CHECK(strcmp(request.path, kPlayIntegrityDecodePath) == 0);
    CHECK(request.authorization_bearer && strcmp(request.authorization_bearer, "ya29.c3-unit") == 0);
    static const char prefix[] = "{\"integrity_token\":\"";
    const std::string sent(reinterpret_cast<const char*>(request.body), request.body_length);
    CHECK(sent.compare(0, sizeof(prefix) - 1, prefix) == 0 && sent.size() > sizeof(prefix) + 1);
    const std::string token = sent.substr(sizeof(prefix) - 1, sent.size() - sizeof(prefix) - 1);
    {
      std::lock_guard<std::mutex> lock(fake->mutex);
      fake->decoded_tokens.push_back(token);
    }
    reply = fake->on_decode(token);
  }
  OutboundResponse response{};
  response.error = reply.error;
  if (reply.error != OutboundError::kNone) return response;
  response.status = reply.status;
  if (!reply.content_type.empty()) {
    response.has_content_type = true;
    snprintf(response.content_type, sizeof(response.content_type), "%s",
             reply.content_type.c_str());
  }
  CHECK(reply.body.size() <= capacity && reply.body.size() <= request.max_response_bytes);
  std::copy_n(reply.body.data(), reply.body.size(), body);
  response.body_length = reply.body.size();
  return response;
}

PlayIntegrityRuntimeConfig RuntimeConfig() {
  PlayIntegrityRuntimeConfig config;
  config.configured = true;
  snprintf(config.credential_file, sizeof(config.credential_file), "%s", g_credential.c_str());
  snprintf(config.ca_file, sizeof(config.ca_file), "%s", g_ca.c_str());
  memcpy(config.certificates[0], g_certificate, 32);
  config.certificate_count = 1;
  config.min_version_code = kMinVersion;
  return config;
}

// One KV directory, one store, one Google client over the fake.
struct Harness {
  std::string directory;
  KvStore store;
  Fake fake;
  PlayIntegrityGoogleClient client;

  bool owned = true;

  // A fresh KV directory, or (`reuse`) an existing one that outlives this
  // harness: a restarted process with an empty replay ring and token cache.
  explicit Harness(const char* name, const std::string& reuse = std::string()) {
    owned = reuse.empty();
    directory = owned ? g_scratch + "/kv-" + name : reuse;
    if (owned) {
      fortis::test::RemoveTree(directory);
      CHECK(mkdir(directory.c_str(), 0700) == 0);
    }
    CHECK(store.Open(directory.c_str()));
    static const uint8_t (*certificates)[32] = nullptr;
    static uint8_t list[1][32];
    memcpy(list[0], g_certificate, 32);
    certificates = list;
    PlayIntegrityGoogleConfig config;
    config.credential_path = g_credential.c_str();
    config.ca_file = g_ca.c_str();
    config.policy.certificates = certificates;
    config.policy.certificate_count = 1;
    config.policy.min_version_code = kMinVersion;
    config.test.transport = FakeTransport;
    config.test.transport_context = &fake;
    ServiceAccountStatus credential = ServiceAccountStatus::NotConfigured;
    CHECK(client.Initialize(config, &credential) && credential == ServiceAccountStatus::Ok);
  }
  ~Harness() {
    store.Close();
    if (owned) fortis::test::RemoveTree(directory);
  }
  std::string Mint(int64_t now = Now()) {
    const std::string challenge = RandomChallenge();
    CHECK(PersistDeviceChallenge(&store, challenge.c_str(), now));
    return challenge;
  }
  bool Live(const std::string& challenge, int64_t now = Now()) {
    DeviceAuthStore auth(&store);
    return auth.PeekChallenge(challenge.data(), challenge.size(), now) == KvReadStatus::Found;
  }
  size_t Devices() {
    size_t count = 0;
    CHECK(store.Scan("device/id/", nullptr, SIZE_MAX, Now(), [context = &count](const char*, const void*, size_t) {
                       ++*static_cast<size_t*>(context);
                       return true;
                     }));
    return count;
  }
  size_t Decodes() {
    std::lock_guard<std::mutex> lock(fake.mutex);
    return fake.decoded_tokens.size();
  }
  size_t OAuth() {
    std::lock_guard<std::mutex> lock(fake.mutex);
    return fake.oauth;
  }
  // Answers every decode with a verdict bound to `challenge`.
  void Verify(const std::string& challenge, const std::string& version = kAppVersion) {
    fake.on_decode = [spec = Bound(challenge, version)](const std::string&) {
      return Reply{OutboundError::kNone, 200, "application/json", Verdict(spec)};
    };
  }
};

struct Call {
  std::string challenge;
  std::string token = FreshToken();
  const char* app_version = kAppVersion;
  size_t app_version_length = sizeof(kAppVersion) - 1;
  int64_t now_ms = 0;  // 0: Now()
  int64_t ttl_ms = kTtl;
  fortis::base::Clock* clock = nullptr;
  bool token_present = true;
};

PlayEnrollOutcome Enroll(Harness& harness, const Call& call, DeviceCredentials* output,
                         PlayIntegrityGoogleClient* client = nullptr) {
  DeviceAuthStore auth(&harness.store);
  PlayEnrollRequest request;
  request.challenge = call.challenge.data();
  request.challenge_length = call.challenge.size();
  request.integrity_token = call.token_present ? call.token.data() : nullptr;
  request.integrity_token_length = call.token_present ? call.token.size() : 0;
  request.app_version = call.app_version;
  request.app_version_length = call.app_version ? call.app_version_length : 0;
  request.now_ms = call.now_ms ? call.now_ms : Now();
  request.ttl_ms = call.ttl_ms;
  request.clock = call.clock;
  return EnrollWithPlayIntegrity(client ? client : &harness.client, auth, request, output);
}

PlayEnrollOutcome Enroll(Harness& harness, const Call& call) {
  DeviceCredentials credentials{};
  return Enroll(harness, call, &credentials);
}

Call For(const std::string& challenge) {
  Call call;
  call.challenge = challenge;
  return call;
}

// ---- Environment fake -----------------------------------------------------------

using Env = std::map<std::string, std::string>;
const char* EnvLookup(void* context, const char* name) {
  const Env* env = static_cast<const Env*>(context);
  const auto found = env->find(name);
  return found == env->end() ? nullptr : found->second.c_str();
}

Env ValidEnv() {
  return {{"FORTIS_PLAY_INTEGRITY_CREDENTIAL_FILE", g_credential},
          {"FORTIS_PLAY_INTEGRITY_CERT_SHA256", Hex(g_certificate, 32, true, true)},
          {"FORTIS_PLAY_INTEGRITY_CERT_SHA256_FORMAT", "hex-colon"},
          {"FORTIS_PLAY_INTEGRITY_MIN_VERSION_CODE", "40"}};
}

bool Load(const Env& env, PlayIntegrityRuntimeConfig* config, std::string* error,
          const char* fallback = "/etc/ssl/fallback.pem") {
  char message[512];
  memset(message, 'x', sizeof(message));
  const bool ok = LoadPlayIntegrityConfig(EnvLookup, const_cast<Env*>(&env), fallback, config,
                                          message, sizeof(message));
  CHECK(memchr(message, '\0', sizeof(message)));
  *error = message;
  return ok;
}

// A refused config names `variable` and leaves nothing configured.
void ExpectRefused(const Env& env, const char* variable, const char* fallback = "/etc/ssl/f.pem") {
  PlayIntegrityRuntimeConfig config;
  std::string error;
  if (Load(env, &config, &error, fallback)) {
    fprintf(stderr, "accepted a config that should name %s\n", variable);
    abort();
  }
  if (error.find(variable) == std::string::npos) {
    fprintf(stderr, "error \"%s\" does not name %s\n", error.c_str(), variable);
    abort();
  }
  CHECK(!config.configured && config.certificate_count == 0 && config.credential_file[0] == '\0');
}

Env With(Env env, const char* name, const std::string& value) {
  env[name] = value;
  return env;
}

Env Without(Env env, const char* name) {
  env.erase(name);
  return env;
}

// ---- Groups -----------------------------------------------------------------------

void TestConfig() {
  PlayIntegrityRuntimeConfig config;
  std::string error;
  // Nothing supplied: Android stays off, no error.
  CHECK(Load(Env{}, &config, &error) && !config.configured && error.empty());
  CHECK(Load(Env{{"FORTIS_CA_BUNDLE_PATH", "/x"}, {"FORTIS_PLAY_INTEGRITY", "1"}}, &config,
             &error) && !config.configured);
  // A complete set, defaults and the CA fallback.
  CHECK(Load(ValidEnv(), &config, &error, "/etc/ssl/fallback.pem") && config.configured);
  CHECK(strcmp(config.credential_file, g_credential.c_str()) == 0);
  CHECK(strcmp(config.ca_file, "/etc/ssl/fallback.pem") == 0);
  CHECK(config.certificate_count == 1 && memcmp(config.certificates[0], g_certificate, 32) == 0);
  CHECK(config.min_version_code == 40 && config.max_age_ms == 300000 &&
        config.max_future_skew_ms == 60000);
  // The three digest forms parse to the same bytes.
  const std::pair<const char*, std::string> forms[] = {
      {"hex-colon", Hex(g_certificate, 32, true, false)},
      {"hex", Hex(g_certificate, 32, false, true)},
      {"hex", Hex(g_certificate, 32, false, false)},
      {"base64url", g_certificate_b64}};
  for (const auto& form : forms) {
    Env env = With(With(ValidEnv(), "FORTIS_PLAY_INTEGRITY_CERT_SHA256_FORMAT", form.first),
                   "FORTIS_PLAY_INTEGRITY_CERT_SHA256", form.second);
    CHECK(Load(env, &config, &error) && config.certificate_count == 1 &&
          memcmp(config.certificates[0], g_certificate, 32) == 0);
  }
  // Four distinct digests; explicit CA, age and skew bounds.
  std::string four;
  uint8_t digests[4][32];
  for (int i = 0; i < 4; ++i) {
    memset(digests[i], 0x10 + i, 32);
    four += (i ? "," : "") + Hex(digests[i], 32, false, false);
  }
  Env full = With(With(With(With(With(ValidEnv(), "FORTIS_PLAY_INTEGRITY_CERT_SHA256", four),
                                 "FORTIS_PLAY_INTEGRITY_CERT_SHA256_FORMAT", "hex"),
                            "FORTIS_PLAY_INTEGRITY_CA_FILE", "/run/secrets/google-ca.pem"),
                       "FORTIS_PLAY_INTEGRITY_MAX_AGE_SECONDS", "3600"),
                  "FORTIS_PLAY_INTEGRITY_MAX_FUTURE_SKEW_SECONDS", "0");
  CHECK(Load(full, &config, &error, nullptr) && config.certificate_count == 4);
  for (int i = 0; i < 4; ++i) CHECK(memcmp(config.certificates[i], digests[i], 32) == 0);
  CHECK(strcmp(config.ca_file, "/run/secrets/google-ca.pem") == 0);
  CHECK(config.max_age_ms == 3600000 && config.max_future_skew_ms == 0);
  CHECK(Load(With(With(ValidEnv(), "FORTIS_PLAY_INTEGRITY_MAX_AGE_SECONDS", "1"),
                  "FORTIS_PLAY_INTEGRITY_MAX_FUTURE_SKEW_SECONDS", "300"), &config, &error) &&
        config.max_age_ms == 1000 && config.max_future_skew_ms == 300000);
  CHECK(Load(With(ValidEnv(), "FORTIS_PLAY_INTEGRITY_MIN_VERSION_CODE", "2100000000"), &config,
             &error) && config.min_version_code == 2100000000);
  CHECK(Load(With(ValidEnv(), "FORTIS_PLAY_INTEGRITY_MIN_VERSION_CODE", "1"), &config, &error));
  const std::string longest = "/" + std::string(kPlayConfigPathMaxBytes - 1, 'p');
  CHECK(Load(With(ValidEnv(), "FORTIS_PLAY_INTEGRITY_CREDENTIAL_FILE", longest), &config,
             &error) && strlen(config.credential_file) == kPlayConfigPathMaxBytes);

  // Supplied but empty is a mistake for every variable, never "off".
  const char* all[] = {"FORTIS_PLAY_INTEGRITY_CREDENTIAL_FILE",
                       "FORTIS_PLAY_INTEGRITY_CERT_SHA256",
                       "FORTIS_PLAY_INTEGRITY_CERT_SHA256_FORMAT",
                       "FORTIS_PLAY_INTEGRITY_MIN_VERSION_CODE",
                       "FORTIS_PLAY_INTEGRITY_CA_FILE",
                       "FORTIS_PLAY_INTEGRITY_MAX_AGE_SECONDS",
                       "FORTIS_PLAY_INTEGRITY_MAX_FUTURE_SKEW_SECONDS"};
  for (const char* name : all) {
    ExpectRefused(With(ValidEnv(), name, ""), name);
    ExpectRefused(Env{{name, ""}}, name);
  }
  // A partial set names the first missing required variable.
  for (const char* name : {"FORTIS_PLAY_INTEGRITY_CREDENTIAL_FILE",
                           "FORTIS_PLAY_INTEGRITY_CERT_SHA256",
                           "FORTIS_PLAY_INTEGRITY_CERT_SHA256_FORMAT",
                           "FORTIS_PLAY_INTEGRITY_MIN_VERSION_CODE"})
    ExpectRefused(Without(ValidEnv(), name), name);
  ExpectRefused(Env{{"FORTIS_PLAY_INTEGRITY_MAX_AGE_SECONDS", "300"}},
                "FORTIS_PLAY_INTEGRITY_CREDENTIAL_FILE");
  ExpectRefused(Env{{"FORTIS_PLAY_INTEGRITY_CA_FILE", "/ca.pem"}},
                "FORTIS_PLAY_INTEGRITY_CREDENTIAL_FILE");

  // The credential is a path, never key material.
  const char* const kSecret = "SECRET-SENTINEL-7f3a";
  const std::string pasted = std::string("{\"type\":\"service_account\",\"private_key\":\"") +
                             kSecret + "\"}";
  for (const std::string& bad : {pasted, std::string("relative/key.json"),
                                 std::string("./key.json"), std::string("/key\n.json"),
                                 std::string("/key\x7f.json"), "/" + std::string(1023, 'p')}) {
    PlayIntegrityRuntimeConfig refused;
    std::string message;
    CHECK(!Load(With(ValidEnv(), "FORTIS_PLAY_INTEGRITY_CREDENTIAL_FILE", bad), &refused,
                &message));
    CHECK(message.find("FORTIS_PLAY_INTEGRITY_CREDENTIAL_FILE") != std::string::npos);
    CHECK(message.find(kSecret) == std::string::npos && message.find(bad) == std::string::npos);
  }

  // The format is stated, never guessed.
  for (const char* bad : {"HEX", "hex ", "colon", "base64", "sha256", "hex,base64url"})
    ExpectRefused(With(ValidEnv(), "FORTIS_PLAY_INTEGRITY_CERT_SHA256_FORMAT", bad),
                  "FORTIS_PLAY_INTEGRITY_CERT_SHA256_FORMAT");
  // Digest lists.
  const std::string one = Hex(g_certificate, 32, true, true);
  std::string five;
  for (int i = 0; i < 5; ++i) {
    uint8_t digest[32];
    memset(digest, 0x40 + i, 32);
    if (i) five += ",";
    five += Hex(digest, 32, true, true);
  }
  for (const std::string& bad :
       {std::string(","), one + ",", "," + one, std::string(one).append(",,").append(one), std::string(one).append(",").append(one), five,
        one + " ", " " + one, std::string(one).append(", ").append(one), one.substr(3), one + ":00",
        Hex(g_certificate, 32, false, true), g_certificate_b64,
        std::string(one).replace(0, 1, "G")})
    ExpectRefused(With(ValidEnv(), "FORTIS_PLAY_INTEGRITY_CERT_SHA256", bad),
                  "FORTIS_PLAY_INTEGRITY_CERT_SHA256");
  // Mixed forms under one declared format.
  ExpectRefused(With(With(ValidEnv(), "FORTIS_PLAY_INTEGRITY_CERT_SHA256_FORMAT", "hex"),
                     "FORTIS_PLAY_INTEGRITY_CERT_SHA256",
                     Hex(digests[0], 32, false, false) + "," + Hex(digests[1], 32, true, false)),
                "FORTIS_PLAY_INTEGRITY_CERT_SHA256");
  // versionCode.
  for (const char* bad : {"0", "01", "+1", "-1", " 1", "1 ", "1.0", "1e3", "0x10",
                          "2100000001", "99999999999", "4294967336"})
    ExpectRefused(With(ValidEnv(), "FORTIS_PLAY_INTEGRITY_MIN_VERSION_CODE", bad),
                  "FORTIS_PLAY_INTEGRITY_MIN_VERSION_CODE");
  // CA: explicit must be absolute; otherwise an absolute fallback is needed.
  ExpectRefused(With(ValidEnv(), "FORTIS_PLAY_INTEGRITY_CA_FILE", "ca.pem"),
                "FORTIS_PLAY_INTEGRITY_CA_FILE");
  ExpectRefused(ValidEnv(), "FORTIS_PLAY_INTEGRITY_CA_FILE", nullptr);
  ExpectRefused(ValidEnv(), "FORTIS_PLAY_INTEGRITY_CA_FILE", "");
  ExpectRefused(ValidEnv(), "FORTIS_PLAY_INTEGRITY_CA_FILE", "relative/ca.pem");
  // Time window bounds.
  for (const char* bad : {"0", "3601", "-5", "60s", "1.5", "007"})
    ExpectRefused(With(ValidEnv(), "FORTIS_PLAY_INTEGRITY_MAX_AGE_SECONDS", bad),
                  "FORTIS_PLAY_INTEGRITY_MAX_AGE_SECONDS");
  for (const char* bad : {"301", "-1", "00", "1 "})
    ExpectRefused(With(ValidEnv(), "FORTIS_PLAY_INTEGRITY_MAX_FUTURE_SKEW_SECONDS", bad),
                  "FORTIS_PLAY_INTEGRITY_MAX_FUTURE_SKEW_SECONDS");
  // Loader misuse.
  char message[64];
  CHECK(!LoadPlayIntegrityConfig(nullptr, nullptr, nullptr, &config, message, sizeof(message)));
  CHECK(!LoadPlayIntegrityConfig(EnvLookup, nullptr, nullptr, nullptr, message,
                                 sizeof(message)));
  // A tiny error buffer is still terminated.
  Env bad_version = With(ValidEnv(), "FORTIS_PLAY_INTEGRITY_MIN_VERSION_CODE", "0");
  CHECK(!LoadPlayIntegrityConfig(EnvLookup, &bad_version, "/ca.pem", &config, message, 8) &&
        strlen(message) == 7);
  puts("config: presence, empty values, required set, paths (no key material), formats, "
       "digest lists, versionCode, CA fallback, time window, no values in messages: ok");
}

void TestInitialize() {
  char error[512];
  {
    PlayIntegrityGoogleClient client;
    CHECK(InitializePlayIntegrityClient(RuntimeConfig(), &client, error, sizeof(error)));
    CHECK(client.configured() && client.oauth_exchanges() == 0 && client.decode_requests() == 0);
    // Initialize runs once per client.
    CHECK(!InitializePlayIntegrityClient(RuntimeConfig(), &client, error, sizeof(error)));
    CHECK(strstr(error, "already initialized") && client.configured());
  }
  {
    PlayIntegrityGoogleClient client;
    PlayIntegrityRuntimeConfig config = RuntimeConfig();
    config.configured = false;
    CHECK(!InitializePlayIntegrityClient(config, &client, error, sizeof(error)));
    CHECK(!InitializePlayIntegrityClient(RuntimeConfig(), nullptr, error, sizeof(error)));
    CHECK(!client.configured());
  }
  struct Case {
    const char* label;
    std::string credential;
    std::string ca;
    const char* expect;
  };
  const std::string open_file = WriteFile("open.json", Credential(), 0644);
  const std::string group_read = WriteFile("group.json", Credential(), 0640);
  const std::string empty = WriteFile("empty.json", "", 0600);
  const std::string not_account =
      WriteFile("wrong-type.json", "{\"type\":\"authorized_user\"}", 0600);
  const std::string link = g_scratch + "/link.json";
  unlink(link.c_str());
  CHECK(symlink(g_credential.c_str(), link.c_str()) == 0);
  const std::string garbage_ca = WriteFile("garbage-ca.pem", "not a certificate\n", 0600);
  const Case cases[] = {
      {"missing credential", g_scratch + "/missing.json", g_ca, "OpenFailed"},
      {"symlinked credential", link, g_ca, "OpenFailed"},
      {"directory credential", g_scratch, g_ca, "NotRegularFile"},
      {"0644 credential", open_file, g_ca, "PermissionsTooOpen"},
      {"0640 credential", group_read, g_ca, "PermissionsTooOpen"},
      {"empty credential", empty, g_ca, "SizeInvalid"},
      {"wrong account type", not_account, g_ca, "TypeInvalid"},
      {"missing CA", g_credential, g_scratch + "/missing-ca.pem", "CA bundle"},
      {"garbage CA", g_credential, garbage_ca, "CA bundle"},
  };
  for (const Case& item : cases) {
    PlayIntegrityGoogleClient client;
    PlayIntegrityRuntimeConfig config = RuntimeConfig();
    snprintf(config.credential_file, sizeof(config.credential_file), "%s",
             item.credential.c_str());
    snprintf(config.ca_file, sizeof(config.ca_file), "%s", item.ca.c_str());
    if (InitializePlayIntegrityClient(config, &client, error, sizeof(error)) ||
        !strstr(error, item.expect) || client.configured()) {
      fprintf(stderr, "%s: \"%s\"\n", item.label, error);
      abort();
    }
    // Messages name the variable and a constant status, never the path.
    CHECK(!strstr(error, item.credential.c_str()) && !strstr(error, g_scratch.c_str()));
  }
  puts("initialize: valid config, double init, unconfigured/null, credential file refusals "
       "(missing, symlink, directory, 0644, 0640, empty, wrong type), CA refusals: ok");
}

void TestRuntime() {
  char error[512];
  CHECK(!RuntimePlayIntegrityClient());
  // Unconfigured: started, no client; a second Start is refused.
  CHECK(StartPlayIntegrityRuntime(PlayIntegrityRuntimeConfig{}, error, sizeof(error)));
  CHECK(!RuntimePlayIntegrityClient());
  CHECK(!StartPlayIntegrityRuntime(RuntimeConfig(), error, sizeof(error)) &&
        strstr(error, "already started"));
  StopPlayIntegrityRuntime();
  // A refused credential publishes nothing.
  PlayIntegrityRuntimeConfig bad = RuntimeConfig();
  snprintf(bad.credential_file, sizeof(bad.credential_file), "%s/missing.json",
           g_scratch.c_str());
  CHECK(!StartPlayIntegrityRuntime(bad, error, sizeof(error)) && strstr(error, "OpenFailed"));
  CHECK(!RuntimePlayIntegrityClient());
  // The test-only endpoint variables go together.
  CHECK(setenv("FORTIS_TEST_PLAY_INTEGRITY_HOST", "localhost", 1) == 0);
  CHECK(!StartPlayIntegrityRuntime(RuntimeConfig(), error, sizeof(error)) &&
        strstr(error, "FORTIS_TEST_PLAY_INTEGRITY_PORT"));
  CHECK(unsetenv("FORTIS_TEST_PLAY_INTEGRITY_HOST") == 0);
  CHECK(!RuntimePlayIntegrityClient());
  // Configured: one initialized client until Stop.
  CHECK(StartPlayIntegrityRuntime(RuntimeConfig(), error, sizeof(error)));
  PlayIntegrityGoogleClient* client = RuntimePlayIntegrityClient();
  CHECK(client && client->configured() && RuntimePlayIntegrityClient() == client);
  CHECK(!StartPlayIntegrityRuntime(RuntimeConfig(), error, sizeof(error)));
  StopPlayIntegrityRuntime();
  CHECK(!RuntimePlayIntegrityClient());
  StopPlayIntegrityRuntime();  // idempotent
  puts("runtime: unconfigured start, double start, refused credential, seam env pairing, "
       "publish and stop: ok");
}

void TestEnrolled() {
  Harness harness("enrolled");
  const std::string challenge = harness.Mint();
  harness.Verify(challenge);
  Call call = For(challenge);
  call.ttl_ms = 600000;
  DeviceCredentials credentials{};
  const int64_t before = Now();
  const PlayEnrollOutcome outcome = Enroll(harness, call, &credentials);
  CHECK_ENROLL(outcome, PlayEnrollStatus::Enrolled);
  CHECK(strcmp(outcome.decode, "Verified") == 0 && outcome.verdict == PlayIntegrityStatus::Verified);
  CHECK(harness.OAuth() == 1 && harness.Decodes() == 1);
  CHECK(harness.fake.decoded_tokens[0] == call.token);
  CHECK(IsDeviceToken(credentials.token) && strlen(credentials.id) == 26);
  CHECK(credentials.expires_at_ms >= before + 600000 && credentials.expires_at_ms <= Now() + 600000);
  // The device authenticates in required mode as an attested Android row.
  DeviceAuthStore auth(&harness.store);
  DeviceRecord row{};
  CHECK(auth.Authenticate(credentials.token, Now(), true, &row) == DeviceAuthResult::Valid);
  CHECK(strcmp(row.id, credentials.id) == 0 && row.platform == DevicePlatform::Android &&
        row.attestation == DeviceAttestation::PlayIntegrity &&
        row.quota_tier == DeviceQuotaTier::Attested && row.has_app_version &&
        strcmp(row.app_version, kAppVersion) == 0 && row.revoked_at_ms == 0 &&
        row.token_expires_at_ms == credentials.expires_at_ms);
  CHECK(DeviceQuotaPerMinute(row.quota_tier) == 600);
  // ...and rotates (rotation semantics are unchanged for Play rows).
  DeviceCredentials rotated{};
  CHECK(auth.Rotate(row, Now(), kTtl, &rotated) == DeviceRotateResult::Rotated);
  CHECK(auth.Authenticate(rotated.token, Now(), true, &row) == DeviceAuthResult::Valid &&
        row.attestation == DeviceAttestation::PlayIntegrity);
  CHECK(!harness.Live(challenge) && harness.Devices() == 1);
  // The spent challenge: refused before Google is called.
  CHECK_ENROLL(Enroll(harness, For(challenge)), PlayEnrollStatus::ChallengeUnavailable);
  CHECK(harness.Decodes() == 1 && harness.Devices() == 1);
  // The spent token on a fresh challenge: Replayed, no request, challenge live.
  const std::string second = harness.Mint();
  Call reuse = For(second);
  reuse.token = call.token;
  const PlayEnrollOutcome replayed = Enroll(harness, reuse);
  CHECK_ENROLL(replayed, PlayEnrollStatus::InvalidProof);
  CHECK(strcmp(replayed.decode, "Replayed") == 0);
  CHECK(harness.Decodes() == 1 && harness.Live(second));
  // A fresh token then enrolls a second device; the OAuth token is reused.
  harness.Verify(second);
  CHECK_ENROLL(Enroll(harness, For(second)), PlayEnrollStatus::Enrolled);
  CHECK(harness.OAuth() == 1 && harness.Decodes() == 2 && harness.Devices() == 2);
  // A challenge enrolled at the last live millisecond.
  const int64_t minted = Now() - kDeviceChallengeTtlMs + 2000;
  const std::string late = harness.Mint(minted);
  harness.Verify(late);
  Call edge = For(late);
  edge.now_ms = minted + kDeviceChallengeTtlMs - 1;
  fortis::testing::ManualClock frozen(edge.now_ms);
  edge.clock = &frozen;
  CHECK_ENROLL(Enroll(harness, edge), PlayEnrollStatus::Enrolled);
  puts("enroll: Verified decode -> Android/PlayIntegrity/Attested row, TTL, required-mode "
       "auth and rotation, spent challenge and token, OAuth reuse, last live millisecond: ok");
}

void TestPreIo() {
  Harness harness("pre-io");
  const std::string challenge = harness.Mint();
  harness.Verify(challenge);
  DeviceCredentials untouched{};
  // Not configured: nothing read or sent.
  {
    DeviceAuthStore auth(&harness.store);
    CHECK_ENROLL(EnrollWithPlayIntegrity(nullptr, auth, PlayEnrollRequest{}, &untouched),
                 PlayEnrollStatus::NotConfigured);
  }
  PlayIntegrityGoogleClient unconfigured;
  CHECK_ENROLL(Enroll(harness, For(challenge), &untouched, &unconfigured),
               PlayEnrollStatus::NotConfigured);
  // Output required.
  {
    DeviceAuthStore auth(&harness.store);
    PlayEnrollRequest request;
    CHECK_ENROLL(EnrollWithPlayIntegrity(&harness.client, auth, request, nullptr),
                 PlayEnrollStatus::StorageError);
  }
  // Malformed challenges are never live.
  std::string with_nul = challenge;
  with_nul[10] = '\0';
  std::string padded = challenge.substr(0, 42) + "=";
  std::string plus = challenge;
  plus[3] = '+';
  for (const std::string& bad : {challenge.substr(0, 42), challenge + "A", with_nul, padded, plus,
                                 std::string(), std::string(43, ' ')}) {
    Call call = For(bad);
    CHECK_ENROLL(Enroll(harness, call), PlayEnrollStatus::ChallengeUnavailable);
  }
  // Missing or out-of-bounds appVersion and token.
  Call no_version = For(challenge);
  no_version.app_version = nullptr;
  CHECK_ENROLL(Enroll(harness, no_version), PlayEnrollStatus::InvalidProof);
  Call empty_version = For(challenge);
  empty_version.app_version_length = 0;
  CHECK_ENROLL(Enroll(harness, empty_version), PlayEnrollStatus::InvalidProof);
  const std::string long_version(201, '1');
  Call big_version = For(challenge);
  big_version.app_version = long_version.c_str();
  big_version.app_version_length = long_version.size();
  CHECK_ENROLL(Enroll(harness, big_version), PlayEnrollStatus::InvalidProof);
  Call no_token = For(challenge);
  no_token.token_present = false;
  CHECK_ENROLL(Enroll(harness, no_token), PlayEnrollStatus::InvalidProof);
  for (size_t length : {size_t(15), kPlayIntegrityTokenMaxBytes + 1}) {
    Call sized = For(challenge);
    sized.token = std::string(length, 'A');
    CHECK_ENROLL(Enroll(harness, sized), PlayEnrollStatus::InvalidProof);
  }
  CHECK(harness.OAuth() == 0 && harness.Decodes() == 0);
  // Well-formed but never minted, and expired: no Google request.
  CHECK_ENROLL(Enroll(harness, For(RandomChallenge())), PlayEnrollStatus::ChallengeUnavailable);
  const int64_t minted = Now() - kDeviceChallengeTtlMs - 5;
  const std::string expired = harness.Mint(minted);
  Call at_expiry = For(expired);
  at_expiry.now_ms = minted + kDeviceChallengeTtlMs;
  CHECK_ENROLL(Enroll(harness, at_expiry), PlayEnrollStatus::ChallengeUnavailable);
  CHECK_ENROLL(Enroll(harness, For(expired)), PlayEnrollStatus::ChallengeUnavailable);
  // Decode's own input checks run before any I/O (and burn nothing).
  Call spaced = For(challenge);
  spaced.token = "eyJhbGciOiJBMjU2S1ciLCJlbmMi Oi.c3.bad";
  const PlayEnrollOutcome bad_token = Enroll(harness, spaced);
  CHECK_ENROLL(bad_token, PlayEnrollStatus::InvalidProof);
  CHECK(strcmp(bad_token.decode, "InputInvalid") == 0);
  static const char kControl[] = "1.4\n2";
  Call control = For(challenge);
  control.app_version = kControl;
  control.app_version_length = sizeof(kControl) - 1;
  CHECK_ENROLL(Enroll(harness, control), PlayEnrollStatus::InvalidProof);
  static const char kBadUtf8[] = "1.\xc0\xae";
  Call utf8 = For(challenge);
  utf8.app_version = kBadUtf8;
  utf8.app_version_length = sizeof(kBadUtf8) - 1;
  CHECK_ENROLL(Enroll(harness, utf8), PlayEnrollStatus::InvalidProof);
  CHECK(harness.OAuth() == 0 && harness.Decodes() == 0 && harness.client.decode_requests() == 0);
  CHECK(harness.Live(challenge) && harness.Devices() == 0 && untouched.id[0] == '\0');
  // After every refusal above a good token still enrolls on the challenge.
  CHECK_ENROLL(Enroll(harness, For(challenge)), PlayEnrollStatus::Enrolled);
  puts("pre-I/O: not configured, null output, malformed/unknown/expired challenges, missing "
       "or oversized appVersion/token, token alphabet, appVersion control/UTF-8: no KV write, "
       "no Google request: ok");
}

void TestVerdictRefusals() {
  Harness harness("verdict");
  const std::string challenge = harness.Mint();
  struct Case {
    const char* label;
    std::function<void(VerdictSpec*)> edit;
    PlayIntegrityStatus expect;
  };
  const std::string other = RandomChallenge();
  const Case cases[] = {
      {"certificate", [](VerdictSpec* s) { s->certificate = B64(reinterpret_cast<const uint8_t*>("\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07\x07"), 32); },
       PlayIntegrityStatus::CertificateMismatch},
      {"versionCode below minimum", [](VerdictSpec* s) { s->version_code = "39"; },
       PlayIntegrityStatus::VersionCodeRejected},
      {"basic integrity only", [](VerdictSpec* s) { s->device = "\"MEETS_BASIC_INTEGRITY\""; },
       PlayIntegrityStatus::DeviceIntegrityFailed},
      {"no device verdict", [](VerdictSpec* s) { s->device = ""; },
       PlayIntegrityStatus::DeviceIntegrityFailed},
      {"virtual integrity only", [](VerdictSpec* s) { s->device = "\"MEETS_VIRTUAL_INTEGRITY\""; },
       PlayIntegrityStatus::DeviceIntegrityFailed},
      {"hash of another challenge", [&](VerdictSpec* s) { s->hash = Hash(other, kAppVersion); },
       PlayIntegrityStatus::RequestHashMismatch},
      {"hash of another appVersion", [&](VerdictSpec* s) { s->hash = Hash(challenge, "1.4.3"); },
       PlayIntegrityStatus::RequestHashMismatch},
      {"stale timestamp", [](VerdictSpec* s) { s->timestamp_ms = Now() - 301000; },
       PlayIntegrityStatus::TimestampStale},
      {"future timestamp", [](VerdictSpec* s) { s->timestamp_ms = Now() + 61000; },
       PlayIntegrityStatus::TimestampFuture},
      {"unrecognized app", [](VerdictSpec* s) { s->app_verdict = "UNRECOGNIZED_VERSION"; },
       PlayIntegrityStatus::AppNotRecognized},
      {"Google replay answer", [](VerdictSpec* s) {
         s->app_verdict = "UNEVALUATED";
         s->device = "";
       }, PlayIntegrityStatus::AppNotRecognized},
      {"other package", [](VerdictSpec* s) { s->package = "io.fortisnet.other"; },
       PlayIntegrityStatus::PackageMismatch},
      {"testing response", [](VerdictSpec* s) { s->testing = true; },
       PlayIntegrityStatus::TestingResponse},
  };
  size_t decodes = 0;
  for (const Case& item : cases) {
    VerdictSpec spec = Bound(challenge);
    item.edit(&spec);
    harness.fake.on_decode = [spec](const std::string&) {
      return Reply{OutboundError::kNone, 200, "application/json", Verdict(spec)};
    };
    DeviceCredentials credentials{};
    const PlayEnrollOutcome outcome = Enroll(harness, For(challenge), &credentials);
    if (outcome.status != PlayEnrollStatus::InvalidProof || outcome.verdict != item.expect ||
        strcmp(outcome.decode, "VerdictRejected") != 0) {
      fprintf(stderr, "%s: %s %s %s\n", item.label, PlayEnrollStatusName(outcome.status),
              outcome.decode, PlayIntegrityStatusName(outcome.verdict));
      abort();
    }
    CHECK(++decodes == harness.Decodes() && credentials.id[0] == '\0');
    CHECK(harness.Live(challenge) && harness.Devices() == 0);
  }
  // A non-JSON-object 2xx body stops in the validator too.
  harness.fake.on_decode = [](const std::string&) {
    return Reply{OutboundError::kNone, 200, "application/json", "{\"error\":{\"code\":400}}"};
  };
  const PlayEnrollOutcome error_body = Enroll(harness, For(challenge));
  CHECK_ENROLL(error_body, PlayEnrollStatus::InvalidProof);
  CHECK(error_body.verdict == PlayIntegrityStatus::PayloadMalformed);
  // The challenge survived every refusal; a good verdict with a new token wins.
  harness.Verify(challenge);
  CHECK_ENROLL(Enroll(harness, For(challenge)), PlayEnrollStatus::Enrolled);
  CHECK(!harness.Live(challenge) && harness.Devices() == 1);
  puts("verdict: certificate, versionCode, basic/empty/virtual device, other challenge or "
       "appVersion hash, stale/future time, unrecognized app, Google replay answer, package, "
       "testing response, error body -> 401 class, challenge live, then a new token wins: ok");
}

void TestUpstream() {
  Harness harness("upstream");
  const std::string challenge = harness.Mint();
  struct Case {
    const char* label;
    Reply reply;
    PlayEnrollStatus expect;
    const char* decode;
  };
  const Case cases[] = {
      {"500", {OutboundError::kNone, 500, "application/json", "{}"},
       PlayEnrollStatus::Unavailable, "UpstreamUnavailable"},
      {"503", {OutboundError::kNone, 503, "text/html", "<html/>"}, PlayEnrollStatus::Unavailable,
       "UpstreamUnavailable"},
      {"429", {OutboundError::kNone, 429, "application/json", "{}"}, PlayEnrollStatus::Busy,
       "RateLimited"},
      {"403", {OutboundError::kNone, 403, "application/json", "{}"},
       PlayEnrollStatus::Unavailable, "PermissionDenied"},
      {"400", {OutboundError::kNone, 400, "application/json", "{}"},
       PlayEnrollStatus::InvalidProof, "TokenRejected"},
      {"404", {OutboundError::kNone, 404, "application/json", "{}"},
       PlayEnrollStatus::Unavailable, "UpstreamRejected"},
      {"302", {OutboundError::kNone, 302, "", ""}, PlayEnrollStatus::Unavailable,
       "UpstreamRejected"},
      {"html 2xx", {OutboundError::kNone, 200, "text/html", "<html/>"},
       PlayEnrollStatus::Unavailable, "ResponseMalformed"},
      {"timeout", {OutboundError::kTimeout, 0, "", ""}, PlayEnrollStatus::Unavailable, "Timeout"},
      {"tls", {OutboundError::kTls, 0, "", ""}, PlayEnrollStatus::Unavailable, "TlsFailed"},
      {"connect", {OutboundError::kConnect, 0, "", ""}, PlayEnrollStatus::Unavailable,
       "TransportFailed"},
      {"too large", {OutboundError::kTooLarge, 0, "", ""}, PlayEnrollStatus::Unavailable,
       "ResponseTooLarge"},
  };
  for (const Case& item : cases) {
    harness.fake.on_decode = [reply = item.reply](const std::string&) { return Reply(reply); };
    const PlayEnrollOutcome outcome = Enroll(harness, For(challenge));
    if (outcome.status != item.expect || strcmp(outcome.decode, item.decode) != 0) {
      fprintf(stderr, "%s: %s %s\n", item.label, PlayEnrollStatusName(outcome.status),
              outcome.decode);
      abort();
    }
    CHECK(harness.Live(challenge) && harness.Devices() == 0);
  }
  // 401 twice: one refresh, then Unauthorized (Unavailable).
  harness.fake.on_decode = [](const std::string&) {
    return Reply{OutboundError::kNone, 401, "application/json", "{}"};
  };
  const size_t oauth = harness.OAuth();
  const PlayEnrollOutcome unauthorized = Enroll(harness, For(challenge));
  CHECK_ENROLL(unauthorized, PlayEnrollStatus::Unavailable);
  CHECK(strcmp(unauthorized.decode, "Unauthorized") == 0 && harness.OAuth() == oauth + 1);
  harness.Verify(challenge);
  CHECK_ENROLL(Enroll(harness, For(challenge)), PlayEnrollStatus::Enrolled);

  // OAuth failures on a fresh client: no decode request is made.
  struct OAuthCase {
    const char* label;
    Reply reply;
    const char* decode;
  };
  const OAuthCase oauth_cases[] = {
      {"oauth 500", {OutboundError::kNone, 500, "application/json", "{}"}, "OAuthUnavailable"},
      {"oauth error", {OutboundError::kNone, 400, "application/json",
                       "{\"error\":\"invalid_grant\"}"}, "OAuthRejected"},
      {"oauth bad token", {OutboundError::kNone, 200, "application/json",
                           "{\"access_token\":\"a b\",\"expires_in\":3599,"
                           "\"token_type\":\"Bearer\"}"}, "OAuthMalformed"},
      {"oauth tls", {OutboundError::kTls, 0, "", ""}, "OAuthUnavailable"},
  };
  for (const OAuthCase& item : oauth_cases) {
    Harness fresh("oauth");
    const std::string value = fresh.Mint();
    fresh.Verify(value);
    fresh.fake.on_oauth = [reply = item.reply] { return Reply(reply); };
    const PlayEnrollOutcome outcome = Enroll(fresh, For(value));
    if (outcome.status != PlayEnrollStatus::Unavailable || strcmp(outcome.decode, item.decode) != 0) {
      fprintf(stderr, "%s: %s %s\n", item.label, PlayEnrollStatusName(outcome.status),
              outcome.decode);
      abort();
    }
    CHECK(fresh.Decodes() == 0 && fresh.Live(value) && fresh.Devices() == 0);
    // Inside the 5 s failure backoff: no new exchange either.
    const size_t exchanges = fresh.OAuth();
    CHECK_ENROLL(Enroll(fresh, For(value)), PlayEnrollStatus::Unavailable);
    CHECK(fresh.OAuth() == exchanges && fresh.Live(value));
  }
  puts("upstream: decode 5xx/403/404/3xx/html/timeout/TLS/connect/oversize -> Unavailable, "
       "429 -> Busy, 400 -> InvalidProof, 401x2 -> Unavailable; OAuth 5xx/error/malformed/TLS "
       "-> Unavailable without a decode; challenge live throughout: ok");
}

void TestClockAndTtl() {
  Harness harness("clock");
  const std::string challenge = harness.Mint();
  harness.Verify(challenge);
  // A clock failure after the decode: nothing written (the token is burned).
  Call broken = For(challenge);
  fortis::testing::ManualClock zero_clock(0);
  broken.clock = &zero_clock;
  CHECK_ENROLL(Enroll(harness, broken), PlayEnrollStatus::StorageError);
  CHECK(harness.Live(challenge) && harness.Devices() == 0);
  // The challenge expired while Google was answering: the frame refuses it.
  fortis::testing::ManualClock later(Now() + kDeviceChallengeTtlMs + 1);
  Call slow = For(challenge);
  slow.clock = &later;
  CHECK_ENROLL(Enroll(harness, slow), PlayEnrollStatus::ChallengeUnavailable);
  CHECK(harness.Live(challenge) && harness.Devices() == 0);
  // TTL outside the device bounds: refused by EnrollAttested, nothing written.
  for (int64_t ttl : {int64_t(299999), int64_t(2592000001LL), int64_t(0)}) {
    Call bad_ttl = For(challenge);
    bad_ttl.ttl_ms = ttl;
    CHECK_ENROLL(Enroll(harness, bad_ttl), PlayEnrollStatus::Rejected);
    CHECK(harness.Live(challenge) && harness.Devices() == 0);
  }
  // A clock that went backwards is clamped to the precheck instant.
  const int64_t request_now = Now();
  fortis::testing::ManualClock earlier(request_now - 120000);
  Call backwards = For(challenge);
  backwards.now_ms = request_now;
  backwards.ttl_ms = 300000;
  backwards.clock = &earlier;
  DeviceCredentials credentials{};
  CHECK_ENROLL(Enroll(harness, backwards, &credentials), PlayEnrollStatus::Enrolled);
  CHECK(credentials.expires_at_ms == request_now + 300000);
  puts("clock/ttl: post-decode clock failure, expiry during decode, TTL bounds, backwards "
       "clock clamped to the request instant: ok");
}

void TestStorage() {
  Harness harness("storage");
  const std::string challenge = harness.Mint();
  harness.Verify(challenge);
  // The conditional frame fails after its comparison matched and before it
  // is written: StorageError, no bearer revealed, challenge still live.
  CHECK(setenv("FORTIS_KV_TEST_FAIL", "compare_matched", 1) == 0);
  DeviceCredentials credentials{};
  const PlayEnrollOutcome failed = Enroll(harness, For(challenge), &credentials);
  CHECK(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
  CHECK_ENROLL(failed, PlayEnrollStatus::StorageError);
  CHECK(credentials.id[0] == '\0' && credentials.token[0] == '\0');
  CHECK(harness.Live(challenge) && harness.Devices() == 0);
  CHECK_ENROLL(Enroll(harness, For(challenge)), PlayEnrollStatus::Enrolled);
  puts("storage: a failed enrollment frame is StorageError with nothing revealed or "
       "consumed: ok");
}

// All decodes of one round are held inside the transport until `parties`
// of them are in flight, so every contender has passed the precheck before
// any of them can consume the challenge.
struct Rendezvous {
  std::mutex mutex;
  std::condition_variable changed;
  unsigned waiting = 0, parties = 0, generation = 0;
  void Arrive() {
    std::unique_lock<std::mutex> lock(mutex);
    const unsigned round = generation;
    if (++waiting == parties) {
      waiting = 0;
      ++generation;
      changed.notify_all();
      return;
    }
    CHECK(changed.wait_for(lock, std::chrono::seconds(10), [&] { return generation != round; }));
  }
};

void TestRace() {
  Harness harness("race");
  constexpr unsigned kRounds = 25;
  // Distinct tokens: kPlayDecodeMaxConcurrent decodes truly in flight.
  {
    Rendezvous rendezvous;
    rendezvous.parties = kPlayDecodeMaxConcurrent;
    std::string bound;
    std::mutex bound_mutex;
    harness.fake.on_decode = [&](const std::string&) {
      rendezvous.Arrive();
      std::lock_guard<std::mutex> lock(bound_mutex);
      return Reply{OutboundError::kNone, 200, "application/json", Verdict(Bound(bound))};
    };
    for (unsigned round = 0; round < kRounds; ++round) {
      const std::string challenge = harness.Mint();
      {
        std::lock_guard<std::mutex> lock(bound_mutex);
        bound = challenge;
      }
      std::atomic<unsigned> enrolled{0}, unavailable{0}, other{0};
      std::vector<std::thread> threads;
      for (unsigned i = 0; i < kPlayDecodeMaxConcurrent; ++i)
        threads.emplace_back([&] {
          const PlayEnrollOutcome outcome = Enroll(harness, For(challenge));
          if (outcome.status == PlayEnrollStatus::Enrolled) ++enrolled;
          else if (outcome.status == PlayEnrollStatus::ChallengeUnavailable) ++unavailable;
          else ++other;
        });
      for (auto& thread : threads) thread.join();
      CHECK(enrolled == 1 && unavailable == kPlayDecodeMaxConcurrent - 1 && other == 0);
      CHECK(!harness.Live(challenge));
    }
    CHECK(harness.Devices() == kRounds);
  }
  // More contenders than decode slots, no barrier: still exactly one device.
  {
    std::string bound;
    std::mutex bound_mutex;
    harness.fake.on_decode = [&](const std::string&) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      std::lock_guard<std::mutex> lock(bound_mutex);
      return Reply{OutboundError::kNone, 200, "application/json", Verdict(Bound(bound))};
    };
    unsigned busy_total = 0;
    for (unsigned round = 0; round < kRounds; ++round) {
      const std::string challenge = harness.Mint();
      {
        std::lock_guard<std::mutex> lock(bound_mutex);
        bound = challenge;
      }
      std::atomic<unsigned> enrolled{0}, unavailable{0}, busy{0}, other{0};
      std::vector<std::thread> threads;
      for (unsigned i = 0; i < 8; ++i)
        threads.emplace_back([&] {
          const PlayEnrollOutcome outcome = Enroll(harness, For(challenge));
          if (outcome.status == PlayEnrollStatus::Enrolled) ++enrolled;
          else if (outcome.status == PlayEnrollStatus::ChallengeUnavailable) ++unavailable;
          else if (outcome.status == PlayEnrollStatus::Busy) ++busy;
          else ++other;
        });
      for (auto& thread : threads) thread.join();
      // A round where every contender was Busy would leave the challenge
      // live; the first one always gets a slot, so one wins.
      CHECK(enrolled == 1 && other == 0 && enrolled + unavailable + busy == 8);
      busy_total += busy;
    }
    CHECK(harness.Devices() == 2 * kRounds);
    printf("  race: 8 contenders x %u rounds, %u answered Busy\n", kRounds, busy_total);
  }
  // One token sent by several contenders: one decode request, one device.
  {
    const std::string challenge = harness.Mint();
    harness.Verify(challenge);
    const std::string shared = FreshToken("shared");
    const size_t before = harness.Decodes();
    std::atomic<unsigned> enrolled{0}, refused{0}, other{0};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 4; ++i)
      threads.emplace_back([&] {
        Call call = For(challenge);
        call.token = shared;
        const PlayEnrollOutcome outcome = Enroll(harness, call);
        if (outcome.status == PlayEnrollStatus::Enrolled) ++enrolled;
        else if (outcome.status == PlayEnrollStatus::InvalidProof ||
                 outcome.status == PlayEnrollStatus::ChallengeUnavailable) ++refused;
        else ++other;
      });
    for (auto& thread : threads) thread.join();
    CHECK(enrolled == 1 && refused == 3 && other == 0 && harness.Decodes() == before + 1);
  }
  puts("race: 4 simultaneous verified decodes per challenge x 25 rounds -> exactly one device, "
       "others ChallengeUnavailable; 8 contenders with Busy; a shared token decodes once: ok");
}

void TestRestart() {
  const std::string directory = g_scratch + "/kv-restart";
  fortis::test::RemoveTree(directory);
  CHECK(mkdir(directory.c_str(), 0700) == 0);
  std::string spent, refused;
  DeviceCredentials credentials{};
  {
    Harness first("restart-first", directory);
    spent = first.Mint();
    refused = first.Mint();
    first.Verify(spent);
    CHECK_ENROLL(Enroll(first, For(spent), &credentials), PlayEnrollStatus::Enrolled);
    first.fake.on_decode = [](const std::string&) {
      return Reply{OutboundError::kNone, 500, "application/json", "{}"};
    };
    CHECK_ENROLL(Enroll(first, For(refused)), PlayEnrollStatus::Unavailable);
  }
  {
    // A new process: new client (empty replay ring), same store.
    Harness second("restart-second", directory);
    DeviceAuthStore auth(&second.store);
    DeviceRecord row{};
    CHECK(auth.Authenticate(credentials.token, Now(), true, &row) == DeviceAuthResult::Valid &&
          row.attestation == DeviceAttestation::PlayIntegrity &&
          row.platform == DevicePlatform::Android);
    second.Verify(spent);
    CHECK_ENROLL(Enroll(second, For(spent)), PlayEnrollStatus::ChallengeUnavailable);
    CHECK(second.Decodes() == 0);
    second.Verify(refused);
    CHECK_ENROLL(Enroll(second, For(refused)), PlayEnrollStatus::Enrolled);
    CHECK(second.Devices() == 2);
  }
  fortis::test::RemoveTree(directory);
  puts("restart: device authenticates and challenge stays spent after reopening; a challenge "
       "refused upstream before the restart enrolls after it: ok");
}

}  // namespace

// The route's admission limit (play_integrity_enrollment.h, "Route
// admission"). enroll_play_integrity holds one PlayEnrollRouteSlot across its
// KV/clock check and EnrollWithPlayIntegrity; the cases below hold it across
// the same call for every outcome and check it comes back.
void TestAdmission() {
  static_assert(kPlayEnrollRouteMaxConcurrent == 3, "one of the four HTTP workers stays free");
  static_assert(kPlayEnrollRouteMaxConcurrent < kPlayDecodeMaxConcurrent,
                "the client's own cap stays behind the route's");
  CHECK(PlayEnrollRouteSlotsInUse() == 0);
  {
    const PlayEnrollRouteSlot first, second, third;
    CHECK(first.admitted() && second.admitted() && third.admitted());
    CHECK(PlayEnrollRouteSlotsInUse() == 3);
    for (int i = 0; i < 100; ++i) {
      const PlayEnrollRouteSlot refused;
      CHECK(!refused.admitted());
    }
    // A refused slot holds nothing and returns nothing.
    CHECK(PlayEnrollRouteSlotsInUse() == 3);
  }
  CHECK(PlayEnrollRouteSlotsInUse() == 0);

  // Holders parked on other threads (as three workers waiting on Google):
  // the fourth is refused at once instead of waiting for one to finish.
  {
    std::mutex mutex;
    std::condition_variable changed;
    bool release = false;
    std::vector<std::thread> holders;
    for (unsigned i = 0; i < kPlayEnrollRouteMaxConcurrent; ++i)
      holders.emplace_back([&] {
        const PlayEnrollRouteSlot slot;
        CHECK(slot.admitted());
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock, std::chrono::seconds(10), [&] { return release; }));
      });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (PlayEnrollRouteSlotsInUse() != kPlayEnrollRouteMaxConcurrent)
      CHECK(std::chrono::steady_clock::now() < deadline);
    const auto started = std::chrono::steady_clock::now();
    {
      const PlayEnrollRouteSlot fourth;
      CHECK(!fourth.admitted());
    }
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    {
      std::lock_guard<std::mutex> lock(mutex);
      release = true;
    }
    changed.notify_all();
    for (std::thread& holder : holders) holder.join();
    CHECK(PlayEnrollRouteSlotsInUse() == 0);
  }

  // Every enrollment outcome returns its slot. Two slots stay held so each
  // case takes the last one; a case that kept it would refuse the next.
  {
    const PlayEnrollRouteSlot held_a, held_b;
    CHECK(held_a.admitted() && held_b.admitted());
    Harness harness("admission");
    const std::string challenge = harness.Mint();
    const std::string other = RandomChallenge();
    // Twice per outcome, each with a fresh token (a decode burns its token).
    auto in_last_slot = [&](const Call& call, PlayEnrollStatus expected) {
      for (int attempt = 0; attempt < 2; ++attempt) {
        Call fresh = call;
        fresh.token = FreshToken();
        const PlayEnrollRouteSlot slot;
        CHECK(slot.admitted() && PlayEnrollRouteSlotsInUse() == 3);
        CHECK_ENROLL(Enroll(harness, fresh), expected);
        const PlayEnrollRouteSlot over;
        CHECK(!over.admitted());
      }
      CHECK(PlayEnrollRouteSlotsInUse() == 2);
    };
    harness.fake.on_decode = [](const std::string&) {
      return Reply{OutboundError::kNone, 500, "application/json", "{}"};
    };
    in_last_slot(For(challenge), PlayEnrollStatus::Unavailable);
    harness.fake.on_decode = [](const std::string&) {
      return Reply{OutboundError::kNone, 429, "application/json", "{}"};
    };
    in_last_slot(For(challenge), PlayEnrollStatus::Busy);
    harness.Verify(other);  // bound to another challenge: requestHash mismatch
    in_last_slot(For(challenge), PlayEnrollStatus::InvalidProof);
    in_last_slot(For(other), PlayEnrollStatus::ChallengeUnavailable);  // never minted
    harness.Verify(challenge);
    Call broken_clock = For(challenge);
    static fortis::testing::ManualClock zero_clock(0);
    broken_clock.clock = &zero_clock;
    in_last_slot(broken_clock, PlayEnrollStatus::StorageError);
    CHECK(setenv("FORTIS_KV_TEST_FAIL", "compare_matched", 1) == 0);
    in_last_slot(For(challenge), PlayEnrollStatus::StorageError);
    CHECK(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
    CHECK(harness.Live(challenge) && harness.Devices() == 0);
    {
      const PlayEnrollRouteSlot slot;
      CHECK(slot.admitted());
      CHECK_ENROLL(Enroll(harness, For(challenge)), PlayEnrollStatus::Enrolled);
    }
    CHECK(PlayEnrollRouteSlotsInUse() == 2 && harness.Devices() == 1);
  }
  CHECK(PlayEnrollRouteSlotsInUse() == 0);

  // Contention: threads released together by a start gate never hold more
  // than three slots between them, and every slot comes back.
  {
    constexpr unsigned kThreads = 12;
    constexpr unsigned kIterations = 20000;
    std::atomic<bool> go{false};
    std::atomic<unsigned> ready{0}, inside{0}, peak{0}, admitted{0}, refused{0};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < kThreads; ++t)
      threads.emplace_back([&] {
        ready.fetch_add(1);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (unsigned i = 0; i < kIterations; ++i) {
          const PlayEnrollRouteSlot slot;
          if (!slot.admitted()) {
            refused.fetch_add(1, std::memory_order_relaxed);
            continue;
          }
          admitted.fetch_add(1, std::memory_order_relaxed);
          const unsigned now = inside.fetch_add(1) + 1;
          CHECK(now <= kPlayEnrollRouteMaxConcurrent);
          unsigned seen = peak.load();
          while (now > seen && !peak.compare_exchange_weak(seen, now)) {}
          inside.fetch_sub(1);
        }
      });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (ready.load() != kThreads) CHECK(std::chrono::steady_clock::now() < deadline);
    go.store(true, std::memory_order_release);
    for (std::thread& thread : threads) thread.join();
    CHECK(PlayEnrollRouteSlotsInUse() == 0);
    CHECK(admitted.load() + refused.load() == kThreads * kIterations);
    CHECK(peak.load() >= 1 && peak.load() <= kPlayEnrollRouteMaxConcurrent);
    printf("admission: 3 slots, 100 refusals release nothing, refusal immediate with three "
           "parked holders; Unavailable/Busy/InvalidProof/ChallengeUnavailable/clock and KV "
           "StorageError/Enrolled each return the slot; %u x %u contended: peak %u, %u "
           "refused, 0 left: ok\n",
           kThreads, kIterations, peak.load(), refused.load());
  }
}

int main(int argc, char** argv) try {
  if (argc != 3 || strcmp(argv[1], "unit") != 0) {
    fprintf(stderr, "usage: %s unit FIXTURE_DIR\n", argv[0]);
    return 2;
  }
  g_fixture = argv[2];
  g_scratch = g_fixture + "/c3-unit";
  fortis::test::RemoveTree(g_scratch);
  CHECK(mkdir(g_scratch.c_str(), 0700) == 0);
  g_ca = g_fixture + "/ca.pem";
  g_credential = WriteFile("credential.json", Credential());
  CHECK(SecureRandomBytes(g_certificate, sizeof(g_certificate)));
  g_certificate_b64 = B64(g_certificate, sizeof(g_certificate));
  CHECK(unsetenv("FORTIS_TEST_PLAY_INTEGRITY_HOST") == 0 &&
        unsetenv("FORTIS_TEST_PLAY_INTEGRITY_PORT") == 0 && unsetenv("FORTIS_KV_TEST_FAIL") == 0);
  CHECK(OutboundHttpsInitialize());
  TestConfig();
  TestInitialize();
  TestRuntime();
  TestEnrolled();
  TestPreIo();
  TestVerdictRefusals();
  TestUpstream();
  TestClockAndTtl();
  TestStorage();
  TestRace();
  TestRestart();
  TestAdmission();
  fortis::test::RemoveTree(g_scratch);
  puts("play_integrity_enrollment_test: all groups passed");
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
