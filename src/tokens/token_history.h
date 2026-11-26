#pragma once

#include "token_facts.h"

#include <stddef.h>
#include <stdint.h>

// Native ports of the TypeScript history read models and their writers:
//   HolderSnapshotStore (recordCount, recordDistribution, trends, history,
//                        distributionReport)
//   LiquiditySnapshotStore (recordSuccess, recordNotFound, recordDiagnostic)
//   LiquidityHistoryFacet (get, summary24h, liquidityHistoryValidator)
//   rollupLiquidityCandles
// over token_data::TokenDataStore. Documents are built in the work arena in
// TypeScript key order. Guest-safe.
namespace fortis::tokens::token_history {

using token_facts::Work;
using codec::tj::Value;

// ---- holder snapshots -------------------------------------------------------

// recordCount(network, tokenId, {count, atLeast, source}, observedAt).
bool RecordCount(Work& work, const char* network, const char* token_id, double count, bool at_least,
                 const Value* source, int64_t observed_at);
// recordDistribution(network, tokenId, {source, holders: topHolders, ...}, observedAt)
// from a normalized TokenContractSecurity.
bool RecordDistribution(Work& work, const char* network, const char* token_id, const Value* contract,
                        int64_t observed_at);
// trends(network, tokenId, {source, now}): an object with change*Pct and
// latestAt (epoch ms number), or JSON null. False on a store failure.
bool Trends(Work& work, const char* network, const char* token_id, const char* source, int64_t now_ms,
            Value** trends);
// The holders/history route body. `source` may be null.
bool HolderHistoryDocument(Work& work, const char* network, const char* token_id, double days,
                           const char* source, int64_t now_ms, Value** document);

// ---- liquidity writer ---------------------------------------------------------

struct ObservationContext {
  bool has_observed_at = false;
  int64_t observed_at = 0;
  const char* source = nullptr;  // null: 'dexscreener'
  bool has_sampling = false;
  double sampling_seconds = 0;
};
bool RecordSuccess(Work& work, const char* network, const char* token_id, const Value* snapshot,
                   const ObservationContext& context);
bool RecordNotFound(Work& work, const char* network, const char* token_id,
                    const ObservationContext& context);
// status: "unsupported" | "error".
bool RecordDiagnostic(Work& work, const char* network, const char* token_id, const char* status,
                      int64_t checked_at, const char* source);

// ---- liquidity read model -------------------------------------------------------

enum class Range : uint8_t { k1d, k7d, k30d, k90d, k1y };
enum class Interval : uint8_t { k1h, k1d, k1w };
bool ParseRange(const char* text, size_t length, Range* range);
bool ParseInterval(const char* text, size_t length, Interval* interval);
bool ResolutionAllowed(Range range, Interval interval);

// LiquidityHistoryFacet.get and its validator payload.
bool LiquidityHistoryDocument(Work& work, const char* network, const char* token_id, Range range,
                              Interval interval, int64_t now_ms, Value** document, Value** validator);
// LiquidityHistoryFacet.summary24h.
bool LiquiditySummaryDocument(Work& work, const char* network, const char* token_id, int64_t now_ms,
                              Value** document);

// rollupLiquidityCandles(observations, candles, now): candles written.
size_t RollupLiquidityCandles(Work& work, int64_t now_ms);

}  // namespace fortis::tokens::token_history
