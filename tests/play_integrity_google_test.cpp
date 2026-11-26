#include <algorithm>
#include "test_support.h"
// Play Integrity: service-account credential, OAuth assertion, token
// cache and decode client. Built with FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT.
//
//   play_integrity_google_test unit FIXTURE_DIR
//       In-process groups over an injected transport and clocks.
//   play_integrity_google_test tls FIXTURE_DIR HOST PORT CA APP_VERSION TOKEN
//       One Decode over real TLS to tests/play_integrity_google_stub.py;
//       prints "<status> <verdict> <oauth exchanges> <decode requests>".
//
// FIXTURE_DIR is made by tests/play_integrity_google_test.py with openssl at
// test time (no private key is stored in Git): rsa2048.pem (PKCS#8),
// rsa2048.pub.pem, rsa3072.pem, rsa1024.pem, ec.pem, rsa_pkcs1.pem,
// encrypted.pem and ca.pem.

#include "../src/auth/play_integrity_google.h"

#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace fortis::auth;
using namespace fortis::http;

using namespace fortis;

static_assert(!std::is_constructible<PlayIntegrityDecodedResponse, const uint8_t*, size_t>::value,
              "only PlayIntegrityDecodeSource may wrap a decoded response");
static_assert(!std::is_copy_constructible<PlayServiceAccount>::value, "key is not copyable");

#define CHECK(condition)                                                            \
  do {                                                                              \
    if (!(condition)) {                                                             \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
      abort();                                                                      \
    }                                                                               \
  } while (0)

#define CHECK_DECODE(actual, expected)                                                  \
  do {                                                                                  \
    const PlayDecodeStatus got_ = (actual);                                             \
    if (got_ != (expected)) {                                                           \
      fprintf(stderr, "%s:%d: got %s, expected %s\n", __FILE__, __LINE__,               \
              PlayDecodeStatusName(got_), PlayDecodeStatusName(expected));              \
      abort();                                                                          \
    }                                                                                   \
  } while (0)

#define CHECK_ACCOUNT(path, expected)                                                   \
  do {                                                                                  \
    const std::string& path_ = (path);                                                   \
    PlayServiceAccount account_;                                                        \
    const ServiceAccountStatus got_ = LoadPlayServiceAccount(path_.c_str(), &account_); \
    if (got_ != (expected)) {                                                           \
      fprintf(stderr, "%s:%d: %s: got %s, expected %s\n", __FILE__, __LINE__,           \
              path_.c_str(), ServiceAccountStatusName(got_),                            \
              ServiceAccountStatusName(expected));                                      \
      abort();                                                                          \
    }                                                                                   \
    CHECK(account_.loaded() == ((expected) == ServiceAccountStatus::Ok));               \
  } while (0)

namespace {

constexpr char kChallenge[] = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8";
constexpr char kOtherChallenge[] = "HwECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8";
constexpr char kAppVersion[] = "1.4.2";
constexpr char kEmail[] = "play-verifier@fortis-test.iam.gserviceaccount.com";
constexpr char kKeyId[] = "0123456789abcdef0123456789abcdef01234567";
constexpr int64_t kWallStart = 1761652800000;  // 2025-10-28T12:00:00Z

std::string g_fixture;
std::string g_accounts;

// ---- Files ----------------------------------------------------------------

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
  const std::string path = g_accounts + "/" + name;
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

using Fields = std::vector<std::pair<std::string, std::string>>;

// A Google-shaped key file; `fields` are raw JSON values.
Fields AccountFields(const std::string& pem_file = "rsa2048.pem") {
  return {{"type", "\"service_account\""},
          {"project_id", "\"fortis-test\""},
          {"private_key_id", JsonString(kKeyId)},
          {"private_key", JsonString(ReadFile(g_fixture + "/" + pem_file))},
          {"client_email", JsonString(kEmail)},
          {"client_id", "\"123456789012345678901\""},
          {"auth_uri", "\"https://accounts.google.com/o/oauth2/auth\""},
          {"token_uri", "\"https://oauth2.googleapis.com/token\""},
          {"auth_provider_x509_cert_url", "\"https://www.googleapis.com/oauth2/v1/certs\""},
          {"universe_domain", "\"googleapis.com\""}};
}

std::string Object(const Fields& fields) {
  std::string out = "{";
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i) out += ",";
    out += JsonString(fields[i].first) + ":" + fields[i].second;
  }
  return out + "}";
}

Fields With(Fields fields, const std::string& key, const std::string& value) {
  for (auto& field : fields)
    if (field.first == key) {
      field.second = value;
      return fields;
    }
  fields.emplace_back(key, value);
  return fields;
}

Fields Without(Fields fields, const std::string& key) {
  for (size_t i = 0; i < fields.size(); ++i)
    if (fields[i].first == key) fields.erase(fields.begin() + long(i));
  return fields;
}

// ---- base64url ------------------------------------------------------------

std::string B64(const std::string& bytes) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  uint32_t buffer = 0;
  int bits = 0;
  for (unsigned char c : bytes) {
    buffer = (buffer << 8) | c;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out += alphabet[(buffer >> bits) & 63];
    }
  }
  if (bits) out += alphabet[(buffer << (6 - bits)) & 63];
  return out;
}

bool Unb64(const std::string& text, std::string* out) {
  out->clear();
  uint32_t buffer = 0;
  int bits = 0;
  for (char c : text) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '-') v = 62;
    else if (c == '_') v = 63;
    else return false;
    buffer = (buffer << 6) | uint32_t(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      *out += char((buffer >> bits) & 0xff);
    }
  }
  return true;
}

std::vector<std::string> Split(const std::string& text, char separator) {
  std::vector<std::string> parts(1);
  for (char c : text) {
    if (c == separator) parts.emplace_back();
    else parts.back() += c;
  }
  return parts;
}

// Verifies a compact RS256 JWS with the fixture public key; returns the
// decoded header and claims.
bool VerifyAssertion(const std::string& jwt, std::string* header, std::string* claims,
                     const char* public_key = "rsa2048.pub.pem") {
  const std::vector<std::string> parts = Split(jwt, '.');
  std::string signature;
  if (parts.size() != 3 || !Unb64(parts[0], header) || !Unb64(parts[1], claims) ||
      !Unb64(parts[2], &signature))
    return false;
  // Canonical unpadded base64url: re-encoding gives the same text.
  if (B64(*header) != parts[0] || B64(*claims) != parts[1] || B64(signature) != parts[2])
    return false;
  const std::string pem = ReadFile(g_fixture + "/" + public_key);
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  bool ok = mbedtls_pk_parse_public_key(&key, reinterpret_cast<const unsigned char*>(pem.c_str()),
                                        pem.size() + 1) == 0;
  unsigned char hash[32];
  const std::string input = parts[0] + "." + parts[1];
  ok = ok &&
       mbedtls_sha256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash,
                      0) == 0 &&
       mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, hash, 32,
                         reinterpret_cast<const unsigned char*>(signature.data()),
                         signature.size()) == 0;
  mbedtls_pk_free(&key);
  return ok;
}

// ---- Verdicts -------------------------------------------------------------

uint8_t g_certificate[32];
std::string g_certificate_b64;

std::string Hash(const char* challenge, const char* app_version) {
  char output[kPlayRequestHashChars + 1];
  CHECK(ComputePlayIntegrityRequestHash(challenge, strlen(challenge), "android", 7,
                                        kPlayIntegrityPackageName,
                                        strlen(kPlayIntegrityPackageName), app_version,
                                        strlen(app_version), output) ==
        PlayRequestHashStatus::Ok);
  return output;
}

struct VerdictSpec {
  std::string hash = Hash(kChallenge, kAppVersion);
  int64_t timestamp_ms = kWallStart - 1000;
  std::string certificate = g_certificate_b64;
  std::string version_code = "42";
  std::string app_verdict = "PLAY_RECOGNIZED";
  std::string device = "\"MEETS_DEVICE_INTEGRITY\",\"MEETS_STRONG_INTEGRITY\"";
  std::string licensing = "LICENSED";
};

std::string Verdict(const VerdictSpec& spec = VerdictSpec()) {
  return "{\"tokenPayloadExternal\":{\"requestDetails\":{\"requestPackageName\":"
         "\"io.fortisnet.wallet\",\"requestHash\":\"" + spec.hash +
         "\",\"timestampMillis\":\"" + std::to_string(spec.timestamp_ms) +
         "\"},\"appIntegrity\":{\"appRecognitionVerdict\":\"" + spec.app_verdict +
         "\",\"packageName\":\"io.fortisnet.wallet\",\"certificateSha256Digest\":[\"" +
         spec.certificate + "\"],\"versionCode\":\"" + spec.version_code +
         "\"},\"deviceIntegrity\":{\"deviceRecognitionVerdict\":[" + spec.device +
         "]},\"accountDetails\":{\"appLicensingVerdict\":\"" + spec.licensing + "\"}}}";
}

