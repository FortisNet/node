#include "exception_boundary.h"
#include "api_telemetry.h"
#include "http_server.h"
#include "telemetry_aggregate.h"
#include "threading.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::operations {

using fortis::codec::history_batch::AppendInteger;
using fortis::codec::history_batch::AppendJsNumber;
using fortis::codec::history_batch::AppendJsonString;
using fortis::codec::history_batch::OutBuffer;

namespace {

// The TypeScript DURATION_BUCKETS_MS; the eleventh bucket is "above 5 s".
using detail::kDurationBoundsMs;
using detail::AddCount;
constexpr size_t kBoundCount = sizeof(kDurationBoundsMs) / sizeof(kDurationBoundsMs[0]);

int64_t floor_div(int64_t value, int64_t divisor) {
  const int64_t quotient = value / divisor;
  return (value % divisor != 0 && value < 0) ? quotient - 1 : quotient;
}

size_t ring_index(int64_t minute) {
  const int64_t length = ApiTelemetry::kMinutesTracked;
  return static_cast<size_t>(((minute % length) + length) % length);
}

size_t history_index(int64_t minute) {
  const int64_t length = ApiTelemetry::kHistoryMinutes;
  return static_cast<size_t>(((minute % length) + length) % length);
}

using detail::AppendCount;


uint64_t hash_text(const char* id) {
  uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(id); *p; ++p) {
    hash ^= *p;
    hash *= 1099511628211ULL;
  }
  return hash ? hash : 1;
}

bool starts_with(const char* text, const char* prefix) {
  return strncmp(text, prefix, strlen(prefix)) == 0;
}

}  // namespace

struct ApiTelemetry::RouteMinute {
  uint16_t route = 0;
  uint64_t requests = 0;
  uint64_t errors = 0;
  double duration_ms = 0;
  double max_duration_ms = 0;
  uint64_t buckets[kDurationBucketCount]{};
};

struct ApiTelemetry::Slot {
  int64_t minute = -1;
  uint64_t requests = 0;
  uint64_t errors = 0;
  size_t route_count = 0;
  RouteMinute routes[kMaxRoutesPerMinute];
  uint16_t route_indices[kMaxRoutes]{};
  size_t device_count = 0;
  uint64_t devices[kMaxDevicesPerMinute];

  bool HasDevice(uint64_t hash) const {
    for (size_t probe = 0, i = hash % kMaxDevicesPerMinute; probe < kMaxDevicesPerMinute;
         ++probe, i = (i + 1) % kMaxDevicesPerMinute) {
      if (devices[i] == hash) return true;
      if (!devices[i]) return false;
    }
    return false;
  }
  void AddDevice(uint64_t hash) {
    // One slot always stays empty so every probe terminates.
    if (device_count + 1 >= kMaxDevicesPerMinute || HasDevice(hash)) return;
    size_t i = hash % kMaxDevicesPerMinute;
    while (devices[i]) i = (i + 1) % kMaxDevicesPerMinute;
    devices[i] = hash;
    ++device_count;
  }
};

namespace {
constexpr const char* kMethods[] = {"OTHER", "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"};
constexpr const char* kPlatforms[] = {"unknown", "iOS", "Android", "macOS", "Windows", "Linux"};
constexpr const char* kAdmissionNames[] = {
    "none", "connectionQueue", "spotSlots", "responseSlots", "spotOwner", "snapshotUnavailable",
    "acceptResourceExhaustion", "snapshotAuthorityBusy", "connectionQueueExpired"};
static_assert(sizeof(kAdmissionNames) / sizeof(kAdmissionNames[0]) ==
              static_cast<size_t>(http::HttpAdmission::Count));

size_t method_index(const char* method) {
  for (size_t i = 1; i < sizeof(kMethods) / sizeof(kMethods[0]); ++i)
    if (method && strcmp(method, kMethods[i]) == 0) return i;
  return 0;
}
}  // namespace

struct ApiTelemetry::RequestMinute {
  using Total = detail::RequestTotals;
  static_assert(kMaxRoutes * 2 < UINT16_MAX);
  // Sparse entries keep storage proportional to the fixed router rather than
  // reserving a full route × method × OS cube for every minute.
  struct Entry { uint16_t route; uint8_t method; Total total; };
  static constexpr size_t kCapacity = 2 * kMaxRoutes;
  int64_t minute = INT64_MIN;
  size_t count = 0;
  uint64_t omitted = 0;
  Entry entries[kCapacity]{};
  uint16_t indices[kMaxRoutes][sizeof(kMethods) / sizeof(kMethods[0])]{};
  Total platforms[sizeof(kPlatforms) / sizeof(kPlatforms[0])]{};
};

struct ApiTelemetry::HistorySlot {
  struct Range {
    double min = 0;
    double max = 0;
    double sum = 0;
    uint64_t samples = 0;
    void Add(double value) {
      if (!isfinite(value) || value < 0) return;
      if (!samples || value < min) min = value;
      if (!samples || value > max) max = value;
      sum += value;
      samples = AddCount(samples, 1);
    }
  };
  int64_t minute = INT64_MIN;
  Range memory_mb;
  Range cpu_percent;
  Range disk_used_mb;
  uint8_t disk_scope = 0;
  uint64_t requests = 0;
  uint64_t errors = 0;
  double duration_sum_ms = 0;
  double duration_max_ms = 0;
};

