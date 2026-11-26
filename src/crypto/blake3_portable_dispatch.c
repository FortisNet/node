/* BLAKE3's dispatch interface (blake3_impl.h) bound to the portable
 * implementation only: what upstream blake3_dispatch.c resolves to under
 * -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512
 * -DBLAKE3_USE_NEON=0, without its x86 CPU probe. That probe includes
 * <immintrin.h>, which the Unikraft guest toolchain does not provide, so
 * host and guest both link this file instead
 * (third_party/blake3/1.8.7/UPSTREAM.md).
 */
#include "blake3_impl.h"

void blake3_compress_in_place(uint32_t cv[8], const uint8_t block[BLAKE3_BLOCK_LEN], uint8_t block_len,
                              uint64_t counter, uint8_t flags) {
  blake3_compress_in_place_portable(cv, block, block_len, counter, flags);
}

void blake3_compress_xof(const uint32_t cv[8], const uint8_t block[BLAKE3_BLOCK_LEN], uint8_t block_len,
                         uint64_t counter, uint8_t flags, uint8_t out[64]) {
  blake3_compress_xof_portable(cv, block, block_len, counter, flags, out);
}

void blake3_xof_many(const uint32_t cv[8], const uint8_t block[BLAKE3_BLOCK_LEN], uint8_t block_len,
                     uint64_t counter, uint8_t flags, uint8_t out[64], size_t outblocks) {
  for (size_t i = 0; i < outblocks; ++i)
    blake3_compress_xof(cv, block, block_len, counter + i, flags, out + 64 * i);
}

void blake3_hash_many(const uint8_t* const* inputs, size_t num_inputs, size_t blocks, const uint32_t key[8],
                      uint64_t counter, bool increment_counter, uint8_t flags, uint8_t flags_start,
                      uint8_t flags_end, uint8_t* out) {
  blake3_hash_many_portable(inputs, num_inputs, blocks, key, counter, increment_counter, flags, flags_start,
                            flags_end, out);
}

size_t blake3_simd_degree(void) { return 1; }

/* blake3_impl.h's popcnt() is __builtin_popcountll, which baseline x86-64
 * (no -mpopcnt) lowers to a call to __popcountdi2. The guest links no libgcc;
 * compiler-rt provides it. */
