#include <atomic>
#include "threading.h"
#pragma once

// Internal to the KV store units (kv_store.cpp, kv_index.cpp, kv_log.cpp,
// kv_recovery.cpp, kv_commit.cpp, kv_compaction.cpp, kv_batch.cpp): the index
// node, the on-disk format constants and the helpers the units share. Nothing
// outside src/storage/kv_*.cpp includes this. Every unit must be compiled
// with the same FORTIS_KV_TEST_FAILURES setting: the failure hooks below are
// inline.
#include "kv_store.h"

#include "kv_checksum.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#ifdef FORTIS_KV_TEST_FAILURES
#include <signal.h>
#endif

namespace fortis::storage {

// `offset` is relative to the file whose parity is `generation`; while a run
// copies, `next_offset` is the record's position in the temporary file.
struct KvNode {
  char* key;
  KvNode* left;
  KvNode* right;
  uint64_t offset;
  uint64_t next_offset;
  int64_t expiry;
  uint32_t length;
  uint8_t generation;
  int8_t height;
};

namespace kv_internal {

constexpr uint32_t kMaxFrame = 8 * 1024 * 1024;
constexpr uint32_t kMaxValue = 1024 * 1024;
constexpr size_t kMaxKey = 1024;
constexpr size_t kHeader = 12;
constexpr size_t kRecord = 15;
constexpr unsigned kRecoveryAttempts = 3;
constexpr int64_t kRecoveryInitialBackoffMs = 1000;
constexpr uint64_t kRecoveryReplayBudgetUs = 60 * 1000000ull;

// The link-time check of the rule at the top of this file. kv_store.cpp
// defines the marker of the setting it was compiled with and every unit
// references the marker of its own, so linking units that disagree fails with
// an undefined symbol instead of mixing two definitions of the inline hooks.
#ifdef FORTIS_KV_TEST_FAILURES
#define FORTIS_KV_UNIT_SETTING kUnitsWithFailureInjection
#else
#define FORTIS_KV_UNIT_SETTING kUnitsWithoutFailureInjection
#endif
extern const char FORTIS_KV_UNIT_SETTING;
__attribute__((used)) static const char* const kUnitSetting = &FORTIS_KV_UNIT_SETTING;

inline uint64_t MonotonicUs() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1000000u +
         static_cast<uint64_t>(now.tv_nsec) / 1000u;
}

inline void RecordLatency(std::atomic<uint64_t>* buckets, uint64_t micros) {
  size_t bucket = 0;
  while (bucket + 1 < KvActivityStats::kBuckets && micros > KvActivityStats::kBoundsUs[bucket])
    ++bucket;
  (buckets[bucket]).fetch_add(1, std::memory_order_relaxed);
}

using Phase = KvActivityStats::Phase;

inline void RecordPhase(KvAtomicActivityStats& activity, Phase phase, uint64_t micros) {
  RecordLatency(activity.phases[static_cast<size_t>(phase)], micros);
}

// Serializes a public operation and records how long it held the mutex.
class TimedGuard {
 public:
  TimedGuard(pthread_mutex_t* mutex, uint64_t* longest,
             KvAtomicActivityStats* activity = nullptr, Phase operation = Phase::ReadHold,
             const bool* deferred_read = nullptr)
      : mutex_(mutex), longest_(longest), activity_(activity), operation_(operation),
        deferred_read_(deferred_read), entered_(activity ? MonotonicUs() : 0), locked_(Acquire()),
        start_(locked_ ? MonotonicUs() : 0) {}
  ~TimedGuard() {
    if (!locked_) return;
    const uint64_t done = MonotonicUs();
    const uint64_t held = done - start_;
    if (held > *longest_) *longest_ = held;
    if (activity_) {
      RecordPhase(*activity_, operation_, held);
      if (operation_ == Phase::ReadHold || operation_ == Phase::ScanHold ||
          operation_ == Phase::WriteHold) {
        RecordLatency(activity_->lock_wait, contended_ ? start_ - entered_ : 0);
        if (!deferred_read_ || !*deferred_read_)
          RecordLatency(operation_ == Phase::WriteHold ? activity_->writes : activity_->reads,
                        done - entered_);
      }
    }
    pthread_mutex_unlock(mutex_);
  }
  bool locked() const { return locked_; }
  TimedGuard(const TimedGuard&) = delete;
  TimedGuard& operator=(const TimedGuard&) = delete;

