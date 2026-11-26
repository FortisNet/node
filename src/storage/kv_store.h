#include <atomic>
#pragma once

#include "function_ref.h"
#include "ownership.h"

#include <stddef.h>
#include <stdint.h>
#include "threading.h"

namespace fortis::storage {

struct KvMutation {
  const char* key;
  const void* value;
  uint32_t value_size;
  int64_t expires_at_ms;  // Zero means no expiry.
  bool erase;
};

using KvVisitor = base::FunctionRef<bool(const char* key, const void* value, size_t size)>;
using KvRecordVisitor = base::FunctionRef<bool(const char*, const void*, size_t, int64_t)>;
enum class KvReadStatus { Found, Missing, BufferTooSmall, IoError };

// Condition for KvStore::CompareAndWrite: `key` must hold a live value equal
// (size and bytes) to `value`. Expectations are bounded so the comparison
// reads into a fixed buffer while the store mutex is held.
struct KvExpectation {
  static constexpr uint32_t kMaxValueBytes = 4096;
  const char* key;
  const void* value;
  uint32_t value_size;
};

// CompareAndWrite outcomes. Only Committed means the frame was acknowledged;
// MissingOrExpired, ValueMismatch and InvalidArgument append nothing; IoError
// is not acknowledged (see CompareAndWrite for what replay may then show).
// AbsentKeyPresent: an `absent_keys` entry of the guarded overload is live.
enum class KvCompareStatus : uint8_t {
  Committed, MissingOrExpired, ValueMismatch, IoError, InvalidArgument,
  AbsentKeyPresent,
};
struct KvPageResult {
  size_t scanned = 0;
  bool done = true;
  char next_cursor[1025]{};
};
struct KvNode;
struct KvNodeTreeDeleter {
  void operator()(KvNode* node) const noexcept;
};

// How an append becomes durable. RollbackMarker is the default protocol: a
// synced `fortis.kv.recover` offset guards each frame, so one append costs a
// marker sync, two directory syncs and a log sync. CommitSlots is an opt-in
// experiment: the log file starts with two padded, checksummed 4 KiB commit
// slots (generation, sequence, committed end) and frames follow at 8 KiB.
// An append syncs the frame, then writes the older slot and syncs again; only
// a slot names replayable bytes. Opening a rollback-marker log in CommitSlots
// mode migrates it once (see docs/kv-capacity.md). The two log formats differ
// in their first bytes, so a store opened in the other mode, or by an older
// runtime, fails to open instead of replaying an unpublished tail.
enum class KvDurability : uint8_t { RollbackMarker = 0, CommitSlots = 1 };

// Incremental compaction moves through these phases. The swap between
// CatchUp and Remap is one bounded step under the store mutex.
enum class KvCompactionPhase : uint8_t { Idle = 0, Copy = 1, CatchUp = 2, Remap = 3 };

enum class KvCompactionError : uint8_t {
  None = 0, CopyRead, CopyWrite, CopySync, TailWrite, SwapSync, Rename,
  LowDisk, Starved, Abandoned, Allocation, NoStore,
};

// Automatic compaction policy (see docs in MaintainCompaction). Defaults are
// the production values; tests shrink them.
struct KvCompactionPolicy {
  // Trigger: the log is at least `min_file_bytes` and dead bytes (file bytes
  // minus encoded live records) reach `dead_percent` of the file, or reach
  // `dead_bytes` while at least `dead_bytes_min_percent` of the file.
  uint64_t min_file_bytes = 8ull << 20;
  uint32_t dead_percent = 50;
  uint64_t dead_bytes = 128ull << 20;
  uint32_t dead_bytes_min_percent = 25;
  // Cadence on the maintenance clock: nothing before `startup_delay_ms` after
  // the first maintenance call, run starts at least `min_interval_ms` apart,
  // and failed or skipped runs back off from `retry_backoff_ms` doubling to
  // `max_backoff_ms`.
  int64_t startup_delay_ms = 5 * 60 * 1000;
  int64_t min_interval_ms = 15 * 60 * 1000;
  int64_t retry_backoff_ms = 60 * 1000;
  int64_t max_backoff_ms = 60 * 60 * 1000;
  // Disk headroom: a run starts only if the volume has room for the live
  // records plus 1/8 growth plus `reserve_bytes`, and a run aborts (deleting
  // its temporary file) once free space falls below `reserve_bytes`, so
  // foreground appends keep that reserve.
  uint64_t reserve_bytes = 64ull << 20;
  // Work slices. One locked step visits at most `slice_nodes` index nodes and
  // stages at most `slice_records` records / `slice_bytes` encoded bytes (one
  // oversized record may exceed it); the swap copies at most
  // `swap_tail_bytes` of live-log tail while holding the mutex.
  size_t slice_nodes = 4096;
  size_t slice_records = 512;
  size_t slice_bytes = 256 * 1024;
  size_t swap_tail_bytes = 256 * 1024;
  // Temporary file fsync cadence outside the mutex.
  uint64_t sync_every_bytes = 8ull << 20;
  // Per-maintenance-call budget, checked between slices.
  uint32_t tick_budget_ms = 500;
  uint32_t tick_max_slices = 4096;
  // Catch-up rounds before a run that cannot overtake foreground appends is
  // abandoned and retried after backoff.
  uint32_t max_catch_up_rounds = 4096;
};

struct KvCompactionStats {
  KvCompactionPhase phase = KvCompactionPhase::Idle;
  KvCompactionError last_error = KvCompactionError::None;
  bool directory_sync_pending = false;
  bool blocked_low_disk = false;
  uint64_t file_bytes = 0;         // Live log length.
  uint64_t live_bytes = 0;         // Encoded live records (no frame headers).
  uint64_t temporary_bytes = 0;    // Temporary file length while running.
  uint64_t runs_started = 0;
  uint64_t runs_completed = 0;
  uint64_t runs_failed = 0;
  uint64_t skipped_low_disk = 0;
  uint64_t bytes_copied = 0;       // Copy-phase frames written.
  uint64_t tail_bytes_copied = 0;  // Catch-up and swap tail bytes written.
  uint64_t bytes_reclaimed = 0;    // Sum of (old file - new file) per swap.
  uint64_t expired_dropped = 0;
  uint64_t fsyncs = 0;
  uint64_t pauses = 0;             // Compaction-held mutex intervals.
  uint64_t pause_us_total = 0;
  uint64_t pause_us_max = 0;
  uint64_t swap_pause_us_max = 0;
  uint64_t lock_hold_us_max = 0;   // Longest hold by any store operation.
  uint64_t directory_sync_failures = 0;
};

struct KvDashboardStats {
  KvCompactionStats compaction;
  size_t indexed_keys = 0;
  bool recovery_pending = false;
};

// Each histogram has one cumulative counter per upper bound and an overflow
// bucket. Foreground read and write latencies include their mutex wait. The
// phase histograms also sample background holds and synchronous file I/O.
struct KvActivityStats {
  static constexpr uint64_t kBoundsUs[] = {
      0, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000,
      25000, 50000, 100000, 250000, 500000, 1000000, 2500000, 5000000};
  static constexpr size_t kBuckets = sizeof(kBoundsUs) / sizeof(kBoundsUs[0]) + 1;
  uint64_t reads[kBuckets]{};
  uint64_t writes[kBuckets]{};
  uint64_t lock_wait[kBuckets]{};
  enum class Phase : size_t { ReadHold, WriteHold, SweepHold, CompactionHold,
                              RecoveryHold, FileRead, FileSync, ScanHold, Count };
  static constexpr size_t kPhases = static_cast<size_t>(Phase::Count);
  uint64_t phases[kPhases][kBuckets]{};
  uint32_t lock_waiters = 0;
  uint32_t peak_lock_waiters = 0;
};

// Live counters are atomic; the public KvActivityStats snapshot stays a plain
// copyable value with its existing layout and wire representation.
struct KvAtomicActivityStats {
  std::atomic<uint64_t> reads[KvActivityStats::kBuckets]{};
  std::atomic<uint64_t> writes[KvActivityStats::kBuckets]{};
  std::atomic<uint64_t> lock_wait[KvActivityStats::kBuckets]{};
  std::atomic<uint64_t> phases[KvActivityStats::kPhases][KvActivityStats::kBuckets]{};
  std::atomic<uint32_t> lock_waiters{0};
  std::atomic<uint32_t> peak_lock_waiters{0};
};

// Append-only store with process ownership enforced by flock. An AVL index
// holds keys and value offsets while values remain on disk. Every Write is one
// checksummed fsynced frame. The per-instance mutex protects the index and
// serializes durable writes. Scan/Page visitors run under that mutex and
// must not call this store.
//
// Compaction rewrites live records into `fortis.kv.compact` in bounded
// slices while foreground operations keep using the live log, then copies the
// live log's tail verbatim and atomically renames the temporary file over it.
// The live log stays the only authoritative log until that rename; the
// rename makes the fully synced replacement authoritative. Index offsets are
// translated lazily, then normalized in slices before the next run.
class KvStore {
 public:
  // Experimental, fixed-size group commit. Lifecycle calls still require
  // callers to quiesce before destroying the store.
  explicit KvStore(bool group_commit = false, bool parallel_point_reads = false,
                   KvDurability durability = KvDurability::RollbackMarker);
  // Successful, published foreground log frames since construction.
  uint64_t WriteFrames() const;
  bool GroupCommitEnabled() const { return commits_.group_commit_; }
  KvDurability Durability() const { return durability_; }
  ~KvStore();
  KvStore(const KvStore&) = delete;
  KvStore& operator=(const KvStore&) = delete;