struct ApiTelemetry::WindowRoute {
  uint16_t route = 0;
  uint64_t requests = 0;
  uint64_t errors = 0;
  double duration_ms = 0;
  double max_duration_ms = 0;
  uint64_t buckets[kDurationBucketCount]{};
};

struct ApiTelemetry::Window {
  uint64_t requests = 0;
  uint64_t errors = 0;
  size_t clients = 0;
  size_t route_count = 0;
  WindowRoute routes[kMaxRoutes];
};

// Writers never read this storage. Renderers serialize its reuse with the
// reader mutex and capture only the required live arrays under mutex_. Sorting,
// aggregation and JSON encoding then run without blocking Record().
struct ApiTelemetry::ReadSnapshot {
  Slot slots[kMinutesTracked];
  HistorySlot history[kHistoryMinutes];
  RequestMinute requests[kMinutesTracked];
  char routes[kMaxRoutes][kMaxRouteBytes + 1];
  size_t route_count = 0;
  RecentError recent[kRecentErrorsKept];
  size_t recent_count = 0;
  size_t recent_start = 0;
  uint64_t revision = 0;
  uint64_t admission[static_cast<size_t>(http::HttpAdmission::Count)]{};
  RequestMinute::Total totals[kMaxRoutes * (sizeof(kMethods) / sizeof(kMethods[0]))];
};

size_t ApiTelemetry::StorageBytes() {
  return sizeof(ApiTelemetry) + sizeof(ReadSnapshot) + sizeof(Slot) * kMinutesTracked +
      sizeof(HistorySlot) * kHistoryMinutes + sizeof(RequestMinute) * kMinutesTracked +
      (kMaxRouteBytes + 1) * kMaxRoutes + kErrorszCapacity + sizeof(Window) +
      sizeof(uint64_t) * kClientScratch;
}

ApiTelemetry::ApiTelemetry() = default;

ApiTelemetry::~ApiTelemetry() = default;

bool ApiTelemetry::Start() {
  fortis::base::LockGuard reader_lock(reader_mutex_);
  fortis::base::LockGuard lock(mutex_);
  if (slots_) return true;
  auto snapshot = base::AllocateZeroedStorage<ReadSnapshot>(1, sizeof(ReadSnapshot));
  auto slots = base::AllocateZeroedStorage<Slot[]>(kMinutesTracked, sizeof(Slot));
  auto history = base::AllocateZeroedStorage<HistorySlot[]>(kHistoryMinutes, sizeof(HistorySlot));
  auto request_stats = base::AllocateZeroedStorage<RequestMinute[]>(kMinutesTracked, sizeof(RequestMinute));
  auto routes = base::AllocateZeroedStorage<char[][kMaxRouteBytes + 1]>(kMaxRoutes, kMaxRouteBytes + 1);
  auto errorsz = base::AllocateStorage<char[]>(kErrorszCapacity);
  auto scratch = base::AllocateZeroedStorage<Window>(1, sizeof(Window));
  auto clients = base::AllocateZeroedStorage<uint64_t[]>(kClientScratch, sizeof(uint64_t));
  if (!snapshot || !slots || !history || !request_stats || !routes || !errorsz || !scratch || !clients) {
    return false;
  }
  for (int64_t i = 0; i < kMinutesTracked; ++i) slots[i].minute = -1;
  for (int64_t i = 0; i < kHistoryMinutes; ++i) history[i].minute = INT64_MIN;
  snapshot_ = std::move(snapshot);
  slots_ = std::move(slots);
  history_ = std::move(history);
  request_stats_ = std::move(request_stats);
  for (int64_t i = 0; i < kMinutesTracked; ++i) request_stats_.get()[i].minute = INT64_MIN;
  routes_ = std::move(routes);
  errorsz_ = std::move(errorsz);
  scratch_ = std::move(scratch);
  clients_ = std::move(clients);
  return true;
}

void ApiTelemetry::SetClock(fortis::base::Clock* clock) {
  clock_ = clock;
}

bool ApiTelemetry::TracksRoute(const char* route) {
  // apiPlugin's onResponse hook covers its /v1 routes; /v1/media/:key is
  // registered at the root scope beside the landing page.
  return route && starts_with(route, "/v1/") && strcmp(route, "/v1/media/:key") != 0;
}

void ApiTelemetry::OnHttpCompletion(void* context, const char* route, int status,
                                    const char* device_id, double duration_ms) {
  auto* telemetry = static_cast<ApiTelemetry*>(context);
  if (!telemetry || !TracksRoute(route)) return;
  if (telemetry->clock_) {
    telemetry->Record(route, status, device_id, duration_ms,
                      telemetry->clock_->NowMs());
    return;
  }
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) return;
  telemetry->Record(route, status, device_id, duration_ms,
                    static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000);
}

void ApiTelemetry::NoteAdmission(http::HttpAdmission reason) {
  fortis::base::LockGuard lock(mutex_);
  const size_t index = static_cast<size_t>(reason);
  if (index && index < static_cast<size_t>(http::HttpAdmission::Count)) admission_[index] = AddCount(admission_[index], 1);
}

