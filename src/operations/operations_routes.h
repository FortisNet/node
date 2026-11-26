#pragma once

#include <stddef.h>
#include <stdint.h>

#include "api_telemetry.h"
#include "clock.h"
#include "http_server.h"
#include "network_registry.h"
#include "operations_pages.h"
#include "resource_snapshot.h"

struct FortisGuestSocketStats;  // guest_socket_stats.h (C, global)
namespace fortis::ai { struct AiServiceConfig; }
namespace fortis::prices { struct PriceSnapshotStatus; }
namespace fortis::storage { class KvStore; struct KvDashboardStats; struct KvActivityStats; }

namespace fortis::operations {
class ErrorBoard;

// Public, root-scoped operations routes (HTML in operations_pages.cpp):
// GET /, /dashboard, /node-features.json, /ai, /assets/*, /doc, /documentation, /openapi.yaml,
// /statusz, /status-history, /stats, /statsz, /benchmark, /errors and /errorsz.
// GET routes also answer HEAD.
// No bearer, no quota, no Origin refusal,
// no KV, database or provider reads while rendering operational pages — each is a replay of a page
// rendered once, embedded bytes, or an in-memory aggregate memoised per
// (second, telemetry revision). /statusz additionally reads /proc/self/statm
// where it exists; its full contract is documented at handle_statusz.
struct OperationsRouteContext {
  ApiTelemetry* telemetry = nullptr;
  // The model inventory GET /ai renders.
  const ai::AiServiceConfig* ai_config = nullptr;
  LandingCounts landing_counts{};
  NodeServices services{};
  // Wall clock (NowMs in epoch milliseconds); tests may substitute their own.
  fortis::base::Clock* clock = nullptr;
  // StatusMonotonicMs() at process start, for uptimeSec. main() sets it on
  // entry; RegisterOperationsRoutes fills it in if it is still zero.
  uint64_t started_monotonic_ms = 0;
  // Filesystem containing the persistent KV store; /data by default.
  const char* data_directory = "/data";
  prices::PriceSnapshotStatus (*sample_price_snapshot)(void*, int64_t) = nullptr;
  void* price_snapshot_context = nullptr;
  const fortis::storage::KvStore* price_store = nullptr;
  void (*sample_price_memory_hits)(void*, uint64_t&, uint64_t&) = nullptr;
  void* price_cache_context = nullptr;
  ErrorBoard* errors = nullptr;
  bool error_reports_attestation_required = false;
};

// CLOCK_MONOTONIC in milliseconds (0 if the clock cannot be read).
uint64_t StatusMonotonicMs();

// Count the same startup capability values published by /v1/networks.
LandingCounts LandingCountsFromCapabilities(const networks::NativeNetworkRegistry& registry,
    const char* const (*values)[networks::kNetworkCapabilityCount], size_t count);

// The /statusz `node` value: the native runtime label, distinct for the host
// build and the Unikraft guest. Never a Node.js version.
extern const char kStatusRuntime[];

// The /statusz document buffer: the stats block plus bounded resource fields.
constexpr size_t kStatusDocumentCapacity = ApiTelemetry::kStatsCapacity + 8192;

// Renders the /statusz document. `stats` is ApiTelemetry::RenderStatusStats
// output, appended verbatim. Optional resource pointers render as JSON null
// when unavailable. CPU percent is relative to one logical core; disk bytes
// describe the filesystem containing the KV store. Returns the length, or 0 when
// stats is empty or the document does not fit `capacity` (never partial).
size_t RenderStatusDocument(uint64_t uptime_sec, const int64_t* rss, const int64_t* heap_used,
                            const double* cpu_percent, const uint64_t* disk_used,
                            const uint64_t* disk_total,
                            const char* stats, size_t stats_length, char* output,
                            size_t capacity, const struct FortisGuestSocketStats* sockets = nullptr,
                            const fortis::storage::KvDashboardStats* database = nullptr,
                            uint64_t database_age_sec = 0,
                            const fortis::storage::KvActivityStats* activity = nullptr,
                            uint64_t activity_at_ms = 0,
                            const uint64_t* sampler_wake_lag = nullptr,
                            bool include_phase_one = false,
                            const prices::PriceSnapshotStatus* price_snapshot = nullptr,
                            const fortis::storage::KvStore* store = nullptr,
                            const fortis::storage::KvStore* price_store = nullptr, uint64_t memory_hits = 0, uint64_t contract_hits = 0,
                            const ResourceSnapshot* resources = nullptr);

// Public model inventory. Only model IDs and non-secret settings are rendered.
size_t RenderAiPage(const ai::AiServiceConfig& config, char* output, size_t capacity);

bool RegisterOperationsRoutes(http::HttpRouter& router, OperationsRouteContext* context);

// Samples process resources every five seconds into ApiTelemetry's 480
// one-minute slots, including when no browser is open. Start after route
// registration and stop before destroying the context or telemetry. Lifecycle
// calls are serialized by the application owner; do not call Stop concurrently
// or from the sampler thread. Stop wakes and joins the borrowed-context worker.
bool StartOperationsHistorySampler(OperationsRouteContext* context);
void StopOperationsHistorySampler();

}  // namespace fortis::operations
