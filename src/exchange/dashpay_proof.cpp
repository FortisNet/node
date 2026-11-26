#include <algorithm>
#include "ownership.h"
#include "dashpay_proof.h"

#include "proto_wire.h"

#include <blake3.h>
#include <blst.h>
#include <mbedtls/sha256.h>

#include <stdlib.h>
#include <string.h>

namespace fortis::exchange::dashpay {

const uint8_t kDpnsContractId[kHashBytes] = {230, 104, 198, 89,  175, 102, 174, 225, 231, 44, 24,
                                             109, 222, 123, 91,  126, 10,  29,  113, 42,  9,  196,
                                             13,  87,  33,  246, 34,  191, 83,  197, 49,  85};

const char* FailureName(Failure failure) {
  switch (failure) {
    case Failure::kNone: return "none";
    case Failure::kInvalidLabel: return "invalid_label";
    case Failure::kResponseFraming: return "response_framing";
    case Failure::kResponseDecode: return "response_decode";
    case Failure::kNoProof: return "no_proof";
    case Failure::kNoMetadata: return "no_metadata";
    case Failure::kProofEnvelope: return "proof_envelope";
    case Failure::kMerkDecode: return "merk_decode";
    case Failure::kMerkStructure: return "merk_structure";
    case Failure::kQueryMismatch: return "query_mismatch";
    case Failure::kLayerHash: return "layer_hash";
    case Failure::kElement: return "element";
    case Failure::kDocument: return "document";
    case Failure::kQuorumHash: return "quorum_hash";
    case Failure::kQuorumType: return "quorum_type";
    case Failure::kQuorumUnknown: return "quorum_unknown";
    case Failure::kPublicKey: return "public_key";
    case Failure::kSignatureEncoding: return "signature_encoding";
    case Failure::kSignature: return "signature";
    case Failure::kCommit: return "commit";
  }
  return "unknown";
}

namespace {

constexpr uint8_t kNullHash[kHashBytes] = {};
constexpr char kBlsDst[] = "BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_NUL_";
// grovedb merk proofs/tree.rs.
constexpr size_t kMaxProofOps = 50000;
constexpr size_t kMaxStackDepth = 10000;
constexpr size_t kMaxTreeHeight = 92;
// grovedb operations/proof MAX_PROOF_DEPTH: layer nesting and children per layer.
constexpr size_t kMaxProofDepth = 128;
// Native bound on the envelope's total layer count (a DPNS proof has nine).
constexpr size_t kMaxLayers = 512;
// The GroveDB path of the parentNameAndLabel index, below the root tree.
constexpr size_t kIndexPathLength = 7;
constexpr size_t kLeafDepth = kIndexPathLength + 1;  // the label subtree's [0] key

// ---- bytes ---------------------------------------------------------------------------------

struct Cursor {
  const uint8_t* data = nullptr;
  size_t length = 0;
  size_t offset = 0;

  size_t Remaining() const { return length - offset; }
  bool Take(size_t count, const uint8_t** out) {
    if (count > Remaining()) return false;
    *out = data + offset;
    offset += count;
    return true;
  }
  bool Byte(uint8_t* out) {
    if (!Remaining()) return false;
    *out = data[offset++];
    return true;
  }
  bool BigEndian(size_t count, uint64_t* out) {
    const uint8_t* bytes = nullptr;
    if (!Take(count, &bytes)) return false;
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i) value = value << 8 | bytes[i];
    *out = value;
    return true;
  }
};

bool equal_bytes(const uint8_t* a, size_t a_length, const uint8_t* b, size_t b_length) {
  return a_length == b_length && (a_length == 0 || memcmp(a, b, a_length) == 0);
}

// Lexicographic byte order (Vec<u8> Ord).
int compare_bytes(const uint8_t* a, size_t a_length, const uint8_t* b, size_t b_length) {
  const size_t common = a_length < b_length ? a_length : b_length;
  const int head = common ? memcmp(a, b, common) : 0;
  if (head) return head < 0 ? -1 : 1;
  return a_length == b_length ? 0 : a_length < b_length ? -1 : 1;
}

// bincode 2 varint (standard config, big endian): <251 is the value itself,
// 251/252/253 prefix a u16/u32/u64. `max_bytes` is the decoded integer width.
bool bincode_varint(Cursor& c, size_t max_bytes, uint64_t* out) {
  uint8_t first = 0;
  if (!c.Byte(&first)) return false;
  if (first < 251) {
    *out = first;
    return true;
  }
  const size_t width = first == 251 ? 2 : first == 252 ? 4 : first == 253 ? 8 : 0;
  if (!width || width > max_bytes) return false;
  return c.BigEndian(width, out);
}

bool bincode_bytes(Cursor& c, const uint8_t** bytes, size_t* length) {
  uint64_t count = 0;
  if (!bincode_varint(c, 8, &count) || count > c.Remaining()) return false;
  *length = static_cast<size_t>(count);
  return c.Take(*length, bytes);
}

// integer-encoding LEB128 (the ed/Merk and document encodings). A
// non-minimal encoding or one beyond 64 bits is rejected.
bool leb128(Cursor& c, uint64_t* out) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 10; ++i) {
    uint8_t byte = 0;
    if (!c.Byte(&byte)) return false;
    if (i == 9 && (byte & 0x7e)) return false;
    value |= static_cast<uint64_t>(byte & 0x7f) << (7 * i);
    if (!(byte & 0x80)) {
      if (i && byte == 0) return false;
      *out = value;
      return true;
    }
  }
  return false;
}

bool leb128_signed(Cursor& c, int64_t* out) {
  uint64_t raw = 0;
  if (!leb128(c, &raw)) return false;
  *out = static_cast<int64_t>((raw >> 1) ^ (~(raw & 1) + 1));
  return true;
}

size_t encode_leb128(uint64_t value, uint8_t out[10]) {
  size_t used = 0;
  do {
    uint8_t byte = value & 0x7f;
    value >>= 7;
    out[used++] = value ? (byte | 0x80) : byte;
  } while (value);
  return used;
}

// Strict UTF-8 (String::from_utf8).
bool valid_utf8(const uint8_t* s, size_t n) {
  size_t i = 0;
  while (i < n) {
    const uint8_t b = s[i];
    if (b < 0x80) { ++i; continue; }
    size_t extra = 0;
    uint32_t cp = 0;
    if (b >= 0xc2 && b <= 0xdf) { extra = 1; cp = b & 0x1f; }
    else if (b >= 0xe0 && b <= 0xef) { extra = 2; cp = b & 0x0f; }
    else if (b >= 0xf0 && b <= 0xf4) { extra = 3; cp = b & 0x07; }
    else return false;
    if (n - i <= extra) return false;
    for (size_t k = 1; k <= extra; ++k) {
      if ((s[i + k] & 0xc0) != 0x80) return false;
      cp = cp << 6 | (s[i + k] & 0x3f);
    }
    if ((extra == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) ||
        (extra == 3 && (cp < 0x10000 || cp > 0x10ffff)))
      return false;
    i += extra + 1;
  }
  return true;
}

