#include "play_integrity.h"

#include "bounded_json.h"

#include <mbedtls/sha256.h>
#include <string.h>

namespace fortis::auth {
namespace {

constexpr char kBase64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr size_t kHexColonDigestChars = 32 * 2 + 31;
// Google bounds the requestHash it echoes to 500 bytes.
constexpr size_t kGoogleRequestHashMaxBytes = 500;
constexpr size_t kInt64MaxDigits = 19;

int base64url_value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

int hex_value(unsigned char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Exactly 43 base64url characters whose last carries zero unused bits:
// the one canonical unpadded spelling of 32 bytes.
bool base64url_decode32(const char* text, size_t length, uint8_t output[32]) {
  if (!text || length != 43) return false;
  uint32_t buffer = 0;
  unsigned bits = 0;
  size_t written = 0;
  for (size_t i = 0; i < length; ++i) {
    const int value = base64url_value(static_cast<unsigned char>(text[i]));
    if (value < 0) return false;
    buffer = (buffer << 6) | static_cast<uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      output[written++] = static_cast<uint8_t>(buffer >> bits);
      buffer &= (1u << bits) - 1;
    }
  }
  return buffer == 0 && written == 32;
}

void base64url_encode32(const uint8_t input[32], char output[kPlayRequestHashChars + 1]) {
  size_t out = 0;
  for (size_t i = 0; i < 30; i += 3) {
    output[out++] = kBase64Url[input[i] >> 2];
    output[out++] = kBase64Url[((input[i] & 3) << 4) | (input[i + 1] >> 4)];
    output[out++] = kBase64Url[((input[i + 1] & 15) << 2) | (input[i + 2] >> 6)];
    output[out++] = kBase64Url[input[i + 2] & 63];
  }
  output[out++] = kBase64Url[input[30] >> 2];
  output[out++] = kBase64Url[((input[30] & 3) << 4) | (input[31] >> 4)];
  output[out++] = kBase64Url[(input[31] & 15) << 2];
  output[out] = '\0';
}

// The minted challenge and the requestHash share this form.
bool is_base64url32(const char* text, size_t length) {
  uint8_t ignored[32];
  return base64url_decode32(text, length, ignored);
}

// Well-formed UTF-8 (no overlongs, surrogates or values past U+10FFFF)
// without U+0000-U+001F or U+007F.
bool is_app_version(const char* text, size_t length) {
  if (!text || length == 0 || length > kPlayAppVersionMaxBytes) return false;
  const unsigned char* bytes = reinterpret_cast<const unsigned char*>(text);
  for (size_t i = 0; i < length;) {
    const unsigned char c = bytes[i++];
    if (c < 0x20 || c == 0x7f) return false;
    if (c < 0x80) continue;
    const unsigned trailing = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 :
                              (c & 0xf8) == 0xf0 ? 3 : 0;
    if (trailing == 0 || length - i < trailing) return false;
    uint32_t scalar = c & (trailing == 1 ? 0x1f : trailing == 2 ? 0x0f : 0x07);
    for (unsigned k = 0; k < trailing; ++k) {
      const unsigned char next = bytes[i++];
      if ((next & 0xc0) != 0x80) return false;
      scalar = (scalar << 6) | (next & 0x3f);
    }
    if ((trailing == 1 && scalar < 0x80) || (trailing == 2 && scalar < 0x800) ||
        (trailing == 3 && scalar < 0x10000) || scalar > 0x10ffff ||
        (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
  }
  return true;
}

// One LP() field of the requestHash preimage.
bool hash_field(mbedtls_sha256_context* context, const char* bytes, size_t length) {
  const uint8_t prefix[4] = {static_cast<uint8_t>(length >> 24), static_cast<uint8_t>(length >> 16),
                             static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length)};
  return mbedtls_sha256_update(context, prefix, sizeof(prefix)) == 0 &&
         mbedtls_sha256_update(context, reinterpret_cast<const uint8_t*>(bytes), length) == 0;
}

// Canonical non-negative decimal int64: 1..19 digits, no sign, no leading
// zero except "0" itself, at most INT64_MAX.
bool parse_int64_text(const char* text, size_t length, int64_t* output) {
  if (length == 0 || length > kInt64MaxDigits || (length > 1 && text[0] == '0')) return false;
  uint64_t value = 0;
  for (size_t i = 0; i < length; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    value = value * 10 + static_cast<uint64_t>(text[i] - '0');
  }
  if (value > static_cast<uint64_t>(INT64_MAX)) return false;
  *output = static_cast<int64_t>(value);
  return true;
}

// ---- Verdict structure ----------------------------------------------------

using Document = codec::BoundedJsonDocument;

// A member the validator reads: its camelCase name, the snake_case proto
// field name that a proto3 JSON parser would also accept (refused here as
// ambiguous), and the value token once collected.
struct Member {
  const char* name;
  const char* alias;
  uint32_t value;
};

// Records each member's value from `object` (an object token, or
// kBoundedJsonNoToken for an absent object, which leaves all absent).
// False when a member appears twice, including through escapes, or under
// its alias. One pass over the keys; the member lists are fixed and small.
bool collect(const Document& document, uint32_t object, Member* members, size_t count) {
  for (size_t i = 0; i < count; ++i) members[i].value = codec::kBoundedJsonNoToken;
  if (object == codec::kBoundedJsonNoToken) return true;
  const codec::BoundedJsonToken* item = document.token(object);
  if (!item || item->type != codec::BoundedJsonType::kObject) return false;
  for (uint32_t key = item->first_child; key != codec::kBoundedJsonNoToken;) {
    const codec::BoundedJsonToken* key_token = document.token(key);
    if (!key_token) return false;
    const uint32_t value = key_token->next_sibling;
    const codec::BoundedJsonToken* value_token = document.token(value);
    if (!value_token) return false;
    for (size_t i = 0; i < count; ++i) {
      if (document.StringEquals(key, members[i].name)) {
        if (members[i].value != codec::kBoundedJsonNoToken) return false;
        members[i].value = value;
      } else if (members[i].alias && document.StringEquals(key, members[i].alias)) {
        return false;
      }
    }
    key = value_token->next_sibling;
  }
  return true;
}

bool is_type(const Document& document, uint32_t index, codec::BoundedJsonType type) {
  const codec::BoundedJsonToken* item = document.token(index);
  return item && item->type == type;
}

// Absent, or an object.
bool optional_object(const Document& document, uint32_t index) {
  return index == codec::kBoundedJsonNoToken || is_type(document, index, codec::BoundedJsonType::kObject);
}

// Decodes a string token of at most capacity - 1 UTF-8 bytes without NUL.
bool decode_text(const Document& document, uint32_t index, char* output, size_t capacity,
                 size_t* length) {
  size_t used = 0;
  if (!is_type(document, index, codec::BoundedJsonType::kString) ||
      !document.DecodeString(index, output, capacity, &used) || strlen(output) != used)
    return false;
  *length = used;
  return true;
}

// A decoded string field of at most `capacity - 1` bytes, if present.
struct Text {
  bool present = false;
  size_t length = 0;
};
bool optional_text(const Document& document, uint32_t index, char* output, size_t capacity,
                   Text* text) {
  output[0] = '\0';
  if (index == codec::kBoundedJsonNoToken) return true;
  text->present = true;
  return decode_text(document, index, output, capacity, &text->length);
}

bool optional_int64(const Document& document, uint32_t index, bool* present, int64_t* value) {
  if (index == codec::kBoundedJsonNoToken) return true;
  char digits[kInt64MaxDigits + 1];
  size_t length = 0;
  *present = true;
  return decode_text(document, index, digits, sizeof(digits), &length) &&
         parse_int64_text(digits, length, value);
}

bool equals(const char* text, size_t length, const char* expected, size_t expected_length) {
  return length == expected_length && memcmp(text, expected, length) == 0;
}

bool equals_literal(const char* text, size_t length, const char* literal) {
  return equals(text, length, literal, strlen(literal));
}

// certificateSha256Digest entries: base64 web-safe SHA-256, unpadded (43)
// or with its one '=' pad (44); either decodes to the same 32 bytes.
bool verdict_digest(const char* text, size_t length, uint8_t output[32]) {
  if (length == 44 && text[43] == '=') length = 43;
  return base64url_decode32(text, length, output);
}

bool policy_valid(const PlayIntegrityPolicy& policy) {
  return IsPlayPackageName(policy.package_name, policy.package_name_length) &&
         policy.certificates && policy.certificate_count >= 1 &&
         policy.certificate_count <= kPlayPolicyMaxCertificates &&
         policy.min_version_code >= 1 && policy.max_age_ms >= 1 &&
         policy.max_age_ms <= kPlayMaxAgeLimitMs && policy.max_future_skew_ms >= 0 &&
         policy.max_future_skew_ms <= kPlayMaxFutureSkewLimitMs;
}

}  // namespace

const char* PlayRequestHashStatusName(PlayRequestHashStatus status) {
  switch (status) {
    case PlayRequestHashStatus::Ok: return "Ok";
    case PlayRequestHashStatus::ChallengeMalformed: return "ChallengeMalformed";
    case PlayRequestHashStatus::PlatformInvalid: return "PlatformInvalid";
    case PlayRequestHashStatus::PackageInvalid: return "PackageInvalid";
    case PlayRequestHashStatus::AppVersionInvalid: return "AppVersionInvalid";
    case PlayRequestHashStatus::Internal: return "Internal";
  }
  return "Unknown";
}

bool IsPlayPackageName(const char* text, size_t length) {
  if (!text || length == 0 || length > kPlayPackageNameMaxBytes) return false;
  size_t segments = 0;
  bool segment_start = true;
  for (size_t i = 0; i < length; ++i) {
    const char c = text[i];
    const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    if (segment_start) {
      if (!letter) return false;
      segment_start = false;
      ++segments;
    } else if (c == '.') {
      segment_start = true;
    } else if (!letter && !(c >= '0' && c <= '9') && c != '_') {
      return false;
    }
  }
  return !segment_start && segments >= 2;
}

PlayRequestHashStatus ComputePlayIntegrityRequestHash(
    const char* challenge, size_t challenge_length, const char* platform,
    size_t platform_length, const char* package_name, size_t package_name_length,
    const char* app_version, size_t app_version_length,
    char output[kPlayRequestHashChars + 1]) {
  if (output) output[0] = '\0';
  if (!output) return PlayRequestHashStatus::Internal;
  if (!is_base64url32(challenge, challenge_length))
    return PlayRequestHashStatus::ChallengeMalformed;
  if (!platform || !equals_literal(platform, platform_length, kPlayRequestPlatform))
    return PlayRequestHashStatus::PlatformInvalid;
  if (!IsPlayPackageName(package_name, package_name_length))
    return PlayRequestHashStatus::PackageInvalid;
  if (!is_app_version(app_version, app_version_length))
    return PlayRequestHashStatus::AppVersionInvalid;
  uint8_t digest[32];
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  const bool ok = mbedtls_sha256_starts(&context, 0) == 0 &&
      hash_field(&context, kPlayRequestHashDomain, sizeof(kPlayRequestHashDomain) - 1) &&
      hash_field(&context, challenge, challenge_length) &&
      hash_field(&context, platform, platform_length) &&
      hash_field(&context, package_name, package_name_length) &&
      hash_field(&context, app_version, app_version_length) &&
      mbedtls_sha256_finish(&context, digest) == 0;
  mbedtls_sha256_free(&context);
  if (!ok) return PlayRequestHashStatus::Internal;
  base64url_encode32(digest, output);
  return PlayRequestHashStatus::Ok;
}

bool ParsePlayCertificateDigest(const char* text, size_t length,
                                PlayCertificateDigestFormat format, uint8_t output[32]) {
  if (!text || !output) return false;
  uint8_t digest[32];
  switch (format) {
    case PlayCertificateDigestFormat::Base64Url:
      if (!base64url_decode32(text, length, digest)) return false;
      break;
    case PlayCertificateDigestFormat::Hex:
    case PlayCertificateDigestFormat::HexColon: {
      const bool colons = format == PlayCertificateDigestFormat::HexColon;
      if (length != (colons ? kHexColonDigestChars : 64)) return false;
      const size_t stride = colons ? 3 : 2;
      for (size_t i = 0; i < 32; ++i) {
        const size_t at = i * stride;
        if (colons && i > 0 && text[at - 1] != ':') return false;
        const int high = hex_value(static_cast<unsigned char>(text[at]));
        const int low = hex_value(static_cast<unsigned char>(text[at + 1]));
        if (high < 0 || low < 0) return false;
        digest[i] = static_cast<uint8_t>(high << 4 | low);
      }
      break;
    }
    default:
      return false;
  }
  memcpy(output, digest, sizeof(digest));
  return true;
}

const char* PlayIntegrityStatusName(PlayIntegrityStatus status) {
  switch (status) {
    case PlayIntegrityStatus::Verified: return "Verified";
    case PlayIntegrityStatus::PolicyInvalid: return "PolicyInvalid";
    case PlayIntegrityStatus::PayloadMalformed: return "PayloadMalformed";
    case PlayIntegrityStatus::PackageMismatch: return "PackageMismatch";
    case PlayIntegrityStatus::RequestHashMismatch: return "RequestHashMismatch";
    case PlayIntegrityStatus::TimestampMissing: return "TimestampMissing";
    case PlayIntegrityStatus::TimestampStale: return "TimestampStale";
    case PlayIntegrityStatus::TimestampFuture: return "TimestampFuture";
    case PlayIntegrityStatus::TestingResponse: return "TestingResponse";
    case PlayIntegrityStatus::AppNotRecognized: return "AppNotRecognized";
    case PlayIntegrityStatus::CertificateMismatch: return "CertificateMismatch";
    case PlayIntegrityStatus::VersionCodeRejected: return "VersionCodeRejected";
    case PlayIntegrityStatus::LicenseRequired: return "LicenseRequired";
    case PlayIntegrityStatus::DeviceIntegrityFailed: return "DeviceIntegrityFailed";
  }
  return "Unknown";
}

PlayIntegrityStatus VerifyPlayIntegrityVerdict(const PlayIntegrityPolicy& policy,
                                               const PlayIntegrityDecodedResponse& response,
                                               const char* expected_request_hash,
                                               size_t expected_request_hash_length,
                                               int64_t now_ms, PlayIntegrityEvidence* output) {
  using Status = PlayIntegrityStatus;
  if (output) *output = PlayIntegrityEvidence{};
  if (!output || !policy_valid(policy) || now_ms < 0 ||
      !is_base64url32(expected_request_hash, expected_request_hash_length))
    return Status::PolicyInvalid;
  if (!response.body() || response.length() == 0 || response.length() > kPlayVerdictMaxBytes)
    return Status::PayloadMalformed;

  codec::BoundedJsonToken tokens[kPlayVerdictMaxTokens];
  Document document;
  if (!document.ParseWithLimits(response.body(), response.length(), tokens,
                                kPlayVerdictMaxTokens, kPlayVerdictMaxTokens,
                                kPlayVerdictMaxBytes, nullptr) ||
      !is_type(document, document.root(), codec::BoundedJsonType::kObject))
    return Status::PayloadMalformed;

  // {"tokenPayloadExternal": {...}}
  Member outer[] = {{"tokenPayloadExternal", "token_payload_external", 0}};
  if (!collect(document, document.root(), outer, 1) ||
      !is_type(document, outer[0].value, codec::BoundedJsonType::kObject))
    return Status::PayloadMalformed;

  enum { kRequest, kApp, kDevice, kAccount, kTesting };
  Member top[] = {{"requestDetails", "request_details", 0},
                  {"appIntegrity", "app_integrity", 0},
                  {"deviceIntegrity", "device_integrity", 0},
                  {"accountDetails", "account_details", 0},
                  {"testingDetails", "testing_details", 0}};
  if (!collect(document, outer[0].value, top, 5)) return Status::PayloadMalformed;
  for (const Member& member : top)
    if (!optional_object(document, member.value)) return Status::PayloadMalformed;

  enum { kRequestPackage, kRequestHash, kTimestamp, kNonce };
  Member request[] = {{"requestPackageName", "request_package_name", 0},
                      {"requestHash", "request_hash", 0},
                      {"timestampMillis", "timestamp_millis", 0},
                      {"nonce", nullptr, 0}};
  enum { kAppVerdict, kAppPackage, kCertificates, kVersionCode };
  Member app[] = {{"appRecognitionVerdict", "app_recognition_verdict", 0},
                  {"packageName", "package_name", 0},
                  {"certificateSha256Digest", "certificate_sha256_digest", 0},
                  {"versionCode", "version_code", 0}};
  Member device[] = {{"deviceRecognitionVerdict", "device_recognition_verdict", 0}};
  Member account[] = {{"appLicensingVerdict", "app_licensing_verdict", 0}};
  Member testing[] = {{"isTestingResponse", "is_testing_response", 0}};
  if (!collect(document, top[kRequest].value, request, 4) ||
      !collect(document, top[kApp].value, app, 4) ||
      !collect(document, top[kDevice].value, device, 1) ||
      !collect(document, top[kAccount].value, account, 1) ||
      !collect(document, top[kTesting].value, testing, 1))
    return Status::PayloadMalformed;

  // Decode and type-check every read field before judging any of them.
  char request_package[kPlayPackageNameMaxBytes + 1];
  char request_hash[kGoogleRequestHashMaxBytes + 1];
  char nonce[kGoogleRequestHashMaxBytes + 1];
  char app_verdict[kPlayVerdictMaxLabelBytes + 1];
  char app_package[kPlayPackageNameMaxBytes + 1];
  char licensing_text[kPlayVerdictMaxLabelBytes + 1];
  Text request_package_text, request_hash_text, nonce_text, app_verdict_text,
      app_package_text, licensing;
  bool has_timestamp = false, has_version_code = false;
  int64_t timestamp_ms = 0, version_code = 0;
  if (!optional_text(document, request[kRequestPackage].value, request_package,
                     sizeof(request_package), &request_package_text) ||
      !optional_text(document, request[kRequestHash].value, request_hash, sizeof(request_hash),
                     &request_hash_text) ||
      !optional_text(document, request[kNonce].value, nonce, sizeof(nonce), &nonce_text) ||
      !optional_int64(document, request[kTimestamp].value, &has_timestamp, &timestamp_ms) ||
      !optional_text(document, app[kAppVerdict].value, app_verdict, sizeof(app_verdict),
                     &app_verdict_text) ||
      !optional_text(document, app[kAppPackage].value, app_package, sizeof(app_package),
                     &app_package_text) ||
      !optional_int64(document, app[kVersionCode].value, &has_version_code, &version_code) ||
      !optional_text(document, account[0].value, licensing_text, sizeof(licensing_text),
                     &licensing))
    return Status::PayloadMalformed;

  bool testing_response = false;
  if (top[kTesting].value != codec::kBoundedJsonNoToken) {
    // Any testingDetails marks a Play Console tester override.
    const uint32_t flag = testing[0].value;
    if (flag != codec::kBoundedJsonNoToken && !is_type(document, flag, codec::BoundedJsonType::kTrue) &&
        !is_type(document, flag, codec::BoundedJsonType::kFalse))
      return Status::PayloadMalformed;
    testing_response = true;
  }

  // certificateSha256Digest: bounded array of digests.
  uint8_t certificates[kPlayVerdictMaxCertificates][32];
  size_t certificate_count = 0;
  if (app[kCertificates].value != codec::kBoundedJsonNoToken) {
    if (!is_type(document, app[kCertificates].value, codec::BoundedJsonType::kArray))
      return Status::PayloadMalformed;
    for (uint32_t entry = document.token(app[kCertificates].value)->first_child;
         entry != codec::kBoundedJsonNoToken; entry = document.token(entry)->next_sibling) {
      char text[kPlayVerdictMaxLabelBytes + 1];
      size_t length = 0;
      if (certificate_count == kPlayVerdictMaxCertificates ||
          !decode_text(document, entry, text, sizeof(text), &length) ||
          !verdict_digest(text, length, certificates[certificate_count]))
        return Status::PayloadMalformed;
      ++certificate_count;
    }
  }

  // deviceRecognitionVerdict: bounded array of labels; unknown ones ignored.
  bool meets_device = false, meets_strong = false;
  if (device[0].value != codec::kBoundedJsonNoToken) {
    if (!is_type(document, device[0].value, codec::BoundedJsonType::kArray))
      return Status::PayloadMalformed;
    size_t labels = 0;
    for (uint32_t entry = document.token(device[0].value)->first_child;
         entry != codec::kBoundedJsonNoToken; entry = document.token(entry)->next_sibling) {
      char label[kPlayVerdictMaxLabelBytes + 1];
      size_t length = 0;
      if (++labels > kPlayVerdictMaxDeviceLabels ||
          !decode_text(document, entry, label, sizeof(label), &length))
        return Status::PayloadMalformed;
      if (equals_literal(label, length, "MEETS_DEVICE_INTEGRITY")) meets_device = true;
      else if (equals_literal(label, length, "MEETS_STRONG_INTEGRITY")) meets_strong = true;
    }
  }

  // requestDetails.
  if (!request_package_text.present ||
      !equals(request_package, request_package_text.length, policy.package_name,
              policy.package_name_length))
    return Status::PackageMismatch;
  if (nonce_text.present || !request_hash_text.present ||
      !equals(request_hash, request_hash_text.length, expected_request_hash,
              expected_request_hash_length))
    return Status::RequestHashMismatch;
  if (!has_timestamp) return Status::TimestampMissing;
  if (timestamp_ms > now_ms) {
    if (timestamp_ms - now_ms > policy.max_future_skew_ms) return Status::TimestampFuture;
  } else if (now_ms - timestamp_ms > policy.max_age_ms) {
    return Status::TimestampStale;
  }

  if (testing_response && !policy.allow_testing_response) return Status::TestingResponse;

  // appIntegrity.
  if (!app_verdict_text.present ||
      !equals_literal(app_verdict, app_verdict_text.length, "PLAY_RECOGNIZED"))
    return Status::AppNotRecognized;
  if (!app_package_text.present ||
      !equals(app_package, app_package_text.length, policy.package_name,
              policy.package_name_length))
    return Status::PackageMismatch;
  bool certificate_match = false;
  for (size_t i = 0; i < certificate_count; ++i)
    for (size_t k = 0; k < policy.certificate_count; ++k)
      if (memcmp(certificates[i], policy.certificates[k], 32) == 0) certificate_match = true;
  if (!certificate_match) return Status::CertificateMismatch;
  if (!has_version_code || version_code < policy.min_version_code)
    return Status::VersionCodeRejected;

  // accountDetails.
  PlayLicensing licensing_verdict = PlayLicensing::Absent;
  if (licensing.present) {
    licensing_verdict =
        equals_literal(licensing_text, licensing.length, "LICENSED") ? PlayLicensing::Licensed
        : equals_literal(licensing_text, licensing.length, "UNLICENSED") ? PlayLicensing::Unlicensed
        : equals_literal(licensing_text, licensing.length, "UNEVALUATED") ? PlayLicensing::Unevaluated
        : PlayLicensing::Other;
  }
  if (policy.require_play_license && licensing_verdict != PlayLicensing::Licensed)
    return Status::LicenseRequired;

  // deviceIntegrity.
  if (!meets_device) return Status::DeviceIntegrityFailed;

  output->version_code = version_code;
  output->timestamp_ms = timestamp_ms;
  output->device_label = meets_strong ? PlayDeviceLabel::StrongIntegrity
                                      : PlayDeviceLabel::DeviceIntegrity;
  output->licensing = licensing_verdict;
  return Status::Verified;
}

}  // namespace fortis::auth
