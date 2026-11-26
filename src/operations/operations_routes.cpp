#include "release_routes.h"
#include "error_board.h"
#include <cerrno>
#include <cstdlib>
#include "ownership.h"
#include <atomic>
#include "exception_boundary.h"
#include "operations_routes.h"
#include "ai_config.h"
#include "threading.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <dirent.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include "guest_allocator_metrics.h"
#include "guest_socket_stats.h"
#include "guest_cpu_stats.h"
#include "kv_runtime.h"
#include "device_auth.h"
#include "price_snapshot.h"
#include "api_docs_generated.h"
#include "landing_background.h"
#include "price_history_batch_codec.h"

namespace fortis::operations {

using fortis::codec::history_batch::AppendInteger;
using fortis::codec::history_batch::OutBuffer;

namespace {

constexpr char kJsonType[] = "application/json; charset=utf-8";
constexpr char kHtmlType[] = "text/html";
constexpr char kPageCache[] = "public, max-age=60";
constexpr char kImageCache[] = "public, max-age=86400";
constexpr size_t kLandingCapacity = 64 * 1024;
constexpr size_t kAiPageCapacity = 4096;

// Rendered once per process, as in TypeScript, then replayed. Never freed: the
// page outlives every response that references it.
fortis::base::Mutex landing_mutex;
std::unique_ptr<char[], base::StorageDeleter> landing_page;
size_t landing_length = 0;

// /statusz memo per (epoch second, telemetry revision), as in TypeScript; see
// handle_statusz. Guarded by status_mutex, which is held while the body is
// copied out, so a concurrent rebuild never tears a response.
fortis::base::Mutex status_mutex;
char status_stats[ApiTelemetry::kStatsCapacity];
char status_document[kStatusDocumentCapacity];
size_t status_length = 0;
int64_t status_sec = -1;
uint64_t status_revision = 0;
#if defined(FORTIS_UNIKRAFT)
uint64_t last_idle_ns = 0;
#else
clock_t last_cpu_ticks = static_cast<clock_t>(-1);
#endif
uint64_t last_cpu_monotonic_ms = 0;
double current_cpu_percent = 0;
bool has_cpu_percent = false;
uint64_t current_disk_used = 0;
FortisGuestSocketStats sampled_sockets{};
fortis::storage::KvDashboardStats sampled_database{};
int64_t database_sample_sec = -1;
std::atomic<uint64_t> sampler_wake_lag[fortis::storage::KvActivityStats::kBuckets]{};
#ifdef FORTIS_UNIKRAFT
uint32_t peak_socket_slots = 0;
#endif
uint64_t current_disk_total = 0;
bool has_disk_sample = false;
ResourceSnapshot sampled_resources;


// Some Unikraft mounted filesystems do not implement statvfs capacity. This
// bounded fallback counts allocated bytes inside the persistent data tree.
// It does not follow symlinks and never traverses more than 4096 entries.
bool data_tree_bytes(const char* directory, unsigned depth, size_t* budget,
                     uint64_t* bytes) {
  if (!directory || depth > 4 || !budget || !*budget || !bytes) return false;
  const auto close_directory = [](DIR* value) noexcept { (void)closedir(value); };
  std::unique_ptr<DIR, decltype(close_directory)> dir(opendir(directory), close_directory);
  if (!dir) return false;
  bool complete = true;
  for (;;) {
    errno = 0;
    dirent* entry = readdir(dir.get());
    if (!entry) { if (errno) complete = false; break; }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
    if (!*budget) { complete = false; break; }
    --*budget;
    char path[1536];
    const int count = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
    if (count < 0 || static_cast<size_t>(count) >= sizeof(path)) { complete = false; break; }
    struct stat info {};
    if (lstat(path, &info) != 0) { complete = false; break; }
    if (S_ISDIR(info.st_mode)) {
      if (!data_tree_bytes(path, depth + 1, budget, bytes)) { complete = false; break; }
    } else if (S_ISREG(info.st_mode)) {
      uint64_t stored = 0;
      if (info.st_blocks > 0) {
        if (static_cast<uint64_t>(info.st_blocks) > static_cast<uint64_t>(INT64_MAX) / 512) {
          complete = false; break;
        }
        stored = static_cast<uint64_t>(info.st_blocks) * 512;
      } else {
        if (info.st_size < 0) { complete = false; break; }
        stored = static_cast<uint64_t>(info.st_size);
      }
      if (stored > static_cast<uint64_t>(INT64_MAX) - *bytes) { complete = false; break; }
      *bytes += stored;
    }
  }
  return complete;
}

fortis::base::Mutex errorsz_mutex;
char errorsz_document[ApiTelemetry::kErrorszCapacity + ErrorBoard::kJsonCapacity + 32];
fortis::base::Mutex history_document_mutex;
char history_document[ApiTelemetry::kHistoryCapacity];

struct HistorySampler {
  fortis::base::ThreadHandle thread;
  fortis::base::Mutex mutex;
  fortis::base::ConditionVariable wake;
  OperationsRouteContext* context = nullptr;
  bool running = false;
  bool stop = false;
};
HistorySampler history_sampler;

int64_t wall_clock_ms() {
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) return 0;
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

int64_t floor_sec(int64_t ms) {
  return ms / 1000 - ((ms % 1000 != 0 && ms < 0) ? 1 : 0);
}

int64_t now_of(const OperationsRouteContext* context) {
  return context->clock ? context->clock->NowMs() : wall_clock_ms();
}

void html_text(OutBuffer& out, const char* value) {
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
    switch (*p) {
      case '&': out.Text("&amp;"); break;
      case '<': out.Text("&lt;"); break;
      case '>': out.Text("&gt;"); break;
      case '"': out.Text("&quot;"); break;
      case '\'': out.Text("&#39;"); break;
      default: out.Byte(*p); break;
    }
  }
}

void ai_model_row(OutBuffer& out, const char* provider, const char* model,
                  const char* state, const char* detail) {
  out.Text("<tr><td>"); html_text(out, provider);
  out.Text("</td><td><code>"); html_text(out, model);
  out.Text("</code></td><td>"); html_text(out, state);
  out.Text("</td><td>"); html_text(out, detail);
  out.Text("</td></tr>");
}

void send_internal(int client, bool head) {
  // errors_page/home handlers have no error path of their own; a failure
  // here takes the TypeScript error handler's 500 shape.
  http::HttpResponse response = http::JsonResponse(500, "Internal Server Error", "{\"error\":\"INTERNAL\"}");
  response.content_type = kJsonType;
  WriteHttpResponse(client, response, head);
}

// Resident set size where the platform publishes it (/proc/self/statm on a
// Linux host). The Unikraft guest and macOS have no such file: null there.
bool resident_set_bytes(int64_t* bytes) {
#if defined(__APPLE__)
  mach_task_basic_info info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return false;
  if (info.resident_size > static_cast<uint64_t>(INT64_MAX)) return false;
  *bytes = static_cast<int64_t>(info.resident_size);
  return true;
#else
  FILE* file = fopen("/proc/self/statm", "r");
  if (!file) return false;
  char line[128]{};
  const bool read = fgets(line, sizeof(line), file) != nullptr;
  fclose(file);
  char* end = nullptr;
  errno = 0;
  std::strtoul(line, &end, 10);
  const bool first = read && end != line && errno != ERANGE;
  const char* second = end;
  errno = 0;
  const unsigned long resident = std::strtoul(second, &end, 10);
  const bool parsed = first && end != second && errno != ERANGE;
  const long page = sysconf(_SC_PAGESIZE);
  if (!parsed || page <= 0 || resident > static_cast<uint64_t>(INT64_MAX) / static_cast<uint64_t>(page)) return false;
  *bytes = static_cast<int64_t>(resident) * page;
  return true;
#endif
}

// The sampler owns its CPU baseline. /statusz keeps a separate short-window
// baseline for its live footer; both read only OS counters, never a provider.
void* sample_history(void* opaque) {
  auto* sampler = static_cast<HistorySampler*>(opaque);
#if defined(FORTIS_UNIKRAFT)
  uint64_t last_idle = 0;
#else
  clock_t last_ticks = static_cast<clock_t>(-1);
#endif
  uint64_t last_monotonic = 0;
  uint64_t last_disk_sample = 0;
  for (;;) {
    OperationsRouteContext* context = sampler->context;
    int64_t rss = 0;
    FortisGuestAllocatorStats allocator{};
    const bool has_rss = resident_set_bytes(&rss);
    const bool has_heap = !has_rss && CollectGuestAllocatorSample(&allocator);
    const double memory_mb = static_cast<double>(has_rss ? rss : allocator.allocated_bytes) / 1048576.0;
    double cpu_percent = 0;
    bool has_cpu = false;
    const uint64_t monotonic = StatusMonotonicMs();
#if defined(FORTIS_UNIKRAFT)
    uint64_t idle = 0;
    if (FortisGuestIdleCpuNs(&idle)) {
      if (last_monotonic && monotonic > last_monotonic && idle >= last_idle) {
        const double elapsed_ns = static_cast<double>(monotonic - last_monotonic) * 1e6;
        const double idle_fraction = static_cast<double>(idle - last_idle) / elapsed_ns;
        cpu_percent = 100.0 * (1.0 - (idle_fraction < 1.0 ? idle_fraction : 1.0));
        has_cpu = true;
      }
      last_idle = idle;
    }
#else
    const clock_t ticks = clock();
    if (ticks != static_cast<clock_t>(-1)) {
      if (last_monotonic && monotonic > last_monotonic && last_ticks != static_cast<clock_t>(-1) &&
          ticks >= last_ticks) {
        cpu_percent = 100.0 * static_cast<double>(ticks - last_ticks) /
            static_cast<double>(CLOCKS_PER_SEC) /
            (static_cast<double>(monotonic - last_monotonic) / 1000.0);
        has_cpu = true;
      }
      last_ticks = ticks;
    }
#endif
    last_monotonic = monotonic;
    if (context->errors) (void)context->errors->Flush();
    ResourceSnapshot resources = CollectResourceSnapshot();
    const int64_t sample_wall = now_of(context);
    resources.sampled_at_ms = sample_wall > 0 ? static_cast<uint64_t>(sample_wall) : 0;
    const bool refresh_disk = !last_disk_sample || monotonic < last_disk_sample || monotonic - last_disk_sample >= 30000;
    uint64_t disk_total = 0, disk_used = 0;
    bool disk_ok = false;
    if (refresh_disk) {
      struct statvfs volume {};
      const bool volume_ok = context->data_directory && statvfs(context->data_directory, &volume) == 0 &&
          volume.f_blocks > 0 && volume.f_bfree <= volume.f_blocks && volume.f_bavail <= volume.f_blocks;
      const uint64_t block = volume.f_frsize ? volume.f_frsize : volume.f_bsize;
      if (volume_ok && block && volume.f_blocks <= static_cast<uint64_t>(INT64_MAX) / block) {
        disk_total = static_cast<uint64_t>(volume.f_blocks) * block;
        disk_used = static_cast<uint64_t>(volume.f_blocks - volume.f_bfree) * block;
        resources.disk_available_bytes = static_cast<uint64_t>(volume.f_bavail) * block;
        resources.disk_scope = ResourceSnapshot::DiskScope::Filesystem;
        disk_ok = true;
      } else {
        size_t budget = 4096;
        disk_ok = data_tree_bytes(context->data_directory, 0, &budget, &disk_used);
        if (disk_ok) resources.disk_scope = ResourceSnapshot::DiskScope::DataTree;
      }
      resources.disk_sampled_at_ms = resources.sampled_at_ms;
      last_disk_sample = monotonic;
    }
    double disk_mb = 0;
    bool disk_known = false;
    {
      fortis::base::LockGuard lock(status_mutex);
      if (refresh_disk) {
        current_disk_used = disk_used;
        current_disk_total = disk_total;
        has_disk_sample = disk_ok;
      } else {
        resources.disk_scope = sampled_resources.disk_scope;
        resources.disk_available_bytes = sampled_resources.disk_available_bytes;
        resources.disk_sampled_at_ms = sampled_resources.disk_sampled_at_ms;
      }
      sampled_resources = resources;
      disk_known = has_disk_sample;
      disk_mb = static_cast<double>(current_disk_used) / 1048576.0;
    }
    context->telemetry->SampleResources(now_of(context),
        has_rss || has_heap ? &memory_mb : nullptr, has_cpu ? &cpu_percent : nullptr,
        disk_known && refresh_disk ? &disk_mb : nullptr,
        resources.disk_scope == ResourceSnapshot::DiskScope::Filesystem);
    fortis::base::UniqueLock lock(sampler->mutex);
    if (sampler->stop) break;
    timespec wake{};
    clock_gettime(CLOCK_REALTIME, &wake);
    wake.tv_sec += 5;
    const uint64_t deadline_ms = StatusMonotonicMs() + 5000;
    const int wait_result = sampler->wake.WaitUntil(lock, wake);
    if (sampler->stop) break;
    if (wait_result == ETIMEDOUT) {
      // Timer lateness is a scheduler pressure indicator, not a measurement of
      // runnable delay for the individual thread waiting on the KV mutex.
      const uint64_t now = StatusMonotonicMs();
      const uint64_t lag_us = now > deadline_ms ? (now - deadline_ms) * 1000 : 0;
      size_t bucket = 0;
      while (bucket + 1 < fortis::storage::KvActivityStats::kBuckets &&
             lag_us > fortis::storage::KvActivityStats::kBoundsUs[bucket]) ++bucket;
      (sampler_wake_lag[bucket]).fetch_add(1, std::memory_order_relaxed);
    }
  }
  return nullptr;
}

void handle_home(int client, const http::HttpRequest&, bool head, void* opaque) {
  const auto* context = static_cast<const OperationsRouteContext*>(opaque);
  if (!context) { send_internal(client, head); return; }
  // On a managed host the orchestrator is the author of this address: its
  // page is handed out unchanged. The built-in page below is the fallback.
  std::string host_page;
  if (CopyHostPage("index.html", &host_page)) {
    http::HttpResponse response{200, "OK", "text/html; charset=utf-8",
                               reinterpret_cast<const uint8_t*>(host_page.data()), host_page.size()};
    response.cache_control = "no-store";
    WriteHttpResponse(client, response, head);
    return;
  }
  char page[32 * 1024];
  const size_t length = RenderNodeHome(context->services, page, sizeof(page));
  if (!length) { send_internal(client, head); return; }
  http::HttpResponse response{200, "OK", kHtmlType,
                             reinterpret_cast<const uint8_t*>(page), length};
  response.cache_control = kPageCache;
  WriteHttpResponse(client, response, head);
}

void handle_node_asset(int client, const http::HttpRequest& request, bool head, void*) {
  const auto asset = FindNodeBrandAsset(request.path);
  if (!asset.bytes) { send_internal(client, head); return; }
  http::HttpResponse response{200, "OK", asset.content_type, asset.bytes, asset.length};
  response.cache_control = kImageCache;
  response.static_body = true;
  WriteHttpResponse(client, response, head);
}

void handle_future_features(int client, const http::HttpRequest&, bool head, void*) {
  http::HttpResponse response{200, "OK", kJsonType,
      reinterpret_cast<const uint8_t*>(kNodeFutureFeaturesJson), strlen(kNodeFutureFeaturesJson)};
  response.cache_control = kPageCache;
  response.static_body = true;
  WriteHttpResponse(client, response, head);
}

void handle_dashboard(int client, const http::HttpRequest&, bool head, void* opaque) {
  const auto* context = static_cast<const OperationsRouteContext*>(opaque);
  {
    fortis::base::LockGuard lock(landing_mutex);
    if (!landing_page && context) {
      auto page = base::AllocateStorage<char[]>(kLandingCapacity);
      const size_t length = page ? RenderLandingPage(
          context->landing_counts, true, page.get(), kLandingCapacity) : 0;
      if (length) {
        landing_page = std::move(page);
        landing_length = length;
      } else {
        page.reset();
      }
    }
  }
  if (!landing_page) { send_internal(client, head); return; }
  http::HttpResponse response{200, "OK", kHtmlType,
                        reinterpret_cast<const uint8_t*>(landing_page.get()), landing_length};
  response.cache_control = kPageCache;
  response.static_body = true;
  WriteHttpResponse(client, response, head);
}

void handle_ai(int client, const http::HttpRequest&, bool head, void* opaque) {
  const auto* context = static_cast<const OperationsRouteContext*>(opaque);
  if (!context || !context->ai_config) { send_internal(client, head); return; }
  char page[kAiPageCapacity];
  const size_t length = RenderAiPage(*context->ai_config, page, sizeof(page));
  if (!length) { send_internal(client, head); return; }
  http::HttpResponse response{200, "OK", "text/html; charset=utf-8",
                        reinterpret_cast<const uint8_t*>(page), length};
  response.cache_control = "no-store";
  WriteHttpResponse(client, response, head);
}

void handle_background(int client, const http::HttpRequest&, bool head, void*) {
  size_t length = 0;
  const uint8_t* bytes = LandingBackground(&length);
  if (!bytes || !length) {
    http::HttpResponse missing{404, "Not Found", nullptr, nullptr, 0};
    missing.omit_content_type = true;
    WriteHttpResponse(client, missing, head);
    return;
  }
  http::HttpResponse response{200, "OK", "image/jpeg", bytes, length};
  response.cache_control = kImageCache;
  response.static_body = true;
  WriteHttpResponse(client, response, head);
}

// GET/HEAD /documentation and /openapi.yaml: generated, immutable, public
// reference. Both send embedded bytes; no filesystem or network access.
void handle_documentation(int client, const http::HttpRequest&, bool head, void*) {
  http::HttpResponse response{200, "OK", kHtmlType,
      reinterpret_cast<const uint8_t*>(kApiDocsHtml), kApiDocsHtmlLength};
  response.cache_control = kPageCache;
  response.static_body = true;
  WriteHttpResponse(client, response, head);
}

void handle_openapi(int client, const http::HttpRequest&, bool head, void*) {
  http::HttpResponse response{200, "OK", "application/yaml; charset=utf-8",
      reinterpret_cast<const uint8_t*>(kApiDocsYaml), kApiDocsYamlLength};
  response.cache_control = kPageCache;
  response.static_body = true;
  WriteHttpResponse(client, response, head);
}

void handle_errors_page(int client, const http::HttpRequest&, bool head, void*) {
  http::HttpResponse response{200, "OK", kHtmlType,
                        reinterpret_cast<const uint8_t*>(kErrorsPageHtml), kErrorsPageHtmlLength};
  response.cache_control = kPageCache;
  response.static_body = true;
  WriteHttpResponse(client, response, head);
}

void handle_errorsz(int client, const http::HttpRequest&, bool head, void* opaque) {
  const auto* context = static_cast<const OperationsRouteContext*>(opaque);
  if (!context || !context->telemetry) { send_internal(client, head); return; }
  fortis::base::LockGuard lock(errorsz_mutex);
  size_t length = context->telemetry->RenderErrorsz(
      now_of(context), errorsz_document, sizeof(errorsz_document));
  if (!length) { send_internal(client, head); return; }
  if (context->errors) {
    if (errorsz_document[length - 1] != '}') { send_internal(client, head); return; }
    --length;
    constexpr char field[] = ",\"operational\":";
    memcpy(errorsz_document + length, field, sizeof(field) - 1);
    length += sizeof(field) - 1;
    const auto incidents = context->errors->RenderJson(errorsz_document + length,
        sizeof(errorsz_document) - length - 1);
    if (!incidents) { send_internal(client, head); return; }
    length += incidents;
    errorsz_document[length++] = '}';
  }
  const http::HttpResponse response{200, "OK", kJsonType,
                              reinterpret_cast<const uint8_t*>(errorsz_document), length};
  WriteHttpResponse(client, response, head);
}

// GET/HEAD /status-history: public, bounded, aggregate operational history.
// Exactly 480 chronological 60-second slots are rendered from process memory.
// Each slot has created (epoch seconds), duration, nullable MB/CPU min/avg/max
// with sample count, and request totals/errors/latency. A cold process shows
// null resources until sampled; no disk, device ID, address or provider read.
// Like /statusz, the route itself is excluded from API request telemetry.
void handle_status_history(int client, const http::HttpRequest&, bool head, void* opaque) {
  const auto* context = static_cast<const OperationsRouteContext*>(opaque);
  if (!context || !context->telemetry) { send_internal(client, head); return; }
  fortis::base::LockGuard lock(history_document_mutex);
  const size_t length = context->telemetry->RenderHistory(
      now_of(context), history_document, sizeof(history_document));
  if (!length) { send_internal(client, head); return; }
  const http::HttpResponse response{200, "OK", kJsonType,
      reinterpret_cast<const uint8_t*>(history_document), length};
  WriteHttpResponse(client, response, head);
}


void handle_benchmark_page(int client, const http::HttpRequest&, bool head, void*) {
  http::HttpResponse response{200, "OK", kHtmlType,
      reinterpret_cast<const uint8_t*>(kBenchmarkPageHtml), kBenchmarkPageHtmlLength};
  response.static_body = true;
  WriteHttpResponse(client, response, head);
}

void handle_request_stats_page(int client, const http::HttpRequest& request, bool head, void* opaque) {
  handle_dashboard(client, request, head, opaque);
}

void handle_request_stats(int client, const http::HttpRequest&, bool head, void* opaque) {
  const auto* context = static_cast<const OperationsRouteContext*>(opaque);
  if (!context || !context->telemetry) { send_internal(client, head); return; }
  constexpr size_t capacity = ApiTelemetry::kRequestStatsCapacity;
  auto body_storage = base::AllocateStorage<char[]>(capacity);
  auto* body = body_storage.get();
  if (!body) { send_internal(client, head); return; }
  const size_t length = context->telemetry->RenderRequestStats(now_of(context), body, capacity);
  if (length) {
    http::WriteHttpResponse(client, {200, "OK", kJsonType,
                      reinterpret_cast<const uint8_t*>(body), length}, head);
  } else {
    send_internal(client, head);
  }
  body_storage.reset();
}

// Public GET/HEAD /statusz: additive native extensions around the established
// wallet health-probe contract. ok=true means the node answered this request,
// not that all providers or assets are available. Query/Authorization/Origin
// do not change this public aggregate response. These reads are excluded from
// API telemetry, but remain visible in the protected /metrics route counters.
//
// RSS is resident process memory: procfs on Linux, task_info on macOS, unknown
// on the guest. heapUsed is tracked guest allocator memory, not V8 or total
// guest RAM. Native CPU is process time / elapsed time, with 100% per logical
// core; guest CPU comes from scheduler idle time. The first CPU interval is
// unknown. Same-second calls reuse that interval. Current CPU intervals and
// the separately sampled per-minute history need not match.
//
// The background sampler publishes procfs/current-cgroup details every five
// seconds and filesystem/data-tree usage every thirty seconds. Their own
// timestamps/scopes are disclosed in resources; unavailable is null. Cgroup
// values describe the local group, not necessarily an effective ancestor limit.
// No host hardware, interface, credential or filesystem path is published.
//
// stats retains the legacy 5/10-minute windows, top-five route templates and
// fifteen minute buckets. Requests include refusals; errors count 5xx. Client
// counts are bounded device-hash unions, not human user counts. /statsz exposes
// the detailed 15-minute method/group histograms and 4xx/429 counters. History
// uses 480 process-local minute slots; missing observation is distinct from idle.
//
// Memoized under status_mutex by (epoch second, telemetry revision). Readers
// copy the body while locked. Resource collection and bounded filesystem scans
// run outside HTTP workers/locks. Here only memory/CPU OS counters and in-memory
// KV aggregates (try-lock) are read; no provider or KV file/network I/O occurs.
// Fixed output buffers fail closed on capacity exhaustion, never partial JSON.
// Existing core fields, native runtime labels, HEAD and error policy are kept.

void handle_statusz(int client, const http::HttpRequest&, bool head, void* opaque) {
  const auto* context = static_cast<const OperationsRouteContext*>(opaque);
  if (!context || !context->telemetry) { send_internal(client, head); return; }
  const int64_t now = now_of(context);
  const int64_t sec = floor_sec(now);
  fortis::base::LockGuard lock(status_mutex);
  const uint64_t revision = context->telemetry->revision();
  if (!status_length || sec != status_sec || revision != status_revision) {
    // The page polls every 5 s; revisions within one second reuse the same
    // sample. The Unikraft clock() is unavailable (its process CPU clock is
    // unsupported), so the guest measures scheduler busy time from idle time.
    if (sec != status_sec) {
      if (auto* kv = fortis::storage::TryRuntimeKvStoreIfOpen()) {
        if (kv->TryDashboardStats(&sampled_database)) database_sample_sec = sec;
      }
#ifdef FORTIS_UNIKRAFT
      fortis_guest_socket_stats(&sampled_sockets);
      if (sampled_sockets.used > peak_socket_slots) peak_socket_slots = sampled_sockets.used;
      sampled_sockets.sampled_peak = peak_socket_slots;
#endif
      const uint64_t sampled_at = StatusMonotonicMs();
#if defined(FORTIS_UNIKRAFT)
      uint64_t idle_ns = 0;
      const bool has_idle = FortisGuestIdleCpuNs(&idle_ns);
      if (has_idle && last_cpu_monotonic_ms && sampled_at > last_cpu_monotonic_ms &&
          idle_ns >= last_idle_ns) {
        const double elapsed_ns = static_cast<double>(sampled_at - last_cpu_monotonic_ms) * 1e6;
        const double idle_delta = static_cast<double>(idle_ns - last_idle_ns);
        const double idle_fraction = idle_delta / elapsed_ns;
        current_cpu_percent = 100.0 * (1.0 - (idle_fraction < 1.0 ? idle_fraction : 1.0));
        has_cpu_percent = true;
      }
      last_idle_ns = idle_ns;
#else
      const clock_t ticks = clock();
      if (ticks != static_cast<clock_t>(-1) &&
          last_cpu_ticks != static_cast<clock_t>(-1) &&
          sampled_at > last_cpu_monotonic_ms && ticks >= last_cpu_ticks) {
        current_cpu_percent = 100.0 * static_cast<double>(ticks - last_cpu_ticks) /
            static_cast<double>(CLOCKS_PER_SEC) /
            (static_cast<double>(sampled_at - last_cpu_monotonic_ms) / 1000.0);
        has_cpu_percent = true;
      }
      last_cpu_ticks = ticks;
#endif
      last_cpu_monotonic_ms = sampled_at;

    }
    const size_t stats_length = context->telemetry->RenderStatusStats(
        now, status_stats, sizeof(status_stats));
    int64_t rss = 0, heap_used = 0;
    const bool has_rss = resident_set_bytes(&rss);
    FortisGuestAllocatorStats allocator{};
    const bool has_heap = CollectGuestAllocatorSample(&allocator);
    if (has_heap) heap_used = allocator.allocated_bytes;
    const uint64_t monotonic = StatusMonotonicMs();
    const uint64_t uptime_ms = monotonic > context->started_monotonic_ms
        ? monotonic - context->started_monotonic_ms : 0;
    fortis::storage::KvActivityStats activity{};
    uint64_t wake_lag[fortis::storage::KvActivityStats::kBuckets];
    for (size_t i = 0; i < fortis::storage::KvActivityStats::kBuckets; ++i)
      wake_lag[i] = (sampler_wake_lag[i]).load(std::memory_order_relaxed);
    const auto* kv = fortis::storage::TryRuntimeKvStoreIfOpen();
    if (kv) kv->ActivityStats(activity);
    const auto snapshot = context->sample_price_snapshot
        ? context->sample_price_snapshot(context->price_snapshot_context, now)
        : prices::PriceSnapshotStatus{};
    uint64_t price_memory_hits = 0, contract_memory_hits = 0;
    if (context->sample_price_memory_hits)
      context->sample_price_memory_hits(context->price_cache_context, price_memory_hits, contract_memory_hits);
    status_length = stats_length ? RenderStatusDocument(
        uptime_ms / 1000, has_rss ? &rss : nullptr, has_heap ? &heap_used : nullptr,
        has_cpu_percent ? &current_cpu_percent : nullptr,
        has_disk_sample ? &current_disk_used : nullptr,
        has_disk_sample && current_disk_total ? &current_disk_total : nullptr,
        status_stats, stats_length, status_document, sizeof(status_document),
        sampled_sockets.capacity ? &sampled_sockets : nullptr,
        database_sample_sec >= 0 && database_sample_sec <= sec ? &sampled_database : nullptr,
        database_sample_sec >= 0 && database_sample_sec <= sec ? sec - database_sample_sec : 0,
        kv ? &activity : nullptr, uptime_ms, wake_lag, true, context->sample_price_snapshot ? &snapshot : nullptr, kv, context->price_store,
        price_memory_hits, contract_memory_hits, &sampled_resources) : 0;
    status_sec = sec;
    status_revision = revision;
  }
  if (!status_length) { send_internal(client, head); return; }
  const http::HttpResponse response{200, "OK", kJsonType,
                              reinterpret_cast<const uint8_t*>(status_document), status_length};
  WriteHttpResponse(client, response, head);
}

void append_text(char* output, size_t capacity, size_t* used, const char* text,
                 size_t length, bool* ok) {
  if (!*ok || length > capacity - *used) { *ok = false; return; }
  memcpy(output + *used, text, length);
  *used += length;
}

}  // namespace