// ---- hashes --------------------------------------------------------------------------------

struct Blake3 {
  blake3_hasher hasher;
  Blake3() { blake3_hasher_init(&hasher); }
  Blake3& Add(const void* bytes, size_t length) {
    if (length) blake3_hasher_update(&hasher, bytes, length);
    return *this;
  }
  void Finish(uint8_t out[kHashBytes]) { blake3_hasher_finalize(&hasher, out, kHashBytes); }
};

void add_be64(Blake3& h, uint64_t value) {
  uint8_t bytes[8];
  for (int i = 7; i >= 0; --i) { bytes[i] = value & 0xff; value >>= 8; }
  h.Add(bytes, 8);
}

// merk tree/hash.rs.
void value_hash(const uint8_t* value, size_t length, uint8_t out[kHashBytes]) {
  uint8_t prefix[10];
  Blake3 h;
  h.Add(prefix, encode_leb128(length, prefix)).Add(value, length).Finish(out);
}

void kv_digest_to_kv_hash(const uint8_t* key, size_t length, const uint8_t digest[kHashBytes],
                          uint8_t out[kHashBytes]) {
  uint8_t prefix[10];
  Blake3 h;
  h.Add(prefix, encode_leb128(length, prefix)).Add(key, length).Add(digest, kHashBytes).Finish(out);
}

void combine_hash(const uint8_t a[kHashBytes], const uint8_t b[kHashBytes], uint8_t out[kHashBytes]) {
  Blake3 h;
  h.Add(a, kHashBytes).Add(b, kHashBytes).Finish(out);
}

void sha256(const uint8_t* bytes, size_t length, uint8_t out[32]) { mbedtls_sha256(bytes, length, out, 0); }

// ---- Merk proofs ---------------------------------------------------------------------------

enum class NodeKind : uint8_t {
  kHash,
  kKVHash,
  kKV,
  kKVValueHash,
  kKVDigest,
  kKVRefValueHash,
  kKVValueHashFeatureType,
  kKVValueHashFeatureTypeWithChildHash,
};

// TreeFeatureType: 0 basic, 1 summed, 2 big summed, 3 counted, 4 counted
// summed, 5 provable counted, 6 provable counted summed, 7 provable summed,
// 8 provable counted and provable summed.
struct Feature {
  uint8_t type = 0;
  uint64_t count = 0;
  int64_t sum = 0;
};

struct Node {
  NodeKind kind = NodeKind::kHash;
  const uint8_t* key = nullptr;
  size_t key_length = 0;
  const uint8_t* value = nullptr;
  size_t value_length = 0;
  // Hash / KVHash: the hash. KVValueHash family, KVDigest: the value hash.
  // KVRefValueHash: the reference's own value hash.
  uint8_t hash[kHashBytes]{};
  Feature feature;
  uint8_t child_hash[kHashBytes]{};
};

bool key_bearing(NodeKind kind) { return kind != NodeKind::kHash && kind != NodeKind::kKVHash; }
bool has_value(NodeKind kind) { return key_bearing(kind) && kind != NodeKind::kKVDigest; }
bool value_hash_family(NodeKind kind) {
  return kind == NodeKind::kKVValueHash || kind == NodeKind::kKVValueHashFeatureType ||
         kind == NodeKind::kKVValueHashFeatureTypeWithChildHash;
}

bool decode_feature(Cursor& c, Feature* out) {
  uint8_t type = 0;
  if (!c.Byte(&type) || type > 8) return false;
  out->type = type;
  switch (type) {
    case 0: return true;
    case 1: case 7: return leb128_signed(c, &out->sum);
    case 2: {
      const uint8_t* ignored = nullptr;  // i128, never part of a node hash
      return c.Take(16, &ignored);
    }
    case 3: case 5: return leb128(c, &out->count);
    default: return leb128(c, &out->count) && leb128_signed(c, &out->sum);
  }
}

enum class OpKind : uint8_t { kPush, kParent, kChild };

// grovedb-query proofs/encoding.rs, left-to-right subset. Inverted ops,
// the count/sum node variants and the u32-length variants are rejected:
// a DPNS lookup is a left-to-right query over plain Merk trees, and a reply
// is at most 64 KiB, so no value needs a 32-bit length.
bool decode_op(Cursor& c, OpKind* op, Node* node) {
  uint8_t tag = 0;
  if (!c.Byte(&tag)) return false;
  if (tag == 0x10 || tag == 0x11) {
    *op = tag == 0x10 ? OpKind::kParent : OpKind::kChild;
    return true;
  }
  *op = OpKind::kPush;
  *node = Node{};
  const uint8_t* bytes = nullptr;
  auto read_hash = [&](uint8_t out[kHashBytes]) {
    if (!c.Take(kHashBytes, &bytes)) return false;
    memcpy(out, bytes, kHashBytes);
    return true;
  };
  auto read_key = [&]() {
    uint8_t length = 0;
    if (!c.Byte(&length)) return false;
    node->key_length = length;
    return c.Take(length, &node->key);
  };
  auto read_value = [&]() {
    uint64_t length = 0;
    if (!c.BigEndian(2, &length)) return false;
    node->value_length = static_cast<size_t>(length);
    return c.Take(node->value_length, &node->value);
  };
  switch (tag) {
    case 0x01: node->kind = NodeKind::kHash; return read_hash(node->hash);
    case 0x02: node->kind = NodeKind::kKVHash; return read_hash(node->hash);
    case 0x03: node->kind = NodeKind::kKV; return read_key() && read_value();
    case 0x04: node->kind = NodeKind::kKVValueHash; return read_key() && read_value() && read_hash(node->hash);
    case 0x05: node->kind = NodeKind::kKVDigest; return read_key() && read_hash(node->hash);
    case 0x06: node->kind = NodeKind::kKVRefValueHash; return read_key() && read_value() && read_hash(node->hash);
    case 0x07:
      node->kind = NodeKind::kKVValueHashFeatureType;
      return read_key() && read_value() && read_hash(node->hash) && decode_feature(c, &node->feature);
    case 0x1c:
      node->kind = NodeKind::kKVValueHashFeatureTypeWithChildHash;
      return read_key() && read_value() && read_hash(node->hash) && decode_feature(c, &node->feature) &&
             read_hash(node->child_hash);
    default: return false;
  }
}

