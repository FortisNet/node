#include "test_support.h"
// The Apple App Attest verifier and its
// required-mode enrollment frame.
//
// Built twice. build/apple_app_attest_test links the verifier compiled with
// FORTIS_APPLE_ATTEST_TEST_ROOT, so EnrollWithAppleAppAttest trusts a
// synthetic root generated here, and runs every group.
// build/apple_app_attest_release_test links the release object and checks
// that the same synthetic proofs are refused there whatever the environment
// says, plus every group that does not need the test root (envelope, the
// synthetic matrix through the verifier core).
#include "../src/auth/apple_app_attest.h"
#include "../src/auth/device_auth.h"
#include "../src/auth/device_challenge.h"
#include "../src/storage/kv_store.h"
#include "apple_app_attest_fixture.h"

#include <atomic>
#include <functional>
#include <signal.h>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using apple_fixture::Authority;
using apple_fixture::Bytes;
using apple_fixture::Minted;
using apple_fixture::MintOptions;
using apple_fixture::Random;
using fortis::auth::AppleAttestEnvironment;
using fortis::auth::AppleAttestEvidence;
using fortis::auth::AppleAttestPolicy;
using fortis::auth::AppleAttestStatus;
using fortis::auth::AppleAttestToken;
using fortis::auth::AppleEnrollOutcome;
using fortis::auth::AppleEnrollStatus;
using fortis::auth::AppleKeyAssociation;
using fortis::auth::DeviceAttestation;
using fortis::auth::DeviceAuthResult;
using fortis::auth::DeviceAuthStore;
using fortis::auth::DeviceCredentials;
using fortis::auth::DevicePlatform;
using fortis::auth::DeviceQuotaTier;
using fortis::auth::DeviceRecord;
using fortis::storage::KvReadStatus;
using fortis::storage::KvStore;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
      abort();                                                                  \
    }                                                                           \
  } while (0)

#define CHECK_STATUS(actual, expected)                                          \
  do {                                                                          \
    const AppleAttestStatus got_ = (actual);                                    \
    if (got_ != (expected)) {                                                   \
      fprintf(stderr, "%s:%d: status %s, expected %s\n", __FILE__, __LINE__,     \
              fortis::auth::AppleAttestStatusName(got_),                              \
              fortis::auth::AppleAttestStatusName(expected));                         \
      abort();                                                                  \
    }                                                                           \
  } while (0)

constexpr int64_t kTtl = 86400000;
constexpr const char* kVersion = "1.4.2";