uint64_t StatusMonotonicMs() {
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return static_cast<uint64_t>(now.tv_sec) * 1000ULL +
         static_cast<uint64_t>(now.tv_nsec / 1000000L);
}

#if defined(FORTIS_UNIKRAFT)
const char kStatusRuntime[] = "native-cpp-unikraft";
#else
const char kStatusRuntime[] = "native-cpp";
#endif

LandingCounts LandingCountsFromCapabilities(const networks::NativeNetworkRegistry& registry,
    const char* const (*values)[networks::kNetworkCapabilityCount], size_t count) {
  LandingCounts counts{};
  size_t available = 0;
  const networks::NativeNetworkConfig* networks = registry.list(&available);
  if (!networks || !values || count != available) return counts;
  auto capability = [](const char* name) {
    for (size_t i = 0; i < networks::kNetworkCapabilityCount; ++i)
      if (strcmp(networks::kNetworkCapabilityNames[i], name) == 0) return static_cast<int>(i);
    return -1;
  };
  struct Tally { const char* name; uint32_t* total; };
  const Tally tallies[] = {{"balances", &counts.balances}, {"transfers", &counts.transfers},
      {"nfts", &counts.nfts}, {"insight", &counts.insight}, {"txStatus", &counts.tx_status},
      {"token", &counts.token}, {"trustlines", &counts.trustlines},
      // TypeScript mirrors the /v1/networks txDetail override; the native
      // capability document already carries the served value.
      {"txDetail", &counts.tx_detail}};
  counts.networks = static_cast<uint32_t>(count);
  for (size_t i = 0; i < count; ++i) {
    bool seen = false;
    for (size_t j = 0; j < i && !seen; ++j) seen = strcmp(networks[i].family, networks[j].family) == 0;
    if (!seen) ++counts.families;
    for (const Tally& tally : tallies) {
      const int index = capability(tally.name);
      if (index >= 0 && values[i][index] && strcmp(values[i][index], "none") != 0) ++*tally.total;
    }
  }
  return counts;
}

