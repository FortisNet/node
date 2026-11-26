# BLAKE3 source

These files come unchanged from
[BLAKE3 1.8.7](https://github.com/BLAKE3-team/BLAKE3/tree/1.8.7), commit
`f3149ec5bb5449af877ba20377a11008ff499fa2`: `c/blake3.c`, `c/blake3.h`,
`c/blake3_impl.h`, `c/blake3_portable.c` and the
three license files (CC0-1.0, Apache-2.0, Apache-2.0 WITH LLVM-exception).
1.8.7 is the `blake3` crate version GroveDB v5.0.1 resolves in the official
Dash Platform v4.1.1 verifier.

The native server builds the portable implementation only
(`-DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512
-DBLAKE3_USE_NEON=0`). It is used solely by `src/dashpay_proof.cpp` for
Merk node hashing.

`c/blake3_dispatch.c` is not vendored: its x86 CPU probe includes
`<immintrin.h>`, which the Unikraft guest toolchain lacks. Host and guest
link `src/blake3_portable_dispatch.c` instead, the same five dispatch
functions bound to the portable implementation (what upstream's dispatcher
resolves to under the flags above).
