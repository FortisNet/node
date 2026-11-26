#include "threading.h"
#pragma once
#include <memory>

#include "token_facts.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

namespace fortis::tokens::token_data { class TokenDataStore; }

// Recurring token work (the TypeScript service's RecurringJob sites) and the
// privacy-minimized fetch telemetry they feed, by TypeScript name:
//   TokenFetchStats (record / flush)
//   TokenDemandService.refresh (the native signal set: foreground fetch
//       requests; transfers, balances and watches are address and sync
//       data and contribute zero)
//   assessTokenDemand, allocateDemandTiers
//   TokenMarketWarmer.run / runActive
//   TokenLiquidityWarmer.run
//   TokenHolderWarmer.run
// Guest-safe.
namespace fortis::tokens::token_jobs {

enum class FetchFacet : uint8_t { kSpot, kHistory, kMarket, kProfile, kLiquidity, kHolders };
enum class FetchOutcome : uint8_t { kFresh, kStale, kFloor, kLive, kFailure };
const char* FetchFacetName(FetchFacet facet);

struct FetchSample {
  const char* network = nullptr;
  const char* token_id = nullptr;
  FetchFacet facet = FetchFacet::kMarket;
  const char* source_id = nullptr;  // null: none
  FetchOutcome outcome = FetchOutcome::kLive;
  bool negative = false;
  bool foreground = true;
  double latency_ms = 0;
  int64_t at_ms = 0;
};

// TokenFetchStats: synchronous O(1)-amortized recording into hourly
// buckets, flushed by the 60 s job. Bounded like TypeScript (10,000 pending
// buckets; overflow samples are dropped and counted). Thread-safe.
class TokenStats {
 public:
  static constexpr size_t kMaxPending = 10000;
  TokenStats();
  ~TokenStats();
  TokenStats(const TokenStats&) = delete;
  TokenStats& operator=(const TokenStats&) = delete;
  bool valid() const { return buckets_ != nullptr; }
  void Record(const FetchSample& sample);
  // flush(): writes the detached buckets; a failed write merges them back.
  // Returns the rows written (0 for none or a failure).
  size_t Flush(token_data::TokenDataStore* store, int64_t now_ms, bool* failed);
  size_t pending() const;
  uint64_t dropped() const;

  struct Bucket;

 private:
  mutable fortis::base::Mutex mutex_;
  std::unique_ptr<Bucket[]> buckets_;
  size_t count_;
  uint64_t dropped_;
};

struct JobResult {
  size_t selected = 0, refreshed = 0, unavailable = 0, unsupported = 0, failed = 0;
};

// TokenDemandService.refresh(now): demand rows written.
size_t RefreshDemand(token_facts::Work& work, int64_t now_ms);
// TokenMarketWarmer.run(now) / runActive(now).
JobResult WarmMarketsBroad(token_facts::Work& work, int64_t now_ms);
JobResult WarmMarketsActive(token_facts::Work& work, int64_t now_ms);
// TokenLiquidityWarmer.run(now).
JobResult WarmLiquidity(token_facts::Work& work, int64_t now_ms);
// TokenHolderWarmer.run(now).
JobResult WarmHolders(token_facts::Work& work, int64_t now_ms);

// The TypeScript TOKEN_WORKLOADS intervals.
constexpr int64_t kStatsFlushMs = 60000;
constexpr int64_t kDemandIntervalMs = 5 * 60000;
constexpr int64_t kMarketActiveIntervalMs = 60000;
constexpr int64_t kMarketBroadIntervalMs = 15 * 60000;
constexpr int64_t kLiquidityIntervalMs = 5 * 60000;
constexpr int64_t kHoldersIntervalMs = 6LL * 60 * 60000;
constexpr int64_t kRollupIntervalMs = 3600000;

}  // namespace fortis::tokens::token_jobs
