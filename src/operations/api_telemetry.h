#include "ownership.h"
#include "threading.h"
#pragma once

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "clock.h"
#include "price_history_batch_codec.h"
#include "http_server.h"

namespace fortis::operations {

/** Native port of the TypeScript TelemetryStats
 * for the routes that read it: /statusz (5- and 10-minute windows, top
 * routes, per-minute series) and /errorsz (15-minute window and the recent
 * 5xx log). Like TypeScript it records only API-scope responses (route
 * templates under /v1/, except the root-scoped /v1/media/:key), keeps one
 * in-memory ring of one-minute slots, and stores route TEMPLATES and device
 * counts only — never a URL, body or device identifier in clear.
 *
 * Bounds TypeScript does not have: at most kMaxRoutes distinct route
 * templates (the router's fixed table is smaller), kMaxRoutesPerMinute per
 * slot, and kMaxDevicesPerMinute distinct devices per slot. Beyond the device
 * bound a slot's client count stops growing (a lower bound); devices are kept
 * as 64-bit hashes. Database operation timings (TypeScript
 * `databaseOperations10m`) are not measured natively and are not reported.
 *
 * Memory: Start() allocates everything once (the 15-slot ring, the route
 * name table, the /errorsz memo, one window scratch and kClientScratch
 * hashes for the distinct-client union and one read snapshot); nothing grows
 * afterwards. A renderer copies live arrays while holding the ingestion mutex,
 * then sorts/aggregates/encodes under a reader-only mutex. DistinctClients sorts
 * at most kClientScratch hashes instead of probing each younger slot.
 */
class ApiTelemetry {
 public:
  static constexpr int64_t kMinutesTracked = 15;
  // Compact operational history is separate from the detailed 15-minute
  // route/device ring: 480 one-minute aggregates cost roughly 70 KiB.
  static constexpr int64_t kHistoryMinutes = 480;
  static constexpr size_t kHistoryCapacity = 384 * 1024;
  static constexpr size_t kRecentErrorsKept = 50;
  static constexpr uint32_t kErrorThreshold = 5;
  static constexpr size_t kDurationBucketCount = 11;
  static constexpr size_t kMaxRoutes = 128;
  static constexpr size_t kMaxRoutesPerMinute = 96;
  static constexpr size_t kMaxDevicesPerMinute = 1024;
  static constexpr size_t kMaxRouteBytes = 127;
  // At most this many device hashes are live in the ring at once.
  static constexpr size_t kClientScratch =
      static_cast<size_t>(kMinutesTracked) * kMaxDevicesPerMinute;
  // Output bounds of the status and request stats renderers.
  static constexpr size_t kErrorszCapacity = 64 * 1024;
  static constexpr size_t kStatsCapacity = 12 * 1024;
  static constexpr size_t kRequestStatsCapacity = 128 * 1024;
  // RenderStatusStats' largest possible output: every count at its type's
  // maximum digits, five top routes of kMaxRouteBytes control characters
  // (each escaped as \u00XX), every duration at the longest JS number text.
  // api_telemetry.cpp asserts it fits kStatsCapacity.
  static size_t StatusStatsWorstCaseBytes();
  static size_t HistoryWorstCaseBytes();
  /// Fixed startup storage including this object, excluding allocator metadata.
  static size_t StorageBytes();

  ApiTelemetry();
  ~ApiTelemetry();
  ApiTelemetry(const ApiTelemetry&) = delete;
  ApiTelemetry& operator=(const ApiTelemetry&) = delete;

  /** Allocate all bounded ingestion and rendering storage once. Idempotent.
   * Record/render calls may run concurrently after Start returns true. No method
   * retains route, device, completion-detail or output pointers. Callers must
   * stop/join all users before destruction; this object owns no worker thread.
   * Renderers serialize snapshot reuse; Record never waits for JSON encoding.
   * A renderer returns zero for unavailable storage/insufficient capacity; output
   * may contain a partial prefix and must be discarded. No exceptions emitted.
   */
  bool Start();

  // Completion adapters enforce API scope. Record accepts a borrowed, NUL-
  // terminated template/device and detail for this synchronous call only.
  // A null or empty route, one over kMaxRouteBytes, or a new template past
  // kMaxRoutes is dropped without bumping the revision. Late samples older than
  // the newest retained 15-minute window never evict newer slots. Counters
  // saturate at UINT64_MAX; non-finite duration sums preserve JSON null policy.
  void Record(const char* route, int status, const char* device_id,
              double duration_ms, int64_t now_ms, const http::HttpCompletionTag* detail = nullptr);
  uint64_t revision();
  // Bounded process-local statistics. Admission totals reset on restart.
  void NoteAdmission(http::HttpAdmission reason);
  size_t RenderRequestStats(int64_t now_ms, char* output, size_t capacity);
  static void OnHttpDetails(void* context, const http::HttpCompletionTag& tag, int status, double duration_ms);

