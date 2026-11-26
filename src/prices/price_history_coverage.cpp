#include <algorithm>
#include "price_history_coverage.h"

#include <string.h>

namespace fortis::prices {
namespace {
constexpr uint8_t kFormatVersion = 2;
// Ranges are seconds; keep them convertible to the store's milliseconds.
constexpr int64_t kMaxSeconds = INT64_MAX / 1000;

void Put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
uint16_t Get16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
void Put32(uint8_t* p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t Get32(const uint8_t* p) {
  uint32_t v = 0;
  for (unsigned i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
  return v;
}
void Put64(uint8_t* p, uint64_t v) {
  for (unsigned i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint64_t Get64(const uint8_t* p) {
  uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

bool RangeOk(int64_t from, int64_t to) {
  return from >= 0 && to > from && to <= kMaxSeconds;
}

// Sorted, disjoint and non-touching: the only shape a merge can produce.
bool Canonical(const HistoryCoverage& coverage) {
  if (coverage.count > kHistoryCoverageMaxRanges) return false;
  for (size_t i = 0; i < coverage.count; ++i) {
    const HistoryCoverageRange& range = coverage.ranges[i];
    if (!RangeOk(range.from, range.to)) return false;
    if (i && range.from <= coverage.ranges[i - 1].to) return false;
  }
  return true;
}

// One step of mergeRanges over input already sorted by `from`.
void Append(HistoryCoverageRange* merged, size_t* count, int64_t from, int64_t to) {
  if (*count && from <= merged[*count - 1].to) {
    if (to > merged[*count - 1].to) merged[*count - 1].to = to;
    return;
  }
  merged[*count].from = from;
  merged[*count].to = to;
  ++*count;
}
}  // namespace

uint32_t HistoryCoverageCrc32(const void* data, size_t size) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

size_t EncodeHistoryCoverage(const HistoryCoverage& coverage, uint32_t key_crc,
                             uint8_t* output, size_t capacity) {
  if (!output || !Canonical(coverage) || coverage.expires_ms <= 0) return 0;
  const size_t size = kHistoryCoverageHeaderBytes +
      static_cast<size_t>(coverage.count) * kHistoryCoverageRangeBytes + 4;
  if (capacity < size) return 0;
  memset(output, 0, size);
  std::copy_n("HCR2", 4, output);
  output[4] = kFormatVersion;
  output[5] = coverage.interval;
  Put16(output + 6, coverage.count);
  Put64(output + 8, static_cast<uint64_t>(coverage.expires_ms));
  Put32(output + 16, key_crc);
  for (size_t i = 0; i < coverage.count; ++i) {
    uint8_t* range = output + kHistoryCoverageHeaderBytes + i * kHistoryCoverageRangeBytes;
    Put64(range, static_cast<uint64_t>(coverage.ranges[i].from));
    Put64(range + 8, static_cast<uint64_t>(coverage.ranges[i].to));
  }
  Put32(output + size - 4, HistoryCoverageCrc32(output, size - 4));
  return size;
}

bool DecodeHistoryCoverage(const uint8_t* input, size_t size, uint8_t interval,
                           uint32_t key_crc, HistoryCoverage* output) {
  if (!input || !output || size < kHistoryCoverageHeaderBytes + 4 ||
      size > kHistoryCoverageMaxBytes)
    return false;
  if (memcmp(input, "HCR2", 4) != 0 || input[4] != kFormatVersion ||
      input[5] != interval)
    return false;
  const uint16_t count = Get16(input + 6);
  if (count > kHistoryCoverageMaxRanges ||
      size != kHistoryCoverageHeaderBytes +
                  static_cast<size_t>(count) * kHistoryCoverageRangeBytes + 4)
    return false;
  if (Get32(input + 16) != key_crc ||
      Get32(input + size - 4) != HistoryCoverageCrc32(input, size - 4))
    return false;
  HistoryCoverage decoded{};
  decoded.interval = interval;
  decoded.count = count;
  decoded.expires_ms = static_cast<int64_t>(Get64(input + 8));
  if (decoded.expires_ms <= 0) return false;
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* range = input + kHistoryCoverageHeaderBytes + i * kHistoryCoverageRangeBytes;
    decoded.ranges[i].from = static_cast<int64_t>(Get64(range));
    decoded.ranges[i].to = static_cast<int64_t>(Get64(range + 8));
  }
  if (!Canonical(decoded)) return false;
  *output = decoded;
  return true;
}

bool InsertHistoryCoverage(HistoryCoverage* coverage, int64_t from, int64_t to) {
  if (!coverage || !RangeOk(from, to) || !Canonical(*coverage)) return false;
  HistoryCoverageRange merged[kHistoryCoverageMaxRanges + 1]{};
  size_t count = 0;
  bool placed = false;
  for (size_t i = 0; i < coverage->count; ++i) {
    const HistoryCoverageRange& range = coverage->ranges[i];
    if (!placed && from < range.from) {
      Append(merged, &count, from, to);
      placed = true;
    }
    Append(merged, &count, range.from, range.to);
  }
  if (!placed) Append(merged, &count, from, to);
  // A new disjoint range adds at most one entry. Losing a range only means a
  // later request may refetch it; joining two ranges would skip a hole.
  while (count > kHistoryCoverageMaxRanges) {
    size_t keep = count;
    for (size_t i = 0; i < count; ++i)
      if (merged[i].from <= from && to <= merged[i].to) { keep = i; break; }
    size_t victim = count;
    for (size_t i = 0; i < count; ++i) {
      if (i == keep) continue;
      if (victim == count ||
          merged[i].to - merged[i].from < merged[victim].to - merged[victim].from)
        victim = i;
    }
    for (size_t i = victim + 1; i < count; ++i) merged[i - 1] = merged[i];
    --count;
  }
  for (size_t i = 0; i < count; ++i) coverage->ranges[i] = merged[i];
  for (size_t i = count; i < kHistoryCoverageMaxRanges; ++i)
    coverage->ranges[i] = HistoryCoverageRange{};
  coverage->count = static_cast<uint16_t>(count);
  return true;
}

bool HistoryCoverageContains(const HistoryCoverage& coverage, int64_t from,
                             int64_t to) {
  if (to <= from) return false;
  const size_t count = coverage.count < kHistoryCoverageMaxRanges
      ? coverage.count : kHistoryCoverageMaxRanges;
  // Stored ranges never touch, so a covered window lies inside one range.
  for (size_t i = 0; i < count; ++i)
    if (coverage.ranges[i].from <= from && to <= coverage.ranges[i].to) return true;
  return false;
}

bool AlignHistoryCoverageInward(int64_t from, int64_t to, int64_t width,
                                int64_t* aligned_from, int64_t* aligned_to) {
  if (!aligned_from || !aligned_to || width <= 0 || from < 0 || to <= from ||
      from > INT64_MAX - (width - 1))
    return false;
  const int64_t first = (from + width - 1) / width * width;
  const int64_t last = to / width * width;
  if (last <= first) return false;
  *aligned_from = first;
  *aligned_to = last;
  return true;
}

HistoryBackfillPlanner::HistoryBackfillPlanner(const HistoryCoverage* covered)
    : covered_(covered) {}

void HistoryBackfillPlanner::Emit(int64_t from, int64_t to) {
  if (count_ == kHistoryCoverageMaxGaps) {
    collapsed_ = true;
    return;
  }
  gaps_[count_][0] = from;
  gaps_[count_][1] = to;
  ++count_;
}

bool HistoryBackfillPlanner::AddRun(int64_t from, int64_t to) {
  if (collapsed_) return false;
  if (to <= from) return true;
  size_t ranges = covered_ ? covered_->count : 0;
  if (ranges > kHistoryCoverageMaxRanges) ranges = kHistoryCoverageMaxRanges;
  // subtractCoveredRanges: a covered range ending at or before the run, or
  // starting at or after its end, leaves the run untouched.
  while (cursor_ < ranges && covered_->ranges[cursor_].to <= from) ++cursor_;
  int64_t position = from;
  for (size_t i = cursor_; i < ranges && covered_->ranges[i].from < to; ++i) {
    const HistoryCoverageRange& range = covered_->ranges[i];
    if (range.from > position) Emit(position, range.from);
    if (range.to > position) position = range.to;
    if (collapsed_ || position >= to) break;
  }
  if (!collapsed_ && position < to) Emit(position, to);
  return !collapsed_;
}

}  // namespace fortis::prices