void ApiTelemetry::OnHttpDetails(void* context, const http::HttpCompletionTag& tag,
                                 int status, double duration_ms) {
  auto* telemetry = static_cast<ApiTelemetry*>(context);
  if (!telemetry) return;
  if (!tag.route) {
    telemetry->NoteAdmission(tag.admission);
    return;
  }
  if (!TracksRoute(tag.route)) return;
  timespec now{};
  if (!telemetry->clock_ && clock_gettime(CLOCK_REALTIME, &now) != 0) return;
  const int64_t ms = telemetry->clock_ ? telemetry->clock_->NowMs()
      : static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
  telemetry->Record(tag.route, status, tag.device_id, duration_ms, ms, &tag);
}

int ApiTelemetry::Intern(const char* route) {
  const size_t length = strnlen(route, kMaxRouteBytes + 1);
  if (length > kMaxRouteBytes) return -1;
  size_t bucket = hash_text(route) % kRouteIndexCapacity;
  while (route_index_[bucket]) {
    const size_t index = route_index_[bucket] - 1;
    if (strcmp(routes_.get()[index], route) == 0) return static_cast<int>(index);
    bucket = (bucket + 1) % kRouteIndexCapacity;
  }
  if (route_count_ == kMaxRoutes) return -1;
  memcpy(routes_.get()[route_count_], route, length + 1);
  route_index_[bucket] = static_cast<uint16_t>(route_count_ + 1);
  return static_cast<int>(route_count_++);
}

const ApiTelemetry::Slot* ApiTelemetry::SnapshotSlot(int64_t minute) const {
  const Slot* slot = &snapshot_->slots[ring_index(minute)];
  return slot->minute == minute ? slot : nullptr;
}

ApiTelemetry::Slot* ApiTelemetry::TouchSlot(int64_t minute) {
  Slot* slot = &slots_.get()[ring_index(minute)];
  if (slot->minute != minute) {
    slot->minute = minute;
    slot->requests = slot->errors = 0;
    slot->route_count = 0;
    memset(slot->route_indices, 0, sizeof(slot->route_indices));
    slot->device_count = 0;
    memset(slot->devices, 0, sizeof(slot->devices));
  }
  return slot;
}

void ApiTelemetry::Record(const char* route, int status, const char* device_id,
                          double duration_ms, int64_t now_ms, const http::HttpCompletionTag* detail) {
  if (!route || !*route) return;
  const uint64_t hashed_device = device_id && *device_id ? hash_text(device_id) : 0;
  fortis::base::LockGuard lock(mutex_);
  if (!slots_) return;
  const int interned = Intern(route);
  if (interned < 0) return;
  revision_ = AddCount(revision_, 1);
  const int64_t minute = floor_div(now_ms, 60000);
  if (minute > newest_history_minute_) newest_history_minute_ = minute;
  if (minute > newest_history_minute_ - kHistoryMinutes) {
    HistorySlot* history = &history_.get()[history_index(minute)];
    if (history->minute != minute) {
      *history = HistorySlot{};
      history->minute = minute;
    }
    history->requests = AddCount(history->requests, 1);
    if (status >= 500) history->errors = AddCount(history->errors, 1);
    const double duration = isfinite(duration_ms) && duration_ms > 0 ? duration_ms : 0;
    history->duration_sum_ms += duration;
    if (duration > history->duration_max_ms) history->duration_max_ms = duration;
  }
  if (minute > newest_minute_) newest_minute_ = minute;
  if (minute <= newest_minute_ - kMinutesTracked) return;
  if (detail) {
    const size_t reason = static_cast<size_t>(detail->admission);
    if (reason && reason < static_cast<size_t>(http::HttpAdmission::Count)) admission_[reason] = AddCount(admission_[reason], 1);
    RequestMinute& sample = request_stats_.get()[ring_index(minute)];
    if (sample.minute != minute) {
      sample.minute = minute;
      sample.count = 0;
      sample.omitted = 0;
      memset(sample.indices, 0, sizeof(sample.indices));
      for (auto& platform : sample.platforms) platform = {};
    }
    const size_t method = method_index(detail->method);
    const double elapsed = isfinite(duration_ms) && duration_ms > 0 ? duration_ms : 0;
    const double queue = isfinite(detail->queue_ms) && detail->queue_ms > 0 ? detail->queue_ms : 0;
    const size_t platform = detail->platform < sizeof(kPlatforms) / sizeof(kPlatforms[0])
        ? detail->platform : 0;
    sample.platforms[platform].Add(status, elapsed, queue, detail->owner_ms);
    uint16_t& index = sample.indices[static_cast<size_t>(interned)][method];
    if (!index && sample.count < RequestMinute::kCapacity) {
      sample.entries[sample.count] = {static_cast<uint16_t>(interned), static_cast<uint8_t>(method), {}};
      index = static_cast<uint16_t>(++sample.count);
    }
    if (index) sample.entries[index - 1].total.Add(status, elapsed, queue, detail->owner_ms);
    else sample.omitted = AddCount(sample.omitted, 1);
  }
  Slot* slot = TouchSlot(minute);
  slot->requests = AddCount(slot->requests, 1);
  const bool error = status >= 500;
  if (error) {
    slot->errors = AddCount(slot->errors, 1);
    RecentError& sample = recent_[(recent_start_ + recent_count_) % kRecentErrorsKept];
    sample = {floor_div(now_ms, 1000), static_cast<uint16_t>(interned), status};
    if (recent_count_ < kRecentErrorsKept) ++recent_count_;
    else recent_start_ = (recent_start_ + 1) % kRecentErrorsKept;
  }
  uint16_t& index = slot->route_indices[static_cast<size_t>(interned)];
  if (!index && slot->route_count < kMaxRoutesPerMinute) {
    slot->routes[slot->route_count] = RouteMinute{};
    slot->routes[slot->route_count].route = static_cast<uint16_t>(interned);
    index = static_cast<uint16_t>(++slot->route_count);
  }
  RouteMinute* entry = index ? &slot->routes[index - 1] : nullptr;
  if (entry) {
    const double duration = duration_ms > 0 ? duration_ms : 0;
    entry->requests = AddCount(entry->requests, 1);
    if (error) entry->errors = AddCount(entry->errors, 1);
    entry->duration_ms += duration;
    if (duration > entry->max_duration_ms) entry->max_duration_ms = duration;
    const size_t bucket = detail::DurationBucket(duration);
    entry->buckets[bucket] = AddCount(entry->buckets[bucket], 1);
  }
  if (hashed_device) slot->AddDevice(hashed_device);

}

