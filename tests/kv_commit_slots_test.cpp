#include <algorithm>
#include "test_support.h"
// Commit-slot durability (KvDurability::CommitSlots): on-disk slot format and
// flush budget, unpublished tails, append and slot failure boundaries,
// SIGKILL after every append stage, slot damage, wrong-mode and migration
// safety, compaction and old-generation recovery, and group-commit
// conditions. Linked with the FORTIS_KV_TEST_FAILURES store object.
#include "../src/storage/kv_store.h"

#include <atomic>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using fortis::storage::KvDurability;
using fortis::storage::KvMutation;
using fortis::storage::KvReadStatus;
using fortis::storage::KvStore;

constexpr KvDurability kSlots = KvDurability::CommitSlots;
constexpr KvDurability kMarker = KvDurability::RollbackMarker;
constexpr uint64_t kSlotBytes = 4096;
constexpr uint64_t kLogBase = 2 * kSlotBytes;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
      abort();                                                                  \
    }                                                                           \
  } while (0)

std::string MakeDirectory() {
  char path[] = "/tmp/fortis-kv-slots-XXXXXX";
  CHECK(mkdtemp(path));
  return path;
}

std::string LogPath(const std::string& directory) { return directory + "/fortis.kv"; }

bool Exists(const std::string& path) { return access(path.c_str(), F_OK) == 0; }

void RemoveDirectory(const std::string& directory) {
  for (const char* name : {"fortis.kv", "fortis.kv.lock", "fortis.kv.compact",
                           "fortis.kv.recover", "fortis.kv.recover.tmp"})
    unlink((directory + "/" + name).c_str());
  CHECK(rmdir(directory.c_str()) == 0);
}

std::string ReadFile(const std::string& path) {
  std::string bytes;
  FILE* file = fopen(path.c_str(), "rb");
  CHECK(file);
  char buffer[65536];
  size_t n;
  while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0) bytes.append(buffer, n);
  CHECK(fclose(file) == 0);
  return bytes;
}