  bool Open(const char* directory);
  void Close();
  // Nonzero process-unique generation for the authoritative index.
  uint64_t CacheEpoch() const;
  bool Get(const char* key, int64_t now_ms, void* output,
           size_t capacity, size_t* value_size) const;
  // In experimental mode the result is linearized when the offset and file
  // are pinned under the index lock. Join all callers before destroying this
  // store, including reads that overlap Close or compaction.
  KvReadStatus GetStatus(const char* key, int64_t now_ms, void* output,
                         size_t capacity, size_t* value_size) const;
  bool Write(const KvMutation* mutations, size_t count);
  // Maintenance entry. Replays an uncertain append with the writer lock held.
  // Returns false while recovery is pending, in progress, or exhausted.
  bool RecoverIfNeeded(int64_t monotonic_ms);
  // Conditional Write. Under the store mutex, with no unlock between the two,
  // reads `expected.key` at `now_ms` (an expiry <= now_ms reads as missing,
  // so a record is live strictly before its expiry instant) and, only when it
  // equals the expectation, appends `mutations` as one checksummed, fsynced
  // frame exactly as Write does. The condition is never persisted: replay and
  // compaction see an ordinary frame, so concurrent callers that expect the
  // same value serialize and at most one of them commits against it (when
  // the mutations change or erase that key).
  //
  // Arguments (key, expectation bound, mutation shape and frame size) are
  // validated before anything is read; InvalidArgument leaves the store
  // open. A condition that fails appends nothing. IoError covers a closed
  // store, a failed condition read, a pending post-swap directory fsync
  // (nothing appended) and a failed append. After a failed append the store
  // suspends access if the log may have changed. On a reported writeback
  // error, recovery retains the confirmed log prefix. A frame synced before
  // an index update error can still be recovered by replay.
  // A durable rollback marker protects the prefix until the frame and
  // marker removal have been synced; in CommitSlots mode a synced slot
  // publishes the frame instead. With the experimental group path, accepted
  // transactions share one frame and conditions see earlier staged mutations.
  KvCompareStatus CompareAndWrite(const KvExpectation& expected, int64_t now_ms,
                                  const KvMutation* mutations, size_t count);
  // The same conditional Write with extra guards: after `expected` matches,
  // each of the `absent_count` (at most kMaxAbsentKeys) `absent_keys` must
  // read as missing at `now_ms` under the same mutex hold, otherwise
  // AbsentKeyPresent and nothing is appended. The expectation is checked
  // first, so a consumed expectation reports MissingOrExpired even when a
  // guarded key is also present. A guarded key that is BufferTooSmall is
  // present; a failed read is IoError.
  static constexpr size_t kMaxAbsentKeys = 4;
  KvCompareStatus CompareAndWrite(const KvExpectation& expected,
                                  const char* const* absent_keys, size_t absent_count,
                                  int64_t now_ms, const KvMutation* mutations, size_t count);
  bool Scan(const char* prefix, const char* after, size_t limit, int64_t now_ms,
            KvVisitor visitor, bool reverse = false) const;
  bool Page(const char* prefix, const char* after, size_t max_scanned,
            int64_t now_ms, bool reverse, KvVisitor visitor,
            KvPageResult* result, KvRecordVisitor record_visitor = nullptr) const;
  size_t Sweep(int64_t now_ms, size_t limit);
  // Runs one full compaction (finishing an in-progress run first) in bounded
  // slices without expiring records; false if it failed.
  bool Compact();
  // Maintenance-thread entry. Starts a run only when the policy says one is
  // due and the volume has headroom, then performs bounded slices until the
  // run finishes, the budget is spent, or `cancelled` returns true. Records
  // with expiry <= `now_ms` are dropped. `monotonic_ms` drives cadence.
  // Returns true while a run remains in progress.
  bool MaintainCompaction(int64_t now_ms, int64_t monotonic_ms,
                          const KvCompactionPolicy& policy,
                          base::FunctionRef<bool()> cancelled = nullptr);
  void CompactionStats(KvCompactionStats* stats) const;
  // In-memory snapshot only. Returns false if the store is closed or busy.
  bool TryDashboardStats(KvDashboardStats* stats) const;
  // Lock-free activity counters remain readable while foreground I/O runs.
  void ActivityStats(KvActivityStats& stats) const;
  size_t Size() const;

