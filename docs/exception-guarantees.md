# Exception guarantees and request ownership

This document states what each boundary in the node guarantees when an
exception is thrown, and who owns the request, socket or job afterwards.
Anyone who adds a handler, a worker thread or a callback needs it. The general
error-handling conventions are in [object-model.md](object-model.md).

The policy does not claim that arbitrary exceptions or memory faults are safe
to resume from. Expected failures are returned as status values; the
boundaries below handle what is left.

## Guarantee matrix

| Operation / owner | State and guarantee | Failure policy / ownership |
| --- | --- | --- |
| Request parsing, `ConnectionSession::Dispatch` | Reader may consume the socket and populate only `HttpRequest`-owned storage; no application mutation or response writes | Allocation exception -> 503; other standard/non-standard exception -> 500. `ParsedRequest` releases partial storage. Caller still closes socket. HEAD suppresses the body once its method is known. |
| Typed synchronous route handler | RAII route lock, response capture and request buffers unwind. Domain operation determines basic or transactional guarantee | `bad_alloc` -> 503; other standard exception -> 500. Captured but unsent success/defer is discarded. Unknown exceptions remain fatal because shared-state validity is unproved. |
| Raw-fd route handler | Same domain state rules, but bytes may already be on the socket | Standard exception closes I/O without appending another response. The original close owner remains responsible unless a reservation transferred it. |
| Reserved / asynchronous HTTP response | Reserve transfers fd ownership immediately. Handoff owns response copies and one terminal callback | Fail pending typed reservations with static 500/503; abandon raw reservations without extra bytes. A completed/claimed response wins a race; late failure never closes or replaces it. |
| Response writer | Bounded stack formatting and borrowed valid bytes; socket I/O returns false on error, possibly after partial bytes | `noexcept`; no automatic retry of the HTTP response. Connection owner closes. No allocation, domain callback or application mutation. |
| Completion / terminal callbacks | May already have consumed context or recorded a side effect | Named fatal boundary logs classification and terminates. No second invocation or fabricated retry; catching here cannot undo consumed ownership. |
| AI job claim | Store serializes claim; durable claim is the final fallible operation before copying identifiers to the caller | Repository must throw before durable claim. `ProcessPending` catches/reports classification and returns false; no job accounting acquired. Production claim uses status-returning KV writes and no throwing operations after commit. |
| AI reply generation | User turn/job is already durable; temporary arena/body owners and request-key wipe guard unwind | Standard, allocation and non-standard exceptions settle that job with fixed `provider_exception` classification. Active-job accounting is released; worker accepts subsequent jobs. |
| AI reply commit / late failure | `SaveReply` writes assistant, index, statistics and job deletion atomically | `SetJobStatus` serializes with `SaveReply`; a committed reply makes later status updates no-ops. Read/corruption/write errors return false. Failure reporting cannot recreate a failed job over a committed reply. |
| AI failure-status persistence | Only bounded public codes enter the error reporter | Report `job_status_write_failed` if persistence fails/throws. Do not invent a durable failure status. Retained running state can recover after restart; existing retry policy applies. |
| KV multi-record write | One frame and backend durability/recovery policy define the transaction, including group commit / commit slots | An unsuccessful result can be ambiguous after fsync. Recovery decides the durable frame. A caught exception is not an application-level rollback. Logging cannot interrupt recovery bookkeeping. |
| Device enrollment/rotate/revoke; wallet sync | Conditional/multi-record KV writes provide atomic record/index updates; domain validation precedes write | Preserve the domain status/HTTP mapping and replay/idempotency behavior. Never restore a stale snapshot after acknowledged writes. Unproven invariant failures remain fatal at the outer boundary. |
| Provider execution / lane coordinator; chain transport | Bounded queued state, status/result transport errors, cancellation and lane settlement | Expected failures are results. Unexpected escaping exceptions remain fatal; a catch around the whole loop cannot safely repeat an attempt, lane settlement or consumed completion. |
| Spot/OHLC/history/discovery owners; price warmer/snapshot/batch driver | Shared cache/series state and borrowed completion handles; status-returning failures and known cleanup guards | Keep fatal infrastructure boundaries for unexpected escaping exceptions. History query temporary allocations unwind to the request boundary; owner callbacks are not retried after throwing. |
| Token/address workers, schedulers and revalidators | Typed owners, request/job scratch, shared repository and bounded queue accounting | Expected failures retain domain status handling; no blanket catch-and-continue around partially consumed shared jobs. Escaping invariant exceptions terminate through the common thread boundary. |
| News/media schedulers and workers; exchange/Robinhood/DashPay workers | Owned scratch and domain result/status failures; some side effects are externally irreversible | Keep the domain refusals and teardown order. Unexpected exceptions reaching thread entry are fatal; no automatic replay of an external write solely because C++ threw. |
| Operations sampler; KV expiry worker; detached DNS resolver | Shared process/owner state; DNS context has its own reference-counted lifetime | Status errors are handled locally. Unexpected exceptions are fatal through the centralized trampoline; do not silently lose a worker. |
| C registrations and signal leaf | `FatalCallback` is the sole throwing-capable C entry adapter; signal handler only sets `sig_atomic_t` | No C++ exception crosses C frames. Unknown failures log and terminate; signal handler performs no allocation/logging. |
| Startup / `main` / composition | Owned service factories return status on expected construction/configuration failure | Unhandled startup exceptions are fatal with a trusted context. No healthy listener is advertised on a failed composition. |
| Generic `Result<T>` | `T` must be nonthrowing-destructible; moves are conditionally noexcept according to T's move constructor | On throwing assignment the destination is empty with `kInternal`, never an empty success. Source retains T's valid, possibly moved-from state. No promise stronger than T's move guarantee. |
| Byte/object ownership helpers | Standard unique owners guard storage before construction; byte resize swaps only after successful allocation/copy | Throwing constructors release storage and propagate. Object deletion requires nonthrowing destruction and is noexcept. Allocation failure retains existing byte owners. |
| Logger / diagnostic boundary | Fixed formatting buffers; trusted component/context and catch-classification labels | All log entry points are noexcept. A throwing sink is not retried; write a fixed diagnostic and the original safe record directly to stderr. Preserve errno; never copy the sink's exception payload. |

