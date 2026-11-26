#pragma once

// Test-only synthetic App Attest CA and attestation minting. A fresh P-384
// root and intermediate (the Apple shape: pathlen-0 intermediate, P-256 leaf
// signed with ecdsa-with-SHA256) and a fresh leaf key per attestation, with
// knobs that break each verification step on purpose. Nothing here is linked
// into a production or guest binary.

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <string>
#include <vector>

namespace apple_fixture {

using Bytes = std::vector<uint8_t>;

inline void Require(bool condition, const char* what) {
  if (!condition) {
    fprintf(stderr, "apple fixture: %s\n", what);
    abort();
  }
}

inline Bytes Sha256(const Bytes& data) {
  Bytes out(32);
  Require(mbedtls_sha256(data.data(), data.size(), out.data(), 0) == 0, "sha256");
  return out;
}

inline Bytes Concat(const Bytes& a, const Bytes& b) {
  Bytes out = a;
  out.insert(out.end(), b.begin(), b.end());
  return out;
}

inline Bytes FromString(const std::string& text) { return Bytes(text.begin(), text.end()); }

inline std::string Base64Url(const Bytes& data) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  size_t i = 0;
  for (; i + 3 <= data.size(); i += 3) {
    const uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    for (int s = 18; s >= 0; s -= 6) out.push_back(kAlphabet[(v >> s) & 63]);
  }
  if (data.size() - i == 1) {
    const uint32_t v = data[i] << 16;
    out.push_back(kAlphabet[(v >> 18) & 63]);
    out.push_back(kAlphabet[(v >> 12) & 63]);
  } else if (data.size() - i == 2) {
    const uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
    out.push_back(kAlphabet[(v >> 18) & 63]);
    out.push_back(kAlphabet[(v >> 12) & 63]);
    out.push_back(kAlphabet[(v >> 6) & 63]);
  }
  return out;
}

inline Bytes Base64Decode(const std::string& text) {  // standard alphabet, padded
  Bytes out;
  uint32_t buffer = 0;
  int bits = 0;
  for (char c : text) {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+' || c == '-') v = 62;
    else if (c == '/' || c == '_') v = 63;
    else continue;
    buffer = (buffer << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>(buffer >> bits));
    }
  }
  return out;
}

// ---- CBOR writer ----------------------------------------------------------

inline void Head(Bytes* out, uint8_t major, uint64_t value, int force_bytes = 0) {
  const uint8_t m = static_cast<uint8_t>(major << 5);
  int size = force_bytes;
  if (!size) size = value < 24 ? 0 : value <= 0xff ? 1 : value <= 0xffff ? 2
                  : value <= 0xffffffffULL ? 4 : 8;
  if (size == 0) {
    out->push_back(static_cast<uint8_t>(m | value));
    return;
  }
  out->push_back(static_cast<uint8_t>(m | (size == 1 ? 24 : size == 2 ? 25 : size == 4 ? 26 : 27)));
  for (int i = size - 1; i >= 0; --i) out->push_back(static_cast<uint8_t>(value >> (8 * i)));
}
inline void Text(Bytes* out, const std::string& text) {
  Head(out, 3, text.size());
  out->insert(out->end(), text.begin(), text.end());
}
inline void Blob(Bytes* out, const Bytes& data, int force_bytes = 0) {
  Head(out, 2, data.size(), force_bytes);
  out->insert(out->end(), data.begin(), data.end());
}
inline void Int(Bytes* out, int64_t value) {
  if (value >= 0) Head(out, 0, static_cast<uint64_t>(value));
  else Head(out, 1, static_cast<uint64_t>(-1 - value));
}

// ---- Keys and certificates -----------------------------------------------

struct Random {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  Random() {
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    Require(mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                  reinterpret_cast<const unsigned char*>("apple-fixture"), 13) == 0,
            "drbg seed");
  }
  ~Random() {
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
  }
  Random(const Random&) = delete;
  Random& operator=(const Random&) = delete;
};

