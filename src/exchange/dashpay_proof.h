#pragma once

#include <stddef.h>
#include <stdint.h>

// Proof verification for GET /v1/dashpay/names/resolve: a native port of the
// chain the TypeScript route runs through the official Evo SDK in trusted
// mode (dashpay/platform v4.1.1 drive-proof-verifier, rs-drive, rs-dpp,
// grovedb v5.0.1, rs-tenderdash-abci v1.5.1) for exactly one query: the
// DPNS `domain` document whose normalizedParentDomainName is "dash" and
// whose normalizedLabel is the given label.
//
//   gRPC-web getDocuments reply ─ proof + metadata
//     GroveDB V1 proof ─ Merk layers along
//       [@, DPNS id, 0x01, "domain", "normalizedParentDomainName", "dash",
//        "normalizedLabel"] / label / [0]
//     → GroveDB root hash and the referenced document Item (or a proven
//       absence)
//     → DPNS domain document (owner id = the Platform identity)
//   Tenderdash commit: StateId(app_hash = root) → CanonicalVote sign bytes →
//     sign digest → BLS12-381 Basic-scheme signature under the quorum public
//     key the caller supplies (the trusted quorum list).
//
// Everything here is pure and bounded: no I/O, no clock, no global state.
// The verifier is deliberately stricter than grovedb v5.0.1 where honest
// proofs are unaffected; each rule below closes a forgery that the official
// verifier accepts (each reproduced against it):
//   - a Merk Hash node is an opaque subtree and may not have children
//     (grovedb hashes it while ignoring attached children, so a fake key and
//     document hung under it verify — a forged identity);
//   - only left-to-right (non-inverted) ops are accepted, so the push order
//     is the in-order traversal the absence logic assumes (inverted ops let a
//     boundary key be visited first — a forged absence);
//   - an empty tree on the path must carry value_hash ==
//     combine_hash(H(value), NULL_HASH) (grovedb skips that check there, so a
//     non-empty subtree can be presented as empty — a forged absence).
namespace fortis::exchange::dashpay {

constexpr size_t kHashBytes = 32;
constexpr size_t kPublicKeyBytes = 48;
constexpr size_t kSignatureBytes = 96;
// normalizeDashPayName's label: 3..63 ASCII letters, digits and hyphens.
constexpr size_t kMaxLabelBytes = 63;
// The DPNS data contract (GWRSAVFMjXx8HpQFaNJMqBV7MBgMK4br5UESsB4S31Ec).
extern const uint8_t kDpnsContractId[kHashBytes];

enum class Failure : uint8_t {
  kNone,
  kInvalidLabel,
  kResponseFraming,    // gRPC-web frames or a non-zero grpc-status
  kResponseDecode,     // protobuf
  kNoProof,            // a documents (unproved) result or no result
  kNoMetadata,
  kProofEnvelope,      // the GroveDB bincode envelope
  kMerkDecode,         // an unsupported or malformed Merk op
  kMerkStructure,      // stack machine, key order, height, AVL, Hash leaf
  kQueryMismatch,      // missing data, unproven absence, extra layers
  kLayerHash,          // a lower layer that does not chain to its parent
  kElement,            // an element of an unexpected type or encoding
  kDocument,           // the DPNS domain document
  kQuorumHash,         // not 32 bytes
  kQuorumType,         // larger than 255
  kQuorumUnknown,      // not in the trusted quorum list
  kPublicKey,
  kSignatureEncoding,
  kSignature,
  kCommit,             // sign bytes could not be formed (block id size, round)
};
const char* FailureName(Failure failure);

// A decoded getDocuments reply; every span points into the reply buffer.
struct DocumentsProof {
  const uint8_t* grovedb_proof = nullptr;
  size_t grovedb_proof_length = 0;
  const uint8_t* quorum_hash = nullptr;
  size_t quorum_hash_length = 0;
  const uint8_t* signature = nullptr;
  size_t signature_length = 0;
  uint32_t round = 0;
  const uint8_t* block_id_hash = nullptr;
  size_t block_id_hash_length = 0;
  uint32_t quorum_type = 0;
  // ResponseMetadata
  uint64_t height = 0;
  uint32_t core_chain_locked_height = 0;
  uint32_t epoch = 0;
  uint64_t time_ms = 0;
  uint32_t protocol_version = 0;
  const char* chain_id = nullptr;
  size_t chain_id_length = 0;
};

// Parses a gRPC-web (application/grpc-web+proto) GetDocumentsResponse body:
// one uncompressed data frame and optional trailer frames whose grpc-status
// must be 0; a V0 or V1 response whose result is a proof, with metadata.
Failure ParseGrpcWebDocumentsResponse(const uint8_t* body, size_t length, DocumentsProof* out);

// Writes the gRPC-web framed GetDocumentsRequest V1 the SDK sends for the
// label: DPNS contract, "domain", normalizedLabel == label and
// normalizedParentDomainName == "dash", prove, one DOCUMENTS select. Returns
// the length, or 0 when the label is invalid or the buffer too small.
size_t BuildGetDocumentsRequest(const char* label, size_t label_length, uint8_t* out, size_t capacity);

// Supplies the trusted public key of a quorum, or false when it is unknown
// (the base knows no quorum).
class QuorumKeys {
 public:
  virtual bool PublicKey(uint32_t quorum_type, const uint8_t quorum_hash[kHashBytes],
                         uint8_t public_key[kPublicKeyBytes]) {
    (void)quorum_type; (void)quorum_hash; (void)public_key;
    return false;
  }

 protected:
  constexpr QuorumKeys() = default;
  ~QuorumKeys() = default;
};

struct Resolution {
  bool found = false;
  uint8_t owner_id[kHashBytes]{};
  uint8_t document_id[kHashBytes]{};
  uint8_t root_hash[kHashBytes]{};
};

// The GroveDB part alone: verifies the proof against the path query for
// `label` and returns the root hash and the result. Exposed for the corpus
// test (the official verifier's DriveDocumentQuery::verify_proof).
Failure VerifyDpnsGroveProof(const char* label, size_t label_length, const uint8_t* proof, size_t proof_length,
                             Resolution* out);

// The Tenderdash part alone: verifies that `root_hash` is the app hash the
// quorum signed for this metadata (verify_tenderdash_proof).
Failure VerifyTenderdashCommit(const DocumentsProof& proof, const uint8_t root_hash[kHashBytes],
                               QuorumKeys* quorum_keys);

// The whole chain: GroveDB proof, then the commit signature.
Failure VerifyDpnsName(const char* label, size_t label_length, const DocumentsProof& proof,
                       QuorumKeys* quorum_keys, Resolution* out);

}  // namespace fortis::exchange::dashpay