void ApiTelemetry::SampleResources(int64_t now_ms, const double* memory_mb,
                                   const double* cpu_percent, const double* disk_used_mb, bool disk_is_filesystem) {
  fortis::base::LockGuard lock(mutex_);
  if (!history_) return;
  const int64_t minute = floor_div(now_ms, 60000);
  if (minute > newest_history_minute_) newest_history_minute_ = minute;
  if (minute <= newest_history_minute_ - kHistoryMinutes) return;
  HistorySlot* slot = &history_.get()[history_index(minute)];
  if (slot->minute != minute) {
    *slot = HistorySlot{};
    slot->minute = minute;
  }
  if (memory_mb) slot->memory_mb.Add(*memory_mb);
  if (cpu_percent) slot->cpu_percent.Add(*cpu_percent);
  if (disk_used_mb) {
    const uint8_t scope = disk_is_filesystem ? 1 : 2;
    if (slot->disk_scope != scope) slot->disk_used_mb = {};
    slot->disk_scope = scope;
    slot->disk_used_mb.Add(*disk_used_mb);
  }
}

bool ApiTelemetry::CaptureSnapshot(ReadKind kind) {
  fortis::base::LockGuard lock(mutex_);
  if (!slots_) return false;
  if (kind == ReadKind::History) {
    memcpy(snapshot_->history, history_.get(), sizeof(snapshot_->history));
    return true;
  }
  if (kind == ReadKind::Requests) {
    memcpy(snapshot_->requests, request_stats_.get(), sizeof(snapshot_->requests));
    memcpy(snapshot_->admission, admission_, sizeof(admission_));
  } else {
    memcpy(snapshot_->slots, slots_.get(), sizeof(snapshot_->slots));
    memcpy(snapshot_->recent, recent_, sizeof(recent_));
    snapshot_->recent_count = recent_count_;
    snapshot_->recent_start = recent_start_;
  }
  memcpy(snapshot_->routes, routes_.get(), route_count_ * sizeof(snapshot_->routes[0]));
  snapshot_->route_count = route_count_;
  snapshot_->revision = revision_;
  return true;
}

size_t ApiTelemetry::RenderHistory(int64_t now_ms, char* output, size_t capacity) {
  if (!output || !capacity) return 0;
  fortis::base::LockGuard lock(reader_mutex_);
  if (!CaptureSnapshot(ReadKind::History)) return 0;
  OutBuffer out{reinterpret_cast<uint8_t*>(output), capacity};
  out.Text("{\"slotDurationSec\":60,\"maxMinutes\":480,\"slots\":[");
  const int64_t current = floor_div(now_ms, 60000);
  auto append_range = [&](const HistorySlot::Range* range) {
    if (!range || !range->samples) { out.Text("null"); return; }
    out.Text("{\"min\":"); AppendJsNumber(out, range->min);
    out.Text(",\"avg\":"); AppendJsNumber(out, range->sum / range->samples);
    out.Text(",\"max\":"); AppendJsNumber(out, range->max);
    out.Text(",\"samples\":"); AppendCount(out, range->samples);
    out.Byte('}');
  };
  for (int64_t age = kHistoryMinutes - 1; age >= 0; --age) {
    if (age != kHistoryMinutes - 1) out.Byte(',');
    const int64_t minute = current - age;
    const HistorySlot* slot = &snapshot_->history[history_index(minute)];
    if (slot->minute != minute) slot = nullptr;
    out.Text("{\"created\":"); AppendInteger(out, minute * 60);
    out.Text(",\"duration\":60,\"observed\":");
    out.Text(slot ? "true" : "false");
    out.Text(",\"memoryMb\":");
    append_range(slot ? &slot->memory_mb : nullptr);
    out.Text(",\"cpuPercent\":");
    append_range(slot ? &slot->cpu_percent : nullptr);
    out.Text(",\"diskScope\":");
    out.Text(slot && slot->disk_scope == 1 ? "\"filesystem\"" : slot && slot->disk_scope == 2 ? "\"data-tree\"" : "null");
    out.Text(",\"diskUsedMb\":");
    append_range(slot ? &slot->disk_used_mb : nullptr);
    out.Text(",\"requests\":{\"total\":");
    AppendCount(out, slot ? slot->requests : 0);
    out.Text(",\"errors\":"); AppendCount(out, slot ? slot->errors : 0);
    out.Text(",\"avgDurationMs\":");
    AppendJsNumber(out, slot && slot->requests ? slot->duration_sum_ms / slot->requests : 0);
    out.Text(",\"maxDurationMs\":");
    AppendJsNumber(out, slot ? slot->duration_max_ms : 0);
    out.Text("}}");
  }
  out.Text("]}");
  return out.ok ? out.used : 0;
}