void WriteFile(const std::string& path, const std::string& bytes) {
  FILE* file = fopen(path.c_str(), "wb");
  CHECK(file && fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
  CHECK(fclose(file) == 0);
}

ino_t Inode(const std::string& path) {
  struct stat info {};
  CHECK(stat(path.c_str(), &info) == 0);
  return info.st_ino;
}

uint32_t Crc(const char* data, size_t length) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint8_t>(data[i]);
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

uint64_t Le(const char* in, unsigned bytes) {
  uint64_t value = 0;
  for (unsigned i = 0; i < bytes; ++i)
    value |= static_cast<uint64_t>(static_cast<uint8_t>(in[i])) << (8 * i);
  return value;
}

void PutLe(char* out, uint64_t value, unsigned bytes) {
  for (unsigned i = 0; i < bytes; ++i) out[i] = static_cast<char>(value >> (8 * i));
}

// Independent decoder for the documented slot layout.
struct Slot {
  bool valid = false;
  uint64_t generation = 0, sequence = 0, committed = 0;
};

Slot DecodeSlot(const std::string& file, unsigned index) {
  Slot slot;
  if (file.size() < kLogBase) return slot;
  const char* block = file.data() + index * kSlotBytes;
  if (memcmp(block, "FKVS", 4) != 0 || Le(block + 4, 4) != 1 || Le(block + 32, 8) != kLogBase ||
      Le(block + 4092, 4) != Crc(block, 4092))
    return slot;
  for (size_t i = 40; i < 4092; ++i)
    if (block[i]) return slot;  // Padding is zero.
  slot.generation = Le(block + 8, 8);
  slot.sequence = Le(block + 16, 8);
  slot.committed = Le(block + 24, 8);
  slot.valid = (slot.sequence & 1) == index;
  return slot;
}

void EncodeSlot(std::string* file, const Slot& slot) {
  char* block = &(*file)[(slot.sequence & 1) * kSlotBytes];
  memset(block, 0, kSlotBytes);
  std::copy_n("FKVS", 4, block);
  PutLe(block + 4, 1, 4);
  PutLe(block + 8, slot.generation, 8);
  PutLe(block + 16, slot.sequence, 8);
  PutLe(block + 24, slot.committed, 8);
  PutLe(block + 32, kLogBase, 8);
  PutLe(block + 4092, Crc(block, 4092), 4);
}

Slot Newest(const std::string& file) {
  const Slot a = DecodeSlot(file, 0), b = DecodeSlot(file, 1);
  if (!a.valid) return b;
  if (!b.valid) return a;
  return a.sequence > b.sequence ? a : b;
}

Slot NewestOnDisk(const std::string& directory) { return Newest(ReadFile(LogPath(directory))); }

// A complete, checksummed one-record frame, as a failed flush can leave in
// the page cache.
std::string Frame(const char* key, const char* value) {
  const size_t key_length = strlen(key), value_length = strlen(value);
  std::string body(4 + 15 + key_length + value_length, '\0');
  PutLe(&body[0], 1, 4);
  body[4] = 0;
  PutLe(&body[5], key_length, 2);
  PutLe(&body[7], value_length, 4);
  memcpy(&body[19], key, key_length);
  memcpy(&body[19 + key_length], value, value_length);
  std::string frame(12, '\0');
  memcpy(&frame[0], "FKV1", 4);
  PutLe(&frame[4], body.size(), 4);
  PutLe(&frame[8], Crc(body.data(), body.size()), 4);
  return frame + body;
}

void Put(KvStore& store, const char* key, const char* value) {
  const KvMutation mutation{key, value, static_cast<uint32_t>(strlen(value)), 0, false};
  CHECK(store.Write(&mutation, 1));
}

KvReadStatus Status(KvStore& store, const char* key) {
  char value[64];
  size_t size = 0;
  return store.GetStatus(key, 0, value, sizeof(value), &size);
}

void Expect(KvStore& store, const char* key, const char* expected) {
  char value[64]{};
  size_t size = 0;
  CHECK(store.GetStatus(key, 0, value, sizeof(value), &size) == KvReadStatus::Found);
  CHECK(size == strlen(expected) && memcmp(value, expected, size) == 0);
}

void ExpectMissing(KvStore& store, const char* key) {
  CHECK(Status(store, key) == KvReadStatus::Missing);
}

uint64_t FileSyncs(const KvStore& store) {
  fortis::storage::KvActivityStats activity;
  store.ActivityStats(activity);
  uint64_t total = 0;
  for (uint64_t count : activity.phases[static_cast<size_t>(
           fortis::storage::KvActivityStats::Phase::FileSync)])
    total += count;
  return total;
}

void Fail(const char* stage) { CHECK(setenv("FORTIS_KV_TEST_FAIL", stage, 1) == 0); }
void ClearFail() { CHECK(unsetenv("FORTIS_KV_TEST_FAIL") == 0); }

// Runs `body` in a child that must die by SIGKILL at `point`.
template <typename Body>
void KilledChild(const char* point, const char* failure, Body body) {
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    alarm(30);
    setenv("FORTIS_KV_TEST_KILL", point, 1);
    if (failure) setenv("FORTIS_KV_TEST_FAIL", failure, 1);
    body();
    _exit(7);
  }
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child);
  if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
    fprintf(stderr, "kill point %s not reached (status=%d)\n", point, status);
    CHECK(false);
  }
}

// ---- Format and flush budget ------------------------------------------------