 private:
  struct CommitRequest;

  enum class StepResult { Idle, Progress, Completed, Failed };
  // `discard_unflushed_tail` retires the log inode: recovery rewrites the
  // confirmed prefix into a new file. `frame_flushed` (commit slots) means
  // the frame's own sync succeeded, so that prefix includes it.
  struct WriteFailure {
    const char* stage = nullptr;
    int error = 0;
    bool needs_replay = false;
    bool discard_unflushed_tail = false;
    bool frame_flushed = false;
  };
  struct SliceItem {
    uint64_t source;
    uint32_t length;
    uint32_t position;
  };

  bool OpenUnlocked(const char* directory);
  void CloseUnlocked();
  void ResetLogUnlocked();
  void PublishCacheEpochUnlocked();
  void WaitMaintenanceUnlocked() const;

  size_t SweepUnlocked(int64_t now_ms, size_t limit);

  // Copies source bytes [start, end) into a fresh inode at the replacement's
  // log base, writes its commit slots (`generation`, `sequence` and
  // sequence + 1) when it uses them, then syncs and renames it over the log.

  void AdoptUnlocked(KvStore& source);
  bool CommitSlots() const { return durability_ == KvDurability::CommitSlots; }

  struct ScanBuffer;

  void CopyCompactionStatsUnlocked(KvCompactionStats* stats) const;

