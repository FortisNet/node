#include "threading.h"
#pragma once
#include "ownership.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

// The sponsored-trade tickets of the TypeScript
// RobinhoodSponsoredTradeGateway: a process-memory map from a
// one-time ticket to the prepared Alchemy operation, bound to the device that
// prepared it. Restart drops every ticket, as in TypeScript.
//
//  - Insert prunes expired tickets, then refuses at 1,000 pending tickets
//    (in flight or not) — the route's 503.
//  - Begin prunes, then claims an unexpired ticket of the same device that is
//    not in flight and whose prepared node count equals the signature count;
//    the claim copies the prepared JSON and marks the ticket in flight, so a
//    concurrent or replayed submit finds nothing.
//  - Finish deletes the ticket after delivery, or clears the in-flight mark
//    after a failure so the wallet may retry. A ticket pruned (expired) while
//    in flight is not resurrected.
//
// Native bound (documented difference): the stored prepared JSON of all
// pending tickets is at most kMaxBytes; past it Insert answers kFull.
// The guest supports the C++17 standard owners used by slots and claims.
namespace fortis::exchange::robinhood {

class TicketStore {
 public:
  static constexpr size_t kCapacity = 1000;
  static constexpr size_t kMaxBytes = 16 * 1024 * 1024;
  static constexpr size_t kTicketChars = 32;
  static constexpr int64_t kTtlMs = 2 * 60000;

  enum InsertResult : uint8_t { kInserted, kFull };
  enum BeginResult : uint8_t { kClaimed, kNotFound, kWrongCount };

  struct Claim {
    size_t slot = 0;
    uint64_t generation = 0;
    std::unique_ptr<char[], base::StorageDeleter> prepared;  // owned copy
    size_t length = 0;
  };

  TicketStore();
  ~TicketStore();
  TicketStore(const TicketStore&) = delete;
  TicketStore& operator=(const TicketStore&) = delete;

  InsertResult Insert(const char* ticket, const char* device_id, int64_t expires_at_ms, const char* prepared,
                      size_t length, size_t node_count, int64_t now_ms);
  BeginResult Begin(const char* ticket, const char* device_id, int64_t now_ms, size_t signature_count,
                    Claim* claim);
  void Finish(const Claim& claim, bool delivered);
  // Pending tickets after pruning (tests).
  size_t Pending(int64_t now_ms);

 private:
  struct Slot {
    bool used = false;
    bool in_flight = false;
    char ticket[kTicketChars + 1]{};
    char device_id[27]{};
    int64_t expires_at_ms = 0;
    uint64_t generation = 0;
    std::unique_ptr<char[], base::StorageDeleter> prepared;
    size_t length = 0;
    size_t node_count = 0;
  };
  void PruneLocked(int64_t now_ms);
  void ReleaseLocked(Slot& slot);

  std::unique_ptr<Slot[]> slots_;
  size_t count_ = 0;
  size_t bytes_ = 0;
  uint64_t next_generation_ = 1;
  fortis::base::Mutex mutex_;
};

}  // namespace fortis::exchange::robinhood