struct Key {
  mbedtls_pk_context pk;
  Key() { mbedtls_pk_init(&pk); }
  ~Key() { mbedtls_pk_free(&pk); }
  Key(const Key&) = delete;
  Key& operator=(const Key&) = delete;
  void Generate(Random& random, mbedtls_ecp_group_id curve) {
    mbedtls_pk_free(&pk);
    mbedtls_pk_init(&pk);
    Require(mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0, "pk setup");
    Require(mbedtls_ecp_gen_key(curve, mbedtls_pk_ec(pk), mbedtls_ctr_drbg_random,
                                &random.drbg) == 0, "ecp gen");
  }
  Bytes Point() const {
    unsigned char buffer[133];
    size_t length = 0;
    Require(mbedtls_ecp_write_public_key(mbedtls_pk_ec(pk), MBEDTLS_ECP_PF_UNCOMPRESSED, &length,
                                         buffer, sizeof(buffer)) == 0, "point");
    return Bytes(buffer, buffer + length);
  }
  std::string Pem() const {
    unsigned char buffer[2048];
    Require(mbedtls_pk_write_key_pem(const_cast<mbedtls_pk_context*>(&pk), buffer,
                                     sizeof(buffer)) == 0, "key pem");
    return reinterpret_cast<char*>(buffer);
  }
  void LoadPem(const std::string& pem, Random& random) {
    mbedtls_pk_free(&pk);
    mbedtls_pk_init(&pk);
    Require(mbedtls_pk_parse_key(&pk, reinterpret_cast<const unsigned char*>(pem.c_str()),
                                 pem.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random,
                                 &random.drbg) == 0, "key parse");
  }
};

inline std::string UtcStamp(int64_t seconds) {
  const time_t t = static_cast<time_t>(seconds);
  struct tm value {};
  gmtime_r(&t, &value);
  char text[16];
  strftime(text, sizeof(text), "%Y%m%d%H%M%S", &value);
  return text;
}

struct Extension {
  std::string oid;  // raw DER OID body
  bool critical = false;
  Bytes value;
};

struct CertSpec {
  std::string subject;
  std::string issuer;
  const Key* subject_key = nullptr;
  const Key* issuer_key = nullptr;
  mbedtls_md_type_t md = MBEDTLS_MD_SHA256;
  int64_t not_before = 0;
  int64_t not_after = 0;
  int basic_constraints = 0;  // 0 none, 1 end entity, 2 CA
  int path_length = -1;
  unsigned key_usage = 0;
  std::vector<std::string> extended_key_usage;  // raw DER OID bodies
  std::vector<Extension> extensions;
  uint8_t serial = 1;
};

inline Bytes WriteCertificate(Random& random, const CertSpec& spec) {
  mbedtls_x509write_cert writer;
  mbedtls_x509write_crt_init(&writer);
  mbedtls_x509write_crt_set_version(&writer, MBEDTLS_X509_CRT_VERSION_3);
  const unsigned char serial[2] = {0x01, spec.serial};
  Require(mbedtls_x509write_crt_set_serial_raw(&writer, const_cast<unsigned char*>(serial), 2) == 0,
          "serial");
  Require(mbedtls_x509write_crt_set_validity(&writer, UtcStamp(spec.not_before).c_str(),
                                             UtcStamp(spec.not_after).c_str()) == 0, "validity");
  Require(mbedtls_x509write_crt_set_subject_name(&writer, spec.subject.c_str()) == 0, "subject");
  Require(mbedtls_x509write_crt_set_issuer_name(&writer, spec.issuer.c_str()) == 0, "issuer");
  mbedtls_x509write_crt_set_subject_key(&writer, const_cast<mbedtls_pk_context*>(&spec.subject_key->pk));
  mbedtls_x509write_crt_set_issuer_key(&writer, const_cast<mbedtls_pk_context*>(&spec.issuer_key->pk));
  mbedtls_x509write_crt_set_md_alg(&writer, spec.md);
  if (spec.basic_constraints)
    Require(mbedtls_x509write_crt_set_basic_constraints(&writer, spec.basic_constraints == 2,
                                                        spec.path_length) == 0, "constraints");
  if (spec.key_usage)
    Require(mbedtls_x509write_crt_set_key_usage(&writer, spec.key_usage) == 0, "key usage");
  std::vector<mbedtls_asn1_sequence> ekus(spec.extended_key_usage.size());
  for (size_t i = 0; i < ekus.size(); ++i) {
    ekus[i].buf.tag = MBEDTLS_ASN1_OID;
    ekus[i].buf.len = spec.extended_key_usage[i].size();
    ekus[i].buf.p = reinterpret_cast<unsigned char*>(
        const_cast<char*>(spec.extended_key_usage[i].data()));
    ekus[i].next = i + 1 < ekus.size() ? &ekus[i + 1] : nullptr;
  }
  if (!ekus.empty())
    Require(mbedtls_x509write_crt_set_ext_key_usage(&writer, ekus.data()) == 0, "eku");
  for (const Extension& extension : spec.extensions)
    Require(mbedtls_x509write_crt_set_extension(&writer, extension.oid.data(), extension.oid.size(),
                                                extension.critical, extension.value.data(),
                                                extension.value.size()) == 0, "extension");
  std::vector<unsigned char> buffer(16384);
  const int length = mbedtls_x509write_crt_der(&writer, buffer.data(), buffer.size(),
                                               mbedtls_ctr_drbg_random, &random.drbg);
  mbedtls_x509write_crt_free(&writer);
  Require(length > 0, "certificate der");
  return Bytes(buffer.end() - length, buffer.end());
}