void TestFormatAndFlushBudget() {
  const std::string directory = MakeDirectory();
  {
    KvStore store(false, false, kSlots);
    CHECK(store.Durability() == kSlots);
    CHECK(store.Open(directory.c_str()));
    // A fresh log is born with both slots naming an empty log.
    const std::string fresh = ReadFile(LogPath(directory));
    CHECK(fresh.size() == kLogBase);
    const Slot a = DecodeSlot(fresh, 0), b = DecodeSlot(fresh, 1);
    CHECK(a.valid && b.valid && a.generation == 1 && b.generation == 1);
    CHECK(a.committed == kLogBase && b.committed == kLogBase);
    const Slot start = Newest(fresh);

    const uint64_t syncs = FileSyncs(store);
    char key[16];
    for (unsigned i = 0; i < 8; ++i) {
      snprintf(key, sizeof(key), "key/%u", i);
      Put(store, key, "value");
    }
    // Exactly two flushes per frame and no directory sync.
    CHECK(FileSyncs(store) - syncs == 16);
    const std::string file = ReadFile(LogPath(directory));
    const Slot newer = Newest(file);
    const Slot older = DecodeSlot(file, (newer.sequence & 1) ^ 1);
    CHECK(newer.generation == 1 && newer.sequence == start.sequence + 8);
    CHECK(newer.committed == file.size());
    CHECK(older.valid && older.sequence + 1 == newer.sequence && older.committed < newer.committed);
    CHECK(store.WriteFrames() == 8);
  }
  {
    // Reopening publishes the same frames on a fresh generation.
    const std::string before = ReadFile(LogPath(directory));
    KvStore store(false, false, kSlots);
    CHECK(store.Open(directory.c_str()));
    const std::string after = ReadFile(LogPath(directory));
    CHECK(Newest(after).generation == Newest(before).generation + 1);
    CHECK(after.substr(kLogBase) == before.substr(kLogBase));
    Expect(store, "key/7", "value");
  }
  const std::string legacy_directory = MakeDirectory();
  {
    // The default protocol for comparison: marker file sync, two directory
    // syncs and the log sync.
    KvStore store;
    CHECK(store.Durability() == kMarker);
    CHECK(store.Open(legacy_directory.c_str()));
    const uint64_t syncs = FileSyncs(store);
    Put(store, "key", "value");
    CHECK(FileSyncs(store) - syncs == 4);
  }
  RemoveDirectory(legacy_directory);
  RemoveDirectory(directory);
  puts("format and flush budget: ok");
}

// ---- Unpublished tails ------------------------------------------------------

void TestUnpublishedTail() {
  const std::string directory = MakeDirectory();
  const std::string path = LogPath(directory);
  {
    KvStore store(false, false, kSlots);
    CHECK(store.Open(directory.c_str()));
    Put(store, "ack", "first");
  }
  const std::string published = ReadFile(path);
  // A complete, valid frame past the published end is never replayed.
  WriteFile(path, published + Frame("ghost", "cached"));
  {
    KvStore store(false, false, kSlots);
    CHECK(store.Open(directory.c_str()));
    ExpectMissing(store, "ghost");
    Expect(store, "ack", "first");
    CHECK(ReadFile(path).substr(kLogBase) == published.substr(kLogBase));
    Put(store, "next", "kept");
  }
  {
    KvStore store(false, false, kSlots);
    CHECK(store.Open(directory.c_str()));
    Expect(store, "next", "kept");
    ExpectMissing(store, "ghost");
  }
  RemoveDirectory(directory);
  puts("unpublished complete frame ignored and dropped: ok");
}

// ---- Append failure boundaries ---------------------------------------------

struct FailureCase {
  const char* stage;
  bool new_inode;   // Recovery retires the inode and rebuilds the prefix.
  bool frame_kept;  // The frame's own flush succeeded.
};

