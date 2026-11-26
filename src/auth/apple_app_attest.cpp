#include "ownership.h"
#include <atomic>
#include "apple_app_attest.h"
#include "log.h"

#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(FORTIS_APPLE_ATTEST_TEST_ROOT) && defined(FORTIS_UNIKRAFT)
#error "The App Attest test root is a host test-only build flag"
#endif

namespace fortis::auth {

// The only issuer of App Attest verdicts in a production build: it is
// reached from EnrollWithAppleAppAttest after VerifyAppleAttestation has
// returned Verified for the request's own challenge.
class DeviceAttestationIssuer {
 public:
  static VerifiedDeviceAttestation IssueApple(const char challenge[44],
                                              const char* app_version,
                                              size_t app_version_length,
                                              const AppleAttestEvidence& evidence,
                                              int64_t verified_at_ms) {
    VerifiedDeviceAttestation proof;
    proof.provider_ = DeviceAttestation::AppleAppAttest;
    proof.platform_ = DevicePlatform::Ios;
    memcpy(proof.challenge_, challenge, 44);
    memcpy(proof.app_version_, app_version, app_version_length);
    proof.app_version_[app_version_length] = '\0';
    proof.app_version_length_ = static_cast<uint16_t>(app_version_length);
    proof.has_app_version_ = true;
    proof.verified_at_ms_ = verified_at_ms;
    memcpy(proof.apple_key_id_, evidence.key_id, 32);
    memcpy(proof.apple_public_key_, evidence.public_key, 65);
    proof.apple_environment_ = evidence.environment;
    proof.apple_validation_category_ = evidence.validation_category;
    memcpy(proof.apple_bundle_version_, evidence.bundle_version,
           evidence.bundle_version_length);
    proof.apple_bundle_version_[evidence.bundle_version_length] = '\0';
    proof.apple_bundle_version_length_ = evidence.bundle_version_length;
    proof.apple_receipt_ = evidence.receipt;
    proof.apple_receipt_length_ = evidence.receipt_length;
    return proof;
  }
};

namespace {

constexpr uint8_t kAaguidProduction[16] = {'a', 'p', 'p', 'a', 't', 't', 'e', 's', 't',
                                           0, 0, 0, 0, 0, 0, 0};
constexpr uint8_t kAaguidDevelopment[16] = {'a', 'p', 'p', 'a', 't', 't', 'e', 's', 't',
                                            'd', 'e', 'v', 'e', 'l', 'o', 'p'};
// 1.2.840.113635.100.8.2 (App Attest nonce) and 1.2.840.113635.100.4.24
// (App Attest extended key usage).
constexpr uint8_t kNonceOid[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x63, 0x64, 0x08, 0x02};
constexpr char kAppAttestEku[] = "\x2a\x86\x48\x86\xf7\x63\x64\x04\x18";
// SubjectPublicKeyInfo prefix of an uncompressed P-256 key: the remaining
// 65 bytes are the X9.62 point.
constexpr uint8_t kP256SpkiPrefix[] = {0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48,
                                       0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48,
                                       0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00};
constexpr char kBase64Url[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int base64url_value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

// Canonical unpadded base64url: alphabet only, no length of 1 mod 4 and
// zero unused bits in the last character, so each byte string has exactly
// one accepted encoding.
bool decode_base64url(const char* text, size_t length, uint8_t* output, size_t capacity,
                      size_t* written) {
  if (!text || !length || length % 4 == 1) return false;
  const size_t bytes = length / 4 * 3 + (length % 4 ? length % 4 - 1 : 0);
  if (bytes > capacity) return false;
  uint32_t buffer = 0;
  unsigned bits = 0;
  size_t out = 0;
  for (size_t i = 0; i < length; ++i) {
    const int value = base64url_value(static_cast<unsigned char>(text[i]));
    if (value < 0) return false;
    buffer = (buffer << 6) | static_cast<uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      output[out++] = static_cast<uint8_t>(buffer >> bits);
      buffer &= (1u << bits) - 1;
    }
  }
  if (buffer != 0 || out != bytes) return false;
  *written = out;
  return true;
}

bool sha256(const uint8_t* first, size_t first_length, const uint8_t* second,
            size_t second_length, uint8_t output[32]) {
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  const bool ok = mbedtls_sha256_starts(&context, 0) == 0 &&
      mbedtls_sha256_update(&context, first, first_length) == 0 &&
      (!second_length || mbedtls_sha256_update(&context, second, second_length) == 0) &&
      mbedtls_sha256_finish(&context, output) == 0;
  mbedtls_sha256_free(&context);
  return ok;
}

bool same(const uint8_t* left, const uint8_t* right, size_t length) {
  uint8_t difference = 0;
  for (size_t i = 0; i < length; ++i) difference |= left[i] ^ right[i];
  return difference == 0;
}

// ---- Strict CBOR (RFC 8949) ---------------------------------------------
//
// Definite lengths only, shortest-form heads only, no tags, no floats; every
// length is checked against the bytes that remain before it is used, so no
// declared size can drive an allocation or a read past the input.

struct Cbor {
  const uint8_t* p;
  const uint8_t* end;
  size_t remaining() const { return static_cast<size_t>(end - p); }
};

bool head(Cbor* in, uint8_t* major, uint64_t* argument, uint8_t* info = nullptr) {
  if (in->p >= in->end) return false;
  const uint8_t initial = *in->p++;
  *major = initial >> 5;
  const uint8_t additional = initial & 31;
  if (info) *info = additional;
  if (additional < 24) {
    *argument = additional;
    return true;
  }
  if (additional > 27) return false;  // reserved or indefinite length
  const size_t size = static_cast<size_t>(1) << (additional - 24);
  if (in->remaining() < size) return false;
  uint64_t value = 0;
  for (size_t i = 0; i < size; ++i) value = (value << 8) | *in->p++;
  if (*major == 7) return false;  // floats and extended simple values
  if ((size == 1 && value < 24) || (size == 2 && value <= 0xff) ||
      (size == 4 && value <= 0xffff) || (size == 8 && value <= 0xffffffffULL))
    return false;  // not the shortest form
  *argument = value;
  return true;
}

// A byte (major 2) or text (major 3) string of at most `max` bytes.
bool string(Cbor* in, uint8_t want, size_t max, const uint8_t** data, size_t* length) {
  uint8_t major = 0;
  uint64_t size = 0;
  if (!head(in, &major, &size) || major != want || size > max || size > in->remaining())
    return false;
  *data = in->p;
  *length = static_cast<size_t>(size);
  in->p += size;
  return true;
}

bool container(Cbor* in, uint8_t want, uint64_t* count) {
  uint8_t major = 0;
  return head(in, &major, count) && major == want;
}

bool text_equals(const uint8_t* data, size_t length, const char* literal) {
  return length == strlen(literal) && memcmp(data, literal, length) == 0;
}

// Skips one data item of an unrecognised extension: depth at most four,
// counts bounded by the bytes left, simple values false/true/null only.
bool skip(Cbor* in, unsigned depth) {
  if (depth > 4) return false;
  uint8_t major = 0, info = 0;
  uint64_t argument = 0;
  if (!head(in, &major, &argument, &info)) return false;
  switch (major) {
    case 0: case 1: return true;
    case 2: case 3:
      if (argument > in->remaining()) return false;
      in->p += argument;
      return true;
    case 4:
      if (argument > in->remaining()) return false;
      for (uint64_t i = 0; i < argument; ++i)
        if (!skip(in, depth + 1)) return false;
      return true;
    case 5:
      if (argument > in->remaining() / 2) return false;
      for (uint64_t i = 0; i < argument; ++i)
        if (!skip(in, depth + 1) || !skip(in, depth + 1)) return false;
      return true;
    case 7: return info >= 20 && info <= 22;
    default: return false;  // tags
  }
}

struct Attestation {
  const uint8_t* certificates[2]{};
  size_t certificate_lengths[2]{};
  const uint8_t* receipt = nullptr;
  size_t receipt_length = 0;
  const uint8_t* auth_data = nullptr;
  size_t auth_data_length = 0;
};

AppleAttestStatus parse_statement(Cbor* in, Attestation* out) {
  uint64_t count = 0;
  if (!container(in, 5, &count) || count != 2) return AppleAttestStatus::AttestationMalformed;
  bool have_x5c = false, have_receipt = false;
  for (unsigned entry = 0; entry < 2; ++entry) {
    const uint8_t* key = nullptr;
    size_t key_length = 0;
    if (!string(in, 3, 16, &key, &key_length)) return AppleAttestStatus::AttestationMalformed;
    if (text_equals(key, key_length, "x5c") && !have_x5c) {
      have_x5c = true;
      uint64_t certificates = 0;
      if (!container(in, 4, &certificates) || certificates != 2)
        return AppleAttestStatus::AttestationMalformed;
      for (unsigned i = 0; i < 2; ++i)
        if (!string(in, 2, kAppleCertificateMaxBytes, &out->certificates[i],
                    &out->certificate_lengths[i]) || !out->certificate_lengths[i])
          return AppleAttestStatus::AttestationMalformed;
    } else if (text_equals(key, key_length, "receipt") && !have_receipt) {
      have_receipt = true;
      if (!string(in, 2, kAppleReceiptMaxBytes, &out->receipt, &out->receipt_length) ||
          !out->receipt_length) return AppleAttestStatus::AttestationMalformed;
    } else {
      return AppleAttestStatus::AttestationMalformed;
    }
  }
  return AppleAttestStatus::Verified;
}

// The attestation object: exactly {fmt, attStmt, authData} in any order,
// fully consumed.
AppleAttestStatus parse_attestation(const uint8_t* data, size_t length, Attestation* out) {
  Cbor in{data, data + length};
  uint64_t count = 0;
  if (!container(&in, 5, &count) || count != 3) return AppleAttestStatus::AttestationMalformed;
  bool have_fmt = false, have_statement = false, have_auth_data = false;
  bool fmt_ok = false;
  for (unsigned entry = 0; entry < 3; ++entry) {
    const uint8_t* key = nullptr;
    size_t key_length = 0;
    if (!string(&in, 3, 16, &key, &key_length)) return AppleAttestStatus::AttestationMalformed;
    if (text_equals(key, key_length, "fmt") && !have_fmt) {
      have_fmt = true;
      const uint8_t* fmt = nullptr;
      size_t fmt_length = 0;
      if (!string(&in, 3, 64, &fmt, &fmt_length)) return AppleAttestStatus::AttestationMalformed;
      fmt_ok = text_equals(fmt, fmt_length, "apple-appattest");
    } else if (text_equals(key, key_length, "attStmt") && !have_statement) {
      have_statement = true;
      const AppleAttestStatus status = parse_statement(&in, out);
      if (status != AppleAttestStatus::Verified) return status;
    } else if (text_equals(key, key_length, "authData") && !have_auth_data) {
      have_auth_data = true;
      if (!string(&in, 2, kAppleAuthDataMaxBytes, &out->auth_data, &out->auth_data_length))
        return AppleAttestStatus::AttestationMalformed;
    } else {
      return AppleAttestStatus::AttestationMalformed;
    }
  }
  if (in.p != in.end) return AppleAttestStatus::AttestationMalformed;
  return fmt_ok ? AppleAttestStatus::Verified : AppleAttestStatus::FormatUnsupported;
}

struct AuthData {
  const uint8_t* rp_id_hash = nullptr;
  uint32_t counter = 0;
  const uint8_t* aaguid = nullptr;
  const uint8_t* credential_id = nullptr;
  const uint8_t* x = nullptr;
  const uint8_t* y = nullptr;
  bool has_extensions = false;
  uint32_t validation_category = 0;
  const uint8_t* bundle_version = nullptr;
  size_t bundle_version_length = 0;
};

// The COSE_Key of an ES256 P-256 credential: exactly kty 2, alg -7, crv 1,
// x and y (32 bytes each), in any order.
bool parse_cose_key(Cbor* in, AuthData* out) {
  uint64_t count = 0;
  if (!container(in, 5, &count) || count != 5) return false;
  unsigned seen = 0;
  for (unsigned entry = 0; entry < 5; ++entry) {
    uint8_t major = 0, value_major = 0;
    uint64_t label = 0, value = 0;
    if (!head(in, &major, &label)) return false;
    unsigned slot = 0;
    if (major == 0 && label == 1) slot = 0;                                // kty
    else if (major == 0 && label == 3) slot = 1;                           // alg
    else if (major == 1 && label <= 2) slot = 2 + static_cast<unsigned>(label);  // crv, x, y
    else return false;
    if (seen & (1u << slot)) return false;
    seen |= 1u << slot;
    if (slot == 3 || slot == 4) {
      const uint8_t* coordinate = nullptr;
      size_t length = 0;
      if (!string(in, 2, 32, &coordinate, &length) || length != 32) return false;
      (slot == 3 ? out->x : out->y) = coordinate;
      continue;
    }
    if (!head(in, &value_major, &value)) return false;
    if (slot == 0 && !(value_major == 0 && value == 2)) return false;  // kty EC2
    if (slot == 1 && !(value_major == 1 && value == 6)) return false;  // alg ES256 (-7)
    if (slot == 2 && !(value_major == 0 && value == 1)) return false;  // crv P-256
  }
  return seen == 0x1f;
}

// The authenticator extensions map: apple_validation_category_01 (a 4-byte
// little-endian UInt32 byte string, as Apple's sample object encodes it)
// and apple_bundle_version_01 (text) exactly once each; at most eight
// entries; unrecognised text-keyed entries are skipped within the bounds.
AppleAttestStatus parse_extensions(Cbor* in, AuthData* out) {
  uint64_t count = 0;
  if (!container(in, 5, &count)) {
    fortis::base::LogWarn("auth", "apple-app-attest extensions refused: MapHeader Remaining=%zu",
            in->remaining());
    return AppleAttestStatus::ExtensionsMalformed;
  }
  if (count < 2 || count > 8) {
    fortis::base::LogWarn("auth", "apple-app-attest extensions refused: MapCount=%llu",
            static_cast<unsigned long long>(count));
    return AppleAttestStatus::ExtensionsMalformed;
  }
  bool have_category = false, have_version = false;
  for (uint64_t entry = 0; entry < count; ++entry) {
    const uint8_t* key = nullptr;
    size_t key_length = 0;
    if (!string(in, 3, 64, &key, &key_length)) {
      fortis::base::LogWarn("auth", "apple-app-attest extensions refused: KeyShape");
      return AppleAttestStatus::ExtensionsMalformed;
    }
    if (text_equals(key, key_length, "apple_validation_category_01")) {
      const uint8_t* bytes = nullptr;
      size_t length = 0;
      if (have_category || !string(in, 2, 4, &bytes, &length) || length != 4) {
        fortis::base::LogWarn("auth", "apple-app-attest extensions refused: CategoryShape");
        return AppleAttestStatus::ExtensionsMalformed;
      }
      have_category = true;
      out->validation_category = static_cast<uint32_t>(bytes[0]) |
          (static_cast<uint32_t>(bytes[1]) << 8) | (static_cast<uint32_t>(bytes[2]) << 16) |
          (static_cast<uint32_t>(bytes[3]) << 24);
    } else if (text_equals(key, key_length, "apple_bundle_version_01")) {
      if (have_version || !string(in, 3, kAppleBundleVersionMaxBytes, &out->bundle_version,
                                  &out->bundle_version_length) ||
          !out->bundle_version_length) {
        fortis::base::LogWarn("auth", "apple-app-attest extensions refused: VersionShape");
        return AppleAttestStatus::ExtensionsMalformed;
      }
      have_version = true;
    } else if (!skip(in, 1)) {
      fortis::base::LogWarn("auth", "apple-app-attest extensions refused: UnknownValueShape");
      return AppleAttestStatus::ExtensionsMalformed;
    }
  }
  if (!have_category || !have_version) {
    fortis::base::LogWarn("auth", "apple-app-attest extensions refused: MissingCategory=%u MissingVersion=%u",
            !have_category, !have_version);
    return AppleAttestStatus::ExtensionsMalformed;
  }
  return AppleAttestStatus::Verified;
}

// rpIdHash(32) flags(1) counter(4, big-endian) aaguid(16) credentialIdLength
// (2) = 32, credentialId(32), COSE key, optional extensions, and nothing
// after them. Flags must be AT (0x40), optionally with ED (0x80): Apple sets
// only AT even when the extensions follow.
AppleAttestStatus parse_auth_data(const uint8_t* data, size_t length, AuthData* out) {
  if (length < 32 + 1 + 4 + 16 + 2 + 32) return AppleAttestStatus::AuthDataMalformed;
  out->rp_id_hash = data;
  const uint8_t flags = data[32];
  if ((flags & 0x7f) != 0x40) return AppleAttestStatus::AuthDataMalformed;
  out->counter = (static_cast<uint32_t>(data[33]) << 24) |
      (static_cast<uint32_t>(data[34]) << 16) | (static_cast<uint32_t>(data[35]) << 8) |
      data[36];
  out->aaguid = data + 37;
  if (data[53] != 0 || data[54] != 32) return AppleAttestStatus::AuthDataMalformed;
  out->credential_id = data + 55;
  Cbor in{data + 87, data + length};
  if (!parse_cose_key(&in, out)) return AppleAttestStatus::AuthDataMalformed;
  // Older App Attest objects end after the credential's COSE key. The iOS 27
  // launch-category and bundle-version map is only checked when it exists;
  // an incomplete or malformed map is still refused.
  if (in.remaining() == 0) {
    if (flags & 0x80) return AppleAttestStatus::ExtensionsMalformed;
    return AppleAttestStatus::Verified;
  }
  const AppleAttestStatus extensions = parse_extensions(&in, out);
  if (extensions != AppleAttestStatus::Verified) return extensions;
  out->has_extensions = true;
  return in.p == in.end ? AppleAttestStatus::Verified : AppleAttestStatus::AuthDataMalformed;
}

// ---- X.509 --------------------------------------------------------------

struct ExtensionScan {
  bool want_nonce = false;
  unsigned nonce_count = 0;
  bool nonce_ok = false;
  uint8_t nonce[32]{};
  bool unknown_critical = false;
};

// Called by Mbed TLS for every extension it does not parse itself. Records
// the nonce (whose value must be exactly SEQUENCE { [1] { OCTET STRING(32) } })
// and refuses unknown critical extensions; a non-zero return on a
// non-critical extension is ignored by the library, so failures are also
// recorded in the scan and checked after parsing.
int scan_extension(void* context, const mbedtls_x509_crt*, const mbedtls_x509_buf* oid,
                   int critical, const unsigned char* p, const unsigned char* end) {
  auto* scan = static_cast<ExtensionScan*>(context);
  if (scan->want_nonce && oid->len == sizeof(kNonceOid) &&
      memcmp(oid->p, kNonceOid, sizeof(kNonceOid)) == 0) {
    ++scan->nonce_count;
    static const uint8_t kShape[] = {0x30, 0x24, 0xa1, 0x22, 0x04, 0x20};
    if (end - p == 38 && memcmp(p, kShape, sizeof(kShape)) == 0) {
      memcpy(scan->nonce, p + 6, 32);
      scan->nonce_ok = true;
    }
    return 0;
  }
  if (critical) {
    scan->unknown_critical = true;
    return MBEDTLS_ERR_X509_INVALID_EXTENSIONS;
  }
  return 0;
}

bool parse_certificate(mbedtls_x509_crt* chain, const uint8_t* der, size_t length,
                       ExtensionScan* scan) {
  if (mbedtls_x509_crt_parse_der_with_ext_cb(chain, der, length, 0, scan_extension, scan) != 0)
    return false;
  const mbedtls_x509_crt* last = chain;
  while (last->next) last = last->next;
  // The certificate must be the whole byte string, with nothing after it.
  return last->raw.len == length && !scan->unknown_critical && last->version == 3;
}

int64_t days_from_civil(int64_t year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 +
      day_of_year;
  return era * 146097 + static_cast<int64_t>(day_of_era) - 719468;
}

int64_t epoch_seconds(const mbedtls_x509_time& time) {
  return days_from_civil(time.year, static_cast<unsigned>(time.mon),
                         static_cast<unsigned>(time.day)) * 86400 +
      time.hour * 3600 + time.min * 60 + time.sec;
}

// notBefore <= now <= notAfter, at second precision (RFC 5280 4.1.2.5).
bool valid_at(const mbedtls_x509_crt& certificate, int64_t now_ms) {
  const int64_t now = now_ms / 1000;
  return epoch_seconds(certificate.valid_from) <= now &&
      now <= epoch_seconds(certificate.valid_to);
}

struct ChainDepth {
  int deepest = -1;
};
int note_depth(void* context, mbedtls_x509_crt*, int depth, uint32_t*) {
  auto* chain = static_cast<ChainDepth*>(context);
  if (depth > chain->deepest) chain->deepest = depth;
  return 0;
}

const mbedtls_x509_crt_profile kAppleProfile = {
    MBEDTLS_X509_ID_FLAG(MBEDTLS_MD_SHA256) | MBEDTLS_X509_ID_FLAG(MBEDTLS_MD_SHA384),
    MBEDTLS_X509_ID_FLAG(MBEDTLS_PK_ECDSA) | MBEDTLS_X509_ID_FLAG(MBEDTLS_PK_ECKEY),
    MBEDTLS_X509_ID_FLAG(MBEDTLS_ECP_DP_SECP256R1) | MBEDTLS_X509_ID_FLAG(MBEDTLS_ECP_DP_SECP384R1),
    0x7fffffff,  // no RSA key is acceptable
};

// Heap-held so the parsed chain stays off the worker stack.
struct Certificates {
  mbedtls_x509_crt chain;  // leaf, then intermediate
  mbedtls_x509_crt root;
  Certificates() noexcept {
    mbedtls_x509_crt_init(&chain);
    mbedtls_x509_crt_init(&root);
  }
  ~Certificates() {
    mbedtls_x509_crt_free(&chain);
    mbedtls_x509_crt_free(&root);
  }
  Certificates(const Certificates&) = delete;
  Certificates& operator=(const Certificates&) = delete;
};

AppleAttestStatus verify_certificates(const AppleAttestPolicy& policy,
                                      const Attestation& attestation, int64_t now_ms,
                                      uint8_t nonce[32], uint8_t point[65]) {
  auto certificates = base::MakeUnique<Certificates>();
  if (!certificates) return AppleAttestStatus::Internal;
  AppleAttestStatus status = AppleAttestStatus::Verified;
  ExtensionScan leaf_scan, intermediate_scan;
  leaf_scan.want_nonce = true;
  mbedtls_x509_crt* leaf = &certificates->chain;
  if (mbedtls_x509_crt_parse_der(&certificates->root, policy.root_der,
                                 policy.root_der_length) != 0 ||
      certificates->root.next) {
    status = AppleAttestStatus::Internal;
  } else if (!parse_certificate(leaf, attestation.certificates[0],
                                attestation.certificate_lengths[0], &leaf_scan) ||
             !parse_certificate(leaf, attestation.certificates[1],
                                attestation.certificate_lengths[1], &intermediate_scan) ||
             !leaf->next || leaf->next->next) {
    status = AppleAttestStatus::CertificateMalformed;
  } else if (leaf_scan.nonce_count != 1 || !leaf_scan.nonce_ok ||
             intermediate_scan.nonce_count != 0 || leaf->pk_raw.len != 91 ||
             memcmp(leaf->pk_raw.p, kP256SpkiPrefix, sizeof(kP256SpkiPrefix)) != 0 ||
             leaf->pk_raw.p[26] != 0x04) {
    status = AppleAttestStatus::CertificateMalformed;
  } else {
    const mbedtls_x509_crt* intermediate = leaf->next;
    // Leaf: an end entity for digital signatures with the App Attest EKU.
    // Intermediate: a CA restricted to signing end entities (pathlen 0,
    // which Mbed TLS stores as 1), not self-issued.
    if (mbedtls_x509_crt_get_ca_istrue(leaf) != 0 ||
        mbedtls_x509_crt_check_key_usage(leaf, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0 ||
        !mbedtls_x509_crt_has_ext_type(leaf, MBEDTLS_X509_EXT_KEY_USAGE) ||
        !mbedtls_x509_crt_has_ext_type(leaf, MBEDTLS_X509_EXT_EXTENDED_KEY_USAGE) ||
        mbedtls_x509_crt_check_extended_key_usage(leaf, kAppAttestEku,
                                                  sizeof(kAppAttestEku) - 1) != 0 ||
        mbedtls_x509_crt_get_ca_istrue(intermediate) != 1 ||
        intermediate->MBEDTLS_PRIVATE(max_pathlen) != 1 ||
        !mbedtls_x509_crt_has_ext_type(intermediate, MBEDTLS_X509_EXT_KEY_USAGE) ||
        mbedtls_x509_crt_check_key_usage(intermediate, MBEDTLS_X509_KU_KEY_CERT_SIGN) != 0 ||
        (intermediate->subject_raw.len == intermediate->issuer_raw.len &&
         memcmp(intermediate->subject_raw.p, intermediate->issuer_raw.p,
                intermediate->subject_raw.len) == 0)) {
      status = AppleAttestStatus::CertificateUsage;
    } else {
      // Signatures, profile and the path to the pinned root. Mbed TLS checks
      // validity against the host clock; that verdict is replaced by the
      // explicit `now_ms` check below so a test clock governs it too.
      uint32_t flags = 0;
      ChainDepth depth;
      mbedtls_x509_crt_verify_with_profile(leaf, &certificates->root, nullptr, &kAppleProfile,
                                           nullptr, &flags, note_depth, &depth);
      flags &= ~static_cast<uint32_t>(MBEDTLS_X509_BADCERT_EXPIRED |
                                      MBEDTLS_X509_BADCERT_FUTURE);
      if (flags != 0 || depth.deepest != 2) {
        status = AppleAttestStatus::CertificateUntrusted;
      } else if (!valid_at(*leaf, now_ms) || !valid_at(*intermediate, now_ms) ||
                 !valid_at(certificates->root, now_ms)) {
        status = AppleAttestStatus::CertificateNotValidNow;
      } else {
        memcpy(nonce, leaf_scan.nonce, 32);
        memcpy(point, leaf->pk_raw.p + 26, 65);
      }
    }
  }
  certificates.reset();
  return status;
}

#ifdef FORTIS_APPLE_ATTEST_TEST_ROOT
bool load_test_root(uint8_t* output, size_t capacity, size_t* length) {
  const char* path = getenv(kAppleTestRootEnvironment);
  FILE* file = path && *path ? fopen(path, "rb") : nullptr;
  if (!file) return false;
  *length = fread(output, 1, capacity, file);
  const bool ok = *length > 0 && *length < capacity && feof(file);
  fclose(file);
  return ok;
}
#endif

std::atomic<int> g_verifications_in_flight{0};

}  // namespace

const char* AppleAttestStatusName(AppleAttestStatus status) {
  switch (status) {
    case AppleAttestStatus::Verified: return "Verified";
    case AppleAttestStatus::TokenMalformed: return "TokenMalformed";
    case AppleAttestStatus::AttestationMalformed: return "AttestationMalformed";
    case AppleAttestStatus::FormatUnsupported: return "FormatUnsupported";
    case AppleAttestStatus::AuthDataMalformed: return "AuthDataMalformed";
    case AppleAttestStatus::ExtensionsMalformed: return "ExtensionsMalformed";
    case AppleAttestStatus::CertificateMalformed: return "CertificateMalformed";
    case AppleAttestStatus::CertificateUntrusted: return "CertificateUntrusted";
    case AppleAttestStatus::CertificateNotValidNow: return "CertificateNotValidNow";
    case AppleAttestStatus::CertificateUsage: return "CertificateUsage";
    case AppleAttestStatus::NonceMismatch: return "NonceMismatch";
    case AppleAttestStatus::KeyIdMismatch: return "KeyIdMismatch";
    case AppleAttestStatus::CredentialIdMismatch: return "CredentialIdMismatch";
    case AppleAttestStatus::RpIdMismatch: return "RpIdMismatch";
    case AppleAttestStatus::CounterNotZero: return "CounterNotZero";
    case AppleAttestStatus::EnvironmentMismatch: return "EnvironmentMismatch";
    case AppleAttestStatus::CategoryRejected: return "CategoryRejected";
    case AppleAttestStatus::BundleVersionMismatch: return "BundleVersionMismatch";
    case AppleAttestStatus::Internal: return "Internal";
  }
  return "Unknown";
}

bool IsAppleBundleVersion(const char* text, size_t length) {
  if (!text || !length || length > kAppleBundleVersionMaxBytes) return false;
  unsigned components = 1;
  size_t digits = 0;
  for (size_t i = 0; i < length; ++i) {
    if (text[i] >= '0' && text[i] <= '9') {
      ++digits;
    } else if (text[i] == '.' && digits) {
      digits = 0;
      if (++components > 3) return false;
    } else {
      return false;
    }
  }
  return digits != 0;
}

AppleAttestStatus ParseAppleAttestToken(const char* token, size_t length, uint8_t* envelope,
                                        size_t capacity, AppleAttestToken* output) {
  size_t size = 0;
  if (!token || !envelope || !output || length > kAppleTokenMaxChars ||
      !decode_base64url(token, length, envelope, capacity, &size) || size < 1 + 32 + 1)
    return AppleAttestStatus::TokenMalformed;
  if (envelope[0] != 1) return AppleAttestStatus::TokenMalformed;
  *output = AppleAttestToken{};
  memcpy(output->key_id, envelope + 1, 32);
  const size_t version_length = envelope[33];
  if (!version_length || version_length > kAppleBundleVersionMaxBytes ||
      size < 34 + version_length + 4 ||
      !IsAppleBundleVersion(reinterpret_cast<const char*>(envelope + 34), version_length))
    return AppleAttestStatus::TokenMalformed;
  memcpy(output->bundle_version, envelope + 34, version_length);
  output->bundle_version_length = static_cast<uint8_t>(version_length);
  const uint8_t* length_field = envelope + 34 + version_length;
  const size_t attestation_length = (static_cast<size_t>(length_field[0]) << 24) |
      (static_cast<size_t>(length_field[1]) << 16) |
      (static_cast<size_t>(length_field[2]) << 8) | length_field[3];
  if (!attestation_length || attestation_length > kAppleAttestationMaxBytes ||
      size != 38 + version_length + attestation_length)
    return AppleAttestStatus::TokenMalformed;
  output->attestation = length_field + 4;
  output->attestation_length = attestation_length;
  return AppleAttestStatus::Verified;
}

uint32_t AppleAllowedCategories(AppleAttestEnvironment environment) {
  return environment == AppleAttestEnvironment::Production ? (1u << 2) | (1u << 4)
       : environment == AppleAttestEnvironment::Development ? (1u << 3) : 0;
}

AppleAttestStatus VerifyAppleAttestation(const AppleAttestPolicy& policy,
                                         const AppleAttestToken& token,
                                         const uint8_t* client_data_hash,
                                         size_t client_data_hash_length, int64_t now_ms,
                                         AppleAttestEvidence* output) {
  if (!output || !policy.app_id || !policy.app_id_length || !policy.root_der ||
      !policy.root_der_length || !client_data_hash || !client_data_hash_length ||
      client_data_hash_length > 64 || now_ms <= 0 || !token.attestation ||
      !token.attestation_length || token.attestation_length > kAppleAttestationMaxBytes ||
      !IsAppleBundleVersion(token.bundle_version, token.bundle_version_length))
    return AppleAttestStatus::Internal;
  Attestation attestation;
  AppleAttestStatus status =
      parse_attestation(token.attestation, token.attestation_length, &attestation);
  if (status != AppleAttestStatus::Verified) return status;
  AuthData auth;
  status = parse_auth_data(attestation.auth_data, attestation.auth_data_length, &auth);
  if (status != AppleAttestStatus::Verified) return status;
  uint8_t certificate_nonce[32], point[65];
  status = verify_certificates(policy, attestation, now_ms, certificate_nonce, point);
  if (status != AppleAttestStatus::Verified) return status;
  uint8_t nonce[32], key_hash[32], rp_id_hash[32];
  if (!sha256(attestation.auth_data, attestation.auth_data_length, client_data_hash,
              client_data_hash_length, nonce) ||
      !sha256(point, sizeof(point), nullptr, 0, key_hash) ||
      !sha256(reinterpret_cast<const uint8_t*>(policy.app_id), policy.app_id_length,
              nullptr, 0, rp_id_hash))
    return AppleAttestStatus::Internal;
  if (!same(nonce, certificate_nonce, 32)) return AppleAttestStatus::NonceMismatch;
  if (!same(key_hash, token.key_id, 32)) return AppleAttestStatus::KeyIdMismatch;
  if (!same(auth.credential_id, token.key_id, 32) || !same(auth.x, point + 1, 32) ||
      !same(auth.y, point + 33, 32))
    return AppleAttestStatus::CredentialIdMismatch;
  if (!same(auth.rp_id_hash, rp_id_hash, 32)) return AppleAttestStatus::RpIdMismatch;
  if (auth.counter != 0) return AppleAttestStatus::CounterNotZero;
  const uint8_t* aaguid = policy.environment == AppleAttestEnvironment::Production
      ? kAaguidProduction : kAaguidDevelopment;
  if (!same(auth.aaguid, aaguid, 16)) return AppleAttestStatus::EnvironmentMismatch;
  if (auth.has_extensions) {
    if (auth.validation_category >= 32 ||
        !(policy.allowed_categories & (1u << auth.validation_category)))
      return AppleAttestStatus::CategoryRejected;
    if (auth.bundle_version_length != token.bundle_version_length ||
        memcmp(auth.bundle_version, token.bundle_version, token.bundle_version_length) != 0)
      return AppleAttestStatus::BundleVersionMismatch;
  }
  *output = AppleAttestEvidence{};
  memcpy(output->key_id, token.key_id, 32);
  memcpy(output->public_key, point, 65);
  output->environment = policy.environment;
  // Zero/empty explicitly means the legacy object did not attest these two
  // launch signals. Never persist the unsigned envelope build as verified.
  if (auth.has_extensions) {
    output->validation_category = auth.validation_category;
    memcpy(output->bundle_version, token.bundle_version, token.bundle_version_length);
    output->bundle_version_length = token.bundle_version_length;
  }
  output->receipt = attestation.receipt;
  output->receipt_length = static_cast<uint32_t>(attestation.receipt_length);
  return AppleAttestStatus::Verified;
}

bool TryAcquireAppleVerifySlot() {
  int current = (g_verifications_in_flight).load(std::memory_order_acquire);
  while (current < kAppleVerifyConcurrency) {
    if ((g_verifications_in_flight).compare_exchange_strong(current, current + 1, std::memory_order_acq_rel, std::memory_order_acquire))
      return true;
  }
  return false;
}

void ReleaseAppleVerifySlot() {
  (g_verifications_in_flight).fetch_sub(1, std::memory_order_acq_rel);
}

AppleEnrollOutcome EnrollWithAppleAppAttest(DeviceAuthStore& auth,
                                            AppleAttestEnvironment environment,
                                            const char* challenge, size_t challenge_length,
                                            const char* token, size_t token_length,
                                            const char* app_version, size_t app_version_length,
                                            int64_t now_ms, int64_t ttl_ms,
                                            DeviceCredentials* output) {
  AppleEnrollOutcome outcome;
  if (!output) return outcome;
  // Only a server-minted challenge (43 base64url characters) can be live.
  bool minted = challenge && challenge_length == 43;
  for (size_t i = 0; minted && i < challenge_length; ++i)
    minted = strchr(kBase64Url, challenge[i]) != nullptr && challenge[i] != '\0';
  if (!minted) {
    outcome.status = AppleEnrollStatus::ChallengeUnavailable;
    return outcome;
  }
  if (!app_version || !app_version_length || app_version_length > 200 || !token ||
      !token_length || token_length > kAppleTokenMaxChars) {
    outcome.status = AppleEnrollStatus::InvalidProof;
    outcome.verification = AppleAttestStatus::TokenMalformed;
    return outcome;
  }
  // A read-only liveness check spares verification for dead challenges; the
  // consuming check is the conditional frame in EnrollAttested.
  switch (auth.PeekChallenge(challenge, challenge_length, now_ms)) {
    case storage::KvReadStatus::Found: break;
    case storage::KvReadStatus::Missing: outcome.status = AppleEnrollStatus::ChallengeUnavailable;
      return outcome;
    default: outcome.status = AppleEnrollStatus::StorageError; return outcome;
  }
  if (!TryAcquireAppleVerifySlot()) {
    outcome.status = AppleEnrollStatus::Busy;
    return outcome;
  }
  auto release_slot = [](AppleEnrollOutcome*) noexcept { ReleaseAppleVerifySlot(); };
  std::unique_ptr<AppleEnrollOutcome, decltype(release_slot)> slot(&outcome, release_slot);
  auto envelope_storage = base::AllocateStorage<uint8_t[]>(kAppleEnvelopeMaxBytes);
  auto* envelope = envelope_storage.get();
  AppleAttestToken parsed;
  AppleAttestEvidence evidence;
  outcome.verification = envelope
      ? ParseAppleAttestToken(token, token_length, envelope, kAppleEnvelopeMaxBytes, &parsed)
      : AppleAttestStatus::Internal;
  if (outcome.verification == AppleAttestStatus::Verified) {
    AppleAttestPolicy policy;
    policy.environment = environment;
    policy.allowed_categories = AppleAllowedCategories(environment);
#ifdef FORTIS_APPLE_ATTEST_TEST_ROOT
    uint8_t test_root[kAppleCertificateMaxBytes];
    size_t test_root_length = 0;
    const bool root_ok = load_test_root(test_root, sizeof(test_root), &test_root_length);
    policy.root_der = test_root;
    policy.root_der_length = test_root_length;
#else
    uint8_t fingerprint[32];
    const bool root_ok = sha256(kAppleAppAttestRootDer, kAppleAppAttestRootDerLength, nullptr,
                                0, fingerprint) &&
        same(fingerprint, kAppleAppAttestRootSha256, 32);
    policy.root_der = kAppleAppAttestRootDer;
    policy.root_der_length = kAppleAppAttestRootDerLength;
#endif
    uint8_t client_data_hash[32];
    outcome.verification =
        !root_ok || !sha256(reinterpret_cast<const uint8_t*>(challenge), challenge_length,
                            nullptr, 0, client_data_hash)
            ? AppleAttestStatus::Internal
            : VerifyAppleAttestation(policy, parsed, client_data_hash,
                                     sizeof(client_data_hash), now_ms, &evidence);
  }
  slot.reset();
  if (outcome.verification != AppleAttestStatus::Verified) {
    envelope_storage.reset();
    outcome.status = outcome.verification == AppleAttestStatus::Internal
        ? AppleEnrollStatus::StorageError : AppleEnrollStatus::InvalidProof;
    return outcome;
  }
  char minted_challenge[44];
  memcpy(minted_challenge, challenge, 43);
  minted_challenge[43] = '\0';
  const VerifiedDeviceAttestation proof = DeviceAttestationIssuer::IssueApple(
      minted_challenge, app_version, app_version_length, evidence, now_ms);
  const DeviceEnrollResult enrolled = auth.EnrollAttested(
      proof, DevicePlatform::Ios, app_version, app_version_length, DeviceQuotaTier::Attested,
      now_ms, ttl_ms, output);
  envelope_storage.reset();  // the proof's receipt pointed into it
  switch (enrolled) {
    case DeviceEnrollResult::Enrolled: outcome.status = AppleEnrollStatus::Enrolled; break;
    case DeviceEnrollResult::ChallengeUnavailable:
    case DeviceEnrollResult::ChallengeMismatch:
      outcome.status = AppleEnrollStatus::ChallengeUnavailable; break;
    case DeviceEnrollResult::KeyAlreadyAssociated:
      outcome.status = AppleEnrollStatus::KeyAlreadyAssociated; break;
    case DeviceEnrollResult::Rejected: outcome.status = AppleEnrollStatus::Rejected; break;
    default: outcome.status = AppleEnrollStatus::StorageError; break;
  }
  return outcome;
}

}  // namespace fortis::auth
