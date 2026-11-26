#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fortis::prices {
namespace history_batch { struct HistoryWindow; }

// Bounded equivalent of the TypeScript CandleHistory's coverage list.
// TypeScript keeps one
// merged list of half-open second ranges per `candles:cov:<source>:<fiat>:
// <interval>` key, and every mark rewrites the whole list with a fresh
// six-hour TTL. Native keeps one durable record per (interval, fiat, source)
// with the same merge and TTL rules, capped at kHistoryCoverageMaxRanges.
//
// Coverage only ever lets a request skip provider work, so every failure
// mode here removes coverage: a corrupt record reads as uncovered, and
// eviction drops whole ranges instead of joining them across a hole.
constexpr size_t kHistoryCoverageMaxRanges = 32;
constexpr int64_t kHistoryCoverageTtlMs = 6 * 60 * 60 * 1000LL;
// CandleHistory MAX_GAP_REQUESTS: more uncovered gaps than this collapse into
// one aligned request over the whole window.
constexpr size_t kHistoryCoverageMaxGaps = 8;

// Record layout (little-endian):
//   0  "HCR2"            4
//   4  format version    1 (= 2)
//   5  interval code     1 (PriceSeriesInterval value; must match the key)
//   6  range count       2 (<= kHistoryCoverageMaxRanges)
//   8  expires_ms        8 (also the KV TTL)
//  16  CRC32 of the key  4 (binds the value to its key)
//  20  ranges            16 * count: from_s, to_s as int64, sorted, disjoint
//                        and non-touching (stored records are always merged)
//  end CRC32 of all preceding bytes
constexpr size_t kHistoryCoverageHeaderBytes = 20;
constexpr size_t kHistoryCoverageRangeBytes = 16;
constexpr size_t kHistoryCoverageMaxBytes = kHistoryCoverageHeaderBytes +
    kHistoryCoverageMaxRanges * kHistoryCoverageRangeBytes + 4;

struct HistoryCoverageRange {
  int64_t from = 0;  // seconds, inclusive
  int64_t to = 0;    // seconds, exclusive
};

struct HistoryCoverage {
  uint8_t interval = 0;
  uint16_t count = 0;
  int64_t expires_ms = 0;
  HistoryCoverageRange ranges[kHistoryCoverageMaxRanges]{};
};

uint32_t HistoryCoverageCrc32(const void* data, size_t size);

// Returns the encoded size, or 0 when the record is not canonical.
size_t EncodeHistoryCoverage(const HistoryCoverage& coverage, uint32_t key_crc,
                             uint8_t* output, size_t capacity);
// Rejects the whole record on any structural, ordering or checksum error.
bool DecodeHistoryCoverage(const uint8_t* input, size_t size, uint8_t interval,
                           uint32_t key_crc, HistoryCoverage* output);

// mergeRanges([...ranges, [from, to]]): touching or overlapping ranges merge.
// Above capacity the narrowest range other than the one containing
// [from, to) is dropped (ties: earliest). Returns false for an invalid range
// or a non-canonical input record; the record is then left unchanged.
bool InsertHistoryCoverage(HistoryCoverage* coverage, int64_t from, int64_t to);
bool HistoryCoverageContains(const HistoryCoverage& coverage, int64_t from,
                             int64_t to);
// Shrinks [from, to) to whole buckets so a partly fetched bucket is never
// claimed. Returns false when no whole bucket remains.
bool AlignHistoryCoverageInward(int64_t from, int64_t to, int64_t width,
                                int64_t* aligned_from, int64_t* aligned_to);

// Streams missing-bucket runs (ascending, as missingCandleRanges emits them)
// through subtractCoveredRanges and the >8-gaps collapse rule in O(runs +
// ranges) with fixed storage. Once collapsed, the caller replaces the plan
// with one request over the aligned window, which ignores coverage exactly as
// TypeScript does.
class HistoryBackfillPlanner {
 public:
  static size_t Plan(const history_batch::HistoryWindow& window, const uint8_t* bitmap,
                      const HistoryCoverage* covered, int64_t gaps[][2]);
  explicit HistoryBackfillPlanner(const HistoryCoverage* covered);
  // Returns false once the plan has collapsed.
  bool AddRun(int64_t from, int64_t to);
  bool collapsed() const { return collapsed_; }
  size_t count() const { return count_; }
  int64_t gap_from(size_t index) const { return gaps_[index][0]; }
  int64_t gap_to(size_t index) const { return gaps_[index][1]; }

 private:
  void Emit(int64_t from, int64_t to);
  const HistoryCoverage* covered_;
  size_t cursor_ = 0;
  size_t count_ = 0;
  bool collapsed_ = false;
  int64_t gaps_[kHistoryCoverageMaxGaps][2]{};
};

using HistoryGapPlanner = HistoryBackfillPlanner;

}  // namespace fortis::prices
