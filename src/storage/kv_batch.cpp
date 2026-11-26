// KvBatch: mutations staged in memory and committed as one frame.
#include "kv_store_internal.h"

namespace fortis::storage {

// ---- KvBatch -----------------------------------------------------------------

static_assert(KvBatch::kMaxFrameBytes == kMaxFrame, "KvBatch frame bound");
static_assert(KvBatch::kRecordOverhead == kRecord, "KvBatch record bound");
static_assert(KvBatch::kFrameOverhead == kHeader + 4, "KvBatch frame header");

KvBatch::KvBatch(size_t max_bytes, size_t max_mutations)
    : max_bytes_(max_bytes), max_mutations_(max_mutations) {
  if (max_mutations_ > kMaxMutations) max_mutations_ = kMaxMutations;
  if (max_mutations_ == 0) max_mutations_ = 1;
  const size_t overhead = kFrameOverhead + max_mutations_ * kRecordOverhead;
  const size_t room = overhead < kMaxFrame ? kMaxFrame - overhead : 0;
  if (max_bytes_ > room) max_bytes_ = room;
}

KvBatch::~KvBatch() = default;

size_t KvBatch::FrameBytes() const {
  return count_ ? kFrameOverhead + count_ * kRecordOverhead + used_ - count_ : 0;
}

void KvBatch::Rollback(Mark mark) {
  if (mark.count <= count_ && mark.used <= used_) {
    count_ = mark.count;
    used_ = mark.used;
  }
  full_ = false;
}

void KvBatch::Clear() {
  count_ = 0;
  used_ = 0;
  full_ = false;
}

bool KvBatch::Reserve(size_t bytes) {
  if (bytes <= capacity_) return true;
  size_t next = capacity_ ? capacity_ : 16 * 1024;
  while (next < bytes) next = next > max_bytes_ / 2 ? max_bytes_ : next * 2;
  if (!base::ResizeStorage(buffer_, used_, next)) return false;
  capacity_ = next;
  return true;
}

bool KvBatch::Stage(const char* key, const void* value, size_t size, int64_t expires_at_ms,
                    bool erase) {
  if (!ValidKey(key) || size > kMaxValue || (size && !value) || expires_at_ms < 0 ||
      (erase && (size || expires_at_ms)))
    return false;
  const size_t key_bytes = strlen(key) + 1;  // stored NUL-terminated
  if (count_ >= max_mutations_ || used_ > max_bytes_ ||
      key_bytes + size > max_bytes_ - used_) {
    full_ = true;
    return false;
  }
  if (count_ == entry_capacity_) {
    size_t next = entry_capacity_ ? entry_capacity_ * 2 : 16;
    if (next > max_mutations_) next = max_mutations_;
    if (!base::ResizeStorage(entries_, count_ * sizeof(Entry), next * sizeof(Entry))) return false;
    entry_capacity_ = next;
  }
  if (!Reserve(used_ + key_bytes + size)) return false;
  Entry& entry = entries_.get()[count_];
  entry.key = used_;
  // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
  // cppcheck-suppress arithOperationsOnVoidPointer
  memcpy(buffer_.get() + used_, key, key_bytes);
  entry.value = used_ + key_bytes;
  // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
  // cppcheck-suppress arithOperationsOnVoidPointer
  if (size) memcpy(buffer_.get() + entry.value, value, size);
  entry.size = static_cast<uint32_t>(size);
  entry.expires_at_ms = expires_at_ms;
  entry.erase = erase;
  used_ += key_bytes + size;
  ++count_;
  return true;
}

bool KvBatch::Put(const char* key, const void* value, size_t size, int64_t expires_at_ms) {
  return Stage(key, value, size, expires_at_ms, false);
}

bool KvBatch::Erase(const char* key) { return Stage(key, nullptr, 0, 0, true); }

const KvBatch::Entry* KvBatch::Find(const char* key) const {
  for (size_t i = count_; i > 0; --i)
    // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
    // cppcheck-suppress arithOperationsOnVoidPointer
    if (strcmp(buffer_.get() + entries_.get()[i - 1].key, key) == 0) return &entries_.get()[i - 1];
  return nullptr;
}

KvReadStatus KvBatch::Get(const KvStore* store, const char* key, int64_t now_ms, void* output,
                          size_t capacity, size_t* value_size) const {
  if (!key || !value_size || (!output && capacity)) return KvReadStatus::IoError;
  const Entry* staged = Find(key);
  if (!staged) {
    return store ? store->GetStatus(key, now_ms, output, capacity, value_size)
                 : KvReadStatus::IoError;
  }
  if (staged->erase || (staged->expires_at_ms && staged->expires_at_ms <= now_ms))
    return KvReadStatus::Missing;
  *value_size = staged->size;
  if (staged->size > capacity) return KvReadStatus::BufferTooSmall;
  // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
  // cppcheck-suppress arithOperationsOnVoidPointer
  if (staged->size) memcpy(output, buffer_.get() + staged->value, staged->size);
  return KvReadStatus::Found;
}

bool KvBatch::Commit(KvStore* store) {
  if (!store) return false;
  if (!count_) return true;
  auto mutations_storage = base::AllocateStorage<KvMutation[]>(count_ * sizeof(KvMutation));
  auto* mutations = mutations_storage.get();
  if (!mutations) return false;
  for (size_t i = 0; i < count_; ++i) {
    const Entry& entry = entries_.get()[i];
    // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
    // cppcheck-suppress arithOperationsOnVoidPointer
    mutations[i] = KvMutation{buffer_.get() + entry.key, entry.erase ? nullptr : buffer_.get() + entry.value,
                              entry.size, entry.expires_at_ms, entry.erase};
  }
  const bool ok = store->Write(mutations, count_);
  mutations_storage.reset();
  if (ok) Clear();
  return ok;
}

}  // namespace fortis::storage