// ---- Injected transport and clocks ---------------------------------------

struct Recorded {
  OutboundMethod method;
  std::string host, port, path, ca_file, content_type, body, bearer;
  uint32_t timeout_ms;
  size_t max_response_bytes;
  bool allow_address, plaintext, require_all;
  size_t header_count;
  bool headers_valid;
};

struct Reply {
  OutboundError error = OutboundError::kNone;
  int status = 200;
  std::string content_type = "application/json; charset=UTF-8";
  std::string body;
  int delay_ms = 0;
};

std::atomic<int64_t> g_wall{kWallStart};
std::atomic<int64_t> g_mono{1000000};
struct TestClock final : fortis::base::Clock {
  int64_t NowMs() override { return g_wall.load(); }
  uint64_t MonotonicMs() override { return static_cast<uint64_t>(g_mono.load()); }
} g_clock;

struct Fake {
  std::mutex mutex;
  std::vector<Recorded> oauth, decode;
  std::function<Reply(const Recorded&, size_t index)> on_oauth, on_decode;
  int issued = 0;
  std::string last_issued;
  std::atomic<int> decode_active{0}, decode_peak{0};
};

Reply DefaultOAuth(Fake* fake, const Recorded& request) {
  static const char prefix[] =
      "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Ajwt-bearer&assertion=";
  std::string header, claims;
  if (request.body.compare(0, sizeof(prefix) - 1, prefix) != 0 ||
      !VerifyAssertion(request.body.substr(sizeof(prefix) - 1), &header, &claims))
    return {OutboundError::kNone, 400, "application/json", "{\"error\":\"invalid_grant\"}"};
  std::lock_guard<std::mutex> lock(fake->mutex);
  fake->last_issued = "ya29.c.token-" + std::to_string(++fake->issued) + "_~+/==";
  return {OutboundError::kNone, 200, "application/json; charset=utf-8",
          "{\"access_token\":\"" + fake->last_issued +
              "\",\"expires_in\":3599,\"token_type\":\"Bearer\"}"};
}

OutboundResponse FakeTransport(void* context, const OutboundRequest& request,
                               unsigned char* body, size_t capacity) {
  Fake* fake = static_cast<Fake*>(context);
  Recorded r{request.method,
             request.host ? request.host : "",
             request.port ? request.port : "",
             request.path ? request.path : "",
             request.ca_file ? request.ca_file : "",
             request.content_type,
             std::string(reinterpret_cast<const char*>(request.body), request.body_length),
             request.authorization_bearer ? request.authorization_bearer : "",
             request.timeout_ms,
             request.max_response_bytes,
             request.allow_address != nullptr,
             request.plaintext,
             request.require_all_addresses_allowed,
             request.header_count,
             OutboundHttpsHeadersValid(request)};
  const bool is_oauth = r.host == kGoogleOAuthHost;
  size_t index;
  {
    std::lock_guard<std::mutex> lock(fake->mutex);
    auto& list = is_oauth ? fake->oauth : fake->decode;
    index = list.size();
    list.push_back(r);
  }
  if (!is_oauth) {
    const int active = ++fake->decode_active;
    int peak = fake->decode_peak.load();
    while (active > peak && !fake->decode_peak.compare_exchange_weak(peak, active)) {}
  }
  Reply reply;
  if (is_oauth) reply = fake->on_oauth ? fake->on_oauth(r, index) : DefaultOAuth(fake, r);
  else if (fake->on_decode) reply = fake->on_decode(r, index);
  else reply.body = Verdict();
  if (reply.delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(reply.delay_ms));
  if (!is_oauth) --fake->decode_active;
  OutboundResponse response{};
  response.error = reply.error;
  if (reply.error != OutboundError::kNone) return response;
  response.status = reply.status;
  if (!reply.content_type.empty()) {
    response.has_content_type = true;
    snprintf(response.content_type, sizeof(response.content_type), "%s",
             reply.content_type.c_str());
  }
  if (reply.body.size() > request.max_response_bytes || reply.body.size() > capacity) {
    response.error = OutboundError::kTooLarge;
    return response;
  }
  std::copy_n(reply.body.data(), reply.body.size(), body);
  response.body_length = reply.body.size();
  return response;
}

const uint8_t (*Certificates())[32] {
  static uint8_t list[1][32];
  memcpy(list[0], g_certificate, 32);
  return list;
}

PlayIntegrityGoogleConfig Config(Fake* fake, const std::string& credential) {
  PlayIntegrityGoogleConfig config;
  config.credential_path = credential.c_str();
  config.ca_file = nullptr;
  config.policy.certificates = Certificates();
  config.policy.certificate_count = 1;
  config.policy.min_version_code = 40;
  config.test.transport = FakeTransport;
  config.test.transport_context = fake;
  config.test.clock = &g_clock;
  return config;
}

std::string g_good_account;
std::string g_ca;

struct Harness {
  Fake fake;
  PlayIntegrityGoogleClient client;
  explicit Harness(bool initialize = true) {
    g_wall = kWallStart;
    g_mono = 1000000;
    if (!initialize) return;
    PlayIntegrityGoogleConfig config = Config(&fake, g_good_account);
    config.ca_file = g_ca.c_str();
    ServiceAccountStatus credential;
    CHECK(client.Initialize(config, &credential));
    CHECK(credential == ServiceAccountStatus::Ok);
  }
};

std::atomic<unsigned> g_token_counter{0};
std::string FreshToken() {
  // JWE-compact-like: five base64url segments.
  return "eyJhbGciOiJBMjU2S1ciLCJlbmMiOiJBMjU2R0NNIn0.token" +
         std::to_string(++g_token_counter) + ".iv_-.cipher.tag";
}

PlayDecodeResult Decode(PlayIntegrityGoogleClient* client, const std::string& token,
                        const char* challenge = kChallenge,
                        const char* app_version = kAppVersion) {
  PlayDecodeInput input;
  input.challenge = challenge;
  input.challenge_length = strlen(challenge);
  input.app_version = app_version;
  input.app_version_length = strlen(app_version);
  input.integrity_token = token.data();
  input.integrity_token_length = token.size();
  return client->Decode(input);
}

// ---- Groups -----------------------------------------------------------------

