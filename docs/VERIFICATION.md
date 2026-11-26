# C++ verification

This document describes `make verify`, the gate that every change must pass
before review: its stages, its reports, how to run one stage, and what the
gate does not cover. Contributors and reviewers need it.

## Running the gate

From the repository root:

```sh
npm ci --omit=optional   # once: the tool packages for generators and tests
make verify
```

Docker must be running. The first run builds the tool image described by
`Dockerfile.verify`; its tag includes the Dockerfile hash. The image supplies
Clang 18, GCC 13, Cppcheck and the lint tools (on macOS, `/usr/bin/gcc` is
Clang, so the host cannot stand in for GCC). Node.js 25.9.0 is pinned by
digest to match the generated Unicode 17 tables; Ubuntu's Node 18 package
would report those tables as stale.

`scripts/verify.py` orchestrates the build and the tests. It is not linked
into the server and is not part of a release.

For a quicker check, `make lean-verify` runs only the Clang build (warnings
as errors, plus the production-binary check) and the lint stage, and writes
`build-verify/report-lean.json`. It skips the Clang analyzer, GCC, Cppcheck
and the unit and integration suites. A passing report with unchanged inputs
from either command qualifies `make native-release`. Run the full gate when a
change needs its tests or analyzers.

## Stages

Before it freezes the source inputs, the verifier runs the checksum-checked
BLS assembly bootstrap (`scripts/fetch-blst-assembly.sh`). The generated
vendor sources are then part of the input snapshot, so a fresh clone obeys
the same unchanged-input rule as a warm checkout. A bootstrap failure blocks
all builds and tests; `bootstrap.log` and the report record its result.

The command then runs seven stages:

1. **clang-build.** Compile the server and the test prerequisites with
   Clang/libc++, with warnings as errors, and check the production binary.
2. **clang-analyzer.** Run the Clang Static Analyzer on the project
   translation units, including test-specific defines. Analysis runs even
   when objects are current.
3. **gcc-build.** Compile the same prerequisites with GCC/libstdc++, with
   warnings as errors.
4. **lint.** Run the tests of the verification tools, the generator and
   boundary checks (`check-network-catalog`, `check-api-docs`,
   `check-redaction-tables`, `check-js-case-tables`, `check-layers`,
   `check-cpp-contracts`) and strict clang-tidy over all of `src/` and
   `tests/` (`lint-all`).
5. **cppcheck.** Run Cppcheck with compiler-derived flags, with errors,
   warnings, performance and portability checks enabled.
6. **unit-tests.** Run every `./build/...` command of the Makefile test
   recipe against **both** compiler builds. A failed build blocks its unit
   tests.
7. **integration-tests.** Run the HTTP/TLS replays of the
   `test-current-contracts` target against the Clang build: public error
   persistence, operations and status routes, durable asynchronous AI,
   native, contract and Robinhood prices, the price fallback warmer, incoming
   HTTPS and anonymous OHLC back-pressure. A failed Clang build blocks these
   tests; stale binaries are never run.

Both compilers use `-Wall -Wextra -Wpedantic -Werror -Wformat=2 -Wundef
-Wcast-align`; C++ also uses `-Woverloaded-virtual -Wnon-virtual-dtor`.
Exceptions are enabled and RTTI is disabled.

Every selected stage runs even if another fails. The command returns nonzero
for findings, build errors, tool failures or failed tests. It does not accept
known failures through a baseline, and it ignores the advisory lint mode.
Review every failed stage.

## Reports and individual stages

Compiler outputs are isolated under `build-verify/clang` and
`build-verify/gcc`. The normal `build/`, the sanitizer builds and persistent
data are not touched.

| File under `build-verify/` | Content |
| --- | --- |
| `report.json` | Stage results, tool versions and input hashes |
| `logs/` | Compiler, Cppcheck and lint output, analyzer results, one log per unit command |
| `unit-results.json` | Every unit command and its exit status |
| `integration-results.json` | The HTTP/TLS replay commands and their outcomes |