void TestAppendFailures() {
  const FailureCase cases[] = {
      {"write_after_header", false, false},  // Body write EIO.
      {"write_body_no_space", false, false},
      {"write_before_fsync", true, false},   // Log flush EIO.
      {"file_fsync", true, false},
      {"commit_slot_write", true, true},
      {"commit_slot_fsync", true, true},
      {"write_after_fsync", false, true},    // Index update after publication.
  };
  int64_t clock = 0;
  for (const FailureCase& test : cases) {
    const std::string directory = MakeDirectory();
    const std::string path = LogPath(directory);
    KvStore store(false, false, kSlots);
    CHECK(store.Open(directory.c_str()));
    Put(store, "ack", "first");
    const ino_t inode = Inode(path);
    const Slot before = NewestOnDisk(directory);

    const KvMutation pending{"pending", "maybe", 5, 0, false};
    Fail(test.stage);
    CHECK(!store.Write(&pending, 1));
    ClearFail();
    // Suspended until recovery: nothing more is acknowledged.
    CHECK(Status(store, "ack") == KvReadStatus::IoError);
    CHECK(!store.Write(&pending, 1));
    fortis::storage::KvStore competitor(false, false, kSlots);
    CHECK(!competitor.Open(directory.c_str()));

    clock += 100000;
    CHECK(store.RecoverIfNeeded(clock));
    Expect(store, "ack", "first");
    if (test.frame_kept) Expect(store, "pending", "maybe");
    else ExpectMissing(store, "pending");
    const Slot after = NewestOnDisk(directory);
    CHECK((Inode(path) != inode) == test.new_inode);
    CHECK(after.generation == before.generation + (test.new_inode ? 1 : 0));
    CHECK(after.sequence >= before.sequence && after.committed == ReadFile(path).size());

    Put(store, "after", "kept");
    store.Close();
    CHECK(store.Open(directory.c_str()));
    Expect(store, "ack", "first");
    Expect(store, "after", "kept");
    if (test.frame_kept) Expect(store, "pending", "maybe");
    else ExpectMissing(store, "pending");
    store.Close();
    RemoveDirectory(directory);
    printf("append failure %-20s: recovered (new inode=%d, frame kept=%d)\n", test.stage,
           test.new_inode, test.frame_kept);
  }

  // A rebuild whose directory sync fails retries into a newer generation.
  const std::string directory = MakeDirectory();
  KvStore store(false, false, kSlots);
  CHECK(store.Open(directory.c_str()));
  Put(store, "ack", "first");
  const Slot before = NewestOnDisk(directory);
  const KvMutation lost{"lost", "x", 1, 0, false};
  Fail("file_fsync");
  CHECK(!store.Write(&lost, 1));
  Fail("recovery_directory_fsync");
  CHECK(!store.RecoverIfNeeded(0));
  ClearFail();
  CHECK(NewestOnDisk(directory).generation == before.generation + 1);
  CHECK(Status(store, "ack") == KvReadStatus::IoError);
  CHECK(store.RecoverIfNeeded(5000));
  CHECK(NewestOnDisk(directory).generation == before.generation + 2);
  Expect(store, "ack", "first");
  ExpectMissing(store, "lost");
  store.Close();

  // An interrupted restart leaves the authoritative inode unchanged until
  // the fresh copy and its two slots have synced.
  CHECK(store.Open(directory.c_str()));
  Put(store, "latest", "kept");
  store.Close();
  const std::string path = LogPath(directory);
  const ino_t inode = Inode(path);
  const std::string previous = ReadFile(path);
  Fail("recovery_file_fsync");
  CHECK(!store.Open(directory.c_str()));
  ClearFail();
  CHECK(Inode(path) == inode && ReadFile(path) == previous);
  KilledChild("rebuild_synced", nullptr, [&] {
    KvStore child(false, false, kSlots);
    child.Open(directory.c_str());
  });
  CHECK(Inode(path) == inode && ReadFile(path) == previous);
  KilledChild("rebuild_renamed", nullptr, [&] {
    KvStore child(false, false, kSlots);
    child.Open(directory.c_str());
  });
  CHECK(Inode(path) != inode);
  CHECK(store.Open(directory.c_str()));
  Expect(store, "ack", "first");
  Expect(store, "latest", "kept");
  ExpectMissing(store, "lost");
  store.Close();
  RemoveDirectory(directory);
  puts("append, slot, rebuild and open publication failures: ok");
}

