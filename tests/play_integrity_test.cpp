#include "test_support.h"
// The Play Integrity requestHash binding and the
// decoded-verdict validator (src/auth/play_integrity.h). Pure host tests; no
// network, credential or storage. The requestHash vectors were computed by
// an independent Python implementation of the documented algorithm.
#include "../src/auth/play_integrity.h"
#include "../src/auth/device_auth.h"

#include <chrono>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

// The test binary's stand-in for the decode client: the only way to wrap
// bytes as a decoded response.
namespace fortis::auth {
class PlayIntegrityDecodeSource {
 public:
  static PlayIntegrityDecodedResponse Wrap(const std::string& body) {
    return PlayIntegrityDecodedResponse(reinterpret_cast<const uint8_t*>(body.data()),
                                        body.size());
  }
  static PlayIntegrityDecodedResponse Null() { return PlayIntegrityDecodedResponse(nullptr, 0); }
};
}  // namespace fortis::auth

namespace {

using fortis::auth::ComputePlayIntegrityRequestHash;
using fortis::auth::ParsePlayCertificateDigest;
using fortis::auth::PlayCertificateDigestFormat;
using fortis::auth::PlayDeviceLabel;
using fortis::auth::PlayIntegrityDecodedResponse;
using fortis::auth::PlayIntegrityDecodeSource;
using fortis::auth::PlayIntegrityEvidence;
using fortis::auth::PlayIntegrityPolicy;
using fortis::auth::PlayIntegrityStatus;
using fortis::auth::PlayLicensing;
using fortis::auth::PlayRequestHashStatus;
using Status = PlayIntegrityStatus;

// Request bytes can never become a decoded response, and nothing turns
// Play Integrity evidence into the device-store proof type.
static_assert(!std::is_constructible<PlayIntegrityDecodedResponse, const uint8_t*, size_t>::value,
              "only PlayIntegrityDecodeSource may wrap a decoded response");
static_assert(!std::is_constructible<PlayIntegrityDecodedResponse, const char*, size_t>::value,
              "only PlayIntegrityDecodeSource may wrap a decoded response");
static_assert(!std::is_default_constructible<PlayIntegrityDecodedResponse>::value,
              "no empty decoded response");
static_assert(!std::is_default_constructible<fortis::auth::VerifiedDeviceAttestation>::value,
              "only DeviceAttestationIssuer builds a verified proof");
static_assert(!std::is_constructible<fortis::auth::VerifiedDeviceAttestation,
                                     const PlayIntegrityEvidence&>::value,
              "evidence is not a device proof");

#define CHECK(condition)                                                            \
  do {                                                                              \
    if (!(condition)) {                                                             \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
      abort();                                                                      \
    }                                                                               \
  } while (0)

#define CHECK_STATUS(actual, expected)                                                    \
  do {                                                                                    \
    const Status got_ = (actual);                                                         \
    if (got_ != (expected)) {                                                             \
      fprintf(stderr, "%s:%d: got %s, expected %s\n", __FILE__, __LINE__,                 \
              fortis::auth::PlayIntegrityStatusName(got_), fortis::auth::PlayIntegrityStatusName(expected)); \
      abort();                                                                            \
    }                                                                                     \
  } while (0)

constexpr int64_t kNow = 1761652800000;  // 2025-10-28T12:00:00Z
const char kPackage[] = "io.fortisnet.wallet";
const char kChallenge[] = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8";  // bytes 0..31
// ComputePlayIntegrityRequestHash(kChallenge, "android", kPackage, "1.4.2").
const char kHash[] = "ZJ4pYhaC110UeOf963c9AK3eMstUd83f_MnmQ9j4VAg";
// SHA-256("fortis test release certificate"), three spellings.
const char kCertHexColon[] =
    "C9:98:A5:B5:5F:1A:99:34:7E:7B:85:EC:1F:FD:1D:0A:46:81:EC:67:1E:8F:66:90:43:D6:1E:F2:4B:3C:A1:FB";
const char kCertHex[] = "c998a5b55f1a99347e7b85ec1ffd1d0a4681ec671e8f669043d61ef24b3ca1fb";
const char kCertBase64[] = "yZiltV8amTR-e4XsH_0dCkaB7Gcej2aQQ9Ye8ks8ofs";
// A different certificate: SHA-256 of 32 zero bytes is not needed; any other 32 bytes.
const char kOtherCertBase64[] = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8";

uint8_t g_release_cert[1][32];

PlayIntegrityPolicy DefaultPolicy() {
  PlayIntegrityPolicy policy;
  policy.certificates = g_release_cert;
  policy.certificate_count = 1;
  return policy;
}

std::string Quote(const std::string& text) { return "\"" + text + "\""; }

// A decodeIntegrityToken response assembled from raw JSON member values.
// An empty value omits the member; `omit_*` drops a whole object.
struct Verdict {
  std::string request_package = Quote(kPackage);
  std::string request_hash = Quote(kHash);
  std::string timestamp = Quote(std::to_string(kNow - 1000));
  std::string nonce;
  std::string request_extra;
  std::string app_verdict = Quote("PLAY_RECOGNIZED");
  std::string app_package = Quote(kPackage);
  std::string certificates = "[" + Quote(kCertBase64) + "]";
  std::string version_code = Quote("42");
  std::string app_extra;
  std::string device_verdict = "[\"MEETS_DEVICE_INTEGRITY\"]";
  std::string device_extra;
  std::string licensing = Quote("LICENSED");
  std::string testing;
  std::string payload_extra;
  std::string outer_extra;
  bool omit_request = false, omit_app = false, omit_device = false, omit_account = false;