const std::string kNonceOid("\x2a\x86\x48\x86\xf7\x63\x64\x08\x02", 9);
const std::string kAppAttestEku("\x2a\x86\x48\x86\xf7\x63\x64\x04\x18", 9);
const std::string kServerAuthEku("\x2b\x06\x01\x05\x05\x07\x03\x01", 8);
constexpr char kAppId[] = "C7Z98WBK8H.io.fortisnet.wallet";

// A synthetic root and intermediate, valid one year either side of
// `now_seconds`.
struct Authority {
  Key root_key;
  Key intermediate_key;
  Bytes root_der;
  Bytes intermediate_der;

  void Create(Random& random, int64_t now_seconds) {
    root_key.Generate(random, MBEDTLS_ECP_DP_SECP384R1);
    intermediate_key.Generate(random, MBEDTLS_ECP_DP_SECP384R1);
    Rebuild(random, now_seconds);
  }
  void Rebuild(Random& random, int64_t now_seconds) {
    CertSpec root;
    root.subject = root.issuer = "CN=Fortis Synthetic App Attest Test Root,O=Fortis Test";
    root.subject_key = root.issuer_key = &root_key;
    root.md = MBEDTLS_MD_SHA384;
    root.not_before = now_seconds - 365 * 86400;
    root.not_after = now_seconds + 365 * 86400;
    root.basic_constraints = 2;
    root.key_usage = MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_CRL_SIGN;
    root_der = WriteCertificate(random, root);
    intermediate_der = WriteIntermediate(random, now_seconds, 0, MBEDTLS_X509_KU_KEY_CERT_SIGN, 2,
                                         &root_key);
  }
  Bytes WriteIntermediate(Random& random, int64_t now_seconds, int path_length, unsigned key_usage,
                          int basic_constraints, const Key* signer) const {
    CertSpec spec;
    spec.subject = "CN=Fortis Synthetic App Attest CA 1,O=Fortis Test";
    spec.issuer = "CN=Fortis Synthetic App Attest Test Root,O=Fortis Test";
    spec.subject_key = &intermediate_key;
    spec.issuer_key = signer;
    spec.md = MBEDTLS_MD_SHA384;
    spec.not_before = now_seconds - 300 * 86400;
    spec.not_after = now_seconds + 300 * 86400;
    spec.basic_constraints = basic_constraints;
    spec.path_length = path_length;
    spec.key_usage = key_usage;
    spec.serial = 2;
    return WriteCertificate(random, spec);
  }
};

