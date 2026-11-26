#include "kv_expiry_sweeper.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "kv_runtime.h"
#include "threading.h"

namespace fortis::storage {

namespace {

constexpr uint32_t kRecoveryCheckMs = 1000;

int64_t wall_clock_ms() {
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) return 0;
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

int64_t monotonic_ms() {
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

bool compaction_cancelled(void* opaque) {
  return static_cast<const KvExpirySweeper*>(opaque)->stopping();
}

__attribute__((format(printf, 4, 5)))
bool append(char* output, size_t capacity, size_t* used, const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  const int written = vsnprintf(output + *used, capacity - *used, format, arguments);
  va_end(arguments);
  if (written < 0 || static_cast<size_t>(written) >= capacity - *used) return false;
  *used += static_cast<size_t>(written);
  return true;
}

timespec realtime_after(uint32_t milliseconds) {
  timespec wake{};
  clock_gettime(CLOCK_REALTIME, &wake);
  wake.tv_sec += static_cast<time_t>(milliseconds / 1000);
  wake.tv_nsec += static_cast<long>(milliseconds % 1000) * 1000000L;
  if (wake.tv_nsec >= 1000000000L) {
    ++wake.tv_sec;
    wake.tv_nsec -= 1000000000L;
  }
  return wake;
}

}  // namespace

bool KvExpirySweeper::Start(fortis::storage::KvStore* kv, uint32_t interval_ms, fortis::base::Clock* clock) {
  if (!interval_ms) return false;
  fortis::base::UniqueLock lock(mutex_);
  if (started_) return true;
  kv_ = kv;
  interval_ms_ = interval_ms;
  clock_ = clock;
  stopping_ = false;
  lock.Unlock();
  // KvStore::Sweep keeps 1,024 keys and mutations on its stack (~40 KiB).
  if (!thread_.Start(Main, this, 128 * 1024)) return false;
  lock.Lock();
  started_ = true;
  return true;
}

void KvExpirySweeper::Stop() {
  fortis::base::UniqueLock lock(mutex_);
  if (!started_) return;
  stopping_ = true;
  wake_.Broadcast();
  lock.Unlock();
  thread_.Join();
  lock.Lock();
  started_ = false;
}

uint64_t KvExpirySweeper::swept() const {
  fortis::base::LockGuard lock(mutex_);
  const uint64_t value = swept_;
  return value;
}

void KvExpirySweeper::SetMaintenance(MaintenanceFn maintenance, void* context) {
  fortis::base::LockGuard lock(mutex_);
  if (!started_) {
    maintenance_ = maintenance;
    maintenance_context_ = context;
  }
}

bool KvExpirySweeper::stopping() const {
  fortis::base::LockGuard lock(mutex_);
  const bool value = stopping_;
  return value;
}

uint64_t KvExpirySweeper::maintained() const {
  fortis::base::LockGuard lock(mutex_);
  const uint64_t value = maintained_;
  return value;
}

void KvExpirySweeper::SetCompactionPolicy(const fortis::storage::KvCompactionPolicy& policy,
                                          bool enabled) {
  fortis::base::LockGuard lock(mutex_);
  if (!started_) {
    compaction_policy_ = policy;
    compaction_enabled_ = enabled;
  }
}

uint64_t KvExpirySweeper::compaction_ticks() const {
  fortis::base::LockGuard lock(mutex_);
  const uint64_t value = compaction_ticks_;
  return value;
}

uint64_t KvExpirySweeper::ticks() const {
  fortis::base::LockGuard lock(mutex_);
  const uint64_t value = ticks_;
  return value;
}

void KvExpirySweeper::Main(KvExpirySweeper* self) { self->Loop(); }

void KvExpirySweeper::Loop() {
  fortis::base::UniqueLock lock(mutex_);
  int64_t next_sweep = monotonic_ms() + interval_ms_;
  while (!stopping_) {
    const int64_t until_sweep = next_sweep - monotonic_ms();
    const uint32_t wait_ms = until_sweep <= 0 ? 0 :
        until_sweep < kRecoveryCheckMs ? static_cast<uint32_t>(until_sweep) : kRecoveryCheckMs;
    const timespec wake = realtime_after(wait_ms);
    wake_.WaitUntilLocked(mutex_, wake);
    if (stopping_) break;
    lock.Unlock();
    const int64_t monotonic = monotonic_ms();
    const bool sweep_due = monotonic >= next_sweep;
    // Recovery needs an existing store. Leave initial open on the usual cadence.
    fortis::storage::KvStore* kv = kv_ ? kv_ :
        sweep_due ? fortis::storage::RuntimeKvStore() : fortis::storage::RuntimeKvStoreIfOpen();
    const bool ready = kv && kv->RecoverIfNeeded(monotonic);
    if (!sweep_due) {
      lock.Lock();
      continue;
    }
    const int64_t now = clock_ ? clock_->NowMs() : wall_clock_ms();
    const size_t removed = ready ? kv->Sweep(now, kLimit) : 0;
    const size_t maintained = ready && maintenance_ ? maintenance_(maintenance_context_, *kv, now, *this) : 0;
    // Never at boot: the store anchors its startup delay on this first call.
    const bool compacting = ready && compaction_enabled_ &&
        kv->MaintainCompaction(now, monotonic_ms(), compaction_policy_, [&, visitor_state = this]() { return (compaction_cancelled)(visitor_state); });
    next_sweep = monotonic_ms() + interval_ms_;
    lock.Lock();
    if (compacting) ++compaction_ticks_;
    swept_ += removed;
    maintained_ += maintained;
    ++ticks_;
  }
}

bool AppendKvCompactionMetrics(const fortis::storage::KvCompactionStats& stats, char* output,
                               size_t capacity, size_t* length) {
  if (!output || !length || *length > capacity) return false;
  size_t used = *length;
  const auto u = [](uint64_t value) { return static_cast<unsigned long long>(value); };
  const bool ok = append(output, capacity, &used,
      "# HELP fortis_kv_compaction_phase KV compaction phase (0 idle, 1 copy, 2 catch-up, 3 remap).\n"
      "# TYPE fortis_kv_compaction_phase gauge\n"
      "fortis_kv_compaction_phase %u\n"
      "# HELP fortis_kv_file_bytes Authoritative KV log length in bytes.\n"
      "# TYPE fortis_kv_file_bytes gauge\n"
      "fortis_kv_file_bytes %llu\n"
      "# HELP fortis_kv_live_bytes Encoded live KV records in bytes.\n"
      "# TYPE fortis_kv_live_bytes gauge\n"
      "fortis_kv_live_bytes %llu\n"
      "# HELP fortis_kv_compaction_temporary_bytes Bytes written to the running compaction's temporary file.\n"
      "# TYPE fortis_kv_compaction_temporary_bytes gauge\n"
      "fortis_kv_compaction_temporary_bytes %llu\n"
      "# HELP fortis_kv_compaction_runs_started_total KV compaction runs started.\n"
      "# TYPE fortis_kv_compaction_runs_started_total counter\n"
      "fortis_kv_compaction_runs_started_total %llu\n"
      "# HELP fortis_kv_compaction_runs_completed_total KV compaction runs swapped and remapped.\n"
      "# TYPE fortis_kv_compaction_runs_completed_total counter\n"
      "fortis_kv_compaction_runs_completed_total %llu\n"
      "# HELP fortis_kv_compaction_failures_total KV compaction runs abandoned before their swap.\n"
      "# TYPE fortis_kv_compaction_failures_total counter\n"
      "fortis_kv_compaction_failures_total %llu\n"
      "# HELP fortis_kv_compaction_last_error Last KV compaction error code (0 none).\n"
      "# TYPE fortis_kv_compaction_last_error gauge\n"
      "fortis_kv_compaction_last_error %u\n"
      "# HELP fortis_kv_compaction_skipped_low_disk_total Due KV compaction runs skipped for disk headroom.\n"
      "# TYPE fortis_kv_compaction_skipped_low_disk_total counter\n"
      "fortis_kv_compaction_skipped_low_disk_total %llu\n"
      "# HELP fortis_kv_compaction_blocked_low_disk 1 while a due KV compaction lacks disk headroom.\n"
      "# TYPE fortis_kv_compaction_blocked_low_disk gauge\n"
      "fortis_kv_compaction_blocked_low_disk %u\n"
      "# HELP fortis_kv_compaction_bytes_copied_total Live-record frame bytes written by KV compaction.\n"
      "# TYPE fortis_kv_compaction_bytes_copied_total counter\n"
      "fortis_kv_compaction_bytes_copied_total %llu\n"
      "# HELP fortis_kv_compaction_tail_bytes_copied_total Log tail bytes copied by KV compaction.\n"
      "# TYPE fortis_kv_compaction_tail_bytes_copied_total counter\n"
      "fortis_kv_compaction_tail_bytes_copied_total %llu\n"
      "# HELP fortis_kv_compaction_bytes_reclaimed_total KV log bytes reclaimed by compaction swaps.\n"
      "# TYPE fortis_kv_compaction_bytes_reclaimed_total counter\n"
      "fortis_kv_compaction_bytes_reclaimed_total %llu\n"
      "# HELP fortis_kv_compaction_expired_dropped_total Expired KV records dropped by compaction.\n"
      "# TYPE fortis_kv_compaction_expired_dropped_total counter\n"
      "fortis_kv_compaction_expired_dropped_total %llu\n"
      "# HELP fortis_kv_compaction_fsyncs_total File and directory fsyncs issued by KV compaction.\n"
      "# TYPE fortis_kv_compaction_fsyncs_total counter\n"
      "fortis_kv_compaction_fsyncs_total %llu\n"
      "# HELP fortis_kv_compaction_pauses_total Store-mutex holds by KV compaction.\n"
      "# TYPE fortis_kv_compaction_pauses_total counter\n"
      "fortis_kv_compaction_pauses_total %llu\n"
      "# HELP fortis_kv_compaction_pause_us_sum Microseconds KV compaction held the store mutex.\n"
      "# TYPE fortis_kv_compaction_pause_us_sum counter\n"
      "fortis_kv_compaction_pause_us_sum %llu\n"
      "# HELP fortis_kv_compaction_pause_us_max Longest store-mutex hold by KV compaction in microseconds.\n"
      "# TYPE fortis_kv_compaction_pause_us_max gauge\n"
      "fortis_kv_compaction_pause_us_max %llu\n"
      "# HELP fortis_kv_compaction_swap_pause_us_max Longest KV compaction swap hold in microseconds.\n"
      "# TYPE fortis_kv_compaction_swap_pause_us_max gauge\n"
      "fortis_kv_compaction_swap_pause_us_max %llu\n"
      "# HELP fortis_kv_lock_hold_us_max Longest store-mutex hold by any KV operation in microseconds.\n"
      "# TYPE fortis_kv_lock_hold_us_max gauge\n"
      "fortis_kv_lock_hold_us_max %llu\n"
      "# HELP fortis_kv_directory_sync_pending 1 while KV writes wait for a durable compaction rename.\n"
      "# TYPE fortis_kv_directory_sync_pending gauge\n"
      "fortis_kv_directory_sync_pending %u\n",
      static_cast<unsigned>(stats.phase), u(stats.file_bytes), u(stats.live_bytes),
      u(stats.temporary_bytes), u(stats.runs_started), u(stats.runs_completed),
      u(stats.runs_failed), static_cast<unsigned>(stats.last_error),
      u(stats.skipped_low_disk), stats.blocked_low_disk ? 1u : 0u, u(stats.bytes_copied),
      u(stats.tail_bytes_copied), u(stats.bytes_reclaimed), u(stats.expired_dropped),
      u(stats.fsyncs), u(stats.pauses), u(stats.pause_us_total), u(stats.pause_us_max),
      u(stats.swap_pause_us_max), u(stats.lock_hold_us_max),
      stats.directory_sync_pending ? 1u : 0u);
  if (!ok) {
    output[*length] = '\0';
    return false;
  }
  *length = used;
  return true;
}

}  // namespace fortis::storage