 private:
  bool Acquire() {
    if (!activity_) return pthread_mutex_lock(mutex_) == 0;
    const int result = pthread_mutex_trylock(mutex_);
    if (result != EBUSY) return result == 0;
    contended_ = true;
    const uint32_t waiting = ((activity_->lock_waiters).fetch_add(1, std::memory_order_relaxed) + 1);
    uint32_t peak = (activity_->peak_lock_waiters).load(std::memory_order_relaxed);
    while (waiting > peak && !(activity_->peak_lock_waiters).compare_exchange_strong(peak, waiting, std::memory_order_relaxed, std::memory_order_relaxed)) {}
    const bool locked = pthread_mutex_lock(mutex_) == 0;
    (activity_->lock_waiters).fetch_sub(1, std::memory_order_relaxed);
    return locked;
  }

  pthread_mutex_t* mutex_;
  uint64_t* longest_;
  KvAtomicActivityStats* activity_;
  Phase operation_;
  const bool* deferred_read_;
  uint64_t entered_;
  bool contended_ = false;
  bool locked_;
  uint64_t start_;
};

#ifdef FORTIS_KV_TEST_FAILURES
// TestFailure's state; the caller holds its mutex.
inline bool TestFailureLocked(const char* stage, const char* requested) {
  static char last_stage[64]{};
  static char last_call[32]{};
  static unsigned int matching_calls = 0;
  const char* call_value = getenv("FORTIS_KV_TEST_FAIL_ON_CALL");
  const char* normalized_call = call_value ? call_value : "";
  if (!requested || strcmp(last_stage, requested) != 0 ||
      strcmp(last_call, normalized_call) != 0) {
    last_stage[0] = '\0'; last_call[0] = '\0'; matching_calls = 0;
    if (!requested) return false;
    snprintf(last_stage, sizeof(last_stage), "%s", requested);
    snprintf(last_call, sizeof(last_call), "%s", normalized_call);
  }
  if (strcmp(requested, stage) != 0) return false;
  ++matching_calls;
  if (!*normalized_call) return true;
  char* end = nullptr;
  const unsigned long target = strtoul(normalized_call, &end, 10);
  return end && *end == '\0' && target > 0 && matching_calls == target;
}
#endif

inline bool TestFailure(const char* stage) {
#ifdef FORTIS_KV_TEST_FAILURES
  const char* requested = getenv("FORTIS_KV_TEST_FAIL");
  // The compaction tests hit failure points from several threads at once;
  // TestFailureLocked's occurrence counter is shared, so it is guarded.
  static fortis::base::Mutex state_mutex;
  fortis::base::LockGuard state_lock(state_mutex);
  const bool fail = TestFailureLocked(stage, requested);
  return fail;
#else
  (void)stage;
  return false;
#endif
}

// Hard-kill points: FORTIS_KV_TEST_KILL names the point and
// FORTIS_KV_TEST_KILL_ON_CALL (default 1) the occurrence that SIGKILLs the
// process, with no unwinding or buffered-state flush.
inline void TestKill(const char* point) {
#ifdef FORTIS_KV_TEST_FAILURES
  const char* requested = getenv("FORTIS_KV_TEST_KILL");
  if (!requested || strcmp(requested, point) != 0) return;
  static std::atomic<unsigned int> calls{0};
  const char* call_value = getenv("FORTIS_KV_TEST_KILL_ON_CALL");
  const unsigned long target = call_value ? strtoul(call_value, nullptr, 10) : 1;
  // Kill points may be reached from several threads.
  if (((calls).fetch_add(1, std::memory_order_relaxed) + 1) >= (target ? target : 1)) raise(SIGKILL);
#else
  (void)point;
#endif
}

void FreeTree(KvNode* node) noexcept;
KvNode* Find(KvNode* node, const char* key);

inline uint64_t RecordBytes(size_t key_length, uint32_t value_length) {
  return kRecord + key_length + value_length;
}

KvNode* Put(KvNode* node, const char* key, uint64_t offset, uint32_t length,
            int64_t expiry, uint8_t generation, bool* added, bool* ok,
            uint64_t* previous) noexcept;
KvNode* Erase(KvNode* node, const char* key, uint64_t* removed) noexcept;

inline void Put32(char* out, uint32_t number) {
  for (unsigned i = 0; i < 4; ++i) out[i] = static_cast<char>(number >> (8 * i));
}

inline void Put64(char* out, uint64_t number) {
  for (unsigned i = 0; i < 8; ++i) out[i] = static_cast<char>(number >> (8 * i));
}

inline uint32_t Read32(const char* in) {
  uint32_t number = 0;
  for (unsigned i = 0; i < 4; ++i)
    number |= static_cast<uint32_t>(static_cast<uint8_t>(in[i])) << (8 * i);
  return number;
}

inline uint64_t Read64(const char* in) {
  uint64_t number = 0;
  for (unsigned i = 0; i < 8; ++i)
    number |= static_cast<uint64_t>(static_cast<uint8_t>(in[i])) << (8 * i);
  return number;
}

// Frame and commit-slot CRC-32; kv_checksum.h keeps it byte-compatible.
inline uint32_t Checksum(const char* data, size_t length) { return KvChecksum(data, length); }

bool WriteAll(int fd, const char* data, size_t length, uint64_t offset);
bool ReadAll(int fd, char* data, size_t length, uint64_t offset, KvAtomicActivityStats& activity);
int SyncFile(int fd, KvAtomicActivityStats& activity);
bool SyncDirectory(const char* directory, KvAtomicActivityStats& activity);
bool ValidKey(const char* key);

// ---- Commit slots (KvDurability::CommitSlots) ---------------------------
//
// The log file begins with two commit slots, one per 4 KiB block, and frames
// start at kCommitLogBase. A slot (little endian) holds the magic "FKVS",
// format version, generation, sequence, committed end and log base, zero
// padding, and a CRC-32 of bytes [0, 4092) in its last four bytes. Sequence
// `s` always lives in block `s & 1`, so an append rewrites the block holding
// the older commit while the newest stays untouched in its own block. Frames
// never share a block with a slot. Replay trusts the valid slot with the
// highest sequence and ignores every byte past its committed end.
constexpr uint64_t kSlotBytes = 4096;
constexpr uint64_t kCommitLogBase = 2 * kSlotBytes;

struct CommitSlot {
  uint64_t generation = 0;
  uint64_t sequence = 0;
  uint64_t committed = 0;
};

bool SelectSlot(const char* header, CommitSlot* selected);
bool ReadCommitSlot(int fd, uint64_t size, KvAtomicActivityStats& activity, CommitSlot* slot);
bool WriteSlot(int fd, const CommitSlot& slot);

// A rollback-marker log always starts with a frame header (or is empty or a
// torn first header), and a commit-slot log never does, so stored values
// cannot make one format look like the other.
enum class LogFormat { Missing, RollbackMarker, CommitSlots, Unknown };

LogFormat DetectLogFormat(const char* path, KvAtomicActivityStats& activity);

// In-order traversal of keys strictly after `cursor` (all keys when null).
// `visit` returns false to stop; the traversal then returns false.
template <typename Visit>
bool VisitAfter(KvNode* node, const char* cursor, Visit& visit) {
  if (!node) return true;
  if (cursor && strcmp(node->key, cursor) <= 0) return VisitAfter(node->right, cursor, visit);
  if (!VisitAfter(node->left, cursor, visit)) return false;
  if (!visit(node)) return false;
  return VisitAfter(node->right, cursor, visit);
}

}  // namespace kv_internal
using namespace kv_internal;

}  // namespace fortis::storage
