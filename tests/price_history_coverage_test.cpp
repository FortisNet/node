#include <algorithm>
#include "price_history_coverage.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <initializer_list>

using fortis::prices::HistoryCoverage;
using fortis::prices::HistoryGapPlanner;

namespace {
constexpr int64_t kHour = 3600;

HistoryCoverage make(std::initializer_list<int64_t> bounds) {
  HistoryCoverage coverage{};
  coverage.interval = 1;
  coverage.expires_ms = 1;
  const int64_t* values = bounds.begin();
  for (size_t i = 0; i + 1 < bounds.size(); i += 2) {
    coverage.ranges[coverage.count].from = values[i];
    coverage.ranges[coverage.count].to = values[i + 1];
    ++coverage.count;
  }
  return coverage;
}

// TypeScript's subtractCoveredRanges, literally: apply each covered
// range in stored order to every pending gap.
size_t ts_subtract(const int64_t (*gaps)[2], size_t gap_count, const HistoryCoverage& covered,
                   int64_t (*out)[2], size_t capacity) {
  int64_t pending[256][2]; size_t pending_count = gap_count;
  assert(gap_count <= 256);
  memcpy(pending, gaps, gap_count * sizeof(pending[0]));
  for (size_t c = 0; c < covered.count; ++c) {
    const int64_t cf = covered.ranges[c].from, ct = covered.ranges[c].to;
    int64_t next[256][2]; size_t next_count = 0;
    for (size_t i = 0; i < pending_count; ++i) {
      const int64_t from = pending[i][0], to = pending[i][1];
      if (ct <= from || cf >= to) {
        next[next_count][0] = from; next[next_count][1] = to; ++next_count;
        continue;
      }
      if (cf > from) { next[next_count][0] = from; next[next_count][1] = cf; ++next_count; }
      if (ct < to) { next[next_count][0] = ct; next[next_count][1] = to; ++next_count; }
      assert(next_count <= 256);
    }
    memcpy(pending, next, next_count * sizeof(pending[0])); pending_count = next_count;
  }
  assert(pending_count <= capacity);
  memcpy(out, pending, pending_count * sizeof(pending[0]));
  return pending_count;
}

// Compares HistoryGapPlanner with subtract-then-collapse over the same runs.
void check_plan(const int64_t (*runs)[2], size_t run_count, const HistoryCoverage* covered) {
  HistoryGapPlanner planner(covered);
  for (size_t i = 0; i < run_count; ++i) planner.AddRun(runs[i][0], runs[i][1]);
  int64_t expected[256][2];
  const HistoryCoverage empty{};
  const size_t count = ts_subtract(runs, run_count, covered ? *covered : empty, expected, 256);
  if (count > fortis::prices::kHistoryCoverageMaxGaps) {
    assert(planner.collapsed());
    return;
  }
  assert(!planner.collapsed() && planner.count() == count);
  for (size_t i = 0; i < count; ++i)
    assert(planner.gap_from(i) == expected[i][0] && planner.gap_to(i) == expected[i][1]);
}

void verify_codec() {
  HistoryCoverage coverage = make({0, 10, 20, 30, 50, 60});
  coverage.expires_ms = 1'800'000'000'000LL;
  uint8_t encoded[fortis::prices::kHistoryCoverageMaxBytes]{};
  const size_t size = fortis::prices::EncodeHistoryCoverage(coverage, 0x12345678u, encoded, sizeof(encoded));
  assert(size == 20 + 3 * 16 + 4);
  assert(memcmp(encoded, "HCR2", 4) == 0 && encoded[4] == 2 && encoded[5] == 1);
  HistoryCoverage decoded{};
  assert(fortis::prices::DecodeHistoryCoverage(encoded, size, 1, 0x12345678u, &decoded));
  assert(decoded.count == 3 && decoded.expires_ms == coverage.expires_ms);
  for (size_t i = 0; i < 3; ++i)
    assert(decoded.ranges[i].from == coverage.ranges[i].from &&
           decoded.ranges[i].to == coverage.ranges[i].to);

  // Every rejection leaves the output untouched and reads as uncovered.
  HistoryCoverage untouched{};
  assert(!fortis::prices::DecodeHistoryCoverage(encoded, size, 2, 0x12345678u, &untouched));  // interval
  assert(!fortis::prices::DecodeHistoryCoverage(encoded, size, 1, 0x12345679u, &untouched));  // key
  assert(!fortis::prices::DecodeHistoryCoverage(encoded, size - 1, 1, 0x12345678u, &untouched));
  assert(!fortis::prices::DecodeHistoryCoverage(encoded, 3, 1, 0x12345678u, &untouched));
  assert(untouched.count == 0);
  for (size_t byte = 0; byte < size; ++byte) {
    uint8_t mutated[fortis::prices::kHistoryCoverageMaxBytes];
    memcpy(mutated, encoded, size);
    mutated[byte] ^= 0x01;
    assert(!fortis::prices::DecodeHistoryCoverage(mutated, size, 1, 0x12345678u, &untouched));
  }
  // Structurally invalid records whose checksum is recomputed still fail.
  const auto reject_with_crc = [&](void (*mutate)(uint8_t*), size_t mutated_size) {
    uint8_t mutated[fortis::prices::kHistoryCoverageMaxBytes + 16]{};
    memcpy(mutated, encoded, size);
    mutate(mutated);
    const uint32_t crc = fortis::prices::HistoryCoverageCrc32(mutated, mutated_size - 4);
    for (unsigned i = 0; i < 4; ++i)
      mutated[mutated_size - 4 + i] = static_cast<uint8_t>(crc >> (8 * i));
    assert(!fortis::prices::DecodeHistoryCoverage(mutated, mutated_size, 1, 0x12345678u, &untouched));
  };
  reject_with_crc([](uint8_t* p) { p[4] = 1; }, size);                    // version
  reject_with_crc([](uint8_t* p) { std::copy_n("HCV1", 4, p); }, size);        // magic
  reject_with_crc([](uint8_t* p) { p[20 + 16 + 0] = 9; }, size);          // overlaps range 0
  reject_with_crc([](uint8_t* p) { p[20 + 16 + 0] = 10; }, size);         // touches range 0
  reject_with_crc([](uint8_t* p) { p[20 + 8] = 0; }, size);               // to <= from
  reject_with_crc([](uint8_t* p) { p[20 + 7] = 0x80; }, size);            // negative from
  reject_with_crc([](uint8_t* p) { memset(p + 8, 0, 8); }, size);         // no expiry
  reject_with_crc([](uint8_t* p) { p[6] = 33; p[7] = 0; }, size);          // count > max
  reject_with_crc([](uint8_t* p) { p[6] = 2; }, size);                     // count/size mismatch
  // Non-canonical input never encodes.
  HistoryCoverage unsorted = make({20, 30, 0, 10});
  assert(fortis::prices::EncodeHistoryCoverage(unsorted, 0, encoded, sizeof(encoded)) == 0);
  HistoryCoverage touching = make({0, 10, 10, 20});
  assert(fortis::prices::EncodeHistoryCoverage(touching, 0, encoded, sizeof(encoded)) == 0);
  assert(fortis::prices::EncodeHistoryCoverage(coverage, 0, encoded, 10) == 0);
}

void verify_merge_and_eviction() {
  HistoryCoverage coverage{};
  assert(fortis::prices::InsertHistoryCoverage(&coverage, 10, 20));
  assert(fortis::prices::InsertHistoryCoverage(&coverage, 30, 40));
  assert(coverage.count == 2);
  assert(fortis::prices::InsertHistoryCoverage(&coverage, 20, 30));  // touching both: one range
  assert(coverage.count == 1 && coverage.ranges[0].from == 10 && coverage.ranges[0].to == 40);
  assert(fortis::prices::InsertHistoryCoverage(&coverage, 0, 5));
  assert(fortis::prices::InsertHistoryCoverage(&coverage, 3, 12));   // overlaps both neighbours
  assert(coverage.count == 1 && coverage.ranges[0].from == 0 && coverage.ranges[0].to == 40);
  assert(fortis::prices::InsertHistoryCoverage(&coverage, 15, 25));  // inside: unchanged
  assert(coverage.count == 1 && coverage.ranges[0].to == 40);
  assert(!fortis::prices::InsertHistoryCoverage(&coverage, 5, 5));
  assert(!fortis::prices::InsertHistoryCoverage(&coverage, -1, 5));
  HistoryCoverage broken = make({20, 30, 0, 10});
  assert(!fortis::prices::InsertHistoryCoverage(&broken, 40, 50));
  assert(broken.ranges[0].from == 20);  // unchanged

  // Fill to capacity with disjoint ranges of width 2 (one of width 1).
  HistoryCoverage full{};
  for (int64_t i = 0; i < 32; ++i)
    assert(fortis::prices::InsertHistoryCoverage(&full, i * 10, i * 10 + (i == 5 ? 1 : 2)));
  assert(full.count == 32);
  // A new narrow range is kept; the narrowest other range is dropped.
  assert(fortis::prices::InsertHistoryCoverage(&full, 1000, 1001));
  assert(full.count == 32);
  assert(!fortis::prices::HistoryCoverageContains(full, 50, 51));
  assert(fortis::prices::HistoryCoverageContains(full, 1000, 1001));
  // Ties drop the earliest equally narrow range other than the new one.
  assert(fortis::prices::InsertHistoryCoverage(&full, 2000, 2002));
  assert(full.count == 32);
  assert(!fortis::prices::HistoryCoverageContains(full, 1000, 1001));
  assert(fortis::prices::InsertHistoryCoverage(&full, 3000, 3002));
  assert(!fortis::prices::HistoryCoverageContains(full, 0, 2));
  assert(fortis::prices::HistoryCoverageContains(full, 10, 12));
  // Eviction never joins: every hole between kept ranges is still a hole.
  for (size_t i = 1; i < full.count; ++i) assert(full.ranges[i].from > full.ranges[i - 1].to);
  // A range merging with a neighbour does not need eviction.
  assert(fortis::prices::InsertHistoryCoverage(&full, 12, 20));
  assert(full.count == 31 && fortis::prices::HistoryCoverageContains(full, 10, 22));
}

void verify_contains_and_align() {
  const HistoryCoverage coverage = make({0, 10, 20, 30});
  assert(fortis::prices::HistoryCoverageContains(coverage, 0, 10));
  assert(fortis::prices::HistoryCoverageContains(coverage, 22, 25));
  assert(!fortis::prices::HistoryCoverageContains(coverage, 5, 25));
  assert(!fortis::prices::HistoryCoverageContains(coverage, 10, 20));
  assert(!fortis::prices::HistoryCoverageContains(coverage, 5, 5));
  int64_t from = 0, to = 0;
  assert(fortis::prices::AlignHistoryCoverageInward(kHour, 3 * kHour, kHour, &from, &to));
  assert(from == kHour && to == 3 * kHour);
  // Partly fetched edge buckets are never claimed.
  assert(fortis::prices::AlignHistoryCoverageInward(kHour + 1, 4 * kHour - 1, kHour, &from, &to));
  assert(from == 2 * kHour && to == 3 * kHour);
  assert(!fortis::prices::AlignHistoryCoverageInward(kHour + 1, 3 * kHour - 1, kHour, &from, &to));
  assert(!fortis::prices::AlignHistoryCoverageInward(kHour + 1800, 2 * kHour + 1800, kHour, &from, &to));
  assert(!fortis::prices::AlignHistoryCoverageInward(-1, kHour, kHour, &from, &to));
  assert(!fortis::prices::AlignHistoryCoverageInward(kHour, kHour, kHour, &from, &to));
  assert(!fortis::prices::AlignHistoryCoverageInward(0, kHour, 0, &from, &to));
}

void verify_planner() {
  // No coverage: runs pass through until the ninth gap collapses the plan.
  int64_t runs[16][2];
  for (int i = 0; i < 16; ++i) { runs[i][0] = i * 2 * kHour; runs[i][1] = runs[i][0] + kHour; }
  check_plan(runs, 8, nullptr);
  check_plan(runs, 9, nullptr);
  {
    HistoryGapPlanner planner(nullptr);
    for (int i = 0; i < 9; ++i) planner.AddRun(runs[i][0], runs[i][1]);
    assert(planner.collapsed() && !planner.AddRun(100 * kHour, 101 * kHour));
  }
  // Ten raw holes, four covered: subtraction happens before the >8 rule.
  HistoryCoverage four{};
  for (int i = 0; i < 4; ++i)
    assert(fortis::prices::InsertHistoryCoverage(&four, runs[i][0], runs[i][1]));
  check_plan(runs, 10, &four);
  {
    HistoryGapPlanner planner(&four);
    for (int i = 0; i < 10; ++i) planner.AddRun(runs[i][0], runs[i][1]);
    assert(!planner.collapsed() && planner.count() == 6 && planner.gap_from(0) == runs[4][0]);
  }
  // Twelve raw holes, two covered: still more than eight, so collapse.
  HistoryCoverage two{};
  assert(fortis::prices::InsertHistoryCoverage(&two, runs[0][0], runs[1][1]));
  check_plan(runs, 12, &two);
  // A covered range spanning several runs and splitting the edges.
  const HistoryCoverage spanning = make({kHour / 2, 5 * kHour + kHour / 2});
  check_plan(runs, 6, &spanning);
  // One long run cut into pieces by many covered ranges.
  const int64_t long_run[1][2] = {{0, 100 * kHour}};
  HistoryCoverage holes{};
  for (int i = 0; i < 7; ++i)
    assert(fortis::prices::InsertHistoryCoverage(&holes, (i * 10 + 2) * kHour, (i * 10 + 5) * kHour));
  check_plan(long_run, 1, &holes);  // eight pieces: no collapse
  assert(fortis::prices::InsertHistoryCoverage(&holes, 82 * kHour, 85 * kHour));
  check_plan(long_run, 1, &holes);  // nine pieces: collapse
  // Fully covered.
  const HistoryCoverage all = make({0, 1000 * kHour});
  check_plan(runs, 16, &all);
  {
    HistoryGapPlanner planner(&all);
    for (int i = 0; i < 16; ++i) planner.AddRun(runs[i][0], runs[i][1]);
    assert(!planner.collapsed() && planner.count() == 0);
  }
  // Touching but not overlapping ranges leave runs whole.
  const HistoryCoverage touching = make({0, kHour, 3 * kHour, 4 * kHour});
  const int64_t middle[1][2] = {{kHour, 3 * kHour}};
  check_plan(middle, 1, &touching);

  // Deterministic pseudo-random agreement with the TypeScript algorithm.
  uint32_t seed = 0x9e3779b9u;
  const auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
  for (int round = 0; round < 2000; ++round) {
    int64_t random_runs[64][2]; size_t run_count = 0; int64_t cursor = 0;
    const size_t wanted = next() % 40;
    for (size_t i = 0; i < wanted; ++i) {
      cursor += (1 + next() % 3) * kHour;
      const int64_t width = (1 + next() % 6) * kHour;
      random_runs[run_count][0] = cursor; random_runs[run_count][1] = cursor + width;
      ++run_count; cursor += width;
    }
    HistoryCoverage random_cover{};
    const size_t ranges = next() % 40;
    for (size_t i = 0; i < ranges; ++i) {
      const int64_t start = (next() % 400) * kHour;
      assert(fortis::prices::InsertHistoryCoverage(&random_cover, start,
          start + (1 + next() % 12) * kHour));
    }
    check_plan(random_runs, run_count, &random_cover);
  }
}
}  // namespace

int main() {
  verify_codec();
  verify_merge_and_eviction();
  verify_contains_and_align();
  verify_planner();
  puts("native history coverage codec, merge, eviction and gap planner passed");
}