uint64_t ApiTelemetry::revision() {
  fortis::base::LockGuard lock(mutex_);
  return revision_;
}

namespace {

int compare_hashes(const void* left, const void* right) {
  const uint64_t a = *static_cast<const uint64_t*>(left);
  const uint64_t b = *static_cast<const uint64_t*>(right);
  return a < b ? -1 : (a > b ? 1 : 0);
}

}  // namespace

// The size of the union of the live slots' device sets (TypeScript unions
// its per-minute Sets). Each slot's set is already distinct, so gathering
// every hash, sorting and counting runs is exact; it costs O(n log n) for
// n <= kClientScratch, where probing each hash in every younger slot cost
// tens of milliseconds under the mutex once slots were full.
size_t ApiTelemetry::DistinctClients(int64_t now_minute, int64_t span) {
  size_t count = 0;
  for (int64_t age = 0; age < span; ++age) {
    const Slot* slot = SnapshotSlot(now_minute - age);
    if (!slot) continue;
    for (size_t i = 0; i < kMaxDevicesPerMinute && count < kClientScratch; ++i)
      if (slot->devices[i]) clients_.get()[count++] = slot->devices[i];
  }
  if (count < 2) return count;
  qsort(clients_.get(), count, sizeof(uint64_t), fortis::base::FatalCallback<compare_hashes>);
  size_t clients = 1;
  for (size_t i = 1; i < count; ++i)
    if (clients_.get()[i] != clients_.get()[i - 1]) ++clients;
  return clients;
}

bool ApiTelemetry::BuildWindow(int64_t now_minute, int64_t minutes, Window* window) {
  const int64_t span = minutes < 1 ? 1 : (minutes > kMinutesTracked ? kMinutesTracked : minutes);
  window->requests = window->errors = 0;
  window->route_count = 0;
  for (int64_t age = 0; age < span; ++age) {
    const Slot* slot = SnapshotSlot(now_minute - age);
    if (!slot) continue;
    window->requests = AddCount(window->requests, slot->requests);
    window->errors = AddCount(window->errors, slot->errors);
    for (size_t r = 0; r < slot->route_count; ++r) {
      const RouteMinute& minute = slot->routes[r];
      WindowRoute* total = nullptr;
      for (size_t i = 0; i < window->route_count && !total; ++i)
        if (window->routes[i].route == minute.route) total = &window->routes[i];
      if (!total) {
        total = &window->routes[window->route_count++];
        *total = WindowRoute{};
        total->route = minute.route;
      }
      total->requests = AddCount(total->requests, minute.requests);
      total->errors = AddCount(total->errors, minute.errors);
      total->duration_ms += minute.duration_ms;
      if (minute.max_duration_ms > total->max_duration_ms)
        total->max_duration_ms = minute.max_duration_ms;
      for (size_t b = 0; b < kDurationBucketCount; ++b) total->buckets[b] = AddCount(total->buckets[b], minute.buckets[b]);
    }
  }
  window->clients = DistinctClients(now_minute, span);
  // Array.prototype.sort is stable: most requests first, ties keep order.
  for (size_t i = 1; i < window->route_count; ++i) {
    const WindowRoute moving = window->routes[i];
    size_t j = i;
    while (j > 0 && window->routes[j - 1].requests < moving.requests) {
      window->routes[j] = window->routes[j - 1];
      --j;
    }
    window->routes[j] = moving;
  }
  return true;
}

namespace {

// The TypeScript p95: the upper bound of the bucket holding the 95th percentile,
// or the maximum when that is the overflow bucket.
double duration_percentile(uint64_t count, const uint64_t* buckets, double max_duration_ms,
           double quantile = 0.95) {
  if (!count) return 0;
  const double rank = ceil(static_cast<double>(count) * quantile);
  double seen = 0;
  for (size_t i = 0; i < ApiTelemetry::kDurationBucketCount; ++i) {
    seen += buckets[i];
    if (seen >= rank) return i < kBoundCount ? kDurationBoundsMs[i] : max_duration_ms;
  }
  return max_duration_ms;
}


}  // namespace