size_t RenderAiPage(const ai::AiServiceConfig& config, char* output, size_t capacity) {
  if (!output || !capacity) return 0;
  OutBuffer out{reinterpret_cast<uint8_t*>(output), capacity};
  out.Text(R"FAI(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>AI models | FortisNet</title><style>body{font:16px system-ui,sans-serif;background:#0b1020;color:#e9edf7;margin:0;padding:3rem 1rem}main{max-width:920px;margin:auto}a{color:#8db5ff}h1{font-size:2rem}p{color:#b8c2d7}table{width:100%;border-collapse:collapse;background:#151d32;border-radius:12px;overflow:hidden}th,td{text-align:left;padding:1rem;border-bottom:1px solid #303a52}th{color:#b8c2d7}code{overflow-wrap:anywhere}footer{margin-top:2rem;color:#b8c2d7}@media(max-width:650px){table{display:block;overflow-x:auto}}</style></head><body><main><a href="/">&larr; FortisNet Node</a><h1>AI models</h1><p>Models configured on this Intelligence server. Availability also depends on the provider and conversation service.</p>)FAI");
  if (config.ai_model_configured || config.openai_model_configured) {
    out.Text("<table><thead><tr><th>Provider</th><th>Model ID</th><th>Configuration</th><th>Mode</th></tr></thead><tbody>");
    if (config.ai_model_configured) {
      const bool ready = config.native_ai_canary && config.ai_provider_configured &&
          strcmp(config.ai_provider, "anthropic") == 0 && config.anthropic_api_key_path_configured;
      ai_model_row(out, "Anthropic", config.ai_model, ready ? "Configured" : "Incomplete",
                   "Async wallet conversations");
    }
    if (config.openai_model_configured) {
      ai_model_row(out, "OpenAI", config.openai_model,
                   config.native_ai_canary && config.openai_api_key_configured ? "Configured" : "Incomplete",
                   "Legacy reply path");
    }
    out.Text("</tbody></table>");
  } else {
    out.Text("<p>No AI models are configured on this server.</p>");
  }
  out.Text("<footer><a href=\"/ai/stats\">View AI statistics</a></footer></main></body></html>");
  return out.ok ? out.used : 0;
}

