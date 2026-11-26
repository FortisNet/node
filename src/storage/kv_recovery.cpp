#include "ownership.h"
#include <atomic>
// Opening and recovering the KV log: recovery after a failed append,
// rebuilding the durable prefix, legacy-log migration and replay.
#include "kv_store_internal.h"
#include "log.h"

namespace fortis::storage {

bool KvStore::KvRecovery::RecoverIfNeeded(int64_t monotonic_ms) {
  uint64_t generation = 0, sequence = 0;
  {
    TimedGuard lock(owner_.mutex_.native(), &owner_.compactor_.stats_.lock_hold_us_max, &owner_.activity_, Phase::RecoveryHold);
    if (!lock.locked()) return false;
    if (!recovery_pending_) return owner_.log_.fd_ >= 0;
    if (recovery_running_ || owner_.compactor_.compact_busy_ || recovery_attempts_ >= kRecoveryAttempts ||
        monotonic_ms < next_recovery_ms_) return false;
    recovery_running_ = true;
    ++recovery_attempts_;
    generation = owner_.log_.commit_generation_;
    sequence = owner_.log_.commit_sequence_;
  }

  // Keep the original writer lock. Build a private index while the live
  // store fails requests promptly. The failed caller's mutation is not retried.
  const uint64_t started = MonotonicUs();
  KvStore replay(false, false, owner_.durability_);
  const char* failed_stage = nullptr;
  const uint64_t deadline = started + kRecoveryReplayBudgetUs;
  if (owner_.CommitSlots()) {
    failed_stage = LoadCommitted(replay, recovery_discard_tail_, recovery_durable_end_,
                                 generation, sequence, deadline);
  } else if (recovery_discard_tail_ && !recovery_truncate_only_) {
    failed_stage = RecoverAppend(replay, deadline);
  } else {
    replay.log_.fd_ = open(owner_.log_.path_, O_RDWR);
    if (replay.log_.fd_ < 0) {
      failed_stage = "open";
    } else if (recovery_truncate_only_) {
      struct stat status {};
      if (fstat(replay.log_.fd_, &status) != 0) {
        failed_stage = "recovery_stat";
      } else if (status.st_size < 0 ||
                 static_cast<uint64_t>(status.st_size) < recovery_durable_end_) {
        errno = EIO;
        failed_stage = "recovery_short_log";
      } else if (ftruncate(replay.log_.fd_, static_cast<off_t>(recovery_durable_end_)) != 0 ||
                 SyncFile(replay.log_.fd_, owner_.activity_) != 0) {
        failed_stage = "recovery_truncate";
        // A failed truncation barrier now requires a fresh inode as well.
        recovery_truncate_only_ = false;
      }
    }
    if (!failed_stage &&
        (TestFailure("recovery_replay") || !replay.recovery_.Replay(deadline, &owner_.activity_))) {
      failed_stage = "replay";
    }
    if (!failed_stage && recovery_truncate_only_)
      failed_stage = ClearRecoveryOffset();
  }
  const int error = failed_stage ? (errno ? errno : EIO) : 0;

  TimedGuard lock(owner_.mutex_.native(), &owner_.compactor_.stats_.lock_hold_us_max, &owner_.activity_, Phase::RecoveryHold);
  if (!lock.locked()) return false;
  recovery_running_ = false;
  owner_.compactor_idle_.Broadcast();
  if (failed_stage) {
    const int64_t elapsed_ms = static_cast<int64_t>((MonotonicUs() - started) / 1000);
    next_recovery_ms_ = monotonic_ms + elapsed_ms +
        (kRecoveryInitialBackoffMs << (recovery_attempts_ - 1));
    fortis::base::LogError("kv", "KV recovery failed: attempt=%u max_attempts=%u stage=%s errno=%d exhausted=%d",
            recovery_attempts_, kRecoveryAttempts, failed_stage, error,
            recovery_attempts_ == kRecoveryAttempts);
    return false;
  }
  owner_.AdoptUnlocked(replay);
  fortis::base::LogInfo("kv", "KV recovery succeeded: attempts=%u elapsed_ms=%llu log_bytes=%llu",
          recovery_attempts_, static_cast<unsigned long long>((MonotonicUs() - started) / 1000),
          static_cast<unsigned long long>(owner_.log_.end_));
  recovery_pending_ = false;
  recovery_discard_tail_ = false;
  recovery_truncate_only_ = false;
  recovery_offset_saved_ = false;
  recovery_durable_end_ = 0;
  owner_.log_.last_write_failure_stage_ = nullptr;
  recovery_attempts_ = 0;
  owner_.PublishCacheEpochUnlocked();
  return true;
}

bool KvStore::RecoverIfNeeded(int64_t monotonic_ms) {
  return recovery_.RecoverIfNeeded(monotonic_ms);
}

const char* KvStore::KvRecovery::SaveRecoveryOffset() {
  char marker[sizeof(owner_.compactor_.compact_path_)], temporary[sizeof(owner_.compactor_.compact_path_)];
  snprintf(marker, sizeof(marker), "%s.recover", owner_.log_.path_);
  snprintf(temporary, sizeof(temporary), "%s.recover.tmp", owner_.log_.path_);
  const int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return "recovery_marker_open";
  char offset[16];
  Put64(offset, recovery_durable_end_);
  Put64(offset + 8, ~recovery_durable_end_);
  const bool saved = WriteAll(fd, offset, sizeof(offset), 0) && SyncFile(fd, owner_.activity_) == 0;
  const int error = errno;
  const bool closed = close(fd) == 0;
  if (!saved) errno = error;
  if (!saved || !closed) return "recovery_marker_file_sync";
  if (rename(temporary, marker) != 0) return "recovery_marker_rename";
  TestKill("append_before_marker_directory_sync");
  if (!SyncDirectory(owner_.log_.directory_, owner_.activity_)) return "recovery_marker_directory_sync";
  recovery_offset_saved_ = true;
  return nullptr;
}

const char* KvStore::KvRecovery::ClearRecoveryOffset() {
  char marker[sizeof(owner_.compactor_.compact_path_)];
  snprintf(marker, sizeof(marker), "%s.recover", owner_.log_.path_);
  if (unlink(marker) != 0 && errno != ENOENT) return "recovery_marker_unlink";
  recovery_offset_saved_ = false;
  if (!SyncDirectory(owner_.log_.directory_, owner_.activity_)) return "recovery_marker_remove_sync";
  return nullptr;
}

const char* KvStore::KvRecovery::RecoverAppend(KvStore& replacement, uint64_t deadline_us) {
  if (!recovery_offset_saved_) {
    if (const char* stage = SaveRecoveryOffset()) return stage;
  }
  const int source = open(owner_.log_.path_, O_RDONLY);
  if (source < 0) return "open";
  const char* stage = RebuildDurablePrefix(source, 0, recovery_durable_end_, deadline_us,
                                           replacement);
  const int error = errno;
  close(source);
  errno = error;
  if (stage) return stage;
  return ClearRecoveryOffset();
}

const char* KvStore::KvRecovery::RebuildDurablePrefix(int source, uint64_t start, uint64_t end,
                                         uint64_t deadline_us, KvStore& replacement,
                                         uint64_t generation, uint64_t sequence,
                                         bool migrating_legacy) {
  // Retrying fsync on the original inode can succeed after writeback has
  // already marked the failed pages clean. Rewrite the confirmed prefix
  // into a new inode and leave the failed append out of the replacement.
  char temporary[sizeof(owner_.compactor_.compact_path_)];
  snprintf(temporary, sizeof(temporary), "%s.compact", owner_.log_.path_);
  if (unlink(temporary) != 0 && errno != ENOENT) return "recovery_unlink";
  replacement.log_.fd_ = open(temporary, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (replacement.log_.fd_ < 0) return "recovery_create";

  const char* stage = nullptr;
  const uint64_t base = replacement.log_.LogBase();
  uint64_t length = base + (end - start);
  constexpr size_t kCopyBytes = 64 * 1024;
  auto buffer_storage = base::Buffer::Allocate(kCopyBytes);
  char* buffer = buffer_storage.chars();
  if (!buffer) stage = "recovery_allocation";
  for (uint64_t offset = start; !stage && offset < end;) {
    const size_t size = static_cast<size_t>(end - offset < kCopyBytes ? end - offset : kCopyBytes);
    if (MonotonicUs() >= deadline_us) {
      errno = ETIMEDOUT;
      stage = "recovery_copy";
    } else if (!ReadAll(source, buffer, size, offset, owner_.activity_) ||
               !WriteAll(replacement.log_.fd_, buffer, size, base + (offset - start))) {
      stage = "recovery_copy";
    }
    offset += size;
  }
  buffer_storage.reset();
  if (!stage && migrating_legacy) {
    // Replay the copied legacy payload once, at its final offsets. A torn
    // legacy tail is trimmed only on the unpublished inode.
    errno = 0;
    if (length == base && ftruncate(replacement.log_.fd_, static_cast<off_t>(base)) != 0) {
      stage = "migration_initialize";
    } else if (TestFailure("recovery_replay") ||
               !replacement.recovery_.Replay(deadline_us, &owner_.activity_, true)) {
      stage = "migration_replay";
    } else {
      length = replacement.log_.end_;
      replacement.log_.commit_generation_ = generation;
      replacement.log_.commit_sequence_ = sequence + 1;
    }
  }
  // Both slots name the copied end, so either one alone recovers it.
  if (!stage && replacement.CommitSlots() &&
      (!WriteSlot(replacement.log_.fd_, {generation, sequence, length}) ||
       !WriteSlot(replacement.log_.fd_, {generation, sequence + 1, length})))
    stage = "recovery_slot_write";
  if (!stage && !migrating_legacy) {
    errno = 0;
    if (TestFailure("recovery_replay") ||
        !replacement.recovery_.Replay(deadline_us, &owner_.activity_) || replacement.log_.end_ != length)
      stage = "replay";
  }
  if (!stage) {
    if (TestFailure("recovery_file_fsync")) {
      errno = EIO;
      stage = "recovery_file_fsync";
    } else if (SyncFile(replacement.log_.fd_, owner_.activity_) != 0) {
      stage = "recovery_file_fsync";
    }
  }
  if (!stage) TestKill("rebuild_synced");
  if (!stage && rename(temporary, owner_.log_.path_) != 0) stage = "recovery_rename";
  if (!stage) {
    TestKill("rebuild_renamed");
    if (TestFailure("recovery_directory_fsync")) {
      errno = EIO;
      return "recovery_directory_fsync";
    }
    return SyncDirectory(owner_.log_.directory_, owner_.activity_) ? nullptr : "recovery_directory_fsync";
  }
  const int error = errno;
  unlink(temporary);
  errno = error;
  return stage;
}

// ---- Commit-slot open and recovery ----------------------------------------

bool KvStore::KvRecovery::OpenCommitLogUnlocked() {
  const uint64_t deadline = MonotonicUs() + kRecoveryReplayBudgetUs;
  KvStore replacement(false, false, owner_.durability_);
  const char* stage = nullptr;
  switch (DetectLogFormat(owner_.log_.path_, owner_.activity_)) {
    case LogFormat::Missing:
      // The log only appears, by rename, once both slots are synced.
      stage = RebuildDurablePrefix(-1, 0, 0, deadline, replacement, 1, 1);
      break;
    case LogFormat::RollbackMarker:
      stage = MigrateLegacyLog(replacement, deadline);
      break;
    case LogFormat::CommitSlots:
      stage = ReopenCommitLog(replacement, deadline);
      break;
    case LogFormat::Unknown:
      errno = EINVAL;
      stage = "log_format";
      break;
  }
  if (stage) {
    fortis::base::LogError("kv", "KV commit-slot open failed: stage=%s errno=%d", stage, errno);
    owner_.CloseUnlocked();
    return false;
  }
  owner_.AdoptUnlocked(replacement);
  fortis::base::LogInfo("kv", "KV durability: commit slots generation=%llu sequence=%llu log_bytes=%llu",
          static_cast<unsigned long long>(owner_.log_.commit_generation_),
          static_cast<unsigned long long>(owner_.log_.commit_sequence_),
          static_cast<unsigned long long>(owner_.log_.end_));
  return true;
}

// One-time conversion of a settled rollback-marker log (OpenUnlocked has
// already resolved any marker). Copy the frames behind fresh slots, then
// validate and index them at their new offsets. The legacy file stays
// authoritative until the synced replacement is renamed over it.
const char* KvStore::KvRecovery::MigrateLegacyLog(KvStore& replacement, uint64_t deadline_us) {
  const int source = open(owner_.log_.path_, O_RDONLY);
  if (source < 0) return "migration_open";
  struct stat info {};
  const char* stage = "migration_stat";
  if (fstat(source, &info) == 0 && info.st_size >= 0)
    stage = RebuildDurablePrefix(source, 0, static_cast<uint64_t>(info.st_size),
                                 deadline_us, replacement, 1, 1, true);
  const int error = errno;
  close(source);
  errno = error;
  if (!stage)
    fortis::base::LogInfo("kv", "KV migrated the rollback-marker log to commit slots: frame_bytes=%llu",
            static_cast<unsigned long long>(replacement.log_.end_ - kCommitLogBase));
  return stage;
}

// A restart can read a slot that the previous process failed to sync. Copy
// the selected prefix and validate its frames on the replacement before
// publishing it, leaving the current log untouched across a second crash.
const char* KvStore::KvRecovery::ReopenCommitLog(KvStore& replacement, uint64_t deadline_us) {
  const int source = open(owner_.log_.path_, O_RDONLY);
  if (source < 0) return "open";
  struct stat info {};
  CommitSlot slot;
  const char* stage = "commit_stat";
  if (fstat(source, &info) == 0 && info.st_size >= 0) {
    stage = "commit_slot_read";
    if (ReadCommitSlot(source, static_cast<uint64_t>(info.st_size), owner_.activity_, &slot))
      stage = RebuildDurablePrefix(source, kCommitLogBase, slot.committed, deadline_us,
                                   replacement, slot.generation + 1, slot.sequence + 1);
  }
  const int error = errno;
  close(source);
  errno = error;
  return stage;
}

// A frame write that never reached fsync leaves the published slot durable.
const char* KvStore::KvRecovery::LoadCommitted(KvStore& replacement, bool rebuild, uint64_t durable_end,
                                   uint64_t generation, uint64_t sequence,
                                   uint64_t deadline_us) {
  if (!rebuild) {
    replacement.log_.fd_ = open(owner_.log_.path_, O_RDWR);
    if (replacement.log_.fd_ < 0) return "open";
    if (TestFailure("recovery_replay") || !replacement.recovery_.Replay(deadline_us, &owner_.activity_))
      return "replay";
    const char* stage = replacement.recovery_.TruncateUnpublishedTail(owner_.activity_);
    if (!stage) return nullptr;
    fortis::base::LogWarn("kv", "KV tail cleanup failed: stage=%s errno=%d; rebuilding the log",
            stage, errno);
    durable_end = replacement.log_.end_;
    generation = replacement.log_.commit_generation_;
    sequence = replacement.log_.commit_sequence_;
    replacement.ResetLogUnlocked();
  }
  if (durable_end < kCommitLogBase) { errno = EINVAL; return "recovery_offset"; }
  const int source = open(owner_.log_.path_, O_RDONLY);
  if (source < 0) return "open";
  // A retry after a rename whose directory sync failed finds the generation
  // it published; the next one must still be newer.
  auto header_storage = base::Buffer::Allocate(kCommitLogBase);
  char* header = header_storage.chars();
  CommitSlot current;
  if (header && ReadAll(source, header, kCommitLogBase, 0, owner_.activity_) &&
      SelectSlot(header, &current)) {
    if (current.generation > generation) generation = current.generation;
    if (current.sequence > sequence) sequence = current.sequence;
  }
  header_storage.reset();
  const char* stage = RebuildDurablePrefix(source, kCommitLogBase, durable_end, deadline_us,
                                           replacement, generation + 1, sequence + 1);
  const int error = errno;
  close(source);
  errno = error;
  return stage;
}

const char* KvStore::KvRecovery::TruncateUnpublishedTail(KvAtomicActivityStats& io) {
  struct stat info {};
  if (fstat(owner_.log_.fd_, &info) != 0 || info.st_size < 0) return "commit_stat";
  if (static_cast<uint64_t>(info.st_size) == owner_.log_.end_) return nullptr;
  if (ftruncate(owner_.log_.fd_, static_cast<off_t>(owner_.log_.end_)) != 0) return "commit_truncate";
  return SyncFile(owner_.log_.fd_, io) == 0 ? nullptr : "commit_truncate_fsync";
}

bool KvStore::KvRecovery::Replay(uint64_t deadline_us, KvAtomicActivityStats* activity, bool migrating_legacy) {
  auto& io = activity ? *activity : owner_.activity_;
  struct stat info {};
  if (fstat(owner_.log_.fd_, &info) != 0 || info.st_size < 0) return false;
  const uint64_t file_size = static_cast<uint64_t>(info.st_size);
  uint64_t offset = migrating_legacy ? kCommitLogBase : 0, limit = file_size;
  const bool require_published_end = owner_.CommitSlots() && !migrating_legacy;
  if (require_published_end) {
    // Replay stops at the published end. Later bytes were never published
    // and are not read.
    CommitSlot slot;
    if (!ReadCommitSlot(owner_.log_.fd_, file_size, io, &slot)) return false;
    offset = kCommitLogBase;
    limit = slot.committed;
    owner_.log_.commit_generation_ = slot.generation;
    owner_.log_.commit_sequence_ = slot.sequence;
  }
  while (offset + kHeader <= limit) {
    if (deadline_us && MonotonicUs() >= deadline_us) { errno = ETIMEDOUT; return false; }
    char header[kHeader];
    if (!ReadAll(owner_.log_.fd_, header, sizeof(header), offset, io)) return false;
    if (memcmp(header, "FKV1", 4) != 0) return false;
    const uint32_t length = Read32(header + 4);
    if (length < 4 || length > kMaxFrame) return false;
    if (length > limit - offset - kHeader) {
      if (require_published_end) return false;  // A published end inside a frame.
      break;
    }
    auto body_storage = base::Buffer::Allocate(static_cast<size_t>(length) + 1);
    char* body = body_storage.chars();
    if (!body) return false;
    body[length] = '\0';
    const bool intact = ReadAll(owner_.log_.fd_, body, length, offset + kHeader, io) &&
                        Checksum(body, length) == Read32(header + 8);
    if (!intact) { body_storage.reset(); return false; }
    const uint32_t count = Read32(body);
    size_t cursor = 4;
    bool valid = true;
    for (uint32_t i = 0; i < count; ++i) {
      if (length - cursor < kRecord) { valid = false; break; }
      const uint8_t op = static_cast<uint8_t>(body[cursor]);
      const size_t key_length = static_cast<uint8_t>(body[cursor + 1]) |
          (static_cast<size_t>(static_cast<uint8_t>(body[cursor + 2])) << 8);
      const uint32_t value_length = Read32(body + cursor + 3);
      const uint64_t raw_expiry = Read64(body + cursor + 7);
      cursor += kRecord;
      if (!key_length || key_length > kMaxKey || value_length > kMaxValue ||
          raw_expiry > INT64_MAX || key_length + value_length > length - cursor ||
          op > 1 || (op == 1 && (value_length || raw_expiry))) { valid = false; break; }
      char saved = body[cursor + key_length];
      body[cursor + key_length] = '\0';
      const char* key = body + cursor;
      if (strlen(key) != key_length) valid = false;
      if (valid && op == 1) {
        uint64_t removed = 0;
        owner_.index_.root_.reset(Erase(owner_.index_.root_.release(), key, &removed));
        if (removed) { --owner_.index_.size_; owner_.index_.live_bytes_ -= removed; }
      } else if (valid) {
        bool added = false, ok = true;
        uint64_t previous = 0;
        owner_.index_.root_.reset(Put(owner_.index_.root_.release(), key, offset + kHeader + cursor + key_length,
                    value_length, static_cast<int64_t>(raw_expiry), owner_.log_.generation_,
                    &added, &ok, &previous));
        if (!ok) valid = false;
        if (added) ++owner_.index_.size_;
        if (ok) owner_.index_.live_bytes_ += RecordBytes(key_length, value_length) - previous;
      }
      body[cursor + key_length] = saved;
      cursor += key_length + value_length;
      if (!valid) break;
    }
    valid = valid && cursor == length;
    body_storage.reset();
    if (!valid) return false;
    offset += kHeader + length;
    TestKill("replay_frame");
  }
  if (require_published_end) {
    if (offset != limit) { errno = EINVAL; return false; }
    owner_.log_.end_ = offset;
    return true;
  }
  // A short final header is recoverable only when it begins with the frame
  // magic. Arbitrary trailing bytes are corruption, not an interrupted append.
  if (offset < file_size && file_size - offset < kHeader) {
    const size_t tail_length = static_cast<size_t>(file_size - offset);
    char tail[kHeader];
    if (!ReadAll(owner_.log_.fd_, tail, tail_length, offset, io) ||
        memcmp(tail, "FKV1", tail_length < 4 ? tail_length : 4) != 0)
      return false;
  }
  if (offset != file_size &&
      (ftruncate(owner_.log_.fd_, static_cast<off_t>(offset)) != 0 ||
       (!migrating_legacy && SyncFile(owner_.log_.fd_, io) != 0))) return false;
  owner_.log_.end_ = offset;
  return true;
}

}  // namespace fortis::storage
