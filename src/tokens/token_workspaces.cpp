#include "token_service_internal.h"
namespace tj = fortis::codec::tj;
namespace fortis::tokens {
// A per-task work context; false when the arenas cannot be allocated.
bool TokenWorkspaces::Make(Work* work, size_t arena_bytes, bool background, uint64_t budget_ms) {
  Free(work);
  work->env = &env_;
  work->arena_storage = base::AllocateStorage<uint8_t[]>(arena_bytes);
  work->arena = tj::Arena{work->arena_storage.get(), arena_bytes, 0, true};
  work->scratch_storage = base::AllocateStorage<uint8_t[]>(kScratchBytes);
  work->scratch = work->scratch_storage.get();
  work->scratch_capacity = kScratchBytes;
  work->background = background;
  work->deadline_ms = providers::TokenMonotonicMs() + budget_ms;
  if (!work->arena.base || !work->scratch) { Free(work); return false; }
  return true;
}
void TokenWorkspaces::Free(Work* work) {
  work->growing_blocks.reset();
  work->arena_storage.reset();
  work->scratch_storage.reset();
  work->arena = tj::Arena{};
  work->scratch = nullptr;
}

void TokenWorkspaces::FreeHistory(Work* work) {
  Free(work);
}

// The KV-only reads build their document in a growing arena: one 1 MiB block
// for a typical read, up to kHistoryArenaBytes for the largest bounded one
// (a 90-day hourly chart; 400 days of counts with wide ranked holder lists).
bool TokenWorkspaces::MakeHistory(Work* work) {
  Free(work);
  work->env = &env_;
  work->arena = tj::GrowingArena(kHistoryArenaBytes);
  work->growing_blocks.reset(&work->arena);
  work->scratch_storage = base::AllocateStorage<uint8_t[]>(kScratchBytes);
  work->scratch = work->scratch_storage.get();
  work->scratch_capacity = kScratchBytes;
  work->background = false;
  work->deadline_ms = providers::TokenMonotonicMs() + kTaskBudgetMs;
  if (!work->scratch) { FreeHistory(work); return false; }
  return true;
}

}  // namespace fortis::tokens