size_t RenderNodeHome(const NodeServices& services, char* output, size_t capacity) {
  if (!output || !capacity) return 0;
  size_t used = 0;
  bool ok = true;
  const auto append = [&](const char* text) {
    append_text(output, capacity, &used, text, strlen(text), &ok);
  };
  append(kNodeHomeBeforeServices);
  struct Service {
    bool enabled;
    const char* name;
    const char* description;
    const char* link;
  };
  const Service entries[] = {
      {services.prices, "Price discovery", "Prices, history, market candles and token discovery.", "/doc"},
      {services.news, "News &amp; media", "Publisher feeds, articles and reactions.", "/doc"},
      {services.ai, "AI models", "Model-backed conversations with asynchronous replies.", "/ai"},
      {true, "Network catalog", "Supported networks and their declared capabilities.", "/networks"},
      {services.addresses, "Wallet &amp; chain data", "Balances, transfers, NFTs and transaction details.", "/doc"},
      {services.cross_chain, "Cross-chain quotes", "Compare routes from configured swap providers.", "/doc"},
      {services.stocks, "Tokenized stocks", "Configured stock and instrument interfaces.", "/doc"},
      {services.payments, "Dash payments", "Payment resolution and proof interfaces.", "/doc"},
  };
  for (const auto& entry : entries) {
    append(entry.enabled ? R"(<li class="on">)" : R"(<li class="off">)");
    append(R"(<span class="check" aria-hidden="true">)");
    append(entry.enabled ? "✓" : "—");
    append(R"(</span><div><strong><a href=")");
    append(entry.link);
    append(R"(">)");
    append(entry.name);
    append("</a></strong><p>");
    append(entry.description);
    append(R"(</p></div><span class="state">)");
    append(entry.enabled ? "Enabled" : "Not enabled");
    append("</span></li>");
  }
  append(kNodeHomeAfterServices);
  return ok ? used : 0;
}