The report records a hash of every input: the sources, tests, scripts,
schemas, vendored code and the build and tool definitions. If a source or
tool changes during the run, the gate fails; the report lists the added,
removed and modified paths. All stages passing does not override a failed
unchanged-input check. A second verification command started while one is
running is rejected, because both would share the isolated build directories
and reports. A change of compiler image or version invalidates that
compiler's cached objects, even if the executable name stays the same.

To run one stage:

```sh
make cppcheck
python3 scripts/verify.py --stage clang-analyzer --jobs 4
python3 scripts/verify.py --stage gcc-build --jobs 4
```

An individual stage writes `build-verify/report-STAGE.json`. It does not turn
an earlier full report into a successful run.

Each unit command has a ten-minute deadline, and each replay a three-minute
deadline. A timeout is a failed test.

`scripts/verify_ci.py` merges the reports of stage groups that were run
separately (`native`, `clang-analyzer`, `lint`, `cppcheck`) into one
`report.json`. It rejects missing or duplicate groups, incomplete or failed
stages, a dirty or wrong commit, and source or tool snapshots that differ
between the groups. This repository has no hosted pipeline; the script exists
for setups that run the groups on separate machines.

## Scope of the analyzers

Cppcheck and the analyzer exclude vendored and generated translation units.
Declarations in project headers that those units include are still analyzed.
Cppcheck's suppressions are documented in `.cppcheck-suppressions`.

The analyzer honors a `NOLINT(clang-analyzer-CHECK)` comment that names the
check, on the exact line of the diagnostic. It records those findings under
`explicitly_suppressed` in its JSON result. Cppcheck excludes
`assertWithSideEffect` in test fixtures: those assertions run the operation
under test on purpose, and the gate rejects `NDEBUG` configurations. There is
no general baseline for production warnings or known failures. The
intentional exceptions are listed in
[static-analysis-limitations.md](static-analysis-limitations.md).

## What the gate does not cover

The gate runs the unit commands and the `test-current-contracts` replays. It
does not run:

- the other Python replays of `make test`;
- the differential tests that compare with Node.js (`tests/*.mjs`);
- the sanitizer builds (see [sanitizers.md](sanitizers.md));
- a release build or a test of the packaged executable.

A passing gate is therefore not proof that a release works on a host. Run the
module's replays when a change touches its routes, and the sanitizer targets
when it touches ownership or threading.

## Release build

`make native-release VERSION=...` builds the static musl executable for
x86-64 and ARM64 with `scripts/build-native-release.sh`. It requires a
complete, passing `make verify` or `make lean-verify` report whose input
hashes match the checkout. The tool-test part of the lint stage
(`make verify-tool-tests`) covers the release and hardening scripts
(`tests/native_release_test.py`, `tests/native_hardening_test.py`) and runs
ShellCheck on the release shell scripts. See
[native-runtime.md](native-runtime.md) for the runtime, and
[FortisNet Node OS](https://github.com/FortisNet/node-os) for how a release
is installed and updated on a host.

## Load tests must match the deployed flags

`tests/portfolio_load_test.py` measures price delivery for many simulated
users. Run it against a node that has the same price flags as the deployment
it stands for. A node that runs with `FORTIS_SPLIT_PRICE_STORE=1` must be
tested with that flag, together with `FORTIS_NATIVE_PRICE_ROUTE_CANARY=1` and
`FORTIS_PRICE_SNAPSHOT_ENABLED=1`:

- The split-store path tracks the requested contracts and starts proactive
  refreshes.
- Without it, on-demand spot entries expire after 30 seconds, and
  stale-while-revalidate can correctly return a quote marked stale at the
  next burst.
- The snapshot flag alone warms the six core assets, not that contract set.

When a run fails for this reason, record both the measured stale result and
the configuration mismatch.

A representative run uses `--users 100 --mode mixed --require-snapshot
--warmup --rounds 3 --interval 30`. A target that is not on loopback needs
`--allow-remote`. The explicit warmup is outside the measured opens. Stale,
missing, invalid and incomplete measured quotes are failures. Do not disable
freshness checks, extend TTLs or shorten the interval to turn a failed result
into a pass. Enrollment is quota-limited on purpose: keep the setup attempts
that received 429 in the record, and revoke all test devices afterwards. Keep
mounted data and credentials private.
