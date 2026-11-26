#pragma once

#include <memory>
#include "kv_store.h"
#include "ownership.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::prices {

class PriceSeriesStore;
enum class PriceSeriesStoreStatus : uint8_t;

constexpr size_t kPriceSpotMaxBatchItems = 100;
constexpr size_t kPriceSpotMaxSourceIdBytes = 512;
constexpr size_t kPriceSpotMaxProviderBytes = 31;
constexpr size_t kPriceSpotEncodedBytes = 172;
constexpr uint32_t kPriceSpotDefaultFreshMs = 30'000;
// Covers the coin price scheduler's deepest bucket: round(64 min x 1.5) =
// 5,760,000 ms plus the coordinator's 10% jitter (6,336,000 ms), and the
// market warmer's 60-minute tier-1 interval. The on-demand path still
// defaults to 30 s.
constexpr uint32_t kPriceSpotMaxFreshMs = 6'400'000;
constexpr int64_t kPriceSpotStaleMs = 10 * 60 * 1000;
constexpr int64_t kPriceSpotNegativeMs = 5 * 60 * 1000;
constexpr int64_t kPriceSpotFloorMs = 7 * 24 * 60 * 60 * 1000LL;

struct PriceSpotQuoteInput {
  const char* source_id;
  double price;
  double change24h_pct;
  double volume24h;
  bool has_change24h_pct;
  bool has_volume24h;
  int64_t as_of_ms;
  // Zero selects the default; otherwise the caller may supply bounded jitter.
  uint32_t fresh_ms;
  // Zero means upstream observation time was not supplied; never infer it.
  int64_t provider_observed_at_ms = 0;
};

struct PriceSpotValue {
  char source_id[kPriceSpotMaxSourceIdBytes + 1]{};
  char provider[kPriceSpotMaxProviderBytes + 1]{};
  double price = 0;
  double change24h_pct = 0;
  double volume24h = 0;
  bool has_change24h_pct = false;
  bool has_volume24h = false;
  int64_t as_of_ms = 0;
  int64_t fresh_until_ms = 0;
  int64_t stale_until_ms = 0;
  // Sidecar metadata, absent for legacy rows. as_of_ms retains its old meaning.
  int64_t received_at_ms = 0;
  int64_t provider_observed_at_ms = 0;
};

enum class PriceSpotLookupState : uint8_t {
  kInvalidKey,
  kMissing,
  kCorrupt,
  kIoError,
  kFresh,
  kStale,
  kNegative,
  // The envelope is expired. `value` is available only as a floor candidate
  // after a live fetch fails; callers must not treat it as a cache hit.
  kExpired,
};

struct PriceSpotLookup {
  PriceSpotLookupState state = PriceSpotLookupState::kMissing;
  bool revalidate_needed = false;
  bool cache_hit = false;
  bool stale = false;
  bool has_floor_candidate = false;
  PriceSpotValue value{};
};

enum class PriceSpotStoreStatus : uint8_t {
  kStored,
  kNoItems,
  kTransportFailureNoWrite,
  kInvalidArguments,
  kAllocationFailure,
  kReadFailure,
  kWriteFailure,
};

// One bounded durable row per canonical (fiat, source_id). A positive batch
// and its provider-answered unknowns are committed in one KvStore frame.
// Transport failure is represented explicitly and never changes the store.
// `source_id` must already be parsed and canonicalized by the caller. This
// repository checks only bounded key-serialization constraints; token grammar
// and invalid-versus-missing request semantics stay outside this cache.
class PriceSpotCache {
 public:
  explicit PriceSpotCache(storage::KvStore& store) : store_(store) {}
  ~PriceSpotCache();
  PriceSpotCache(const PriceSpotCache&) = delete;
  PriceSpotCache& operator=(const PriceSpotCache&) = delete;
  bool EnableMemory();
  uint64_t MemoryHits() const;
  uint64_t ContractMemoryHits() const;

  PriceSpotLookup Lookup(const char* fiat, const char* source_id,
                         int64_t now_ms, bool include_metadata = false) const;

  PriceSpotStoreStatus StoreProviderBatch(
      const char* fiat, const char* provider,
      const PriceSpotQuoteInput* quotes, size_t quote_count,
      const char* const* unknown_source_ids, size_t unknown_count,
      bool provider_answered, int64_t now_ms);

  // Atomically persist the cache envelope/negative rows and matching series
  // spot/open/marker rows in one KvStore frame. Both stores must wrap the same
  // KvStore. `series_status` reports the series preparation/commit outcome.
  PriceSpotStoreStatus StoreProviderBatchWithSeries(
      PriceSeriesStore& series, const char* fiat, const char* provider,
      const PriceSpotQuoteInput* quotes, size_t quote_count,
      const char* const* unknown_source_ids, size_t unknown_count,
      bool provider_answered, int64_t now_ms,
      PriceSeriesStoreStatus* series_status);

 private:
  PriceSpotStoreStatus StoreProviderBatchInternal(
      PriceSeriesStore* series, const char* fiat, const char* provider,
      const PriceSpotQuoteInput* quotes, size_t quote_count,
      const char* const* unknown_source_ids, size_t unknown_count,
      bool provider_answered, int64_t now_ms,
      PriceSeriesStoreStatus* series_status);
  struct Memory;
  // Defined beside Memory: only that unit can destroy one.
  struct MemoryDeleter { void operator()(Memory* memory) const; };
  std::unique_ptr<Memory, MemoryDeleter> memory_;
  storage::KvStore& store_;
};

}  // namespace fortis::prices