size_t RenderLandingPage(const LandingCounts& counts, bool docs_enabled, char* output,
                         size_t capacity) {
  if (!output || !capacity) return 0;
  size_t used = 0;
  bool ok = true;
  for (size_t i = 0; i < kLandingSegmentCount; ++i) {
    const LandingSegment& segment = kLandingSegments[i];
    uint32_t value = 0;
    switch (segment.slot) {
      case LandingSlot::kText:
        append_text(output, capacity, &used, segment.text, segment.length, &ok);
        continue;
      case LandingSlot::kDocsLink: {
        const char* link = docs_enabled ? kLandingDocsEnabled : kLandingDocsDisabled;
        append_text(output, capacity, &used, link, strlen(link), &ok);
        continue;
      }
      case LandingSlot::kNetworks: value = counts.networks; break;
      case LandingSlot::kFamilies: value = counts.families; break;
      case LandingSlot::kBalances: value = counts.balances; break;
      case LandingSlot::kTransfers: value = counts.transfers; break;
      case LandingSlot::kNfts: value = counts.nfts; break;
      case LandingSlot::kInsight: value = counts.insight; break;
      case LandingSlot::kTxStatus: value = counts.tx_status; break;
      case LandingSlot::kToken: value = counts.token; break;
      case LandingSlot::kTrustlines: value = counts.trustlines; break;
      case LandingSlot::kTxDetail: value = counts.tx_detail; break;
    }
    char digits[16];
    const int length = snprintf(digits, sizeof(digits), "%u", static_cast<unsigned>(value));
    append_text(output, capacity, &used, digits, static_cast<size_t>(length), &ok);
  }
  return ok ? used : 0;
}

