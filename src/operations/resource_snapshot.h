#pragma once

#include "price_history_batch_codec.h"
#include <cstdint>
#include <optional>

namespace fortis::operations {

/** Owned, bounded OS counters. Missing/unsupported measurements stay null.
 * Collection reads only fixed procfs/cgroup files and descriptor metadata;
 * it never reads wallet data, environment values, network interfaces or secrets.
 * Collect outside request locks, then publish a value copy under the owner's
 * mutex. Counters are process-local; cgroup values describe the whole cgroup.
 */
struct ResourceSnapshot {
  // The publisher supplies its wall clock after collection; zero before publication.
  uint64_t sampled_at_ms = 0;
  std::optional<uint64_t> peak_rss_bytes;
  std::optional<uint64_t> virtual_bytes;
  std::optional<uint64_t> threads;
  std::optional<uint64_t> descriptors;
  std::optional<uint64_t> read_bytes;
  std::optional<uint64_t> written_bytes;
  std::optional<uint64_t> cgroup_memory_bytes;
  std::optional<uint64_t> cgroup_memory_limit_bytes;
  std::optional<double> cpu_quota_cores;
  // Filesystem sample has its own (slower) refresh cadence.
  uint64_t disk_sampled_at_ms = 0;
  std::optional<uint64_t> disk_available_bytes;
  enum class DiskScope { Unavailable, Filesystem, DataTree };
  DiskScope disk_scope = DiskScope::Unavailable;
};

// Paths are fixed by production. Alternate roots exist only for deterministic
// fixture tests, not request/config input. Bounded reads; no exceptions emitted
// for missing files or invalid/overflowing counters.
ResourceSnapshot CollectResourceSnapshot(const char* proc_root = "/proc", const char* cgroup_root = "/sys/fs/cgroup");
void AppendResourceSnapshot(codec::history_batch::OutBuffer& out, const ResourceSnapshot& sample);

}  // namespace fortis::operations