void TestServiceAccountFiles() {
  const std::string good = WriteFile("good.json", Object(AccountFields()));
  CHECK_ACCOUNT(good, ServiceAccountStatus::Ok);
  {
    PlayServiceAccount account;
    CHECK(LoadPlayServiceAccount(good.c_str(), &account) == ServiceAccountStatus::Ok);
    CHECK(strcmp(account.client_email(), kEmail) == 0);
    CHECK(strcmp(account.key_id(), kKeyId) == 0);
    account.Clear();
    CHECK(!account.loaded() && account.client_email()[0] == '\0');
  }
  CHECK_ACCOUNT(WriteFile("rsa3072.json", Object(AccountFields("rsa3072.pem"))),
                ServiceAccountStatus::Ok);
  // Minimal file: only the members the loader needs.
  CHECK_ACCOUNT(WriteFile("minimal.json",
                          Object(Without(Without(Without(Without(AccountFields(), "token_uri"),
                                                         "universe_domain"),
                                                 "project_id"),
                                         "auth_uri"))),
                ServiceAccountStatus::Ok);
  CHECK_ACCOUNT(WriteFile("owner-read.json", Object(AccountFields()), 0400),
                ServiceAccountStatus::Ok);

  // Location, type, ownership, permissions and size.
  CHECK(LoadPlayServiceAccount(nullptr, nullptr) == ServiceAccountStatus::NotConfigured);
  {
    PlayServiceAccount account;
    CHECK(LoadPlayServiceAccount("", &account) == ServiceAccountStatus::NotConfigured);
    CHECK(LoadPlayServiceAccount(nullptr, &account) == ServiceAccountStatus::NotConfigured);
  }
  CHECK_ACCOUNT(g_accounts + "/missing.json", ServiceAccountStatus::OpenFailed);
  const std::string link = g_accounts + "/link.json";
  unlink(link.c_str());
  CHECK(symlink(good.c_str(), link.c_str()) == 0);
  CHECK_ACCOUNT(link, ServiceAccountStatus::OpenFailed);
  CHECK_ACCOUNT(g_accounts, ServiceAccountStatus::NotRegularFile);
  for (mode_t mode : {0640, 0604, 0660, 0644, 0610, 0601, 0700 | 0004})
    CHECK_ACCOUNT(WriteFile("open.json", Object(AccountFields()), mode),
                  ServiceAccountStatus::PermissionsTooOpen);
  CHECK_ACCOUNT(WriteFile("empty.json", ""), ServiceAccountStatus::SizeInvalid);
  {
    std::string padded = Object(AccountFields());
    padded.insert(1, std::string(kServiceAccountMaxFileBytes - padded.size(), ' '));
    CHECK(padded.size() == kServiceAccountMaxFileBytes);
    CHECK_ACCOUNT(WriteFile("max.json", padded), ServiceAccountStatus::Ok);
    padded.insert(1, " ");
    CHECK_ACCOUNT(WriteFile("over.json", padded), ServiceAccountStatus::SizeInvalid);
  }

  // JSON structure and duplicates.
  const std::string object = Object(AccountFields());
  for (const std::string& bad :
       {std::string("not json"), object + "x", object + "{}", std::string("[") + object + "]",
        object.substr(0, object.size() - 1), std::string("\"service_account\"")})
    CHECK_ACCOUNT(WriteFile("bad.json", bad), ServiceAccountStatus::Malformed);
  for (const char* key : {"type", "client_email", "private_key_id", "private_key", "token_uri",
                          "universe_domain"}) {
    Fields fields = AccountFields();
    for (const auto& field : AccountFields())
      if (field.first == key) fields.emplace_back(field);
    CHECK_ACCOUNT(WriteFile("duplicate.json", Object(fields)), ServiceAccountStatus::Malformed);
    // Wrong JSON type for a read member.
    CHECK_ACCOUNT(WriteFile("typed.json", Object(With(AccountFields(), key, "42"))),
                  ServiceAccountStatus::Malformed);
  }
  {
    // A duplicate spelled with an escape is still a duplicate.
    std::string escaped = object;
    escaped.insert(escaped.size() - 1,
                   ",\"private\\u005fkey\":" + JsonString(ReadFile(g_fixture + "/rsa1024.pem")));
    CHECK_ACCOUNT(WriteFile("escaped.json", escaped), ServiceAccountStatus::Malformed);
  }

  CHECK_ACCOUNT(WriteFile("type.json", Object(With(AccountFields(), "type", "\"user\""))),
                ServiceAccountStatus::TypeInvalid);
  CHECK_ACCOUNT(WriteFile("type.json", Object(Without(AccountFields(), "type"))),
                ServiceAccountStatus::TypeInvalid);
  for (const std::string& email :
       {std::string("\"\""), std::string("\"someone@example.com\""),
        std::string("\"@fortis.iam.gserviceaccount.com\""),
        std::string("\"a@b@fortis.iam.gserviceaccount.com\""),
        std::string("\"a\\\"b@fortis.iam.gserviceaccount.com\""),
        std::string("\"a@.gserviceaccount.com\""), std::string("\"a@gserviceaccount.com\""),
        std::string("\"a b@fortis.iam.gserviceaccount.com\""),
        "\"" + std::string(65, 'a') + "@fortis.iam.gserviceaccount.com\"",
        "\"a@" + std::string(240, 'b') + ".iam.gserviceaccount.com\""})
    CHECK_ACCOUNT(WriteFile("email.json", Object(With(AccountFields(), "client_email", email))),
                  ServiceAccountStatus::EmailInvalid);
  CHECK_ACCOUNT(WriteFile("email.json", Object(Without(AccountFields(), "client_email"))),
                ServiceAccountStatus::EmailInvalid);
  CHECK_ACCOUNT(WriteFile("email.json",
                          Object(With(AccountFields(), "client_email",
                                      "\"legacy-123@developer.gserviceaccount.com\""))),
                ServiceAccountStatus::Ok);
  for (const std::string& id : {std::string("\"\""), std::string("\"a\\\"b\""),
                                std::string("\"a b\""), "\"" + std::string(65, 'a') + "\""})
    CHECK_ACCOUNT(WriteFile("kid.json", Object(With(AccountFields(), "private_key_id", id))),
                  ServiceAccountStatus::KeyIdInvalid);
  CHECK_ACCOUNT(WriteFile("kid.json", Object(Without(AccountFields(), "private_key_id"))),
                ServiceAccountStatus::KeyIdInvalid);
  for (const std::string& uri :
       {std::string("\"https://oauth2.example.com/token\""),
        std::string("\"https://oauth2.googleapis.com/token/\""),
        std::string("\"http://oauth2.googleapis.com/token\""),
        std::string("\"https://accounts.google.com/o/oauth2/token\"")})
    CHECK_ACCOUNT(WriteFile("uri.json", Object(With(AccountFields(), "token_uri", uri))),
                  ServiceAccountStatus::EndpointMismatch);
  CHECK_ACCOUNT(WriteFile("universe.json",
                          Object(With(AccountFields(), "universe_domain", "\"example.com\""))),
                ServiceAccountStatus::EndpointMismatch);

  // Keys: RSA PKCS#8 of 2048-4096 bits only, and it must work.
  for (const char* pem : {"rsa1024.pem", "ec.pem", "rsa_pkcs1.pem", "encrypted.pem"})
    CHECK_ACCOUNT(WriteFile("key.json", Object(AccountFields(pem))),
                  ServiceAccountStatus::PrivateKeyInvalid);
  CHECK_ACCOUNT(WriteFile("key.json", Object(Without(AccountFields(), "private_key"))),
                ServiceAccountStatus::PrivateKeyInvalid);
  {
    std::string pem = ReadFile(g_fixture + "/rsa2048.pem");
    std::string corrupt = pem;
    corrupt[pem.size() / 2] = corrupt[pem.size() / 2] == 'A' ? 'B' : 'A';
    CHECK_ACCOUNT(WriteFile("key.json", Object(With(AccountFields(), "private_key",
                                                    JsonString(corrupt)))),
                  ServiceAccountStatus::PrivateKeyInvalid);
    CHECK_ACCOUNT(WriteFile("key.json", Object(With(AccountFields(), "private_key",
                                                    JsonString("x" + pem)))),
                  ServiceAccountStatus::PrivateKeyInvalid);
    CHECK_ACCOUNT(WriteFile("key.json", Object(With(AccountFields(), "private_key",
                                                    JsonString(pem + std::string(6600, '\n'))))),
                  ServiceAccountStatus::PrivateKeyInvalid);
  }
  printf("service account: location, ownership, mode, size, JSON, duplicates, fields, keys\n");
}

