#include "resource_snapshot.h"

#include <cstdio>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <limits>
#include <memory>

namespace fortis::operations {
namespace {
using codec::history_batch::OutBuffer;

// Strict unsigned decimal, with an optional exact kB suffix used by procfs.
std::optional<uint64_t> Counter(const char* text, bool kilobytes = false) {
  while (*text == ' ' || *text == '\t') ++text;
  if (*text < '0' || *text > '9') return std::nullopt;
  uint64_t value = 0;
  while (*text >= '0' && *text <= '9') {
    const auto digit = static_cast<uint64_t>(*text++ - '0');
    if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) return std::nullopt;
    value = value * 10 + digit;
  }
  while (*text == ' ' || *text == '\t') ++text;
  if (kilobytes) {
    if (strncmp(text, "kB", 2) != 0 || value > std::numeric_limits<uint64_t>::max() / 1024) return std::nullopt;
    value *= 1024;
    text += 2;
  }
  while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
  return *text ? std::nullopt : std::optional<uint64_t>(value);
}

template <typename Read>
void ReadLines(const char* root, const char* suffix, Read read) {
  char path[1024];
  const int length = snprintf(path, sizeof(path), "%s/%s", root, suffix);
  if (length < 0 || static_cast<size_t>(length) >= sizeof(path)) return;
  const auto close_file = [](FILE* value) noexcept { (void)fclose(value); };
  std::unique_ptr<FILE, decltype(close_file)> file(fopen(path, "r"), close_file);
  if (!file) return;
  char line[512];
  size_t lines = 0;
  while (lines++ < 128 && fgets(line, sizeof(line), file.get())) {
    // Truncated input must never turn a partial counter into a valid sample.
    if (!strchr(line, '\n') && !feof(file.get())) break;
    read(line);
  }
}

void Number(OutBuffer& out, const std::optional<uint64_t>& number) {
  if (!number) {
    out.Text("null");
    return;
  }
  char value[32];
  const int size = snprintf(value, sizeof(value), "%llu", static_cast<unsigned long long>(*number));
  if (size <= 0 || static_cast<size_t>(size) >= sizeof(value)) {
    out.ok = false;
    return;
  }
  out.Append(value, static_cast<size_t>(size));
}
}  // namespace

ResourceSnapshot CollectResourceSnapshot(const char* proc_root, const char* cgroup_root) {
  ResourceSnapshot result;
  if (!proc_root || !cgroup_root) return result;
  ReadLines(proc_root, "self/status", [&](const char* line) {
    if (strncmp(line, "VmHWM:", 6) == 0) result.peak_rss_bytes = Counter(line + 6, true);
    if (strncmp(line, "VmSize:", 7) == 0) result.virtual_bytes = Counter(line + 7, true);
    if (strncmp(line, "Threads:", 8) == 0) result.threads = Counter(line + 8);
  });
  ReadLines(proc_root, "self/io", [&](const char* line) {
    if (strncmp(line, "read_bytes:", 11) == 0) result.read_bytes = Counter(line + 11);
    if (strncmp(line, "write_bytes:", 12) == 0) result.written_bytes = Counter(line + 12);
  });
  char group_root[1024]{};
  ReadLines(proc_root, "self/cgroup", [&](const char* line) {
    if (strncmp(line, "0::/", 4) != 0) return;
    char relative[512];
    const size_t length = strcspn(line + 3, "\r\n");
    if (!length || length >= sizeof(relative)) return;
    memcpy(relative, line + 3, length);
    relative[length] = 0;
    // Do not follow a process path outside the mounted cgroup tree.
    if (strstr(relative, "/../") || strstr(relative, "/./") || strcmp(relative, "/..") == 0 ||
        strcmp(relative, "/.") == 0 || (length >= 3 && strcmp(relative + length - 3, "/..") == 0))
      return;
    const int size = snprintf(group_root, sizeof(group_root), "%s%s", cgroup_root, relative);
    if (size < 0 || static_cast<size_t>(size) >= sizeof(group_root)) group_root[0] = 0;
  });
  if (group_root[0]) {
    ReadLines(group_root, "memory.current", [&](const char* line) { result.cgroup_memory_bytes = Counter(line); });
    ReadLines(group_root, "memory.max", [&](const char* line) { result.cgroup_memory_limit_bytes = Counter(line); });
    ReadLines(group_root, "cpu.max", [&](const char* line) {
      char quota[32], period[32], extra[2];
      if (sscanf(line, "%31s %31s %1s", quota, period, extra) != 2) return;
      const auto q = Counter(quota), p = Counter(period);
      if (q && p && *q > 0 && *p > 0) result.cpu_quota_cores = static_cast<double>(*q) / static_cast<double>(*p);
    });
  }
  char path[1024];
  const int length = snprintf(path, sizeof(path), "%s/self/fd", proc_root);
  if (length > 0 && static_cast<size_t>(length) < sizeof(path)) {
    const auto close_directory = [](DIR* value) noexcept { (void)closedir(value); };
    std::unique_ptr<DIR, decltype(close_directory)> directory(opendir(path), close_directory);
    if (directory) {
      uint64_t count = 0;
      size_t scanned = 0;
      bool complete = true;
      const int own = dirfd(directory.get());
      errno = 0;
      while (const dirent* entry = readdir(directory.get())) {
        if (++scanned > 65539) {
          complete = false;
          break;
        }
        const auto fd = Counter(entry->d_name);
        if (!fd || *fd > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
            (own >= 0 && *fd == static_cast<uint64_t>(own)))
          continue;
        if (++count > 65536) {
          complete = false;
          break;
        }
      }
      complete = complete && errno == 0;
      if (complete) result.descriptors = count;
    }
  }
  return result;
}

void AppendResourceSnapshot(OutBuffer& out, const ResourceSnapshot& sample) {
  out.Text(",\"resources\":{\"sampledAtMs\":");
  Number(out, sample.sampled_at_ms);
  const auto field = [&](const char* name, const std::optional<uint64_t>& value) {
    out.Text(name);
    Number(out, value);
  };
  field(",\"peakRssBytes\":", sample.peak_rss_bytes);
  field(",\"virtualBytes\":", sample.virtual_bytes);
  field(",\"threads\":", sample.threads);
  field(",\"openDescriptors\":", sample.descriptors);
  field(",\"readBytesSinceBoot\":", sample.read_bytes);
  field(",\"writtenBytesSinceBoot\":", sample.written_bytes);
  field(",\"cgroupMemoryBytes\":", sample.cgroup_memory_bytes);
  field(",\"cgroupMemoryLimitBytes\":", sample.cgroup_memory_limit_bytes);
  out.Text(",\"cpuQuotaCores\":");
  if (sample.cpu_quota_cores) codec::history_batch::AppendJsNumber(out, *sample.cpu_quota_cores);
  else out.Text("null");
  field(",\"diskSampledAtMs\":", sample.disk_sampled_at_ms);
  field(",\"diskAvailableBytes\":", sample.disk_available_bytes);
  out.Text(",\"diskScope\":\"");
  switch (sample.disk_scope) {
    case ResourceSnapshot::DiskScope::Filesystem: out.Text("filesystem"); break;
    case ResourceSnapshot::DiskScope::DataTree: out.Text("data-tree"); break;
    case ResourceSnapshot::DiskScope::Unavailable: out.Text("unavailable"); break;
  }
  out.Text("\"}");
}
}  // namespace fortis::operations
