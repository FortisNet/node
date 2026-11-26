#include <atomic>
// The KV store's lifecycle and read side: open and close, point reads,
// scans, pages, the expiry sweep and the statistics. The index, the log
// format, recovery, the write path, compaction and KvBatch live in the other
// kv_*.cpp units.
#include "kv_store_internal.h"
#include "log.h"
#include "threading.h"

namespace fortis::storage {

namespace {

int lock_unsupported_reported = 0;
std::atomic<uint64_t> next_cache_epoch{0};

}  // namespace

namespace kv_internal {

const char FORTIS_KV_UNIT_SETTING = 0;

bool ValidKey(const char* key) {
  if (!key || !*key) return false;
  const size_t length = strnlen(key, kMaxKey + 1);
  return length > 0 && length <= kMaxKey;
}

}  // namespace kv_internal

KvStore::KvStore(bool group_commit, bool parallel_point_reads, KvDurability durability)
    : parallel_point_reads_(parallel_point_reads), durability_(durability),
      log_(*this), index_(*this), recovery_(*this), compactor_(*this), commits_(*this, group_commit) {}

KvStore::~KvStore() {
  fortis::base::UniqueLock lock(mutex_);
  WaitMaintenanceUnlocked();
  CloseUnlocked();
}

// Close and Open wait for compaction or replay to release the files and index
// they use outside the mutex.
void KvStore::WaitMaintenanceUnlocked() const {
  while (compactor_.compact_busy_ || recovery_.recovery_running_) compactor_idle_.WaitLocked(mutex_);
}

bool KvStore::Open(const char* directory) {
  fortis::base::LockGuard lock(mutex_);
  if (!lock.locked()) return false;
  WaitMaintenanceUnlocked();
  const bool opened = OpenUnlocked(directory);
  if (opened) PublishCacheEpochUnlocked();
  return opened;
}

uint64_t KvStore::CacheEpoch() const {
  return (cache_epoch_).load(std::memory_order_acquire);
}

void KvStore::PublishCacheEpochUnlocked() {
  const auto epoch = ((next_cache_epoch).fetch_add(1, std::memory_order_relaxed) + 1);
  (cache_epoch_).store(epoch, std::memory_order_release);
}

bool KvStore::OpenUnlocked(const char* directory) {
  CloseUnlocked();
  if (!directory) return false;
  struct stat info {};
  if (stat(directory, &info) != 0 || !S_ISDIR(info.st_mode)) return false;
  if (strlen(directory) >= sizeof(log_.directory_)) return false;
  char lock_path[512];
  const int lock_count = snprintf(lock_path, sizeof(lock_path), "%s/fortis.kv.lock", directory);
  if (lock_count <= 0 || static_cast<size_t>(lock_count) >= sizeof(lock_path)) return false;
  log_.lock_fd_ = open(lock_path, O_RDWR | O_CREAT, 0600);
  if (log_.lock_fd_ < 0) {
    fortis::base::LogError("kv", "KV lock open failed: errno=%d", errno);
    CloseUnlocked();
    return false;
  }
  if (flock(log_.lock_fd_, LOCK_EX | LOCK_NB) != 0) {
    if (errno != EWOULDBLOCK && errno != EAGAIN)
      fortis::base::LogError("kv", "KV lock acquisition failed: errno=%d", errno);
    CloseUnlocked();
    return false;
  }
  // The launch wrappers set this only after taking and verifying the host lock
  // on this same volume. Unwrapped/direct guest starts retain the strict local
  // probe, because some unikernel VFS implementations stub flock as success.
  bool host_locked = false;
#ifdef __Unikraft__
  const char* lock_mode = getenv("FORTIS_KV_LOCK_MODE");
  host_locked = lock_mode && strcmp(lock_mode, "host") == 0;
#endif
  if (!host_locked) {
    const int probe_fd = open(lock_path, O_RDWR);
    if (probe_fd < 0) {
      fortis::base::LogError("kv", "KV lock verification open failed: errno=%d", errno);
      CloseUnlocked();
      return false;
    }
    const bool probe_acquired = flock(probe_fd, LOCK_EX | LOCK_NB) == 0;
    const int probe_error = errno;
    close(probe_fd);
    if (probe_acquired || (probe_error != EWOULDBLOCK && probe_error != EAGAIN)) {
      if (__sync_bool_compare_and_swap(&lock_unsupported_reported, 0, 1)) {
        if (probe_acquired)
          fortis::base::LogError("kv", "KV lock unsupported: independent lock probe succeeded");
        else
          fortis::base::LogError("kv", "KV lock unsupported: independent lock probe failed (errno=%d)",
                  probe_error);
      }
      CloseUnlocked();
      return false;
    }
  }
  if (!SyncDirectory(directory, activity_)) {
    fortis::base::LogError("kv", "KV lock directory sync failed: errno=%d", errno);
    CloseUnlocked();
    return false;
  }
  snprintf(log_.directory_, sizeof(log_.directory_), "%s", directory);
  const int count = snprintf(log_.path_, sizeof(log_.path_), "%s/fortis.kv", directory);
  if (count <= 0 || static_cast<size_t>(count) >= sizeof(log_.path_)) { CloseUnlocked(); return false; }
  // A temporary file from an interrupted run was never authoritative: the
  // rename that would make it so did not happen, or it would not exist.
  char stale[sizeof(compactor_.compact_path_)];
  snprintf(stale, sizeof(stale), "%s.compact", log_.path_);
  if (unlink(stale) == 0) {
    TestKill("open_after_stale_unlink");
    if (!SyncDirectory(directory, activity_)) { CloseUnlocked(); return false; }
  }
  char preparing[sizeof(compactor_.compact_path_)];
  snprintf(preparing, sizeof(preparing), "%s.recover.tmp", log_.path_);
  if (unlink(preparing) != 0 && errno != ENOENT) { CloseUnlocked(); return false; }
  const LogFormat format = DetectLogFormat(log_.path_, activity_);
  const bool fresh = format == LogFormat::Missing;
  char recovery[sizeof(compactor_.compact_path_)];
  snprintf(recovery, sizeof(recovery), "%s.recover", log_.path_);
  const int marker = open(recovery, O_RDONLY);
  if (marker >= 0) {
    char offset[16];
    struct stat status {};
    const bool valid = fstat(marker, &status) == 0 && status.st_size == sizeof(offset) &&
                       ReadAll(marker, offset, sizeof(offset), 0, activity_) &&
                       Read64(offset) == ~Read64(offset + 8);
    close(marker);
    // Only a rollback-marker log can carry a marker.
    if (!valid || fresh || format == LogFormat::CommitSlots) { CloseUnlocked(); return false; }
    recovery_.recovery_durable_end_ = Read64(offset);
    recovery_.recovery_offset_saved_ = true;
    KvStore replacement;
    const char* stage = recovery_.RecoverAppend(replacement, MonotonicUs() + kRecoveryReplayBudgetUs);
    if (stage) {
      fortis::base::LogError("kv", "KV startup recovery failed: stage=%s errno=%d", stage, errno);
      CloseUnlocked();
      return false;
    }
    recovery_.recovery_offset_saved_ = false;
    recovery_.recovery_durable_end_ = 0;
    // A commit-slot store migrates the settled log below.
    if (!CommitSlots()) {
      AdoptUnlocked(replacement);
      return true;
    }
  } else if (errno != ENOENT) {
    CloseUnlocked();
    return false;
  }
  if (CommitSlots()) return recovery_.OpenCommitLogUnlocked();
  if (format == LogFormat::CommitSlots) {
    fortis::base::LogError("kv", "KV log uses commit slots; open it with FORTIS_KV_DURABILITY=commit-slots");
    CloseUnlocked();
    return false;
  }
  log_.fd_ = open(log_.path_, O_RDWR | O_CREAT, 0600);
  if (log_.fd_ < 0) { CloseUnlocked(); return false; }
  if ((fresh && !SyncDirectory(directory, activity_)) || !recovery_.Replay()) { CloseUnlocked(); return false; }
  return true;
}

void KvStore::Close() {
  fortis::base::LockGuard lock(mutex_);
  if (!lock.locked()) return;
  WaitMaintenanceUnlocked();
  CloseUnlocked();
}

void KvStore::CloseUnlocked() {
  ResetLogUnlocked();
  if (log_.lock_fd_ >= 0) close(log_.lock_fd_);
  log_.lock_fd_ = -1;
  log_.last_write_failure_stage_ = nullptr;
  recovery_.recovery_pending_ = false;
  recovery_.recovery_discard_tail_ = false;
  recovery_.recovery_truncate_only_ = false;
  recovery_.recovery_offset_saved_ = false;
  recovery_.recovery_durable_end_ = 0;
  recovery_.recovery_attempts_ = 0;
  recovery_.next_recovery_ms_ = 0;
  log_.commit_generation_ = 0;
  log_.commit_sequence_ = 0;
}

uint64_t KvStore::KvLog::LogBase() const { return owner_.CommitSlots() ? kCommitLogBase : 0; }

// Takes over a privately replayed log; `source` keeps no descriptor or index.
void KvStore::AdoptUnlocked(KvStore& source) {
  log_.fd_ = source.log_.fd_; source.log_.fd_ = -1;
  index_.root_ = std::move(source.index_.root_);
  log_.end_ = source.log_.end_;
  index_.size_ = source.index_.size_;
  index_.live_bytes_ = source.index_.live_bytes_;
  log_.commit_generation_ = source.log_.commit_generation_;
  log_.commit_sequence_ = source.log_.commit_sequence_;
}

void KvStore::ResetLogUnlocked() {
  (cache_epoch_).store(0, std::memory_order_release);
  // A swapped run has no files left; its remaining remap dies with the index.
  if (compactor_.phase_ == KvCompactionPhase::Remap) compactor_.phase_ = KvCompactionPhase::Idle;
  else if (compactor_.phase_ != KvCompactionPhase::Idle) compactor_.AbortCompactionUnlocked(KvCompactionError::Abandoned);
  if (log_.fd_ >= 0) close(log_.fd_);
  log_.fd_ = -1; log_.end_ = 0; index_.size_ = 0; index_.live_bytes_ = 0;
  log_.generation_ = 0; log_.directory_sync_pending_ = false;
  index_.root_.reset();
}

// Nodes left from before the last swap translate lazily: tail records moved
// verbatim, older records to where the copy phase put them.
uint64_t KvStore::KvIndex::Resolve(const KvNode* node) const {
  if (node->generation == owner_.log_.generation_) return node->offset;
  return node->offset >= owner_.compactor_.remap_base_ ? node->offset - owner_.compactor_.remap_base_ + owner_.compactor_.remap_copy_end_
                                     : node->next_offset;
}

bool KvStore::KvLog::ReadValue(uint64_t offset, uint32_t length, void* output) const {
  return length == 0 || ReadAll(fd_, static_cast<char*>(output), length, offset, owner_.activity_);
}

bool KvStore::Get(const char* key, int64_t now_ms, void* output,
                  size_t capacity, size_t* value_size) const {
  return GetStatus(key, now_ms, output, capacity, value_size) == KvReadStatus::Found;
}

KvReadStatus KvStore::GetStatus(const char* key, int64_t now_ms, void* output,
                                size_t capacity, size_t* value_size) const {
  const uint64_t started = parallel_point_reads_ ? MonotonicUs() : 0;
  int pinned_fd;
  uint64_t offset;
  uint32_t length;
  bool deferred = false;
  {
    TimedGuard lock(mutex_.native(), &compactor_.stats_.lock_hold_us_max, &activity_, Phase::ReadHold, &deferred);
    if (!lock.locked()) return KvReadStatus::IoError;
    constexpr size_t kMaxPinnedReaders = 4;
    if (!parallel_point_reads_ || (pinned_readers_).load(std::memory_order_relaxed) >= kMaxPinnedReaders)
      return index_.GetStatusUnlocked(key, now_ms, output, capacity, value_size);
    if (log_.fd_ < 0 || !key || !value_size || (!output && capacity))
      return KvReadStatus::IoError;
    const KvNode* found = Find(index_.root_.get(), key);
    if (!found || (found->expiry && found->expiry <= now_ms)) return KvReadStatus::Missing;
    length = found->length;
    *value_size = length;
    if (length > capacity) return KvReadStatus::BufferTooSmall;
    if (!length) return KvReadStatus::Found;
    // Resolve against the same file we pin across a possible compaction swap.
    pinned_fd = dup(log_.fd_);
    offset = index_.Resolve(found);
    if (pinned_fd < 0)
      return log_.ReadValue(offset, length, output) ? KvReadStatus::Found : KvReadStatus::IoError;
    (void)((pinned_readers_).fetch_add(1, std::memory_order_relaxed) + 1);
    deferred = true;
  }
  const bool ok = ReadAll(pinned_fd, static_cast<char*>(output), length, offset, activity_);
  close(pinned_fd);
  RecordLatency(activity_.reads, MonotonicUs() - started);
  (void)((pinned_readers_).fetch_sub(1, std::memory_order_relaxed) - 1);
  return ok ? KvReadStatus::Found : KvReadStatus::IoError;
}

KvReadStatus KvStore::KvIndex::GetStatusUnlocked(const char* key, int64_t now_ms,
                                        void* output, size_t capacity,
                                        size_t* value_size) const {
  if (owner_.log_.fd_ < 0 || !key || !value_size || (!output && capacity))
    return KvReadStatus::IoError;
  const KvNode* found = Find(root_.get(), key);
  if (!found || (found->expiry && found->expiry <= now_ms))
    return KvReadStatus::Missing;
  *value_size = found->length;
  if (found->length > capacity) return KvReadStatus::BufferTooSmall;
  if (!owner_.log_.ReadValue(Resolve(found), found->length, output)) return KvReadStatus::IoError;
  return KvReadStatus::Found;
}

// Most index values fit in the inline buffer. Reuse heap storage within a
// scan only when a matching live record actually needs it.
struct KvStore::ScanBuffer {
  char inline_bytes[512];
  std::unique_ptr<char[], base::StorageDeleter> heap;
  size_t capacity = 0;


