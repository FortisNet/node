#include "ownership.h"
#include "sync_scope.h"

#include "address_records.h"
#include "address_store.h"
#include "threading.h"

#include <stdlib.h>
#include <string.h>

namespace fortis::wallet::sync {
namespace {

namespace tj = fortis::codec::tj;

uint64_t device_hash(const char* device) {
  uint64_t h = 1469598103934665603ULL;
  for (const char* p = device; *p; ++p) h = (h ^ static_cast<uint8_t>(*p)) * 1099511628211ULL;
  return h ? h : 1;
}

void listener(void* context, const char* device_id) { static_cast<Scope*>(context)->Invalidate(device_id); }

const char* text_member(const tj::Value* object, const char* key) {
  const tj::Value* value = tj::Get(object, key);
  return tj::IsString(value) ? value->text : nullptr;
}

}  // namespace

// One cached scope: "id\0network\0address\0" per account in `blob`.
struct Scope::Slot {
  uint64_t hash = 0;  // 0: empty
  uint64_t epoch = 0;
  int64_t at = 0;
  char device[64]{};
  std::unique_ptr<char[], base::StorageDeleter> blob;
  size_t bytes = 0;
  size_t count = 0;

  void Clear() {
    blob.reset();
    bytes = count = 0;
    hash = 0;
    device[0] = '\0';
  }
};

Scope::Scope() = default;
Scope::~Scope() { Stop(); }

bool Scope::Start(address::Store* store) {
  if (slots_ || !store) return false;
  try { slots_ = std::make_unique<Slot[]>(kScopeSlots); }
  catch (const std::bad_alloc&) { return false; }
  if (!slots_) return false;
  store_ = store;
  store_->SetTopicListener(listener, this);
  return true;
}

void Scope::Stop() {
  if (!slots_) return;
  if (store_) store_->SetTopicListener(nullptr, nullptr);
  for (size_t i = 0; i < kScopeSlots; ++i) slots_[i].Clear();
  slots_.reset();
  store_ = nullptr;
}

void Scope::Invalidate(const char* device_id) {
  if (!slots_ || !device_id) return;
  const uint64_t hash = device_hash(device_id);
  fortis::base::LockGuard lock(mutex_);
  Slot& slot = slots_[hash % kScopeSlots];
  ++slot.epoch;
  if (slot.hash == hash && !strcmp(slot.device, device_id)) slot.Clear();
}

bool Scope::Accounts(const char* device_id, int64_t now_ms, tj::Arena& arena, ScopeAccount** accounts,
                     size_t* count) {
  *accounts = nullptr;
  *count = 0;
  if (!slots_ || !device_id || strlen(device_id) >= sizeof(Slot::device)) return false;
  const uint64_t hash = device_hash(device_id);
  Slot& slot = slots_[hash % kScopeSlots];
  fortis::base::UniqueLock lock(mutex_);
  if (slot.hash == hash && !strcmp(slot.device, device_id) && now_ms - slot.at < kScopeCacheMs) {
    ScopeAccount* rows = static_cast<ScopeAccount*>(arena.Alloc(sizeof(ScopeAccount) * slot.count));
    char* copy = rows ? static_cast<char*>(arena.Alloc(slot.bytes, 1)) : nullptr;
    if (copy) {
      memcpy(copy, slot.blob.get(), slot.bytes);
      const char* cursor = copy;
      for (size_t i = 0; i < slot.count; ++i) {
        rows[i].id = cursor;
        cursor += strlen(cursor) + 1;
        rows[i].network = cursor;
        cursor += strlen(cursor) + 1;
        rows[i].address = cursor;
        cursor += strlen(cursor) + 1;
      }
    }
    const size_t cached = slot.count;
    lock.Unlock();
    if (!copy) return false;
    *accounts = rows;
    *count = cached;
    return true;
  }
  const uint64_t epoch = slot.epoch;
  lock.Unlock();

  ScopeAccount* rows = nullptr;
  size_t resolved = 0;
  if (!Resolve(device_id, now_ms, arena, &rows, &resolved)) return false;
  size_t bytes = 0;
  for (size_t i = 0; i < resolved; ++i)
    bytes += strlen(rows[i].id) + strlen(rows[i].network) + strlen(rows[i].address) + 3;
  auto blob = resolved ? base::AllocateStorage<char[]>(bytes) : nullptr;
  if (blob) {
    char* cursor = blob.get();
    for (size_t i = 0; i < resolved; ++i) {
      const char* const parts[] = {rows[i].id, rows[i].network, rows[i].address};
      for (const char* part : parts) {
        const size_t length = strlen(part) + 1;
        memcpy(cursor, part, length);
        cursor += length;
      }
    }
  }
  lock.Lock();
  // cache.delete(deviceId); a non-empty scope is cached unless a topic was
  // written for this slot while it resolved.
  if (slot.hash == hash && !strcmp(slot.device, device_id)) slot.Clear();
  if (blob && slot.epoch == epoch) {
    slot.Clear();
    slot.hash = hash;
    memcpy(slot.device, device_id, strlen(device_id) + 1);
    slot.at = now_ms;
    slot.blob = std::move(blob);
    slot.bytes = bytes;
    slot.count = resolved;
    blob = nullptr;
  }
  lock.Unlock();
  blob.reset();
  *accounts = rows;
  *count = resolved;
  return true;
}

bool Scope::Resolve(const char* device_id, int64_t now_ms, tj::Arena& arena, ScopeAccount** accounts,
                    size_t* count) {
  tj::Value* rows = nullptr;
  if (!store_->ListDeviceWatches(device_id, now_ms, arena, &rows)) return false;
  if (!rows->count) return true;
  const size_t capacity = rows->count * 2;
  const char** ids = static_cast<const char**>(arena.Alloc(sizeof(char*) * rows->count));
  const char** topics = static_cast<const char**>(arena.Alloc(sizeof(char*) * rows->count));
  ScopeAccount* found = static_cast<ScopeAccount*>(arena.Alloc(sizeof(ScopeAccount) * capacity));
  if (!ids || !topics || !found) return false;
  size_t id_count = 0, due_count = 0, found_count = 0;
  for (uint32_t i = 0; i < rows->count; ++i) {
    const tj::Value* row = rows->items[i];
    const char* topic = text_member(row, "topic");
    if (topic && address::RowMs(row, "expiresAt") <= now_ms + kWatchRenewBeforeMs) topics[due_count++] = topic;
    const char* account = text_member(row, "accountId");
    if (!account) continue;
    bool seen = false;
    for (size_t j = 0; j < id_count && !seen; ++j) seen = !strcmp(ids[j], account);
    if (!seen) ids[id_count++] = account;
  }
  // found.set(row.id, row): a repeated id keeps its first position.
  auto add = [&](const address::Account& account) -> bool {
    for (size_t j = 0; j < found_count; ++j)
      if (!strcmp(found[j].id, account.id)) return true;
    if (found_count == capacity) return false;
    ScopeAccount& row = found[found_count];
    row.id = arena.CopyText(account.id, strlen(account.id));
    row.network = arena.CopyText(account.network, strlen(account.network));
    row.address = arena.CopyText(account.address, strlen(account.address));
    if (!row.id || !row.network || !row.address) return false;
    ++found_count;
    return true;
  };
  for (size_t i = 0; i < id_count; ++i) {
    address::Account account;
    const address::ReadStatus status = store_->ById(ids[i], now_ms, &account);
    if (status == address::ReadStatus::kError) return false;
    if (status == address::ReadStatus::kFound && !add(account)) return false;
  }
  for (uint32_t i = 0; i < rows->count; ++i) {
    // topic.split(':'): `address:{network}:{address…}` (the address keeps
    // any further colons).
    const char* topic = text_member(rows->items[i], "topic");
    if (!topic || strncmp(topic, "address:", 8) != 0) continue;
    const char* network = topic + 8;
    const char* colon = strchr(network, ':');
    if (!colon) continue;
    char* network_copy = arena.CopyText(network, static_cast<size_t>(colon - network));
    if (!network_copy) return false;
    address::Account account;
    const address::ReadStatus status = store_->Find(network_copy, colon + 1, now_ms, &account);
    if (status == address::ReadStatus::kError) return false;
    if (status == address::ReadStatus::kFound && !add(account)) return false;
  }
  // renewTopics(deviceId, dueTopics, now): fire-and-forget in the source,
  // so a failed renewal does not fail the read.
  if (due_count) (void)store_->RenewTopics(device_id, topics, due_count, now_ms, arena);
  *accounts = found;
  *count = found_count;
  return true;
}

}  // namespace fortis::wallet::sync
