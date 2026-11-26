// Play Integrity release build: the decode client compiled WITHOUT
// FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT. Proves at compile time that the
// configuration has no endpoint, transport or clock override, and at run
// time that an unconfigured or misconfigured client refuses every decode
// without network I/O. tests/play_integrity_google_test.py additionally
// inspects the release object and server binary for seam symbols.

#include "../src/auth/play_integrity_google.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <type_traits>

using namespace fortis::auth;

#ifdef FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT
#error "the release test must be built without the test transport"
#endif

#define CHECK(condition)                                                            \
  do {                                                                              \
    if (!(condition)) {                                                             \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
      abort();                                                                      \
    }                                                                               \
  } while (0)

namespace {

template <typename T, typename = void>
struct HasTestSeam : std::false_type {};
template <typename T>
struct HasTestSeam<T, decltype(void(&T::test))> : std::true_type {};

static_assert(!HasTestSeam<fortis::auth::PlayIntegrityGoogleConfig>::value,
              "release configuration must not carry the test seam");
static_assert(!std::is_constructible<fortis::auth::PlayIntegrityDecodedResponse, const uint8_t*,
                                     size_t>::value,
              "request bytes can never become a decoded response");
static_assert(sizeof(fortis::auth::kPlayIntegrityDecodePath) ==
                  sizeof("/v1/io.fortisnet.wallet:decodeIntegrityToken"),
              "decode path is fixed");

}  // namespace

int main(int argc, char** argv) {
  using namespace fortis;
  CHECK(strcmp(kGoogleOAuthHost, "oauth2.googleapis.com") == 0);
  CHECK(strcmp(kGoogleOAuthPath, "/token") == 0);
  CHECK(strcmp(kGoogleOAuthAudience, "https://oauth2.googleapis.com/token") == 0);
  CHECK(strcmp(kPlayIntegrityScope, "https://www.googleapis.com/auth/playintegrity") == 0);
  CHECK(strcmp(kPlayIntegrityApiHost, "playintegrity.googleapis.com") == 0);
  CHECK(strcmp(kPlayIntegrityDecodePath, "/v1/io.fortisnet.wallet:decodeIntegrityToken") == 0);

  static const char kChallenge[] = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8";
  static const char kToken[] = "eyJhbGciOiJBMjU2S1ciLCJlbmMiOiJBMjU2R0NNIn0.release.iv.c.t";
  PlayDecodeInput input;
  input.challenge = kChallenge;
  input.challenge_length = strlen(kChallenge);
  input.app_version = "1.0.0";
  input.app_version_length = 5;
  input.integrity_token = kToken;
  input.integrity_token_length = strlen(kToken);

  // Unconfigured: refused, no verdict, no evidence, no exchange.
  PlayIntegrityGoogleClient client;
  PlayDecodeResult result = client.Decode(input);
  CHECK(result.status == PlayDecodeStatus::NotConfigured);
  CHECK(result.verdict == PlayIntegrityStatus::PolicyInvalid);
  CHECK(result.evidence.version_code == 0);

  // Missing pieces of configuration are refused at Initialize.
  uint8_t certificate[1][32] = {{1}};
  PlayIntegrityGoogleConfig config;
  config.policy.certificates = certificate;
  config.policy.certificate_count = 1;
  ServiceAccountStatus credential = ServiceAccountStatus::Ok;
  CHECK(!client.Initialize(config, &credential));
  if (argc == 3) {
    // argv[1]: a valid CA bundle; argv[2]: a valid credential (0600).
    config.ca_file = argv[1];
    PlayIntegrityGoogleConfig no_certificate = config;
    no_certificate.credential_path = argv[2];
    no_certificate.policy.certificate_count = 0;
    CHECK(!client.Initialize(no_certificate, &credential));
    config.credential_path = argv[2];
    config.policy.allow_testing_response = true;
    CHECK(!client.Initialize(config, &credential));
    config.policy.allow_testing_response = false;
    // A complete configuration initializes; inputs are still checked
    // before any network activity.
    CHECK(client.Initialize(config, &credential));
    CHECK(credential == ServiceAccountStatus::Ok);
    PlayDecodeInput bad = input;
    bad.app_version_length = 0;
    CHECK(client.Decode(bad).status == PlayDecodeStatus::InputInvalid);
    CHECK(client.oauth_exchanges() == 0 && client.decode_requests() == 0);
  }
  CHECK(client.Decode(input).status == PlayDecodeStatus::NotConfigured || argc == 3);
  printf("play_integrity_google_release_test: no seam in the release config; unconfigured "
         "and misconfigured clients refuse%s\n", argc == 3 ? "; configured client checks inputs offline" : "");
  return 0;
}
