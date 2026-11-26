#include "test_support.h"
// Test-only CLI over tests/apple_app_attest_fixture.h for the real-socket
// App Attest test.
//
//   apple_attest_fixture_tool init DIR
//       Creates a synthetic root and intermediate: DIR/root.der (what the
//       test-root server trusts), DIR/intermediate.der and both keys.
//   apple_attest_fixture_tool mint DIR CHALLENGE [option...]
//       Prints one DeviceAttestation.token for CHALLENGE. Options:
//       --bundle B          apple_bundle_version_01 and envelope build (57)
//       --envelope-bundle B envelope build only
//       --category N        apple_validation_category_01 (4)
//       --development       development AAGUID
//       --legacy            omit iOS 27 launch extensions
//       --app-id ID         RP ID source (the production App ID)
//       --expired           leaf certificate expired a minute ago
//       --counter N         authenticator counter
//       --raw-client-data   nonce over the raw challenge, not its SHA-256
//       --save-key FILE     write the leaf key (PEM) for later --key
//       --key FILE          attest this existing leaf key
#include "apple_app_attest_fixture.h"

#include <fstream>
#include <sstream>

namespace {

using apple_fixture::Bytes;

void WriteFile(const std::string& path, const std::string& data) {
  std::ofstream file(path, std::ios::binary);
  file << data;
  apple_fixture::Require(static_cast<bool>(file), "write file");
}

std::string ReadFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  apple_fixture::Require(static_cast<bool>(file), "read file");
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

std::string AsString(const Bytes& bytes) { return std::string(bytes.begin(), bytes.end()); }
Bytes AsBytes(const std::string& text) { return Bytes(text.begin(), text.end()); }

int Usage() {
  fprintf(stderr, "usage: apple_attest_fixture_tool init DIR | mint DIR CHALLENGE [options]\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc < 3) return Usage();
  const std::string command = argv[1], directory = argv[2];
  apple_fixture::Random random;
  const int64_t now = static_cast<int64_t>(time(nullptr));
  if (command == "init" && argc == 3) {
    apple_fixture::Authority authority;
    authority.Create(random, now);
    WriteFile(directory + "/root.der", AsString(authority.root_der));
    WriteFile(directory + "/intermediate.der", AsString(authority.intermediate_der));
    WriteFile(directory + "/root_key.pem", authority.root_key.Pem());
    WriteFile(directory + "/intermediate_key.pem", authority.intermediate_key.Pem());
    return 0;
  }
  if (command != "mint" || argc < 4) return Usage();
  apple_fixture::Authority authority;
  authority.root_key.LoadPem(ReadFile(directory + "/root_key.pem"), random);
  authority.intermediate_key.LoadPem(ReadFile(directory + "/intermediate_key.pem"), random);
  authority.root_der = AsBytes(ReadFile(directory + "/root.der"));
  authority.intermediate_der = AsBytes(ReadFile(directory + "/intermediate.der"));
  apple_fixture::MintOptions options;
  options.challenge = argv[3];
  options.now_seconds = now;
  std::string save_key;
  apple_fixture::Key existing;
  for (int i = 4; i < argc; ++i) {
    const std::string flag = argv[i];
    const bool has_value = i + 1 < argc;
    if (flag == "--bundle" && has_value) options.bundle_version = argv[++i];
    else if (flag == "--envelope-bundle" && has_value) options.envelope_bundle = argv[++i];
    else if (flag == "--category" && has_value) options.category = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 10));
    else if (flag == "--development") options.development = true;
    else if (flag == "--legacy") options.extension_mode = 10;
    else if (flag == "--app-id" && has_value) options.app_id = argv[++i];
    else if (flag == "--expired") { options.leaf_not_before = now - 86400; options.leaf_not_after = now - 60; }
    else if (flag == "--counter" && has_value) options.counter = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 10));
    else if (flag == "--raw-client-data") options.raw_client_data = true;
    else if (flag == "--save-key" && has_value) save_key = argv[++i];
    else if (flag == "--key" && has_value) {
      existing.LoadPem(ReadFile(argv[++i]), random);
      options.leaf_key = &existing;
    } else return Usage();
  }
  apple_fixture::Key fresh;
  if (!options.leaf_key && !save_key.empty()) {
    fresh.Generate(random, MBEDTLS_ECP_DP_SECP256R1);
    options.leaf_key = &fresh;
  }
  if (!save_key.empty()) WriteFile(save_key, options.leaf_key->Pem());
  const apple_fixture::Minted minted = apple_fixture::Mint(random, authority, options);
  printf("%s\n", minted.token.c_str());
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