  class KvLog;
  class KvIndex;
  class KvRecovery;
  class KvCompactor;
  class CommitCoordinator;
  class KvLog {
   public:
    KvLog(const KvLog&) = delete;
    KvLog& operator=(const KvLog&) = delete;
   private:
    friend class KvStore;
    friend class KvIndex;
    friend class KvRecovery;
    friend class KvCompactor;
    friend class CommitCoordinator;
    explicit KvLog(KvStore& owner) : owner_(owner) {}

    WriteFailure Append(const KvMutation* mutations, size_t count, size_t body_size);
    WriteFailure PersistWithMarker(const char* header, const char* body, size_t body_size);
    WriteFailure PersistWithSlots(const char* header, const char* body, size_t body_size);
    bool ReadValue(uint64_t offset, uint32_t length, void* output) const;
    uint64_t LogBase() const;
    KvStore& owner_;
    char path_[512]{};
    char directory_[512]{};
    int lock_fd_ = -1;
    int fd_ = -1;
    uint64_t end_ = 0;
    uint8_t generation_ = 0;
    uint64_t commit_generation_ = 0;
    uint64_t commit_sequence_ = 0;
    bool directory_sync_pending_ = false;
    const char* last_write_failure_stage_ = nullptr;
  };
  class KvIndex {
   public:
    KvIndex(const KvIndex&) = delete;
    KvIndex& operator=(const KvIndex&) = delete;
   private:
    friend class KvStore;
    friend class KvLog;
    friend class KvRecovery;
    friend class KvCompactor;
    friend class CommitCoordinator;
    explicit KvIndex(KvStore& owner) : owner_(owner) {}

