#pragma once
#include "https_listener_internal.h"
#include "unit_test_support.h"
#include <mbedtls/ecp.h>
#include <filesystem>
#include <fstream>

namespace fortis::testing::https {
using testing::Require;
namespace detail = app::https_detail;
namespace fs = std::filesystem;
struct Environment {
  std::string name, previous;
  bool existed;
  Environment(const char* key, const std::string& value) : name(key), existed(getenv(key) != nullptr) {
    if (existed) previous = getenv(key);
    Require(setenv(key, value.c_str(), 1) == 0, "set isolated test environment");
  }
  ~Environment() { if (existed) setenv(name.c_str(), previous.c_str(), 1); else unsetenv(name.c_str()); }
  Environment(const Environment&) = delete;
  Environment& operator=(const Environment&) = delete;
};
struct Key {
  mbedtls_pk_context value{};
  Key() { mbedtls_pk_init(&value); }
  ~Key() { mbedtls_pk_free(&value); }
  Key(const Key&) = delete;
  Key& operator=(const Key&) = delete;
  void Generate() {
    Require(mbedtls_pk_setup(&value, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0, "setup fixture key");
    Require(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(value), detail::random_bytes, nullptr) == 0, "generate ephemeral EC key");
  }
  void Save(const fs::path& path) const {
    std::array<unsigned char, 4096> pem{};
    Require(mbedtls_pk_write_key_pem(&value, pem.data(), pem.size()) == 0, "encode fixture key");
    std::ofstream out(path, std::ios::binary); out << reinterpret_cast<char*>(pem.data());
    Require(out.good(), "save fixture key"); out.close();
    Require(chmod(path.c_str(), 0600) == 0, "protect fixture key");
    mbedtls_platform_zeroize(pem.data(), pem.size());
  }
};
struct Writer {
  mbedtls_x509write_cert value{};
  Writer() { mbedtls_x509write_crt_init(&value); }
  ~Writer() { mbedtls_x509write_crt_free(&value); }
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
};
/// Ephemeral CA/leaves use the production crypto dependency; no external CLI,
/// network, checked-in secret, expiring fixture, or provider credential needed.
struct Certificates {
  TemporaryDirectory directory;
  fs::path root{directory.path()};
  Key ca, domain, ip;
  Environment trust{"FORTIS_CA_BUNDLE_PATH", (root / "ca.pem").string()};
  Certificates() {
    ca.Generate(); domain.Generate(); ip.Generate();
    fs::create_directory(root / "generation");
    fs::create_directory_symlink("generation", root / "current");
    Write("ca.pem", ca, true, false);
    Write("domain.pem", domain, false, false);
    Write("ip.pem", ip, false, true);
    domain.Save(root / "generation/domain.key"); ip.Save(root / "generation/ip.key");
  }
  void Write(const char* name, Key& key, bool is_ca, bool is_ip,
             const char* before = "20200101000000", const char* after = "20990101000000", bool client_only = false) {
    Writer writer;
    auto* crt = &writer.value;
    mbedtls_x509write_crt_set_version(crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(crt, &key.value);
    mbedtls_x509write_crt_set_issuer_key(crt, &ca.value);
    unsigned char serial = is_ca ? 1 : is_ip ? 2 : 3;
    Require(mbedtls_x509write_crt_set_serial_raw(crt, &serial, 1) == 0, "certificate serial");
    Require(mbedtls_x509write_crt_set_subject_name(crt, is_ca ? "CN=Fortis Unit CA" : "CN=localhost") == 0, "certificate subject");
    Require(mbedtls_x509write_crt_set_issuer_name(crt, "CN=Fortis Unit CA") == 0, "certificate issuer");
    Require(mbedtls_x509write_crt_set_validity(crt, before, after) == 0, "certificate validity");
    Require(mbedtls_x509write_crt_set_basic_constraints(crt, is_ca, is_ca ? 0 : -1) == 0, "CA constraint");
    Require(mbedtls_x509write_crt_set_key_usage(crt, is_ca ? MBEDTLS_X509_KU_KEY_CERT_SIGN : MBEDTLS_X509_KU_DIGITAL_SIGNATURE) == 0, "certificate key usage");
    if (!is_ca) {
      unsigned char dns[] = "localhost", address[] = {127, 0, 0, 1};
      mbedtls_x509_san_list san{};
      san.node.type = is_ip ? MBEDTLS_X509_SAN_IP_ADDRESS : MBEDTLS_X509_SAN_DNS_NAME;
      san.node.san.unstructured_name.p = is_ip ? address : dns;
      san.node.san.unstructured_name.len = is_ip ? sizeof(address) : sizeof(dns) - 1;
      Require(mbedtls_x509write_crt_set_subject_alternative_name(crt, &san) == 0, "certificate SAN");
      unsigned char server_oid[] = MBEDTLS_OID_SERVER_AUTH, client_oid[] = MBEDTLS_OID_CLIENT_AUTH;
      mbedtls_asn1_sequence usage{};
      usage.buf.tag = MBEDTLS_ASN1_OID;
      usage.buf.p = client_only ? client_oid : server_oid;
      usage.buf.len = client_only ? sizeof(client_oid) - 1 : sizeof(server_oid) - 1;
      Require(mbedtls_x509write_crt_set_ext_key_usage(crt, &usage) == 0, "server authentication usage");
    }
    std::array<unsigned char, 4096> pem{};
    Require(mbedtls_x509write_crt_pem(crt, pem.data(), pem.size(), detail::random_bytes, nullptr) == 0, "sign ephemeral certificate");
    std::ofstream out(is_ca ? root / name : root / "generation" / name, std::ios::binary);
    out << reinterpret_cast<char*>(pem.data()); Require(out.good(), "save certificate");
  }
  std::shared_ptr<detail::TlsConfiguration> Load() const {
    auto result = std::make_shared<detail::TlsConfiguration>();
    Require(result->Load(root.string(), "localhost", "127.0.0.1"), "load valid independent domain/IP certificates");
    return result;
  }
};
struct Client {
  mbedtls_ssl_config config{};
  mbedtls_ssl_context ssl{};
  mbedtls_x509_crt trust{};
  int fd = -1;
  Client() { mbedtls_ssl_config_init(&config); mbedtls_ssl_init(&ssl); mbedtls_x509_crt_init(&trust); }
  ~Client() { mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&config); mbedtls_x509_crt_free(&trust); if (fd >= 0) close(fd); }
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  void Start(int socket, const fs::path& ca, const char* hostname, mbedtls_ssl_protocol_version version) {
    fd = socket; Require(detail::nonblocking(fd), "nonblocking client");
    Require(mbedtls_x509_crt_parse_file(&trust, ca.c_str()) == 0, "client trust CA");
    Require(mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) == 0, "client TLS defaults");
    mbedtls_ssl_conf_rng(&config, detail::random_bytes, nullptr);
    mbedtls_ssl_conf_ca_chain(&config, &trust, nullptr);
    mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_min_tls_version(&config, version); mbedtls_ssl_conf_max_tls_version(&config, version);
    static const char* protocols[] = {"http/1.1", nullptr};
    Require(mbedtls_ssl_conf_alpn_protocols(&config, protocols) == 0 && mbedtls_ssl_setup(&ssl, &config) == 0, "client ALPN/context");
    Require(mbedtls_ssl_set_hostname(&ssl, hostname) == 0, "verified client hostname");
    mbedtls_ssl_set_bio(&ssl, &fd, Send, Receive, nullptr);
  }
  static int Send(void* opaque, const unsigned char* bytes, size_t size) {
    const auto n = send(*static_cast<int*>(opaque), bytes, size, 0);
    return n >= 0 ? static_cast<int>(n) : errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
  static int Receive(void* opaque, unsigned char* bytes, size_t size) {
    const auto n = recv(*static_cast<int*>(opaque), bytes, size, 0);
    return n >= 0 ? static_cast<int>(n) : errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
  }
};
}  // namespace fortis::testing::https