bool ApiTelemetry::RenderErrorsSnapshot(int64_t now_ms, OutBuffer& out) {
  Window& window = *scratch_;
  BuildWindow(floor_div(now_ms, 60000), kMinutesTracked, &window);
  // routes.filter(errors > 0).sort((a, b) => b.errors - a.errors), stable.
  size_t order[kMaxRoutes];
  size_t count = 0;
  for (size_t i = 0; i < window.route_count; ++i) {
    if (!window.routes[i].errors) continue;
    size_t j = count++;
    while (j > 0 && window.routes[order[j - 1]].errors < window.routes[i].errors) {
      order[j] = order[j - 1];
      --j;
    }
    order[j] = i;
  }
  out.Text("{\"ok\":true,\"threshold\":");
  AppendCount(out, kErrorThreshold);
  out.Text(",\"windowMinutes\":");
  AppendCount(out, static_cast<uint64_t>(kMinutesTracked));
  out.Text(",\"totalErrors\":");
  AppendCount(out, window.errors);
  out.Text(",\"totalRequests\":");
  AppendCount(out, window.requests);
  out.Text(",\"routes\":[");
  for (size_t k = 0; k < count; ++k) {
    const WindowRoute& route = window.routes[order[k]];
    const char* name = snapshot_->routes[route.route];
    out.Text(k ? ",{\"service\":" : "{\"service\":");
    const char* service = ServiceForRoute(name);
    AppendJsonString(out, service, strlen(service));
    out.Text(",\"route\":");
    AppendJsonString(out, name, strlen(name));
    out.Text(",\"requests\":");
    AppendCount(out, route.requests);
    out.Text(",\"errors\":");
    AppendCount(out, route.errors);
    out.Text(route.errors >= kErrorThreshold ? ",\"overThreshold\":true}"
                                             : ",\"overThreshold\":false}");
  }
  out.Text("],\"recent\":[");
  for (size_t k = 0; k < snapshot_->recent_count; ++k) {
    const RecentError& sample =
        snapshot_->recent[(snapshot_->recent_start + snapshot_->recent_count - 1 - k) % kRecentErrorsKept];
    const char* name = snapshot_->routes[sample.route];
    out.Text(k ? ",{\"sec\":" : "{\"sec\":");
    AppendInteger(out, sample.sec);
    out.Text(",\"service\":");
    const char* service = ServiceForRoute(name);
    AppendJsonString(out, service, strlen(service));
    out.Text(",\"route\":");
    AppendJsonString(out, name, strlen(name));
    out.Text(",\"status\":");
    AppendInteger(out, sample.status);
    out.Byte('}');
  }
  out.Text("]}");
  return out.ok;
}

size_t ApiTelemetry::RenderErrorsz(int64_t now_ms, char* output, size_t capacity) {
  if (!output) return 0;
  fortis::base::LockGuard lock(reader_mutex_);
  if (!CaptureSnapshot(ReadKind::Errors)) return 0;
  const int64_t sec = floor_div(now_ms, 1000);
  if (errorsz_sec_ != sec || errorsz_revision_ != snapshot_->revision || !errorsz_length_) {
    OutBuffer out{reinterpret_cast<uint8_t*>(errorsz_.get()), kErrorszCapacity};
    if (!RenderErrorsSnapshot(now_ms, out)) {
      errorsz_length_ = 0;
      return 0;
    }
    errorsz_length_ = out.used;
    errorsz_sec_ = sec;
    errorsz_revision_ = snapshot_->revision;
  }
  if (errorsz_length_ > capacity) return 0;
  memcpy(output, errorsz_.get(), errorsz_length_);
  return errorsz_length_;
}

namespace {

constexpr size_t literal(const char* text) {
  size_t length = 0;
  while (text[length]) ++length;
  return length;
}

// Digits of the widest value each field can hold (int64 with sign for
// startSec, size_t client counts, uint64 request/error/bucket counts) and of
// the longest text AppendJsNumber writes for a non-negative double
// ("0.0000012345678901234567"; null and integers are shorter).
constexpr size_t kInt64Digits = 20;
constexpr size_t kSizeDigits = 20;
constexpr size_t kCounterDigits = 20;
constexpr size_t kJsNumberDigits = 24;

constexpr size_t kRangeWorstBytes = literal("{\"min\":") + kJsNumberDigits +
    literal(",\"avg\":") + kJsNumberDigits + literal(",\"max\":") + kJsNumberDigits +
    literal(",\"samples\":") + kCounterDigits + 1;
constexpr size_t kHistorySlotWorstBytes = literal(",{\"created\":") + kInt64Digits +
    literal(",\"duration\":60,\"observed\":false,\"memoryMb\":") + kRangeWorstBytes +
    literal(",\"cpuPercent\":") + kRangeWorstBytes + literal(",\"diskScope\":\"filesystem\",\"diskUsedMb\":") +
    kRangeWorstBytes + literal(",\"requests\":{\"total\":") + kCounterDigits +
    literal(",\"errors\":") + kCounterDigits + literal(",\"avgDurationMs\":") + kJsNumberDigits +
    literal(",\"maxDurationMs\":") + kJsNumberDigits + literal("}}");
constexpr size_t kHistoryWorstBytes = literal("{\"slotDurationSec\":60,\"maxMinutes\":480,\"slots\":[") +
    static_cast<size_t>(ApiTelemetry::kHistoryMinutes) * kHistorySlotWorstBytes + literal("]}");
static_assert(kHistoryWorstBytes <= ApiTelemetry::kHistoryCapacity,
              "the full-width 480-minute history must fit its default buffer");

constexpr size_t kTotalsWorstBytes = literal("{\"clients\":") + kSizeDigits +
    literal(",\"requests\":") + kCounterDigits + literal(",\"errors\":") + kCounterDigits + 1;
constexpr size_t kTopRouteWorstBytes = literal(",{\"route\":") +
    2 + 6 * ApiTelemetry::kMaxRouteBytes +
    literal(",\"requests\":") + kCounterDigits + literal(",\"errors\":") + kCounterDigits +
    literal(",\"durationMs\":") + kJsNumberDigits +
    literal(",\"maxDurationMs\":") + kJsNumberDigits +
    literal(",\"durationP95Ms\":") + kJsNumberDigits +
    literal(",\"durationBuckets\":[") +
    ApiTelemetry::kDurationBucketCount * (kCounterDigits + 1) + literal("]}");
constexpr size_t kMinuteWorstBytes = literal(",{\"startSec\":") + kInt64Digits +
    literal(",\"requests\":") + kCounterDigits + literal(",\"errors\":") + kCounterDigits +
    literal(",\"clients\":") + kSizeDigits + 1;
constexpr size_t kTopRoutesShown = 5;
constexpr size_t kStatusStatsWorstBytes = literal("{\"window5m\":") + kTotalsWorstBytes +
    literal(",\"window10m\":") + kTotalsWorstBytes + literal(",\"topRoutes10m\":[") +
    kTopRoutesShown * kTopRouteWorstBytes + literal("],\"perMinute\":[") +
    static_cast<size_t>(ApiTelemetry::kMinutesTracked) * kMinuteWorstBytes + literal("]}");
static_assert(kStatusStatsWorstBytes <= ApiTelemetry::kStatsCapacity,
              "the /statusz stats block must always fit its buffer");

}  // namespace

