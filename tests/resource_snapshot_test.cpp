#include "resource_snapshot.h"
#include "unit_test_support.h"
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>

namespace {
using fortis::operations::CollectResourceSnapshot;
using fortis::operations::ResourceSnapshot;
using fortis::testing::Require;
namespace fs = std::filesystem;
void Put(const fs::path& path, const char* value) {
  fs::create_directories(path.parent_path());
  std::ofstream file(path);
  file << value;
  Require(file.good(), "fixture write");
}
struct Fixture {
  fortis::testing::TemporaryDirectory temporary;
  std::string proc = (fs::path(temporary.path()) / "proc").string();
  std::string group = (fs::path(temporary.path()) / "group").string();
  ResourceSnapshot Read() const { return CollectResourceSnapshot(proc.c_str(), group.c_str()); }
};
void KnownCounters() {
  Fixture f;
  Put(fs::path(f.proc) / "self/status", "VmHWM: 2048 kB\nVmSize: 4096 kB\nThreads: 8\n");
  Put(fs::path(f.proc) / "self/io", "rchar: 999\nread_bytes: 0\nwrite_bytes: 4096\n");
  Put(fs::path(f.proc) / "self/cgroup", "0::/service/node\n");
  Put(fs::path(f.group) / "memory.current", "99999\n");  // Wrong group must not be sampled.
  Put(fs::path(f.group) / "service/node/memory.current", "12345\n");
  Put(fs::path(f.group) / "service/node/memory.max", "65536\n");
  Put(fs::path(f.group) / "service/node/cpu.max", "50000 100000\n");
  Put(fs::path(f.proc) / "self/fd/100001", "");
  Put(fs::path(f.proc) / "self/fd/100002", "");
  Put(fs::path(f.proc) / "self/fd/not-a-descriptor", "");
  const auto s = f.Read();
  Require(s.peak_rss_bytes == 2097152 && s.virtual_bytes == 4194304 && s.threads == 8,
          "proc kB are binary KiB; threads independent");
  Require(s.read_bytes == 0 && s.written_bytes == 4096, "known zero I/O is different from missing");
  Require(s.descriptors == 2, "numeric descriptors only, bounded scan");
  Require(s.cgroup_memory_bytes == 12345 && s.cgroup_memory_limit_bytes == 65536 && s.cpu_quota_cores == 0.5,
          "process cgroup, not host root; fractional quota");
}
void UnsupportedAndUnlimited() {
  Fixture f;
  auto s = f.Read();
  Require(!s.threads && !s.descriptors && !s.read_bytes && !s.cpu_quota_cores, "missing files are unknown, not zero");
  Put(fs::path(f.proc) / "self/cgroup", "0::/\n");
  Put(fs::path(f.group) / "memory.current", "0\n");
  Put(fs::path(f.group) / "memory.max", "max\n");
  Put(fs::path(f.group) / "cpu.max", "max 100000\n");
  s = f.Read();
  Require(s.cgroup_memory_bytes == 0 && !s.cgroup_memory_limit_bytes && !s.cpu_quota_cores,
          "unbounded quota is not a zero-capacity machine");
  Put(fs::path(f.proc) / "self/cgroup", "0::/../../outside\n");
  s = f.Read();
  Require(!s.cgroup_memory_bytes, "out-of-tree cgroup path rejected");
  Require(!CollectResourceSnapshot(nullptr, nullptr).threads, "null fixture roots safe");
}
void MalformedAndOverflow() {
  Fixture f;
  Put(fs::path(f.proc) / "self/status",
      "VmHWM: 18446744073709551615 kB\nVmSize: -1 kB\nThreads: 18446744073709551616\n");
  Put(fs::path(f.proc) / "self/io", "read_bytes: 12junk\nwrite_bytes: 18446744073709551615\n");
  Put(fs::path(f.proc) / "self/cgroup", "0::/\n");
  Put(fs::path(f.group) / "cpu.max", "100 0\n");
  const auto s = f.Read();
  Require(!s.peak_rss_bytes && !s.virtual_bytes && !s.threads && !s.read_bytes && !s.cpu_quota_cores,
          "overflow, signs, junk, zero divisor rejected");
  Require(s.written_bytes == std::numeric_limits<uint64_t>::max(),
          "unsigned counter retained without signed narrowing");
  char output[2048];
  fortis::codec::history_batch::OutBuffer out{reinterpret_cast<uint8_t*>(output), sizeof(output)};
  fortis::operations::AppendResourceSnapshot(out, s);
  Require(out.ok && out.used < sizeof(output), "bounded serialization");
  output[out.used] = 0;
  Require(std::string_view(output).find("18446744073709551615") != std::string_view::npos, "unsigned JSON encoding");
  Require(std::string_view(output).find("\"threads\":null") != std::string_view::npos, "unknown serialized explicitly");
  char tiny[8];
  fortis::codec::history_batch::OutBuffer small{reinterpret_cast<uint8_t*>(tiny), sizeof(tiny)};
  fortis::operations::AppendResourceSnapshot(small, s);
  Require(!small.ok, "small buffer cannot masquerade as complete JSON");
}
}  // namespace
int main() {
  fortis::testing::UnitSuite suite;
  suite.Run("known process and cgroup counters", KnownCounters);
  suite.Run("unsupported, unlimited and path boundaries", UnsupportedAndUnlimited);
  suite.Run("malformed, overflow and serialization bounds", MalformedAndOverflow);
  return suite.Finish("resource_snapshot_test");
}
