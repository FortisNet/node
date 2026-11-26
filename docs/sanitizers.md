# Sanitizer builds and lint

This document describes the sanitizer builds (ASan, UBSan, TSan) and the
clang-tidy lint targets: what each `make` target does, how the builds are
isolated, and why macOS runs them in a container. Developers who run these
checks or read their reports need it.

## Targets

| Target | What it does |
| --- | --- |
| `make asan` / `make ubsan` / `make tsan` | Build the server and every unit-test binary of `make test` with the sanitizer, into `build-asan/`, `build-ubsan/` or `build-tsan/`. |
| `make sanitize-test` | Build under ASan + UBSan (+ LeakSanitizer) and run the `./build/...` commands of the `make test` recipe. `UNIT='regex'` narrows them. |
| `make ubsan-test` / `make tsan-test` | The same with UBSan alone or with ThreadSanitizer. |
| `make sanitize-replay REPLAY='regex' [SANITIZER=asan]` | Run the matching `python3 tests/...` replays against sanitizer-built server binaries. |
| `make sanitize-clean` | Delete the three variant trees. |
| `make lint` | clang-tidy over the files changed against the merge-base with the default branch (`LINT_BASE=ref` changes the base, for example `HEAD` for uncommitted work). It fails only on findings on added or modified lines of `git diff -U0` against that base. `LINT_WHOLE_FILES=1` reports every finding in the changed files. |
| `make lint-all` | clang-tidy over all of `src/` and `tests/`. It fails on findings or analysis errors. `make verify` runs it. |
| `make format-diffstat` | Report how many lines `.clang-format` would change. Nothing is rewritten. |

None of these targets changes `make test` or `build/`.

## How a variant is built

`scripts/sanitize.py` makes `build-<variant>/` a directory of symlinks to the
project's top-level entries, with its own `build/` and its own `.deps/`. It
copies the pinned Mbed TLS archive and builds the library there with the
variant's compiler. The unchanged Makefile runs inside that tree with the
variant's `CXX`, `CC`, `CXXFLAGS` and `CFLAGS`. The normal `build/`, `.deps/`
and the flags stamp are never touched. Inside the variant tree, the recipe's
`build/...` paths mean the variant's binaries.

Every variant keeps `-std=c++23 -fno-rtti -Wall -Wextra -Werror -pedantic
-ffp-contract=off`. UBSan runs with `-fno-sanitize=vptr` (the vptr check
needs RTTI) and `-fno-sanitize-recover=all`. The objects use `-O1 -g
-fno-omit-frame-pointer`.

`tests/sanitizers/thread_stack.h` is force-included into sanitizer builds
only. The server sizes its thread stacks explicitly (128 to 512 KiB). The
header multiplies those sizes by eight, because instrumented frames are
several times larger. A passing sanitizer run therefore says nothing about
whether the production sizes suffice.

A command fails when it exits non-zero or when any process it started wrote
a sanitizer report (`build-<variant>/sanitizer-logs/`).

## Why macOS runs them in a container

On macOS (observed with macOS 26 and Apple clang 17), a trivial threaded
program built with `-fsanitize=address` hangs during runtime initialization,
the same program built with `-fsanitize=thread` dies with SIGSEGV before
`main`, and there is no LeakSanitizer. Only UBSan alone works. On macOS,
`scripts/sanitize.py` and `scripts/lint.py` therefore run inside the
`Dockerfile.sanitizers` image (Ubuntu 24.04 pinned by digest, clang 18,
libc++). The image tag carries a checksum of the Dockerfile, and the image
is built on first use. On Linux the host toolchain is used.
`FORTIS_SANITIZER_RUNTIME=host|docker` overrides the choice.

In the container the server links libc++ statically. With Ubuntu's shared
libc++abi, ASan reports a false alloc-dealloc mismatch whenever the runtime
destroys an exception.

The container's `/tmp` is on its disk, where an fsync costs milliseconds.
`spot_read_concurrency_test` reads `FORTIS_TEST_TMPDIR`, which the script
sets to `/dev/shm`, because the test's time bounds assume a cheap fsync.

## Suppressions and skips

`tests/sanitizers/{lsan,tsan,ubsan}.supp` are empty: every finding so far was
fixed in the code. A new entry needs a written justification above it.

`tests/sanitizers/skip.txt` lists recipe commands that the sanitizer runs
leave out because they fail for a reason no sanitizer build can change. It
is never used for a sanitizer report. Its one entry is
`network_catalog_concurrency_test`, whose assertion about the route-state
lock is stale; the file explains the details.

Two tests adjust to the instrumented environment without hiding a finding:

- `apple_app_attest_test` skips its peak-RSS bound under ASan and TSan,
  because quarantine and shadow memory keep freed blocks resident.
- `http_server_test` accepts `ECONNRESET` after a 413 or 431 refusal, because
  Linux resets a socket that is closed with unread request bytes.

`src/crypto/blst_platform_cap.c` defines blst's `__blst_platform_cap` with
the alignment of an `int`. The pre-generated assembly only declares it as a
byte-aligned common symbol, and an aarch64 ELF link (for example the
container on Apple silicon) fails with "relocation truncated to fit" when
the symbol lands on an address that is not a multiple of 4. The value stays
zero, so no optional instruction set is enabled.

## Lint

`.clang-tidy` is a conservative set: `bugprone-*`, `clang-analyzer-*`,
`performance-*`, and selected `cert-*`, `modernize-*` and `readability-*`
checks. The file lists what it leaves out and why. Struct padding advice is
excluded, because a layout change needs its own measurement.

`scripts/lint.py` analyses each source with the flags of its own compile
line, taken from a dry run of the test build, so per-object test switches and
include paths apply. A program under `tests/` without a Makefile build rule
fails lint; it does not silently lose coverage.

`make lint` reports a finding only when its line is new against the base: an
added or modified line in `git diff -U0`, or any line of an untracked file.
Findings elsewhere in the analysed files are counted in the summary, and
`make lint-all` lists them. A finding on an unchanged line that a change
provokes (for example, a caller made unsafe by an edit to the callee) is not
reported. Review the `LINT_WHOLE_FILES=1` output when a change alters a
function's contract.

`make lint-all` is the strict gate, and `make verify` clears any advisory
setting before it runs it. `FORTIS_LINT_REPORT_ONLY=1` makes findings exit 0;
it is a deliberate local opt-in for an advisory run, never the default.
`tests/lint_tool_test.py` tests the wrapper itself: failures, warnings, clean
analysis, header deduplication and missing build flags.

`.clang-format` describes the code style. `make format-diffstat` measures
the difference between the tree and that style without rewriting anything.