// ---- SIGKILL after each append stage ---------------------------------------

struct KillCase {
  const char* point;
  const char* failure;
  bool present;  // Process kill keeps the page cache: a written slot is read.
};

void TestKillBoundaries() {
  const KillCase cases[] = {
      {"write_after_header", nullptr, false},
      {"write_before_fsync", nullptr, false},  // Complete cached frame.
      {"commit_frame_synced", nullptr, false},
      {"commit_slot_written", nullptr, true},
      {"write_acknowledged", nullptr, true},
      {"append_failed_before_recovery", "file_fsync", false},
      {"append_failed_before_recovery", "commit_slot_write", false},
      {"append_failed_before_recovery", "commit_slot_fsync", true},
  };
  for (const KillCase& test : cases) {
    const std::string directory = MakeDirectory();
    {
      KvStore store(false, false, kSlots);
      CHECK(store.Open(directory.c_str()));
      Put(store, "ack", "first");
    }
    KilledChild(test.point, test.failure, [&] {
      KvStore store(false, false, kSlots);
      if (!store.Open(directory.c_str())) _exit(2);
      const KvMutation crashed{"crashed", "frame", 5, 0, false};
      store.Write(&crashed, 1);
    });
    KvStore store(false, false, kSlots);
    CHECK(store.Open(directory.c_str()));
    Expect(store, "ack", "first");
    if (test.present) Expect(store, "crashed", "frame");
    else ExpectMissing(store, "crashed");
    // The reopened log holds exactly the published frames.
    CHECK(NewestOnDisk(directory).committed == ReadFile(LogPath(directory)).size());
    Put(store, "post-crash", "kept");
    store.Close();
    CHECK(store.Open(directory.c_str()));
    Expect(store, "post-crash", "kept");
    store.Close();
    RemoveDirectory(directory);
    printf("kill %-30s %-18s: crashed frame %s\n", test.point,
           test.failure ? test.failure : "", test.present ? "published" : "discarded");
  }
}

// ---- Slot damage --------------------------------------------------------------

void TestSlotDamage() {
  const std::string directory = MakeDirectory();
  const std::string path = LogPath(directory);
  {
    KvStore store(false, false, kSlots);
    CHECK(store.Open(directory.c_str()));
    Put(store, "a", "1");
    Put(store, "b", "2");
  }
  const std::string original = ReadFile(path);
  const Slot newer = Newest(original);
  const Slot older = DecodeSlot(original, (newer.sequence & 1) ^ 1);
  CHECK(older.valid && older.generation == newer.generation);

  // A torn newest slot leaves the previous commit in its own block.
  std::string torn = original;
  torn[(newer.sequence & 1) * kSlotBytes + 2000] ^= 0x5a;
  WriteFile(path, torn);
  {
    KvStore store(false, false, kSlots);
    CHECK(store.Open(directory.c_str()));
    Expect(store, "a", "1");
    ExpectMissing(store, "b");
    CHECK(ReadFile(path).size() == older.committed);
    // The next commit takes the damaged block's sequence parity again.
    Put(store, "c", "3");
    const std::string file = ReadFile(path);
    CHECK(DecodeSlot(file, 0).valid && DecodeSlot(file, 1).valid);
  }

  // Each of these is refused and the file is left exactly as found.
  std::vector<std::string> refused;
  std::string both = original;
  both[2000] ^= 0x5a;
  both[kSlotBytes + 2000] ^= 0x5a;
  refused.push_back(both);
  std::string foreign = original;  // Older slot from another generation.
  EncodeSlot(&foreign, {true, older.generation + 5, older.sequence, older.committed});
  refused.push_back(foreign);
  std::string beyond = original;  // Names bytes the file does not have.
  EncodeSlot(&beyond, {true, newer.generation, newer.sequence, original.size() + 100});
  refused.push_back(beyond);
  std::string inside = original;  // Ends inside a frame.
  EncodeSlot(&inside, {true, newer.generation, newer.sequence, older.committed + 3});
  refused.push_back(inside);
  std::string backwards = original;  // Newer slot behind the older one.
  EncodeSlot(&backwards, {true, newer.generation, newer.sequence, kLogBase});
  refused.push_back(backwards);
  for (const std::string& bytes : refused) {
    WriteFile(path, bytes);
    KvStore store(false, false, kSlots);
    CHECK(!store.Open(directory.c_str()));
    CHECK(ReadFile(path) == bytes);
  }
  RemoveDirectory(directory);
  puts("torn newest slot falls back; inconsistent slots fail closed: ok");
}