// grovedb-element ElementType::from_serialized_value: false when the leading
// discriminant is not a legal element type; `simple` when its value hash is
// plain H(value) (items).
bool classify_element(const uint8_t* value, size_t length, bool* simple) {
  if (!length) return false;
  const uint8_t first = value[0];
  if (first == 15) {  // NonCounted wrapper
    if (length < 2) return false;
    const uint8_t inner = value[1];
    if (!(inner <= 14 || inner == 18 || inner == 19 || inner == 20)) return false;
    *simple = inner == 0 || inner == 3 || inner == 9;
    return true;
  }
  if (first == 16 || first == 17) {  // NotSummed / NotCountedOrSummed wrappers
    if (length < 2) return false;
    const uint8_t inner = value[1];
    *simple = false;
    return inner == 4 || inner == 5 || inner == 7 || inner == 10 || inner == 19 || inner == 20;
  }
  const bool legal = first <= 14 || first == 18 || first == 19 || first == 20 || (first >= 128 && first <= 142) ||
                     first == 146 || first == 147 || first == 148 || first == 177 || first == 178 ||
                     first == 180 || first == 181 || first == 183 || first == 186 || first == 193 ||
                     first == 194 || first == 196 || first == 197 || first == 199 || first == 202;
  if (!legal) return false;
  *simple = first == 0 || first == 3 || first == 9 || first == 128 || first == 131 || first == 137;
  return true;
}

struct Entry {
  Node node;
  bool has_left = false;
  bool has_right = false;
  uint8_t left[kHashBytes]{};
  uint8_t right[kHashBytes]{};
  size_t height = 1;
  size_t left_height = 0;
  size_t right_height = 0;
};

// proofs/tree.rs Tree::hash, with collapsed children.
void entry_hash(const Entry& e, uint8_t out[kHashBytes]) {
  const Node& n = e.node;
  if (n.kind == NodeKind::kHash) {
    memcpy(out, n.hash, kHashBytes);
    return;
  }
  uint8_t kv[kHashBytes];
  switch (n.kind) {
    case NodeKind::kKVHash: memcpy(kv, n.hash, kHashBytes); break;
    case NodeKind::kKV: {
      uint8_t vh[kHashBytes];
      value_hash(n.value, n.value_length, vh);
      kv_digest_to_kv_hash(n.key, n.key_length, vh, kv);
      break;
    }
    case NodeKind::kKVRefValueHash: {
      uint8_t referenced[kHashBytes], combined[kHashBytes];
      value_hash(n.value, n.value_length, referenced);
      combine_hash(n.hash, referenced, combined);
      kv_digest_to_kv_hash(n.key, n.key_length, combined, kv);
      break;
    }
    default: kv_digest_to_kv_hash(n.key, n.key_length, n.hash, kv); break;
  }
  Blake3 h;
  h.Add(kv, kHashBytes).Add(e.has_left ? e.left : kNullHash, kHashBytes).Add(e.has_right ? e.right : kNullHash,
                                                                              kHashBytes);
  if (value_hash_family(n.kind)) {
    // node_hash_with_count / _with_sum / _with_count_and_sum for provable trees.
    switch (n.feature.type) {
      case 5: case 6: add_be64(h, n.feature.count); break;
      case 7: add_be64(h, static_cast<uint64_t>(n.feature.sum)); break;
      case 8: add_be64(h, n.feature.count); add_be64(h, static_cast<uint64_t>(n.feature.sum)); break;
      default: break;
    }
  }
  h.Finish(out);
}

// Tree::attach after proofs::execute's collapse: the child is first turned
// into a fresh Hash tree (`child.into_hash()`, height 1), so every attached
// child has height 1. Hardening: a Hash node is an opaque subtree and never
// a parent — grovedb hashes a Hash node while ignoring attached children.
bool attach(Entry& parent, bool left, const Entry& child) {
  constexpr size_t kCollapsedChildHeight = 1;
  if (parent.node.kind == NodeKind::kHash) return false;
  if (left ? parent.has_left : parent.has_right) return false;
  const size_t height =
      parent.height > kCollapsedChildHeight + 1 ? parent.height : kCollapsedChildHeight + 1;
  if (height > kMaxTreeHeight) return false;
  parent.height = height;
  if (left) {
    parent.left_height = kCollapsedChildHeight;
    entry_hash(child, parent.left);
    parent.has_left = true;
  } else {
    parent.right_height = kCollapsedChildHeight;
    entry_hash(child, parent.right);
    parent.has_right = true;
  }
  return true;
}

struct KeyProof {
  uint8_t root[kHashBytes]{};
  bool found = false;
  Node node;                         // the matching node when found
  uint8_t proved_hash[kHashBytes]{};  // its value hash as the query saw it
  bool child_hash_verified = false;
};

// Query::execute_proof for one QueryItem::Key, left to right, no limit,
// PROOF_VERSION_LATEST (strict), over a collapsing proofs::execute.
Failure execute_key_proof(const uint8_t* bytes, size_t length, const uint8_t* key, size_t key_length,
                          KeyProof* out) {
  *out = KeyProof{};
  if (!length) return Failure::kMerkStructure;
  // A Push op is at least four bytes (a KV with an empty key and value).
  size_t capacity = length / 4 + 1;
  if (capacity > kMaxStackDepth) capacity = kMaxStackDepth;
  auto stack_storage = base::AllocateStorage<Entry[]>(capacity * sizeof(Entry));
  auto* stack = stack_storage.get();
  if (!stack) return Failure::kMerkStructure;
  Failure failure = Failure::kNone;
  size_t depth = 0, ops = 0;
  const uint8_t* last_key = nullptr;
  size_t last_key_length = 0;
  bool have_last_key = false, pending = true, any_push = false;
  NodeKind last_push = NodeKind::kHash;
  Cursor c{bytes, length, 0};
  while (c.Remaining()) {
    if (++ops > kMaxProofOps) { failure = Failure::kMerkStructure; break; }
    OpKind op = OpKind::kPush;
    Node node;
    if (!decode_op(c, &op, &node)) { failure = Failure::kMerkDecode; break; }
    if (op != OpKind::kPush) {
      if (depth < 2) { failure = Failure::kMerkStructure; break; }
      Entry& top = stack[depth - 1];
      Entry& below = stack[depth - 2];
      bool ok = false;
      if (op == OpKind::kParent) {
        // (parent, child) = (pop, pop): the top becomes the parent of the entry below.
        Entry parent = top;
        ok = attach(parent, true, below);
        if (ok) below = parent;
      } else {
        // (child, parent) = (pop, pop): the top becomes the right child.
        ok = attach(below, false, top);
      }
      if (!ok) { failure = Failure::kMerkStructure; break; }
      --depth;
      continue;
    }
    if (key_bearing(node.kind)) {
      if (have_last_key && compare_bytes(node.key, node.key_length, last_key, last_key_length) <= 0) {
        failure = Failure::kMerkStructure;  // "Incorrect key ordering"
        break;
      }
      last_key = node.key;
      last_key_length = node.key_length;
      have_last_key = true;
    }
    // visit_node: the strict V1 element check first.
    if (value_hash_family(node.kind)) {
      bool simple = false;
      if (!classify_element(node.value, node.value_length, &simple) || simple) {
        failure = Failure::kMerkStructure;
        break;
      }
    }
    bool child_hash_verified = false;
    if (node.kind == NodeKind::kKVValueHashFeatureTypeWithChildHash) {
      uint8_t element[kHashBytes], computed[kHashBytes];
      value_hash(node.value, node.value_length, element);
      combine_hash(element, node.child_hash, computed);
      if (memcmp(computed, node.hash, kHashBytes) != 0) { failure = Failure::kMerkStructure; break; }
      child_hash_verified = true;
    }
    if (key_bearing(node.kind) && pending) {
      const int order = compare_bytes(key, key_length, node.key, node.key_length);
      if (order <= 0) {
        // The first node at or past the key: its predecessor must prove the
        // lower bound unless it is the key itself.
        if (order != 0 && any_push && !key_bearing(last_push)) { failure = Failure::kQueryMismatch; break; }
        pending = false;
        if (order == 0) {
          if (!has_value(node.kind)) { failure = Failure::kQueryMismatch; break; }
          out->found = true;
          out->node = node;
          out->child_hash_verified = child_hash_verified;
          if (node.kind == NodeKind::kKV) value_hash(node.value, node.value_length, out->proved_hash);
          else memcpy(out->proved_hash, node.hash, kHashBytes);
        }
      }
    }
    last_push = node.kind;
    any_push = true;
    if (depth >= capacity) { failure = Failure::kMerkStructure; break; }
    stack[depth] = Entry{};
    stack[depth].node = node;
    ++depth;
  }
  if (failure == Failure::kNone && depth != 1) failure = Failure::kMerkStructure;
  if (failure == Failure::kNone) {
    const Entry& root = stack[0];
    const size_t high = root.left_height > root.right_height ? root.left_height : root.right_height;
    const size_t low = root.left_height > root.right_height ? root.right_height : root.left_height;
    if (high - low > 1) failure = Failure::kMerkStructure;  // "a valid avl tree"
  }
  // An unconsumed key needs the right-most node to be a key-bearing bound.
  if (failure == Failure::kNone && pending && (!any_push || !key_bearing(last_push)))
    failure = Failure::kQueryMismatch;
  if (failure == Failure::kNone) entry_hash(stack[0], out->root);
  stack_storage.reset();
  return failure;
}

