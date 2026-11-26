# Native HTTPS C++ unit coverage

This document describes the C++ unit suites of the node's own HTTPS ingress:
how to run them, how to measure their coverage, and which cases and
boundaries they pin. Developers who change the listener, TLS or transport
code need it. The ingress itself is described in
[native-https.md](native-https.md).

Three suites link against the production objects:
`tests/https_listener_unit_test.cpp`, `tests/https_tls_unit_test.cpp` and
`tests/http_transport_unit_test.cpp`. Internal declarations come from the
app's private `src/app/https_listener_internal.h`. The tests do not include a
`.cpp` file, reimplement admission logic, bypass authentication or replace
the TLS cryptography.

## Running

```sh
make https-unit-tests
make https-unit-coverage
make verify
```

The three binaries also run in the normal test recipe, so `make verify` runs
them under both Clang/libc++ and GCC/libstdc++.

Coverage (`scripts/https_unit_coverage.py`) needs Clang and matching LLVM
tools: Xcode's selected tools on macOS, or versioned LLVM 18 tools where
available on Linux. It builds instrumented objects in
`build-https-unit-coverage/`, separate from the ordinary and verification
objects, and writes `summary.json`, `report.txt`, `listener.txt` and one log
per suite there. Each run deletes its own old raw profiles, so earlier
executions cannot inflate the result.

## Measured scope

The figures below are the last recorded measurement. Run
`make https-unit-coverage` for current figures.

| Production file | Lines | Functions | Branch outcomes |
| --- | ---: | ---: | ---: |
| `src/app/https_listener.cpp` | 98.18% | 100% | 84.19% |
| `src/http/http_transport.h` | 100% | 100% | 100% |
| `src/app/shutdown_signal.cpp` | 93.75% | 83.33% | not applicable |

These are measurements of three targeted suites, not of the whole node. The
raw report also includes the much larger HTTP parser and router files; these
suites do not cover all of their routing functionality, which the rest of the
test suite and the HTTP replays cover. Unexecuted listener paths are mainly
allocator exhaustion and rare clock, poll, file or socket system-call
failures. The results use no production suppression and no failure baseline.

## Cases and boundaries

- DNS labels and total length; case-insensitive configured Host authorities;
  port boundaries 1 and 65535; zero, overflowing and non-numeric ports;
  userinfo; unknown hosts.
- Exclusion of public diagnostics, encoded separators and controls, malformed
  and double escapes, dot segments and repeated-slash ambiguity. Original
  signed targets and opaque bodies stay intact.
- The exact 16 KiB header and 512 KiB body ceilings and one byte above them;
  duplicate framing, unsupported transfer coding, `Expect`, bodyless methods,
  fragmented headers and bodies, pipelining and EOF.
- Token-bucket burst and refill boundaries, identity table saturation and
  eviction, protection against underflow on a backward clock, and the health
  exemption.
- Global and per-peer upload and connection limits, accept fairness, worker
  queue saturation and shutdown, and application admission. Isolated health
  still succeeds.
- Nonblocking reads and writes, reactor backpressure, idle and frame
  deadlines, ordered reuse, failed writes, request ownership through handoff
  and shutdown, and exactly 100 requests before a persistent TLS connection
  is closed.
- An ephemeral CA generated in C++, with independent DNS and IP keys and
  certificates: valid, wrong SAN, wrong key, wrong trust root, expired, not
  yet valid and client-only EKU; TLS 1.2 and 1.3; SNI case handling and
  unknown names; HTTP/1.1 ALPN.
- Atomic certificate reload, bounded retained generations, retry after old
  references are released, live SIGHUP reload and cross-thread shutdown.
- Key file size, owner-only permissions, symlinks, directories, FIFOs and
  missing files. ACME regular-file bounds, token traversal, GET/HEAD/405
  behavior, and canonical redirects including overflow and preservation of a
  signed query.
- Deferred and reserved replies, timeouts, failed writes, late and duplicate
  completion and stop cancellation. Each finishes exactly once and never
  frees a borrowed transport.
- Registry capacity, collisions, removal and reuse, concurrent lookup and
  borrowed close; the generated `Date` format and cardinality, HEAD framing
  and write failures.
- Startup configuration, occupied ports, invalid descriptors and challenge
  mounts; cleanup after a refusal, single-use startup, and refusal to run
  before a successful startup.

## Behaviors the suites pin

Each of these rules has a regression test in the suites:

1. The ACME responder serves only GET and HEAD. Other methods get 405 with
   `Allow`.
2. A Host port must be in 1 to 65535. Configured DNS authorities compare
   without regard to case.
3. A configured DNS name with an empty label, an oversized label or a label
   that ends in a hyphen is refused.
4. A TLS private key file must have owner-only permissions. The staging hook
   keeps its stricter mode 400.
5. A backward or failed clock reading cannot grant rate-limit tokens or evict
   live identities.
6. A registry collision cannot replace an existing transport. Registration
   refuses a second owner, and a failed connection start cannot unregister
   the first.
7. Static response extras cannot duplicate the generated `Date` header. They
   are refused, like a duplicate `Server-Timing`, before any write.
8. Startup is single-use, and `Run` refuses a candidate that was not started
   or whose startup was refused. Repeating startup therefore cannot replace
   or leak redirect descriptors.
