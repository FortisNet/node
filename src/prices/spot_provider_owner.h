#include <memory>
#include <string_view>
#pragma once
#include "error_reporter.h"

#include "status.h"
#include "seams.h"

#include <variant>
#include <stdlib.h>

#include "provider_runtime_config.h"
#include "ownership.h"
#include "spot_resolution.h"
#include "spot_response_serializer.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::base { class Clock; }
namespace fortis::prices {
struct SpotRuntime;
class SpotRequestProcessor; class PriceSpotCache; class PriceSeriesStore; }
namespace fortis::discovery { class CoinPopularity; }
namespace fortis::providers { class ProviderExecutionCoordinator; struct RobinhoodAssetCatalog; }

namespace fortis::prices {

// A Robinhood issuer catalog the owner shares instead of keeping its own
// (TypeScript's PricesFacet and Token360 share one RobinhoodStockTokenClient):
// `fresh` answers the shared catalog while it is unexpired; `adopt` parses a
// `/rhj/assets` body the owner fetched into the shared catalog. Both run on
// the owner thread and must not block on provider work.
struct SpotRobinhoodCatalog {
  bool (*fresh)(void* context, providers::RobinhoodAssetCatalog* out) = nullptr;
  bool (*adopt)(void* context, const uint8_t* body, size_t length, providers::RobinhoodAssetCatalog* out) = nullptr;
  void* context = nullptr;
};

constexpr size_t kSpotProviderOwnerMaxRequests = 8;
constexpr uint32_t kSpotProviderOwnerMaxDeadlineMs = 30000;

enum class SpotProviderOwnerFormat : uint8_t { kJson, kProtobuf };
enum class SpotSubmitMode : uint8_t { kForeground, kBackground };
enum class SpotProviderOwnerSubmitStatus : uint8_t {
  kAccepted, kBusy, kInvalidRequest, kNotRunning, kStopping
};
struct SpotSubmitTicket { uint64_t request_id; };

// Exactly one accepted ticket or rejection. A rejected submission has no ID
// to cancel or accidentally reuse; accepted IDs are always nonzero.
class [[nodiscard]] SpotProviderOwnerSubmission {
 public:
  const SpotSubmitTicket* ticket() const noexcept { return std::get_if<SpotSubmitTicket>(&value_); }
  SpotProviderOwnerSubmitStatus status() const noexcept {
    const auto* rejected = std::get_if<SpotProviderOwnerSubmitStatus>(&value_);
    return rejected ? *rejected : SpotProviderOwnerSubmitStatus::kAccepted;
  }

 private:
  friend class SpotProviderOwner;
  friend class SpotRequestProcessor;
  SpotProviderOwnerSubmission(SpotProviderOwnerSubmitStatus rejected) : value_(rejected) {
    if (rejected == SpotProviderOwnerSubmitStatus::kAccepted) abort();
  }
  SpotProviderOwnerSubmission(SpotSubmitTicket accepted) : value_(accepted) {
    if (!accepted.request_id) abort();
  }
  std::variant<SpotSubmitTicket, SpotProviderOwnerSubmitStatus> value_;
};
enum class SpotProviderOwnerResultStatus : uint8_t {
  kComplete, kUnsupportedProvider, kTimeout, kCancelled,
  kShutdown, kInternalFailure
};

struct SpotProviderOwnerResult {
  uint64_t request_id = 0;
  SpotProviderOwnerResultStatus status = SpotProviderOwnerResultStatus::kInternalFailure;
  SpotResolutionCompletionStatus resolution = SpotResolutionCompletionStatus::kNotNeeded;
  const char* content_type = nullptr;
  const uint8_t* body = nullptr;
  size_t body_length = 0;
};

// Saturating process-lifetime counters for provider batches whose atomic
// cache-plus-series preparation or commit failed. They reset on restart.
struct SpotProviderOwnerStorageStats {
  uint64_t cache_write_failures = 0;
  uint64_t series_write_failures = 0;
};

// Called on the owner thread. Body storage is owner-owned and remains valid
// only until the callback returns. Keep callbacks short and nonblocking; an
// HTTP integration should copy it into HttpResponseHandoff::Complete().
using SpotProviderOwnerCompletionFn = void (*)(void* context,
                                               const SpotProviderOwnerResult& result);

class SpotProviderOwnerStartup;
class PriceSpotService;
struct SpotProviderOwnerOptions {
  providers::ProviderExecutionCoordinator* coordinator = nullptr;
  base::Clock* wall_clock = nullptr;
  // Adapter, lanes and owner deadlines use this clock. A shared coordinator
  // uses SystemClock, so a custom monotonic clock is standalone-only.
  base::Clock* monotonic_clock = nullptr;
  discovery::CoinPopularity* popularity = nullptr;
  discovery::CoinPopularity* contract_popularity = nullptr;
  const SpotRobinhoodCatalog* robinhood_catalog = nullptr;
  base::Starter<SpotProviderOwnerStartup>* start_owner = nullptr;
  base::ErrorReporter* errors = nullptr;
};

// Bounded async CoinGecko /simple/price owner. Submit copies IDs and fiat;
// callbacks run off HTTP workers. Create publishes an initialized movable
// handle; Stop drains it. Moved-from handles are destruction-only. No route is registered here.
// Do not call Stop from a completion callback; Stop joins the owner thread.
// Create/Stop/Destruction are lifecycle operations; callers must serialize them.
class SpotProviderOwner {
 public:
  // When supplied, coordinator must already be started and must outlive this
  // owner. Stop drains and unregisters only this owner's lease; it never stops
  // the shared coordinator.
  static base::Result<SpotProviderOwner> Create(PriceSpotCache& cache, PriceSeriesStore& series,
      const config::ProviderRuntimeConfig& config, const SpotProviderOwnerOptions& options = {});
  SpotProviderOwner(SpotProviderOwner&&) noexcept;
  SpotProviderOwner& operator=(SpotProviderOwner&&) = delete;
  ~SpotProviderOwner();
  SpotProviderOwner(const SpotProviderOwner&) = delete;
  SpotProviderOwner& operator=(const SpotProviderOwner&) = delete;

