#include <algorithm>
// KV log compaction.
#include "kv_store_internal.h"

#include "threading.h"

namespace fortis::storage {

// ---- Compaction -----------------------------------------------------------
//
// Phases (all state under mutex_; one compactor at a time via compact_busy_):
//   Copy     Walk the index in key order from a cursor. One locked step stages
//            up to a slice of records that predate the run (offset < base):
//            headers and keys go into a frame buffer and each node's
//            next_offset is set to its position in the temporary file. Values
//            are read from a dup of the live log and the frame is written
//            outside the mutex. Records written since the run began are left
//            to the tail; expired records (maintenance runs) are dropped from
//            the index instead.
//   CatchUp  Copy the live log's committed tail [base, end) verbatim outside
//            the mutex in slices, so tombstones, expiry and multi-record
//            transaction frames keep their order and atomicity.
//   Swap     Under the mutex, once at most swap_tail_bytes remain: copy the
//            rest, fsync, rename over fortis.kv, fsync the directory, then
//            switch descriptors. Nothing is acknowledged in between.
//   Remap    Readers translate pre-swap nodes on access (Resolve); slices
//            normalize them so the next run starts from one generation.
// Any failure before the rename deletes the temporary file and leaves the
// live log, which never stopped being authoritative, in service.

void KvStore::KvCompactor::NotePauseUnlocked(uint64_t start_us, bool swap) {
  const uint64_t held = MonotonicUs() - start_us;
  RecordPhase(owner_.activity_, Phase::CompactionHold, held);
  ++stats_.pauses;
  stats_.pause_us_total += held;
  if (held > stats_.pause_us_max) stats_.pause_us_max = held;
  if (swap && held > stats_.swap_pause_us_max) stats_.swap_pause_us_max = held;
  if (held > stats_.lock_hold_us_max) stats_.lock_hold_us_max = held;
}

bool KvStore::KvCompactor::LowDiskUnlocked(uint64_t required) const {
#ifdef FORTIS_KV_TEST_FAILURES
  if (const char* forced = getenv("FORTIS_KV_TEST_FREE_BYTES"))
    return strtoull(forced, nullptr, 10) < required;
#endif
  struct statvfs volume {};
  // An unknown free-space figure (no statvfs in the VFS) does not block;
  // ENOSPC on the temporary file then aborts the run instead.
  if (owner_.log_.fd_ < 0 || fstatvfs(owner_.log_.fd_, &volume) != 0) return false;
  const uint64_t unit = volume.f_frsize ? volume.f_frsize : volume.f_bsize;
  return static_cast<uint64_t>(volume.f_bavail) * unit < required;
}

void KvStore::KvCompactor::ReleaseCompactionUnlocked() {
  if (compact_fd_ >= 0) {
    close(compact_fd_);
    unlink(compact_path_);
  }
  if (compact_source_fd_ >= 0) close(compact_source_fd_);
  compact_fd_ = -1;
  compact_source_fd_ = -1;
  compact_buffer_.reset(); compact_buffer_size_ = 0;
  compact_items_.reset(); compact_items_capacity_ = 0;
  compact_abandoned_ = false;
  compact_cursor_set_ = false;
  compact_end_ = 0;
  phase_ = KvCompactionPhase::Idle;
}

void KvStore::KvCompactor::AbortCompactionUnlocked(KvCompactionError error) {
  stats_.last_error = error;
  ++stats_.runs_failed;
  compact_failed_ = true;
  // The busy compactor still owns the descriptors; it releases them when it
  // retakes the mutex and sees the flag.
  if (compact_busy_) { compact_abandoned_ = true; return; }
  ReleaseCompactionUnlocked();
}

bool KvStore::KvCompactor::BeginCompactionUnlocked(const KvCompactionPolicy& policy, bool expire) {
  if (owner_.log_.fd_ < 0 || phase_ != KvCompactionPhase::Idle || compact_busy_) return false;
  const size_t slice_bytes = policy.slice_bytes ? policy.slice_bytes : 1;
  const size_t tail_bytes = policy.swap_tail_bytes > slice_bytes ? policy.swap_tail_bytes
                                                                  : slice_bytes;
  // One slice, or one maximal record, plus the frame header and count.
  compact_buffer_size_ = tail_bytes + kHeader + 4 + kRecord + kMaxKey + kMaxValue;
  compact_items_capacity_ = policy.slice_records ? policy.slice_records : 1;
  compact_buffer_ = base::AllocateStorage<char[]>(compact_buffer_size_);
  compact_items_ = base::AllocateStorage<SliceItem[]>(compact_items_capacity_ * sizeof(SliceItem));
  const int length = snprintf(compact_path_, sizeof(compact_path_), "%s.compact", owner_.log_.path_);
  if (!compact_buffer_ || !compact_items_ || length <= 0 ||
      static_cast<size_t>(length) >= sizeof(compact_path_)) {
    stats_.last_error = KvCompactionError::Allocation;
    ++stats_.runs_failed;
    ReleaseCompactionUnlocked();
    return false;
  }
  compact_fd_ = open(compact_path_, O_RDWR | O_CREAT | O_TRUNC, 0600);
  compact_source_fd_ = compact_fd_ >= 0 ? dup(owner_.log_.fd_) : -1;
  if (compact_fd_ < 0 || compact_source_fd_ < 0) {
    stats_.last_error = KvCompactionError::CopyWrite;
    ++stats_.runs_failed;
    ReleaseCompactionUnlocked();
    return false;
  }
  compact_base_ = owner_.log_.end_;
  // Commit slots: frames start after the replacement's slot blocks, which
  // the swap writes once the replacement's end is known.
  compact_end_ = owner_.log_.LogBase();
  compact_copy_end_ = compact_end_;
  compact_tail_ = owner_.log_.end_;
  compact_synced_ = 0;
  compact_rounds_ = 0;
  compact_cursor_set_ = false;
  compact_expire_ = expire;
  compact_abandoned_ = false;
  compact_failed_ = false;
  phase_ = KvCompactionPhase::Copy;
  ++stats_.runs_started;
  return true;
}

KvStore::StepResult KvStore::KvCompactor::CopyStepLocked(int64_t now_ms, const KvCompactionPolicy& policy,
                                            uint64_t* lock_start_us) {
  if (LowDiskUnlocked(policy.reserve_bytes)) {
    AbortCompactionUnlocked(KvCompactionError::LowDisk);
    return StepResult::Failed;
  }
  // Frames stay far below kMaxFrame so replay accepts every one.
  size_t slice_bytes = policy.slice_bytes < compact_buffer_size_ - kHeader - 4
      ? policy.slice_bytes : compact_buffer_size_ - kHeader - 4;
  if (slice_bytes > kMaxFrame / 2) slice_bytes = kMaxFrame / 2;
  size_t record_limit = policy.slice_records && policy.slice_records < compact_items_capacity_
      ? policy.slice_records : compact_items_capacity_;
  if (record_limit > 65535) record_limit = 65535;
  const size_t node_limit = policy.slice_nodes ? policy.slice_nodes : 1;
  auto expired_storage = base::AllocateStorage<const char*[]>(record_limit * sizeof(const char*));
  const char** expired = expired_storage.get();
  if (!expired) { AbortCompactionUnlocked(KvCompactionError::Allocation); return StepResult::Failed; }
  char* const frame = compact_buffer_.get();
  const uint64_t destination = compact_end_;
  size_t body = 4, count = 0, dropped = 0, visited = 0;
  const char* last = nullptr;
  auto stage = [&](KvNode* node) -> bool {
    if (visited >= node_limit) return false;
    if (node->offset >= compact_base_) {  // Rewritten since the run began.
      ++visited; last = node->key;
      return true;
    }
    if (compact_expire_ && node->expiry && node->expiry <= now_ms) {
      if (dropped == record_limit) return false;
      expired[dropped++] = node->key;
      ++visited; last = node->key;
      return true;
    }
    const size_t key_length = strlen(node->key);
    const uint64_t need = RecordBytes(key_length, node->length);
    if (count && (count == record_limit || body + need > slice_bytes)) return false;
    char* record = frame + kHeader + body;
    record[0] = 0;
    record[1] = static_cast<char>(key_length);
    record[2] = static_cast<char>(key_length >> 8);
    Put32(record + 3, node->length);
    Put64(record + 7, static_cast<uint64_t>(node->expiry));
    memcpy(record + kRecord, node->key, key_length);
    const size_t position = kHeader + body + kRecord + key_length;
    compact_items_.get()[count++] = {node->offset, node->length, static_cast<uint32_t>(position)};
    node->next_offset = destination + position;
    body += need;
    ++visited; last = node->key;
    return true;
  };
  const bool finished = VisitAfter(owner_.index_.root_.get(), compact_cursor_set_ ? compact_cursor_ : nullptr, stage);
  if (last) {
    memcpy(compact_cursor_, last, strlen(last) + 1);
    compact_cursor_set_ = true;
  }
  for (size_t i = 0; i < dropped; ++i) {
    char key[kMaxKey + 1];
    memcpy(key, expired[i], strlen(expired[i]) + 1);
    uint64_t removed = 0;
    owner_.index_.root_.reset(Erase(owner_.index_.root_.release(), key, &removed));
    if (removed) { --owner_.index_.size_; owner_.index_.live_bytes_ -= removed; }
  }
  expired_storage.reset();
  stats_.expired_dropped += dropped;
  if (count) {
    // Stage the frame, then read values and write it outside the mutex.
    Put32(frame + kHeader, static_cast<uint32_t>(count));
    const size_t frame_size = kHeader + body;
    const bool sync = destination + frame_size - compact_synced_ >= policy.sync_every_bytes;
    const int source = compact_source_fd_, target = compact_fd_;
    compact_busy_ = true;
    NotePauseUnlocked(*lock_start_us, false);
    owner_.mutex_.Unlock();
    KvCompactionError error = KvCompactionError::None;
    for (size_t i = 0; i < count && error == KvCompactionError::None; ++i) {
      const SliceItem& item = compact_items_.get()[i];
      if (item.length && !ReadAll(source, frame + item.position, item.length, item.source, owner_.activity_))
        error = KvCompactionError::CopyRead;
    }
    if (error == KvCompactionError::None) {
      std::copy_n("FKV1", 4, frame);
      Put32(frame + 4, static_cast<uint32_t>(body));
      Put32(frame + 8, Checksum(frame + kHeader, body));
      if (TestFailure("compact_copy_write") || !WriteAll(target, frame, frame_size, destination))
        error = KvCompactionError::CopyWrite;
    }
    if (error == KvCompactionError::None && sync &&
        (TestFailure("compact_copy_fsync") || SyncFile(target, owner_.activity_) != 0))
      error = KvCompactionError::CopySync;
    TestKill("copy_slice");
    owner_.mutex_.Lock();
    *lock_start_us = MonotonicUs();
    compact_busy_ = false;
    owner_.compactor_idle_.Broadcast();
    if (compact_abandoned_) { ReleaseCompactionUnlocked(); return StepResult::Failed; }
    if (error != KvCompactionError::None) {
      AbortCompactionUnlocked(error);
      return StepResult::Failed;
    }
    if (sync) { compact_synced_ = destination + frame_size; ++stats_.fsyncs; }
    compact_end_ = destination + frame_size;
    stats_.bytes_copied += frame_size;
  }
  if (finished) {
    compact_copy_end_ = compact_end_;
    compact_tail_ = compact_base_;
    compact_cursor_set_ = false;
    phase_ = KvCompactionPhase::CatchUp;
  }
  return StepResult::Progress;
}

KvStore::StepResult KvStore::KvCompactor::CatchUpStepLocked(const KvCompactionPolicy& policy,
                                               uint64_t* lock_start_us) {
  const uint64_t remaining = owner_.log_.end_ - compact_tail_;
  if (remaining <= policy.swap_tail_bytes && compact_synced_ < compact_end_) {
    // Flush the copied bytes outside the mutex, so the swap's own fsync
    // covers at most the final tail.
    const int target = compact_fd_;
    const uint64_t synced = compact_end_;
    compact_busy_ = true;
    NotePauseUnlocked(*lock_start_us, false);
    owner_.mutex_.Unlock();
    const bool ok = !TestFailure("compact_copy_fsync") && SyncFile(target, owner_.activity_) == 0;
    TestKill("catch_up_fsync");
    owner_.mutex_.Lock();
    *lock_start_us = MonotonicUs();
    compact_busy_ = false;
    owner_.compactor_idle_.Broadcast();
    if (compact_abandoned_) { ReleaseCompactionUnlocked(); return StepResult::Failed; }
    if (!ok) { AbortCompactionUnlocked(KvCompactionError::CopySync); return StepResult::Failed; }
    compact_synced_ = synced;
    ++stats_.fsyncs;
    return StepResult::Progress;
  }
  if (remaining <= policy.swap_tail_bytes) return SwapLocked();
  if (++compact_rounds_ > policy.max_catch_up_rounds) {
    AbortCompactionUnlocked(KvCompactionError::Starved);
    return StepResult::Failed;
  }
  if (LowDiskUnlocked(policy.reserve_bytes)) {
    AbortCompactionUnlocked(KvCompactionError::LowDisk);
    return StepResult::Failed;
  }
  uint64_t chunk = policy.slice_bytes ? policy.slice_bytes : 1;
  if (chunk > compact_buffer_size_) chunk = compact_buffer_size_;
  if (chunk > remaining) chunk = remaining;
  const uint64_t source_offset = compact_tail_, destination = compact_end_;
  const bool sync = destination + chunk - compact_synced_ >= policy.sync_every_bytes;
  const int source = compact_source_fd_, target = compact_fd_;
  compact_busy_ = true;
  NotePauseUnlocked(*lock_start_us, false);
  owner_.mutex_.Unlock();
  // Committed frames below end_ never change, so the dup reads them freely.
  bool ok = ReadAll(source, compact_buffer_.get(), static_cast<size_t>(chunk), source_offset, owner_.activity_) &&
            !TestFailure("compact_catchup_write") &&
            WriteAll(target, compact_buffer_.get(), static_cast<size_t>(chunk), destination);
  if (ok && sync) ok = !TestFailure("compact_copy_fsync") && SyncFile(target, owner_.activity_) == 0;
  TestKill("catch_up_slice");
  owner_.mutex_.Lock();
  *lock_start_us = MonotonicUs();
  compact_busy_ = false;
  owner_.compactor_idle_.Broadcast();
  if (compact_abandoned_) { ReleaseCompactionUnlocked(); return StepResult::Failed; }
  if (!ok) { AbortCompactionUnlocked(KvCompactionError::TailWrite); return StepResult::Failed; }
  if (sync) { compact_synced_ = destination + chunk; ++stats_.fsyncs; }
  compact_tail_ += chunk;
  compact_end_ += chunk;
  stats_.tail_bytes_copied += chunk;
  return StepResult::Progress;
}

KvStore::StepResult KvStore::KvCompactor::SwapLocked() {
  while (compact_tail_ < owner_.log_.end_) {
    uint64_t chunk = owner_.log_.end_ - compact_tail_;
    if (chunk > compact_buffer_size_) chunk = compact_buffer_size_;
    if (!ReadAll(owner_.log_.fd_, compact_buffer_.get(), static_cast<size_t>(chunk), compact_tail_, owner_.activity_) ||
        TestFailure("compact_swap_tail") ||
        !WriteAll(compact_fd_, compact_buffer_.get(), static_cast<size_t>(chunk), compact_end_)) {
      AbortCompactionUnlocked(KvCompactionError::TailWrite);
      return StepResult::Failed;
    }
    compact_tail_ += chunk;
    compact_end_ += chunk;
    stats_.tail_bytes_copied += chunk;
  }
  // The replacement is the next generation and carries its own slots, so
  // the rename below publishes log and commit pointer together. The old log
  // keeps its own slots and stays authoritative until then.
  const CommitSlot next{owner_.log_.commit_generation_ + 1, owner_.log_.commit_sequence_ + 1, compact_end_};
  if (owner_.CommitSlots() &&
      (TestFailure("compact_slot_write") || !WriteSlot(compact_fd_, next) ||
       !WriteSlot(compact_fd_, {next.generation, next.sequence + 1, next.committed}))) {
    AbortCompactionUnlocked(KvCompactionError::TailWrite);
    return StepResult::Failed;
  }
  TestKill("swap_tail_copied");
  if (TestFailure("compact_before_fsync") || SyncFile(compact_fd_, owner_.activity_) != 0) {
    AbortCompactionUnlocked(KvCompactionError::SwapSync);
    return StepResult::Failed;
  }
  ++stats_.fsyncs;
  TestKill("swap_file_synced");
  if (TestFailure("compact_after_file_fsync") || rename(compact_path_, owner_.log_.path_) != 0) {
    AbortCompactionUnlocked(KvCompactionError::Rename);
    return StepResult::Failed;
  }
  // Committed: the replacement holds every acknowledged frame and is now the
  // authoritative log. If the rename's directory entry is not yet durable,
  // writes stay refused (see WriteUnlocked) until a directory fsync works.
  TestKill("swap_renamed");
  if (TestFailure("compact_after_rename") || !SyncDirectory(owner_.log_.directory_, owner_.activity_)) {
    owner_.log_.directory_sync_pending_ = true;
    ++stats_.directory_sync_failures;
  } else {
    owner_.log_.directory_sync_pending_ = false;
    ++stats_.fsyncs;
  }
  TestKill("swap_directory_synced");
  const uint64_t old_end = owner_.log_.end_;
  close(owner_.log_.fd_);
  close(compact_source_fd_);
  owner_.log_.fd_ = compact_fd_;
  compact_fd_ = -1;
  compact_source_fd_ = -1;
  remap_base_ = compact_base_;
  remap_copy_end_ = compact_copy_end_;
  owner_.log_.generation_ ^= 1;
  owner_.log_.end_ = compact_end_;
  if (owner_.CommitSlots()) {
    owner_.log_.commit_generation_ = next.generation;
    owner_.log_.commit_sequence_ = next.sequence + 1;
  }
  if (old_end > owner_.log_.end_) stats_.bytes_reclaimed += old_end - owner_.log_.end_;
  compact_buffer_.reset(); compact_buffer_size_ = 0;
  compact_items_.reset(); compact_items_capacity_ = 0;
  compact_end_ = 0;
  compact_cursor_set_ = false;
  phase_ = KvCompactionPhase::Remap;
  return StepResult::Progress;
}

KvStore::StepResult KvStore::KvCompactor::RemapStepLocked(const KvCompactionPolicy& policy) {
  const size_t node_limit = policy.slice_nodes ? policy.slice_nodes : 1;
  size_t visited = 0;
  const char* last = nullptr;
  auto remap = [&](KvNode* node) -> bool {
    if (visited++ >= node_limit) return false;
    if (node->generation != owner_.log_.generation_) {
      node->offset = owner_.index_.Resolve(node);
      node->generation = owner_.log_.generation_;
    }
    last = node->key;
    return true;
  };
  const bool finished = VisitAfter(owner_.index_.root_.get(), compact_cursor_set_ ? compact_cursor_ : nullptr, remap);
  if (last) {
    memcpy(compact_cursor_, last, strlen(last) + 1);
    compact_cursor_set_ = true;
  }
  TestKill("remap_slice");
  if (!finished) return StepResult::Progress;
  compact_cursor_set_ = false;
  phase_ = KvCompactionPhase::Idle;
  compact_failed_ = false;
  ++stats_.runs_completed;
  return StepResult::Completed;
}

KvStore::StepResult KvStore::KvCompactor::CompactionStep(int64_t now_ms, const KvCompactionPolicy& policy) {
  fortis::base::LockGuard lock(owner_.mutex_);
  if (!lock.locked()) return StepResult::Failed;
  owner_.WaitMaintenanceUnlocked();
  uint64_t lock_start = MonotonicUs();
  StepResult result = StepResult::Idle;
  bool swap = false;
  const bool active = owner_.log_.fd_ >= 0 && phase_ != KvCompactionPhase::Idle;
  if (owner_.log_.fd_ < 0) {
    result = StepResult::Failed;
  } else if (phase_ == KvCompactionPhase::Copy) {
    result = CopyStepLocked(now_ms, policy, &lock_start);
  } else if (phase_ == KvCompactionPhase::CatchUp) {
    swap = owner_.log_.end_ - compact_tail_ <= policy.swap_tail_bytes && compact_synced_ >= compact_end_;
    result = CatchUpStepLocked(policy, &lock_start);
  } else if (phase_ == KvCompactionPhase::Remap) {
    result = RemapStepLocked(policy);
  }
  if (active) NotePauseUnlocked(lock_start, swap);
  return result;
}

bool KvStore::KvCompactor::Compact() {
  const KvCompactionPolicy policy;
  // Finish a run already in progress (its records predate this call only up
  // to its base), then run a fresh one over everything written so far.
  for (int run = 0; run < 2; ++run) {
    fortis::base::UniqueLock run_lock(owner_.mutex_);
    if (!run_lock.locked()) return false;
    owner_.WaitMaintenanceUnlocked();
    const uint64_t completed = stats_.runs_completed;
    const bool inherited = phase_ != KvCompactionPhase::Idle;
    const bool running = inherited || (owner_.log_.fd_ >= 0 && BeginCompactionUnlocked(policy, false));
    run_lock.Unlock();
    if (!running) return false;
    StepResult result = StepResult::Progress;
    while (result == StepResult::Progress) result = CompactionStep(0, policy);
    if (result == StepResult::Idle) {
      // Another caller finished (or failed) the shared run.
      TimedGuard lock(owner_.mutex_.native(), &stats_.lock_hold_us_max);
      result = stats_.runs_completed > completed && !compact_failed_
          ? StepResult::Completed : StepResult::Failed;
    }
    if (!inherited) return result == StepResult::Completed;
  }
  return false;
}

bool KvStore::Compact() {
  return compactor_.Compact();
}

bool KvStore::KvCompactor::CompactionDueUnlocked(const KvCompactionPolicy& policy, int64_t monotonic_ms) {
  if (!schedule_anchored_) {
    schedule_anchored_ = true;
    schedule_anchor_ms_ = monotonic_ms;
  }
  if (monotonic_ms - schedule_anchor_ms_ < policy.startup_delay_ms) return false;
  if (monotonic_ms < next_attempt_ms_) return false;
  const uint64_t file = owner_.log_.end_ > owner_.log_.LogBase() ? owner_.log_.end_ - owner_.log_.LogBase() : 0;  // Frame bytes.
  const uint64_t dead = file > owner_.index_.live_bytes_ ? file - owner_.index_.live_bytes_ : 0;
  const bool due = file >= policy.min_file_bytes &&
      (dead * 100 >= file * policy.dead_percent ||
       (dead >= policy.dead_bytes && dead * 100 >= file * policy.dead_bytes_min_percent));
  if (!due) { stats_.blocked_low_disk = false; return false; }
  const uint64_t required = owner_.index_.live_bytes_ + owner_.index_.live_bytes_ / 8 + policy.reserve_bytes;
  if (LowDiskUnlocked(required)) {
    ++stats_.skipped_low_disk;
    stats_.blocked_low_disk = true;
    stats_.last_error = KvCompactionError::LowDisk;
    backoff_ms_ = backoff_ms_ ? backoff_ms_ * 2 : policy.retry_backoff_ms;
    if (backoff_ms_ > policy.max_backoff_ms) backoff_ms_ = policy.max_backoff_ms;
    next_attempt_ms_ = monotonic_ms + backoff_ms_;
    return false;
  }
  stats_.blocked_low_disk = false;
  return true;
}

bool KvStore::KvCompactor::MaintainCompaction(int64_t now_ms, int64_t monotonic_ms,
                                 const KvCompactionPolicy& policy,
                                 base::FunctionRef<bool()> cancelled) {
  {
    TimedGuard lock(owner_.mutex_.native(), &stats_.lock_hold_us_max, &owner_.activity_, Phase::CompactionHold);
    if (!lock.locked() || owner_.log_.fd_ < 0) return false;
    if (owner_.log_.directory_sync_pending_ && !TestFailure("directory_sync_retry") &&
        SyncDirectory(owner_.log_.directory_, owner_.activity_))
      owner_.log_.directory_sync_pending_ = false;
    if (phase_ == KvCompactionPhase::Idle && !compact_busy_) {
      if (!CompactionDueUnlocked(policy, monotonic_ms)) return false;
      if (!BeginCompactionUnlocked(policy, true)) {
        backoff_ms_ = backoff_ms_ ? backoff_ms_ * 2 : policy.retry_backoff_ms;
        if (backoff_ms_ > policy.max_backoff_ms) backoff_ms_ = policy.max_backoff_ms;
        next_attempt_ms_ = monotonic_ms + backoff_ms_;
        return false;
      }
      next_attempt_ms_ = monotonic_ms + policy.min_interval_ms;
    }
  }
  const uint64_t deadline = MonotonicUs() + static_cast<uint64_t>(policy.tick_budget_ms) * 1000u;
  for (uint32_t slice = 0; slice < policy.tick_max_slices; ++slice) {
    if (cancelled && cancelled()) return true;
    const StepResult result = CompactionStep(now_ms, policy);
    if (result == StepResult::Completed) {
      TimedGuard lock(owner_.mutex_.native(), &stats_.lock_hold_us_max, &owner_.activity_, Phase::CompactionHold);
      backoff_ms_ = 0;
      return false;
    }
    if (result == StepResult::Failed) {
      TimedGuard lock(owner_.mutex_.native(), &stats_.lock_hold_us_max, &owner_.activity_, Phase::CompactionHold);
      backoff_ms_ = backoff_ms_ ? backoff_ms_ * 2 : policy.retry_backoff_ms;
      if (backoff_ms_ > policy.max_backoff_ms) backoff_ms_ = policy.max_backoff_ms;
      const int64_t retry = monotonic_ms + backoff_ms_;
      if (retry > next_attempt_ms_) next_attempt_ms_ = retry;
      return false;
    }
    if (result == StepResult::Idle) return false;
    if (MonotonicUs() >= deadline) break;
  }
  return true;
}

bool KvStore::MaintainCompaction(int64_t now_ms, int64_t monotonic_ms,
                                 const KvCompactionPolicy& policy,
                                 base::FunctionRef<bool()> cancelled) {
  return compactor_.MaintainCompaction(now_ms, monotonic_ms, policy, cancelled);
}

}  // namespace fortis::storage