    uint64_t Resolve(const KvNode* node) const;
    bool Walk(const KvNode* node, const char* prefix, size_t prefix_length,
            const char* after, size_t limit, int64_t now_ms, bool reverse,
            bool physical, KvVisitor visitor,
            size_t* visited, ScanBuffer& buffer, KvPageResult* result,
            KvRecordVisitor record_visitor = nullptr) const;
    void CollectExpired(const KvNode* node, int64_t now_ms, const char** keys,
                      size_t capacity, size_t* count) const;
    KvReadStatus GetStatusUnlocked(const char* key, int64_t now_ms, void* output,
                                 size_t capacity, size_t* value_size) const;
    KvStore& owner_;
    std::unique_ptr<KvNode, KvNodeTreeDeleter> root_;
    size_t size_ = 0;
    uint64_t live_bytes_ = 0;
  };
  class KvRecovery {
   public:
    KvRecovery(const KvRecovery&) = delete;
    KvRecovery& operator=(const KvRecovery&) = delete;
   private:
    friend class KvStore;
    friend class KvLog;
    friend class KvIndex;
    friend class KvCompactor;
    friend class CommitCoordinator;
    explicit KvRecovery(KvStore& owner) : owner_(owner) {}

    bool RecoverIfNeeded(int64_t monotonic_ms);
    const char* SaveRecoveryOffset();
    const char* ClearRecoveryOffset();
    const char* RecoverAppend(KvStore& replacement, uint64_t deadline_us);
    const char* RebuildDurablePrefix(int source, uint64_t start, uint64_t end,
                                   uint64_t deadline_us, KvStore& replacement,
                                   uint64_t generation = 0, uint64_t sequence = 0,
                                   bool migrating_legacy = false);
    bool OpenCommitLogUnlocked();
    const char* MigrateLegacyLog(KvStore& replacement, uint64_t deadline_us);
    const char* ReopenCommitLog(KvStore& replacement, uint64_t deadline_us);
    const char* LoadCommitted(KvStore& replacement, bool rebuild, uint64_t durable_end,
                            uint64_t generation, uint64_t sequence, uint64_t deadline_us);
    const char* TruncateUnpublishedTail(KvAtomicActivityStats& io);
    bool Replay(uint64_t deadline_us = 0, KvAtomicActivityStats* activity = nullptr,
              bool migrating_legacy = false);
    KvStore& owner_;
    bool recovery_pending_ = false;
    bool recovery_discard_tail_ = false;
    bool recovery_truncate_only_ = false;
    bool recovery_offset_saved_ = false;
    uint64_t recovery_durable_end_ = 0;
    bool recovery_running_ = false;
    unsigned recovery_attempts_ = 0;
    int64_t next_recovery_ms_ = 0;
  };
  class KvCompactor {
   public:
    KvCompactor(const KvCompactor&) = delete;
    KvCompactor& operator=(const KvCompactor&) = delete;
   private:
    friend class KvStore;
    friend class KvLog;
    friend class KvIndex;
    friend class KvRecovery;
    friend class CommitCoordinator;
    explicit KvCompactor(KvStore& owner) : owner_(owner) {}