size_t ApiTelemetry::HistoryWorstCaseBytes() { return kHistoryWorstBytes; }

size_t ApiTelemetry::StatusStatsWorstCaseBytes() { return kStatusStatsWorstBytes; }

size_t ApiTelemetry::RenderStatusStats(int64_t now_ms, char* output, size_t capacity) {
  if (!output) return 0;
  fortis::base::LockGuard lock(reader_mutex_);
  if (!CaptureSnapshot(ReadKind::Status)) return 0;
  const int64_t now_minute = floor_div(now_ms, 60000);
  OutBuffer out{reinterpret_cast<uint8_t*>(output), capacity};
  Window& window = *scratch_;
  auto totals = [&out](const Window& source) {
    out.Text("{\"clients\":");
    AppendCount(out, source.clients);
    out.Text(",\"requests\":");
    AppendCount(out, source.requests);
    out.Text(",\"errors\":");
    AppendCount(out, source.errors);
    out.Byte('}');
  };
  BuildWindow(now_minute, 5, &window);
  out.Text("{\"window5m\":");
  totals(window);
  BuildWindow(now_minute, 10, &window);
  out.Text(",\"window10m\":");
  totals(window);
  out.Text(",\"topRoutes10m\":[");
  for (size_t i = 0; i < window.route_count && i < kTopRoutesShown; ++i) {
    const WindowRoute& route = window.routes[i];
    const char* name = snapshot_->routes[route.route];
    out.Text(i ? ",{\"route\":" : "{\"route\":");
    AppendJsonString(out, name, strlen(name));
    out.Text(",\"requests\":");
    AppendCount(out, route.requests);
    out.Text(",\"errors\":");
    AppendCount(out, route.errors);
    out.Text(",\"durationMs\":");
    AppendJsNumber(out, route.duration_ms);
    out.Text(",\"maxDurationMs\":");
    AppendJsNumber(out, route.max_duration_ms);
    out.Text(",\"durationP95Ms\":");
    AppendJsNumber(out, duration_percentile(route.requests, route.buckets, route.max_duration_ms));
    out.Text(",\"durationBuckets\":[");
    for (size_t b = 0; b < kDurationBucketCount; ++b) {
      if (b) out.Byte(',');
      AppendCount(out, route.buckets[b]);
    }
    out.Text("]}");
  }
  out.Text("],\"perMinute\":[");
  for (int64_t age = kMinutesTracked - 1; age >= 0; --age) {
    const int64_t minute = now_minute - age;
    const Slot* slot = SnapshotSlot(minute);
    out.Text(age == kMinutesTracked - 1 ? "{\"startSec\":" : ",{\"startSec\":");
    AppendInteger(out, minute * 60);
    out.Text(",\"requests\":");
    AppendCount(out, slot ? slot->requests : 0);
    out.Text(",\"errors\":");
    AppendCount(out, slot ? slot->errors : 0);
    out.Text(",\"clients\":");
    AppendCount(out, slot ? slot->device_count : 0);
    out.Byte('}');
  }
  out.Text("]}");
  return out.ok ? out.used : 0;
}

const char* ServiceForRoute(const char* route) {
  if (!route) return "API";
  if (starts_with(route, "/v1/prices")) return "Prices";
  if (strstr(route, "/tokens/search")) return "Token Discovery";
  if (strstr(route, "/token/")) return "Token 360";
  if (strstr(route, "/address/")) return "Address Data";
  if (starts_with(route, "/v1/devices")) return "Devices";
  if (starts_with(route, "/v1/exchange")) return "Exchange";
  if (starts_with(route, "/v1/bittensor")) return "Bittensor";
  if (starts_with(route, "/v1/crash-reports")) return "Crash Reports";
  if (starts_with(route, "/v1/sync")) return "Sync";
  if (starts_with(route, "/v1/networks")) return "Networks";
  return "API";
}