// ---- GroveDB envelope ----------------------------------------------------------------------

struct Layer {
  uint32_t variant = 0;  // ProofBytes: 0 Merk, 1 MMR, 2 BulkAppendTree, 3 DenseTree, 4 CommitmentTree
  const uint8_t* merk = nullptr;
  size_t merk_length = 0;
  const uint8_t* key = nullptr;  // the key in the parent layer
  size_t key_length = 0;
  long parent = -1;
};

struct Layers {
  std::unique_ptr<Layer[], base::StorageDeleter> items;
  size_t count = 0;
};

// LayerProof::decode_with_depth.
bool parse_layer(Cursor& c, Layers& layers, long parent, const uint8_t* key, size_t key_length, size_t depth) {
  if (depth > kMaxProofDepth || layers.count >= kMaxLayers) return false;
  uint64_t variant = 0;
  if (!bincode_varint(c, 4, &variant) || variant > 4) return false;
  const long index = static_cast<long>(layers.count++);
  Layer& layer = layers.items[index];
  layer.variant = static_cast<uint32_t>(variant);
  layer.parent = parent;
  layer.key = key;
  layer.key_length = key_length;
  if (!bincode_bytes(c, &layer.merk, &layer.merk_length)) return false;
  uint64_t children = 0;
  if (!bincode_varint(c, 8, &children) || children > kMaxProofDepth) return false;
  const size_t first_child = layers.count;
  for (uint64_t i = 0; i < children; ++i) {
    const uint8_t* child_key = nullptr;
    size_t child_key_length = 0;
    if (!bincode_bytes(c, &child_key, &child_key_length)) return false;
    // BTreeMap keys: a duplicate is never produced by an honest prover.
    for (size_t j = first_child; j < layers.count; ++j)
      if (layers.items[j].parent == index &&
          equal_bytes(layers.items[j].key, layers.items[j].key_length, child_key, child_key_length))
        return false;
    if (!parse_layer(c, layers, index, child_key, child_key_length, depth + 1)) return false;
  }
  return true;
}

// Element::deserialize for the two element types a DPNS lookup crosses:
// Item(Vec<u8>, Option<flags>) and Tree(Option<root key>, Option<flags>).
struct Element {
  uint32_t variant = 0;
  const uint8_t* item = nullptr;
  size_t item_length = 0;
  bool has_root_key = false;
};

bool parse_option_bytes(Cursor& c, bool* present) {
  uint8_t tag = 0;
  if (!c.Byte(&tag) || tag > 1) return false;
  *present = tag == 1;
  if (!*present) return true;
  const uint8_t* bytes = nullptr;
  size_t length = 0;
  return bincode_bytes(c, &bytes, &length);
}

bool parse_element(const uint8_t* value, size_t length, Element* out) {
  Cursor c{value, length, 0};
  uint64_t variant = 0;
  if (!bincode_varint(c, 4, &variant)) return false;
  *out = Element{};
  out->variant = static_cast<uint32_t>(variant);
  bool flags = false;
  if (variant == 0) {
    if (!bincode_bytes(c, &out->item, &out->item_length) || !parse_option_bytes(c, &flags)) return false;
  } else if (variant == 2) {
    if (!parse_option_bytes(c, &out->has_root_key) || !parse_option_bytes(c, &flags)) return false;
  } else {
    return false;
  }
  return c.Remaining() == 0;
}

// ---- the DPNS domain document ----------------------------------------------------------------

struct DomainDocument {
  uint8_t id[kHashBytes]{};
  uint8_t owner_id[kHashBytes]{};
  const uint8_t* normalized_label = nullptr;
  size_t normalized_label_length = 0;
  const uint8_t* normalized_parent = nullptr;
  size_t normalized_parent_length = 0;
};

// DocumentPropertyType::read_optionally_from's presence marker. Returns false
// on an error; `*read` false means no value follows.
bool property_marker(Cursor& c, bool required, bool* finished, bool* read) {
  *read = false;
  if (*finished) return !required;  // "required field after finished buffer"
  if (required) {
    *read = true;
    return true;
  }
  uint8_t marker = 0;
  if (!c.Byte(&marker)) {
    *finished = true;
    return true;
  }
  *read = marker != 0;
  return true;
}

bool read_string(Cursor& c, const uint8_t** text, size_t* length) {
  uint64_t count = 0;
  if (!leb128(c, &count) || count > c.Remaining()) return false;
  *length = static_cast<size_t>(count);
  return c.Take(*length, text) && valid_utf8(*text, *length);
}

