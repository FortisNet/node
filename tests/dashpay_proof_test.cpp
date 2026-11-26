#include "test_support.h"
// Replays tests/corpus/dashpay_proof_corpus.json through the
// native DashPay proof verifier. Every expectation in the corpus comes from
// the official Dash Platform v4.1.1 verifier (tests/dashpay_oracle), except
// the forgeries the official verifier accepts — those must be rejected —
// and the gRPC-web transport cases, which follow gRPC semantics.
#include "chain_core.h"
#include "dashpay_proof.h"
#include "token_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace dp = fortis::exchange::dashpay;
namespace tj = fortis::codec::tj;

namespace {

int g_failures = 0;

void fail(const char* id, const char* message) {
  fprintf(stderr, "FAIL %s: %s\n", id, message);
  ++g_failures;
}

bool read_file(const char* path, uint8_t** data, size_t* length) {
  FILE* file = fopen(path, "rb");
  if (!file) return false;
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  *data = static_cast<uint8_t*>(malloc(size > 0 ? static_cast<size_t>(size) : 1));
  const bool ok = *data && fread(*data, 1, static_cast<size_t>(size), file) == static_cast<size_t>(size);
  fclose(file);
  *length = static_cast<size_t>(size);
  return ok;
}

bool hex_bytes(const tj::Value* text, uint8_t* out, size_t expected) {
  if (!tj::IsString(text) || text->length != expected * 2) return false;
  for (size_t i = 0; i < expected; ++i) {
    unsigned value = 0;
    if (fortis::test::ParseHexByte(text->text + 2 * i, &value) != 1) return false;
    out[i] = static_cast<uint8_t>(value);
  }
  return true;
}

void hex_text(const uint8_t* bytes, size_t length, char* out) {
  for (size_t i = 0; i < length; ++i) snprintf(out + 2 * i, 3, "%02x", bytes[i]);
}

struct Quorum final : dp::QuorumKeys {
  bool known = false;
  uint8_t hash[dp::kHashBytes]{};
  uint8_t key[dp::kPublicKeyBytes]{};