size_t ApiTelemetry::RenderRequestStats(int64_t now_ms, char* output, size_t capacity) {
  if (!output || !capacity) return 0;
  constexpr size_t method_count = sizeof(kMethods) / sizeof(kMethods[0]);
  fortis::base::LockGuard reader_lock(reader_mutex_);
  if (!CaptureSnapshot(ReadKind::Requests)) return 0;
  auto& totals = snapshot_->totals;
  for (auto& total : totals) total = {};
  const auto& names = snapshot_->routes;
  const size_t count = snapshot_->route_count;
  uint64_t omitted = 0;
  const auto& admission = snapshot_->admission;
  RequestMinute::Total platforms[sizeof(kPlatforms) / sizeof(kPlatforms[0])]{};
  constexpr const char* groups[] = {"Prices", "Token Discovery", "Token 360", "Address Data", "Devices",
      "Exchange", "Bittensor", "Crash Reports", "Sync", "Networks", "AI", "News", "API"};
  RequestMinute::Total group_totals[sizeof(groups) / sizeof(groups[0])]{};
  const auto group_for = [&](const char* route) {
    const char* service = ServiceForRoute(route);
    if (starts_with(route, "/v1/ai")) service = "AI";
    else if (starts_with(route, "/v1/news")) service = "News";
    for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); ++i)
      if (strcmp(service, groups[i]) == 0) return i;
    return sizeof(groups) / sizeof(groups[0]) - 1;
  };
  {
    const int64_t current = floor_div(now_ms, 60000);
    for (int64_t age = 0; age < kMinutesTracked; ++age) {
      const auto& sample = snapshot_->requests[ring_index(current - age)];
      if (sample.minute != current - age) continue;
      omitted = AddCount(omitted, sample.omitted);
      for (size_t i = 0; i < sample.count; ++i) {
        const auto& entry = sample.entries[i];
        totals[entry.route * method_count + entry.method].Merge(entry.total);
      }
      for (size_t i = 0; i < sizeof(kPlatforms) / sizeof(kPlatforms[0]); ++i)
        platforms[i].Merge(sample.platforms[i]);
    }
  }
  OutBuffer out{reinterpret_cast<uint8_t*>(output), capacity};
  auto emit_total = [&](const RequestMinute::Total& total) {
    out.Text(",\"requests\":"); AppendCount(out, total.requests);
    out.Text(",\"errors5xx\":"); AppendCount(out, total.errors);
    out.Text(",\"errors4xx\":"); AppendCount(out, total.client_errors);
    out.Text(",\"rateLimited\":"); AppendCount(out, total.limited);
    out.Text(",\"avgMs\":"); AppendJsNumber(out, total.requests ? total.duration / total.requests : 0);
    out.Text(",\"p50Ms\":"); AppendJsNumber(out, duration_percentile(total.requests, total.histogram, total.max_duration, 0.50));
    out.Text(",\"p95Ms\":"); AppendJsNumber(out, duration_percentile(total.requests, total.histogram, total.max_duration));
    out.Text(",\"p99Ms\":"); AppendJsNumber(out, duration_percentile(total.requests, total.histogram, total.max_duration, 0.99));
    out.Text(",\"maxMs\":"); AppendJsNumber(out, total.max_duration);
    out.Text(",\"avgQueueMs\":"); AppendJsNumber(out, total.requests ? total.queue / total.requests : 0);
    out.Text(",\"maxQueueMs\":"); AppendJsNumber(out, total.max_queue);
    out.Text(",\"ownerSamples\":"); AppendCount(out, total.owner_samples);
    out.Text(",\"avgOwnerMs\":");
    AppendJsNumber(out, total.owner_samples ? total.owner_time / total.owner_samples : 0);
    out.Text(",\"maxOwnerMs\":"); AppendJsNumber(out, total.max_owner_time);
    out.Byte('}');
  };
  out.Text("{\"windowMinutes\":15,\"omittedRouteSamples\":"); AppendCount(out, omitted);
  out.Text(",\"admissionSinceBoot\":{");
  for (size_t i = 1; i < sizeof(admission) / sizeof(admission[0]); ++i) {
    if (i > 1) out.Byte(',');
    AppendJsonString(out, kAdmissionNames[i], strlen(kAdmissionNames[i]));
    out.Byte(':'); AppendCount(out, admission[i]);
  }
  out.Text("},\"routes\":[");
  bool first = true;
  for (size_t i = 0; i < count; ++i) {
    for (size_t j = 0; j < method_count; ++j) {
      const auto& total = totals[i * method_count + j];
      if (!total.requests) continue;
      if (!first) out.Byte(',');
      first = false;
      out.Text("{\"route\":"); AppendJsonString(out, names[i], strlen(names[i]));
      const size_t group = group_for(names[i]);
      group_totals[group].Merge(total);
      out.Text(",\"service\":"); AppendJsonString(out, groups[group], strlen(groups[group]));
      out.Text(",\"method\":"); AppendJsonString(out, kMethods[j], strlen(kMethods[j]));
      emit_total(total);
    }
  }
  out.Text("],\"groups\":[");
  for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); ++i) {
    if (i) out.Byte(',');
    out.Text("{\"service\":"); AppendJsonString(out, groups[i], strlen(groups[i]));
    emit_total(group_totals[i]);
  }
  out.Text("],\"platforms\":[");
  for (size_t i = 0; i < sizeof(kPlatforms) / sizeof(kPlatforms[0]); ++i) {
    if (i) out.Byte(',');
    out.Text("{\"os\":"); AppendJsonString(out, kPlatforms[i], strlen(kPlatforms[i]));
    emit_total(platforms[i]);
  }
  out.Text("]}");
  return out.ok ? out.used : 0;
}

}  // namespace fortis::operations