size_t RenderStatusDocument(uint64_t uptime_sec, const int64_t* rss, const int64_t* heap_used,
                            const double* cpu_percent, const uint64_t* disk_used,
                            const uint64_t* disk_total,
                            const char* stats, size_t stats_length, char* output,
                            size_t capacity, const FortisGuestSocketStats* sockets,
                            const fortis::storage::KvDashboardStats* database, uint64_t database_age_sec,
                            const fortis::storage::KvActivityStats* activity, uint64_t activity_at_ms,
                            const uint64_t* wake_lag, bool include_phase_one,
                            const prices::PriceSnapshotStatus* snapshot, const fortis::storage::KvStore* store,
                            const fortis::storage::KvStore* price_store, uint64_t memory_hits, uint64_t contract_hits, const ResourceSnapshot* resources) {
  if (!stats || !stats_length || !output) return 0;
  OutBuffer out{reinterpret_cast<uint8_t*>(output), capacity};
  // The TypeScript keys, in order. `node` carries process.version there;
  // this service has no Node.js runtime, so it names the native one instead
  // (kStatusRuntime), never a Node.js version. `coordinator` and
  // `tokenRefresh` describe TypeScript subsystems with no native counterpart
  // yet and are omitted; OpenAPI does not require them and the app does not
  // read them.
  out.Text("{\"ok\":true,\"version\":\"");
  out.Text(kOperationsPackageVersion);
  out.Text("\",\"node\":\"");
  out.Text(kStatusRuntime);
  out.Text("\",\"uptimeSec\":");
  AppendInteger(out, static_cast<int64_t>(uptime_sec));
  out.Text(",\"memory\":{\"rss\":");
  if (rss) AppendInteger(out, *rss); else out.Text("null");
  // heapUsed: bytes currently allocated from the Unikraft default allocator
  // (allocations it tracks, not every library heap or thread stack).
  out.Text(",\"heapUsed\":");
  if (heap_used) AppendInteger(out, *heap_used); else out.Text("null");
  out.Text("},\"cpu\":{\"percent\":");
  if (cpu_percent) {
    char value[32];
    const int size = snprintf(value, sizeof(value), "%.2f", *cpu_percent);
    if (size < 0 || static_cast<size_t>(size) >= sizeof(value)) return 0;
    out.Append(value, static_cast<size_t>(size));
  } else out.Text("null");
  out.Text("},\"disk\":");
  if (disk_used) {
    out.Text("{\"used\":");
    AppendInteger(out, static_cast<int64_t>(*disk_used));
    out.Text(",\"total\":");
    if (disk_total) AppendInteger(out, static_cast<int64_t>(*disk_total));
    else out.Text("null");
    out.Byte('}');
  } else out.Text("null");
  if (sockets) {
    out.Text(",\"sockets\":{\"limit\":");
    AppendInteger(out, sockets->capacity);
    out.Text(",\"used\":");
    AppendInteger(out, sockets->used);
    out.Text(",\"slotBytes\":");
    AppendInteger(out, sockets->slot_bytes);
    out.Text(",\"tableBytes\":");
    AppendInteger(out, static_cast<int64_t>(sockets->capacity) * sockets->slot_bytes);
    out.Text(",\"sampledPeak\":");
    AppendInteger(out, sockets->sampled_peak);
    out.Byte('}');
  }
  if (database) {
    const auto& c = database->compaction;
    const char* phase = "idle";
    switch (c.phase) {
      case fortis::storage::KvCompactionPhase::Idle: break;
      case fortis::storage::KvCompactionPhase::Copy: phase = "copying"; break;
      case fortis::storage::KvCompactionPhase::CatchUp: phase = "catching up"; break;
      case fortis::storage::KvCompactionPhase::Remap: phase = "remapping"; break;
    }
    const auto field = [&](const char* name, uint64_t value) {
      out.Text(name);
      AppendInteger(out, static_cast<int64_t>(value));
    };
    out.Text(",\"database\":{");
    field("\"indexedKeys\":", database->indexed_keys);
    field(",\"fileBytes\":", c.file_bytes);
    field(",\"liveBytes\":", c.live_bytes);
    field(",\"compactionsCompleted\":", c.runs_completed);
    field(",\"compactionsFailed\":", c.runs_failed);
    field(",\"maxLockHoldUs\":", c.lock_hold_us_max);
    field(",\"sampleAgeSec\":", database_age_sec);
    out.Text(",\"compactionPhase\":\"");
    out.Text(phase);
    out.Text("\",\"recoveryPending\":");
    out.Text(database->recovery_pending ? "true}" : "false}");
  }
  if (activity) {
    const auto histogram = [&](const char* key, const uint64_t* buckets) {
      out.Text(key);
      for (size_t i = 0; i < fortis::storage::KvActivityStats::kBuckets; ++i) {
        if (i) out.Byte(',');
        AppendInteger(out, static_cast<int64_t>(buckets[i]));
      }
      out.Byte(']');
    };
    out.Text(",\"databaseActivity\":{\"sampleMs\":");
    AppendInteger(out, static_cast<int64_t>(activity_at_ms));
    out.Text(",\"lockWaiters\":");
    AppendInteger(out, activity->lock_waiters);
    out.Text(",\"peakLockWaiters\":");
    AppendInteger(out, activity->peak_lock_waiters);
    out.Text(",\"boundsUs\":[");
    for (size_t i = 0; i + 1 < fortis::storage::KvActivityStats::kBuckets; ++i) {
      if (i) out.Byte(',');
      AppendInteger(out, static_cast<int64_t>(fortis::storage::KvActivityStats::kBoundsUs[i]));
    }
    out.Byte(']');
    histogram(",\"reads\":[", activity->reads);
    histogram(",\"writes\":[", activity->writes);
    if (store) {
      out.Text(",\"groupCommit\":");
      out.Text(store->GroupCommitEnabled() ? "true" : "false");
      out.Text(",\"writeFrames\":");
      AppendInteger(out, static_cast<int64_t>(store->WriteFrames()));
    }
    histogram(",\"lockWait\":[", activity->lock_wait);
    static constexpr const char* phase_keys[] = {
        ",\"readHold\":[", ",\"writeHold\":[", ",\"sweepHold\":[", ",\"compactionHold\":[",
        ",\"recoveryHold\":[", ",\"fileRead\":[", ",\"fileSync\":[", ",\"scanHold\":["};
    static_assert(sizeof(phase_keys) / sizeof(phase_keys[0]) == fortis::storage::KvActivityStats::kPhases);
    for (size_t i = 0; i < fortis::storage::KvActivityStats::kPhases; ++i)
      histogram(phase_keys[i], activity->phases[i]);
    if (wake_lag) histogram(",\"samplerWakeLag\":[", wake_lag);
    out.Byte('}');
  }
  if (include_phase_one) {
    const auto timings = http::ReadHttpQueueTimings();
    const auto auth = fortis::auth::ReadSnapshotAuthStats();
    const auto array = [&](const uint64_t* values, size_t count) {
      for (size_t i = 0; i < count; ++i) {
        if (i) out.Byte(',');
        AppendInteger(out, static_cast<int64_t>(values[i]));
      }
      out.Byte(']');
    };
    constexpr const char* keys[] = {
        ",\"dispatch\":[", ",\"readLockWait\":[", ",\"readLockHold\":[",
        ",\"mutationLockWait\":[", ",\"mutationLockHold\":["};
    static_assert(sizeof(keys) / sizeof(keys[0]) == http::HttpQueueTimings::Count);
    out.Text(",\"httpTiming\":{\"boundsUs\":[");
    array(http::HttpQueueTimings::kBoundsUs, http::HttpQueueTimings::kBuckets - 1);
    for (size_t i = 0; i < http::HttpQueueTimings::Count; ++i) {
      out.Text(keys[i]);
      array(timings.buckets[i], http::HttpQueueTimings::kBuckets);
    }
    out.Text("},\"snapshotAuth\":{\"hits\":");
    AppendInteger(out, static_cast<int64_t>(auth.hits));
    out.Text(",\"misses\":");
    AppendInteger(out, static_cast<int64_t>(auth.misses));
    out.Text(",\"invalidations\":");
    AppendInteger(out, static_cast<int64_t>(auth.invalidations));
    out.Byte('}');
  }
  if (snapshot) {
    out.Text(",\"priceSnapshot\":{\"ready\":");
    out.Text(snapshot->ready ? "true" : "false");
    out.Text(",\"sequence\":");
    AppendInteger(out, static_cast<int64_t>(snapshot->sequence));
    out.Text(",\"ageMs\":");
    AppendInteger(out, snapshot->age_ms);
    out.Text(",\"unavailable\":");
    AppendInteger(out, static_cast<int64_t>(snapshot->unavailable));
    out.Byte('}');
  }
  if (price_store) {
    fortis::storage::KvActivityStats activity{};
    price_store->ActivityStats(activity);
    uint64_t syncs = 0;
    for (uint64_t count : activity.phases[static_cast<size_t>(fortis::storage::KvActivityStats::Phase::FileSync)])
      syncs += count;
    out.Text(",\"priceStore\":{\"memoryHits\":");
    AppendInteger(out, static_cast<int64_t>(memory_hits));
    out.Text(",\"contractMemoryHits\":");
    AppendInteger(out, static_cast<int64_t>(contract_hits));
    out.Text(",\"writeFrames\":");
    AppendInteger(out, static_cast<int64_t>(price_store->WriteFrames()));
    out.Text(",\"fileSyncs\":");
    AppendInteger(out, static_cast<int64_t>(syncs));
    out.Byte('}');
  }
  if (resources) AppendResourceSnapshot(out, *resources);
  out.Text(",\"stats\":");
  out.Append(stats, stats_length);
  out.Byte('}');
  return out.ok ? out.used : 0;
}

