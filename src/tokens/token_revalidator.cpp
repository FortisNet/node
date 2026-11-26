#include "token_service_internal.h"
#include "token_facts_service.h"
namespace fortis::tokens {
namespace { constexpr size_t kBackgroundArenaBytes = 6 * 1024 * 1024; }
// ---- revalidation queue ---------------------------------------------------
bool TokenRevalidator::Enqueue(const fortis::tokens::token_facts::Revalidation& task) {
  auto* self = this;
  auto copy = base::MakeUnique<token_facts::Revalidation>(task);
  if (!copy) return false;
  fortis::base::UniqueLock lock(self->control_.mutex());
  if (self->control_.StoppingLocked() || self->tasks_.size() == kRevalidationSlots) {
    lock.Unlock();
    copy.reset();
    return false;
  }
  self->tasks_.Push(copy.release());
  self->control_.changed().Broadcast();
  return true;
}

void* TokenRevalidator::Entry(void* opaque) {
  auto* self = static_cast<TokenRevalidator*>(opaque);
  for (;;) {
    fortis::base::UniqueLock lock(self->control_.mutex());
    while (!self->control_.StoppingLocked() && self->tasks_.size() == 0) self->control_.changed().WaitLocked(self->control_.mutex());
    if (self->tasks_.size() == 0) break;
    auto* task = self->tasks_.Pop();
    self->revalidating_ = true;
    // CoinGeckoClient.coinMarket's WindowBatcher: market revalidations that
    // arrive within 20 ms share one /coins/markets request. The Tron
    // driver's snapshot flight: the chainMeta/insight/holders revalidations
    // one stale read starts together share one snapshot read.
    fortis::tokens::token_facts::Revalidation* batch[kRevalidationSlots];
    size_t batched = 0;
    const bool market = fortis::tokens::token_facts::BatchableMarket(*task);
    const bool snapshot = !market && fortis::tokens::token_facts::BatchableSnapshot(*task, self->env_);
    if (market || snapshot) {
      batch[batched++] = task;
      const timespec window = wake_after(20);
      while (!self->control_.StoppingLocked()) {
        if (self->control_.changed().WaitUntilLocked(self->control_.mutex(), window) != 0) break;
      }
      batched += self->tasks_.Select([&](const token_facts::Revalidation* queued) {
        return market ? token_facts::BatchableMarket(*queued)
                      : token_facts::SameSnapshot(*task, *queued) &&
                            token_facts::BatchableSnapshot(*queued, self->env_);
      }, batch + batched, kRevalidationSlots - batched);
    }
    const bool stopping = self->control_.StoppingLocked();
    lock.Unlock();
    std::unique_ptr<token_facts::Revalidation, base::ObjectDeleter<token_facts::Revalidation>> owned[kRevalidationSlots];
    if (batched) for (size_t i = 0; i < batched; ++i) owned[i].reset(batch[i]);
    else owned[0].reset(task);
    Work work{};
    if (!stopping && self->workspaces_.Make(&work, kBackgroundArenaBytes, true, kTaskBudgetMs)) {
      if (snapshot) token_facts::TokenFactsService(work).RevalidateSnapshots( batch, batched);
      else if (batched) token_facts::TokenFactsService(work).RevalidateMarkets( batch, batched);
      else token_facts::TokenFactsService(work).Revalidate( *task);
      TokenWorkspaces::Free(&work);
    } else if (batched) {
      for (size_t i = 0; i < batched; ++i) self->env_.swr->EndRevalidate(batch[i]->key.data(), self->env_.swr->Now(), false, false);
    } else {
      self->env_.swr->EndRevalidate(task->key.data(), self->env_.swr->Now(), false, false);
    }
    for (auto& item : owned) item.reset();
    lock.Lock();
    self->revalidating_ = false;
    self->control_.changed().Broadcast();
  }
  return nullptr;
}

bool TokenRevalidator::WaitIdle(uint32_t timeout_ms) {
  const uint64_t deadline = providers::TokenMonotonicMs() + timeout_ms;
  fortis::base::LockGuard lock(control_.mutex());
  while (tasks_.size() || revalidating_) {
    const uint64_t now = providers::TokenMonotonicMs();
    if (now >= deadline) break;
    const timespec wake = wake_after(deadline - now > 50 ? 50 : deadline - now);
    control_.changed().WaitUntilLocked(control_.mutex(), wake);
  }
  const bool idle = !tasks_.size() && !revalidating_;
  return idle;
}

bool TokenRevalidator::Start(const pthread_attr_t* attributes) {
  if (started_) return false;
  started_ = thread_.StartEntry<Entry>(this, attributes);
  return started_;
}
void TokenRevalidator::Join() { if (started_) thread_.Join(); }
void TokenRevalidator::DiscardPending() { while (tasks_.size()) base::Delete(tasks_.Pop()); }
}  // namespace fortis::tokens