    void NotePauseUnlocked(uint64_t start_us, bool swap);
    bool LowDiskUnlocked(uint64_t required) const;
    void ReleaseCompactionUnlocked();
    void AbortCompactionUnlocked(KvCompactionError error);
    bool BeginCompactionUnlocked(const KvCompactionPolicy& policy, bool expire);
    StepResult CopyStepLocked(int64_t now_ms, const KvCompactionPolicy& policy,
                            uint64_t* lock_start_us);
    StepResult CatchUpStepLocked(const KvCompactionPolicy& policy, uint64_t* lock_start_us);
    StepResult SwapLocked();
    StepResult RemapStepLocked(const KvCompactionPolicy& policy);
    StepResult CompactionStep(int64_t now_ms, const KvCompactionPolicy& policy);
    bool Compact();
    bool CompactionDueUnlocked(const KvCompactionPolicy& policy, int64_t monotonic_ms);
    bool MaintainCompaction(int64_t now_ms, int64_t monotonic_ms,
                          const KvCompactionPolicy& policy,
                          base::FunctionRef<bool()> cancelled = nullptr);
    KvStore& owner_;
    KvCompactionPhase phase_ = KvCompactionPhase::Idle;
    bool compact_busy_ = false;
    bool compact_abandoned_ = false;
    bool compact_expire_ = false;
    bool compact_failed_ = false;
    int compact_fd_ = -1;
    int compact_source_fd_ = -1;
    char compact_path_[sizeof(KvLog::path_) + 16]{};
    uint64_t compact_base_ = 0;       // Live-log end when the run began.
    uint64_t compact_end_ = 0;        // Temporary file bytes written.
    uint64_t compact_copy_end_ = 0;   // Temporary offset where the tail starts.
    uint64_t compact_tail_ = 0;       // Live-log offset the tail is copied to.
    uint64_t compact_synced_ = 0;
    uint32_t compact_rounds_ = 0;
    char compact_cursor_[1025]{};
    bool compact_cursor_set_ = false;
    std::unique_ptr<char[], base::StorageDeleter> compact_buffer_;
    size_t compact_buffer_size_ = 0;
    std::unique_ptr<SliceItem[], base::StorageDeleter> compact_items_;
    size_t compact_items_capacity_ = 0;
    uint64_t remap_base_ = 0;
    uint64_t remap_copy_end_ = 0;
    bool schedule_anchored_ = false;
    int64_t schedule_anchor_ms_ = 0;
    int64_t next_attempt_ms_ = 0;
    int64_t backoff_ms_ = 0;
    mutable KvCompactionStats stats_{};
  };
  class CommitCoordinator {
   public:
    CommitCoordinator(const CommitCoordinator&) = delete;
    CommitCoordinator& operator=(const CommitCoordinator&) = delete;
   private:
    friend class KvStore;
    friend class KvLog;
    friend class KvIndex;
    friend class KvRecovery;
    friend class KvCompactor;
    explicit CommitCoordinator(KvStore& owner, bool group_commit) : owner_(owner), group_commit_(group_commit) {}

    static bool ValidMutations(const KvMutation* mutations, size_t count, size_t* body_size);
    uint64_t WriteFrames() const;
    KvCompareStatus SubmitCommit(CommitRequest& request);
    void CommitGroup(CommitRequest* const* requests, size_t count);
    bool Write(const KvMutation* mutations, size_t count);
    bool WriteUnlocked(const KvMutation* mutations, size_t count);
    bool RejectWrite(WriteFailure failure, size_t count, size_t body_size);
    KvCompareStatus CompareAndWrite(const KvExpectation& expected, int64_t now_ms,
                                  const KvMutation* mutations, size_t count);
    KvCompareStatus CompareAndWrite(const KvExpectation& expected,
                                  const char* const* absent_keys, size_t absent_count,
                                  int64_t now_ms, const KvMutation* mutations, size_t count);
    KvCompareStatus CheckCondition(const CommitRequest& request,
                                 const KvMutation* staged, size_t count) const;
    KvStore& owner_;
    const bool group_commit_;
    base::Mutex commit_mutex_;
    base::ConditionVariable commit_changed_;
    CommitRequest* commit_head_ = nullptr;
    CommitRequest* commit_tail_ = nullptr;
    size_t pending_commits_ = 0;
    size_t pending_bytes_ = 0;
    bool committing_ = false;
    std::atomic<uint64_t> write_frames_{0};
  };

