# Strict analysis and intentional exceptions

This document lists the places where the code deliberately annotates or
disables a compiler, clang-tidy, analyzer or Cppcheck diagnostic, and why.
Anyone who meets a `NOLINT` or `cppcheck-suppress` comment, or wants to add
one, needs it.

## Policy

All C++ builds use pedantic warnings as errors. Clang-tidy sets
`WarningsAsErrors: '*'`; the lint wrapper also rejects tool failures and
sources without a compilation rule. `make verify` runs both compiler
families, the Clang Static Analyzer, clang-tidy, Cppcheck and the tests (see
[verification.md](verification.md)). No known-failure baseline turns a
failure green.

Some checks flag intentional behavior or hit a limit of a tool's model.
Those cases carry a local annotation that names the exact diagnostic and
gives the reason. A diagnostic is never suppressed across production code.
An exception is a reviewable source comment, not an entry in an opaque
baseline. A failed or incomplete stage is still reported as failed.

## Exceptions in the tree

- **Cppcheck 2.13 typed storage.** Its template model sometimes reads
  `std::unique_ptr<T, StorageDeleter>::get()` as `void*`
  (`arithOperationsOnVoidPointer`), and arena-allocated arrays of
  `const char*` as integers (`AssignmentAddressToInteger`). The code has
  typed pointers, and Clang and GCC compile the operations under pedantic
  errors. Each affected expression has a local explanation and the exact
  Cppcheck diagnostic name. Removing an annotation reproduces the finding.
  When the tool is updated, recheck these models.
- **Fallible generic moves.** clang-tidy 18's `bugprone-exception-escape`
  recommends nonthrowing move operations even when the move of a generic
  value can throw. `Result<T>` propagates that failure on purpose, with a
  conditional `noexcept`. Its two declarations carry local annotations.
  Compile-time traits, runtime tests with throwing values and the sanitizer
  tests validate the contract.
- **Moved-from contract tests.** Ownership tests inspect the defined empty
  state after moving standard owners and project handles. Cppcheck's
  `accessMoved` and the `use-after-move` checks of clang-tidy and the
  analyzer are suppressed only on those assertions.
- **Standard-library bounds probe.** `block.at(index)` in
  `src/base/stdlib_check.h` throws on purpose, to test exception unwinding at
  startup. Cppcheck's range diagnostic is annotated on that exact call.
- **Truncation oracle.** `tests/fixed_string_test.cpp` asks libc `snprintf`
  to truncate tiny buffers, and compares every byte with the bounded string
  implementation. GCC's `-Wformat-truncation` is disabled around this oracle
  only. Production formatting has no such exemption.
- **Assertions in test fixtures.** Cppcheck's `assertWithSideEffect` is
  excluded for `tests/` in `.cppcheck-suppressions`: those assertions run the
  operation under test. Verification rejects `NDEBUG` and executes the
  assertions under both compilers.
- **Invalid enumerator tests.** A few tests cast a value that is not an
  enumerator into an enum with a fixed underlying type, to exercise its
  rejection: `tests/error_board_test.cpp` (255 into
  `ErrorOrigin : std::uint8_t`), `tests/play_integrity_test.cpp` and
  `tests/device_attestation_foundation_test.cpp`. The value fits the
  underlying type and is discarded before use. Each call carries one
  `clang-analyzer-optin.core.EnumCastOutOfRange` annotation. Production enum
  conversions have no exemption.
- **Vendored and generated code.** Dependency implementations under
  `third_party/`, `.deps/` and `proto/generated/` are pinned and reviewed
  separately. Their build systems keep their own warning policies, and
  Cppcheck and the analyzer exclude findings in them. Project headers under
  `src/` are ordinary includes and are analyzed.

The analyzer records the findings it suppresses through an explicit
annotation in `explicitly_suppressed` in its JSON result, so they stay
visible in the verification report.