// Document::from_bytes for the DPNS `domain` type (contract v1/v2 schema:
// mutable no, transferable yes, trade mode direct purchase; properties in
// position order label, normalizedLabel, parentDomainName,
// normalizedParentDomainName, preorderSalt (transient), records{identity},
// subdomainRules{allowSubdomains}).
bool parse_domain_document(const uint8_t* bytes, size_t length, DomainDocument* out) {
  Cursor c{bytes, length, 0};
  uint64_t version = 0;
  if (!leb128(c, &version) || version > 2 || c.Remaining() < 64) return false;
  const uint8_t* span = nullptr;
  if (!c.Take(kHashBytes, &span)) return false;
  memcpy(out->id, span, kHashBytes);
  if (!c.Take(kHashBytes, &span)) return false;
  memcpy(out->owner_id, span, kHashBytes);
  uint8_t flag = 0;
  if (version == 2) {  // $creatorId (trade mode or transferable)
    if (!c.Byte(&flag) || (flag && !c.Take(kHashBytes, &span))) return false;
  }
  uint64_t scratch = 0;
  if (!leb128(c, &scratch)) return false;  // $revision (transferable documents)
  uint64_t timestamps = 0;
  if (!c.BigEndian(2, &timestamps)) return false;
  for (unsigned bit = 0; bit < 9; ++bit)
    if ((timestamps >> bit & 1) && !c.BigEndian(bit < 6 ? 8 : 4, &scratch)) return false;
  if (!c.Byte(&flag) || (flag && !c.BigEndian(8, &scratch))) return false;  // $price
  bool finished = false, read = false;
  const uint8_t* text = nullptr;
  size_t text_length = 0;
  // label, normalizedLabel (required strings)
  if (!property_marker(c, true, &finished, &read) || (read && !read_string(c, &text, &text_length))) return false;
  if (!property_marker(c, true, &finished, &read) ||
      (read && !read_string(c, &out->normalized_label, &out->normalized_label_length)))
    return false;
  // parentDomainName (optional)
  if (!property_marker(c, false, &finished, &read) || (read && !read_string(c, &text, &text_length))) return false;
  // normalizedParentDomainName (required)
  if (!property_marker(c, true, &finished, &read) ||
      (read && !read_string(c, &out->normalized_parent, &out->normalized_parent_length)))
    return false;
  // preorderSalt: required but transient, so optional; a 32-byte array.
  if (!property_marker(c, false, &finished, &read) || (read && !c.Take(kHashBytes, &span))) return false;
  // records {identity: Identifier, optional} and subdomainRules
  // {allowSubdomains: bool, required}: required objects, each a
  // length-prefixed sub-buffer.
  for (int object = 0; object < 2; ++object) {
    if (!property_marker(c, true, &finished, &read)) return false;
    if (!read) continue;
    uint64_t object_length = 0;
    const uint8_t* object_bytes = nullptr;
    if (!leb128(c, &object_length) || object_length > c.Remaining() ||
        !c.Take(static_cast<size_t>(object_length), &object_bytes))
      return false;
    Cursor inner{object_bytes, static_cast<size_t>(object_length), 0};
    bool inner_finished = false, inner_read = false;
    if (object == 0) {
      if (!property_marker(inner, false, &inner_finished, &inner_read) ||
          (inner_read && !inner.Take(kHashBytes, &span)))
        return false;
    } else {
      if (!property_marker(inner, true, &inner_finished, &inner_read) || (inner_read && !inner.Byte(&flag)))
        return false;
    }
  }
  return out->normalized_label && out->normalized_parent;
}

// ---- GroveDB path query walk ---------------------------------------------------------------

struct Walk {
  const Layers* layers = nullptr;
  const uint8_t* label = nullptr;
  size_t label_length = 0;
  bool found = false;
  const uint8_t* document = nullptr;
  size_t document_length = 0;
};

const uint8_t kRootKey[] = {0x40};
const uint8_t kDocumentsKey[] = {0x01};
const uint8_t kPrimaryKey[] = {0x00};

void path_key(const Walk& w, size_t depth, const uint8_t** key, size_t* length) {
  static const char* const kNames[] = {"domain", "normalizedParentDomainName", "dash", "normalizedLabel"};
  switch (depth) {
    case 0: *key = kRootKey; *length = 1; return;
    case 1: *key = kDpnsContractId; *length = kHashBytes; return;
    case 2: *key = kDocumentsKey; *length = 1; return;
    case kIndexPathLength: *key = w.label; *length = w.label_length; return;
    case kLeafDepth: *key = kPrimaryKey; *length = 1; return;
    default:
      *key = reinterpret_cast<const uint8_t*>(kNames[depth - 3]);
      *length = strlen(kNames[depth - 3]);
      return;
  }
}

// GroveDb::verify_layer_proof_v1 specialized to the path query
//   path [@, DPNS, 0x01, "domain", "normalizedParentDomainName", "dash",
//         "normalizedLabel"], items [Key(label)], default subquery path [[0]],
//   conditional Key("") -> [[0]] / RangeFull (never matches a 3+ byte label)
// with verify_proof_succinctness and no limit. Every layer queries one key;
// a proven absence anywhere on the path is a proven absence of the name.
Failure walk_layer(Walk& w, long index, size_t depth, uint8_t root[kHashBytes]) {
  const Layer& layer = w.layers->items[index];
  if (layer.variant != 0) return Failure::kProofEnvelope;  // "Expected Merk proof at this layer"
  const uint8_t* key = nullptr;
  size_t key_length = 0;
  path_key(w, depth, &key, &key_length);
  KeyProof proof;
  const Failure merk = execute_key_proof(layer.merk, layer.merk_length, key, key_length, &proof);
  if (merk != Failure::kNone) return merk;
  memcpy(root, proof.root, kHashBytes);
  // Succinctness: a lower layer is allowed only under the proven key.
  long child = -1;
  for (size_t i = 0; i < w.layers->count; ++i) {
    const Layer& candidate = w.layers->items[i];
    if (candidate.parent != index) continue;
    if (!proof.found || !equal_bytes(candidate.key, candidate.key_length, key, key_length))
      return Failure::kQueryMismatch;
    child = static_cast<long>(i);
  }
  if (!proof.found) return Failure::kNone;  // proven absent
  Element element;
  if (!parse_element(proof.node.value, proof.node.value_length, &element)) return Failure::kElement;
  if (depth == kLeafDepth) {
    // The document reference: KVRefValueHash carries the referenced Item; a
    // plain KV item is accepted as grovedb does.
    if (element.variant != 0 || child >= 0 ||
        (proof.node.kind != NodeKind::kKVRefValueHash && proof.node.kind != NodeKind::kKV))
      return Failure::kElement;
    w.found = true;
    w.document = element.item;
    w.document_length = element.item_length;
    return Failure::kNone;
  }
  // A subtree on the path, carried by a KVValueHash-family node.
  if (element.variant != 2 || !value_hash_family(proof.node.kind)) return Failure::kElement;
  uint8_t element_hash[kHashBytes], expected[kHashBytes];
  value_hash(proof.node.value, proof.node.value_length, element_hash);
  if (!element.has_root_key) {
    // An empty tree: hardening — its value hash must commit to NULL_HASH.
    if (child >= 0) return Failure::kQueryMismatch;
    combine_hash(element_hash, kNullHash, expected);
    return memcmp(expected, proof.proved_hash, kHashBytes) == 0 ? Failure::kNone : Failure::kLayerHash;
  }
  if (child < 0) return Failure::kQueryMismatch;  // "missing lower layer for non-empty tree"
  uint8_t lower[kHashBytes];
  const Failure below = walk_layer(w, child, depth + 1, lower);
  if (below != Failure::kNone) return below;
  combine_hash(element_hash, lower, expected);
  return memcmp(expected, proof.proved_hash, kHashBytes) == 0 ? Failure::kNone : Failure::kLayerHash;
}

