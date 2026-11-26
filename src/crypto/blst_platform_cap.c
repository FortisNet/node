// blst's CPU capability word. With __BLST_NO_CPUID__ (the host and guest
// builds; see BLST_FLAGS) blst's own cpuid.c defines nothing, and the
// pre-generated assembly only declares `.comm __blst_platform_cap,4`: a
// common symbol that the linker zero-fills, which is what keeps every
// optional instruction set off.
//
// A common symbol declared without an alignment is byte-aligned under clang's
// integrated assembler. On aarch64 ELF the assembly loads the word with a
// scaled 32-bit load, and ld rejects that relocation ("relocation truncated
// to fit: R_AARCH64_LDST32_ABS_LO12_NC") whenever the symbol lands on an
// address that is not a multiple of 4, so the link succeeded or failed with
// the layout of the binary. This definition gives the word the alignment of
// an int and the same value, zero; the declarations in the assembly resolve
// to it. x86-64 (the guest) reads the word PC-relative and was never affected.
int __blst_platform_cap = 0;