int64_t WallMs() {
  struct timespec now {};
  clock_gettime(CLOCK_REALTIME, &now);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

// Synthetic proofs are minted around the wall clock so the release binary,
// which cannot be given another clock, sees them as currently valid.
const int64_t kNowMs = WallMs();
const int64_t kNowSeconds = kNowMs / 1000;

std::string MakeDirectory(const char* name) {
  char path[256];
  snprintf(path, sizeof(path), "/tmp/fortis-apple-%s-XXXXXX", name);
  CHECK(mkdtemp(path));
  return path;
}

void RemoveDirectory(const std::string& directory) {
  for (const char* file : {"/fortis.kv", "/fortis.kv.lock", "/fortis.kv.compact", "/root.der"})
    unlink((directory + file).c_str());
  CHECK(rmdir(directory.c_str()) == 0);
}

std::string Challenge(unsigned seed) {
  char text[44];
  snprintf(text, sizeof(text), "A%042u", seed);
  return text;
}

bool CountVisit(const char*, const void*, size_t, void* context) {
  ++*static_cast<size_t*>(context);
  return true;
}

size_t CountPrefix(KvStore& store, const char* prefix, int64_t now) {
  size_t count = 0;
  CHECK(store.Scan(prefix, nullptr, SIZE_MAX, now, [&, visitor_state = &count](const char* key, const void* value, size_t size) { return (CountVisit)(key, value, size, visitor_state); }));
  return count;
}

Random& Rng() {
  static Random random;
  return random;
}

Authority& TestAuthority() {
  static Authority* authority = [] {
    auto* created = new Authority();
    created->Create(Rng(), kNowSeconds);
    return created;
  }();
  return *authority;
}

AppleAttestPolicy SyntheticPolicy(AppleAttestEnvironment environment =
                                      AppleAttestEnvironment::Production) {
  AppleAttestPolicy policy;
  policy.environment = environment;
  policy.allowed_categories = fortis::auth::AppleAllowedCategories(environment);
  policy.root_der = TestAuthority().root_der.data();
  policy.root_der_length = TestAuthority().root_der.size();
  return policy;
}

MintOptions Options(const std::string& challenge) {
  MintOptions options;
  options.challenge = challenge;
  options.now_seconds = kNowSeconds;
  return options;
}

// Parse + verify a minted token through the core with production binding.
AppleAttestStatus VerifyMinted(const Minted& minted, const std::string& challenge,
                               const AppleAttestPolicy& policy, int64_t now_ms = kNowMs,
                               AppleAttestEvidence* evidence_out = nullptr) {
  std::vector<uint8_t> envelope(fortis::auth::kAppleEnvelopeMaxBytes);
  AppleAttestToken token;
  const AppleAttestStatus parsed = fortis::auth::ParseAppleAttestToken(
      minted.token.data(), minted.token.size(), envelope.data(), envelope.size(), &token);
  if (parsed != AppleAttestStatus::Verified) return parsed;
  const Bytes hash = apple_fixture::Sha256(apple_fixture::FromString(challenge));
  AppleAttestEvidence evidence;
  const AppleAttestStatus status = fortis::auth::VerifyAppleAttestation(
      policy, token, hash.data(), hash.size(), now_ms, &evidence);
  if (evidence_out) *evidence_out = evidence;
  return status;
}

AppleEnrollOutcome Enroll(DeviceAuthStore& auth, const std::string& challenge,
                          const std::string& token, int64_t now_ms,
                          DeviceCredentials* credentials,
                          AppleAttestEnvironment environment = AppleAttestEnvironment::Production,
                          const char* app_version = kVersion) {
  return fortis::auth::EnrollWithAppleAppAttest(
      auth, environment, challenge.data(), challenge.size(), token.data(), token.size(),
      app_version, app_version ? strlen(app_version) : 0, now_ms, kTtl, credentials);
}

// ---- Envelope ---------------------------------------------------------------

void TestEnvelope() {
  const Bytes key(32, 0x11);
  const Bytes attestation = {0xa0};
  std::vector<uint8_t> buffer(fortis::auth::kAppleEnvelopeMaxBytes);
  AppleAttestToken token;
  auto parse = [&](const std::string& text) {
    return fortis::auth::ParseAppleAttestToken(text.data(), text.size(), buffer.data(), buffer.size(),
                                         &token);
  };
  const Bytes valid = apple_fixture::Envelope(1, key, "57", attestation);
  const std::string text = apple_fixture::Base64Url(valid);
  CHECK_STATUS(parse(text), AppleAttestStatus::Verified);
  CHECK(token.bundle_version_length == 2 && strcmp(token.bundle_version, "57") == 0);
  CHECK(token.attestation_length == 1 && token.attestation[0] == 0xa0);
  CHECK(memcmp(token.key_id, key.data(), 32) == 0);
  // Encoding: padding, standard alphabet, whitespace, a length of 1 mod 4,
  // non-zero unused bits, empty and oversized strings.
  CHECK_STATUS(parse(text + "="), AppleAttestStatus::TokenMalformed);
  CHECK_STATUS(parse(text + "=="), AppleAttestStatus::TokenMalformed);
  std::string plus = text;
  plus[3] = '+';
  CHECK_STATUS(parse(plus), AppleAttestStatus::TokenMalformed);
  CHECK_STATUS(parse(" " + text), AppleAttestStatus::TokenMalformed);
  CHECK_STATUS(parse(text + "\n"), AppleAttestStatus::TokenMalformed);
  CHECK(text.size() == 55);  // 41 envelope bytes
  CHECK_STATUS(parse(text + "A"), AppleAttestStatus::TokenMalformed);   // a 42nd byte
  CHECK_STATUS(parse(text + "AA"), AppleAttestStatus::TokenMalformed);  // 1 mod 4
  std::string noisy = text;
  const int last = noisy.back() >= 'A' && noisy.back() <= 'Z' ? noisy.back() - 'A' : -1;
  CHECK(last >= 0);
  noisy.back() = static_cast<char>('A' + (last ^ 1));  // flips an unused bit
  CHECK_STATUS(parse(noisy), AppleAttestStatus::TokenMalformed);
  CHECK_STATUS(parse(""), AppleAttestStatus::TokenMalformed);
  CHECK_STATUS(parse(std::string(fortis::auth::kAppleTokenMaxChars + 1, 'A')),
               AppleAttestStatus::TokenMalformed);
  // Envelope fields.
  CHECK_STATUS(parse(apple_fixture::Base64Url(apple_fixture::Envelope(2, key, "57", attestation))),
               AppleAttestStatus::TokenMalformed);
  CHECK_STATUS(parse(apple_fixture::Base64Url(apple_fixture::Envelope(0, key, "57", attestation))),
               AppleAttestStatus::TokenMalformed);
  for (const char* bundle : {"", "a", ".1", "1.", "1..2", "1.2.3.4", "1-2", " 1",
                             "123456789012345678901234567890123"})
    CHECK_STATUS(parse(apple_fixture::Base64Url(apple_fixture::Envelope(1, key, bundle,
                                                                        attestation))),
                 AppleAttestStatus::TokenMalformed);
  for (const char* bundle : {"1", "1.0", "2026.10.28", "045", "12345678901234567890123456789012"})
    CHECK_STATUS(parse(apple_fixture::Base64Url(apple_fixture::Envelope(1, key, bundle,
                                                                        attestation))),
                 AppleAttestStatus::Verified);
  Bytes trailing = valid;
  trailing.push_back(0);
  CHECK_STATUS(parse(apple_fixture::Base64Url(trailing)), AppleAttestStatus::TokenMalformed);
  Bytes truncated = valid;
  truncated.pop_back();
  CHECK_STATUS(parse(apple_fixture::Base64Url(truncated)), AppleAttestStatus::TokenMalformed);
  CHECK_STATUS(parse(apple_fixture::Base64Url(Bytes(valid.begin(), valid.begin() + 20))),
               AppleAttestStatus::TokenMalformed);
  Bytes zero_length = apple_fixture::Envelope(1, key, "57", Bytes());
  CHECK_STATUS(parse(apple_fixture::Base64Url(zero_length)), AppleAttestStatus::TokenMalformed);
  const Bytes largest(fortis::auth::kAppleAttestationMaxBytes, 0xa0);
  const std::string largest_text =
      apple_fixture::Base64Url(apple_fixture::Envelope(1, key, std::string(32, '9'), largest));
  CHECK(largest_text.size() == fortis::auth::kAppleTokenMaxChars);
  CHECK_STATUS(parse(largest_text), AppleAttestStatus::Verified);
  const Bytes too_large(fortis::auth::kAppleAttestationMaxBytes + 1, 0xa0);
  CHECK_STATUS(parse(apple_fixture::Base64Url(apple_fixture::Envelope(1, key, "57", too_large))),
               AppleAttestStatus::TokenMalformed);
  // A declared length beyond the bytes present.
  Bytes lying = valid;
  lying[34 + 2 + 3] = 2;
  CHECK_STATUS(parse(apple_fixture::Base64Url(lying)), AppleAttestStatus::TokenMalformed);
  lying[34 + 2] = 0xff;
  CHECK_STATUS(parse(apple_fixture::Base64Url(lying)), AppleAttestStatus::TokenMalformed);
  // The decode buffer bound is enforced, not assumed.
  CHECK(fortis::auth::ParseAppleAttestToken(text.data(), text.size(), buffer.data(), 10, &token) ==
        AppleAttestStatus::TokenMalformed);
  puts("envelope: canonical base64url, version, fields, bounds and trailing bytes: ok");
}

// ---- Synthetic adversarial matrix (verifier core) ---------------------------

struct Case {
  const char* name;
  std::function<void(MintOptions&)> change;
  AppleAttestStatus expected;
  AppleAttestEnvironment environment = AppleAttestEnvironment::Production;
};

std::vector<Case> Matrix() {
  using S = AppleAttestStatus;
  using E = AppleAttestEnvironment;
  return {
      {"valid", [](MintOptions&) {}, S::Verified},
      {"testflight category", [](MintOptions& o) { o.category = 2; }, S::Verified},
      {"ED flag also set", [](MintOptions& o) { o.flags = 0xc0; }, S::Verified},
      {"keys reordered", [](MintOptions& o) { o.reorder = true; }, S::Verified},
      {"unknown extension entry", [](MintOptions& o) { o.extension_mode = 5; }, S::Verified},
      {"three-part build", [](MintOptions& o) { o.bundle_version = "3.10.200"; }, S::Verified},
      {"development key and category", [](MintOptions& o) {
         o.development = true; o.category = 3; }, S::Verified, E::Development},
      {"largest receipt", [](MintOptions& o) { o.receipt_size = 8192; }, S::Verified},
      // CBOR syntax, shape and consumption.
      {"fmt", [](MintOptions& o) { o.fmt = "packed"; }, S::FormatUnsupported},
      {"fmt empty", [](MintOptions& o) { o.fmt = ""; }, S::FormatUnsupported},
      {"top-level trailing byte", [](MintOptions& o) { o.top_trailing_byte = true; },
       S::AttestationMalformed},
      {"top-level extra key", [](MintOptions& o) { o.top_extra_key = true; },
       S::AttestationMalformed},
      {"top-level duplicate key", [](MintOptions& o) { o.top_duplicate_key = true; },
       S::AttestationMalformed},
      {"non-shortest head", [](MintOptions& o) { o.long_head = true; }, S::AttestationMalformed},
      {"indefinite receipt", [](MintOptions& o) { o.indefinite_receipt = true; },
       S::AttestationMalformed},
      {"tagged receipt", [](MintOptions& o) { o.tagged_receipt = true; },
       S::AttestationMalformed},
      {"receipt too large", [](MintOptions& o) { o.receipt_size = 8193; },
       S::AttestationMalformed},
      {"empty receipt", [](MintOptions& o) { o.receipt_size = 0; }, S::AttestationMalformed},
      {"one certificate", [](MintOptions& o) { o.x5c_count = 1; }, S::AttestationMalformed},
      {"three certificates", [](MintOptions& o) { o.x5c_count = 3; }, S::AttestationMalformed},
      {"certificate too large", [](MintOptions& o) { o.leaf_padding = 3800; },
       S::AttestationMalformed},
      // Authenticator data.
      {"authData trailing byte", [](MintOptions& o) { o.authdata_trailing_byte = true; },
       S::AuthDataMalformed},
      {"flags UP", [](MintOptions& o) { o.flags = 0x41; }, S::AuthDataMalformed},
      {"flags without AT", [](MintOptions& o) { o.flags = 0x80; }, S::AuthDataMalformed},
      {"credential id length 31", [](MintOptions& o) { o.credential_id_length = 31; },
       S::AuthDataMalformed},
      {"COSE extra label", [](MintOptions& o) { o.cose_mode = 1; }, S::AuthDataMalformed},
      {"COSE missing y", [](MintOptions& o) { o.cose_mode = 2; }, S::AuthDataMalformed},
      {"COSE alg ES512", [](MintOptions& o) { o.cose_mode = 3; }, S::AuthDataMalformed},
      {"COSE curve P-384", [](MintOptions& o) { o.cose_mode = 4; }, S::AuthDataMalformed},
      {"COSE short x", [](MintOptions& o) { o.cose_mode = 5; }, S::AuthDataMalformed},
      {"legacy without extensions", [](MintOptions& o) { o.extension_mode = 10; }, S::Verified},
      {"legacy with ED flag", [](MintOptions& o) {
         o.extension_mode = 10; o.flags = 0xc0; }, S::ExtensionsMalformed},
      {"legacy trailing byte", [](MintOptions& o) {
         o.extension_mode = 10; o.authdata_trailing_byte = true; }, S::ExtensionsMalformed},
      {"missing category", [](MintOptions& o) { o.extension_mode = 1; }, S::ExtensionsMalformed},
      {"missing bundle version", [](MintOptions& o) { o.extension_mode = 2; },
       S::ExtensionsMalformed},
      {"duplicate category", [](MintOptions& o) { o.extension_mode = 3; },
       S::ExtensionsMalformed},
      {"category as integer", [](MintOptions& o) { o.extension_mode = 4; },
       S::ExtensionsMalformed},
      {"category three bytes", [](MintOptions& o) { o.extension_mode = 9; },
       S::ExtensionsMalformed},
      {"bundle version as bytes", [](MintOptions& o) { o.extension_mode = 8; },
       S::ExtensionsMalformed},
      {"unknown entry too deep", [](MintOptions& o) { o.extension_mode = 6; },
       S::ExtensionsMalformed},
      {"nine extension entries", [](MintOptions& o) { o.extension_mode = 7; },
       S::ExtensionsMalformed},
      // Certificates.
      {"certificate trailing byte", [](MintOptions& o) { o.certificate_trailing_byte = true; },
       S::CertificateMalformed},
      {"x5c reversed", [](MintOptions& o) { o.swap_x5c = true; }, S::CertificateMalformed},
      {"no nonce extension", [](MintOptions& o) { o.nonce_mode = 1; }, S::CertificateMalformed},
      {"nonce wrong shape", [](MintOptions& o) { o.nonce_mode = 3; }, S::CertificateMalformed},
      {"unknown critical extension", [](MintOptions& o) { o.unknown_critical_extension = true; },
       S::CertificateMalformed},
      {"P-384 leaf key", [](MintOptions& o) { o.leaf_p384 = true; }, S::CertificateMalformed},
      {"leaf is a CA", [](MintOptions& o) { o.leaf_is_ca = true; }, S::CertificateUsage},
      {"leaf without EKU", [](MintOptions& o) { o.leaf_without_eku = true; },
       S::CertificateUsage},
      {"leaf with server-auth EKU", [](MintOptions& o) { o.leaf_wrong_eku = true; },
       S::CertificateUsage},
      {"leaf without digitalSignature", [](MintOptions& o) {
         o.leaf_without_digital_signature = true; }, S::CertificateUsage},
      {"intermediate without pathlen", [](MintOptions& o) {
         o.intermediate_without_path_length = true; }, S::CertificateUsage},
      {"intermediate not a CA", [](MintOptions& o) { o.intermediate_not_ca = true; },
       S::CertificateUsage},
      {"leaf signed by the root", [](MintOptions& o) { o.leaf_signed_by_root = true; },
       S::CertificateUsage},
      {"intermediate from another root", [](MintOptions& o) {
         o.intermediate_from_other_root = true; }, S::CertificateUntrusted},
      {"leaf not yet valid", [](MintOptions& o) {
         o.leaf_not_before = kNowSeconds + 60; o.leaf_not_after = kNowSeconds + 86400; },
       S::CertificateNotValidNow},
      {"leaf expired", [](MintOptions& o) {
         o.leaf_not_before = kNowSeconds - 86400; o.leaf_not_after = kNowSeconds - 60; },
       S::CertificateNotValidNow},
      // Binding.
      {"nonce value", [](MintOptions& o) { o.nonce_mode = 2; }, S::NonceMismatch},
      {"nonce from raw challenge", [](MintOptions& o) { o.raw_client_data = true; },
       S::NonceMismatch},
      {"envelope key id", [](MintOptions& o) { o.wrong_envelope_key_id = true; },
       S::KeyIdMismatch},
      {"credential id", [](MintOptions& o) { o.wrong_credential_id = true; },
       S::CredentialIdMismatch},
      {"COSE key not the certificate key", [](MintOptions& o) { o.cose_other_key = true; },
       S::CredentialIdMismatch},
      {"RP ID of another app", [](MintOptions& o) { o.app_id = "C7Z98WBK8H.io.fortisnet.other"; },
       S::RpIdMismatch},
      {"RP ID without team", [](MintOptions& o) { o.app_id = "io.fortisnet.wallet"; },
       S::RpIdMismatch},
      {"counter 1", [](MintOptions& o) { o.counter = 1; }, S::CounterNotZero},
      {"development key in production", [](MintOptions& o) { o.development = true; },
       S::EnvironmentMismatch},
      {"production key in development", [](MintOptions& o) { o.category = 3; },
       S::EnvironmentMismatch, E::Development},
      {"category 0", [](MintOptions& o) { o.category = 0; }, S::CategoryRejected},
      {"category 1 (OS)", [](MintOptions& o) { o.category = 1; }, S::CategoryRejected},
      {"category 3 in production", [](MintOptions& o) { o.category = 3; }, S::CategoryRejected},
      {"category 5 (enterprise/ad hoc)", [](MintOptions& o) { o.category = 5; },
       S::CategoryRejected},
      {"category 6 (Developer ID)", [](MintOptions& o) { o.category = 6; }, S::CategoryRejected},
      {"category 10", [](MintOptions& o) { o.category = 10; }, S::CategoryRejected},
      {"category 2^32-1", [](MintOptions& o) { o.category = 0xffffffffu; }, S::CategoryRejected},
      {"category 4 in development", [](MintOptions& o) {
         o.development = true; o.category = 4; }, S::CategoryRejected, E::Development},
      {"bundle version mismatch", [](MintOptions& o) {
         o.bundle_version = "58"; o.envelope_bundle = "57"; }, S::BundleVersionMismatch},
      {"bundle version prefix", [](MintOptions& o) {
         o.bundle_version = "57.1"; o.envelope_bundle = "57"; }, S::BundleVersionMismatch},
      {"marketing version as build", [](MintOptions& o) {
         o.bundle_version = "57"; o.envelope_bundle = "1.4.2"; }, S::BundleVersionMismatch},
  };
}

void TestSyntheticMatrix() {
  unsigned count = 0;
  for (const Case& item : Matrix()) {
    const std::string challenge = Challenge(1000 + count);
    MintOptions options = Options(challenge);
    item.change(options);
    const Minted minted = apple_fixture::Mint(Rng(), TestAuthority(), options);
    const AppleAttestStatus status = VerifyMinted(minted, challenge, SyntheticPolicy(item.environment));
    if (status != item.expected) {
      fprintf(stderr, "case '%s': %s, expected %s\n", item.name,
              fortis::auth::AppleAttestStatusName(status), fortis::auth::AppleAttestStatusName(item.expected));
      abort();
    }
    ++count;
  }
  // A valid proof for one challenge does not verify for another.
  const Minted minted = apple_fixture::Mint(Rng(), TestAuthority(), Options(Challenge(1)));
  CHECK_STATUS(VerifyMinted(minted, Challenge(2), SyntheticPolicy()),
               AppleAttestStatus::NonceMismatch);
  CHECK_STATUS(VerifyMinted(minted, Challenge(1), SyntheticPolicy()),
               AppleAttestStatus::Verified);
  // Certificate validity follows the given clock in both directions.
  CHECK_STATUS(VerifyMinted(minted, Challenge(1), SyntheticPolicy(), kNowMs + 3 * 86400000LL),
               AppleAttestStatus::CertificateNotValidNow);
  // The synthetic proof is refused against the official root.
  AppleAttestPolicy official = SyntheticPolicy();
  official.root_der = fortis::auth::kAppleAppAttestRootDer;
  official.root_der_length = fortis::auth::kAppleAppAttestRootDerLength;
  CHECK_STATUS(VerifyMinted(minted, Challenge(1), official), AppleAttestStatus::CertificateUntrusted);
  printf("synthetic adversarial matrix: %u cases plus challenge, clock and root binding: ok\n",
         count);
}

// Hand-built CBOR that declares more than it holds, nests too deep or uses
// reserved encodings: refused without a large allocation or an over-read.
void TestHostileCbor() {
  const AppleAttestPolicy policy = SyntheticPolicy();
  const Bytes hash(32, 1);
  auto verify = [&](const Bytes& attestation) {
    AppleAttestToken token;
    memset(token.key_id, 0x22, 32);
    memcpy(token.bundle_version, "57", 3);
    token.bundle_version_length = 2;
    token.attestation = attestation.data();
    token.attestation_length = attestation.size();
    AppleAttestEvidence evidence;
    return fortis::auth::VerifyAppleAttestation(policy, token, hash.data(), hash.size(), kNowMs,
                                          &evidence);
  };
  const std::vector<Bytes> hostile = {
      {0xa3},                                                  // map with no entries present
      {0xa3, 0x63, 'f', 'm', 't', 0x7b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
      {0xa3, 0x63, 'f', 'm', 't', 0x7f, 0x61, 'a', 0xff},       // indefinite text
      {0xbf, 0xff},                                            // indefinite map
      {0xa3, 0x63, 'f', 'm', 't', 0xf9, 0x3c, 0x00},           // half float
      {0xa3, 0x63, 'f', 'm', 't', 0xc0, 0x60},                 // tag 0
      {0xa3, 0x63, 'f', 'm', 't', 0x1c},                       // reserved additional info
      {0xbb, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00},  // map of 2^32 entries
      {0xa3, 0x78, 0x03, 'f', 'm', 't'},                       // non-shortest key length
      {0xa3, 0x63, 'f', 'm', 't'},                             // truncated
      {0x83, 0x01, 0x02, 0x03},                                // array, not a map
  };
  for (const Bytes& attestation : hostile) {
    const AppleAttestStatus status = verify(attestation);
    CHECK(status == AppleAttestStatus::AttestationMalformed);
  }
  // A valid object with a hostile authData: an extension entry that claims
  // a 2^64-1 element array.
  MintOptions options = Options(Challenge(3));
  Minted minted = apple_fixture::Mint(Rng(), TestAuthority(), options);
  {
    Bytes auth = minted.auth_data;
    // The extensions map {bundle "57", category 4} is the last 63 bytes.
    const size_t map = auth.size() - 63;
    CHECK(auth[map] == 0xa2 && auth[map + 1] == 0x77);
    auth[map] = 0xa3;  // now three entries
    apple_fixture::Text(&auth, "x");
    for (int i = 0; i < 9; ++i) auth.push_back(i ? 0xff : 0x9b);
    // Splice it into a fresh attestation by hand.
    Bytes o;
    apple_fixture::Head(&o, 5, 3);
    apple_fixture::Text(&o, "fmt");
    apple_fixture::Text(&o, "apple-appattest");
    apple_fixture::Text(&o, "attStmt");
    apple_fixture::Head(&o, 5, 2);
    apple_fixture::Text(&o, "x5c");
    apple_fixture::Head(&o, 4, 2);
    apple_fixture::Blob(&o, minted.leaf_der);
    apple_fixture::Blob(&o, TestAuthority().intermediate_der);
    apple_fixture::Text(&o, "receipt");
    apple_fixture::Blob(&o, Bytes(10, 1));
    apple_fixture::Text(&o, "authData");
    apple_fixture::Blob(&o, auth);
    CHECK_STATUS(verify(o), AppleAttestStatus::ExtensionsMalformed);
    // authData beyond its 1,024-byte bound.
    Bytes big;
    apple_fixture::Head(&big, 5, 3);
    apple_fixture::Text(&big, "fmt");
    apple_fixture::Text(&big, "apple-appattest");
    apple_fixture::Text(&big, "authData");
    apple_fixture::Blob(&big, Bytes(1025, 0));
    CHECK_STATUS(verify(big), AppleAttestStatus::AttestationMalformed);
  }
  puts("hostile CBOR: huge declared lengths/counts, indefinite, tags, floats, bounds: ok");
}

#ifdef FORTIS_APPLE_ATTEST_TEST_ROOT

std::string WriteRoot(const std::string& directory) {
  const std::string path = directory + "/root.der";
  FILE* file = fopen(path.c_str(), "wb");
  CHECK(file);
  CHECK(fwrite(TestAuthority().root_der.data(), 1, TestAuthority().root_der.size(), file) ==
        TestAuthority().root_der.size());
  fclose(file);
  CHECK(setenv(fortis::auth::kAppleTestRootEnvironment, path.c_str(), 1) == 0);
  return path;
}

void ExpectUntouched(KvStore& store, DeviceAuthStore& auth, const std::string& challenge,
                     const Bytes& key_id, size_t devices) {
  CHECK(auth.PeekChallenge(challenge.data(), challenge.size(), kNowMs) == KvReadStatus::Found);
  CHECK(CountPrefix(store, "device/id/", kNowMs) == devices);
  AppleKeyAssociation association;
  CHECK(auth.LoadAppleKeyAssociation(key_id.data(), kNowMs, &association) ==
        KvReadStatus::Missing);
}

// Every rejection in the matrix, through the enrollment entry point: the
// challenge stays live and nothing is written.
void TestRejectionsConsumeNothing() {
  const std::string directory = MakeDirectory("reject");
  WriteRoot(directory);
  KvStore store;
  CHECK(store.Open(directory.c_str()));
  DeviceAuthStore auth(&store);
  unsigned rejected = 0, seed = 2000;
  for (const Case& item : Matrix()) {
    if (item.expected == AppleAttestStatus::Verified) continue;
    const std::string challenge = Challenge(seed++);
    CHECK(fortis::auth::PersistDeviceChallenge(&store, challenge.c_str(), kNowMs));
    MintOptions options = Options(challenge);
    item.change(options);
    const Minted minted = apple_fixture::Mint(Rng(), TestAuthority(), options);
    DeviceCredentials credentials{};
    const AppleEnrollOutcome outcome =
        Enroll(auth, challenge, minted.token, kNowMs, &credentials, item.environment);
    if (outcome.status != AppleEnrollStatus::InvalidProof || outcome.verification != item.expected) {
      fprintf(stderr, "case '%s': enroll %d/%s\n", item.name, static_cast<int>(outcome.status),
              fortis::auth::AppleAttestStatusName(outcome.verification));
      abort();
    }
    ExpectUntouched(store, auth, challenge, minted.key_id, 0);
    ++rejected;
  }
  // Request-shape refusals that never reach verification.
  const std::string challenge = Challenge(seed++);
  CHECK(fortis::auth::PersistDeviceChallenge(&store, challenge.c_str(), kNowMs));
  const Minted minted = apple_fixture::Mint(Rng(), TestAuthority(), Options(challenge));
  DeviceCredentials credentials{};
  CHECK(Enroll(auth, challenge, minted.token, kNowMs, &credentials,
               AppleAttestEnvironment::Production, nullptr).status ==
        AppleEnrollStatus::InvalidProof);
  CHECK(Enroll(auth, challenge, minted.token, kNowMs, &credentials,
               AppleAttestEnvironment::Production, "").status ==
        AppleEnrollStatus::InvalidProof);
  CHECK(Enroll(auth, challenge, "", kNowMs, &credentials).status ==
        AppleEnrollStatus::InvalidProof);
  CHECK(Enroll(auth, challenge, minted.token + "A", kNowMs, &credentials).status ==
        AppleEnrollStatus::InvalidProof);
  CHECK(Enroll(auth, challenge.substr(0, 42), minted.token, kNowMs, &credentials).status ==
        AppleEnrollStatus::ChallengeUnavailable);
  CHECK(Enroll(auth, challenge + "A", minted.token, kNowMs, &credentials).status ==
        AppleEnrollStatus::ChallengeUnavailable);
  std::string with_nul = challenge;
  with_nul[10] = '\0';
  CHECK(Enroll(auth, with_nul, minted.token, kNowMs, &credentials).status ==
        AppleEnrollStatus::ChallengeUnavailable);
  CHECK(Enroll(auth, Challenge(seed + 50), minted.token, kNowMs, &credentials).status ==
        AppleEnrollStatus::ChallengeUnavailable);  // never minted
  ExpectUntouched(store, auth, challenge, minted.key_id, 0);
  // The same challenge then enrolls with its valid proof.
  CHECK(Enroll(auth, challenge, minted.token, kNowMs, &credentials).status ==
        AppleEnrollStatus::Enrolled);
  store.Close();
  RemoveDirectory(directory);
  printf("rejections through enrollment: %u verifier cases + 8 request cases, challenge "
         "kept, nothing written: ok\n", rejected);
}

void TestEnrollmentLifecycle() {
  const std::string directory = MakeDirectory("life");
  WriteRoot(directory);
  const std::string challenge = Challenge(1), second = Challenge(2), expiring = Challenge(3);
  Minted minted;
  DeviceCredentials enrolled{}, rotated{};
  apple_fixture::Key reused_key;
  reused_key.Generate(Rng(), MBEDTLS_ECP_DP_SECP256R1);
  {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    DeviceAuthStore auth(&store);
    CHECK(fortis::auth::PersistDeviceChallenge(&store, challenge.c_str(), kNowMs));
    CHECK(fortis::auth::PersistDeviceChallenge(&store, second.c_str(), kNowMs));
    MintOptions options = Options(challenge);
    options.leaf_key = &reused_key;
    options.receipt_size = 3977;
    minted = apple_fixture::Mint(Rng(), TestAuthority(), options);
    const AppleEnrollOutcome outcome = Enroll(auth, challenge, minted.token, kNowMs + 5, &enrolled);
    CHECK(outcome.status == AppleEnrollStatus::Enrolled);
    CHECK(auth.PeekChallenge(challenge.data(), challenge.size(), kNowMs + 6) ==
          KvReadStatus::Missing);
    DeviceRecord row{};
    CHECK(auth.Authenticate(enrolled.token, kNowMs + 10, true, &row) == DeviceAuthResult::Valid);
    CHECK(row.attestation == DeviceAttestation::AppleAppAttest &&
          row.platform == DevicePlatform::Ios && row.quota_tier == DeviceQuotaTier::Attested &&
          row.has_app_version && strcmp(row.app_version, kVersion) == 0);
    AppleKeyAssociation association;
    CHECK(auth.LoadAppleKeyAssociation(minted.key_id.data(), kNowMs + 10, &association) ==
          KvReadStatus::Found);
    CHECK(strcmp(association.device_id, enrolled.id) == 0);
    CHECK(Bytes(association.public_key, association.public_key + 65) == minted.public_key);
    CHECK(association.environment == AppleAttestEnvironment::Production &&
          association.validation_category == 4 && association.counter == 0 &&
          strcmp(association.bundle_version, "57") == 0 &&
          association.verified_at_ms == kNowMs + 5 && association.receipt_length == 3977);
    for (uint32_t i = 0; i < association.receipt_length; ++i)
      CHECK(association.receipt[i] == static_cast<uint8_t>(i * 7 + 3));
    // Replaying the accepted proof finds its challenge consumed.
    DeviceCredentials replay{};
    CHECK(Enroll(auth, challenge, minted.token, kNowMs + 20, &replay).status ==
          AppleEnrollStatus::ChallengeUnavailable);
    // The same attested key with a fresh challenge and a fresh, valid
    // attestation still cannot back a second device; the challenge survives.
    MintOptions again = Options(second);
    again.leaf_key = &reused_key;
    const Minted reattested = apple_fixture::Mint(Rng(), TestAuthority(), again);
    CHECK(reattested.key_id == minted.key_id);
    CHECK(Enroll(auth, second, reattested.token, kNowMs + 30, &replay).status ==
          AppleEnrollStatus::KeyAlreadyAssociated);
    CHECK(auth.PeekChallenge(second.data(), second.size(), kNowMs + 31) == KvReadStatus::Found);
    CHECK(CountPrefix(store, "device/id/", kNowMs) == 1);
    // Rotation keeps attestation, tier and version; the association is
    // untouched.
    CHECK(auth.Authenticate(enrolled.token, kNowMs + 40, true, &row) == DeviceAuthResult::Valid);
    CHECK(auth.Rotate(row, kNowMs + 50, kTtl, &rotated) == fortis::auth::DeviceRotateResult::Rotated);
    CHECK(auth.Authenticate(enrolled.token, kNowMs + 60, true, &row) ==
          DeviceAuthResult::Unauthorized);
    CHECK(auth.Authenticate(rotated.token, kNowMs + 60, true, &row) == DeviceAuthResult::Valid);
    CHECK(row.attestation == DeviceAttestation::AppleAppAttest &&
          row.quota_tier == DeviceQuotaTier::Attested);
    CHECK(auth.Rotate(row, kNowMs + 70, kTtl, &rotated) == fortis::auth::DeviceRotateResult::Rotated);
    // Challenge expiry: live at its last instant, gone at its expiry.
    CHECK(fortis::auth::PersistDeviceChallenge(&store, expiring.c_str(), kNowMs));
    const Minted late = apple_fixture::Mint(Rng(), TestAuthority(), Options(expiring));
    CHECK(Enroll(auth, expiring, late.token, kNowMs + fortis::auth::kDeviceChallengeTtlMs, &replay)
              .status == AppleEnrollStatus::ChallengeUnavailable);
    CHECK(Enroll(auth, expiring, late.token, kNowMs + fortis::auth::kDeviceChallengeTtlMs - 1, &replay)
              .status == AppleEnrollStatus::Enrolled);
  }
  // Restart: device, rotated bearer, consumed challenge and association all
  // come back from replay, and again after compaction.
  for (int pass = 0; pass < 2; ++pass) {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    DeviceAuthStore auth(&store);
    DeviceRecord row{};
    CHECK(auth.Authenticate(rotated.token, kNowMs + 80, true, &row) == DeviceAuthResult::Valid);
    CHECK(row.attestation == DeviceAttestation::AppleAppAttest &&
          row.quota_tier == DeviceQuotaTier::Attested);
    AppleKeyAssociation association;
    CHECK(auth.LoadAppleKeyAssociation(minted.key_id.data(), kNowMs + 80, &association) ==
          KvReadStatus::Found);
    CHECK(strcmp(association.device_id, rotated.id) == 0);
    DeviceCredentials replay{};
    CHECK(Enroll(auth, challenge, minted.token, kNowMs + 90, &replay).status ==
          AppleEnrollStatus::ChallengeUnavailable);
    // Revocation is enforced; the association stays (the key is spent).
    if (pass == 1) {
      CHECK(auth.Revoke(rotated.id, kNowMs + 100));
      CHECK(auth.Authenticate(rotated.token, kNowMs + 101, true, &row) ==
            DeviceAuthResult::SessionExpired);
      CHECK(auth.LoadAppleKeyAssociation(minted.key_id.data(), kNowMs + 101, &association) ==
            KvReadStatus::Found);
    }
    CHECK(store.Compact());
  }
  RemoveDirectory(directory);
  puts("Apple enrollment lifecycle: record, association, replay, key reuse, rotation, "
       "challenge expiry, restart, compaction, revoke: ok");
}

void TestDevelopmentEnvironment() {
  const std::string directory = MakeDirectory("devenv");
  WriteRoot(directory);
  KvStore store;
  CHECK(store.Open(directory.c_str()));
  DeviceAuthStore auth(&store);
  const std::string challenge = Challenge(5);
  CHECK(fortis::auth::PersistDeviceChallenge(&store, challenge.c_str(), kNowMs));
  MintOptions options = Options(challenge);
  options.development = true;
  options.category = 3;
  const Minted minted = apple_fixture::Mint(Rng(), TestAuthority(), options);
  DeviceCredentials credentials{};
  const AppleEnrollOutcome production = Enroll(auth, challenge, minted.token, kNowMs, &credentials);
  CHECK(production.status == AppleEnrollStatus::InvalidProof &&
        production.verification == AppleAttestStatus::EnvironmentMismatch);
  CHECK(Enroll(auth, challenge, minted.token, kNowMs, &credentials,
               AppleAttestEnvironment::Development).status == AppleEnrollStatus::Enrolled);
  AppleKeyAssociation association;
  CHECK(auth.LoadAppleKeyAssociation(minted.key_id.data(), kNowMs, &association) ==
        KvReadStatus::Found);
  CHECK(association.environment == AppleAttestEnvironment::Development &&
        association.validation_category == 3);
  store.Close();
  RemoveDirectory(directory);
  puts("development environment: AAGUID and category per configuration: ok");
}

void TestLegacyWithoutExtensions() {
  const std::string directory = MakeDirectory("legacy");
  WriteRoot(directory);
  const std::string challenge = Challenge(45);
  MintOptions options = Options(challenge);
  options.extension_mode = 10;
  const Minted minted = apple_fixture::Mint(Rng(), TestAuthority(), options);
  AppleAttestEvidence evidence;
  CHECK_STATUS(VerifyMinted(minted, challenge, SyntheticPolicy(), kNowMs, &evidence),
               AppleAttestStatus::Verified);
  CHECK(evidence.validation_category == 0 && evidence.bundle_version_length == 0);
  DeviceCredentials credentials{};
  {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    DeviceAuthStore auth(&store);
    CHECK(fortis::auth::PersistDeviceChallenge(&store, challenge.c_str(), kNowMs));
    CHECK(Enroll(auth, challenge, minted.token, kNowMs, &credentials).status ==
          AppleEnrollStatus::Enrolled);
    DeviceRecord row{};
    CHECK(auth.Authenticate(credentials.token, kNowMs + 1, true, &row) == DeviceAuthResult::Valid);
    CHECK(row.attestation == DeviceAttestation::AppleAppAttest &&
          row.quota_tier == DeviceQuotaTier::Attested);
    store.Close();
  }
  {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    DeviceAuthStore auth(&store);
    AppleKeyAssociation association;
    CHECK(auth.LoadAppleKeyAssociation(minted.key_id.data(), kNowMs + 2, &association) ==
          KvReadStatus::Found);
    CHECK(association.validation_category == 0 && association.bundle_version_length == 0 &&
          association.receipt_length > 0);
    DeviceCredentials replay{};
    CHECK(Enroll(auth, challenge, minted.token, kNowMs + 2, &replay).status ==
          AppleEnrollStatus::ChallengeUnavailable);
    store.Close();
  }
  RemoveDirectory(directory);
  puts("legacy App Attest object: verified key, unknown launch metadata, durable association: ok");
}

// Simultaneous uses: one proof raced by eight threads; two different valid
// proofs for one challenge; one key attested for two challenges.
void TestRaces() {
  const std::string directory = MakeDirectory("race");
  WriteRoot(directory);
  KvStore store;
  CHECK(store.Open(directory.c_str()));
  constexpr unsigned kThreads = 8, kRounds = 30;
  std::vector<std::string> challenges;
  std::vector<Minted> proofs;
  for (unsigned round = 0; round < kRounds; ++round) {
    challenges.push_back(Challenge(3000 + round));
    CHECK(fortis::auth::PersistDeviceChallenge(&store, challenges.back().c_str(), kNowMs));
    proofs.push_back(apple_fixture::Mint(Rng(), TestAuthority(), Options(challenges.back())));
  }
  std::atomic<unsigned> enrolled{0}, unavailable{0}, busy{0}, other{0};
  std::atomic<unsigned> arrived{0};
  for (unsigned round = 0; round < kRounds; ++round) {
    arrived = 0;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (unsigned thread = 0; thread < kThreads; ++thread) {
      threads.emplace_back([&, round] {
        DeviceAuthStore auth(&store);
        DeviceCredentials credentials{};
        ++arrived;
        while (arrived.load() < kThreads) std::this_thread::yield();
        switch (Enroll(auth, challenges[round], proofs[round].token, kNowMs + 1, &credentials)
                    .status) {
          case AppleEnrollStatus::Enrolled: ++enrolled; break;
          case AppleEnrollStatus::ChallengeUnavailable: ++unavailable; break;
          case AppleEnrollStatus::Busy: ++busy; break;
          default: ++other; break;
        }
      });
    }
    for (auto& thread : threads) thread.join();
  }
  CHECK(enrolled == kRounds && other == 0 && enrolled + unavailable + busy == kRounds * kThreads);
  CHECK(CountPrefix(store, "device/id/", kNowMs) == kRounds);
  CHECK(CountPrefix(store, "attest:apple:key:", kNowMs) == kRounds);
  printf("one proof x %u threads x %u rounds: %u enrolled, %u consumed-loser, %u busy: ok\n",
         kThreads, kRounds, enrolled.load(), unavailable.load(), busy.load());
  // Two distinct valid proofs (two keys) bound to one challenge.
  unsigned two_proof_wins = 0;
  for (unsigned round = 0; round < 20; ++round) {
    const std::string challenge = Challenge(4000 + round);
    CHECK(fortis::auth::PersistDeviceChallenge(&store, challenge.c_str(), kNowMs));
    const Minted a = apple_fixture::Mint(Rng(), TestAuthority(), Options(challenge));
    const Minted b = apple_fixture::Mint(Rng(), TestAuthority(), Options(challenge));
    std::atomic<unsigned> wins{0}, losses{0};
    std::atomic<unsigned> ready{0};
    auto run = [&](const Minted& proof) {
      DeviceAuthStore auth(&store);
      DeviceCredentials credentials{};
      ++ready;
      while (ready.load() < 2) std::this_thread::yield();
      const AppleEnrollStatus status =
          Enroll(auth, challenge, proof.token, kNowMs + 1, &credentials).status;
      if (status == AppleEnrollStatus::Enrolled) ++wins;
      else if (status == AppleEnrollStatus::ChallengeUnavailable) ++losses;
    };
    std::thread first(run, std::cref(a)), second(run, std::cref(b));
    first.join();
    second.join();
    CHECK(wins == 1 && losses == 1);
    DeviceAuthStore auth(&store);
    AppleKeyAssociation association;
    const bool a_won = auth.LoadAppleKeyAssociation(a.key_id.data(), kNowMs, &association) ==
                       KvReadStatus::Found;
    const bool b_won = auth.LoadAppleKeyAssociation(b.key_id.data(), kNowMs, &association) ==
                       KvReadStatus::Found;
    CHECK(a_won != b_won);
    ++two_proof_wins;
  }
  // One key attested for two live challenges, raced: one device, and the
  // loser's challenge stays live.
  for (unsigned round = 0; round < 20; ++round) {
    apple_fixture::Key key;
    key.Generate(Rng(), MBEDTLS_ECP_DP_SECP256R1);
    const std::string c1 = Challenge(5000 + 2 * round), c2 = Challenge(5001 + 2 * round);
    CHECK(fortis::auth::PersistDeviceChallenge(&store, c1.c_str(), kNowMs));
    CHECK(fortis::auth::PersistDeviceChallenge(&store, c2.c_str(), kNowMs));
    MintOptions o1 = Options(c1), o2 = Options(c2);
    o1.leaf_key = o2.leaf_key = &key;
    const Minted p1 = apple_fixture::Mint(Rng(), TestAuthority(), o1);
    const Minted p2 = apple_fixture::Mint(Rng(), TestAuthority(), o2);
    std::atomic<unsigned> ready{0};
    AppleEnrollStatus s1{}, s2{};
    auto run = [&](const std::string& c, const Minted& proof, AppleEnrollStatus* out) {
      DeviceAuthStore auth(&store);
      DeviceCredentials credentials{};
      ++ready;
      while (ready.load() < 2) std::this_thread::yield();
      *out = Enroll(auth, c, proof.token, kNowMs + 1, &credentials).status;
    };
    std::thread first(run, std::cref(c1), std::cref(p1), &s1);
    std::thread second(run, std::cref(c2), std::cref(p2), &s2);
    first.join();
    second.join();
    CHECK((s1 == AppleEnrollStatus::Enrolled) != (s2 == AppleEnrollStatus::Enrolled));
    const AppleEnrollStatus loser = s1 == AppleEnrollStatus::Enrolled ? s2 : s1;
    CHECK(loser == AppleEnrollStatus::KeyAlreadyAssociated);
    DeviceAuthStore auth(&store);
    const std::string& losing = s1 == AppleEnrollStatus::Enrolled ? c2 : c1;
    CHECK(auth.PeekChallenge(losing.data(), losing.size(), kNowMs + 2) == KvReadStatus::Found);
  }
  store.Close();
  RemoveDirectory(directory);
  printf("two proofs for one challenge (%u rounds) and one key for two challenges (20 rounds): "
         "one device each: ok\n", two_proof_wins);
}

// SIGKILL at each point of the enrollment frame: the challenge, the device
// and the association stand or fall together, and a retry agrees.
void TestCrashImages() {
  struct Point { const char* name; bool committed; };
  const Point points[] = {{"compare_matched", false}, {"write_after_header", false},
                          {"write_before_fsync", false}, {"write_acknowledged", true}};
  const std::string directory = MakeDirectory("crash");
  WriteRoot(directory);
  unsigned seed = 6000;
  for (const Point& point : points) {
    const std::string challenge = Challenge(seed++);
    size_t devices_before = 0;
    {
      KvStore store;
      CHECK(store.Open(directory.c_str()));
      CHECK(fortis::auth::PersistDeviceChallenge(&store, challenge.c_str(), kNowMs));
      devices_before = CountPrefix(store, "device/id/", kNowMs);
    }
    const Minted minted = apple_fixture::Mint(Rng(), TestAuthority(), Options(challenge));
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
      alarm(30);
      CHECK(setenv("FORTIS_KV_TEST_KILL", point.name, 1) == 0);
      KvStore store;
      if (!store.Open(directory.c_str())) _exit(4);
      DeviceAuthStore auth(&store);
      DeviceCredentials credentials{};
      Enroll(auth, challenge, minted.token, kNowMs + 1, &credentials);
      _exit(7);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    DeviceAuthStore auth(&store);
    AppleKeyAssociation association;
    const bool live = auth.PeekChallenge(challenge.data(), challenge.size(), kNowMs + 2) ==
                      KvReadStatus::Found;
    const bool associated = auth.LoadAppleKeyAssociation(minted.key_id.data(), kNowMs + 2,
                                                         &association) == KvReadStatus::Found;
    const size_t devices = CountPrefix(store, "device/id/", kNowMs);
    CHECK(live == !point.committed && associated == point.committed &&
          devices == devices_before + (point.committed ? 1 : 0));
    if (associated) {
      char key[48];
      snprintf(key, sizeof(key), "device/id/%s", association.device_id);
      uint8_t data[512];
      size_t size = 0;
      CHECK(store.GetStatus(key, kNowMs, data, sizeof(data), &size) == KvReadStatus::Found);
      CHECK(data[95] == static_cast<uint8_t>(DeviceAttestation::AppleAppAttest));
    }
    DeviceCredentials credentials{};
    const AppleEnrollStatus retry = Enroll(auth, challenge, minted.token, kNowMs + 3,
                                           &credentials).status;
    CHECK(retry == (point.committed ? AppleEnrollStatus::ChallengeUnavailable
                                    : AppleEnrollStatus::Enrolled));
    printf("crash image at %-18s: %s\n", point.name,
           point.committed ? "committed together" : "nothing committed");
  }
  // An injected torn append fails the enrollment (store closes) and leaves
  // the challenge usable after restart.
  const std::string challenge = Challenge(seed++);
  const Minted minted = apple_fixture::Mint(Rng(), TestAuthority(), Options(challenge));
  {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    CHECK(fortis::auth::PersistDeviceChallenge(&store, challenge.c_str(), kNowMs));
    DeviceAuthStore auth(&store);
    CHECK(setenv("FORTIS_KV_TEST_FAIL", "write_after_header", 1) == 0);
    DeviceCredentials credentials{};
    CHECK(Enroll(auth, challenge, minted.token, kNowMs + 1, &credentials).status ==
          AppleEnrollStatus::StorageError);
    CHECK(unsetenv("FORTIS_KV_TEST_FAIL") == 0);
  }
  {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    DeviceAuthStore auth(&store);
    AppleKeyAssociation association;
    CHECK(auth.LoadAppleKeyAssociation(minted.key_id.data(), kNowMs, &association) ==
          KvReadStatus::Missing);
    DeviceCredentials credentials{};
    CHECK(Enroll(auth, challenge, minted.token, kNowMs + 2, &credentials).status ==
          AppleEnrollStatus::Enrolled);
  }
  RemoveDirectory(directory);
  puts("injected torn append: StorageError, challenge reusable after restart: ok");
}

#endif  // FORTIS_APPLE_ATTEST_TEST_ROOT

// The release object never trusts a test root, whatever the environment.
void TestReleaseRefusesSyntheticRoot() {
#ifndef FORTIS_APPLE_ATTEST_TEST_ROOT
  const std::string directory = MakeDirectory("release");
  const std::string path = directory + "/root.der";
  FILE* file = fopen(path.c_str(), "wb");
  CHECK(file);
  fwrite(TestAuthority().root_der.data(), 1, TestAuthority().root_der.size(), file);
  fclose(file);
  CHECK(setenv("FORTIS_TEST_APPLE_ATTEST_ROOT", path.c_str(), 1) == 0);
  KvStore store;
  CHECK(store.Open(directory.c_str()));
  DeviceAuthStore auth(&store);
  const std::string challenge = Challenge(7);
  CHECK(fortis::auth::PersistDeviceChallenge(&store, challenge.c_str(), kNowMs));
  const Minted minted = apple_fixture::Mint(Rng(), TestAuthority(), Options(challenge));
  CHECK_STATUS(VerifyMinted(minted, challenge, SyntheticPolicy()), AppleAttestStatus::Verified);
  DeviceCredentials credentials{};
  const AppleEnrollOutcome outcome = Enroll(auth, challenge, minted.token, kNowMs, &credentials);
  CHECK(outcome.status == AppleEnrollStatus::InvalidProof &&
        outcome.verification == AppleAttestStatus::CertificateUntrusted);
  CHECK(auth.PeekChallenge(challenge.data(), challenge.size(), kNowMs) == KvReadStatus::Found);
  CHECK(CountPrefix(store, "device/id/", kNowMs) == 0);
  store.Close();
  RemoveDirectory(directory);
  CHECK(unsetenv("FORTIS_TEST_APPLE_ATTEST_ROOT") == 0);
  puts("release object: synthetic proof refused (CertificateUntrusted) with the test-root "
       "variable set; challenge kept: ok");
#endif
}

off_t FileSize(const std::string& directory) {
  struct stat info {};
  CHECK(stat((directory + "/fortis.kv").c_str(), &info) == 0);
  return info.st_size;
}

// KvStore::CompareAndWrite with absent-key guards, which the enrollment frame
// uses for the key association.
void TestGuardedCompareAndWrite() {
  using fortis::storage::KvCompareStatus;
  using fortis::storage::KvExpectation;
  using fortis::storage::KvMutation;
  const std::string directory = MakeDirectory("guard");
  KvStore store;
  CHECK(store.Open(directory.c_str()));
  const char marker = '1';
  const Bytes big(5000, 7);
  const KvMutation seed[] = {{"cond/a", &marker, 1, 0, false},
                             {"held", "x", 1, 0, false}, {"held/big", big.data(), 5000, 0, false},
                             {"gone", "x", 1, kNowMs, false}};
  CHECK(store.Write(seed, 4));
  const KvExpectation a{"cond/a", &marker, 1}, none{"cond/none", &marker, 1};
  const KvMutation take_a[] = {{"cond/a", nullptr, 0, 0, true}, {"won", "a", 1, 0, false}};
  const char* held[] = {"held"};
  const char* held_big[] = {"held/big"};
  const char* gone[] = {"gone"};
  const char* five[] = {"k1", "k2", "k3", "k4", "k5"};
  const char* empty_key[] = {""};
  const off_t before = FileSize(directory);
  CHECK(store.CompareAndWrite(a, held, 1, kNowMs, take_a, 2) == KvCompareStatus::AbsentKeyPresent);
  CHECK(store.CompareAndWrite(a, held_big, 1, kNowMs, take_a, 2) ==
        KvCompareStatus::AbsentKeyPresent);
  CHECK(store.CompareAndWrite(none, held, 1, kNowMs, take_a, 2) ==
        KvCompareStatus::MissingOrExpired);  // the expectation is checked first
  CHECK(store.CompareAndWrite(a, five, 5, kNowMs, take_a, 2) == KvCompareStatus::InvalidArgument);
  CHECK(store.CompareAndWrite(a, nullptr, 1, kNowMs, take_a, 2) ==
        KvCompareStatus::InvalidArgument);
  CHECK(store.CompareAndWrite(a, empty_key, 1, kNowMs, take_a, 2) ==
        KvCompareStatus::InvalidArgument);
  CHECK(FileSize(directory) == before);
  // An expired guarded key reads as absent at its expiry instant.
  CHECK(store.CompareAndWrite(a, gone, 1, kNowMs - 1, take_a, 2) ==
        KvCompareStatus::AbsentKeyPresent);
  CHECK(store.CompareAndWrite(a, gone, 1, kNowMs, take_a, 2) == KvCompareStatus::Committed);
  store.Close();
  // Eight consumers of distinct live expectations race to create one
  // guarded key: exactly one commits per round, also after reopen.
  constexpr unsigned kThreads = 8, kRounds = 200;
  CHECK(store.Open(directory.c_str()));
  for (unsigned round = 0; round < kRounds; ++round)
    for (unsigned thread = 0; thread < kThreads; ++thread) {
      const std::string key = "race/c/" + std::to_string(round) + "/" + std::to_string(thread);
      const KvMutation mutation{key.c_str(), &marker, 1, 0, false};
      CHECK(store.Write(&mutation, 1));
    }
  std::atomic<unsigned> committed{0}, present{0}, other{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  std::atomic<unsigned> generation{0}, arrived{0};
  for (unsigned thread = 0; thread < kThreads; ++thread)
    threads.emplace_back([&, thread] {
      for (unsigned round = 0; round < kRounds; ++round) {
        const std::string condition = "race/c/" + std::to_string(round) + "/" + std::to_string(thread);
        const std::string guarded = "race/k/" + std::to_string(round);
        const KvExpectation expected{condition.c_str(), &marker, 1};
        const char* absent[] = {guarded.c_str()};
        const KvMutation writes[] = {{condition.c_str(), nullptr, 0, 0, true},
                                     {guarded.c_str(), &marker, 1, 0, false}};
        if (arrived.fetch_add(1) + 1 == kThreads * (round + 1)) generation.fetch_add(1);
        while (generation.load() <= round) std::this_thread::yield();
        switch (store.CompareAndWrite(expected, absent, 1, kNowMs, writes, 2)) {
          case KvCompareStatus::Committed: ++committed; break;
          case KvCompareStatus::AbsentKeyPresent: ++present; break;
          default: ++other; break;
        }
      }
    });
  for (auto& thread : threads) thread.join();
  CHECK(committed == kRounds && present == kRounds * (kThreads - 1) && other == 0);
  store.Close();
  CHECK(store.Open(directory.c_str()));
  CHECK(CountPrefix(store, "race/k/", kNowMs) == kRounds);
  CHECK(CountPrefix(store, "race/c/", kNowMs) == kRounds * (kThreads - 1));
  store.Close();
  for (const char* file : {"/fortis.kv", "/fortis.kv.lock", "/fortis.kv.compact"})
    unlink((directory + file).c_str());
  CHECK(rmdir(directory.c_str()) == 0);
  printf("guarded CompareAndWrite: outcomes, ordering, bounds, expiry, %u rounds x %u threads "
         "on one guarded key: ok\n", kRounds, kThreads);
}

void TestVerifySlots() {
  CHECK(fortis::auth::TryAcquireAppleVerifySlot());
  CHECK(fortis::auth::TryAcquireAppleVerifySlot());
  CHECK(!fortis::auth::TryAcquireAppleVerifySlot());
  fortis::auth::ReleaseAppleVerifySlot();
  CHECK(fortis::auth::TryAcquireAppleVerifySlot());
  fortis::auth::ReleaseAppleVerifySlot();
  fortis::auth::ReleaseAppleVerifySlot();
  puts("verification gate: two in flight, third refused: ok");
}

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define FORTIS_TEST_SANITIZED_HEAP 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define FORTIS_TEST_SANITIZED_HEAP 1
#endif
#endif
#ifndef FORTIS_TEST_SANITIZED_HEAP
#define FORTIS_TEST_SANITIZED_HEAP 0
#endif

long MaxRssKiB() {
  struct rusage usage {};
  getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
  return usage.ru_maxrss / 1024;
#else
  return usage.ru_maxrss;
#endif
}

// Repeated verification of the largest admissible objects from four threads
// (the HTTP worker count) does not grow the process: peak RSS and per-call
// time are reported.
void TestResourceBounds() {
  const std::string challenge = Challenge(9);
  MintOptions options = Options(challenge);
  options.receipt_size = fortis::auth::kAppleReceiptMaxBytes;
  options.leaf_padding = 2500;
  const Minted minted = apple_fixture::Mint(Rng(), TestAuthority(), options);
  CHECK(minted.token.size() > 15000 && minted.token.size() <= fortis::auth::kAppleTokenMaxChars);
  CHECK_STATUS(VerifyMinted(minted, challenge, SyntheticPolicy()), AppleAttestStatus::Verified);
  const AppleAttestPolicy policy = SyntheticPolicy();
  for (int i = 0; i < 50; ++i) CHECK_STATUS(VerifyMinted(minted, challenge, policy),
                                            AppleAttestStatus::Verified);
  const long before = MaxRssKiB();
  constexpr int kThreads = 4, kPerThread = 150;
  const int64_t start = WallMs();
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  std::atomic<int> failures{0};
  for (int t = 0; t < kThreads; ++t)
    threads.emplace_back([&] {
      for (int i = 0; i < kPerThread; ++i)
        if (VerifyMinted(minted, challenge, policy) != AppleAttestStatus::Verified) ++failures;
    });
  for (auto& thread : threads) thread.join();
  const int64_t elapsed = WallMs() - start;
  const long after = MaxRssKiB();
  CHECK(failures == 0);
#if FORTIS_TEST_SANITIZED_HEAP
  // ASan's quarantine and TSan's shadow memory keep freed blocks resident, so
  // peak RSS says nothing about the verifier there (LeakSanitizer does).
  (void)before;
  (void)after;
#else
  CHECK(after - before < 8 * 1024);
#endif
  printf("resource bounds: %zu-character token, %d verifications on %d threads in %lld ms "
         "(%.2f ms each), peak RSS %ld KiB -> %ld KiB: ok\n",
         minted.token.size(), kThreads * kPerThread, kThreads, static_cast<long long>(elapsed),
         static_cast<double>(elapsed) * kThreads / (kThreads * kPerThread), before, after);
}

}  // namespace

int main() try {
  TestEnvelope();
  TestSyntheticMatrix();
  TestHostileCbor();
  TestGuardedCompareAndWrite();
  TestVerifySlots();
  TestResourceBounds();
  TestReleaseRefusesSyntheticRoot();
#ifdef FORTIS_APPLE_ATTEST_TEST_ROOT
  TestRejectionsConsumeNothing();
  TestEnrollmentLifecycle();
  TestLegacyWithoutExtensions();
  TestDevelopmentEnvironment();
  TestRaces();
  TestCrashImages();
  puts("Apple App Attest step B test (test-root build) passed");
#else
  puts("Apple App Attest step B test (release object) passed");
#endif
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
