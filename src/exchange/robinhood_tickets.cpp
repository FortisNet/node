#include <cstdio>
#include "robinhood_tickets.h"

#include "threading.h"

#include <stdlib.h>
#include <string.h>

namespace fortis::exchange::robinhood {

TicketStore::TicketStore() {
  try { slots_ = std::make_unique<Slot[]>(kCapacity); } catch (const std::bad_alloc&) { slots_.reset(); }
}

TicketStore::~TicketStore() = default;

void TicketStore::ReleaseLocked(Slot& slot) {
  bytes_ -= slot.length;
  slot.prepared.reset();
  slot = Slot{};
  --count_;
}

void TicketStore::PruneLocked(int64_t now_ms) {
  for (size_t i = 0; i < kCapacity; ++i)
    if (slots_[i].used && slots_[i].expires_at_ms <= now_ms) ReleaseLocked(slots_[i]);
}

TicketStore::InsertResult TicketStore::Insert(const char* ticket, const char* device_id, int64_t expires_at_ms,
                                              const char* prepared, size_t length, size_t node_count,
                                              int64_t now_ms) {
  if (!slots_ || strlen(ticket) != kTicketChars || strlen(device_id) >= sizeof(Slot::device_id)) return kFull;
  auto copy = base::AllocateStorage<char[]>(length ? length : 1);
  if (!copy) return kFull;
  memcpy(copy.get(), prepared, length);
  fortis::base::UniqueLock lock(mutex_);
  PruneLocked(now_ms);
  // Map.set on an existing ticket replaces it in place.
  Slot* target = nullptr;
  for (size_t i = 0; i < kCapacity && !target; ++i)
    if (slots_[i].used && strcmp(slots_[i].ticket, ticket) == 0) target = &slots_[i];
  const size_t replaced = target ? target->length : 0;
  if (count_ >= kCapacity || bytes_ - replaced + length > kMaxBytes) {
    lock.Unlock();
    copy.reset();
    return kFull;
  }
  if (target) ReleaseLocked(*target);
  for (size_t i = 0; i < kCapacity && !target; ++i)
    if (!slots_[i].used) target = &slots_[i];
  target->used = true;
  target->in_flight = false;
  memcpy(target->ticket, ticket, kTicketChars + 1);
  std::snprintf(target->device_id, sizeof(target->device_id), "%s", device_id);
  target->expires_at_ms = expires_at_ms;
  target->generation = next_generation_++;
  target->prepared = std::move(copy);
  target->length = length;
  target->node_count = node_count;
  ++count_;
  bytes_ += length;
  return kInserted;
}

TicketStore::BeginResult TicketStore::Begin(const char* ticket, const char* device_id, int64_t now_ms,
                                            size_t signature_count, Claim* claim) {
  if (!slots_) return kNotFound;
  fortis::base::LockGuard lock(mutex_);
  PruneLocked(now_ms);
  Slot* slot = nullptr;
  for (size_t i = 0; i < kCapacity && !slot; ++i)
    if (slots_[i].used && strcmp(slots_[i].ticket, ticket) == 0) slot = &slots_[i];
  if (!slot || strcmp(slot->device_id, device_id) != 0 || slot->expires_at_ms <= now_ms || slot->in_flight) {
    return kNotFound;
  }
  if (signature_count != slot->node_count) return kWrongCount;
  auto copy = base::AllocateStorage<char[]>(slot->length ? slot->length : 1);
  if (!copy) return kNotFound;
  memcpy(copy.get(), slot->prepared.get(), slot->length);
  slot->in_flight = true;
  claim->slot = static_cast<size_t>(slot - slots_.get());
  claim->generation = slot->generation;
  claim->prepared = std::move(copy);
  claim->length = slot->length;
  return kClaimed;
}

void TicketStore::Finish(const Claim& claim, bool delivered) {
  if (!slots_ || claim.slot >= kCapacity) return;
  fortis::base::LockGuard lock(mutex_);
  Slot& slot = slots_[claim.slot];
  if (slot.used && slot.generation == claim.generation) {
    if (delivered) ReleaseLocked(slot);
    else slot.in_flight = false;
  }
}

size_t TicketStore::Pending(int64_t now_ms) {
  fortis::base::LockGuard lock(mutex_);
  PruneLocked(now_ms);
  const size_t count = count_;
  return count;
}

}  // namespace fortis::exchange::robinhood