void TestAssertion() {
  PlayServiceAccount account;
  char jwt[kPlayOAuthAssertionMaxBytes + 1];
  size_t length = 99;
  CHECK(!BuildPlayOAuthAssertion(account, 1761652800, jwt, sizeof(jwt), &length));
  CHECK(length == 0 && jwt[0] == '\0');
  CHECK(LoadPlayServiceAccount(g_good_account.c_str(), &account) == ServiceAccountStatus::Ok);
  CHECK(BuildPlayOAuthAssertion(account, 1761652800, jwt, sizeof(jwt), &length));
  CHECK(length == strlen(jwt));
  std::string header, claims;
  CHECK(VerifyAssertion(jwt, &header, &claims));
  CHECK(header == std::string("{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"") + kKeyId + "\"}");
  CHECK(claims == std::string("{\"iss\":\"") + kEmail +
                      "\",\"scope\":\"https://www.googleapis.com/auth/playintegrity\","
                      "\"aud\":\"https://oauth2.googleapis.com/token\","
                      "\"exp\":1761653100,\"iat\":1761652800}");
  // RS256 PKCS#1 v1.5 is deterministic: the same instant signs identically.
  char again[kPlayOAuthAssertionMaxBytes + 1];
  size_t again_length = 0;
  CHECK(BuildPlayOAuthAssertion(account, 1761652800, again, sizeof(again), &again_length));
  CHECK(strcmp(jwt, again) == 0);
  // Any changed byte breaks the signature.
  std::string tampered = jwt;
  const size_t dot = tampered.find('.');
  tampered[dot + 5] = tampered[dot + 5] == 'A' ? 'B' : 'A';
  CHECK(!VerifyAssertion(tampered, &header, &claims));
  CHECK(!VerifyAssertion(jwt, &header, &claims, "rsa3072.pub.pem"));
  // A 3072-bit signature is longer and still fits.
  PlayServiceAccount large;
  CHECK(LoadPlayServiceAccount(WriteFile("rsa3072.json", Object(AccountFields("rsa3072.pem")))
                                   .c_str(),
                               &large) == ServiceAccountStatus::Ok);
  CHECK(BuildPlayOAuthAssertion(large, 1761652800, jwt, sizeof(jwt), &length));
  CHECK(VerifyAssertion(jwt, &header, &claims, "rsa3072.pub.pem"));
  // Clocks and buffers.
  CHECK(!BuildPlayOAuthAssertion(account, 0, jwt, sizeof(jwt), &length));
  CHECK(!BuildPlayOAuthAssertion(account, -5, jwt, sizeof(jwt), &length));
  CHECK(!BuildPlayOAuthAssertion(account, 253402300799, jwt, sizeof(jwt), &length));
  CHECK(!BuildPlayOAuthAssertion(account, 1761652800, jwt, 600, &length));
  CHECK(length == 0 && jwt[0] == '\0');
  CHECK(!BuildPlayOAuthAssertion(account, 1761652800, nullptr, 0, &length));
  printf("assertion: exact header and claims, RS256 verifies, tamper and bounds refused\n");
}

void TestOAuthParse() {
  char token[kGoogleAccessTokenMaxBytes + 1];
  size_t length = 0;
  int64_t expires = 0;
  auto parse = [&](const std::string& body) {
    return ParseGoogleOAuthResponse(reinterpret_cast<const unsigned char*>(body.data()),
                                    body.size(), token, &length, &expires);
  };
  CHECK(parse("{\"access_token\":\"ya29.a0AfB_-x~+/==\",\"expires_in\":3599,"
              "\"token_type\":\"Bearer\"}") == GoogleOAuthParse::Ok);
  CHECK(strcmp(token, "ya29.a0AfB_-x~+/==") == 0 && length == 18 && expires == 3599);
  CHECK(parse("{\"token_type\":\"bearer\",\"scope\":\"x\",\"expires_in\":60,"
              "\"access_token\":\"t\",\"id_token\":\"ignored\"}") == GoogleOAuthParse::Ok);
  CHECK(parse("{\"access_token\":\"" + std::string(2048, 'a') +
              "\",\"expires_in\":86400,\"token_type\":\"Bearer\"}") == GoogleOAuthParse::Ok);

  // An error member is never a token, even next to one.
  CHECK(parse("{\"error\":\"invalid_grant\",\"error_description\":\"Invalid JWT\"}") ==
        GoogleOAuthParse::ErrorResponse);
  CHECK(parse("{\"access_token\":\"t\",\"expires_in\":3599,\"token_type\":\"Bearer\","
              "\"error\":{}}") == GoogleOAuthParse::ErrorResponse);
  CHECK(token[0] == '\0' && length == 0);

  const std::string good_tail = "\",\"expires_in\":3599,\"token_type\":\"Bearer\"}";
  for (const std::string& bad : {
           std::string(""), std::string("[]"), std::string("null"), std::string("{"),
           std::string("{\"expires_in\":3599,\"token_type\":\"Bearer\"}"),
           std::string("{\"access_token\":\"t\",\"token_type\":\"Bearer\"}"),
           std::string("{\"access_token\":\"t\",\"expires_in\":3599}"),
           "{\"access_token\":\"\"" + good_tail.substr(1),
           "{\"access_token\":\"" + std::string(2049, 'a') + good_tail,
           "{\"access_token\":\"a b" + good_tail, "{\"access_token\":\"a\\r\\nX: y" + good_tail,
           "{\"access_token\":\"a\\\"b" + good_tail, "{\"access_token\":\"=a" + good_tail,
           "{\"access_token\":\"a=b" + good_tail, "{\"access_token\":\"a\\u0000" + good_tail,
           "{\"access_token\":\"a,b" + good_tail, "{\"access_token\":\"\\u00e9" + good_tail,
           std::string("{\"access_token\":7,\"expires_in\":3599,\"token_type\":\"Bearer\"}"),
           std::string("{\"access_token\":\"t\",\"expires_in\":3599,\"token_type\":\"MAC\"}"),
           std::string("{\"access_token\":\"t\",\"expires_in\":3599,\"token_type\":\"Bearer \"}"),
           std::string("{\"access_token\":\"t\",\"expires_in\":\"3599\",\"token_type\":\"Bearer\"}"),
           std::string("{\"access_token\":\"t\",\"expires_in\":3599.0,\"token_type\":\"Bearer\"}"),
           std::string("{\"access_token\":\"t\",\"expires_in\":3.599e3,\"token_type\":\"Bearer\"}"),
           std::string("{\"access_token\":\"t\",\"expires_in\":-3599,\"token_type\":\"Bearer\"}"),
           std::string("{\"access_token\":\"t\",\"expires_in\":03599,\"token_type\":\"Bearer\"}"),
           std::string("{\"access_token\":\"t\",\"expires_in\":59,\"token_type\":\"Bearer\"}"),
           std::string("{\"access_token\":\"t\",\"expires_in\":86401,\"token_type\":\"Bearer\"}"),
           std::string("{\"access_token\":\"t\",\"expires_in\":9999999,\"token_type\":\"Bearer\"}"),
           std::string("{\"access_token\":\"t\",\"access_token\":\"u\",\"expires_in\":3599,"
                       "\"token_type\":\"Bearer\"}"),
           std::string("{\"access_token\":\"t\",\"access\\u005ftoken\":\"u\",\"expires_in\":3599,"
                       "\"token_type\":\"Bearer\"}"),
           std::string("{\"access_token\":\"t\",\"expires_in\":3599,\"expires_in\":3599,"
                       "\"token_type\":\"Bearer\"}"),
           "{\"access_token\":\"t" + good_tail + "x",
       }) {
    if (parse(bad) != GoogleOAuthParse::Malformed) {
      fprintf(stderr, "accepted: %.120s\n", bad.c_str());
      abort();
    }
    CHECK(token[0] == '\0' && length == 0 && expires == 0);
  }
  std::string large = "{\"access_token\":\"t\",\"expires_in\":3599,\"token_type\":\"Bearer\","
                      "\"pad\":\"";
  large += std::string(kGoogleOAuthResponseMaxBytes - large.size() - 2, 'p') + "\"}";
  CHECK(large.size() == kGoogleOAuthResponseMaxBytes && parse(large) == GoogleOAuthParse::Ok);
  large.insert(large.size() - 2, "p");
  CHECK(parse(large) == GoogleOAuthParse::Malformed);
  CHECK(ParseGoogleOAuthResponse(nullptr, 4, token, &length, &expires) ==
        GoogleOAuthParse::Malformed);
  printf("oauth response: bearer, bounds, types, duplicates and error bodies\n");
}

// The bearer extension of OutboundHttpsRequest: refused before any I/O.
void TestOutboundBearer() {
  unsigned char body[64];
  auto attempt = [&](const char* bearer, bool plaintext = false, const char* custom = nullptr) {
    OutboundRequest request{};
    request.method = OutboundMethod::kPostJson;
    request.host = "playintegrity.invalid";
    request.path = "/";
    request.ca_file = "/nonexistent/fortis-ca.pem";
    request.timeout_ms = 1000;
    request.max_response_bytes = sizeof(body);
    request.plaintext = plaintext;
    if (plaintext) request.ca_file = nullptr;
    request.authorization_bearer = bearer;
    if (custom) {
      request.header_count = 1;
      snprintf(request.headers[0].name, sizeof(request.headers[0].name), "%s", custom);
      snprintf(request.headers[0].value, sizeof(request.headers[0].value), "x");
    }
    return OutboundHttpsRequest(request, body, sizeof(body)).error;
  };
  const std::string max(kOutboundMaxBearerBytes, 'a');
  for (const std::string& bad :
       {std::string(""), std::string("a b"), std::string("a\r\nHost: evil"), std::string("a\n"),
        std::string("a\tb"), std::string("=abc"), std::string("ab=c"), std::string("a\"b"),
        std::string("a,b"), std::string("a\x7f"), std::string("\xc3\xa9"), max + "a"})
    CHECK(attempt(bad.c_str()) == OutboundError::kInvalidRequest);
  // Valid bearers pass validation and fail later, at the missing CA file.
  for (const std::string& good : {std::string("ya29.a0-_~+/=="), max, std::string("a")})
    CHECK(attempt(good.c_str()) == OutboundError::kTooLarge);
  CHECK(attempt("token", true) == OutboundError::kInvalidRequest);
  CHECK(attempt("token", false, "Authorization") == OutboundError::kInvalidRequest);
  CHECK(attempt("token", false, "aUTHORIZATION") == OutboundError::kInvalidRequest);
  CHECK(attempt(nullptr, false, "Authorization") == OutboundError::kTooLarge);
  printf("outbound bearer: b64token only, no CRLF, TLS only, no duplicate Authorization\n");
}

void TestInitialize() {
  Fake fake;
  {
    PlayIntegrityGoogleClient client;
    CHECK_DECODE(Decode(&client, FreshToken()).status, PlayDecodeStatus::NotConfigured);
    PlayIntegrityGoogleConfig config = Config(&fake, g_good_account);
    ServiceAccountStatus credential = ServiceAccountStatus::Ok;
    CHECK(!client.Initialize(config, &credential));  // no CA bundle
    CHECK(credential == ServiceAccountStatus::NotConfigured);
    config.ca_file = "/nonexistent/ca.pem";
    CHECK(!client.Initialize(config, &credential));
    config.ca_file = g_ca.c_str();
    config.credential_path = nullptr;
    CHECK(!client.Initialize(config, &credential));
    CHECK(credential == ServiceAccountStatus::NotConfigured);
    const std::string open = WriteFile("init-open.json", Object(AccountFields()), 0644);
    config.credential_path = open.c_str();
    CHECK(!client.Initialize(config, &credential));
    CHECK(credential == ServiceAccountStatus::PermissionsTooOpen);
    CHECK_DECODE(Decode(&client, FreshToken()).status, PlayDecodeStatus::NotConfigured);
    CHECK(fake.oauth.empty() && fake.decode.empty());
  }
  auto refused = [&](const std::function<void(PlayIntegrityGoogleConfig*)>& edit) {
    PlayIntegrityGoogleClient client;
    PlayIntegrityGoogleConfig config = Config(&fake, g_good_account);
    config.ca_file = g_ca.c_str();
    edit(&config);
    CHECK(!client.Initialize(config, nullptr));
    CHECK_DECODE(Decode(&client, FreshToken()).status, PlayDecodeStatus::NotConfigured);
  };
  refused([](PlayIntegrityGoogleConfig* c) { c->policy.certificates = nullptr; });
  refused([](PlayIntegrityGoogleConfig* c) { c->policy.certificate_count = 0; });
  refused([](PlayIntegrityGoogleConfig* c) { c->policy.certificate_count = 5; });
  refused([](PlayIntegrityGoogleConfig* c) { c->policy.min_version_code = 0; });
  refused([](PlayIntegrityGoogleConfig* c) { c->policy.allow_testing_response = true; });
  refused([](PlayIntegrityGoogleConfig* c) { c->policy.max_age_ms = 0; });
  refused([](PlayIntegrityGoogleConfig* c) { c->policy.max_future_skew_ms = -1; });
  refused([](PlayIntegrityGoogleConfig* c) {
    c->policy.package_name = "io.fortisnet.wallet2";
    c->policy.package_name_length = 20;
  });
  refused([](PlayIntegrityGoogleConfig* c) {
    c->policy.package_name = "io.fortisnet.wallet";
    c->policy.package_name_length = 18;
  });
  refused([](PlayIntegrityGoogleConfig* c) { c->test.host = ""; });
  refused([](PlayIntegrityGoogleConfig* c) { c->test.host = "localhost"; });  // no port
  {
    Harness harness;
    PlayIntegrityGoogleConfig config = Config(&harness.fake, g_good_account);
    config.ca_file = g_ca.c_str();
    CHECK(!harness.client.Initialize(config, nullptr));  // only once
  }
  CHECK(fake.oauth.empty() && fake.decode.empty());
  printf("initialize: CA, credential and policy required; package fixed; testing refused\n");
}

void TestHappyPath() {
  Harness harness;
  const std::string token = FreshToken();
  const PlayDecodeResult result = Decode(&harness.client, token);
  CHECK_DECODE(result.status, PlayDecodeStatus::Verified);
  CHECK(result.verdict == PlayIntegrityStatus::Verified);
  CHECK(result.evidence.version_code == 42);
  CHECK(result.evidence.timestamp_ms == kWallStart - 1000);
  CHECK(result.evidence.device_label == PlayDeviceLabel::StrongIntegrity);
  CHECK(result.evidence.licensing == PlayLicensing::Licensed);

  CHECK(harness.fake.oauth.size() == 1 && harness.fake.decode.size() == 1);
  const Recorded& oauth = harness.fake.oauth[0];
  CHECK(oauth.method == OutboundMethod::kPostBinary);
  CHECK(oauth.host == "oauth2.googleapis.com" && oauth.port == "443" && oauth.path == "/token");
  CHECK(oauth.content_type == "application/x-www-form-urlencoded");
  CHECK(oauth.ca_file == g_ca && oauth.bearer.empty() && oauth.header_count == 0);
  CHECK(!oauth.allow_address && !oauth.plaintext && oauth.headers_valid);
  CHECK(oauth.timeout_ms > 0 && oauth.timeout_ms <= kPlayOAuthTimeoutMs);
  CHECK(oauth.max_response_bytes == kGoogleOAuthResponseMaxBytes);
  std::string header, claims;
  CHECK(VerifyAssertion(oauth.body.substr(oauth.body.find("&assertion=") + 11), &header, &claims));
  CHECK(claims.find("\"iat\":1761652800}") != std::string::npos);
  CHECK(claims.find("\"exp\":1761653100,") != std::string::npos);

  const Recorded& decode = harness.fake.decode[0];
  CHECK(decode.method == OutboundMethod::kPostJson);
  CHECK(decode.host == "playintegrity.googleapis.com" && decode.port == "443");
  CHECK(decode.path == "/v1/io.fortisnet.wallet:decodeIntegrityToken");
  CHECK(decode.body == "{\"integrity_token\":\"" + token + "\"}");
  CHECK(decode.bearer == harness.fake.last_issued && decode.header_count == 0);
  CHECK(decode.ca_file == g_ca && !decode.allow_address && !decode.plaintext && !decode.require_all);
  CHECK(decode.timeout_ms > 0 && decode.timeout_ms <= kPlayDecodeTimeoutMs);
  CHECK(decode.max_response_bytes == kPlayVerdictMaxBytes);
  CHECK(harness.client.oauth_exchanges() == 1 && harness.client.decode_requests() == 1);
  printf("decode: fixed endpoints, form grant, JSON body, bearer, evidence\n");
}

void TestTokenCache() {
  Harness harness;
  CHECK_DECODE(Decode(&harness.client, FreshToken()).status, PlayDecodeStatus::Verified);
  CHECK_DECODE(Decode(&harness.client, FreshToken()).status, PlayDecodeStatus::Verified);
  CHECK(harness.client.oauth_exchanges() == 1);
  CHECK(harness.fake.decode[1].bearer == "ya29.c.token-1_~+/==");
  // expires_in 3599 s, refreshed 300 s early.
  g_mono += (3599 - 300) * 1000 - 1;
  CHECK_DECODE(Decode(&harness.client, FreshToken()).status, PlayDecodeStatus::Verified);
  CHECK(harness.client.oauth_exchanges() == 1);
  g_mono += 1;
  CHECK_DECODE(Decode(&harness.client, FreshToken()).status, PlayDecodeStatus::Verified);
  CHECK(harness.client.oauth_exchanges() == 2);
  CHECK(harness.fake.decode.back().bearer == "ya29.c.token-2_~+/==");
  // Short lifetimes are refreshed at half-life.
  harness.fake.on_oauth = [&](const Recorded&, size_t) {
    std::lock_guard<std::mutex> lock(harness.fake.mutex);
    harness.fake.last_issued = "short-" + std::to_string(++harness.fake.issued);
    return Reply{OutboundError::kNone, 200, "application/json",
                 "{\"access_token\":\"" + harness.fake.last_issued +
                     "\",\"expires_in\":100,\"token_type\":\"Bearer\"}"};
  };
  g_mono += 3600 * 1000;
  CHECK_DECODE(Decode(&harness.client, FreshToken()).status, PlayDecodeStatus::Verified);
  CHECK(harness.client.oauth_exchanges() == 3);
  g_mono += 49999;
  CHECK_DECODE(Decode(&harness.client, FreshToken()).status, PlayDecodeStatus::Verified);
  CHECK(harness.client.oauth_exchanges() == 3);
  g_mono += 1;
  CHECK_DECODE(Decode(&harness.client, FreshToken()).status, PlayDecodeStatus::Verified);
  CHECK(harness.client.oauth_exchanges() == 4);
  // The JWT is signed with the wall clock at refresh time.
  g_wall += 7200 * 1000;
  g_mono += 3600 * 1000;
  VerdictSpec spec;
  spec.timestamp_ms = g_wall - 10;
  harness.fake.on_decode = [&](const Recorded&, size_t) { return Reply{OutboundError::kNone, 200, "application/json", Verdict(spec)}; };
  CHECK_DECODE(Decode(&harness.client, FreshToken()).status, PlayDecodeStatus::Verified);
  std::string header, claims;
  const Recorded& last = harness.fake.oauth.back();
  CHECK(VerifyAssertion(last.body.substr(last.body.find("&assertion=") + 11), &header, &claims));
  CHECK(claims.find("\"iat\":1761660000}") != std::string::npos);
  printf("token cache: reuse, conservative margin, half-life for short tokens\n");
}

// Concurrent callers share one refresh; the decode cap holds.
void TestRefreshRace() {
  Harness harness;
  std::atomic<int> oauth_active{0};
  std::atomic<bool> overlap{false};
  harness.fake.on_oauth = [&](const Recorded& r, size_t) {
    if (++oauth_active > 1) overlap = true;
    const Reply reply = DefaultOAuth(&harness.fake, r);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // waiters pile up
    --oauth_active;
    return reply;
  };
  for (int round = 0; round < 5; ++round) {
    g_mono += 4000 * 1000;  // past every refresh point
    std::vector<std::thread> threads;
    std::atomic<int> verified{0};
    for (unsigned i = 0; i < kPlayDecodeMaxConcurrent; ++i)
      threads.emplace_back([&] {
        if (Decode(&harness.client, FreshToken()).status == PlayDecodeStatus::Verified) ++verified;
      });
    for (auto& thread : threads) thread.join();
    CHECK(verified == int(kPlayDecodeMaxConcurrent));
    CHECK(harness.client.oauth_exchanges() == uint64_t(round + 1));
  }
  CHECK(!overlap);
  // Every decode carried the token that was current when it ran.
  for (const Recorded& r : harness.fake.decode) CHECK(r.bearer.rfind("ya29.c.token-", 0) == 0);

  // A fifth concurrent call is refused without I/O while four are in flight.
  std::mutex gate_mutex;
  std::condition_variable gate;
  bool open = false;
  std::atomic<int> waiting{0};
  harness.fake.on_decode = [&](const Recorded&, size_t) {
    ++waiting;
    std::unique_lock<std::mutex> lock(gate_mutex);
    gate.wait(lock, [&] { return open; });
    return Reply{OutboundError::kNone, 200, "application/json", Verdict()};
  };
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < kPlayDecodeMaxConcurrent; ++i)
    threads.emplace_back([&] { Decode(&harness.client, FreshToken()); });
  while (waiting < int(kPlayDecodeMaxConcurrent)) std::this_thread::yield();
  const size_t before = harness.fake.decode.size();
  // Run the extra call on a thread. If the cap is accidentally removed, it
  // will enter the gated transport; release the gate before asserting so a
  // mutant fails instead of deadlocking this test.
  std::atomic<int> fifth_status{-1};
  std::thread fifth([&] {
    fifth_status.store(static_cast<int>(Decode(&harness.client, FreshToken()).status));
  });
  const auto fifth_deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(200);
  while (fifth_status.load() == -1 && std::chrono::steady_clock::now() < fifth_deadline)
    std::this_thread::yield();
  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    open = true;
  }
  gate.notify_all();
  fifth.join();
  for (auto& thread : threads) thread.join();
  CHECK_DECODE(static_cast<PlayDecodeStatus>(fifth_status.load()), PlayDecodeStatus::Busy);
  CHECK(harness.fake.decode.size() == before);
  CHECK(harness.fake.decode_peak.load() == int(kPlayDecodeMaxConcurrent));
  CHECK_DECODE(Decode(&harness.client, FreshToken()).status, PlayDecodeStatus::Verified);
  printf("refresh race: one exchange per expiry across %u callers x 5; cap %u holds\n",
         kPlayDecodeMaxConcurrent, kPlayDecodeMaxConcurrent);
}

