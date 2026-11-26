#pragma once

#include "api_telemetry.h"
#include <cmath>
#include <limits>

namespace fortis::operations::detail {
// Private value operations shared by ingestion and snapshot aggregation. No I/O,
// allocation or synchronization; the caller owns and serializes each value.
inline constexpr double kDurationBoundsMs[] = {5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000};
inline uint64_t AddCount(uint64_t left, uint64_t right) {
  return right > std::numeric_limits<uint64_t>::max() - left ? std::numeric_limits<uint64_t>::max() : left + right;
}
// Decimal encoding avoids narrowing UINT64_MAX to a negative int64. A bounded
// reverse-digit buffer is supported on the pinned guest without locale or I/O.
inline void AppendCount(codec::history_batch::OutBuffer& out, uint64_t value) {
  char digits[20];
  size_t begin = sizeof(digits);
  do {
    digits[--begin] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value);
  out.Append(digits + begin, sizeof(digits) - begin);
}
inline size_t DurationBucket(double duration) {
  for (size_t i = 0; i < ApiTelemetry::kDurationBucketCount - 1; ++i)
    if (duration <= kDurationBoundsMs[i]) return i;
  return ApiTelemetry::kDurationBucketCount - 1;
}

// Saturating counters never wrap to an apparently healthy zero. Floating sums
// retain the existing non-finite -> JSON null policy, rather than hiding overflow.
struct RequestTotals {
  uint64_t requests = 0;
  uint64_t errors = 0;
  uint64_t limited = 0;
  uint64_t client_errors = 0;
  double duration = 0;
  double max_duration = 0;
  double queue = 0;
  double max_queue = 0;
  double owner_time = 0;
  double max_owner_time = 0;
  uint64_t owner_samples = 0;
  uint64_t histogram[ApiTelemetry::kDurationBucketCount]{};
  // ms and queue_ms are finite/non-negative, normalized by the ingress owner.
  // An owner timing is counted only when finite and strictly positive.
  void Add(int status, double ms, double queue_ms, double owner_ms) {
    requests = AddCount(requests, 1);
    errors = AddCount(errors, status >= 500);
    limited = AddCount(limited, status == 429);
    client_errors = AddCount(client_errors, status >= 400 && status < 500);
    duration += ms;
    queue += queue_ms;
    if (ms > max_duration) max_duration = ms;
    if (queue_ms > max_queue) max_queue = queue_ms;
    if (std::isfinite(owner_ms) && owner_ms > 0) {
      owner_time += owner_ms;
      owner_samples = AddCount(owner_samples, 1);
      if (owner_ms > max_owner_time) max_owner_time = owner_ms;
    }
    const size_t bucket = DurationBucket(ms);
    histogram[bucket] = AddCount(histogram[bucket], 1);
  }
  void Merge(const RequestTotals& other) {
    requests = AddCount(requests, other.requests);
    errors = AddCount(errors, other.errors);
    limited = AddCount(limited, other.limited);
    client_errors = AddCount(client_errors, other.client_errors);
    duration += other.duration;
    queue += other.queue;
    owner_time += other.owner_time;
    owner_samples = AddCount(owner_samples, other.owner_samples);
    if (other.max_owner_time > max_owner_time) max_owner_time = other.max_owner_time;
    if (other.max_duration > max_duration) max_duration = other.max_duration;
    if (other.max_queue > max_queue) max_queue = other.max_queue;
    for (size_t i = 0; i < ApiTelemetry::kDurationBucketCount; ++i)
      histogram[i] = AddCount(histogram[i], other.histogram[i]);
  }
};

}  // namespace fortis::operations::detail
