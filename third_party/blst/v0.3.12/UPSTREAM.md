# blst source

These files come unchanged from
[blst v0.3.12](https://github.com/supranational/blst/tree/v0.3.12),
commit `e99f7db0db413e2efefcfd077a4e335766f39c27`: `LICENSE`,
`bindings/blst.h`, `bindings/blst_aux.h`, every `src/*.c` / `src/*.h`
(the C++ header and the perlasm sources are omitted), and the pre-generated
assembly `build/assembly.S`, `build/elf/*` and `build/mach-o/*` (the COFF,
Win64 and CHERI flavours are omitted). `src/`, `build/` and
`bindings/blst.h` are byte-identical to the `blst-0.3.12` crate that the
official Dash Platform v4.1.1 proof verifier links through `blstrs_plus`.

blst supports its portable C (`__BLST_NO_ASM__`) only on 32-bit-limb
targets; on x86_64 and aarch64 it always uses this assembly, as the Rust
crate does. The native server compiles `src/server.c` and
`build/assembly.S` with upstream's `-D__BLST_PORTABLE__` plus
`-D__BLST_NO_CPUID__`: every optional instruction set (SSSE3 and SHA-NI
SHA-256, ADX/BMI2 multiplication) sits behind `__blst_platform_cap`, which
stays zero without the CPU probe, so baseline x86-64 code runs on every
host and in the guest. Without `__BLST_PORTABLE__` the x86_64 SHA-256 is
the SSSE3 one (`pshufb`), which faults on the QEMU guest CPU.
It is used solely by `src/dashpay_proof.cpp` for BLS12-381 Basic-scheme
signature verification of Tenderdash commits.
