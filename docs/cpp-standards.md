# C++ language and API documentation

This document states the toolchain policy, the rules for documentation
comments and the contract checks that the build enforces. Anyone who writes
or reviews C++ in this repository needs it. The design conventions for
classes, ownership and error handling are in [object-model.md](object-model.md).

## Toolchain policy

All C++ sources are compiled in C++23 language mode. The verification matrix
uses Clang 18 with libc++ and GCC 13 with libstdc++. Apple Clang works for
development on macOS.

The same sources also compile as a Unikraft unikernel guest, built with
GCC 13 and a pinned libc++ 14.0.6 port. The guest build files are not part of
this repository, but the code keeps the guest's constraints. Language mode
does not imply that every C++23 library facility is available on every
target:

- Keep library features compatible with the guest's libc++. Do not replace
  the status contract (`fortis::base::Status`, `Result<T>`) with
  `std::expected`, and do not depend on `std::print`, only because host
  headers provide them. Use feature-test macros, and test the real target.
- Exceptions are enabled.
- RTTI is disabled (`-fno-rtti`).
- Floating-point contraction is disabled (`-ffp-contract=off`), to keep
  wire-format and arithmetic behavior identical across targets.

Cppcheck also runs in C++23 mode. Its parser is independent of Clang and GCC.
The three analyses supplement each other; none replaces a build for the real
target.

libc++ 18 keeps `std::jthread` behind experimental stop-token support. Do not
enable the experimental library ABI. Test code that needs a thread uses
`std::thread` owned by a `std::unique_ptr` with a joining deleter (see
`tests/news_reads_test.cpp`). Production code uses `fortis::base::Thread`.

## Documentation comments

Use Doxygen `/** ... */` or `///` comments on public classes and nontrivial
public operations. Describe the caller's contract; do not restate the name:

- ownership and lifetime of returned values, borrowed dependencies and callbacks;
- valid input ranges, buffer sizes, nullability and whether failure changes output;
- errors, exceptions, blocking behavior and thread-safety requirements;
- protocol or persistence behavior that a future change must preserve.

Use `@param`, `@return`, `@throws` and `@pre` when they clarify a contract.
Keep simple accessors and implementation details concise. Do not copy
signatures or add author and date boilerplate. Put operational procedures in
`docs/`.

```cpp
/** Enqueue a durable user turn and acknowledge it independently of generation.
 * @return HTTP 201 after the message and pending job are committed atomically.
 * Provider failures update the durable job; clients read its status when polling.
 * Conversation credentials and message bodies must never enter public errors.
 */
```

When a contract changes, update its comment, the OpenAPI input or overlay
where relevant, and the tests together. Regenerate generated sources from the
project-owned inputs; do not edit them by hand.

## Enforced contract scope

`make check-cpp-contracts` runs `scripts/check_memory_ownership.py` and
`scripts/cpp_contracts.py`. It enforces the scope listed in
`config/cpp_contracts.json`: formatting, public naming, the production
C-allocation boundaries, and generated Doxygen HTML and XML without warnings.
The scope is incremental. Add new contract headers to it when practical, and
keep mechanical reformatting separate from behavior changes. The exact
accessor-naming exceptions for standard-style names are in
`scripts/cpp_contracts.py`. The check validates the listed contracts; it does
not measure documentation coverage of the whole tree.

Rules that the checked headers follow:

- An accessor that returns a borrowed pointer, span or `string_view` explains
  what invalidates it, and rejects borrowing from a temporary owner where the
  language allows.
- Copy borrowed data to owned storage before queueing asynchronous work.
- Serialize mutation, then stop and wake workers, before joining them or
  destroying their dependencies.
- Use composition for scoped ownership around static handles.

## Release protections

The hardening flags of the release executable are defined in
`scripts/native_hardening.mk`. `scripts/native_hardening.py` verifies them
against the actual compilation and the exact stripped ELF. PIE, NX and RELRO
alone do not establish compiler coverage, so each release also records the
emitted stack-guard calls, the effective flags per source, mechanism probes
and the explicit limits for prebuilt runtime and assembly objects.