bool valid_label(const char* label, size_t length) {
  if (length < 3 || length > kMaxLabelBytes) return false;
  for (size_t i = 0; i < length; ++i) {
    const char ch = label[i];
    const bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
    if (!alnum && !(ch == '-' && i != 0 && i + 1 != length)) return false;
  }
  return true;
}

// ---- protobuf ------------------------------------------------------------------------------

bool pb_varint(Cursor& c, uint64_t* out) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 10; ++i) {
    uint8_t byte = 0;
    if (!c.Byte(&byte)) return false;
    if (i == 9 && byte > 1) return false;
    value |= static_cast<uint64_t>(byte & 0x7f) << (7 * i);
    if (!(byte & 0x80)) {
      *out = value;
      return true;
    }
  }
  return false;
}

bool pb_skip(Cursor& c, uint32_t wire) {
  uint64_t scratch = 0;
  const uint8_t* bytes = nullptr;
  switch (wire) {
    case 0: return pb_varint(c, &scratch);
    case 1: return c.Take(8, &bytes);
    case 2: return pb_varint(c, &scratch) && scratch <= c.Remaining() && c.Take(static_cast<size_t>(scratch), &bytes);
    case 5: return c.Take(4, &bytes);
    default: return false;
  }
}

bool pb_field(Cursor& c, uint32_t* number, uint32_t* wire) {
  uint64_t key = 0;
  if (!pb_varint(c, &key) || key > 0xffffffffULL) return false;
  *number = static_cast<uint32_t>(key >> 3);
  *wire = static_cast<uint32_t>(key & 7);
  return *number != 0;
}

bool pb_bytes(Cursor& c, const uint8_t** bytes, size_t* length) {
  uint64_t count = 0;
  if (!pb_varint(c, &count) || count > c.Remaining()) return false;
  *length = static_cast<size_t>(count);
  return c.Take(*length, bytes);
}

// prost merges repeated occurrences of a message field; scalars take the last.
bool parse_proof_message(const uint8_t* bytes, size_t length, DocumentsProof* out) {
  Cursor c{bytes, length, 0};
  while (c.Remaining()) {
    uint32_t number = 0, wire = 0;
    if (!pb_field(c, &number, &wire)) return false;
    uint64_t scalar = 0;
    switch (number) {
      case 1: if (wire != 2 || !pb_bytes(c, &out->grovedb_proof, &out->grovedb_proof_length)) return false; break;
      case 2: if (wire != 2 || !pb_bytes(c, &out->quorum_hash, &out->quorum_hash_length)) return false; break;
      case 3: if (wire != 2 || !pb_bytes(c, &out->signature, &out->signature_length)) return false; break;
      case 4: if (wire != 0 || !pb_varint(c, &scalar)) return false; out->round = static_cast<uint32_t>(scalar); break;
      case 5: if (wire != 2 || !pb_bytes(c, &out->block_id_hash, &out->block_id_hash_length)) return false; break;
      case 6:
        if (wire != 0 || !pb_varint(c, &scalar)) return false;
        out->quorum_type = static_cast<uint32_t>(scalar);
        break;
      default: if (!pb_skip(c, wire)) return false; break;
    }
  }
  return true;
}

bool parse_metadata_message(const uint8_t* bytes, size_t length, DocumentsProof* out) {
  Cursor c{bytes, length, 0};
  while (c.Remaining()) {
    uint32_t number = 0, wire = 0;
    if (!pb_field(c, &number, &wire)) return false;
    uint64_t scalar = 0;
    if (number >= 1 && number <= 5) {
      if (wire != 0 || !pb_varint(c, &scalar)) return false;
      switch (number) {
        case 1: out->height = scalar; break;
        case 2: out->core_chain_locked_height = static_cast<uint32_t>(scalar); break;
        case 3: out->epoch = static_cast<uint32_t>(scalar); break;
        case 4: out->time_ms = scalar; break;
        default: out->protocol_version = static_cast<uint32_t>(scalar); break;
      }
    } else if (number == 6) {
      const uint8_t* text = nullptr;
      size_t text_length = 0;
      if (wire != 2 || !pb_bytes(c, &text, &text_length) || !valid_utf8(text, text_length)) return false;
      out->chain_id = reinterpret_cast<const char*>(text);
      out->chain_id_length = text_length;
    } else if (!pb_skip(c, wire)) {
      return false;
    }
  }
  return true;
}

// GetDocumentsResponse{ oneof version { V0 v0 = 1; V1 v1 = 2; } }, each
// version { oneof result { <rows> = 1; Proof proof = 2; } ResponseMetadata metadata = 3; }.
Failure parse_documents_response(const uint8_t* bytes, size_t length, DocumentsProof* out) {
  Cursor c{bytes, length, 0};
  uint32_t version = 0;
  enum class Result : uint8_t { kNone, kRows, kProof } result = Result::kNone;
  bool metadata = false;
  DocumentsProof parsed;
  while (c.Remaining()) {
    uint32_t number = 0, wire = 0;
    if (!pb_field(c, &number, &wire)) return Failure::kResponseDecode;
    if (number != 1 && number != 2) {
      if (!pb_skip(c, wire)) return Failure::kResponseDecode;
      continue;
    }
    const uint8_t* body = nullptr;
    size_t body_length = 0;
    if (wire != 2 || !pb_bytes(c, &body, &body_length)) return Failure::kResponseDecode;
    if (number != version) {  // a different oneof variant replaces the version
      version = number;
      result = Result::kNone;
      metadata = false;
      parsed = DocumentsProof{};
    }
    Cursor inner{body, body_length, 0};
    while (inner.Remaining()) {
      uint32_t field = 0, field_wire = 0;
      if (!pb_field(inner, &field, &field_wire)) return Failure::kResponseDecode;
      if (field < 1 || field > 3) {
        if (!pb_skip(inner, field_wire)) return Failure::kResponseDecode;
        continue;
      }
      const uint8_t* message = nullptr;
      size_t message_length = 0;
      if (field_wire != 2 || !pb_bytes(inner, &message, &message_length)) return Failure::kResponseDecode;
      if (field == 1) {
        result = Result::kRows;
      } else if (field == 2) {
        if (result != Result::kProof) {
          const DocumentsProof keep = parsed;
          parsed = DocumentsProof{};
          // Keep the metadata already merged.
          parsed.height = keep.height;
          parsed.core_chain_locked_height = keep.core_chain_locked_height;
          parsed.epoch = keep.epoch;
          parsed.time_ms = keep.time_ms;
          parsed.protocol_version = keep.protocol_version;
          parsed.chain_id = keep.chain_id;
          parsed.chain_id_length = keep.chain_id_length;
        }
        result = Result::kProof;
        if (!parse_proof_message(message, message_length, &parsed)) return Failure::kResponseDecode;
      } else {
        metadata = true;
        if (!parse_metadata_message(message, message_length, &parsed)) return Failure::kResponseDecode;
      }
    }
  }
  if (result != Result::kProof) return Failure::kNoProof;
  if (!metadata) return Failure::kNoMetadata;
  *out = parsed;
  return Failure::kNone;
}