// ---- Modes and migration ---------------------------------------------------------

void TestModesAndMigration() {
  const std::string directory = MakeDirectory();
  const std::string path = LogPath(directory);
  {
    KvStore store;
    CHECK(store.Open(directory.c_str()));
    Put(store, "a", "1");
    Put(store, "b", "2");
  }
  const std::string legacy = ReadFile(path);
  CHECK(legacy.compare(0, 4, "FKV1") == 0);
  std::string invalid = legacy;
  invalid.back() ^= 1;
  WriteFile(path, invalid);
  {
    KvStore store(false, false, kSlots);
    CHECK(!store.Open(directory.c_str()));
    CHECK(ReadFile(path) == invalid);
  }
  std::string torn = Frame("unfinished", "value");
  torn.pop_back();
  WriteFile(path, legacy + torn);
  KilledChild("rebuild_synced", nullptr, [&] {
    KvStore store(false, false, kSlots);
    store.Open(directory.c_str());
  });
  CHECK(ReadFile(path) == legacy + torn);
  {
    // Opting in migrates once; the frames are copied verbatim.
    KvStore store(false, false, kSlots);
    CHECK(store.Open(directory.c_str()));
    Expect(store, "a", "1");
    Expect(store, "b", "2");
    ExpectMissing(store, "unfinished");
    const std::string migrated = ReadFile(path);
    CHECK(migrated.size() == kLogBase + legacy.size());
    CHECK(migrated.compare(kLogBase, std::string::npos, legacy) == 0);
    CHECK(Newest(migrated).generation == 1 && Newest(migrated).committed == migrated.size());
    Put(store, "c", "3");
  }
  {
    // The default mode, like a runtime predating commit slots, refuses it.
    const std::string before = ReadFile(path);
    KvStore store;
    CHECK(!store.Open(directory.c_str()));
    CHECK(ReadFile(path) == before);
  }
  {
    KvStore store(false, false, kSlots);
    CHECK(store.Open(directory.c_str()));
    Expect(store, "c", "3");
  }
  RemoveDirectory(directory);

  // A pending rollback marker is resolved under its own rules first.
  const std::string pending = MakeDirectory();
  {
    KvStore store;
    CHECK(store.Open(pending.c_str()));
    Put(store, "ack", "first");
    const KvMutation lost{"lost", "x", 1, 0, false};
    Fail("file_fsync");
    CHECK(!store.Write(&lost, 1));
    ClearFail();
  }
  CHECK(Exists(pending + "/fortis.kv.recover"));
  {
    KvStore store(false, false, kSlots);
    CHECK(store.Open(pending.c_str()));
    Expect(store, "ack", "first");
    ExpectMissing(store, "lost");
    CHECK(!Exists(pending + "/fortis.kv.recover"));
    CHECK(Newest(ReadFile(LogPath(pending))).valid);
  }
  RemoveDirectory(pending);

  // An unrecognized file is left alone by both modes.
  const std::string unknown = MakeDirectory();
  WriteFile(LogPath(unknown), std::string("not a log at all") + std::string(9000, 'z'));
  for (KvDurability durability : {kSlots, kMarker}) {
    KvStore store(false, false, durability);
    CHECK(!store.Open(unknown.c_str()));
  }
  CHECK(ReadFile(LogPath(unknown)).compare(0, 16, "not a log at all") == 0);
  RemoveDirectory(unknown);

  // A crash before the migration's rename keeps the legacy log authoritative;
  // after it, the migrated log is.
  for (const char* point : {"rebuild_synced", "rebuild_renamed"}) {
    const std::string crashed = MakeDirectory();
    {
      KvStore store;
      CHECK(store.Open(crashed.c_str()));
      Put(store, "a", "1");
    }
    KilledChild(point, nullptr, [&] {
      KvStore store(false, false, kSlots);
      store.Open(crashed.c_str());
    });
    const bool renamed = strcmp(point, "rebuild_renamed") == 0;
    CHECK(ReadFile(LogPath(crashed)).compare(0, 4, renamed ? "FKVS" : "FKV1") == 0);
    {
      KvStore store(false, false, renamed ? kSlots : kMarker);
      CHECK(store.Open(crashed.c_str()));
      CHECK(!Exists(crashed + "/fortis.kv.compact"));
      Expect(store, "a", "1");
    }
    KvStore store(false, false, kSlots);
    CHECK(store.Open(crashed.c_str()));
    Expect(store, "a", "1");
    store.Close();
    RemoveDirectory(crashed);
  }
  puts("wrong mode fails closed; migration, pending marker and migration crashes: ok");
}