  // The full /errorsz JSON document, memoised per (second, revision) like
  // the TypeScript errors page. Returns its length, or zero on failure.
  size_t RenderErrorsz(int64_t now_ms, char* output, size_t capacity);
  // The /statusz "stats" object without databaseOperations10m.
  size_t RenderStatusStats(int64_t now_ms, char* output, size_t capacity);
  // Aggregate API requests and process resources for /status-history. A
  // missing resource sample renders null; quiet request minutes render zero.
  // observed distinguishes unmeasured history from real idle minutes. Disk
  // samples have a scope and are supplied only on a fresh filesystem read;
  // switching scope resets that minute's disk range rather than mixing units.
  // Sampling uses the ingestion mutex; rendering releases it after a bounded copy.
  void SampleResources(int64_t now_ms, const double* memory_mb,
                       const double* cpu_percent, const double* disk_used_mb = nullptr,
                       bool disk_is_filesystem = true);
  size_t RenderHistory(int64_t now_ms, char* output, size_t capacity);

  static bool TracksRoute(const char* route);
  // The clock OnHttpCompletion stamps samples with (epoch milliseconds).
  // Null, the default, is CLOCK_REALTIME. Set it before the server starts:
  // it is read without the mutex. Test binaries point it at the same clock
  // the /statusz and /errorsz handlers read, so windows can be moved.
  void SetClock(fortis::base::Clock* clock);
  // The adapter matching HttpCompletionHook; `context` is an ApiTelemetry.
  static void OnHttpCompletion(void* context, const char* route, int status,
                               const char* device_id, double duration_ms);

 private:
  // Render lock precedes the ingestion lock; writers never acquire it.
  enum class ReadKind { Status, Errors, History, Requests };
  struct ReadSnapshot;
  bool CaptureSnapshot(ReadKind kind);  // caller holds reader_mutex_; copies values only
  std::unique_ptr<ReadSnapshot, base::StorageDeleter> snapshot_;
  static constexpr size_t kRouteIndexCapacity = 2 * kMaxRoutes;
  uint16_t route_index_[kRouteIndexCapacity]{};
  fortis::base::Mutex reader_mutex_;
  struct RouteMinute;
  struct Slot;
  struct WindowRoute;
  struct Window;
  struct HistorySlot;
  struct RequestMinute;
  std::unique_ptr<RequestMinute[], base::StorageDeleter> request_stats_;
  uint64_t admission_[static_cast<size_t>(http::HttpAdmission::Count)]{};
  struct RecentError {
    int64_t sec = 0;
    uint16_t route = 0;
    int status = 0;
  };

  // Borrowed snapshot view, valid only while reader_mutex_ is held.
  const Slot* SnapshotSlot(int64_t minute) const;
  Slot* TouchSlot(int64_t minute);
  int Intern(const char* route);
  bool BuildWindow(int64_t now_minute, int64_t minutes, Window* window);
  size_t DistinctClients(int64_t now_minute, int64_t span);
  bool RenderErrorsSnapshot(int64_t now_ms, fortis::codec::history_batch::OutBuffer& out);

  std::unique_ptr<Slot[], base::StorageDeleter> slots_;
  std::unique_ptr<HistorySlot[], base::StorageDeleter> history_;
  int64_t newest_minute_ = INT64_MIN;
  int64_t newest_history_minute_ = INT64_MIN;
  std::unique_ptr<char[][kMaxRouteBytes + 1], base::StorageDeleter> routes_;
  size_t route_count_ = 0;
  RecentError recent_[kRecentErrorsKept]{};
  size_t recent_start_ = 0;
  size_t recent_count_ = 0;
  uint64_t revision_ = 0;
  std::unique_ptr<char[], base::StorageDeleter> errorsz_;
  size_t errorsz_length_ = 0;
  int64_t errorsz_sec_ = -1;
  uint64_t errorsz_revision_ = 0;
  std::unique_ptr<Window, base::StorageDeleter> scratch_;
  std::unique_ptr<uint64_t[], base::StorageDeleter> clients_;
  fortis::base::Clock* clock_ = nullptr;
  fortis::base::Mutex mutex_;
};

// The TypeScript serviceForRoute: which subsystem a route template blames.
const char* ServiceForRoute(const char* route);

}  // namespace fortis::operations