void TestOAuthFailures() {
  struct Case {
    Reply reply;
    PlayDecodeStatus expected;
  };
  const Case cases[] = {
      {{OutboundError::kNone, 500, "application/json", "{}"}, PlayDecodeStatus::OAuthUnavailable},
      {{OutboundError::kNone, 503, "", ""}, PlayDecodeStatus::OAuthUnavailable},
      {{OutboundError::kNone, 429, "application/json", "{}"}, PlayDecodeStatus::OAuthUnavailable},
      {{OutboundError::kTimeout, 0, "", ""}, PlayDecodeStatus::OAuthUnavailable},
      {{OutboundError::kTls, 0, "", ""}, PlayDecodeStatus::OAuthUnavailable},
      {{OutboundError::kTooLarge, 0, "", ""}, PlayDecodeStatus::OAuthUnavailable},
      {{OutboundError::kNone, 400, "application/json",
        "{\"error\":\"invalid_grant\",\"error_description\":\"Invalid JWT Signature.\"}"},
       PlayDecodeStatus::OAuthRejected},
      {{OutboundError::kNone, 401, "application/json", "{\"error\":\"invalid_client\"}"},
       PlayDecodeStatus::OAuthRejected},
      {{OutboundError::kNone, 302, "text/html", ""}, PlayDecodeStatus::OAuthRejected},
      // 2xx with an error body, or a token the client cannot use.
      {{OutboundError::kNone, 200, "application/json",
        "{\"error\":\"invalid_scope\",\"access_token\":\"t\",\"expires_in\":3599,"
        "\"token_type\":\"Bearer\"}"},
       PlayDecodeStatus::OAuthRejected},
      {{OutboundError::kNone, 200, "application/json",
        "{\"access_token\":\"t\",\"expires_in\":3599,\"token_type\":\"MAC\"}"},
       PlayDecodeStatus::OAuthMalformed},
      {{OutboundError::kNone, 200, "text/html",
        "{\"access_token\":\"t\",\"expires_in\":3599,\"token_type\":\"Bearer\"}"},
       PlayDecodeStatus::OAuthMalformed},
      {{OutboundError::kNone, 200, "application/json", "<html>"}, PlayDecodeStatus::OAuthMalformed},
  };
  for (const Case& c : cases) {
    Harness harness;
    harness.fake.on_oauth = [&](const Recorded&, size_t) { return c.reply; };
    CHECK_DECODE(Decode(&harness.client, FreshToken()).status, c.expected);
    CHECK(harness.fake.decode.empty());
    // Inside the backoff the failure is reused without another exchange.
    g_mono += kPlayOAuthFailureBackoffMs - 1;
    CHECK_DECODE(Decode(&harness.client, FreshToken()).status, c.expected);
    CHECK(harness.client.oauth_exchanges() == 1 && harness.fake.oauth.size() == 1);
    g_mono += 1;
    harness.fake.on_oauth = nullptr;
    CHECK_DECODE(Decode(&harness.client, FreshToken()).status, PlayDecodeStatus::Verified);
    CHECK(harness.client.oauth_exchanges() == 2);
  }
  printf("oauth failures: unavailable, rejected and malformed statuses; backoff; recovery\n");
}

