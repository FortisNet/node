#include <algorithm>
#include "ownership.h"
#include <atomic>
// The KV log's on-disk format: little-endian fields, the frame checksum,
// whole-buffer file I/O and the commit slots.
#include "kv_store_internal.h"

namespace fortis::storage {

namespace {

constexpr uint32_t kSlotVersion = 1;
constexpr size_t kSlotChecksumOffset = kSlotBytes - 4;

void EncodeSlot(char* block, const CommitSlot& slot) {
  memset(block, 0, kSlotBytes);
  std::copy_n("FKVS", 4, block);
  Put32(block + 4, kSlotVersion);
  Put64(block + 8, slot.generation);
  Put64(block + 16, slot.sequence);
  Put64(block + 24, slot.committed);
  Put64(block + 32, kCommitLogBase);
  Put32(block + kSlotChecksumOffset, Checksum(block, kSlotChecksumOffset));
}

bool DecodeSlot(const char* block, uint64_t index, CommitSlot* slot) {
  if (memcmp(block, "FKVS", 4) != 0 || Read32(block + 4) != kSlotVersion ||
      Read32(block + kSlotChecksumOffset) != Checksum(block, kSlotChecksumOffset))
    return false;
  slot->generation = Read64(block + 8);
  slot->sequence = Read64(block + 16);
  slot->committed = Read64(block + 24);
  return slot->generation && slot->sequence && (slot->sequence & 1) == index &&
         Read64(block + 32) == kCommitLogBase && slot->committed >= kCommitLogBase;
}

}  // namespace

namespace kv_internal {

bool WriteAll(int fd, const char* data, size_t length, uint64_t offset) {
  while (length) {
    const ssize_t n = pwrite(fd, data, length, static_cast<off_t>(offset));
    if (n < 0 && errno == EINTR) continue;
    if (n == 0) errno = EIO;
    if (n <= 0) return false;
    data += n; length -= static_cast<size_t>(n); offset += static_cast<uint64_t>(n);
  }
  return true;
}
bool ReadAll(int fd, char* data, size_t length, uint64_t offset, KvAtomicActivityStats& activity) {
  const uint64_t started = MonotonicUs();
  bool ok = true;
  while (length) {
    const ssize_t n = pread(fd, data, length, static_cast<off_t>(offset));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { ok = false; break; }
    data += n; length -= static_cast<size_t>(n); offset += static_cast<uint64_t>(n);
  }
  const int error = errno;
  RecordPhase(activity, Phase::FileRead, MonotonicUs() - started);
  errno = error;
  return ok;
}
int SyncFile(int fd, KvAtomicActivityStats& activity) {
  const uint64_t started = MonotonicUs();
  const int result = fsync(fd);
  const int error = errno;
  RecordPhase(activity, Phase::FileSync, MonotonicUs() - started);
  errno = error;
  return result;
}
bool SyncDirectory(const char* directory, KvAtomicActivityStats& activity) {
  const int fd = open(directory, O_RDONLY | O_DIRECTORY);
  if (fd < 0) return false;
  const bool synced = SyncFile(fd, activity) == 0;
  const int sync_error = errno;
  const bool closed = close(fd) == 0;
  if (!synced) errno = sync_error;
  return synced && closed;
}

// Picks the newest valid slot. One torn slot is expected after a crash during
// its write; two valid slots must describe the same file and a committed end
// that never moves backwards, otherwise the header is not ours to trust.
bool SelectSlot(const char* header, CommitSlot* selected) {
  CommitSlot slots[2];
  const bool valid[2] = {DecodeSlot(header, 0, &slots[0]),
                         DecodeSlot(header + kSlotBytes, 1, &slots[1])};
  if (!valid[0] && !valid[1]) return false;
  if (valid[0] != valid[1]) {
    *selected = slots[valid[0] ? 0 : 1];
    return true;
  }
  const CommitSlot& newer = slots[0].sequence > slots[1].sequence ? slots[0] : slots[1];
  const CommitSlot& older = &newer == &slots[0] ? slots[1] : slots[0];
  if (newer.generation != older.generation || newer.committed < older.committed) return false;
  *selected = newer;
  return true;
}

bool ReadCommitSlot(int fd, uint64_t size, KvAtomicActivityStats& activity, CommitSlot* slot) {
  char header[kCommitLogBase];
  if (size < kCommitLogBase) { errno = EINVAL; return false; }
  if (!ReadAll(fd, header, sizeof(header), 0, activity)) return false;
  if (!SelectSlot(header, slot) || slot->committed > size) {
    errno = EINVAL;
    return false;
  }
  return true;
}

bool WriteSlot(int fd, const CommitSlot& slot) {
  char block[kSlotBytes];
  EncodeSlot(block, slot);
  return WriteAll(fd, block, sizeof(block), (slot.sequence & 1) * kSlotBytes);
}

LogFormat DetectLogFormat(const char* path, KvAtomicActivityStats& activity) {
  const int fd = open(path, O_RDONLY);
  if (fd < 0) return errno == ENOENT ? LogFormat::Missing : LogFormat::Unknown;
  struct stat info {};
  LogFormat format = LogFormat::Unknown;
  char magic[4];
  if (fstat(fd, &info) == 0 && info.st_size >= 0) {
    const uint64_t size = static_cast<uint64_t>(info.st_size);
    if (size < sizeof(magic)) {
      format = LogFormat::RollbackMarker;
    } else if (ReadAll(fd, magic, sizeof(magic), 0, activity)) {
      if (memcmp(magic, "FKV1", 4) == 0) {
        format = LogFormat::RollbackMarker;
      } else if (size >= kCommitLogBase) {
        auto header_storage = base::Buffer::Allocate(kCommitLogBase);
        char* header = header_storage.chars();
        CommitSlot slot;
        if (header && ReadAll(fd, header, kCommitLogBase, 0, activity) &&
            (DecodeSlot(header, 0, &slot) || DecodeSlot(header + kSlotBytes, 1, &slot)))
          format = LogFormat::CommitSlots;
        header_storage.reset();
      }
    }
  }
  close(fd);
  return format;
}

}  // namespace kv_internal

}  // namespace fortis::storage
