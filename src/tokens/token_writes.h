#pragma once

#include "token_comparison_store.h"
#include "token_data.h"
#include "token_json.h"

#include <stddef.h>
#include <stdint.h>

// Atomic token writes. Each logically related update of the token facts,
// comparison and raw liquidity documents is staged into one KvBatch under
// both store locks and committed as one KvStore::Write frame, so a replayed
// log shows all of it or none of it. The TypeScript transactional units:
//
//   liquidity history recordSuccess / recordNotFound      facts + comparison
//       + observation row + pair row, one tokens.write transaction
//   liquidity history recordDiagnostic                    facts + comparison
//   token market warmer                                   64 tokens per
//       transaction, each facts + comparison + demand
//   Token360 writeThrough                                 facts patch, then
//       the market comparison patch: TWO transactions in TypeScript
//
// The SWR entries those writers prime afterwards are independent cache
// writes and stay outside the frame. Lock order (never reversed):
// TokenDataStore::Lock, TokenComparisonStore::WriterLock, KvStore mutex.
// Guest-safe.
namespace fortis::tokens::token_writes {

// Optional frame accounting for tests and measurements.
struct FrameStats {
  size_t frames = 0;         // committed KvStore::Write frames
  size_t failed_frames = 0;  // frames whose Write failed (outcome uncertain)
  size_t mutations = 0;      // mutations in committed frames
  size_t bytes = 0;          // encoded bytes of committed frames
  size_t max_frame_bytes = 0;
  size_t splits = 0;         // market chunks split by the frame bound
  size_t max_staging_bytes = 0;  // largest KvBatch heap footprint
};

// One liquidity outcome. `observation` is null for a diagnostic
// ('unsupported' / 'error'); the facts fields and comparison patch are the
// ones LiquiditySnapshotStore builds for that outcome.
struct LiquidityOutcome {
  const char* network = nullptr;
  const char* token_id = nullptr;
  const codec::tj::Value* facts = nullptr;
  const token_store::ComparisonPatch* comparison = nullptr;
  const token_data::Observation* observation = nullptr;
};

// Staging bound for one liquidity outcome: a facts document, a comparison
// document, an observation block and its pair row.
constexpr size_t kLiquidityFrameBytes = 2 * token_data::kMaxDocumentBytes + 64 * 1024;

// All-or-nothing: false (nothing staged is committed) when any document is
// refused or the frame write fails.
bool RecordLiquidity(token_data::TokenDataStore& data, const LiquidityOutcome& outcome, int64_t now_ms,
                     FrameStats* stats = nullptr);

// Token360Facet.writeThrough: the facts patch plus, for a market change,
// the market comparison patch (`comparison` may be null), committed as one
// frame. TypeScript commits the two in separate transactions, so a
// comparison patch the native bounds refuse (an over-long or invalid
// string, a corrupt stored row) does not hold the facts patch back: the
// facts patch then commits alone and the call returns false, as before.
bool WriteThrough(token_data::TokenDataStore& data, const char* network, const char* token_id,
                  const codec::tj::Value* facts, const token_store::ComparisonPatch* patch, int64_t now_ms,
                  FrameStats* stats = nullptr);

// One TokenMarketWarmer refresh: facts, comparison and demand patches.
struct MarketRefresh {
  const char* network = nullptr;
  const char* token_id = nullptr;
  const codec::tj::Value* facts = nullptr;
  const token_store::ComparisonPatch* comparison = nullptr;
  const codec::tj::Value* demand = nullptr;
  bool committed = false;  // out
};

constexpr size_t kMarketChunk = 64;
// Staging bound for one market frame. One token's group (three documents)
// is at most ~520 KiB, so a group always fits an empty batch; a chunk whose
// documents exceed the bound splits into further frames at token
// boundaries, never inside a group.
constexpr size_t kMarketFrameBytes = 4 * 1024 * 1024;

// Commits `items` in chunks of kMarketChunk tokens, each chunk one frame
// unless the bound splits it. A token whose own group is refused (a native
// bound or a corrupt stored row; TypeScript has no such per-token refusal)
// is rolled back out of the frame and the rest of its chunk commits. A failed
// frame write stops the run (the store has closed itself); later items stay
// uncommitted. Sets each item's `committed` and returns how many committed.
size_t RecordMarketRefreshes(token_data::TokenDataStore& data, MarketRefresh* items, size_t count,
                             int64_t now_ms, FrameStats* stats = nullptr);

}  // namespace fortis::tokens::token_writes
