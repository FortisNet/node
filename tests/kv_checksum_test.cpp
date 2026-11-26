#include "test_support.h"
// KV checksum compatibility. KvChecksum (a compile-time CRC-32 table) must
// equal the bit-at-a-time CRC it replaced on every input, and the store must
// keep writing and replaying the exact bytes of the earlier runtime for both
// log formats: rollback-marker frames, commit slots, the legacy-to-slots
// migration and compacted logs. The golden layouts below were captured with
// `kv_checksum_test --dump` linked against the bitwise runtime (16781f54).
#include "../src/storage/kv_checksum.h"
#include "../src/storage/kv_store.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>

namespace {

using fortis::storage::KvChecksum;
using fortis::storage::KvDurability;
using fortis::storage::KvMutation;
using fortis::storage::KvReadStatus;
using fortis::storage::KvStore;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
      abort();                                                                  \
    }                                                                           \
  } while (0)

// The pre-table implementation, verbatim.
uint32_t BitwiseChecksum(const char* data, size_t length) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint8_t>(data[i]);
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

constexpr char kFox[] = "The quick brown fox jumps over the lazy dog";
static_assert(KvChecksum(kFox, sizeof(kFox) - 1) == 0x414fa339u, "CRC-32 of the fox");
static_assert(KvChecksum("a", 1) == 0xe8b7be43u, "CRC-32 of one byte");

void TestVectors() {
  CHECK(KvChecksum("", 0) == 0u);
  CHECK(KvChecksum("123456789", 9) == 0xcbf43926u);
  CHECK(KvChecksum(kFox, sizeof(kFox) - 1) == 0x414fa339u);
  const char zeros[32] = {};
  CHECK(KvChecksum(zeros, sizeof(zeros)) == 0x190a55adu);
  char ones[32];
  memset(ones, 0xff, sizeof(ones));
  CHECK(KvChecksum(ones, sizeof(ones)) == 0xff6cab0bu);

  // Every single byte value, then every length and alignment up to past a
  // full commit slot, over deterministic pseudo-random bytes.
  for (unsigned value = 0; value < 256; ++value) {
    const char byte = static_cast<char>(value);
    CHECK(KvChecksum(&byte, 1) == BitwiseChecksum(&byte, 1));
  }
  static char buffer[4096 + 128];
  uint32_t state = 0x9e3779b9u;
  for (char& byte : buffer) {
    state = state * 1664525u + 1013904223u;
    byte = static_cast<char>(state >> 24);
  }
  for (size_t offset = 0; offset < 8; ++offset)
    for (size_t length = 0; offset + length <= sizeof(buffer); length += length < 64 ? 1 : 61)
      CHECK(KvChecksum(buffer + offset, length) == BitwiseChecksum(buffer + offset, length));
  CHECK(KvChecksum(buffer, 4092) == BitwiseChecksum(buffer, 4092));
}

// ---- Golden logs ---------------------------------------------------------

std::string MakeDirectory() {
  char path[] = "/tmp/fortis-kv-checksum-XXXXXX";
  CHECK(mkdtemp(path));
  return path;
}

void RemoveDirectory(const std::string& directory) {
  for (const char* name : {"fortis.kv", "fortis.kv.lock", "fortis.kv.compact",
                           "fortis.kv.recover", "fortis.kv.recover.tmp"})
    unlink((directory + "/" + name).c_str());
  CHECK(rmdir(directory.c_str()) == 0);
}

std::string ReadLog(const std::string& directory) {
  std::string bytes;
  FILE* file = fopen((directory + "/fortis.kv").c_str(), "rb");
  CHECK(file);
  char chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), file)) > 0) bytes.append(chunk, n);
  CHECK(fclose(file) == 0);
  return bytes;
}