  bool PublicKey(uint32_t, const uint8_t quorum_hash[dp::kHashBytes], uint8_t public_key[dp::kPublicKeyBytes]) override {
    if (!known || memcmp(hash, quorum_hash, dp::kHashBytes) != 0) return false;
    memcpy(public_key, key, dp::kPublicKeyBytes);
    return true;
  }
};

void run_case(const tj::Value* entry, size_t* checked, size_t* verified) {
  const tj::Value* id_value = tj::Get(entry, "id");
  const char* id = tj::IsString(id_value) ? id_value->text : "?";
  const tj::Value* label = tj::Get(entry, "label");
  const tj::Value* body64 = tj::Get(entry, "body_b64");
  const tj::Value* expect = tj::Get(entry, "expect");
  if (!tj::IsString(label) || !tj::IsString(body64) || !tj::IsObject(expect)) return fail(id, "malformed case");
  const bool expect_ok = tj::Get(expect, "ok") && tj::Get(expect, "ok")->type == tj::Type::kTrue;
  Quorum quorum;
  quorum.known = hex_bytes(tj::Get(entry, "quorum_hash_hex"), quorum.hash, dp::kHashBytes) &&
                 hex_bytes(tj::Get(entry, "quorum_key_hex"), quorum.key, dp::kPublicKeyBytes);
  const size_t capacity = body64->length;
  uint8_t* body = static_cast<uint8_t*>(malloc(capacity + 1));
  size_t body_length = 0;
  if (!body || !fortis::chains::Base64Decode(body64->text, body64->length, body, capacity, &body_length)) {
    free(body);
    return fail(id, "body base64");
  }
  dp::DocumentsProof proof;
  dp::Resolution resolution;
  dp::Failure failure = dp::ParseGrpcWebDocumentsResponse(body, body_length, &proof);
  if (failure == dp::Failure::kNone)
    failure = dp::VerifyDpnsName(label->text, label->length, proof, &quorum, &resolution);
  ++*checked;
  const bool ok = failure == dp::Failure::kNone;
  if (ok != expect_ok) {
    char message[160];
    snprintf(message, sizeof(message), "native %s (%s), expected %s", ok ? "accepted" : "rejected",
             dp::FailureName(failure), expect_ok ? "acceptance" : "rejection");
    fail(id, message);
  } else if (ok) {
    ++*verified;
    char root[65];
    hex_text(resolution.root_hash, dp::kHashBytes, root);
    const tj::Value* expected_root = tj::Get(expect, "root_hash");
    if (!tj::IsString(expected_root) || strcmp(expected_root->text, root) != 0) fail(id, "root hash differs");
    const tj::Value* expected_identity = tj::Get(expect, "identity_id");
    char identity[64] = "";
    if (resolution.found && !fortis::chains::Base58Encode(resolution.owner_id, dp::kHashBytes, identity, sizeof(identity)))
      fail(id, "base58");
    if (resolution.found != tj::IsString(expected_identity) ||
        (resolution.found && strcmp(identity, expected_identity->text) != 0))
      fail(id, "identity differs");
  }
  free(body);
}

void request_checks() {
  // The exact frame tests/dashpay_capture.py sends (live evonodes answered it).
  static const char kDash[] =
      "0000000071126f0a20e668c659af66aee1e72c186dde7b5b7e0a1d712a09c40d5721f622bf53c531551206646f6d61696e1a190a0f6e"
      "6f726d616c697a65644c6162656c1a062a04646173681a240a1a6e6f726d616c697a6564506172656e74446f6d61696e4e616d651a062a"
      "046461736840014a00";
  uint8_t frame[512];
  const size_t length = dp::BuildGetDocumentsRequest("dash", 4, frame, sizeof(frame));
  char text[1024];
  hex_text(frame, length, text);
  if (length * 2 != strlen(kDash) || strcmp(text, kDash) != 0) fail("request/dash", "request bytes differ");
  char longest[64];
  memset(longest, 'a', 63);
  if (dp::BuildGetDocumentsRequest(longest, 63, frame, sizeof(frame)) != 178) fail("request/63", "length");
  if (dp::BuildGetDocumentsRequest(longest, 63, frame, 177) != 0) fail("request/capacity", "accepted");
  static const char* const kInvalid[] = {"ab", "-ab", "ab-", "Dash", "da.sh", "da sh", ""};
  for (const char* label : kInvalid)
    if (dp::BuildGetDocumentsRequest(label, strlen(label), frame, sizeof(frame)) != 0) fail("request/invalid", label);
  if (dp::BuildGetDocumentsRequest(longest, 64, frame, sizeof(frame)) != 0) fail("request/64", "accepted");
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc != 2) {
    fprintf(stderr, "usage: %s dashpay_proof_corpus.json\n", argv[0]);
    return 2;
  }
  uint8_t* text = nullptr;
  size_t length = 0;
  if (!read_file(argv[1], &text, &length)) {
    fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  tj::Arena arena = tj::GrowingArena(64 * 1024 * 1024);
  const tj::Value* corpus = tj::Parse(arena, text, length);
  const tj::Value* cases = corpus ? tj::Get(corpus, "cases") : nullptr;
  if (!tj::IsArray(cases)) {
    fprintf(stderr, "malformed corpus\n");
    return 2;
  }
  size_t checked = 0, verified = 0;
  for (size_t i = 0; i < cases->count; ++i) run_case(cases->items[i], &checked, &verified);
  request_checks();
  tj::FreeArena(arena);
  free(text);
  printf("dashpay_proof_test: %zu cases (%zu verified), %d failures\n", checked, verified, g_failures);
  return g_failures ? 1 : 0;
} catch (...) {
  return fortis::test::ReportException();
}
