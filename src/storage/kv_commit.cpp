#include <algorithm>
#include "ownership.h"
#include <atomic>
// The KV store's write path: frame validation and append, the two
// durability modes, group commit and compare-and-write.
#include "kv_store_internal.h"
#include "log.h"

#include "threading.h"

namespace fortis::storage {

bool KvStore::CommitCoordinator::ValidMutations(const KvMutation* mutations, size_t count, size_t* body_size) {
  if (!mutations || !count || count > 65535) return false;
  size_t size = 4;
  for (size_t i = 0; i < count; ++i) {
    const KvMutation& item = mutations[i];
    if (!ValidKey(item.key) || item.value_size > kMaxValue ||
        (!item.erase && item.value_size && !item.value) ||
        (item.erase && (item.value_size || item.expires_at_ms)) ||
        item.expires_at_ms < 0) return false;
    const size_t next = kRecord + strlen(item.key) + item.value_size;
    if (next > kMaxFrame - size) return false;
    size += next;
  }
  if (body_size) *body_size = size;
  return true;
}

KvStore::WriteFailure KvStore::KvLog::Append(const KvMutation* mutations, size_t count, size_t body_size) {
  auto body_storage = TestFailure("write_allocate") ? base::Buffer{} : base::Buffer::Allocate(body_size);
  char* body = body_storage.chars();
  if (!body) return {"frame_allocation", ENOMEM, false};
  auto offsets_storage = base::Buffer::Allocate(count * sizeof(uint64_t));
  auto* offsets = reinterpret_cast<uint64_t*>(offsets_storage.data());
  if (!offsets) { body_storage.reset(); return {"offset_allocation", ENOMEM, false}; }
  Put32(body, static_cast<uint32_t>(count));
  size_t cursor = 4;
  for (size_t i = 0; i < count; ++i) {
    const KvMutation& item = mutations[i];
    const size_t key_length = strlen(item.key);
    body[cursor] = item.erase ? 1 : 0;
    body[cursor + 1] = static_cast<char>(key_length);
    body[cursor + 2] = static_cast<char>(key_length >> 8);
    Put32(body + cursor + 3, item.value_size);
    Put64(body + cursor + 7, static_cast<uint64_t>(item.expires_at_ms));
    cursor += kRecord;
    memcpy(body + cursor, item.key, key_length);
    cursor += key_length;
    offsets[i] = end_ + kHeader + cursor;
    if (item.value_size) memcpy(body + cursor, item.value, item.value_size);
    cursor += item.value_size;
  }
  char header[kHeader];
  std::copy_n("FKV1", 4, header);
  Put32(header + 4, static_cast<uint32_t>(body_size));
  Put32(header + 8, Checksum(body, body_size));
  WriteFailure failure = owner_.CommitSlots() ? PersistWithSlots(header, body, body_size)
                                       : PersistWithMarker(header, body, body_size);
  if (!failure.stage && TestFailure("write_after_fsync")) failure = {"index_update", ENOMEM, true};
  if (!failure.stage) {
    for (size_t i = 0; i < count; ++i) {
      const KvMutation& item = mutations[i];
      if (item.erase) {
        uint64_t removed = 0;
        owner_.index_.root_.reset(Erase(owner_.index_.root_.release(), item.key, &removed));
        if (removed) { --owner_.index_.size_; owner_.index_.live_bytes_ -= removed; }
      } else {
        bool added = false;
        bool ok = true;
        uint64_t previous = 0;
        owner_.index_.root_.reset(Put(owner_.index_.root_.release(), item.key, offsets[i], item.value_size,
                    item.expires_at_ms, generation_, &added, &ok, &previous));
        if (added) ++owner_.index_.size_;
        if (!ok) { failure = {"index_update", ENOMEM, true}; break; }
        owner_.index_.live_bytes_ += RecordBytes(strlen(item.key), item.value_size) - previous;
      }
    }
  }
  if (!failure.stage) {
    end_ += sizeof(header) + body_size;
    (owner_.commits_.write_frames_).fetch_add(1, std::memory_order_relaxed);
  }
  offsets_storage.reset(); body_storage.reset();
  if (!failure.stage) TestKill("write_acknowledged");
  return failure;
}

KvStore::WriteFailure KvStore::KvLog::PersistWithMarker(const char* header, const char* body,
                                                 size_t body_size) {
  // Establish the rollback point before issuing any write to the log.
  owner_.recovery_.recovery_durable_end_ = end_;
  if (const char* stage = owner_.recovery_.SaveRecoveryOffset()) return {stage, errno, true, true};
  WriteFailure failure{};
  if (!WriteAll(fd_, header, kHeader, end_)) {
    failure = {"header_write", errno, true, true};
  } else {
    TestKill("write_after_header");
    if (TestFailure("write_body_no_space")) {
      failure = {"body_write", ENOSPC, true, true};
    } else if (TestFailure("write_after_header")) {
      failure = {"body_write", EIO, true, true};
    } else if (!WriteAll(fd_, body, body_size, end_ + kHeader)) {
      failure = {"body_write", errno, true, true};
    } else {
      TestKill("write_before_fsync");
      if (TestFailure("write_before_fsync")) failure = {"file_fsync", EIO, true, true};
      else if (TestFailure("file_fsync")) failure = {"file_fsync", EIO, true, true};
      else if (SyncFile(fd_, owner_.activity_) != 0) failure = {"file_fsync", errno, true, true};
    }
  }
  if (failure.stage) {
    TestKill("append_failed_before_recovery");
    return failure;
  }
  TestKill("append_synced_before_marker_clear");
  if (const char* stage = owner_.recovery_.ClearRecoveryOffset()) return {stage, errno, true, true};
  return failure;
}

// Two flushes per frame and no directory sync: the frame is synced first and
// only then named by the older slot, which is synced in turn. No slot ever
// names bytes whose own flush has not succeeded.
KvStore::WriteFailure KvStore::KvLog::PersistWithSlots(const char* header, const char* body,
                                                size_t body_size) {
  WriteFailure failure{};
  // A failed write leaves only unpublished bytes in a file that has not
  // failed a flush, so recovery reloads the published state in place.
  if (!WriteAll(fd_, header, kHeader, end_)) {
    failure = {"header_write", errno, true};
  } else {
    TestKill("write_after_header");
    if (TestFailure("write_body_no_space")) failure = {"body_write", ENOSPC, true};
    else if (TestFailure("write_after_header")) failure = {"body_write", EIO, true};
    else if (!WriteAll(fd_, body, body_size, end_ + kHeader)) failure = {"body_write", errno, true};
  }
  // After a failed flush the frame's pages may be clean in the cache yet
  // missing on disk, so recovery moves the published prefix to a new inode.
  if (!failure.stage) {
    TestKill("write_before_fsync");
    if (TestFailure("write_before_fsync") || TestFailure("file_fsync"))
      failure = {"file_fsync", EIO, true, true};
    else if (SyncFile(fd_, owner_.activity_) != 0)
      failure = {"file_fsync", errno, true, true};
  }
  // From here the frame is proven flushed. An uncertain slot may publish it
  // and recovery keeps it, but this caller is not acknowledged.
  if (!failure.stage) {
    TestKill("commit_frame_synced");
    const CommitSlot slot{commit_generation_, commit_sequence_ + 1, end_ + kHeader + body_size};
    if (TestFailure("commit_slot_write")) {
      failure = {"commit_slot_write", EIO, true, true, true};
    } else if (!WriteSlot(fd_, slot)) {
      failure = {"commit_slot_write", errno, true, true, true};
    } else {
      TestKill("commit_slot_written");
      if (TestFailure("commit_slot_fsync")) failure = {"commit_slot_fsync", EIO, true, true, true};
      else if (SyncFile(fd_, owner_.activity_) != 0) failure = {"commit_slot_fsync", errno, true, true, true};
      else commit_sequence_ = slot.sequence;
    }
  }
  if (failure.stage) TestKill("append_failed_before_recovery");
  return failure;
}

// Bounds include requests whose callers are waiting for the active group.
constexpr size_t kGroupRequests = 32;
constexpr size_t kGroupBytes = 256 * 1024;
constexpr size_t kQueuedRequests = 128;
constexpr size_t kQueuedBytes = 1024 * 1024;
constexpr size_t kGroupMutations = 256;
constexpr long kHoldoffNs = 150000;

struct KvStore::CommitRequest {
  const KvMutation* mutations;
  size_t count;
  size_t bytes;
  const KvExpectation* expected = nullptr;
  const char* const* absent_keys = nullptr;
  size_t absent_count = 0;
  int64_t now_ms = 0;
  CommitRequest* next = nullptr;
  KvCompareStatus result = KvCompareStatus::IoError;
  bool done = false;
  uint64_t epoch = 0;
};

uint64_t KvStore::CommitCoordinator::WriteFrames() const {
  return (write_frames_).load(std::memory_order_relaxed);
}

uint64_t KvStore::WriteFrames() const {
  return commits_.WriteFrames();
}

KvCompareStatus KvStore::CommitCoordinator::SubmitCommit(CommitRequest& request) {
  request.epoch = owner_.CacheEpoch();
  if (!request.epoch) return KvCompareStatus::IoError;
  fortis::base::UniqueLock commit_lock(commit_mutex_);
  if (pending_commits_ >= kQueuedRequests || request.bytes > kQueuedBytes - pending_bytes_) {
    return KvCompareStatus::IoError;
  }
  if (commit_tail_) commit_tail_->next = &request;
  else commit_head_ = &request;
  commit_tail_ = &request;
  ++pending_commits_;
  pending_bytes_ += request.bytes;
  while (!request.done) {
    if (committing_ || commit_head_ != &request) {
      commit_changed_.WaitLocked(commit_mutex_);
      continue;
    }
    committing_ = true;
    // Fill once without holding the storage or queue mutex. The relative
    // delay is independent of wall-clock corrections.
    if (!request.next) {
      commit_lock.Unlock();
      long holdoff_ns = kHoldoffNs;
#ifdef FORTIS_KV_TEST_FAILURES
      if (getenv("FORTIS_KV_TEST_GROUP_FILL")) holdoff_ns = 50000000;
#endif
      const timespec delay{0, holdoff_ns};
      nanosleep(&delay, nullptr);
      commit_lock.Lock();
    }
    CommitRequest* group[kGroupRequests];
    size_t total = 0, bytes = 0, mutations = 0;
    while (commit_head_ && total < kGroupRequests &&
           commit_head_->bytes <= kGroupBytes - bytes &&
           commit_head_->count <= kGroupMutations - mutations) {
      auto* member = commit_head_;
      commit_head_ = member->next;
      group[total++] = member;
      bytes += member->bytes;
      mutations += member->count;
    }
    if (!commit_head_) commit_tail_ = nullptr;
    commit_lock.Unlock();

    CommitGroup(group, total);

    commit_lock.Lock();
    pending_commits_ -= total;
    pending_bytes_ -= bytes;
    for (size_t i = 0; i < total; ++i) group[i]->done = true;
    committing_ = false;
    commit_changed_.Broadcast();
  }
  return request.result;
}

void KvStore::CommitCoordinator::CommitGroup(CommitRequest* const* requests, size_t count) {
  TimedGuard lock(owner_.mutex_.native(), &owner_.compactor_.stats_.lock_hold_us_max, &owner_.activity_, Phase::WriteHold);
  if (!lock.locked() || owner_.log_.fd_ < 0) return;
  KvMutation staged[kGroupMutations];
  size_t used = 0;
  for (size_t i = 0; i < count; ++i) {
    auto& request = *requests[i];
    if (request.epoch != owner_.CacheEpoch()) continue;
    request.result = CheckCondition(request, staged, used);
    if (request.result != KvCompareStatus::Committed) continue;
    memcpy(staged + used, request.mutations, request.count * sizeof(KvMutation));
    used += request.count;
  }
  // A single legacy frame gives the entire accepted group an atomic replay
  // boundary. Readers cannot see any staged value before sync and publication.
  if (used && !WriteUnlocked(staged, used)) {
    for (size_t i = 0; i < count; ++i) requests[i]->result = KvCompareStatus::IoError;
  }
}

bool KvStore::CommitCoordinator::Write(const KvMutation* mutations, size_t count) {
  size_t bytes = 0;
  if (group_commit_ && count <= kGroupMutations &&
      ValidMutations(mutations, count, &bytes) && bytes <= kGroupBytes) {
    CommitRequest request{mutations, count, bytes};
    return SubmitCommit(request) == KvCompareStatus::Committed;
  }
  TimedGuard lock(owner_.mutex_.native(), &owner_.compactor_.stats_.lock_hold_us_max, &owner_.activity_, Phase::WriteHold);
  return lock.locked() && WriteUnlocked(mutations, count);
}

bool KvStore::Write(const KvMutation* mutations, size_t count) {
  return commits_.Write(mutations, count);
}

bool KvStore::CommitCoordinator::WriteUnlocked(const KvMutation* mutations, size_t count) {
  if (owner_.log_.fd_ < 0) return false;
  size_t body_size = 0;
  if (!ValidMutations(mutations, count, &body_size))
    return RejectWrite({"validation", EINVAL, false}, count, 0);
  // After a swap whose directory fsync failed, power loss could bring back
  // the old name; nothing is acknowledged until the rename is durable.
  if (owner_.log_.directory_sync_pending_) {
    const bool injected = TestFailure("directory_sync_retry");
    if (injected || !SyncDirectory(owner_.log_.directory_, owner_.activity_)) {
      const int error = injected ? EIO : errno;
      ++owner_.compactor_.stats_.directory_sync_failures;
      return RejectWrite({"directory_fsync", error, false}, count, body_size);
    }
    owner_.log_.directory_sync_pending_ = false;
  }
  const WriteFailure failure = owner_.log_.Append(mutations, count, body_size);
  if (failure.stage) return RejectWrite(failure, count, body_size);
  owner_.log_.last_write_failure_stage_ = nullptr;
  return true;
}

bool KvStore::CommitCoordinator::RejectWrite(WriteFailure failure, size_t count, size_t body_size) {
  if (!owner_.log_.last_write_failure_stage_ || strcmp(owner_.log_.last_write_failure_stage_, failure.stage) != 0 ||
      failure.needs_replay) {
    fortis::base::LogError("kv", "KV write failed: stage=%s errno=%d mutations=%zu frame_body_bytes=%zu log_offset=%llu replay=%d",
            failure.stage, failure.error, count, body_size,
            static_cast<unsigned long long>(owner_.log_.end_), failure.needs_replay);
    owner_.log_.last_write_failure_stage_ = failure.stage;
  }
  if (failure.needs_replay) {
    owner_.recovery_.recovery_pending_ = true;
    owner_.recovery_.recovery_discard_tail_ = failure.discard_unflushed_tail;
    owner_.recovery_.recovery_durable_end_ = owner_.log_.end_ + (failure.frame_flushed ? kHeader + body_size : 0);
    owner_.recovery_.recovery_truncate_only_ = !owner_.CommitSlots() && owner_.recovery_.recovery_discard_tail_ &&
        (failure.error == ENOSPC || failure.error == EDQUOT) &&
        (strcmp(failure.stage, "header_write") == 0 ||
         strcmp(failure.stage, "body_write") == 0);
    owner_.recovery_.next_recovery_ms_ = 0;
    owner_.ResetLogUnlocked();
  }
  return false;
}

KvCompareStatus KvStore::CommitCoordinator::CompareAndWrite(const KvExpectation& expected, int64_t now_ms,
                                         const KvMutation* mutations, size_t count) {
  return CompareAndWrite(expected, nullptr, 0, now_ms, mutations, count);
}

KvCompareStatus KvStore::CompareAndWrite(const KvExpectation& expected, int64_t now_ms,
                                         const KvMutation* mutations, size_t count) {
  return commits_.CompareAndWrite(expected, now_ms, mutations, count);
}

KvCompareStatus KvStore::CommitCoordinator::CompareAndWrite(const KvExpectation& expected,
                                         const char* const* absent_keys, size_t absent_count,
                                         int64_t now_ms, const KvMutation* mutations,
                                         size_t count) {
  size_t bytes = 0;
  if (!ValidKey(expected.key) || expected.value_size > KvExpectation::kMaxValueBytes ||
      (expected.value_size && !expected.value) ||
      absent_count > kMaxAbsentKeys || (absent_count && !absent_keys) ||
      !ValidMutations(mutations, count, &bytes)) return KvCompareStatus::InvalidArgument;
  for (size_t i = 0; i < absent_count; ++i)
    if (!ValidKey(absent_keys[i])) return KvCompareStatus::InvalidArgument;
  CommitRequest request{mutations, count, bytes, &expected, absent_keys, absent_count, now_ms};
  if (group_commit_ && count <= kGroupMutations && bytes <= kGroupBytes)
    return SubmitCommit(request);
  TimedGuard lock(owner_.mutex_.native(), &owner_.compactor_.stats_.lock_hold_us_max, &owner_.activity_, Phase::WriteHold);
  if (!lock.locked() || owner_.log_.fd_ < 0) return KvCompareStatus::IoError;
  const auto condition = CheckCondition(request, nullptr, 0);
  if (condition != KvCompareStatus::Committed) return condition;
  return WriteUnlocked(mutations, count) ? KvCompareStatus::Committed : KvCompareStatus::IoError;
}

KvCompareStatus KvStore::CompareAndWrite(const KvExpectation& expected,
                                         const char* const* absent_keys, size_t absent_count,
                                         int64_t now_ms, const KvMutation* mutations,
                                         size_t count) {
  return commits_.CompareAndWrite(expected, absent_keys, absent_count, now_ms, mutations, count);
}

KvCompareStatus KvStore::CommitCoordinator::CheckCondition(const CommitRequest& request,
                                         const KvMutation* staged, size_t count) const {
  if (!request.expected) return KvCompareStatus::Committed;
  const auto& expected = *request.expected;
  const auto get = [&](const char* key, void* output, size_t capacity, size_t* size) {
    for (size_t i = count; i > 0; --i) {
      const auto& mutation = staged[i - 1];
      if (strcmp(key, mutation.key) != 0) continue;
      if (mutation.erase || (mutation.expires_at_ms && mutation.expires_at_ms <= request.now_ms))
        return KvReadStatus::Missing;
      *size = mutation.value_size;
      if (*size > capacity) return KvReadStatus::BufferTooSmall;
      if (*size) memcpy(output, mutation.value, *size);
      return KvReadStatus::Found;
    }
    return owner_.index_.GetStatusUnlocked(key, request.now_ms, output, capacity, size);
  };
  char live[KvExpectation::kMaxValueBytes];
  size_t size = 0;
  switch (get(expected.key, live, sizeof(live), &size)) {
    case KvReadStatus::Missing: return KvCompareStatus::MissingOrExpired;
    // Longer than any admissible expectation.
    case KvReadStatus::BufferTooSmall: return KvCompareStatus::ValueMismatch;
    case KvReadStatus::Found: break;
    default: return KvCompareStatus::IoError;
  }
  if (size != expected.value_size ||
      (size && memcmp(live, expected.value, size) != 0)) return KvCompareStatus::ValueMismatch;
  for (size_t i = 0; i < request.absent_count; ++i) {
    switch (get(request.absent_keys[i], live, sizeof(live), &size)) {
      case KvReadStatus::Missing: break;
      case KvReadStatus::Found:
      case KvReadStatus::BufferTooSmall: return KvCompareStatus::AbsentKeyPresent;
      default: return KvCompareStatus::IoError;
    }
  }
  TestKill("compare_matched");
  if (TestFailure("compare_matched")) return KvCompareStatus::IoError;
  return KvCompareStatus::Committed;
}

}  // namespace fortis::storage