// ---- Tenderdash ----------------------------------------------------------------------------

void put_le(uint8_t* out, uint64_t value, size_t width) {
  for (size_t i = 0; i < width; ++i) { out[i] = value & 0xff; value >>= 8; }
}

}  // namespace

Failure ParseGrpcWebDocumentsResponse(const uint8_t* body, size_t length, DocumentsProof* out) {
  Cursor c{body, length, 0};
  const uint8_t* message = nullptr;
  size_t message_length = 0;
  bool have_message = false;
  while (c.Remaining()) {
    uint8_t flag = 0;
    uint64_t frame_length = 0;
    const uint8_t* frame = nullptr;
    if (!c.Byte(&flag) || !c.BigEndian(4, &frame_length) || frame_length > c.Remaining() ||
        !c.Take(static_cast<size_t>(frame_length), &frame))
      return Failure::kResponseFraming;
    if (flag & 0x80) {
      // Trailers: "name:value\r\n" lines; grpc-status must be 0 when present.
      size_t at = 0;
      while (at < frame_length) {
        size_t end = at;
        while (end < frame_length && frame[end] != '\n') ++end;
        size_t line_end = end;
        if (line_end > at && frame[line_end - 1] == '\r') --line_end;
        const char kStatus[] = "grpc-status:";
        const size_t prefix = sizeof(kStatus) - 1;
        if (line_end - at >= prefix) {
          bool match = true;
          for (size_t i = 0; i < prefix && match; ++i) {
            char ch = static_cast<char>(frame[at + i]);
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            match = ch == kStatus[i];
          }
          if (match) {
            size_t v = at + prefix;
            while (v < line_end && (frame[v] == ' ' || frame[v] == '\t')) ++v;
            size_t v_end = line_end;
            while (v_end > v && (frame[v_end - 1] == ' ' || frame[v_end - 1] == '\t')) --v_end;
            if (v_end - v != 1 || frame[v] != '0') return Failure::kResponseFraming;
          }
        }
        at = end + 1;
      }
      continue;
    }
    if (flag != 0 || have_message) return Failure::kResponseFraming;  // compressed or a second message
    message = frame;
    message_length = static_cast<size_t>(frame_length);
    have_message = true;
  }
  if (!have_message) return Failure::kResponseFraming;
  return parse_documents_response(message, message_length, out);
}

size_t BuildGetDocumentsRequest(const char* label, size_t label_length, uint8_t* out, size_t capacity) {
  namespace pw = fortis::codec::proto_wire;
  if (!valid_label(label, label_length)) return 0;
  // Every message here is < 512 bytes; each nested one is built in its own
  // bounded span and then copied in as a length-delimited field.
  uint8_t v1_bytes[512]{}, value_bytes[512]{}, clause_bytes[512]{}, request_bytes[512]{};
  pw::Span v1{v1_bytes, sizeof(v1_bytes)};
  pw::PutBytes(v1, 1, kDpnsContractId, kHashBytes);  // data_contract_id
  pw::PutBytes(v1, 2, "domain", 6);                  // document_type
  // where_clauses: {field, operator EQUAL (0, omitted), value {text = 5}}
  const char* const fields[2] = {"normalizedLabel", "normalizedParentDomainName"};
  const char* const texts[2] = {label, "dash"};
  const size_t text_lengths[2] = {label_length, 4};
  for (size_t i = 0; i < 2; ++i) {
    pw::Span value{value_bytes, sizeof(value_bytes)}, clause{clause_bytes, sizeof(clause_bytes)};
    pw::PutBytes(value, 5, texts[i], text_lengths[i]);
    pw::PutBytes(clause, 1, fields[i], strlen(fields[i]));
    pw::PutBytes(clause, 3, value.data, value.used);
    if (!value.ok || !clause.ok) return 0;
    pw::PutBytes(v1, 3, clause.data, clause.used);
  }
  pw::PutVarintField(v1, 8, 1);    // prove
  pw::PutBytes(v1, 9, nullptr, 0);  // selects: one Select{function: DOCUMENTS} (all defaults)
  pw::Span request{request_bytes, sizeof(request_bytes)};
  pw::PutBytes(request, 2, v1.data, v1.used);  // GetDocumentsRequest.v1
  if (!v1.ok || !request.ok || 5 + request.used > capacity) return 0;
  out[0] = 0;  // uncompressed data frame
  out[1] = static_cast<uint8_t>(request.used >> 24);
  out[2] = static_cast<uint8_t>(request.used >> 16);
  out[3] = static_cast<uint8_t>(request.used >> 8);
  out[4] = static_cast<uint8_t>(request.used);
  memcpy(out + 5, request.data, request.used);
  return 5 + request.used;
}

Failure VerifyDpnsGroveProof(const char* label, size_t label_length, const uint8_t* proof, size_t proof_length,
                             Resolution* out) {
  *out = Resolution{};
  if (!valid_label(label, label_length)) return Failure::kInvalidLabel;
  Cursor c{proof, proof_length, 0};
  uint64_t envelope = 0;
  // GroveDBProof::V1 only: V0 embeds prover-chosen options and is not what
  // the current Platform version emits.
  if (!bincode_varint(c, 4, &envelope) || envelope != 1) return Failure::kProofEnvelope;
  Layers layers;
  layers.items = base::AllocateZeroedStorage<Layer[]>(kMaxLayers, sizeof(Layer));
  if (!layers.items) return Failure::kProofEnvelope;
  Failure failure = Failure::kNone;
  if (!parse_layer(c, layers, -1, nullptr, 0, 0) || c.Remaining() != 0) failure = Failure::kProofEnvelope;
  Walk walk;
  walk.layers = &layers;
  walk.label = reinterpret_cast<const uint8_t*>(label);
  walk.label_length = label_length;
  if (failure == Failure::kNone) failure = walk_layer(walk, 0, 0, out->root_hash);
  if (failure == Failure::kNone && walk.found) {
    DomainDocument document;
    if (!parse_domain_document(walk.document, walk.document_length, &document)) {
      failure = Failure::kDocument;
    } else if (!equal_bytes(document.normalized_label, document.normalized_label_length,
                            reinterpret_cast<const uint8_t*>(label), label_length) ||
               !equal_bytes(document.normalized_parent, document.normalized_parent_length,
                            reinterpret_cast<const uint8_t*>("dash"), 4)) {
      // Hardening: the document must be the one its index path names.
      failure = Failure::kDocument;
    } else {
      out->found = true;
      memcpy(out->owner_id, document.owner_id, kHashBytes);
      memcpy(out->document_id, document.id, kHashBytes);
    }
  }
  layers.items.reset();
  if (failure != Failure::kNone) {
    *out = Resolution{};
  }
  return failure;
}

