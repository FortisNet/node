#include "threading.h"
#pragma once
#include <memory>

#include "token_json.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

namespace fortis::wallet::address { class Store; }

// The TypeScript device sync scope: the accounts one device may read from the
// sync journal, resolved from its live watches only — the account id of a
// portfolio watch and the (network, address) of an `address:` topic.
// Resolving renews every watch within five minutes of expiry (one frame).
//
// A non-empty scope is reused for four minutes. The source keeps up to
// 50,000 devices in an insertion-ordered map; this cache is direct-mapped
// over kScopeSlots slots, so two devices that share a slot evict each other
// early (a recompute: never another device's accounts, only an earlier
// renewal and an earlier view of a new watch). A topic written by
// rememberTopic()/remember() invalidates the device's entry at once.
// Guest-safe.
namespace fortis::wallet::sync {

constexpr int64_t kScopeCacheMs = 4 * 60000;  // SCOPE_CACHE_MS
constexpr int64_t kWatchRenewBeforeMs = 5 * 60000;
constexpr size_t kScopeSlots = 8192;

struct ScopeAccount {
  const char* id;
  const char* network;
  const char* address;
};

class Scope {
 public:
  Scope();
  ~Scope();
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

  // Allocates the slot table and becomes the store's topic listener.
  bool Start(address::Store* store);
  void Stop();

  // accounts(deviceId, now): the scope as arena-owned rows.
  bool Accounts(const char* device_id, int64_t now_ms, codec::tj::Arena& arena, ScopeAccount** accounts,
                size_t* count);

  // watchesService.onTopicWritten(deviceId): drop the cached scope.
  void Invalidate(const char* device_id);

 private:
  struct Slot;
  bool Resolve(const char* device_id, int64_t now_ms, codec::tj::Arena& arena, ScopeAccount** accounts, size_t* count);

  address::Store* store_ = nullptr;
  std::unique_ptr<Slot[]> slots_;
  fortis::base::Mutex mutex_;
};

}  // namespace fortis::wallet::sync
