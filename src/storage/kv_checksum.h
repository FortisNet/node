#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fortis::storage {

// CRC-32/ISO-HDLC (the IEEE 802.3 / zlib CRC: reflected polynomial
// 0xEDB88320, initial value and final XOR 0xFFFFFFFF). Every KV frame, commit
// slot and compacted frame stores this value, so its output is part of both
// on-disk log formats and must never change. The 256-entry table is built at
// compile time and shared read-only; no C++ standard library is needed, so
// the header is usable in the Unikraft guest.
namespace kv_checksum_detail {
struct Table {
  uint32_t entries[256];
};

constexpr Table MakeTable() {
  Table table{};
  for (uint32_t index = 0; index < 256; ++index) {
    uint32_t crc = index;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    table.entries[index] = crc;
  }
  return table;
}

inline constexpr Table kTable = MakeTable();
}  // namespace kv_checksum_detail

constexpr uint32_t KvChecksum(const char* data, size_t length) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < length; ++i)
    crc = (crc >> 8) ^
          kv_checksum_detail::kTable.entries[(crc ^ static_cast<uint8_t>(data[i])) & 0xffu];
  return ~crc;
}

static_assert(kv_checksum_detail::kTable.entries[1] == 0x77073096u,
              "CRC-32 table entry 1");
static_assert(kv_checksum_detail::kTable.entries[255] == 0x2d02ef8du,
              "CRC-32 table entry 255");
static_assert(KvChecksum("", 0) == 0u, "CRC-32 of no bytes");
static_assert(KvChecksum("123456789", 9) == 0xcbf43926u, "CRC-32/ISO-HDLC check value");

}  // namespace fortis::storage