Failure VerifyTenderdashCommit(const DocumentsProof& proof, const uint8_t root_hash[kHashBytes],
                               QuorumKeys* quorum_keys) {
  if (proof.quorum_hash_length != kHashBytes || !proof.quorum_hash) return Failure::kQuorumHash;
  uint8_t public_key[kPublicKeyBytes];
  if (!quorum_keys || !quorum_keys->PublicKey(proof.quorum_type, proof.quorum_hash, public_key))
    return Failure::kQuorumUnknown;
  // StateId (proto3, length-delimited): fixed64 app_version = 1, fixed64
  // height = 2, bytes app_hash = 3, fixed32 core_chain_locked_height = 4,
  // fixed64 time = 5; zero values are omitted.
  uint8_t state[96];
  size_t used = 0;
  if (proof.protocol_version) { state[used++] = 0x09; put_le(state + used, proof.protocol_version, 8); used += 8; }
  if (proof.height) { state[used++] = 0x11; put_le(state + used, proof.height, 8); used += 8; }
  state[used++] = 0x1a;
  state[used++] = kHashBytes;
  memcpy(state + used, root_hash, kHashBytes);
  used += kHashBytes;
  if (proof.core_chain_locked_height) {
    state[used++] = 0x25;
    put_le(state + used, proof.core_chain_locked_height, 4);
    used += 4;
  }
  if (proof.time_ms) { state[used++] = 0x29; put_le(state + used, proof.time_ms, 8); used += 8; }
  uint8_t delimited[100];
  const size_t prefix = fortis::codec::proto_wire::EncodeVarint(delimited, used);
  memcpy(delimited + prefix, state, used);
  uint8_t state_hash[32];
  sha256(delimited, prefix + used, state_hash);
  if (proof.quorum_type > 255) return Failure::kQuorumType;
  // CanonicalVote sign bytes: the round is compared as i32 and i64.
  if (proof.round > 0x7fffffffU || proof.block_id_hash_length != kHashBytes || !proof.block_id_hash)
    return Failure::kCommit;
  const uint64_t height = proof.height;  // `as i64`: the same 64 bits
  const size_t chain_length = proof.chain_id_length;
  const size_t sign_length = 4 + 8 + 8 + 32 + 32 + chain_length;
  auto sign_bytes_storage = base::AllocateStorage<uint8_t[]>(sign_length ? sign_length : 1);
  auto* sign_bytes = sign_bytes_storage.get();
  if (!sign_bytes) return Failure::kCommit;
  put_le(sign_bytes, 2, 4);  // SignedMsgType::Precommit
  // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
  // cppcheck-suppress arithOperationsOnVoidPointer
  put_le(sign_bytes + 4, height, 8);
  // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
  // cppcheck-suppress arithOperationsOnVoidPointer
  put_le(sign_bytes + 12, proof.round, 8);
  // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
  // cppcheck-suppress arithOperationsOnVoidPointer
  memcpy(sign_bytes + 20, proof.block_id_hash, kHashBytes);
  // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
  // cppcheck-suppress arithOperationsOnVoidPointer
  memcpy(sign_bytes + 52, state_hash, kHashBytes);
  // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
  // cppcheck-suppress arithOperationsOnVoidPointer
  if (chain_length) memcpy(sign_bytes + 84, proof.chain_id, chain_length);
  uint8_t message_hash[32];
  sha256(sign_bytes, sign_length, message_hash);
  sign_bytes_storage.reset();
  uint8_t request[7 + 8 + 4];
  std::copy_n("dpbvote", 7, request);
  put_le(request + 7, height, 8);
  put_le(request + 15, proof.round, 4);
  uint8_t request_id[32];
  sha256(request, sizeof(request), request_id);
  uint8_t digest_input[1 + 32 * 3];
  digest_input[0] = static_cast<uint8_t>(proof.quorum_type);
  for (size_t i = 0; i < 32; ++i) {
    digest_input[1 + i] = proof.quorum_hash[31 - i];
    digest_input[33 + i] = request_id[31 - i];
    digest_input[65 + i] = message_hash[31 - i];
  }
  uint8_t first[32], digest[32];
  sha256(digest_input, sizeof(digest_input), first);
  sha256(first, sizeof(first), digest);
  if (proof.signature_length != kSignatureBytes || !proof.signature) return Failure::kSignatureEncoding;
  blst_p1_affine key;
  if (blst_p1_uncompress(&key, public_key) != BLST_SUCCESS || blst_p1_affine_is_inf(&key) ||
      !blst_p1_affine_in_g1(&key))
    return Failure::kPublicKey;
  bool zero = true;
  for (size_t i = 0; i < kSignatureBytes && zero; ++i) zero = proof.signature[i] == 0;
  if (zero) return Failure::kSignatureEncoding;  // "empty signature"
  blst_p2_affine signature;
  if (blst_p2_uncompress(&signature, proof.signature) != BLST_SUCCESS || !blst_p2_affine_in_g2(&signature))
    return Failure::kSignatureEncoding;
  const BLST_ERROR verified =
      blst_core_verify_pk_in_g1(&key, &signature, true, digest, sizeof(digest),
                                reinterpret_cast<const uint8_t*>(kBlsDst), sizeof(kBlsDst) - 1, nullptr, 0);
  return verified == BLST_SUCCESS ? Failure::kNone : Failure::kSignature;
}

Failure VerifyDpnsName(const char* label, size_t label_length, const DocumentsProof& proof, QuorumKeys* quorum_keys,
                       Resolution* out) {
  *out = Resolution{};
  Resolution grove;
  if (!proof.grovedb_proof) return Failure::kNoProof;
  const Failure failure = VerifyDpnsGroveProof(label, label_length, proof.grovedb_proof,
                                               proof.grovedb_proof_length, &grove);
  if (failure != Failure::kNone) return failure;
  const Failure commit = VerifyTenderdashCommit(proof, grove.root_hash, quorum_keys);
  if (commit != Failure::kNone) return commit;
  *out = grove;
  return Failure::kNone;
}

}  // namespace fortis::exchange::dashpay