  mutable KvAtomicActivityStats activity_{};
  std::atomic<uint64_t> cache_epoch_{0};
  mutable base::Mutex mutex_;
  mutable base::ConditionVariable compactor_idle_;
  const bool parallel_point_reads_;
  const KvDurability durability_;
  mutable std::atomic<size_t> pinned_readers_{0};

  KvLog log_;
  KvIndex index_;
  KvRecovery recovery_;
  KvCompactor compactor_;
  CommitCoordinator commits_;
};

// Bounded builder for one KvStore::Write frame. Callers stage puts and erases
// (keys and values are copied into one owned, growable buffer), read their
// own staged values back through Get, and commit everything as one
// checksummed fsynced frame. A replayed log therefore shows all of a batch or
// none of it. Not thread-safe: callers serialize the read-modify-writes that
// build a batch (see token_writes.h). Guest-safe byte storage.
class KvBatch {
 public:
  // Same bounds as KvStore::Append.
  static constexpr size_t kMaxFrameBytes = 8 * 1024 * 1024;
  static constexpr size_t kMaxMutations = 65535;
  static constexpr size_t kRecordOverhead = 15;
  static constexpr size_t kFrameOverhead = 12 + 4;

  // `max_bytes` bounds the staged key+value bytes (clamped so the encoded
  // frame stays inside kMaxFrameBytes); `max_mutations` the staged count.
  explicit KvBatch(size_t max_bytes, size_t max_mutations = 1024);
  ~KvBatch();
  KvBatch(const KvBatch&) = delete;
  KvBatch& operator=(const KvBatch&) = delete;

  struct Mark {
    size_t count = 0;
    size_t used = 0;
  };
  size_t count() const { return count_; }
  // Staged key and value bytes.
  size_t bytes() const { return used_; }
  // Encoded frame size KvStore::Write would append (header + body).
  size_t FrameBytes() const;
  // True when a Put/Erase was refused because the batch reached its bound
  // (as opposed to invalid input or an allocation failure); Rollback and
  // Clear reset it.
  bool full() const { return full_; }
  // Heap bytes the batch holds (staging buffer plus entry table).
  size_t allocated() const { return capacity_ + entry_capacity_ * sizeof(Entry); }
  Mark Position() const { return Mark{count_, used_}; }
  void Rollback(Mark mark);
  void Clear();

  // Stages a put (`expires_at_ms` zero = no expiry) or an erase. A later
  // mutation of the same key supersedes an earlier one, as in KvStore.
  bool Put(const char* key, const void* value, size_t size, int64_t expires_at_ms);
  bool Erase(const char* key);
  // Read-your-writes: the newest staged mutation of `key` wins (an erase or
  // an expiry <= now_ms reads Missing); otherwise the store answers.
  KvReadStatus Get(const KvStore* store, const char* key, int64_t now_ms, void* output,
                   size_t capacity, size_t* value_size) const;
  // One KvStore::Write of every staged mutation in order. Clears the batch
  // on success; on failure the batch is kept and the store has closed itself
  // (KvStore::WriteUnlocked) if recovering from an append error.
  // An empty batch commits trivially.
  bool Commit(KvStore* store);

 private:
  struct Entry {
    size_t key;  // offset of the NUL-terminated key in buffer_
    size_t value;
    uint32_t size;
    int64_t expires_at_ms;
    bool erase;
  };
  bool Stage(const char* key, const void* value, size_t size, int64_t expires_at_ms, bool erase);
  bool Reserve(size_t bytes);
  const Entry* Find(const char* key) const;

  size_t max_bytes_;
  size_t max_mutations_;
  std::unique_ptr<char[], base::StorageDeleter> buffer_;
  size_t capacity_ = 0;
  size_t used_ = 0;
  std::unique_ptr<Entry[], base::StorageDeleter> entries_;
  size_t entry_capacity_ = 0;
  size_t count_ = 0;
  bool full_ = false;
};

}  // namespace fortis::storage