void TestUnauthorizedRefresh() {
  {
    Harness harness;
    harness.fake.on_decode = [&](const Recorded&, size_t index) {
      return index == 0 ? Reply{OutboundError::kNone, 401, "application/json",
                                "{\"error\":{\"code\":401,\"status\":\"UNAUTHENTICATED\"}}"}
                        : Reply{OutboundError::kNone, 200, "application/json", Verdict()};
    };
    CHECK_DECODE(Decode(&harness.client, FreshToken()).status, PlayDecodeStatus::Verified);
    CHECK(harness.client.oauth_exchanges() == 2 && harness.fake.decode.size() == 2);
    CHECK(harness.fake.decode[0].bearer == "ya29.c.token-1_~+/==");
    CHECK(harness.fake.decode[1].bearer == "ya29.c.token-2_~+/==");
    CHECK(harness.fake.decode[0].body == harness.fake.decode[1].body);
  }
  {
    Harness harness;
    harness.fake.on_decode = [&](const Recorded&, size_t) {
      return Reply{OutboundError::kNone, 401, "application/json", "{}"};
    };
    CHECK_DECODE(Decode(&harness.client, FreshToken()).status, PlayDecodeStatus::Unauthorized);
    CHECK(harness.client.oauth_exchanges() == 2 && harness.fake.decode.size() == 2);
    // Only the first refusal invalidates; the second token stays cached and
    // a later call starts with it (and would refresh again on its own 401).
    harness.fake.on_decode = nullptr;
    CHECK_DECODE(Decode(&harness.client, FreshToken()).status, PlayDecodeStatus::Verified);
    CHECK(harness.client.oauth_exchanges() == 2);
  }
  {
    // A late 401 for an older token does not discard the newer one: A and B
    // both send token 1; A's 401 arrives first and A refreshes to token 2;
    // B's 401 arrives 200 ms later and must leave token 2 in place.
    Harness harness;
    std::atomic<int> arrivals{0};
    harness.fake.on_decode = [&](const Recorded& r, size_t) {
      if (r.bearer != "ya29.c.token-1_~+/==")
        return Reply{OutboundError::kNone, 200, "application/json", Verdict()};
      const int arrival = arrivals++;
      while (arrivals < 2) std::this_thread::yield();
      if (arrival == 1) std::this_thread::sleep_for(std::chrono::milliseconds(200));
      return Reply{OutboundError::kNone, 401, "application/json", "{}"};
    };
    std::thread a([&] {
      CHECK_DECODE(Decode(&harness.client, FreshToken()).status, PlayDecodeStatus::Verified);
    });
    std::thread b([&] {
      CHECK_DECODE(Decode(&harness.client, FreshToken()).status, PlayDecodeStatus::Verified);
    });
    a.join();
    b.join();
    CHECK(harness.client.oauth_exchanges() == 2 && harness.fake.decode.size() == 4);
  }
  printf("401: one refresh and one retry; a second 401 is Unauthorized\n");
}