## Why some boundaries remain fatal

A request catch cannot roll back shared memory, undo an external provider write,
recover from undefined behavior or safely resume after a segmentation fault.
Unknown exceptions at a handler can indicate unproved mutation invariants.
Completion callbacks may have freed their context before throwing. Retaining
termination at those boundaries is intentional and covered by death tests. It
is preferable to silent corruption or duplicate completion.

Parser recovery is narrower: the reader's contract permits only this request's
storage and input consumption, before any handler runs. Its injected dependency
is a synchronous borrowed callable parameter, never a stored view or public
runtime test hook. Unknown parser exceptions are therefore safely discarded.

Do not infer request rollback from HTTP 500. If a domain operation committed
before a later response failure, its data remains durable. Clients must use the
domain idempotency and replay rules. No catch restores a database snapshot.

## Diagnostics and failure reporting

Never log `std::exception::what()` at a boundary: it can contain credentials,
provider responses or wallet/message contents. Record only trusted static context
(route template, job class, callback registration) and the caught class category.
HTTP errors carry bounded generic codes. AI errors carry their public codes.
No upstream body is added to logs or operational pages.

Logging is a no-throw effect throughout the process, including storage failure
paths. Containment only in the exception logger is insufficient: a sink throwing
inside a failed KV append could otherwise skip the later recovery-state update.
The logger does not retry a sink that might have partially consumed its record.

## Regression coverage

- `exception_boundary_test`: parsing/handler standard, allocation and unknown
  failure policies; HEAD; next-request progress; route-lock/capture reset;
  metrics; raw partial replies; deferred/reserved ownership and completion races;
  fatal completion/terminal/C/thread/startup boundaries; private-message omission;
  durable multi-record writes before/after a handler throws and reopen checks.
- `base_status_test`: throwing move construction/assignment, destination failure
  invariants, valid source lifetime and subsequent reuse, nonassignable values,
  nonthrowing type traits, existing move-only/value/failure contracts.
- `base_ownership_test`: constructor unwinding, object lifetime/destruction,
  allocation bounds, byte resize retention and standard owner contracts.
- `base_log_test`: all three sink failure classes, fixed stderr fallback, no sink
  retry, errno preservation and omission of private exception text.
- `kv_failure_test`: throwing sinks cannot interrupt failed-append recovery;
  reopen retains previously acknowledged data, rejects incomplete frames and
  allows a subsequent write; fsync ambiguity, crash and compaction cases.
- `content_components_test`: AI provider/claim/pre-save/post-save/status-write
  exception injection, next-job progress, no duplicate reply/provider attempt,
  fixed error codes and no private exception payload in logs; real Store completion
  remains terminal across late status updates and database reopen.
- The durable asynchronous AI replay (`tests/ai_anthropic_async_route_test.py`)
  and the storage and device transaction contract tests: 201 acknowledgement,
  pending/reply state, idempotency and recovery.

`make verify` runs these tests with both compilers, together with the
analyzers, lint and the HTTP/TLS replays (see [verification.md](verification.md)).
When a change touches shared ownership or the base headers, also run the
sanitizer builds (see [sanitizers.md](sanitizers.md)).
