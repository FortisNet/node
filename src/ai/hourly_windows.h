#pragma once

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "threading.h"

namespace fortis::ai::quota {

// The hourly allowances of the AI conversation routes
// (docs/compat-decisions.md#hourly-windows): a fixed window from the first
// hit, kept in a bounded table on the caller's clock. The process runtime's
// quota table serves per-minute windows only (it restarts every minute), so
// it cannot hold an hour. Counters are in memory and reset on restart; a
// full table fails open for a new key. `KeyBytes` bounds the scope letter
// plus the identifier (device id or owner id).
template <size_t KeyBytes>
class HourlyWindows {
 public:
  static constexpr size_t kCapacity = 4096;  // power of two

  bool Increment(char scope, const char* id, int64_t now, int64_t window, uint64_t* count) {
    char key[KeyBytes];
    if (snprintf(key, sizeof(key), "%c%s", scope, id) >= static_cast<int>(sizeof(key))) return false;
    uint64_t hash = 1469598103934665603ULL;
    for (const char* p = key; *p; ++p) hash = (hash ^ static_cast<uint8_t>(*p)) * 1099511628211ULL;
    fortis::base::LockGuard lock(mutex_);
    Entry* found = nullptr;
    Entry* reusable = nullptr;
    for (size_t probe = 0; probe < kCapacity; ++probe) {
      Entry& entry = entries_[(hash + probe) & (kCapacity - 1)];
      if (!entry.key[0]) {
        if (!reusable) reusable = &entry;
        break;  // never used: the key is not further along
      }
      if (strcmp(entry.key, key) == 0) { found = &entry; break; }
      if (!reusable && entry.expires_ms <= now) reusable = &entry;
    }
    // A live entry keeps its original expiry; an expired one starts over.
    Entry* entry = found ? found : reusable;
    const bool ok = entry != nullptr;
    if (ok) {
      if (entry != found || entry->expires_ms <= now) {
        memcpy(entry->key, key, sizeof(key));
        entry->expires_ms = now + window;
        entry->count = 0;
      }
      *count = ++entry->count;
    }
    return ok;
  }

 private:
  struct Entry {
    char key[KeyBytes];
    int64_t expires_ms;
    uint64_t count;
  };
  Entry entries_[kCapacity]{};
  fortis::base::Mutex mutex_;
};

}  // namespace fortis::ai::quota
