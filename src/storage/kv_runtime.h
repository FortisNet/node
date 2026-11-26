#pragma once
#include "kv_store.h"
#include <stdint.h>
namespace fortis::storage {
// Directory selection is serialized with initialization and only takes effect
// before the process-owned store has opened successfully.
void RuntimeKvSetDirectory(const char* directory, bool group_commit = false,
                           KvDurability durability = KvDurability::RollbackMarker);
// Concurrent calls share one process-owned store; failed opens remain retryable.
KvStore* RuntimeKvStore();
// The process store if it is already open; never opens it.
KvStore* RuntimeKvStoreIfOpen();
// Nonblocking lookup for the dashboard, which never opens the store.
KvStore* TryRuntimeKvStoreIfOpen();
// Bounded 16,384-slot in-memory fixed-window quotas. Counters reset when this
// process restarts. A saturated bucket rejects new keys with a count of
// UINT64_MAX and preserves active counters.
bool RuntimeKvIncrementWindow(const char* key, int64_t now_ms, int64_t ttl_ms,
                              uint64_t* value);
bool RuntimeKvIncrementByWindow(const char* key, int64_t increment, int64_t now_ms,
                                int64_t ttl_ms, uint64_t* value);
}