void TestDecodeFailures() {
  struct Case {
    Reply reply;
    PlayDecodeStatus expected;
  };
  const std::string verdict = Verdict();
  const Case cases[] = {
      {{OutboundError::kNone, 400, "application/json",
        "{\"error\":{\"code\":400,\"status\":\"INVALID_ARGUMENT\"}}"},
       PlayDecodeStatus::TokenRejected},
      {{OutboundError::kNone, 403, "application/json", "{}"}, PlayDecodeStatus::PermissionDenied},
      {{OutboundError::kNone, 429, "application/json", "{}"}, PlayDecodeStatus::RateLimited},
      {{OutboundError::kNone, 500, "application/json", verdict}, PlayDecodeStatus::UpstreamUnavailable},
      {{OutboundError::kNone, 503, "", ""}, PlayDecodeStatus::UpstreamUnavailable},
      {{OutboundError::kNone, 302, "application/json", verdict}, PlayDecodeStatus::UpstreamRejected},
      {{OutboundError::kNone, 404, "application/json", verdict}, PlayDecodeStatus::UpstreamRejected},
      {{OutboundError::kNone, 204, "", ""}, PlayDecodeStatus::ResponseMalformed},
      {{OutboundError::kNone, 200, "text/html", verdict}, PlayDecodeStatus::ResponseMalformed},
      {{OutboundError::kNone, 200, "", verdict}, PlayDecodeStatus::ResponseMalformed},
      {{OutboundError::kNone, 200, "application/jsonp", verdict}, PlayDecodeStatus::ResponseMalformed},
      {{OutboundError::kTimeout, 0, "", ""}, PlayDecodeStatus::Timeout},
      {{OutboundError::kTls, 0, "", ""}, PlayDecodeStatus::TlsFailed},
      {{OutboundError::kTooLarge, 0, "", ""}, PlayDecodeStatus::ResponseTooLarge},
      {{OutboundError::kHttp, 0, "", ""}, PlayDecodeStatus::ResponseMalformed},
      {{OutboundError::kUnsupportedFraming, 0, "", ""}, PlayDecodeStatus::ResponseMalformed},
      {{OutboundError::kResolve, 0, "", ""}, PlayDecodeStatus::TransportFailed},
      {{OutboundError::kConnect, 0, "", ""}, PlayDecodeStatus::TransportFailed},
      {{OutboundError::kPrivateAddress, 0, "", ""}, PlayDecodeStatus::TransportFailed},
      {{OutboundError::kBusy, 0, "", ""}, PlayDecodeStatus::Busy},
      // One byte over the verdict bound.
      {{OutboundError::kNone, 200, "application/json", std::string(kPlayVerdictMaxBytes + 1, ' ')},
       PlayDecodeStatus::ResponseTooLarge},
  };
  for (const Case& c : cases) {
    Harness harness;
    harness.fake.on_decode = [&](const Recorded&, size_t) { return c.reply; };
    const PlayDecodeResult result = Decode(&harness.client, FreshToken());
    CHECK_DECODE(result.status, c.expected);
    CHECK(result.verdict == PlayIntegrityStatus::PolicyInvalid);
    CHECK(result.evidence.version_code == 0 && result.evidence.timestamp_ms == 0);
    CHECK(harness.fake.decode.size() == 1);
  }
  // 2xx JSON that is not a verdict reaches the validator and fails there.
  struct Body {
    std::string body;
    PlayIntegrityStatus expected;
  };
  const Body bodies[] = {
      {"{\"error\":{\"code\":400,\"message\":\"Integrity token cannot be decoded\"}}",
       PlayIntegrityStatus::PayloadMalformed},
      {"{}", PlayIntegrityStatus::PayloadMalformed},
      {"{\"tokenPayloadExternal\":{}}", PlayIntegrityStatus::PackageMismatch},
      {verdict.substr(0, verdict.size() - 1), PlayIntegrityStatus::PayloadMalformed},
      {verdict + "{}", PlayIntegrityStatus::PayloadMalformed},
  };
  for (const Body& b : bodies) {
    Harness harness;
    harness.fake.on_decode = [&](const Recorded&, size_t) {
      return Reply{OutboundError::kNone, 200, "application/json", b.body};
    };
    const PlayDecodeResult result = Decode(&harness.client, FreshToken());
    CHECK_DECODE(result.status, PlayDecodeStatus::VerdictRejected);
    CHECK(result.verdict == b.expected);
  }
  printf("decode failures: HTTP, transport, media type, bounds and non-verdict bodies\n");
}

void TestVerdictBinding() {
  struct Case {
    std::function<void(VerdictSpec*)> edit;
    PlayIntegrityStatus expected;
    const char* challenge;
    const char* app_version;
  };
  const Case cases[] = {
      {[](VerdictSpec* s) { s->certificate = B64(std::string(32, 'x')); },
       PlayIntegrityStatus::CertificateMismatch, kChallenge, kAppVersion},
      // The verdict was requested for another challenge or appVersion.
      {[](VerdictSpec* s) { s->hash = Hash(kOtherChallenge, kAppVersion); },
       PlayIntegrityStatus::RequestHashMismatch, kChallenge, kAppVersion},
      {[](VerdictSpec*) {}, PlayIntegrityStatus::RequestHashMismatch, kOtherChallenge, kAppVersion},
      {[](VerdictSpec*) {}, PlayIntegrityStatus::RequestHashMismatch, kChallenge, "1.4.3"},
      {[](VerdictSpec* s) { s->version_code = "39"; }, PlayIntegrityStatus::VersionCodeRejected,
       kChallenge, kAppVersion},
      {[](VerdictSpec* s) { s->timestamp_ms = kWallStart - kPlayDefaultMaxAgeMs - 1; },
       PlayIntegrityStatus::TimestampStale, kChallenge, kAppVersion},
      {[](VerdictSpec* s) { s->timestamp_ms = kWallStart + kPlayDefaultMaxFutureSkewMs + 1; },
       PlayIntegrityStatus::TimestampFuture, kChallenge, kAppVersion},
      // What Google answers for a token decoded before: cleared verdicts.
      {[](VerdictSpec* s) {
         s->app_verdict = "UNEVALUATED";
         s->device = "";
         s->licensing = "UNEVALUATED";
       },
       PlayIntegrityStatus::AppNotRecognized, kChallenge, kAppVersion},
      {[](VerdictSpec* s) { s->device = ""; }, PlayIntegrityStatus::DeviceIntegrityFailed,
       kChallenge, kAppVersion},
      {[](VerdictSpec* s) { s->device = "\"MEETS_BASIC_INTEGRITY\""; },
       PlayIntegrityStatus::DeviceIntegrityFailed, kChallenge, kAppVersion},
  };
  for (const Case& c : cases) {
    Harness harness;
    VerdictSpec spec;
    c.edit(&spec);
    harness.fake.on_decode = [&](const Recorded&, size_t) {
      return Reply{OutboundError::kNone, 200, "application/json", Verdict(spec)};
    };
    const PlayDecodeResult result = Decode(&harness.client, FreshToken(), c.challenge, c.app_version);
    CHECK_DECODE(result.status, PlayDecodeStatus::VerdictRejected);
    if (result.verdict != c.expected) {
      fprintf(stderr, "verdict %s\n", PlayIntegrityStatusName(result.verdict));
      abort();
    }
    CHECK(result.evidence.version_code == 0);
  }
  printf("verdict binding: certificate, challenge, appVersion, version, time, replayed verdict\n");
}