// ---- Compaction and old generations -----------------------------------------

void Fill(KvStore& store, unsigned rounds) {
  char key[32], value[32];
  for (unsigned round = 0; round < rounds; ++round)
    for (unsigned i = 0; i < 64; ++i) {
      snprintf(key, sizeof(key), "row/%03u", i);
      snprintf(value, sizeof(value), "v%u-%u", round, i);
      Put(store, key, value);
    }
}

void ExpectFilled(KvStore& store, unsigned rounds) {
  char key[32], value[32];
  for (unsigned i = 0; i < 64; ++i) {
    snprintf(key, sizeof(key), "row/%03u", i);
    snprintf(value, sizeof(value), "v%u-%u", rounds - 1, i);
    Expect(store, key, value);
  }
}

void TestCompaction() {
  const std::string directory = MakeDirectory();
  const std::string path = LogPath(directory);
  KvStore store(false, false, kSlots);
  CHECK(store.Open(directory.c_str()));
  Fill(store, 4);
  const std::string old_generation = ReadFile(path);
  const Slot before = Newest(old_generation);
  const ino_t inode = Inode(path);

  CHECK(store.Compact());
  const std::string compacted = ReadFile(path);
  const Slot after = Newest(compacted);
  const Slot other = DecodeSlot(compacted, (after.sequence & 1) ^ 1);
  CHECK(Inode(path) != inode && compacted.size() < old_generation.size());
  CHECK(after.generation == before.generation + 1 && after.sequence > before.sequence);
  CHECK(other.valid && other.generation == after.generation && other.committed == after.committed);
  CHECK(after.committed == compacted.size());
  ExpectFilled(store, 4);
  Put(store, "after", "compaction");
  store.Close();

  // The pre-swap generation still opens on its own: a rename that never
  // became durable brings back a complete, consistent log.
  WriteFile(path, old_generation);
  CHECK(store.Open(directory.c_str()));
  ExpectFilled(store, 4);
  ExpectMissing(store, "after");
  CHECK(NewestOnDisk(directory).generation == before.generation + 1);
  store.Close();

  // A failed slot write for the new generation aborts before the rename.
  CHECK(store.Open(directory.c_str()));
  const Slot before_failed_compaction = NewestOnDisk(directory);
  Fail("compact_slot_write");
  CHECK(!store.Compact());
  ClearFail();
  CHECK(!Exists(directory + "/fortis.kv.compact"));
  CHECK(NewestOnDisk(directory).generation == before_failed_compaction.generation);
  Put(store, "still", "writable");
  store.Close();

  // SIGKILL around the swap: before the rename the old generation is in
  // service; after it, the new one is.
  for (const char* point : {"swap_tail_copied", "swap_file_synced", "swap_renamed",
                            "swap_directory_synced"}) {
    const Slot prior = NewestOnDisk(directory);
    KilledChild(point, nullptr, [&] {
      KvStore child(false, false, kSlots);
      if (!child.Open(directory.c_str())) _exit(2);
      Fill(child, 1);
      child.Compact();
    });
    const bool swapped = strncmp(point, "swap_renamed", 12) == 0 ||
                         strcmp(point, "swap_directory_synced") == 0;
    const Slot now = NewestOnDisk(directory);
    CHECK(now.generation == prior.generation + (swapped ? 2 : 1));
    CHECK(store.Open(directory.c_str()));
    CHECK(!Exists(directory + "/fortis.kv.compact"));
    ExpectFilled(store, 1);
    Expect(store, "still", "writable");
    store.Close();
    printf("compaction kill %-22s: generation %llu\n", point,
           static_cast<unsigned long long>(now.generation));
  }
  RemoveDirectory(directory);
  puts("compaction generations, old-generation reopen and swap kills: ok");
}

