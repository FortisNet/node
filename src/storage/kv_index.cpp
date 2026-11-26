// The KV store's in-memory index: an AVL tree of keys to log positions.
#include "kv_store_internal.h"

namespace fortis::storage {

namespace {

int Height(const KvNode* node) { return node ? node->height : 0; }
void Reheight(KvNode* node) {
  const int left = Height(node->left), right = Height(node->right);
  node->height = static_cast<int8_t>(1 + (left > right ? left : right));
}
KvNode* RotateRight(KvNode* top) {
  KvNode* next = top->left;
  top->left = next->right;
  next->right = top;
  Reheight(top); Reheight(next);
  return next;
}
KvNode* RotateLeft(KvNode* top) {
  KvNode* next = top->right;
  top->right = next->left;
  next->left = top;
  Reheight(top); Reheight(next);
  return next;
}
KvNode* Balance(KvNode* node) {
  Reheight(node);
  if (Height(node->left) - Height(node->right) > 1) {
    if (Height(node->left->left) < Height(node->left->right))
      node->left = RotateLeft(node->left);
    return RotateRight(node);
  }
  if (Height(node->right) - Height(node->left) > 1) {
    if (Height(node->right->right) < Height(node->right->left))
      node->right = RotateRight(node->right);
    return RotateLeft(node);
  }
  return node;
}

KvNode* ExtractMin(KvNode* node, KvNode** minimum) {
  if (!node->left) { *minimum = node; return node->right; }
  node->left = ExtractMin(node->left, minimum);
  return Balance(node);
}

}  // namespace

namespace kv_internal {

void FreeTree(KvNode* node) noexcept {
  if (!node) return;
  std::unique_ptr<KvNode, base::StorageDeleter> owner(node);
  std::unique_ptr<char, base::StorageDeleter> key(node->key);
  FreeTree(node->left); FreeTree(node->right);
}
KvNode* Find(KvNode* node, const char* key) {
  while (node) {
    const int order = strcmp(key, node->key);
    if (!order) break;
    node = order < 0 ? node->left : node->right;
  }
  return node;
}

// `*previous` receives the replaced record's encoded size, or 0 when added.
KvNode* Put(KvNode* node, const char* key, uint64_t offset, uint32_t length,
            int64_t expiry, uint8_t generation, bool* added, bool* ok,
            uint64_t* previous) noexcept {
  if (!node) {
    auto created = base::AllocateZeroedStorage<KvNode>(1, sizeof(KvNode));
    auto key_storage = created ? base::AllocateStorage<char[]>(strlen(key) + 1) : nullptr;
    if (!created || !key_storage) {
      *ok = false;
      return nullptr;
    }
    memcpy(key_storage.get(), key, strlen(key) + 1);
    created->key = key_storage.release();
    created->offset = offset; created->length = length;
    created->expiry = expiry; created->generation = generation;
    created->height = 1;
    *added = true;
    return created.release();
  }
  const int order = strcmp(key, node->key);
  if (order < 0) {
    node->left = Put(node->left, key, offset, length, expiry, generation, added, ok, previous);
  } else if (order > 0) {
    node->right = Put(node->right, key, offset, length, expiry, generation, added, ok, previous);
  } else {
    *previous = RecordBytes(strlen(node->key), node->length);
    node->offset = offset; node->length = length;
    node->expiry = expiry; node->generation = generation;
  }
  return *ok ? Balance(node) : node;
}

// `*removed` receives the erased record's encoded size, or stays 0.
KvNode* Erase(KvNode* node, const char* key, uint64_t* removed) noexcept {
  if (!node) return nullptr;
  const int order = strcmp(key, node->key);
  if (order < 0) node->left = Erase(node->left, key, removed);
  else if (order > 0) node->right = Erase(node->right, key, removed);
  else {
    *removed = RecordBytes(strlen(node->key), node->length);
    KvNode* left = node->left;
    KvNode* right = node->right;
    {
      std::unique_ptr<KvNode, base::StorageDeleter> owner(node);
      std::unique_ptr<char, base::StorageDeleter> owned_key(node->key);
    }
    if (!right) return left;
    KvNode* minimum = nullptr;
    right = ExtractMin(right, &minimum);
    minimum->left = left; minimum->right = right;
    return Balance(minimum);
  }
  return *removed ? Balance(node) : node;
}

}  // namespace kv_internal

void KvNodeTreeDeleter::operator()(KvNode* node) const noexcept {
  kv_internal::FreeTree(node);
}

}  // namespace fortis::storage