void WriteLog(const std::string& directory, const std::string& bytes) {
  FILE* file = fopen((directory + "/fortis.kv").c_str(), "wb");
  CHECK(file && fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
  CHECK(fclose(file) == 0);
}

constexpr int64_t kExpiry = 4102444800000;  // 2100-01-01T00:00:00Z
constexpr int64_t kNow = 1767225600000;     // 2026-01-01T00:00:00Z

std::string BetaValue() {
  std::string value(300, '\0');
  for (size_t i = 0; i < value.size(); ++i) value[i] = static_cast<char>((i * 7) & 0xff);
  return value;
}

// One single-record frame, one three-record frame, an erase and a binary
// value: every frame shape the runtime writes.
void Populate(KvStore& store) {
  const KvMutation alpha{"alpha", "one", 3, 0, false};
  CHECK(store.Write(&alpha, 1));
  const std::string beta = BetaValue();
  const KvMutation batch[] = {
      {"beta", beta.data(), static_cast<uint32_t>(beta.size()), 0, false},
      {"gamma", "g", 1, kExpiry, false},
      {"epsilon", "short-lived", 11, kExpiry, false},
  };
  CHECK(store.Write(batch, 3));
  const KvMutation erase{"alpha", nullptr, 0, 0, true};
  CHECK(store.Write(&erase, 1));
  const char delta[] = {'\0', '\xff', '\x10'};
  const KvMutation binary{"delta", delta, sizeof(delta), 0, false};
  CHECK(store.Write(&binary, 1));
}

std::string Read(const KvStore& store, const char* key) {
  char value[512];
  size_t size = 0;
  if (store.GetStatus(key, kNow, value, sizeof(value), &size) != KvReadStatus::Found) return "<missing>";
  return std::string(value, size);
}

void CheckContents(const KvStore& store) {
  CHECK(Read(store, "alpha") == "<missing>");
  CHECK(Read(store, "beta") == BetaValue());
  CHECK(Read(store, "gamma") == "g");
  CHECK(Read(store, "epsilon") == "short-lived");
  CHECK(Read(store, "delta") == std::string("\0\xff\x10", 3));
}

enum class Scenario { Marker, MarkerCompacted, Slots, SlotsCompacted, Migrated };
constexpr Scenario kScenarios[] = {Scenario::Marker, Scenario::MarkerCompacted,
                                   Scenario::Slots, Scenario::SlotsCompacted,
                                   Scenario::Migrated};
const char* Name(Scenario scenario) {
  switch (scenario) {
    case Scenario::Marker: return "marker";
    case Scenario::MarkerCompacted: return "marker_compacted";
    case Scenario::Slots: return "slots";
    case Scenario::SlotsCompacted: return "slots_compacted";
    case Scenario::Migrated: return "migrated";
  }
  return "";
}

std::string Produce(Scenario scenario) {
  const std::string directory = MakeDirectory();
  const bool slots = scenario == Scenario::Slots || scenario == Scenario::SlotsCompacted;
  {
    KvStore store(false, false, slots ? KvDurability::CommitSlots : KvDurability::RollbackMarker);
    CHECK(store.Open(directory.c_str()));
    Populate(store);
    if (scenario == Scenario::MarkerCompacted || scenario == Scenario::SlotsCompacted)
      CHECK(store.Compact());
    CheckContents(store);
    store.Close();
  }
  if (scenario == Scenario::Migrated) {
    KvStore store(false, false, KvDurability::CommitSlots);
    CHECK(store.Open(directory.c_str()));
    CheckContents(store);
    store.Close();
  }
  const std::string bytes = ReadLog(directory);
  RemoveDirectory(directory);
  return bytes;
}

// A golden log is its length plus the non-zero byte runs (commit slots are
// mostly zero padding). Runs separated by fewer than 16 zero bytes merge.
struct Segment {
  size_t offset;
  const char* hex;
};
struct GoldenLog {
  Scenario scenario;
  size_t length;
  const Segment* segments;
  size_t count;
};

const char* Identifier(Scenario scenario) {
  switch (scenario) {
    case Scenario::Marker: return "Marker";
    case Scenario::MarkerCompacted: return "MarkerCompacted";
    case Scenario::Slots: return "Slots";
    case Scenario::SlotsCompacted: return "SlotsCompacted";
    case Scenario::Migrated: return "Migrated";
  }
  return "";
}

// Prints one golden log as the C++ definitions kv_checksum_golden.inc holds.
void Dump(Scenario scenario, const std::string& bytes) {
  printf("constexpr size_t k%sLength = %zu;\n", Identifier(scenario), bytes.size());
  printf("constexpr Segment k%sSegments[] = {\n", Identifier(scenario));
  size_t i = 0;
  while (i < bytes.size()) {
    if (bytes[i] == '\0') { ++i; continue; }
    size_t end = i;
    size_t zeros = 0;
    for (size_t j = i; j < bytes.size(); ++j) {
      if (bytes[j] != '\0') { end = j + 1; zeros = 0; }
      else if (++zeros >= 16) break;
    }
    printf("    {%zu,\n     \"", i);
    for (size_t j = i; j < end; ++j) {
      if (j > i && (j - i) % 48 == 0) printf("\"\n     \"");
      printf("%02x", static_cast<uint8_t>(bytes[j]));
    }
    printf("\"},\n");
    i = end;
  }
  printf("};\n");
}

void DumpAll() {
  printf("// Generated by `kv_checksum_test --dump` against the bitwise-CRC KV\n"
         "// runtime (16781f54). Do not regenerate with a changed checksum.\n");
  for (Scenario scenario : kScenarios) Dump(scenario, Produce(scenario));
  printf("constexpr GoldenLog kGoldenLogs[] = {\n");
  for (Scenario scenario : kScenarios)
    printf("    {Scenario::%s, k%sLength, k%sSegments,\n     sizeof(k%sSegments) / sizeof(Segment)},\n",
           Identifier(scenario), Identifier(scenario), Identifier(scenario), Identifier(scenario));
  printf("};\n");
}

#ifndef KV_CHECKSUM_CAPTURE
std::string Expand(const GoldenLog& golden) {
  std::string bytes(golden.length, '\0');
  for (size_t s = 0; s < golden.count; ++s) {
    const Segment& segment = golden.segments[s];
    const size_t length = strlen(segment.hex) / 2;
    CHECK(segment.offset + length <= bytes.size());
    for (size_t i = 0; i < length; ++i) {
      unsigned value = 0;
      CHECK(fortis::test::ParseHexByte(segment.hex + 2 * i, &value) == 1);
      bytes[segment.offset + i] = static_cast<char>(value);
    }
  }
  return bytes;
}

#include "kv_checksum_golden.inc"

const GoldenLog& Golden(Scenario scenario) {
  for (const GoldenLog& golden : kGoldenLogs)
    if (golden.scenario == scenario) return golden;
  abort();
}

// Checks that the stored checksums are the bitwise CRC over the bytes the
// format says they cover, so a golden mismatch points at the checksum.
void CheckFrames(const std::string& bytes, size_t start, size_t end) {
  size_t offset = start;
  while (offset < end) {
    CHECK(end - offset >= 12 && memcmp(bytes.data() + offset, "FKV1", 4) == 0);
    uint32_t length = 0, stored = 0;
    for (unsigned i = 0; i < 4; ++i) {
      length |= static_cast<uint32_t>(static_cast<uint8_t>(bytes[offset + 4 + i])) << (8 * i);
      stored |= static_cast<uint32_t>(static_cast<uint8_t>(bytes[offset + 8 + i])) << (8 * i);
    }
    CHECK(length <= end - offset - 12);
    CHECK(stored == BitwiseChecksum(bytes.data() + offset + 12, length));
    offset += 12 + length;
  }
}

void CheckSlot(const std::string& bytes, size_t base) {
  CHECK(memcmp(bytes.data() + base, "FKVS", 4) == 0);
  uint32_t stored = 0;
  for (unsigned i = 0; i < 4; ++i)
    stored |= static_cast<uint32_t>(static_cast<uint8_t>(bytes[base + 4092 + i])) << (8 * i);
  CHECK(stored == BitwiseChecksum(bytes.data() + base, 4092));
}

void TestGoldenWrites() {
  for (Scenario scenario : kScenarios) {
    const std::string produced = Produce(scenario);
    const std::string expected = Expand(Golden(scenario));
    if (produced != expected) {
      fprintf(stderr, "golden mismatch: %s\n", Name(scenario));
      Dump(scenario, produced);
      abort();
    }
    const bool slots = scenario != Scenario::Marker && scenario != Scenario::MarkerCompacted;
    if (slots) {
      CheckSlot(produced, 0);
      CheckSlot(produced, 4096);
    }
    CheckFrames(produced, slots ? 8192 : 0, produced.size());
  }
}

// Logs written by the bitwise runtime replay, migrate and keep accepting
// appends under KvChecksum.
void TestGoldenReplay() {
  for (Scenario scenario : kScenarios) {
    const bool slots = scenario != Scenario::Marker && scenario != Scenario::MarkerCompacted;
    const std::string directory = MakeDirectory();
    WriteLog(directory, Expand(Golden(scenario)));
    {
      KvStore store(false, false, slots ? KvDurability::CommitSlots : KvDurability::RollbackMarker);
      CHECK(store.Open(directory.c_str()));
      CheckContents(store);
      const KvMutation after{"after", "replayed", 8, 0, false};
      CHECK(store.Write(&after, 1));
      store.Close();
    }
    {
      KvStore store(false, false, slots ? KvDurability::CommitSlots : KvDurability::RollbackMarker);
      CHECK(store.Open(directory.c_str()));
      CheckContents(store);
      CHECK(Read(store, "after") == "replayed");
      store.Close();
    }
    RemoveDirectory(directory);
  }

  // The legacy marker log migrates to exactly the golden commit-slot bytes.
  const std::string directory = MakeDirectory();
  WriteLog(directory, Expand(Golden(Scenario::Marker)));
  {
    KvStore store(false, false, KvDurability::CommitSlots);
    CHECK(store.Open(directory.c_str()));
    CheckContents(store);
    store.Close();
  }
  CHECK(ReadLog(directory) == Expand(Golden(Scenario::Migrated)));
  RemoveDirectory(directory);
}

// A checksum mismatch is still detected: a flipped frame body byte makes
// replay refuse the log, and a damaged newest slot falls back to the older
// slot, which does not publish the final frame.
void TestCorruption() {
  {
    const std::string directory = MakeDirectory();
    std::string bytes = Expand(Golden(Scenario::Marker));
    bytes[20] = static_cast<char>(bytes[20] ^ 0x01);  // Inside the first frame body.
    WriteLog(directory, bytes);
    KvStore store;
    CHECK(!store.Open(directory.c_str()));
    RemoveDirectory(directory);
  }
  {
    const std::string directory = MakeDirectory();
    std::string bytes = Expand(Golden(Scenario::Slots));
    uint64_t sequence[2] = {};
    for (unsigned slot = 0; slot < 2; ++slot)
      for (unsigned i = 0; i < 8; ++i)
        sequence[slot] |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[slot * 4096 + 16 + i])) << (8 * i);
    const size_t newest = sequence[1] > sequence[0] ? 4096 : 0;
    bytes[newest + 4092] = static_cast<char>(bytes[newest + 4092] ^ 0x80);
    WriteLog(directory, bytes);
    KvStore store(false, false, KvDurability::CommitSlots);
    CHECK(store.Open(directory.c_str()));
    // The last frame wrote "delta"; only the newest slot published it.
    CHECK(Read(store, "delta") == "<missing>");
    CHECK(Read(store, "beta") == BetaValue());
    store.Close();
    RemoveDirectory(directory);
  }
}
#endif  // KV_CHECKSUM_CAPTURE

}  // namespace

int main(int argc, char** argv) try {
  if (argc == 2 && strcmp(argv[1], "--dump") == 0) {
    DumpAll();
    return 0;
  }
#ifdef KV_CHECKSUM_CAPTURE
  return 2;
#else
  TestVectors();
  TestGoldenWrites();
  TestGoldenReplay();
  TestCorruption();
  printf("kv checksum: table CRC-32 matches the bitwise CRC and golden logs\n");
  return 0;
#endif
} catch (...) {
  return fortis::test::ReportException();
}