  static void Add(std::string* object, const char* name, const std::string& value) {
    if (value.empty()) return;
    if (object->size() > 1) *object += ",";
    *object += Quote(name) + ":" + value;
  }
  static void Raw(std::string* object, const std::string& members) {
    if (members.empty()) return;
    if (object->size() > 1) *object += ",";
    *object += members;
  }
  std::string Payload() const {
    std::string request = "{", app = "{", device = "{", account = "{", payload = "{";
    Add(&request, "requestPackageName", request_package);
    Add(&request, "requestHash", request_hash);
    Add(&request, "timestampMillis", timestamp);
    Add(&request, "nonce", nonce);
    Raw(&request, request_extra);
    Add(&app, "appRecognitionVerdict", app_verdict);
    Add(&app, "packageName", app_package);
    Add(&app, "certificateSha256Digest", certificates);
    Add(&app, "versionCode", version_code);
    Raw(&app, app_extra);
    Add(&device, "deviceRecognitionVerdict", device_verdict);
    Raw(&device, device_extra);
    Add(&account, "appLicensingVerdict", licensing);
    if (!omit_request) Add(&payload, "requestDetails", request + "}");
    if (!omit_app) Add(&payload, "appIntegrity", app + "}");
    if (!omit_device) Add(&payload, "deviceIntegrity", device + "}");
    if (!omit_account) Add(&payload, "accountDetails", account + "}");
    Add(&payload, "testingDetails", testing);
    Raw(&payload, payload_extra);
    return payload + "}";
  }
  std::string Json() const {
    std::string outer = "{";
    Add(&outer, "tokenPayloadExternal", Payload());
    Raw(&outer, outer_extra);
    return outer + "}";
  }
};

Status Run(const std::string& body, const PlayIntegrityPolicy& policy, int64_t now = kNow,
           PlayIntegrityEvidence* evidence = nullptr, const char* hash = kHash) {
  PlayIntegrityEvidence local;
  const PlayIntegrityDecodedResponse response = PlayIntegrityDecodeSource::Wrap(body);
  return fortis::auth::VerifyPlayIntegrityVerdict(policy, response, hash, hash ? strlen(hash) : 0, now,
                                            evidence ? evidence : &local);
}
Status Run(const std::string& body, int64_t now = kNow, PlayIntegrityEvidence* evidence = nullptr) {
  return Run(body, DefaultPolicy(), now, evidence);
}
Status Run(const Verdict& verdict, int64_t now = kNow, PlayIntegrityEvidence* evidence = nullptr) {
  return Run(verdict.Json(), now, evidence);
}

std::string Hash(const std::string& challenge, const std::string& platform,
                 const std::string& package, const std::string& version,
                 PlayRequestHashStatus expected = PlayRequestHashStatus::Ok) {
  char output[fortis::auth::kPlayRequestHashChars + 1];
  memset(output, 'x', sizeof(output));
  const PlayRequestHashStatus status = ComputePlayIntegrityRequestHash(
      challenge.data(), challenge.size(), platform.data(), platform.size(), package.data(),
      package.size(), version.data(), version.size(), output);
  if (status != expected) {
    fprintf(stderr, "requestHash status %s, expected %s\n",
            fortis::auth::PlayRequestHashStatusName(status), fortis::auth::PlayRequestHashStatusName(expected));
    abort();
  }
  if (status != PlayRequestHashStatus::Ok) CHECK(output[0] == '\0');
  else CHECK(strlen(output) == fortis::auth::kPlayRequestHashChars);
  return output;
}

void TestRequestHashVectors() {
  // Independent Python vectors (hashlib + struct.pack(">I")).
  CHECK(Hash("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "android", kPackage, "1.0.0") ==
        "vYuFrv2kY1HZOnJ-E8Moub2QLaUIYtKBfKqG7esaghg");
  CHECK(Hash(kChallenge, "android", kPackage, "1.4.2") == kHash);
  CHECK(Hash("__________________________________________8", "android", kPackage,
             "2.0.0-\xce\xb2+7") == "BEKFIYV1r_ueVWpXtBQWtPYLmtBE5lEZxMRHhWckG90");
  CHECK(Hash(kChallenge, "android", kPackage, "1.4.20") ==
        "Lf5_5HzAmIuIeRAdDJkYe7nbwAvlWfQGOX2mhUwd7Bg");
  CHECK(Hash(kChallenge, "android", kPackage, "1.4.2 ") ==
        "kohU1IEidNn380WgYUw8J-zKhdhZXqOEtsZZb0mdc8A");
  // Every input moves the hash; length prefixes keep field boundaries.
  CHECK(Hash(kChallenge, "android", "io.fortisnet.walleta", "1") !=
        Hash(kChallenge, "android", "io.fortisnet.wallet", "a1"));
  CHECK(Hash(kChallenge, "android", "io.fortisnet.wallet2", "1.4.2") != kHash);
  CHECK(Hash("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHhA", "android", kPackage, "1.4.2") != kHash);
  CHECK(Hash(kChallenge, "android", kPackage, std::string(200, '9')).size() == 43);
  puts("requestHash: independent vectors and field separation: ok");
}

void TestRequestHashRefusals() {
  using R = PlayRequestHashStatus;
  const std::string version = "1.4.2";
  for (const std::string& challenge :
       {std::string(""), std::string(kChallenge).substr(0, 42), std::string(kChallenge) + "A",
        std::string(kChallenge) + "=", std::string("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB"),
        std::string("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh+"),
        std::string("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh/"),
        std::string("AAECAwQFBgcICQoLDA0ODxAREh\0UFRYXGBkaGxwdHh8", 43),
        std::string("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh 8").substr(0, 43),
        std::string(200, 'A')})
    Hash(challenge, "android", kPackage, version, R::ChallengeMalformed);
  for (const std::string& platform :
       {std::string(""), std::string("ios"), std::string("Android"), std::string("androidx"),
        std::string("android\0", 8), std::string(" android")})
    Hash(kChallenge, platform, kPackage, version, R::PlatformInvalid);
  for (const std::string& package :
       {std::string(""), std::string("io"), std::string(".io.x"), std::string("io..x"),
        std::string("io.x."), std::string("1io.x"), std::string("io.1x"), std::string("io.fortis-net"),
        std::string("io.fortis net"), std::string("io.fortisnet.wallet\0", 20),
        "a." + std::string(254, 'b')})
    Hash(kChallenge, "android", package, version, R::PackageInvalid);
  CHECK(Hash(kChallenge, "android", "a." + std::string(253, 'b'), version).size() == 43);
  CHECK(Hash(kChallenge, "android", "A_1.b_", version).size() == 43);
  for (const std::string& bad :
       {std::string(""), std::string(201, '1'), std::string("1.4\0.2", 6), std::string("1.4\n"),
        std::string("1.4\x7f"), std::string("\xc0\x80"), std::string("\xff"),
        std::string("\xed\xa0\x80"), std::string("\xe2\x82"), std::string("\xf4\x90\x80\x80"),
        std::string("\xe0\x80\xaf")})
    Hash(kChallenge, "android", kPackage, bad, R::AppVersionInvalid);
  CHECK(ComputePlayIntegrityRequestHash(kChallenge, 43, "android", 7, kPackage, 19, "1", 1,
                                        nullptr) == R::Internal);
  char output[fortis::auth::kPlayRequestHashChars + 1];
  CHECK(ComputePlayIntegrityRequestHash(nullptr, 43, "android", 7, kPackage, 19, "1", 1, output) ==
        R::ChallengeMalformed && output[0] == '\0');
  CHECK(ComputePlayIntegrityRequestHash(kChallenge, 43, nullptr, 7, kPackage, 19, "1", 1, output) ==
        R::PlatformInvalid);
  CHECK(ComputePlayIntegrityRequestHash(kChallenge, 43, "android", 7, nullptr, 19, "1", 1, output) ==
        R::PackageInvalid);
  CHECK(ComputePlayIntegrityRequestHash(kChallenge, 43, "android", 7, kPackage, 19, nullptr, 1, output) ==
        R::AppVersionInvalid);
  puts("requestHash: malformed challenge, platform, package, appVersion, NUL and oversize refused: ok");
}

void TestCertificateDigests() {
  uint8_t colon[32], hex[32], base64[32], lower[32];
  CHECK(ParsePlayCertificateDigest(kCertHexColon, strlen(kCertHexColon),
                                   PlayCertificateDigestFormat::HexColon, colon));
  CHECK(ParsePlayCertificateDigest(kCertHex, strlen(kCertHex), PlayCertificateDigestFormat::Hex, hex));
  CHECK(ParsePlayCertificateDigest(kCertBase64, strlen(kCertBase64),
                                   PlayCertificateDigestFormat::Base64Url, base64));
  std::string lowered = kCertHexColon;
  for (char& c : lowered) c = static_cast<char>(tolower(c));
  CHECK(ParsePlayCertificateDigest(lowered.data(), lowered.size(),
                                   PlayCertificateDigestFormat::HexColon, lower));
  CHECK(!memcmp(colon, hex, 32) && !memcmp(hex, base64, 32) && !memcmp(base64, lower, 32));
  CHECK(colon[0] == 0xc9 && colon[31] == 0xfb);
  struct Bad { std::string text; PlayCertificateDigestFormat format; };
  const std::string colon_text = kCertHexColon, hex_text = kCertHex, b64 = kCertBase64;
  const Bad bad[] = {
      {colon_text, PlayCertificateDigestFormat::Hex},
      {colon_text, PlayCertificateDigestFormat::Base64Url},
      {hex_text, PlayCertificateDigestFormat::HexColon},
      {hex_text, PlayCertificateDigestFormat::Base64Url},
      {b64, PlayCertificateDigestFormat::Hex},
      {b64 + "=", PlayCertificateDigestFormat::Base64Url},
      {"yZiltV8amTR+e4XsH/0dCkaB7Gcej2aQQ9Ye8ks8ofs", PlayCertificateDigestFormat::Base64Url},
      {"yZiltV8amTR-e4XsH_0dCkaB7Gcej2aQQ9Ye8ks8oft", PlayCertificateDigestFormat::Base64Url},
      {hex_text.substr(0, 62), PlayCertificateDigestFormat::Hex},
      {hex_text + "00", PlayCertificateDigestFormat::Hex},
      {"g" + hex_text.substr(1), PlayCertificateDigestFormat::Hex},
      {colon_text.substr(0, 2) + "-" + colon_text.substr(3), PlayCertificateDigestFormat::HexColon},
      {"C998:" + colon_text.substr(5) + ":", PlayCertificateDigestFormat::HexColon},
      {colon_text + ":", PlayCertificateDigestFormat::HexColon},
      {"", PlayCertificateDigestFormat::Base64Url},
      {b64, static_cast<PlayCertificateDigestFormat>(9)},  // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange): deliberately tests rejection of an unknown byte-valued format.
  };
  for (const Bad& item : bad) {
    uint8_t output[32];
    memset(output, 0xa5, sizeof(output));
    CHECK(!ParsePlayCertificateDigest(item.text.data(), item.text.size(), item.format, output));
    for (uint8_t byte : output) CHECK(byte == 0xa5);
  }
  puts("certificate digest policy formats (hex-colon, hex, base64url) and refusals: ok");
}

void TestValidVerdicts() {
  PlayIntegrityEvidence evidence;
  CHECK_STATUS(Run(Verdict{}, kNow, &evidence), Status::Verified);
  CHECK(evidence.version_code == 42 && evidence.timestamp_ms == kNow - 1000 &&
        evidence.device_label == PlayDeviceLabel::DeviceIntegrity &&
        evidence.licensing == PlayLicensing::Licensed);

  Verdict strong;
  strong.device_verdict =
      "[\"MEETS_BASIC_INTEGRITY\",\"MEETS_DEVICE_INTEGRITY\",\"MEETS_STRONG_INTEGRITY\"]";
  CHECK_STATUS(Run(strong, kNow, &evidence), Status::Verified);
  CHECK(evidence.device_label == PlayDeviceLabel::StrongIntegrity);

  // Every documented optional verdict, deprecated and future fields, and
  // unknown labels: forward compatible, and none of them is a pass.
  Verdict full;
  full.device_verdict =
      "[\"MEETS_DEVICE_INTEGRITY\",\"MEETS_FUTURE_INTEGRITY\",\"UNEVALUATED\",\"UNKNOWN\"]";
  full.device_extra =
      "\"legacyDeviceRecognitionVerdict\":[\"MEETS_DEVICE_INTEGRITY\"],"
      "\"deviceAttributes\":{\"sdkVersion\":34},"
      "\"recentDeviceActivity\":{\"deviceActivityLevel\":\"LEVEL_1\"},"
      "\"deviceRecall\":{\"values\":{\"bitFirst\":true},\"writeDates\":{\"yyyymmFirst\":202401}}";
  full.app_extra = "\"futureAppField\":[1,2,{\"x\":null}]";
  full.request_extra = "\"futureRequestField\":\"x\"";
  full.payload_extra =
      "\"environmentDetails\":{\"appAccessRiskVerdict\":{\"appsDetected\":[\"KNOWN_INSTALLED\","
      "\"UNKNOWN_CAPTURING\"]},\"playProtectVerdict\":\"NO_ISSUES\","
      "\"locationSpoofingRiskVerdict\":[\"LOW_RISK_DEVICE\"]},\"futureVerdict\":{\"a\":[[]]}";
  full.outer_extra = "\"futureTopLevel\":true";
  full.licensing = Quote("UNLICENSED");
  CHECK_STATUS(Run(full, kNow, &evidence), Status::Verified);
  CHECK(evidence.device_label == PlayDeviceLabel::DeviceIntegrity &&
        evidence.licensing == PlayLicensing::Unlicensed);

  Verdict no_account;
  no_account.omit_account = true;
  CHECK_STATUS(Run(no_account, kNow, &evidence), Status::Verified);
  CHECK(evidence.licensing == PlayLicensing::Absent);
  Verdict other_license;
  other_license.licensing = Quote("UNEVALUATED");
  CHECK_STATUS(Run(other_license, kNow, &evidence), Status::Verified);
  CHECK(evidence.licensing == PlayLicensing::Unevaluated);
  other_license.licensing = Quote("NEW_STATE");
  CHECK_STATUS(Run(other_license, kNow, &evidence), Status::Verified);
  CHECK(evidence.licensing == PlayLicensing::Other);

  // A rotated certificate list: any configured digest matches any entry;
  // the padded web-safe spelling is the same digest.
  Verdict rotated;
  rotated.certificates = "[" + Quote(kOtherCertBase64) + "," + Quote(std::string(kCertBase64) + "=") + "]";
  CHECK_STATUS(Run(rotated), Status::Verified);
  uint8_t two[2][32];
  CHECK(ParsePlayCertificateDigest(kOtherCertBase64, 43, PlayCertificateDigestFormat::Base64Url, two[0]));
  memcpy(two[1], g_release_cert[0], 32);
  PlayIntegrityPolicy rotation = DefaultPolicy();
  rotation.certificates = two;
  rotation.certificate_count = 2;
  Verdict old_cert;
  old_cert.certificates = "[" + Quote(kOtherCertBase64) + "]";
  CHECK_STATUS(Run(old_cert.Json(), rotation), Status::Verified);
  CHECK_STATUS(Run(old_cert), Status::CertificateMismatch);

  // Escaped spellings decode to the same strings and keys.
  Verdict escaped;
  escaped.request_package = "\"io.fortisnet\\u002ewallet\"";
  escaped.app_verdict = "\"PLAY_\\u0052ECOGNIZED\"";
  escaped.request_extra = "\"\\u0066uture\":1";
  CHECK_STATUS(Run(escaped), Status::Verified);

  // Max int64 version code.
  Verdict big;
  big.version_code = Quote("9223372036854775807");
  CHECK_STATUS(Run(big, kNow, &evidence), Status::Verified);
  CHECK(evidence.version_code == INT64_MAX);
  puts("valid verdicts, strongest label, licensing report, optional and future fields: ok");
}

void TestReordered() {
  const std::string reordered = std::string(
      "\n{ \"futureTopLevel\" : [ ] ,\n \"tokenPayloadExternal\" : {\n"
      "  \"deviceIntegrity\": {\"deviceAttributes\": {}, \"deviceRecognitionVerdict\": "
      "[\"MEETS_STRONG_INTEGRITY\", \"MEETS_DEVICE_INTEGRITY\"]},\n"
      "  \"accountDetails\": {\"appLicensingVerdict\": \"LICENSED\"},\n"
      "  \"appIntegrity\": {\"versionCode\": \"7\", \"certificateSha256Digest\": [\"") +
      kCertBase64 + "\"], \"packageName\": \"io.fortisnet.wallet\", "
      "\"appRecognitionVerdict\": \"PLAY_RECOGNIZED\"},\n"
      "  \"requestDetails\": {\"timestampMillis\": \"" + std::to_string(kNow) +
      "\", \"requestHash\": \"" + kHash + "\", \"requestPackageName\": \"io.fortisnet.wallet\"}\n"
      " }\n}\n";
  PlayIntegrityEvidence evidence;
  CHECK_STATUS(Run(reordered, kNow, &evidence), Status::Verified);
  CHECK(evidence.version_code == 7 && evidence.device_label == PlayDeviceLabel::StrongIntegrity &&
        evidence.timestamp_ms == kNow);
  puts("reordered members and whitespace: ok");
}

void TestMissing() {
  struct Case { const char* name; Verdict verdict; Status expected; };
  std::vector<Case> cases;
  auto add = [&](const char* name, Status expected, auto edit) {
    Verdict verdict;
    edit(verdict);
    cases.push_back({name, verdict, expected});
  };
  add("requestDetails", Status::PackageMismatch, [](Verdict& v) { v.omit_request = true; });
  add("requestPackageName", Status::PackageMismatch, [](Verdict& v) { v.request_package.clear(); });
  add("requestHash", Status::RequestHashMismatch, [](Verdict& v) { v.request_hash.clear(); });
  add("timestampMillis", Status::TimestampMissing, [](Verdict& v) { v.timestamp.clear(); });
  add("appIntegrity", Status::AppNotRecognized, [](Verdict& v) { v.omit_app = true; });
  add("appRecognitionVerdict", Status::AppNotRecognized, [](Verdict& v) { v.app_verdict.clear(); });
  add("packageName", Status::PackageMismatch, [](Verdict& v) { v.app_package.clear(); });
  add("certificateSha256Digest", Status::CertificateMismatch, [](Verdict& v) { v.certificates.clear(); });
  add("versionCode", Status::VersionCodeRejected, [](Verdict& v) { v.version_code.clear(); });
  add("deviceIntegrity", Status::DeviceIntegrityFailed, [](Verdict& v) { v.omit_device = true; });
  add("deviceRecognitionVerdict", Status::DeviceIntegrityFailed,
      [](Verdict& v) { v.device_verdict.clear(); });
  for (const Case& item : cases) {
    const Status status = Run(item.verdict);
    if (status != item.expected) {
      fprintf(stderr, "missing %s: %s\n", item.name, fortis::auth::PlayIntegrityStatusName(status));
      abort();
    }
  }
  CHECK_STATUS(Run(std::string("{}")), Status::PayloadMalformed);
  CHECK_STATUS(Run(std::string("{\"tokenPayloadExternal\":{}}")), Status::PackageMismatch);
  PlayIntegrityPolicy licensed = DefaultPolicy();
  licensed.require_play_license = true;
  Verdict no_account;
  no_account.omit_account = true;
  CHECK_STATUS(Run(no_account.Json(), licensed), Status::LicenseRequired);
  Verdict no_license;
  no_license.licensing.clear();
  CHECK_STATUS(Run(no_license.Json(), licensed), Status::LicenseRequired);
  CHECK_STATUS(Run(Verdict{}.Json(), licensed), Status::Verified);
  printf("missing security fields (%zu cases) and the license policy: ok\n", cases.size() + 5);
}

void TestWrongValues() {
  struct Case { std::string label; Verdict verdict; Status expected; };
  std::vector<Case> cases;
  auto add = [&](std::string label, Status expected, auto edit) {
    Verdict verdict;
    edit(verdict);
    cases.push_back({std::move(label), verdict, expected});
  };
  for (const char* package : {"io.fortisnet.wallet2", "IO.FORTISNET.WALLET", "io.fortisnet.wallet ",
                              "io.fortisnet", "", "com.attacker.wallet"}) {
    add(std::string("request package ") + package, Status::PackageMismatch,
        [&](Verdict& v) { v.request_package = Quote(package); });
    add(std::string("app package ") + package, Status::PackageMismatch,
        [&](Verdict& v) { v.app_package = Quote(package); });
  }
  const std::string hash = kHash;
  std::string flipped = hash;
  flipped[10] = flipped[10] == 'A' ? 'B' : 'A';
  std::string lowered = hash;
  for (char& c : lowered) c = static_cast<char>(tolower(c));
  for (const std::string& wrong :
       {flipped, lowered, hash + "=", hash.substr(0, 42), std::string(""),
        std::string("Lf5_5HzAmIuIeRAdDJkYe7nbwAvlWfQGOX2mhUwd7Bg"), hash + " ", std::string(500, 'A')})
    add("hash " + wrong, Status::RequestHashMismatch, [&](Verdict& v) { v.request_hash = Quote(wrong); });
  add("classic nonce beside the hash", Status::RequestHashMismatch,
      [](Verdict& v) { v.nonce = Quote(kHash); });
  add("classic nonce instead of the hash", Status::RequestHashMismatch, [](Verdict& v) {
    v.nonce = Quote(kHash);
    v.request_hash.clear();
  });
  for (const char* verdict : {"UNEVALUATED", "UNRECOGNIZED_VERSION", "UNKNOWN", "play_recognized",
                              "PLAY_RECOGNIZED ", "", "PLAY_RECOGNISED"})
    add(std::string("app verdict ") + verdict, Status::AppNotRecognized,
        [&](Verdict& v) { v.app_verdict = Quote(verdict); });
  add("UNEVALUATED app with no app fields", Status::AppNotRecognized, [](Verdict& v) {
    v.app_verdict = Quote("UNEVALUATED");
    v.app_package.clear();
    v.certificates.clear();
    v.version_code.clear();
  });
  add("wrong certificate", Status::CertificateMismatch,
      [](Verdict& v) { v.certificates = "[" + Quote(kOtherCertBase64) + "]"; });
  add("empty certificate list", Status::CertificateMismatch, [](Verdict& v) { v.certificates = "[]"; });
  add("version code 0", Status::VersionCodeRejected, [](Verdict& v) { v.version_code = Quote("0"); });
  for (const char* labels :
       {"[]", "[\"MEETS_BASIC_INTEGRITY\"]", "[\"MEETS_STRONG_INTEGRITY\"]",
        "[\"MEETS_VIRTUAL_INTEGRITY\"]", "[\"UNEVALUATED\"]", "[\"meets_device_integrity\"]",
        "[\"MEETS_DEVICE_INTEGRITY \"]", "[\"\"]", "[\"MEETS_BASIC_INTEGRITY\",\"MEETS_STRONG_INTEGRITY\"]"})
    add(std::string("device ") + labels, Status::DeviceIntegrityFailed,
        [&](Verdict& v) { v.device_verdict = labels; });
  add("device verdict only in the legacy field", Status::DeviceIntegrityFailed, [](Verdict& v) {
    v.device_verdict.clear();
    v.device_extra = "\"legacyDeviceRecognitionVerdict\":[\"MEETS_DEVICE_INTEGRITY\"]";
  });
  // Google's answer to a re-decoded token: verdicts cleared.
  add("replayed-token clearing", Status::AppNotRecognized, [](Verdict& v) {
    v.app_verdict = Quote("UNEVALUATED");
    v.licensing = Quote("UNEVALUATED");
    v.device_verdict = "[]";
  });
  for (const Case& item : cases) {
    const Status status = Run(item.verdict);
    if (status != item.expected) {
      fprintf(stderr, "wrong %s: got %s, expected %s\n", item.label.c_str(),
              fortis::auth::PlayIntegrityStatusName(status), fortis::auth::PlayIntegrityStatusName(item.expected));
      abort();
    }
  }
  PlayIntegrityPolicy minimum = DefaultPolicy();
  minimum.min_version_code = 43;
  CHECK_STATUS(Run(Verdict{}.Json(), minimum), Status::VersionCodeRejected);
  minimum.min_version_code = 42;
  CHECK_STATUS(Run(Verdict{}.Json(), minimum), Status::Verified);
  PlayIntegrityPolicy licensed = DefaultPolicy();
  licensed.require_play_license = true;
  for (const char* license : {"UNLICENSED", "UNEVALUATED", "UNKNOWN", "licensed"}) {
    Verdict verdict;
    verdict.licensing = Quote(license);
    CHECK_STATUS(Run(verdict.Json(), licensed), Status::LicenseRequired);
  }
  // The expected hash is the server's: a verdict for another appVersion,
  // challenge or package fails against it.
  CHECK_STATUS(Run(Verdict{}.Json(), DefaultPolicy(), kNow, nullptr,
                   "Lf5_5HzAmIuIeRAdDJkYe7nbwAvlWfQGOX2mhUwd7Bg"),
               Status::RequestHashMismatch);
  PlayIntegrityPolicy other_package = DefaultPolicy();
  other_package.package_name = "io.fortisnet.other";
  other_package.package_name_length = strlen(other_package.package_name);
  CHECK_STATUS(Run(Verdict{}.Json(), other_package), Status::PackageMismatch);
  printf("wrong package, hash, nonce, app, certificate, version, device and license (%zu cases): ok\n",
         cases.size() + 9);
}

void TestDuplicatesAndAliases() {
  struct Case { std::string label; Verdict verdict; };
  std::vector<Case> cases;
  auto add = [&](std::string label, auto edit) {
    Verdict verdict;
    edit(verdict);
    cases.push_back({std::move(label), verdict});
  };
  const std::string hash = Quote(kHash);
  add("requestPackageName", [](Verdict& v) { v.request_extra = "\"requestPackageName\":\"io.fortisnet.wallet\""; });
  add("requestHash same", [&](Verdict& v) { v.request_extra = "\"requestHash\":" + hash; });
  add("requestHash other", [](Verdict& v) { v.request_extra = "\"requestHash\":\"x\""; });
  add("requestHash escaped key", [&](Verdict& v) { v.request_extra = "\"requestH\\u0061sh\":" + hash; });
  add("timestampMillis", [](Verdict& v) { v.request_extra = "\"timestampMillis\":\"1\""; });
  add("nonce twice", [&](Verdict& v) { v.request_extra = "\"nonce\":\"a\",\"nonce\":\"b\""; });
  add("appRecognitionVerdict", [](Verdict& v) { v.app_extra = "\"appRecognitionVerdict\":\"UNEVALUATED\""; });
  add("appRecognitionVerdict same", [](Verdict& v) { v.app_extra = "\"appRecognitionVerdict\":\"PLAY_RECOGNIZED\""; });
  add("packageName", [](Verdict& v) { v.app_extra = "\"packageName\":\"io.fortisnet.wallet\""; });
  add("certificateSha256Digest", [](Verdict& v) { v.app_extra = "\"certificateSha256Digest\":[]"; });
  add("versionCode", [](Verdict& v) { v.app_extra = "\"versionCode\":\"43\""; });
  add("deviceRecognitionVerdict", [](Verdict& v) { v.device_extra = "\"deviceRecognitionVerdict\":[]"; });
  add("appLicensingVerdict", [](Verdict& v) { v.payload_extra = ""; v.licensing = "\"LICENSED\",\"appLicensingVerdict\":\"LICENSED\""; });
  add("isTestingResponse", [](Verdict& v) { v.testing = "{\"isTestingResponse\":false,\"isTestingResponse\":false}"; });
  add("requestDetails object", [](Verdict& v) { v.payload_extra = "\"requestDetails\":{}"; });
  add("appIntegrity object", [](Verdict& v) { v.payload_extra = "\"appIntegrity\":{}"; });
  add("deviceIntegrity object", [](Verdict& v) { v.payload_extra = "\"deviceIntegrity\":{}"; });
  add("accountDetails object", [](Verdict& v) { v.payload_extra = "\"accountDetails\":{}"; });
  add("testingDetails object", [](Verdict& v) { v.payload_extra = "\"testingDetails\":{},\"testingDetails\":{}"; });
  add("tokenPayloadExternal", [](Verdict& v) { v.outer_extra = "\"tokenPayloadExternal\":{}"; });
  add("tokenPayloadExternal escaped key", [](Verdict& v) { v.outer_extra = "\"tokenPayload\\u0045xternal\":{}"; });
  // proto3 JSON parsers accept the snake_case names too; they are refused.
  add("request_hash alias", [&](Verdict& v) { v.request_extra = "\"request_hash\":" + hash; });
  add("request_package_name alias", [](Verdict& v) { v.request_extra = "\"request_package_name\":\"x\""; });
  add("timestamp_millis alias", [](Verdict& v) { v.request_extra = "\"timestamp_millis\":\"1\""; });
  add("app_recognition_verdict alias", [](Verdict& v) { v.app_extra = "\"app_recognition_verdict\":\"X\""; });
  add("package_name alias", [](Verdict& v) { v.app_extra = "\"package_name\":\"x\""; });
  add("certificate_sha256_digest alias", [](Verdict& v) { v.app_extra = "\"certificate_sha256_digest\":[]"; });
  add("version_code alias", [](Verdict& v) { v.app_extra = "\"version_code\":\"1\""; });
  add("device_recognition_verdict alias", [](Verdict& v) { v.device_extra = "\"device_recognition_verdict\":[]"; });
  add("app_licensing_verdict alias", [](Verdict& v) { v.licensing = "\"LICENSED\",\"app_licensing_verdict\":\"X\""; });
  add("request_details alias", [](Verdict& v) { v.payload_extra = "\"request_details\":{}"; });
  add("app_integrity alias", [](Verdict& v) { v.payload_extra = "\"app_integrity\":{}"; });
  add("device_integrity alias", [](Verdict& v) { v.payload_extra = "\"device_integrity\":{}"; });
  add("account_details alias", [](Verdict& v) { v.payload_extra = "\"account_details\":{}"; });
  add("testing_details alias", [](Verdict& v) { v.payload_extra = "\"testing_details\":{}"; });
  add("is_testing_response alias", [](Verdict& v) { v.testing = "{\"is_testing_response\":false}"; });
  add("token_payload_external alias", [](Verdict& v) { v.outer_extra = "\"token_payload_external\":{}"; });
  for (const Case& item : cases) {
    const Status status = Run(item.verdict);
    if (status != Status::PayloadMalformed) {
      fprintf(stderr, "duplicate %s: %s\n", item.label.c_str(), fortis::auth::PlayIntegrityStatusName(status));
      abort();
    }
  }
  // Unread fields may repeat (forward compatible, never trusted).
  Verdict unread;
  unread.payload_extra = "\"environmentDetails\":{},\"environmentDetails\":{}";
  unread.device_extra = "\"deviceAttributes\":{},\"deviceAttributes\":{\"sdkVersion\":33}";
  CHECK_STATUS(Run(unread), Status::Verified);
  printf("duplicate and snake_case alias security fields (%zu cases): ok\n", cases.size());
}

void TestTypes() {
  struct Case { std::string label; Verdict verdict; };
  std::vector<Case> cases;
  auto add = [&](std::string label, auto edit) {
    Verdict verdict;
    edit(verdict);
    cases.push_back({std::move(label), verdict});
  };
  for (const char* timestamp :
       {"1761652799000", "\"+1761652799000\"", "\"01761652799000\"", "\"1761652799000.0\"",
        "\"\"", "\" 1761652799000\"", "\"-1\"", "\"17616527990000000000\"",
        "\"9223372036854775808\"", "\"1e12\"", "null", "true", "[\"1761652799000\"]", "{}",
        "\"0x1\"", "\"1761652799000\\u0000\""})
    add(std::string("timestamp ") + timestamp, [&](Verdict& v) { v.timestamp = timestamp; });
  for (const char* version : {"42", "\"042\"", "\"-42\"", "\"4 2\"", "\"\"", "\"42.0\"", "null",
                              "\"99999999999999999999\""})
    add(std::string("versionCode ") + version, [&](Verdict& v) { v.version_code = version; });
  add("requestPackageName number", [](Verdict& v) { v.request_package = "1"; });
  add("requestPackageName null", [](Verdict& v) { v.request_package = "null"; });
  add("requestPackageName NUL", [](Verdict& v) { v.request_package = "\"io.fortisnet.wallet\\u0000\""; });
  add("requestHash array", [](Verdict& v) { v.request_hash = "[\"" + std::string(kHash) + "\"]"; });
  add("requestHash null", [](Verdict& v) { v.request_hash = "null"; });
  add("nonce number", [](Verdict& v) { v.nonce = "1"; });
  add("appRecognitionVerdict number", [](Verdict& v) { v.app_verdict = "1"; });
  add("appRecognitionVerdict array", [](Verdict& v) { v.app_verdict = "[\"PLAY_RECOGNIZED\"]"; });
  add("packageName object", [](Verdict& v) { v.app_package = "{}"; });
  add("certificate string", [](Verdict& v) { v.certificates = Quote(kCertBase64); });
  add("certificate entry number", [](Verdict& v) { v.certificates = "[1]"; });
  add("certificate entry null", [](Verdict& v) { v.certificates = "[null]"; });
  add("certificate nested", [](Verdict& v) { v.certificates = "[[" + Quote(kCertBase64) + "]]"; });
  add("certificate hex", [](Verdict& v) { v.certificates = "[" + Quote(kCertHex) + "]"; });
  add("certificate standard base64", [](Verdict& v) {
    v.certificates = "[\"yZiltV8amTR+e4XsH/0dCkaB7Gcej2aQQ9Ye8ks8ofs=\"]"; });
  add("certificate bad trailing bits", [](Verdict& v) {
    v.certificates = "[\"yZiltV8amTR-e4XsH_0dCkaB7Gcej2aQQ9Ye8ks8oft\"]"; });
  add("certificate double pad", [](Verdict& v) { v.certificates = "[\"" + std::string(kCertBase64) + "==\"]"; });
  add("certificate unknown entry beside match", [](Verdict& v) {
    v.certificates = "[" + Quote(kCertBase64) + ",\"not-a-digest\"]"; });
  add("device verdict string", [](Verdict& v) { v.device_verdict = "\"MEETS_DEVICE_INTEGRITY\""; });
  add("device verdict null", [](Verdict& v) { v.device_verdict = "null"; });
  add("device entry null", [](Verdict& v) { v.device_verdict = "[\"MEETS_DEVICE_INTEGRITY\",null]"; });
  add("device entry object", [](Verdict& v) { v.device_verdict = "[{\"MEETS_DEVICE_INTEGRITY\":1}]"; });
  add("licensing number", [](Verdict& v) { v.licensing = "1"; });
  add("testing flag string", [](Verdict& v) { v.testing = "{\"isTestingResponse\":\"true\"}"; });
  add("testingDetails array", [](Verdict& v) { v.testing = "[]"; });
  add("testingDetails null", [](Verdict& v) { v.testing = "null"; });
  add("requestDetails array", [](Verdict& v) { v.omit_request = true; v.payload_extra = "\"requestDetails\":[]"; });
  add("appIntegrity null", [](Verdict& v) { v.omit_app = true; v.payload_extra = "\"appIntegrity\":null"; });
  add("deviceIntegrity string", [](Verdict& v) { v.omit_device = true; v.payload_extra = "\"deviceIntegrity\":\"x\""; });
  add("accountDetails array", [](Verdict& v) { v.omit_account = true; v.payload_extra = "\"accountDetails\":[]"; });
  for (const Case& item : cases) {
    const Status status = Run(item.verdict);
    if (status != Status::PayloadMalformed) {
      fprintf(stderr, "type %s: %s\n", item.label.c_str(), fortis::auth::PlayIntegrityStatusName(status));
      abort();
    }
  }
  const std::string payload = Verdict{}.Payload();
  for (const std::string& body :
       {std::string("[]"), std::string("\"x\""), std::string("null"),
        "{\"tokenPayloadExternal\":\"" + std::string(kHash) + "\"}",
        "{\"tokenPayloadExternal\":[" + payload + "]}", std::string("{\"tokenPayloadExternal\":null}"),
        payload /* the bare payload is not a decode response */})
    CHECK_STATUS(Run(body), Status::PayloadMalformed);
  printf("ambiguous and wrong JSON types (%zu cases): ok\n", cases.size() + 7);
}

void TestMalformedJson() {
  const std::string valid = Verdict{}.Json();
  std::vector<std::string> bodies = {
      "", " ", valid.substr(0, valid.size() - 1), valid.substr(1), valid + "}", valid + "x",
      valid + valid, valid + ",", "\xef\xbb\xbf" + valid, "//c\n" + valid,
      std::string(valid).replace(valid.find("\"io."), 1, "'"),
  };
  std::string trailing_comma = valid;
  trailing_comma.insert(trailing_comma.size() - 2, ",");
  bodies.push_back(trailing_comma);
  std::string lone = valid;
  lone.replace(lone.find("PLAY_RECOGNIZED"), 4, "\\ud800");
  bodies.push_back(lone);
  std::string bad_utf8 = valid;
  bad_utf8.replace(bad_utf8.find("PLAY_RECOGNIZED"), 1, "\xff");
  bodies.push_back(bad_utf8);
  std::string control = valid;
  control.replace(control.find("PLAY_RECOGNIZED"), 1, "\n");
  bodies.push_back(control);
  std::string nan = valid;
  nan.insert(1, "\"x\":NaN,");
  bodies.push_back(nan);
  std::string bad_escape = valid;
  bad_escape.replace(bad_escape.find("PLAY_RECOGNIZED"), 1, "\\x");
  bodies.push_back(bad_escape);
  std::string embedded_nul = valid;
  embedded_nul.insert(embedded_nul.size() - 1, std::string("\0", 1));
  bodies.push_back(embedded_nul);
  for (const std::string& body : bodies) CHECK_STATUS(Run(body), Status::PayloadMalformed);
  PlayIntegrityEvidence evidence;
  CHECK_STATUS(fortis::auth::VerifyPlayIntegrityVerdict(DefaultPolicy(), PlayIntegrityDecodeSource::Null(),
                                                  kHash, 43, kNow, &evidence),
               Status::PayloadMalformed);
  printf("malformed JSON and incomplete consumption (%zu cases): ok\n", bodies.size() + 1);
}

void TestBounds() {
  // Size: exactly kPlayVerdictMaxBytes passes, one more byte does not.
  Verdict sized;
  const size_t base = sized.Json().size() + strlen(",\"pad\":\"\"");
  sized.outer_extra = "\"pad\":\"" + std::string(fortis::auth::kPlayVerdictMaxBytes - base, 'p') + "\"";
  CHECK(sized.Json().size() == fortis::auth::kPlayVerdictMaxBytes);
  CHECK_STATUS(Run(sized), Status::Verified);
  sized.outer_extra = "\"pad\":\"" + std::string(fortis::auth::kPlayVerdictMaxBytes - base + 1, 'p') + "\"";
  CHECK_STATUS(Run(sized), Status::PayloadMalformed);
  CHECK_STATUS(Run(Verdict{}.Json() + std::string(fortis::auth::kPlayVerdictMaxBytes, ' ')),
               Status::PayloadMalformed);

  // Tokens: an unknown array fills the arena.
  const size_t base_tokens = 40;
  auto numbers = [](size_t count) {
    std::string array = "[";
    for (size_t i = 0; i < count; ++i) array += i ? ",0" : "0";
    return array + "]";
  };
  Verdict tokens;
  tokens.outer_extra = "\"many\":" + numbers(fortis::auth::kPlayVerdictMaxTokens - base_tokens - 20);
  CHECK_STATUS(Run(tokens), Status::Verified);
  tokens.outer_extra = "\"many\":" + numbers(fortis::auth::kPlayVerdictMaxTokens);
  CHECK_STATUS(Run(tokens), Status::PayloadMalformed);

  // Depth: the parser's 16-level bound counts the outer object and payload.
  auto nested = [](size_t depth) { return std::string(depth, '[') + std::string(depth, ']'); };
  Verdict deep;
  deep.payload_extra = "\"deep\":" + nested(14);
  CHECK_STATUS(Run(deep), Status::Verified);
  deep.payload_extra = "\"deep\":" + nested(15);
  CHECK_STATUS(Run(deep), Status::PayloadMalformed);

  // Strings.
  Verdict long_package;
  long_package.request_package = Quote("a." + std::string(254, 'b'));
  CHECK_STATUS(Run(long_package), Status::PayloadMalformed);
  long_package.request_package = Quote("a." + std::string(253, 'b'));
  CHECK_STATUS(Run(long_package), Status::PackageMismatch);
  long_package = Verdict{};
  long_package.app_package = Quote(std::string(256, 'a'));
  CHECK_STATUS(Run(long_package), Status::PayloadMalformed);
  Verdict long_hash;
  long_hash.request_hash = Quote(std::string(501, 'A'));
  CHECK_STATUS(Run(long_hash), Status::PayloadMalformed);
  long_hash.request_hash = Quote(std::string(500, 'A'));
  CHECK_STATUS(Run(long_hash), Status::RequestHashMismatch);
  long_hash = Verdict{};
  // Length counts after decoding: 501 escaped "A"s are 501 bytes.
  std::string escapes;
  for (int i = 0; i < 501; ++i) escapes += "\\u0041";
  long_hash.request_hash = Quote(escapes);
  CHECK_STATUS(Run(long_hash), Status::PayloadMalformed);
  Verdict long_label;
  long_label.device_verdict = "[\"MEETS_DEVICE_INTEGRITY\",\"" + std::string(65, 'L') + "\"]";
  CHECK_STATUS(Run(long_label), Status::PayloadMalformed);
  long_label.device_verdict = "[\"MEETS_DEVICE_INTEGRITY\",\"" + std::string(64, 'L') + "\"]";
  CHECK_STATUS(Run(long_label), Status::Verified);
  Verdict long_verdict;
  long_verdict.app_verdict = Quote(std::string(65, 'P'));
  CHECK_STATUS(Run(long_verdict), Status::PayloadMalformed);
  long_verdict.app_verdict.clear();
  long_verdict.licensing = Quote(std::string(65, 'L'));
  CHECK_STATUS(Run(long_verdict), Status::PayloadMalformed);
  Verdict long_cert;
  long_cert.certificates = "[\"" + std::string(65, 'A') + "\"]";
  CHECK_STATUS(Run(long_cert), Status::PayloadMalformed);

  // Arrays.
  auto repeat = [](const std::string& item, size_t count) {
    std::string array = "[";
    for (size_t i = 0; i < count; ++i) array += (i ? "," : "") + item;
    return array + "]";
  };
  Verdict certs;
  certs.certificates = repeat(Quote(kCertBase64), fortis::auth::kPlayVerdictMaxCertificates);
  CHECK_STATUS(Run(certs), Status::Verified);
  certs.certificates = repeat(Quote(kCertBase64), fortis::auth::kPlayVerdictMaxCertificates + 1);
  CHECK_STATUS(Run(certs), Status::PayloadMalformed);
  Verdict labels;
  labels.device_verdict = repeat("\"MEETS_DEVICE_INTEGRITY\"", fortis::auth::kPlayVerdictMaxDeviceLabels);
  CHECK_STATUS(Run(labels), Status::Verified);
  labels.device_verdict = repeat("\"MEETS_DEVICE_INTEGRITY\"", fortis::auth::kPlayVerdictMaxDeviceLabels + 1);
  CHECK_STATUS(Run(labels), Status::PayloadMalformed);

  // CPU: the worst bounded input, many times: as many long unknown keys
  // as the token arena holds, each compared with every member name.
  Verdict worst;
  std::string keys;
  for (int i = 0; i < 230; ++i)
    keys += (i ? "," : "") + std::string("\"requestPackageNamf") + std::string(40, 'x') +
            std::to_string(i) + "\":0";
  worst.request_extra = keys;
  CHECK(worst.Json().size() > 15000 && worst.Json().size() <= fortis::auth::kPlayVerdictMaxBytes);
  CHECK_STATUS(Run(worst), Status::Verified);
  const auto start = std::chrono::steady_clock::now();
  // Sanitizers make the bounded parser's repeated string comparisons much
  // slower; a smaller positive count keeps the same input/path under ASan.
  int rounds = 2000;
  if (const char* override = getenv("FORTIS_PLAY_TEST_STRESS_ROUNDS")) {
    char* end = nullptr;
    const long parsed = strtol(override, &end, 10);
    CHECK(end != override && *end == '\0' && parsed >= 1 && parsed <= 2000);
    rounds = static_cast<int>(parsed);
  }
  for (int i = 0; i < rounds; ++i) CHECK_STATUS(Run(worst), Status::Verified);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  printf("bounds: 16 KiB, 512 tokens, depth 16, string/array caps: ok "
         "(%d worst-case %zu-byte verifications in %.1f ms)\n",
         rounds, worst.Json().size(), ms);
}

void TestTimestamps() {
  PlayIntegrityPolicy policy = DefaultPolicy();
  auto at = [](int64_t timestamp) {
    Verdict verdict;
    verdict.timestamp = Quote(std::to_string(timestamp));
    return verdict.Json();
  };
  const int64_t age = fortis::auth::kPlayDefaultMaxAgeMs, skew = fortis::auth::kPlayDefaultMaxFutureSkewMs;
  CHECK_STATUS(Run(at(kNow), policy), Status::Verified);
  CHECK_STATUS(Run(at(kNow - age), policy), Status::Verified);
  CHECK_STATUS(Run(at(kNow - age - 1), policy), Status::TimestampStale);
  CHECK_STATUS(Run(at(kNow + skew), policy), Status::Verified);
  CHECK_STATUS(Run(at(kNow + skew + 1), policy), Status::TimestampFuture);
  CHECK_STATUS(Run(at(0), policy), Status::TimestampStale);
  CHECK_STATUS(Run(at(INT64_MAX), policy), Status::TimestampFuture);
  policy.max_age_ms = 1000;
  policy.max_future_skew_ms = 0;
  CHECK_STATUS(Run(at(kNow - 1000), policy), Status::Verified);
  CHECK_STATUS(Run(at(kNow - 1001), policy), Status::TimestampStale);
  CHECK_STATUS(Run(at(kNow + 1), policy), Status::TimestampFuture);
  policy.max_age_ms = fortis::auth::kPlayMaxAgeLimitMs;
  policy.max_future_skew_ms = fortis::auth::kPlayMaxFutureSkewLimitMs;
  CHECK_STATUS(Run(at(kNow - fortis::auth::kPlayMaxAgeLimitMs), policy), Status::Verified);
  CHECK_STATUS(Run(at(kNow + fortis::auth::kPlayMaxFutureSkewLimitMs), policy), Status::Verified);
  CHECK_STATUS(Run(at(0), DefaultPolicy(), 0), Status::Verified);
  CHECK_STATUS(Run(at(INT64_MAX), DefaultPolicy(), INT64_MAX), Status::Verified);
  CHECK_STATUS(Run(at(0), DefaultPolicy(), INT64_MAX), Status::TimestampStale);
  puts("timestamp window boundaries (inclusive age and skew, extremes): ok");
}

void TestTestingResponses() {
  for (const char* testing : {"{\"isTestingResponse\":true}", "{\"isTestingResponse\":false}", "{}"}) {
    Verdict verdict;
    verdict.testing = testing;
    CHECK_STATUS(Run(verdict), Status::TestingResponse);
    PlayIntegrityPolicy allowed = DefaultPolicy();
    allowed.allow_testing_response = true;
    CHECK_STATUS(Run(verdict.Json(), allowed), Status::Verified);
  }
  puts("testingDetails refused unless the policy allows tester overrides: ok");
}

void TestPolicy() {
  const std::string valid = Verdict{}.Json();
  std::vector<PlayIntegrityPolicy> bad;
  auto add = [&](auto edit) {
    PlayIntegrityPolicy policy = DefaultPolicy();
    edit(policy);
    bad.push_back(policy);
  };
  uint8_t five[5][32]{};
  add([](PlayIntegrityPolicy& p) { p.certificates = nullptr; });
  add([](PlayIntegrityPolicy& p) { p.certificate_count = 0; });
  add([&](PlayIntegrityPolicy& p) { p.certificates = five; p.certificate_count = 5; });
  add([](PlayIntegrityPolicy& p) { p.package_name = nullptr; });
  add([](PlayIntegrityPolicy& p) { p.package_name = "wallet"; p.package_name_length = 6; });
  add([](PlayIntegrityPolicy& p) { p.package_name_length = 0; });
  add([](PlayIntegrityPolicy& p) { p.min_version_code = 0; });
  add([](PlayIntegrityPolicy& p) { p.min_version_code = -1; });
  add([](PlayIntegrityPolicy& p) { p.max_age_ms = 0; });
  add([](PlayIntegrityPolicy& p) { p.max_age_ms = fortis::auth::kPlayMaxAgeLimitMs + 1; });
  add([](PlayIntegrityPolicy& p) { p.max_future_skew_ms = -1; });
  add([](PlayIntegrityPolicy& p) { p.max_future_skew_ms = fortis::auth::kPlayMaxFutureSkewLimitMs + 1; });
  for (const PlayIntegrityPolicy& policy : bad) CHECK_STATUS(Run(valid, policy), Status::PolicyInvalid);
  CHECK_STATUS(Run(valid, DefaultPolicy(), -1), Status::PolicyInvalid);
  const std::string hash = kHash;
  for (const std::string& expected :
       {hash.substr(0, 42), hash + "=", hash + "A", std::string("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB"),
        std::string("")})
    CHECK_STATUS(Run(valid, DefaultPolicy(), kNow, nullptr, expected.c_str()), Status::PolicyInvalid);
  CHECK_STATUS(Run(valid, DefaultPolicy(), kNow, nullptr, nullptr), Status::PolicyInvalid);
  CHECK_STATUS(fortis::auth::VerifyPlayIntegrityVerdict(DefaultPolicy(), PlayIntegrityDecodeSource::Wrap(valid),
                                                  kHash, 43, kNow, nullptr),
               Status::PolicyInvalid);
  printf("invalid policy, expected hash, clock and output (%zu cases): ok\n", bad.size() + 8);
}

void TestEvidenceReset() {
  PlayIntegrityEvidence evidence;
  CHECK_STATUS(Run(Verdict{}, kNow, &evidence), Status::Verified);
  Verdict failing;
  failing.device_verdict = "[]";
  CHECK_STATUS(Run(failing, kNow, &evidence), Status::DeviceIntegrityFailed);
  CHECK(evidence.version_code == 0 && evidence.timestamp_ms == 0 &&
        evidence.device_label == PlayDeviceLabel::None && evidence.licensing == PlayLicensing::Absent);
  CHECK(sizeof(PlayIntegrityEvidence) <= 24);
  puts("evidence is reset on failure and holds no payload text: ok");
}

// Nothing the validator or the hash function does writes to stdout or
// stderr, and status names never echo payload or token text.
void TestRedaction() {
  fflush(stdout);
  fflush(stderr);
  char path[] = "/tmp/play_integrity_redaction_XXXXXX";
  const int capture = mkstemp(path);
  CHECK(capture >= 0);
  const int saved_out = dup(1), saved_err = dup(2);
  CHECK(dup2(capture, 1) >= 0 && dup2(capture, 2) >= 0);
  const char secret[] = "ya29.SECRET-OAUTH-TOKEN";
  Verdict verdict;
  verdict.request_package = Quote(secret);
  Run(verdict);
  verdict = Verdict{};
  verdict.request_hash = Quote(secret);
  Run(verdict);
  Run(std::string("{\"tokenPayloadExternal\":\"") + secret);
  char output[44];
  ComputePlayIntegrityRequestHash(secret, strlen(secret), "android", 7, kPackage, 19, "1", 1, output);
  fflush(stdout);
  fflush(stderr);
  CHECK(dup2(saved_out, 1) >= 0 && dup2(saved_err, 2) >= 0);
  close(saved_out);
  close(saved_err);
  struct stat info{};
  CHECK(fstat(capture, &info) == 0);
  close(capture);
  unlink(path);
  CHECK(info.st_size == 0);
  for (int i = 0; i <= static_cast<int>(Status::DeviceIntegrityFailed); ++i) {
    const char* name = fortis::auth::PlayIntegrityStatusName(static_cast<Status>(i));
    CHECK(name && strcmp(name, "Unknown") != 0 && !strstr(name, "SECRET"));
  }
  for (int i = 0; i <= static_cast<int>(PlayRequestHashStatus::Internal); ++i)
    CHECK(strcmp(fortis::auth::PlayRequestHashStatusName(static_cast<PlayRequestHashStatus>(i)), "Unknown") != 0);
  puts("no output, constant status names (redaction): ok");
}

}  // namespace

int main() try {
  CHECK(ParsePlayCertificateDigest(kCertBase64, strlen(kCertBase64),
                                   PlayCertificateDigestFormat::Base64Url, g_release_cert[0]));
  TestRequestHashVectors();
  TestRequestHashRefusals();
  TestCertificateDigests();
  TestValidVerdicts();
  TestReordered();
  TestMissing();
  TestWrongValues();
  TestDuplicatesAndAliases();
  TestTypes();
  TestMalformedJson();
  TestBounds();
  TestTimestamps();
  TestTestingResponses();
  TestPolicy();
  TestEvidenceReset();
  TestRedaction();
  puts("play integrity C1 validator test passed");
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