bool RegisterOperationsRoutes(http::HttpRouter& router, OperationsRouteContext* context) {
  if (!context || !context->telemetry) return false;
  if (!context->started_monotonic_ms) context->started_monotonic_ms = StatusMonotonicMs();
  // GET registrations answer HEAD through the router, as Fastify's
  // exposeHeadRoutes does for these root-scoped routes.
  return router.Add("GET", "/", handle_home, context) &&
         router.Add("GET", "/dashboard", handle_dashboard, context) &&
         router.Add("GET", "/node-features.json", handle_future_features, context) &&
         router.Add("GET", "/assets/node-hall.webp", handle_node_asset, context) &&
         router.Add("GET", "/assets/fortis-favicon.svg", handle_node_asset, context) &&
         router.Add("GET", "/assets/fortisnet-lockup.svg", handle_node_asset, context) &&
         router.Add("GET", "/assets/social-preview.jpg", handle_node_asset, context) &&
         router.Add("GET", "/ai", handle_ai, context) &&
         router.Add("GET", "/doc", handle_documentation, context) &&
         router.Add("GET", "/documentation", handle_documentation, context) &&
         router.Add("GET", "/openapi.yaml", handle_openapi, context) &&
         router.Add("GET", "/assets/background.jpg", handle_background, context) &&
         router.Add("GET", "/benchmark", handle_benchmark_page, context) &&
         router.Add("GET", "/stats", handle_request_stats_page, context) &&
         router.Add("GET", "/statsz", handle_request_stats, context) &&
         router.Add("GET", "/statusz", handle_statusz, context) &&
         router.Add("GET", "/status-history", handle_status_history, context) &&
         router.Add("GET", "/errors", handle_errors_page, context) &&
         router.Add("GET", "/error", handle_errors_page, context) &&
         router.Add("GET", "/errorsz", handle_errorsz, context) &&
         (!context->errors || context->errors->RegisterMissingIdentifiers(
             router, context->error_reports_attestation_required));
}

bool StartOperationsHistorySampler(OperationsRouteContext* context) {
  if (!context || !context->telemetry) return false;
  fortis::base::LockGuard lock(history_sampler.mutex);
  if (history_sampler.running) return false;
  history_sampler.context = context;
  history_sampler.stop = false;
  if ((history_sampler.thread.StartEntry<sample_history>(&history_sampler, nullptr) ? 0 : -1) != 0) {
    history_sampler.context = nullptr;
    return false;
  }
  history_sampler.running = true;
  return true;
}

void StopOperationsHistorySampler() {
  fortis::base::UniqueLock history_sampler_lock(history_sampler.mutex);
  if (!history_sampler.running) return;
  history_sampler.stop = true;
  history_sampler.wake.Signal();
  history_sampler_lock.Unlock();
  history_sampler.thread.Join();
  fortis::base::LockGuard lock(history_sampler.mutex);
  history_sampler.running = false;
  history_sampler.context = nullptr;
}

}  // namespace fortis::operations