// Every knob defaults to a valid attestation.
struct MintOptions {
  std::string challenge;                 // clientDataHash = SHA-256(challenge)
  bool raw_client_data = false;          // clientDataHash = challenge bytes
  std::string app_id = kAppId;
  bool development = false;
  uint32_t category = 4;
  std::string bundle_version = "57";     // in the authenticator extensions
  std::string envelope_bundle = "";      // empty: same as bundle_version
  uint32_t counter = 0;
  uint8_t flags = 0x40;
  int64_t now_seconds = 0;
  int64_t leaf_not_before = INT64_MIN;   // INT64_MIN: now - 1 day
  int64_t leaf_not_after = INT64_MIN;    // INT64_MIN: now + 2 days
  // Certificate breakage.
  int nonce_mode = 0;  // 0 ok, 1 missing, 2 wrong value, 3 wrong shape, 4 critical
  bool leaf_is_ca = false;
  bool leaf_without_eku = false;
  bool leaf_wrong_eku = false;
  bool leaf_without_digital_signature = false;
  bool leaf_p384 = false;
  bool unknown_critical_extension = false;
  bool intermediate_without_path_length = false;
  bool intermediate_not_ca = false;
  bool intermediate_from_other_root = false;
  bool leaf_signed_by_root = false;      // x5c[1] = the root itself
  bool swap_x5c = false;
  bool certificate_trailing_byte = false;
  int x5c_count = 2;
  // Binding breakage.
  bool wrong_envelope_key_id = false;
  bool wrong_credential_id = false;
  bool cose_other_key = false;
  int cose_mode = 0;   // 0 ok, 1 extra label, 2 missing y, 3 alg -35, 4 crv 2, 5 x 31 bytes
  uint16_t credential_id_length = 32;
  // CBOR breakage.
  std::string fmt = "apple-appattest";
  int extension_mode = 0;  // 0 ok, 1 missing category, 2 missing version, 3 duplicate category,
                           // 4 category as uint, 5 unknown text entry, 6 deep unknown entry,
                           // 7 nine entries, 8 version as bytes, 9 category 3 bytes, 10 none
  bool authdata_trailing_byte = false;
  bool top_trailing_byte = false;
  bool top_extra_key = false;
  bool top_duplicate_key = false;
  bool long_head = false;           // authData length with a non-shortest head
  bool indefinite_receipt = false;
  bool reorder = false;             // authData, attStmt, fmt
  bool tagged_receipt = false;
  size_t receipt_size = 3000;
  uint8_t version_byte = 1;
  const Key* leaf_key = nullptr;    // reuse an attested key (else a fresh one)
  size_t leaf_padding = 0;          // non-critical padding extension bytes
};

struct Minted {
  Bytes key_id;
  Bytes public_key;
  Bytes auth_data;
  Bytes attestation;
  Bytes envelope;
  std::string token;
  Bytes leaf_der;
};

inline Bytes Envelope(uint8_t version, const Bytes& key_id, const std::string& bundle,
                      const Bytes& attestation) {
  Bytes out{version};
  out.insert(out.end(), key_id.begin(), key_id.end());
  out.push_back(static_cast<uint8_t>(bundle.size()));
  out.insert(out.end(), bundle.begin(), bundle.end());
  const uint32_t n = static_cast<uint32_t>(attestation.size());
  out.push_back(static_cast<uint8_t>(n >> 24));
  out.push_back(static_cast<uint8_t>(n >> 16));
  out.push_back(static_cast<uint8_t>(n >> 8));
  out.push_back(static_cast<uint8_t>(n));
  out.insert(out.end(), attestation.begin(), attestation.end());
  return out;
}

