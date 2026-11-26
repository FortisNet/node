#include "evm_proof.h"

#include <mbedtls/bignum.h>
#include <mbedtls/ecp.h>

#include <stdio.h>
#include <string.h>

namespace fortis::auth {

namespace {
constexpr uint64_t kRound[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
    0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
};
constexpr unsigned kRotation[25] = {
    0, 1, 62, 28, 27, 36, 44, 6, 55, 20, 3, 10, 43,
    25, 39, 41, 45, 15, 21, 8, 18, 2, 61, 56, 14,
};
uint64_t Rotate(uint64_t value, unsigned n) {
  return n ? (value << n) | (value >> (64 - n)) : value;
}
void Permute(uint64_t state[25]) {
  for (uint64_t round : kRound) {
    uint64_t columns[5], deltas[5], temp[25];
    for (unsigned x = 0; x < 5; ++x)
      columns[x] = state[x] ^ state[x + 5] ^ state[x + 10] ^ state[x + 15] ^ state[x + 20];
    for (unsigned x = 0; x < 5; ++x)
      deltas[x] = columns[(x + 4) % 5] ^ Rotate(columns[(x + 1) % 5], 1);
    for (unsigned y = 0; y < 5; ++y)
      for (unsigned x = 0; x < 5; ++x) {
        const unsigned source = x + 5 * y;
        temp[y + 5 * ((2 * x + 3 * y) % 5)] =
            Rotate(state[source] ^ deltas[x], kRotation[source]);
      }
    for (unsigned y = 0; y < 5; ++y)
      for (unsigned x = 0; x < 5; ++x)
        state[x + 5 * y] = temp[x + 5 * y] ^
            ((~temp[(x + 1) % 5 + 5 * y]) & temp[(x + 2) % 5 + 5 * y]);
    state[0] ^= round;
  }
}
void Absorb(uint64_t state[25], size_t* offset, const uint8_t* data, size_t length) {
  constexpr size_t kRate = 136;
  for (size_t i = 0; i < length; ++i) {
    const size_t slot = (*offset)++ % kRate;
    state[slot / 8] ^= static_cast<uint64_t>(data[i]) << (8 * (slot % 8));
    if (slot == kRate - 1) Permute(state);
  }
}
int HexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
bool DecodeSignature(const char* hex, uint8_t bytes[65]) {
  if (!hex || hex[0] != '0' || hex[1] != 'x' || strnlen(hex, 133) != 132) return false;
  for (unsigned i = 0; i < 65; ++i) {
    const int high = HexNibble(hex[2 + 2 * i]);
    const int low = HexNibble(hex[3 + 2 * i]);
    if (high < 0 || low < 0) return false;
    bytes[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return bytes[64] == 0 || bytes[64] == 1 ||
         bytes[64] == 27 || bytes[64] == 28;
}
}  // namespace

void evm_keccak256(const uint8_t* data, size_t length, uint8_t digest[32]) {
  uint64_t state[25]{};
  size_t offset = 0;
  if (length) Absorb(state, &offset, data, length);
  const size_t slot = offset % 136;
  state[slot / 8] ^= static_cast<uint64_t>(1) << (8 * (slot % 8));
  state[16] ^= 0x8000000000000000ULL;
  Permute(state);
  for (unsigned i = 0; i < 32; ++i)
    digest[i] = static_cast<uint8_t>(state[i / 8] >> (8 * (i % 8)));
}

bool evm_recover_address(const uint8_t* message, size_t message_length,
                         const char* signature, char address[43]) {
  if (!message || !address || message_length > 1024 * 1024) return false;
  uint8_t sig[65];
  if (!DecodeSignature(signature, sig)) return false;
  // Solidity and ethers use Keccak-256, not the NIST SHA3-256 padding.
  const char prefix[] = "\x19" "Ethereum Signed Message:\n";
  char decimal[24];
  const int decimal_length = snprintf(decimal, sizeof(decimal), "%zu", message_length);
  if (decimal_length <= 0 || static_cast<size_t>(decimal_length) >= sizeof(decimal)) return false;
  uint64_t state[25]{};
  size_t offset = 0;
  const size_t prefix_length = sizeof(prefix) - 1;
  uint8_t digest[32];
  Absorb(state, &offset, reinterpret_cast<const uint8_t*>(prefix), prefix_length);
  Absorb(state, &offset, reinterpret_cast<const uint8_t*>(decimal),
         static_cast<size_t>(decimal_length));
  Absorb(state, &offset, message, message_length);
  const size_t slot = offset % 136;
  state[slot / 8] ^= static_cast<uint64_t>(1) << (8 * (slot % 8));
  state[16] ^= 0x8000000000000000ULL;
  Permute(state);
  for (unsigned i = 0; i < 32; ++i)
    digest[i] = static_cast<uint8_t>(state[i / 8] >> (8 * (i % 8)));

  mbedtls_ecp_group group;
  mbedtls_ecp_point recovered, point;
  mbedtls_mpi r, s, z, inverse, first, second;
  mbedtls_ecp_group_init(&group);
  mbedtls_ecp_point_init(&recovered);
  mbedtls_ecp_point_init(&point);
  mbedtls_mpi_init(&r); mbedtls_mpi_init(&s); mbedtls_mpi_init(&z);
  mbedtls_mpi_init(&inverse); mbedtls_mpi_init(&first); mbedtls_mpi_init(&second);
  bool ok = false;
  do {
    if (mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256K1) != 0 ||
        mbedtls_mpi_read_binary(&r, sig, 32) != 0 ||
        mbedtls_mpi_read_binary(&s, sig + 32, 32) != 0 ||
        mbedtls_mpi_read_binary(&z, digest, 32) != 0 ||
        mbedtls_mpi_cmp_int(&r, 0) <= 0 || mbedtls_mpi_cmp_mpi(&r, &group.N) >= 0 ||
        mbedtls_mpi_cmp_int(&s, 0) <= 0 || mbedtls_mpi_cmp_mpi(&s, &group.N) >= 0)
      break;
    uint8_t compressed[33];
    compressed[0] = sig[64] == 27 || sig[64] == 0 ? 0x02 : 0x03;
    if (mbedtls_mpi_write_binary(&r, compressed + 1, 32) != 0 ||
        mbedtls_ecp_point_read_binary(&group, &point, compressed, sizeof(compressed)) != 0 ||
        mbedtls_ecp_check_pubkey(&group, &point) != 0 ||
        mbedtls_mpi_inv_mod(&inverse, &r, &group.N) != 0 ||
        mbedtls_mpi_mod_mpi(&z, &z, &group.N) != 0 ||
        mbedtls_mpi_sub_mpi(&first, &group.N, &z) != 0 ||
        mbedtls_mpi_mul_mpi(&first, &first, &inverse) != 0 ||
        mbedtls_mpi_mod_mpi(&first, &first, &group.N) != 0 ||
        mbedtls_mpi_mul_mpi(&second, &s, &inverse) != 0 ||
        mbedtls_mpi_mod_mpi(&second, &second, &group.N) != 0 ||
        mbedtls_ecp_muladd(&group, &recovered, &first, &group.G,
                           &second, &point) != 0)
      break;
    uint8_t public_key[65];
    size_t key_length = 0;
    if (mbedtls_ecp_point_write_binary(&group, &recovered, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                       &key_length, public_key, sizeof(public_key)) != 0 ||
        key_length != 65) break;
    evm_keccak256(public_key + 1, 64, digest);
    constexpr char hex[] = "0123456789abcdef";
    address[0] = '0'; address[1] = 'x';
    for (unsigned i = 0; i < 20; ++i) {
      address[2 + 2 * i] = hex[digest[12 + i] >> 4];
      address[3 + 2 * i] = hex[digest[12 + i] & 15];
    }
    address[42] = '\0';
    ok = true;
  } while (false);
  mbedtls_mpi_free(&second); mbedtls_mpi_free(&first); mbedtls_mpi_free(&inverse);
  mbedtls_mpi_free(&z); mbedtls_mpi_free(&s); mbedtls_mpi_free(&r);
  mbedtls_ecp_point_free(&point); mbedtls_ecp_point_free(&recovered);
  mbedtls_ecp_group_free(&group);
  return ok;
}

}  // namespace fortis::auth