void TestInputsAndReplay() {
  Harness harness;
  const std::string token = FreshToken();
  struct Input {
    const char* challenge;
    const char* app_version;
    std::string token;
  };
  const Input bad[] = {
      {"short", kAppVersion, token},
      {"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh9", kAppVersion, token},  // unused bits
      {kChallenge, "", token},
      {kChallenge, "1.0\n", token},
      {kChallenge, kAppVersion, std::string(kPlayIntegrityTokenMinBytes - 1, 'a')},
      {kChallenge, kAppVersion, std::string(kPlayIntegrityTokenMaxBytes + 1, 'a')},
      {kChallenge, kAppVersion, token + "\""},
      {kChallenge, kAppVersion, token + "\\"},
      {kChallenge, kAppVersion, token + " "},
      {kChallenge, kAppVersion, token + "\n"},
      {kChallenge, kAppVersion, std::string("abcdefghijklmnop\0q", 18)},
  };
  for (const Input& input : bad)
    CHECK_DECODE(Decode(&harness.client, input.token, input.challenge, input.app_version).status,
                 PlayDecodeStatus::InputInvalid);
  {
    PlayDecodeInput empty;
    CHECK_DECODE(harness.client.Decode(empty).status, PlayDecodeStatus::InputInvalid);
  }
  CHECK(harness.fake.oauth.empty() && harness.fake.decode.empty());
  CHECK_DECODE(Decode(&harness.client, std::string(kPlayIntegrityTokenMaxBytes, 'A')).status,
               PlayDecodeStatus::Verified);
  CHECK(harness.fake.decode.back().body.size() == kPlayIntegrityTokenMaxBytes + 22);

  // A token is accepted once; the replay never reaches the network.
  CHECK_DECODE(Decode(&harness.client, token).status, PlayDecodeStatus::Verified);
  const size_t sent = harness.fake.decode.size();
  CHECK_DECODE(Decode(&harness.client, token).status, PlayDecodeStatus::Replayed);
  CHECK_DECODE(Decode(&harness.client, token, kOtherChallenge).status, PlayDecodeStatus::Replayed);
  CHECK(harness.fake.decode.size() == sent);
  // Burned even when the first attempt failed.
  const std::string failed = FreshToken();
  harness.fake.on_decode = [&](const Recorded&, size_t) {
    return Reply{OutboundError::kNone, 503, "", ""};
  };
  CHECK_DECODE(Decode(&harness.client, failed).status, PlayDecodeStatus::UpstreamUnavailable);
  harness.fake.on_decode = nullptr;
  CHECK_DECODE(Decode(&harness.client, failed).status, PlayDecodeStatus::Replayed);
  // Remembered for max_age + future skew, then forgotten (the verdict's
  // own timestamp check refuses such an old token anyway).
  g_mono += kPlayDefaultMaxAgeMs + kPlayDefaultMaxFutureSkewMs - 1;
  CHECK_DECODE(Decode(&harness.client, token).status, PlayDecodeStatus::Replayed);
  g_mono += 1;
  CHECK_DECODE(Decode(&harness.client, token).status, PlayDecodeStatus::Verified);
  printf("inputs and replay: malformed inputs refused offline; tokens burned on first use\n");
}

void TestReplayCapacity() {
  Harness harness;
  // Fill the ring with live entries.
  harness.fake.on_decode = [&](const Recorded&, size_t) {
    return Reply{OutboundError::kNone, 503, "", ""};
  };
  for (size_t i = 0; i < kPlayReplayCacheEntries; ++i)
    CHECK_DECODE(Decode(&harness.client, FreshToken()).status,
                 PlayDecodeStatus::UpstreamUnavailable);
  CHECK_DECODE(Decode(&harness.client, FreshToken()).status, PlayDecodeStatus::Busy);
  g_mono += kPlayDefaultMaxAgeMs + kPlayDefaultMaxFutureSkewMs;
  harness.fake.on_decode = nullptr;
  CHECK_DECODE(Decode(&harness.client, FreshToken()).status, PlayDecodeStatus::Verified);
  printf("replay capacity: a full ring of live digests answers Busy, never forgets\n");
}

long MaxRssKiB() {
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
  return usage.ru_maxrss / 1024;
#else
  return usage.ru_maxrss;
#endif
}

// Static memory budget of this path against the 512 MiB guest, and a
// soak that must not grow the heap.
void TestMemory() {
  const size_t client = sizeof(PlayIntegrityGoogleClient) +
                        kPlayReplayCacheEntries * (32 + sizeof(int64_t));
  // Per in-flight decode: the heap scratch (bearer, request body, response),
  // the verdict validator's token arena and the TLS client's buffers
  // (two 16 KiB Mbed TLS records, 16 KiB header block, 8 KiB request header).
  const size_t decode = (kGoogleAccessTokenMaxBytes + 1) + (kPlayIntegrityTokenMaxBytes + 32) +
                        kPlayVerdictMaxBytes + kPlayVerdictMaxTokens * 24 + 2 * 16704 +
                        16384 + 8192;
  const size_t refresh = (kPlayOAuthAssertionMaxBytes + 1) * 2 + 128 +
                         kGoogleOAuthResponseMaxBytes + kGoogleOAuthResponseMaxTokens * 24 +
                         2 * 16704 + 16384 + 8192;
  const size_t total = client + kPlayDecodeMaxConcurrent * decode + refresh;
  printf("memory: client %zu B, per decode %zu B, refresh %zu B, worst case %zu B (%.2f%% of 512 MiB)\n",
         client, decode, refresh, total, 100.0 * double(total) / (512.0 * 1024 * 1024));
  CHECK(total < 1024 * 1024);

  Harness harness;
  for (int i = 0; i < 200; ++i)
    CHECK_DECODE(Decode(&harness.client, FreshToken()).status, PlayDecodeStatus::Verified);
  const long before = MaxRssKiB();
  for (int i = 0; i < 3000; ++i) {
    if (i % 600 == 0) g_mono += 4000 * 1000;  // force refreshes (RSA signing) too
    if (i % 1000 == 999) g_mono += kPlayDefaultMaxAgeMs + kPlayDefaultMaxFutureSkewMs;
    CHECK_DECODE(Decode(&harness.client, FreshToken()).status, PlayDecodeStatus::Verified);
  }
  const long after = MaxRssKiB();
  printf("memory soak: 3,000 decodes, max RSS %ld -> %ld KiB\n", before, after);
  CHECK(after - before < 4096);
}

// ---- TLS mode -------------------------------------------------------------

int RunTls(int argc, char** argv) {
  if (argc != 8) return 2;
  g_fixture = argv[2];
  g_accounts = g_fixture + "/accounts";
  mkdir(g_accounts.c_str(), 0700);
  const std::string account = WriteFile("tls.json", Object(AccountFields()));
  const std::string certificate = ReadFile(g_fixture + "/certificate.b64");
  uint8_t digest[32];
  CHECK(ParsePlayCertificateDigest(certificate.data(), certificate.size() - 1,
                                   PlayCertificateDigestFormat::Base64Url, digest));
  uint8_t certificates[1][32];
  memcpy(certificates[0], digest, 32);
  PlayIntegrityGoogleConfig config;
  config.credential_path = account.c_str();
  config.ca_file = argv[5];
  config.policy.certificates = certificates;
  config.policy.certificate_count = 1;
  config.policy.min_version_code = 40;
  config.test.host = argv[3];
  config.test.port = argv[4];
  PlayIntegrityGoogleClient client;
  ServiceAccountStatus credential;
  if (!client.Initialize(config, &credential)) {
    printf("init %s\n", ServiceAccountStatusName(credential));
    return 1;
  }
  const std::string token = argv[7];
  PlayDecodeInput input;
  input.challenge = kChallenge;
  input.challenge_length = strlen(kChallenge);
  input.app_version = argv[6];
  input.app_version_length = strlen(argv[6]);
  input.integrity_token = token.data();
  input.integrity_token_length = token.size();
  PlayDecodeResult result = client.Decode(input);
  printf("%s %s %llu %llu", PlayDecodeStatusName(result.status),
         PlayIntegrityStatusName(result.verdict),
         static_cast<unsigned long long>(client.oauth_exchanges()),
         static_cast<unsigned long long>(client.decode_requests()));
  // The same token again is a local replay.
  if (result.status == PlayDecodeStatus::Verified) {
    result = client.Decode(input);
    printf(" %s %lld", PlayDecodeStatusName(result.status),
           static_cast<long long>(client.decode_requests()));
  }
  printf("\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc >= 2 && strcmp(argv[1], "tls") == 0) return RunTls(argc, argv);
  if (argc != 3 || strcmp(argv[1], "unit") != 0) {
    fprintf(stderr, "usage: %s unit FIXTURE_DIR | tls FIXTURE_DIR HOST PORT CA APP_VERSION TOKEN\n",
            argv[0]);
    return 2;
  }
  g_fixture = argv[2];
  g_accounts = g_fixture + "/accounts";
  mkdir(g_accounts.c_str(), 0700);
  g_ca = g_fixture + "/ca.pem";
  for (int i = 0; i < 32; ++i) g_certificate[i] = uint8_t(0xa0 + i);
  g_certificate_b64 = B64(std::string(reinterpret_cast<char*>(g_certificate), 32));
  g_good_account = WriteFile("good.json", Object(AccountFields()));

  TestServiceAccountFiles();
  TestAssertion();
  TestOAuthParse();
  TestOutboundBearer();
  TestInitialize();
  TestHappyPath();
  TestTokenCache();
  TestRefreshRace();
  TestOAuthFailures();
  TestUnauthorizedRefresh();
  TestDecodeFailures();
  TestVerdictBinding();
  TestInputsAndReplay();
  TestReplayCapacity();
  TestMemory();
  printf("play_integrity_google_test: all groups passed\n");
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