inline Minted Mint(Random& random, const Authority& authority, const MintOptions& options) {
  Require(options.now_seconds > 0, "mint now");
  Minted minted;
  Key generated;
  const Key* leaf_key = options.leaf_key;
  if (!leaf_key) {
    generated.Generate(random, options.leaf_p384 ? MBEDTLS_ECP_DP_SECP384R1
                                                 : MBEDTLS_ECP_DP_SECP256R1);
    leaf_key = &generated;
  }
  minted.public_key = leaf_key->Point();
  minted.key_id = Sha256(minted.public_key);
  Bytes cose_point = minted.public_key;
  if (options.cose_other_key || options.leaf_p384) {
    Key other;
    other.Generate(random, MBEDTLS_ECP_DP_SECP256R1);
    cose_point = other.Point();
  }
  // Authenticator data.
  Bytes& a = minted.auth_data;
  const Bytes rp = Sha256(FromString(options.app_id));
  a.insert(a.end(), rp.begin(), rp.end());
  a.push_back(options.flags);
  for (int i = 3; i >= 0; --i) a.push_back(static_cast<uint8_t>(options.counter >> (8 * i)));
  const char* aaguid = options.development ? "appattestdevelop" : "appattest\0\0\0\0\0\0\0";
  a.insert(a.end(), aaguid, aaguid + 16);
  a.push_back(static_cast<uint8_t>(options.credential_id_length >> 8));
  a.push_back(static_cast<uint8_t>(options.credential_id_length));
  Bytes credential = minted.key_id;
  if (options.wrong_credential_id) credential[0] ^= 1;
  credential.resize(options.credential_id_length);
  a.insert(a.end(), credential.begin(), credential.end());
  {
    const Bytes x(cose_point.begin() + 1, cose_point.begin() + 33);
    const Bytes y(cose_point.begin() + 33, cose_point.begin() + 65);
    Head(&a, 5, options.cose_mode == 1 ? 6 : options.cose_mode == 2 ? 4 : 5);
    Int(&a, 1); Int(&a, 2);
    Int(&a, 3); Int(&a, options.cose_mode == 3 ? -35 : -7);
    Int(&a, -1); Int(&a, options.cose_mode == 4 ? 2 : 1);
    Int(&a, -2); Blob(&a, options.cose_mode == 5 ? Bytes(x.begin(), x.end() - 1) : x);
    if (options.cose_mode != 2) { Int(&a, -3); Blob(&a, y); }
    if (options.cose_mode == 1) { Int(&a, 4); Int(&a, 0); }
  }
  if (options.extension_mode != 10) {
    const int mode = options.extension_mode;
    const unsigned entries = mode == 1 || mode == 2 ? 1 : mode == 3 || mode == 5 || mode == 6 ? 3
                           : mode == 7 ? 9 : 2;
    Head(&a, 5, entries);
    if (mode != 2) {
      Text(&a, "apple_bundle_version_01");
      if (mode == 8) Blob(&a, FromString(options.bundle_version));
      else Text(&a, options.bundle_version);
    }
    if (mode != 1) {
      Text(&a, "apple_validation_category_01");
      const uint32_t c = options.category;
      if (mode == 4) Int(&a, c);
      else if (mode == 9) Blob(&a, Bytes{static_cast<uint8_t>(c), 0, 0});
      else Blob(&a, Bytes{static_cast<uint8_t>(c), static_cast<uint8_t>(c >> 8),
                          static_cast<uint8_t>(c >> 16), static_cast<uint8_t>(c >> 24)});
    }
    if (mode == 3) { Text(&a, "apple_validation_category_01"); Blob(&a, Bytes{2, 0, 0, 0}); }
    if (mode == 5) { Text(&a, "apple_future_01"); Head(&a, 4, 2); Text(&a, "x"); Head(&a, 7, 21); }
    if (mode == 6) {
      Text(&a, "apple_future_01");
      for (int i = 0; i < 6; ++i) Head(&a, 4, 1);
      Int(&a, 0);
    }
    if (mode == 7)
      for (int i = 0; i < 7; ++i) { Text(&a, "apple_pad_0" + std::to_string(i)); Int(&a, i); }
  }
  if (options.authdata_trailing_byte) a.push_back(0);
  // Nonce = SHA-256(authData || clientDataHash).
  const Bytes client_data = options.raw_client_data ? FromString(options.challenge)
                                                    : Sha256(FromString(options.challenge));
  Bytes nonce = Sha256(Concat(minted.auth_data, client_data));
  if (options.nonce_mode == 2) nonce[5] ^= 0x40;
  // Leaf certificate.
  CertSpec leaf;
  char subject[128];
  snprintf(subject, sizeof(subject), "CN=%02x%02x%02x%02x,OU=AAA Certification,O=Fortis Test",
           minted.key_id[0], minted.key_id[1], minted.key_id[2], minted.key_id[3]);
  leaf.subject = subject;
  leaf.issuer = "CN=Fortis Synthetic App Attest CA 1,O=Fortis Test";
  leaf.subject_key = leaf_key;
  leaf.issuer_key = options.leaf_signed_by_root ? &authority.root_key : &authority.intermediate_key;
  if (options.leaf_signed_by_root)
    leaf.issuer = "CN=Fortis Synthetic App Attest Test Root,O=Fortis Test";
  leaf.md = MBEDTLS_MD_SHA256;
  leaf.not_before = options.leaf_not_before != INT64_MIN ? options.leaf_not_before
                                                          : options.now_seconds - 86400;
  leaf.not_after = options.leaf_not_after != INT64_MIN ? options.leaf_not_after
                                                        : options.now_seconds + 2 * 86400;
  leaf.basic_constraints = options.leaf_is_ca ? 2 : 1;
  leaf.key_usage = options.leaf_without_digital_signature ? MBEDTLS_X509_KU_KEY_ENCIPHERMENT
      : MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_NON_REPUDIATION |
        MBEDTLS_X509_KU_KEY_ENCIPHERMENT | MBEDTLS_X509_KU_DATA_ENCIPHERMENT;
  if (options.leaf_is_ca) leaf.key_usage |= MBEDTLS_X509_KU_KEY_CERT_SIGN;
  if (!options.leaf_without_eku)
    leaf.extended_key_usage.push_back(options.leaf_wrong_eku ? kServerAuthEku : kAppAttestEku);
  if (options.nonce_mode != 1) {
    Extension nonce_extension;
    nonce_extension.oid = kNonceOid;
    nonce_extension.critical = options.nonce_mode == 4;
    nonce_extension.value = {0x30, 0x24, 0xa1, 0x22, 0x04, 0x20};
    nonce_extension.value.insert(nonce_extension.value.end(), nonce.begin(), nonce.end());
    if (options.nonce_mode == 3) nonce_extension.value[2] = 0xa2;  // [2] instead of [1]
    leaf.extensions.push_back(nonce_extension);
  }
  if (options.unknown_critical_extension) {
    Extension unknown;
    unknown.oid = std::string("\x2a\x86\x48\x86\xf7\x63\x64\x08\x63", 9);
    unknown.critical = true;
    unknown.value = {0x05, 0x00};
    leaf.extensions.push_back(unknown);
  }
  if (options.leaf_padding) {
    Extension padding;
    padding.oid = std::string("\x2a\x86\x48\x86\xf7\x63\x64\x08\x62", 9);
    padding.value = {0x04, 0x82, static_cast<uint8_t>(options.leaf_padding >> 8),
                     static_cast<uint8_t>(options.leaf_padding)};
    padding.value.resize(4 + options.leaf_padding, 0x5a);
    leaf.extensions.push_back(padding);
  }
  leaf.serial = 3;
  minted.leaf_der = WriteCertificate(random, leaf);
  Bytes intermediate = authority.intermediate_der;
  if (options.intermediate_without_path_length || options.intermediate_not_ca ||
      options.intermediate_from_other_root) {
    Key other_root;
    if (options.intermediate_from_other_root) other_root.Generate(random, MBEDTLS_ECP_DP_SECP384R1);
    intermediate = authority.WriteIntermediate(
        random, options.now_seconds, options.intermediate_without_path_length ? -1 : 0,
        MBEDTLS_X509_KU_KEY_CERT_SIGN, options.intermediate_not_ca ? 1 : 2,
        options.intermediate_from_other_root ? &other_root : &authority.root_key);
  }
  if (options.leaf_signed_by_root) intermediate = authority.root_der;
  Bytes first = minted.leaf_der, second = intermediate;
  if (options.certificate_trailing_byte) first.push_back(0);
  if (options.swap_x5c) std::swap(first, second);
  // Receipt: opaque bytes as far as attestation verification goes.
  Bytes receipt(options.receipt_size);
  for (size_t i = 0; i < receipt.size(); ++i) receipt[i] = static_cast<uint8_t>(i * 7 + 3);
  // Attestation object.
  Bytes& o = minted.attestation;
  Head(&o, 5, options.top_extra_key ? 4 : 3);
  auto write_fmt = [&] { Text(&o, "fmt"); Text(&o, options.fmt); };
  auto write_statement = [&] {
    Text(&o, "attStmt");
    Head(&o, 5, 2);
    Text(&o, "x5c");
    Head(&o, 4, options.x5c_count);
    for (int i = 0; i < options.x5c_count; ++i) Blob(&o, i == 0 ? first : i == 1 ? second : second);
    Text(&o, "receipt");
    if (options.tagged_receipt) Head(&o, 6, 24);
    if (options.indefinite_receipt) {
      o.push_back(0x5f);
      Blob(&o, receipt);
      o.push_back(0xff);
    } else {
      Blob(&o, receipt);
    }
  };
  auto write_auth = [&] {
    Text(&o, "authData");
    Blob(&o, minted.auth_data, options.long_head ? 4 : 0);
  };
  if (options.reorder) { write_auth(); write_statement(); write_fmt(); }
  else { write_fmt(); write_statement(); write_auth(); }
  if (options.top_duplicate_key) {
    o.clear();  // still three entries: fmt twice, no authData
    Head(&o, 5, 3);
    write_fmt();
    write_statement();
    write_fmt();
  }
  if (options.top_extra_key) { Text(&o, "extra"); Int(&o, 1); }
  if (options.top_trailing_byte) o.push_back(0);
  Bytes envelope_key = minted.key_id;
  if (options.wrong_envelope_key_id) envelope_key[31] ^= 1;
  const std::string bundle = options.envelope_bundle.empty() ? options.bundle_version
                                                             : options.envelope_bundle;
  minted.envelope = Envelope(options.version_byte, envelope_key, bundle, minted.attestation);
  minted.token = Base64Url(minted.envelope);
  return minted;
}

}  // namespace apple_fixture
