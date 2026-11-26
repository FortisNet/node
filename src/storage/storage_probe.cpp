#include "storage_probe.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fortis::storage {

namespace {
constexpr char kDir[] = "/data";
constexpr char kFile[] = "/data/probe.counter";
constexpr char kTemp[] = "/data/probe.counter.tmp";

bool mounted_directory() {
  struct stat root {};
  struct stat info {};
  if (stat("/", &root) != 0 || stat(kDir, &info) != 0 || !S_ISDIR(info.st_mode))
    return false;
  fortis::base::LogInfo("probe", "storage probe: root dev=%llu data dev=%llu",
          static_cast<unsigned long long>(root.st_dev),
          static_cast<unsigned long long>(info.st_dev));
  return true;
}

bool write_all(int fd, const char* data, size_t size) {
  while (size > 0) {
    const ssize_t written = write(fd, data, size);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
    data += written;
    size -= static_cast<size_t>(written);
  }
  return true;
}
}  // namespace

bool storage_probe_read(uint64_t* value) {
  if (!value || !mounted_directory()) return false;
  const int fd = open(kFile, O_RDONLY);
  if (fd < 0 && errno == ENOENT) {
    *value = 0;
    return true;
  }
  if (fd < 0) return false;
  char buffer[32]{};
  const ssize_t count = read(fd, buffer, sizeof(buffer) - 1);
  const int close_result = close(fd);
  if (count <= 0 || close_result != 0) return false;
  buffer[count] = '\0';
  char* end = nullptr;
  const unsigned long long parsed = strtoull(buffer, &end, 10);
  if (end == buffer || *end != '\n' || end[1] != '\0') return false;
  *value = static_cast<uint64_t>(parsed);
  return true;
}

bool storage_probe_increment(uint64_t* value) {
  uint64_t current = 0;
  if (!storage_probe_read(&current) || current == UINT64_MAX) return false;
  char buffer[32];
  const int length = snprintf(buffer, sizeof(buffer), "%llu\n",
                              static_cast<unsigned long long>(current) + 1);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(buffer)) return false;
  const int fd = open(kTemp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    fortis::base::LogErrno(fortis::base::LogLevel::kError, "probe", "storage probe: open temp", errno);
    return false;
  }
  const bool write_ok = write_all(fd, buffer, static_cast<size_t>(length));
  if (!write_ok) fortis::base::LogErrno(fortis::base::LogLevel::kError, "probe", "storage probe: write temp", errno);
  const bool sync_ok = write_ok && fsync(fd) == 0;
  if (write_ok && !sync_ok) fortis::base::LogErrno(fortis::base::LogLevel::kError, "probe", "storage probe: fsync temp", errno);
  const int close_result = close(fd);
  if (close_result != 0) fortis::base::LogErrno(fortis::base::LogLevel::kError, "probe", "storage probe: close temp", errno);
  if (!sync_ok || close_result != 0) return false;
  if (rename(kTemp, kFile) != 0) {
    fortis::base::LogErrno(fortis::base::LogLevel::kError, "probe", "storage probe: rename", errno);
    return false;
  }
  const int dir = open(kDir, O_RDONLY | O_DIRECTORY);
  if (dir < 0) {
    fortis::base::LogErrno(fortis::base::LogLevel::kError, "probe", "storage probe: open dir", errno);
    return false;
  }
  const bool dir_ok = fsync(dir) == 0;
  if (!dir_ok) fortis::base::LogErrno(fortis::base::LogLevel::kError, "probe", "storage probe: fsync dir", errno);
  const int dir_close_result = close(dir);
  if (dir_close_result != 0) fortis::base::LogErrno(fortis::base::LogLevel::kError, "probe", "storage probe: close dir", errno);
  if (!dir_ok || dir_close_result != 0) return false;
  *value = current + 1;
  return true;
}

}  // namespace fortis::storage