// ---- Group commit ------------------------------------------------------------------

void TestGroupCommit() {
  const std::string directory = MakeDirectory();
  KvStore store(true, true, kSlots);
  CHECK(store.Open(directory.c_str()));
  Put(store, "counter", "0");
  const uint64_t syncs = FileSyncs(store), frames = store.WriteFrames();
  constexpr unsigned kThreads = 8, kIncrements = 40;
  std::atomic<unsigned> conflicts{0};
  std::vector<std::thread> threads;
  for (unsigned t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      for (unsigned done = 0; done < kIncrements;) {
        char current[16]{}, next[16];
        size_t size = 0;
        CHECK(store.Get("counter", 0, current, sizeof(current) - 1, &size));
        const KvMutation mutation{"counter", next,
                                  static_cast<uint32_t>(snprintf(next, sizeof(next), "%lu",
                                                                 strtoul(current, nullptr, 10) + 1)),
                                  0, false};
        const fortis::storage::KvExpectation expected{"counter", current, static_cast<uint32_t>(size)};
        const auto result = store.CompareAndWrite(expected, 0, &mutation, 1);
        if (result == fortis::storage::KvCompareStatus::Committed) {
          ++done;
        } else {
          CHECK(result == fortis::storage::KvCompareStatus::ValueMismatch);
          ++conflicts;
        }
      }
    });
  }
  for (auto& thread : threads) thread.join();
  char expected[16];
  snprintf(expected, sizeof(expected), "%u", kThreads * kIncrements);
  Expect(store, "counter", expected);
  const uint64_t written = store.WriteFrames() - frames;
  CHECK(written >= 1 && written <= kThreads * kIncrements);
  CHECK(FileSyncs(store) - syncs == 2 * written);
  store.Close();
  CHECK(store.Open(directory.c_str()));
  Expect(store, "counter", expected);
  store.Close();
  RemoveDirectory(directory);
  printf("group commit: %u increments in %llu frames, %u conflicts: ok\n",
         kThreads * kIncrements, static_cast<unsigned long long>(written), conflicts.load());
}

}  // namespace

int main() try {
  TestFormatAndFlushBudget();
  TestUnpublishedTail();
  TestAppendFailures();
  TestKillBoundaries();
  TestSlotDamage();
  TestModesAndMigration();
  TestCompaction();
  TestGroupCommit();
  puts("native KV commit-slot durability test passed");
  return 0;
} catch (...) {
  return fortis::test::ReportException();
}