  // `background` is PricesFacet.spot(..., { background: true }): the
  // request's cold misses become background flights. Their provider batches
  // use the lane's background priority (shed while the lane is busy, at most
  // one background batch in flight), cold foreground misses are batched
  // first, and a foreground request joining the flight makes it foreground.
  // Cached and stale values answer exactly as for a foreground request.
  // A foreground submit (TypeScript `foreground !== false`) of valid
  // arguments first records its CoinGecko ids in the coin popularity tracker
  // (when one is set), with `device_id` (the authenticated device, or null
  // for internal reads such as the token page quote and balance prices).
  // Background submits are operational work and never count as demand.
  SpotProviderOwnerSubmission Submit(
      const std::string_view* raw_ids, size_t raw_count, std::string_view fiat,
      SpotProviderOwnerFormat format, uint64_t deadline_monotonic_ms,
      SpotProviderOwnerCompletionFn completion, void* completion_context,
      SpotSubmitMode mode = SpotSubmitMode::kForeground, const char* device_id = nullptr,
      int64_t refresh_lead_ms = 0);
  void RecordDemand(const std::string_view* raw_ids, size_t count, const char* device_id);
  // Shares `catalog` (null: the owner's own 15-minute catalog again). Safe
  // at any time; returns after any call into the previous catalog finished,
  // so its owner may then be destroyed.
  void SetRobinhoodCatalog(const SpotRobinhoodCatalog* catalog);
  // Returns false after completion has been committed to its callback; a
  // true return guarantees one cancellation result before successful delivery.
  bool Cancel(uint64_t request_id);
  void Stop();
  size_t active_request_count() const;
  SpotProviderOwnerStorageStats storage_stats() const;

 private:
  friend class SpotProviderOwnerStartup;
  friend class PriceSpotService;
  SpotProviderOwner(fortis::prices::PriceSpotCache& cache,
                    fortis::prices::PriceSeriesStore& series,
                    const config::ProviderRuntimeConfig& config,
                    providers::ProviderExecutionCoordinator* coordinator, base::Clock* monotonic_clock = nullptr);
  bool Start();
  base::Status Initialize(base::Starter<SpotProviderOwnerStartup>* starter);
  void SetClock(base::Clock* clock);
  // Startup-only; reporter outlives all owner callbacks and must not block.
  void SetErrorReporter(base::ErrorReporter* reporter);
  void SetCoinPopularity(discovery::CoinPopularity* popularity);
  void SetContractPopularity(discovery::CoinPopularity* popularity);
  bool StartRedirected(const char* host, const char* port, const char* ca_file,
      bool (*allow_address)(void*, uint32_t), void* context,
      size_t serialized_capacity = static_cast<size_t>(-1));
  using Impl = SpotRuntime;
  std::unique_ptr<Impl, fortis::base::ObjectDeleter<Impl>> impl_;
};

// Borrowed only during Create/parent initialization. It cannot be constructed
// or copied by callers and must never be retained by a starter.
class SpotProviderOwnerStartup {
 public:
  SpotProviderOwnerStartup(const SpotProviderOwnerStartup&) = delete;
  SpotProviderOwnerStartup& operator=(const SpotProviderOwnerStartup&) = delete;
  bool Start() { return impl_->Start(); }
  bool StartRedirected(const char* host, const char* port, const char* ca_file,
      bool (*allow_address)(void*, uint32_t), void* context,
      size_t serialized_capacity = static_cast<size_t>(-1)) {
    return impl_->StartRedirected(host, port, ca_file, allow_address, context, serialized_capacity);
  }
 private:
  friend class SpotProviderOwner;
  explicit SpotProviderOwnerStartup(SpotProviderOwner& owner) : impl_(&owner) {}
  SpotProviderOwner* impl_;
};

}  // namespace fortis::prices
