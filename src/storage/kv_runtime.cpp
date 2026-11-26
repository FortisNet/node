#include "kv_runtime.h"
#include "ownership.h"

#include "threading.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
namespace fortis::storage {
namespace {
KvStore* store = nullptr;
bool runtime_group_commit = false;
KvDurability runtime_durability = KvDurability::RollbackMarker;
char runtime_kv_dir[1024] = "/data";
fortis::base::Mutex runtime_store_mutex;
fortis::base::Mutex quota_mutex;

// Mirrors the TypeScript MemoryKv quota counters: fixed-window counters are
// deliberately ephemeral and reset on process restart. Durable device and
// challenge state stays in KvStore. Open addressing keeps lookups average O(1)
// across 16,384 bounded slots; full-key comparisons disambiguate hash collisions.
struct QuotaCounter {
  char key[64]{};
  uint64_t value = 0;
  uint64_t generation = 0;
};
constexpr size_t kQuotaCounterCapacity = 16384;
constexpr size_t kQuotaMaxProbes = 64;
QuotaCounter quota_counters[kQuotaCounterCapacity]{};
bool quota_epoch_initialized = false;
int64_t quota_epoch_minute = 0;
uint64_t quota_generation = 0;

size_t quota_hash(const char* key) {
  uint32_t hash = 2166136261u;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(key); *p; ++p) {
    hash ^= *p;
    hash *= 16777619u;
  }
  return hash & (kQuotaCounterCapacity - 1);
}
}
void RuntimeKvSetDirectory(const char* directory, bool group_commit, KvDurability durability) {
  fortis::base::LockGuard lock(runtime_store_mutex);
  if (!lock.locked()) return;
  if (store) return;
  runtime_group_commit = group_commit;
  runtime_durability = durability;
  if (directory && *directory && strlen(directory) < sizeof(runtime_kv_dir))
    snprintf(runtime_kv_dir, sizeof(runtime_kv_dir), "%s", directory);
}
KvStore* RuntimeKvStore() {
  fortis::base::LockGuard lock(runtime_store_mutex);
  if (!lock.locked()) return nullptr;
  if (store) return store;
  auto* candidate = fortis::base::New<KvStore>(runtime_group_commit, false, runtime_durability);
  if (!candidate) return nullptr;
  if (!candidate->Open(runtime_kv_dir)) {
    fortis::base::Delete(candidate); return nullptr;
  }
  store = candidate;
  return store;
}
KvStore* RuntimeKvStoreIfOpen() {
  fortis::base::LockGuard lock(runtime_store_mutex);
  return lock.locked() ? store : nullptr;
}
KvStore* TryRuntimeKvStoreIfOpen() {
  fortis::base::UniqueLock lock(runtime_store_mutex, fortis::base::kTryToLock);
  if (!lock.locked()) return nullptr;
  // The published store pointer is stable for the lifetime of the process.
  KvStore* current = store;
  return current;
}
bool RuntimeKvIncrementWindow(const char* key, int64_t now_ms, int64_t ttl_ms,
                              uint64_t* value) {
  return RuntimeKvIncrementByWindow(key, 1, now_ms, ttl_ms, value);
}
bool RuntimeKvIncrementByWindow(const char* key, int64_t increment, int64_t now_ms,
                                int64_t ttl_ms, uint64_t* value) {
  if (!key || !value || increment <= 0 || now_ms < 0 || ttl_ms <= 0 ||
      ttl_ms > INT64_MAX - now_ms) return false;
  const size_t key_length = strnlen(key, sizeof(quota_counters[0].key));
  if (!key_length || key_length >= sizeof(quota_counters[0].key)) return false;
  const int64_t epoch_minute = now_ms / 60000;
  fortis::base::LockGuard lock(quota_mutex);
  if (!lock.locked()) return false;
  if (!quota_epoch_initialized || epoch_minute != quota_epoch_minute) {
    quota_epoch_minute = epoch_minute;
    quota_epoch_initialized = true;
    if (++quota_generation == 0) {
      for (QuotaCounter& entry : quota_counters) entry.generation = 0;
      quota_generation = 1;
    }
  }
  QuotaCounter* selected = nullptr;
  const size_t start = quota_hash(key);
  for (size_t probe = 0; probe < kQuotaMaxProbes; ++probe) {
    QuotaCounter& entry = quota_counters[(start + probe) & (kQuotaCounterCapacity - 1)];
    if (entry.generation == quota_generation) {
      if (strcmp(entry.key, key) == 0) {
        selected = &entry;
        break;
      }
      continue;
    }
    selected = &entry;
    break;
  }
  // Keep existing counters. Return a value above every admission limit for
  // a new key that cannot be tracked within the bounded probe budget.
  if (!selected) {
    *value = UINT64_MAX;
    return true;
  }
  if (selected->generation != quota_generation) {
    memcpy(selected->key, key, key_length);
    selected->key[key_length] = '\0';
    selected->value = 0;
    selected->generation = quota_generation;
  }
  if (static_cast<uint64_t>(increment) > UINT64_MAX - selected->value) return false;
  selected->value += static_cast<uint64_t>(increment);
  *value = selected->value;
  return true;
}
}