  char* ForLength(size_t length) {
    if (length <= sizeof(inline_bytes)) return inline_bytes;
    if (length > capacity) {
      heap.reset();
      heap = TestFailure("scan_allocate") ? nullptr : base::AllocateStorage<char[]>(length);
      capacity = heap ? length : 0;
    }
    return heap.get();
  }
};

bool KvStore::KvIndex::Walk(const KvNode* node, const char* prefix, size_t prefix_length,
                   const char* after, size_t limit, int64_t now_ms, bool reverse,
                   bool physical, KvVisitor visitor,
                   size_t* visited, ScanBuffer& buffer, KvPageResult* result, KvRecordVisitor record_visitor) const {
  if (!node || *visited >= limit) return true;
  const int prefix_order = strncmp(node->key, prefix, prefix_length);
  if (prefix_order < 0)
    return Walk(node->right, prefix, prefix_length, after, limit, now_ms,
                reverse, physical, visitor, visited, buffer, result, record_visitor);
  if (prefix_order > 0)
    return Walk(node->left, prefix, prefix_length, after, limit, now_ms,
                reverse, physical, visitor, visited, buffer, result, record_visitor);
  const int cursor_order = after && *after ? strcmp(node->key, after) : 0;
  if (after && *after && ((!reverse && cursor_order <= 0) ||
                          (reverse && cursor_order >= 0))) {
    return Walk(reverse ? node->left : node->right, prefix, prefix_length,
                after, limit, now_ms, reverse, physical, visitor,
                visited, buffer, result, record_visitor);
  }
  const KvNode* first = reverse ? node->right : node->left;
  const KvNode* second = reverse ? node->left : node->right;
  if (!Walk(first, prefix, prefix_length, after, limit, now_ms, reverse,
            physical, visitor, visited, buffer, result, record_visitor)) return false;
  if (*visited < limit) {
    if (physical) {
      ++*visited;
      if (result) {
        result->scanned = *visited;
        memcpy(result->next_cursor, node->key, strlen(node->key) + 1);
      }
    }
    if (!node->expiry || node->expiry > now_ms) {
      char* value = buffer.ForLength(node->length);
      if (!value || !owner_.log_.ReadValue(Resolve(node), node->length, value) ||
          !(record_visitor ? record_visitor(node->key, value, node->length, node->expiry)
                           : visitor(node->key, value, node->length))) return false;
      if (!physical) ++*visited;
    }
  }
  return Walk(second, prefix, prefix_length, after, limit, now_ms, reverse,
              physical, visitor, visited, buffer, result, record_visitor);
}

bool KvStore::Scan(const char* prefix, const char* after, size_t limit, int64_t now_ms,
                   KvVisitor visitor, bool reverse) const {
  TimedGuard lock(mutex_.native(), &compactor_.stats_.lock_hold_us_max, &activity_, Phase::ScanHold);
  if (!lock.locked()) return false;
  if (log_.fd_ < 0 || !prefix || !visitor || (after && *after &&
      strncmp(after, prefix, strlen(prefix)) != 0)) return false;
  if (!limit) return true;
  ScanBuffer buffer;
  size_t visited = 0;
  return index_.Walk(index_.root_.get(), prefix, strlen(prefix), after, limit, now_ms,
              reverse, false, visitor, &visited, buffer, nullptr);
}

bool KvStore::Page(const char* prefix, const char* after, size_t max_scanned,
                   int64_t now_ms, bool reverse, KvVisitor visitor,
                   KvPageResult* result, KvRecordVisitor record_visitor) const {
  TimedGuard lock(mutex_.native(), &compactor_.stats_.lock_hold_us_max, &activity_, Phase::ScanHold);
  if (!lock.locked()) return false;
  if (log_.fd_ < 0 || !prefix || (!visitor && !record_visitor) || !result || !max_scanned ||
      (after && *after && strncmp(after, prefix, strlen(prefix)) != 0)) return false;
  // A scan boundary need not be a stored key. A decoded transfer cursor can
  // compare beyond the maximum key length, so keep the full bounded input.
  // The common pagination call passes result->next_cursor back as `after`;
  // copy that short alias before clearing result.
  char previous_cursor[kMaxKey + 1];
  const char* scan_after = after;
  if (after == result->next_cursor) {
    const size_t length = strnlen(after, sizeof(previous_cursor));
    if (length >= sizeof(previous_cursor)) return false;
    memcpy(previous_cursor, after, length + 1);
    scan_after = previous_cursor;
  } else if (after && strnlen(after, 8192) >= 8192) {
    return false;
  }
  *result = KvPageResult{};
  ScanBuffer buffer;
  size_t visited = 0;
  const bool ok = index_.Walk(index_.root_.get(), prefix, strlen(prefix), scan_after, max_scanned,
                       now_ms, reverse, true, visitor, &visited, buffer, result, record_visitor);
  result->done = visited < max_scanned;
  return ok;
}

void KvStore::KvIndex::CollectExpired(const KvNode* node, int64_t now_ms,
                             const char** keys, size_t capacity, size_t* count) const {
  if (!node || *count >= capacity) return;
  CollectExpired(node->left, now_ms, keys, capacity, count);
  if (*count < capacity && node->expiry && node->expiry <= now_ms)
    keys[(*count)++] = node->key;
  CollectExpired(node->right, now_ms, keys, capacity, count);
}

size_t KvStore::Sweep(int64_t now_ms, size_t limit) {
  TimedGuard lock(mutex_.native(), &compactor_.stats_.lock_hold_us_max, &activity_, Phase::SweepHold);
  return lock.locked() ? SweepUnlocked(now_ms, limit) : 0;
}

size_t KvStore::SweepUnlocked(int64_t now_ms, size_t limit) {
  if (!limit || log_.fd_ < 0) return 0;
  if (limit > 1024) limit = 1024;
  const char* keys[1024];
  size_t count = 0;
  index_.CollectExpired(index_.root_.get(), now_ms, keys, limit, &count);
  if (!count) return 0;
  KvMutation mutations[1024];
  for (size_t i = 0; i < count; ++i) mutations[i] = {keys[i], nullptr, 0, 0, true};
  return commits_.WriteUnlocked(mutations, count) ? count : 0;
}

void KvStore::CompactionStats(KvCompactionStats* stats) const {
  if (!stats) return;
  TimedGuard lock(mutex_.native(), &compactor_.stats_.lock_hold_us_max);
  if (!lock.locked()) return;
  CopyCompactionStatsUnlocked(stats);
}

bool KvStore::TryDashboardStats(KvDashboardStats* stats) const {
  if (!stats) return false;
  fortis::base::UniqueLock lock(mutex_, fortis::base::kTryToLock);
  if (!lock.locked()) return false;
  const bool open = log_.fd_ >= 0;
  if (open) {
    CopyCompactionStatsUnlocked(&stats->compaction);
    stats->indexed_keys = index_.size_;
    stats->recovery_pending = recovery_.recovery_pending_;
  }
  return open;
}

void KvStore::ActivityStats(KvActivityStats& stats) const {
  for (size_t i = 0; i < KvActivityStats::kBuckets; ++i) {
    stats.reads[i] = (activity_.reads[i]).load(std::memory_order_relaxed);
    stats.writes[i] = (activity_.writes[i]).load(std::memory_order_relaxed);
    stats.lock_wait[i] = (activity_.lock_wait[i]).load(std::memory_order_relaxed);
    for (size_t phase = 0; phase < KvActivityStats::kPhases; ++phase)
      stats.phases[phase][i] = (activity_.phases[phase][i]).load(std::memory_order_relaxed);
  }
  stats.lock_waiters = (activity_.lock_waiters).load(std::memory_order_relaxed);
  stats.peak_lock_waiters = (activity_.peak_lock_waiters).load(std::memory_order_relaxed);
}

void KvStore::CopyCompactionStatsUnlocked(KvCompactionStats* stats) const {
  *stats = compactor_.stats_;
  stats->phase = compactor_.phase_;
  stats->file_bytes = log_.fd_ >= 0 ? log_.end_ : 0;
  stats->live_bytes = index_.live_bytes_;
  stats->temporary_bytes = compactor_.phase_ == KvCompactionPhase::Copy ||
                           compactor_.phase_ == KvCompactionPhase::CatchUp ? compactor_.compact_end_ : 0;
  stats->directory_sync_pending = log_.directory_sync_pending_;
}

size_t KvStore::Size() const {
  TimedGuard lock(mutex_.native(), &compactor_.stats_.lock_hold_us_max);
  return lock.locked() ? index_.size_ : 0;
}

}  // namespace fortis::storage
