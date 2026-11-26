#pragma once

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "clock.h"
#include "threading.h"
#include "kv_store.h"

namespace fortis::storage {

// Upper bound of AppendKvCompactionMetrics output.
constexpr size_t kKvCompactionMetricsMaxBytes = 5120;

// Prometheus gauges and counters for the process store's compaction (no
// labels). False if `capacity` cannot hold them; `*length` is then unchanged.
bool AppendKvCompactionMetrics(const fortis::storage::KvCompactionStats& stats, char* output,
                               size_t capacity, size_t* length);

// The TypeScript maintenance sweep (a startup timer: every 10 s,
// `embedded.sweep(Date.now(), 256)`), which also performs the daily
// crash-report retention task (CrashReportsFacet.pruneExpired is the same
// bounded sweep). Expired records are already invisible to reads; this
// physically removes at most `limit` of them per tick so they cannot
// accumulate, without an unbounded sweep on a request path.
class KvExpirySweeper {
 public:
  // Runs on the sweeper thread right after each tick's sweep, with the store
  // and instant the sweep used (the TypeScript timer's
  // `priceSeries.sealDueYielding(Date.now(), 1_024)`); returns the work done
  // for `maintained()`. A long hook delays the next tick (the TypeScript
  // timer skips ticks while maintenance runs). Set before Start.
  using MaintenanceFn = size_t (*)(void* context, fortis::storage::KvStore& kv, int64_t now_ms,
                                   const KvExpirySweeper& sweeper);

  static constexpr uint32_t kIntervalMs = 10000;
  static constexpr size_t kLimit = 256;

  KvExpirySweeper() = default;
  ~KvExpirySweeper() { Stop(); }
  KvExpirySweeper(const KvExpirySweeper&) = delete;
  KvExpirySweeper& operator=(const KvExpirySweeper&) = delete;

  // A null `kv` sweeps the process store (RuntimeKvStore) once it is open.
  // `clock` (NowMs) may substitute the wall clock (tests); null uses
  // CLOCK_REALTIME. It must outlive the sweeper.
  bool Start(fortis::storage::KvStore* kv, uint32_t interval_ms, fortis::base::Clock* clock = nullptr);
  void Stop();
  uint64_t swept() const;
  void SetMaintenance(MaintenanceFn maintenance, void* context);
  // After the sweep and hook, each tick gives automatic compaction one
  // bounded budget (KvStore::MaintainCompaction) on the monotonic clock; a
  // run in progress resumes on the next tick and Stop cancels between
  // slices. Set before Start; tests shrink the policy.
  void SetCompactionPolicy(const fortis::storage::KvCompactionPolicy& policy, bool enabled = true);
  // Ticks that left a compaction run in progress.
  uint64_t compaction_ticks() const;
  // True once Stop began: a hook checks it between bounded work slices.
  bool stopping() const;
  uint64_t maintained() const;
  uint64_t ticks() const;

 private:
  static void Main(KvExpirySweeper* self);
  void Loop();

  fortis::storage::KvStore* kv_ = nullptr;
  uint32_t interval_ms_ = kIntervalMs;
  fortis::base::Clock* clock_ = nullptr;
  fortis::base::Thread thread_;
  bool started_ = false;
  bool stopping_ = false;
  uint64_t swept_ = 0;
  uint64_t maintained_ = 0;
  uint64_t ticks_ = 0;
  MaintenanceFn maintenance_ = nullptr;
  fortis::storage::KvCompactionPolicy compaction_policy_{};
  bool compaction_enabled_ = true;
  uint64_t compaction_ticks_ = 0;
  void* maintenance_context_ = nullptr;
  mutable fortis::base::Mutex mutex_;
  fortis::base::ConditionVariable wake_;
};

}  // namespace fortis::storage
