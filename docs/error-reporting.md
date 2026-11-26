# Public operational errors and durable AI feedback

This document describes the node's public error board, the endpoint with
which clients report missing price identifiers, and how failures of
asynchronous AI replies reach the client. Operators and client developers
need it. How each error is attributed to the node or to a third party is in
[error-classification.md](error-classification.md).

`/errors` and its `/error` alias display HTTP failure aggregates and a separate
operational error board. `/errorsz` returns both as JSON. These are public,
read-only operational views; they do not require a device credential.

## Operational board

`operations::ErrorBoard` owns 64 in-memory summaries. A summary is identified
by its origin, service, provider, HTTP status, code and public identifier, and
has a count and first/last timestamps. When the board is full, a new incident
replaces the oldest summary of the same origin; an origin that has no summary
yet takes the oldest slot. The summaries are retained incidents. They do not
claim that a past failure is still active, or that every incident since the
process started is still on the board.

`Report` copies bounded fields without storage/network work. The operations
sampler normally flushes dirty summaries every five seconds, outside provider
callbacks and public page rendering. A generation-checked snapshot is written
in one KV transaction, so a concurrent report is not lost when a flush completes.
Shutdown flushes after service owners stop and before the runtime store closes.

Records use `ops/error/v1/<slot>`, with typed metadata under
`ops/error-meta/v2/`, and a validated versioned encoding. Corrupt
records are skipped and produce a stable `invalid_error_record` incident, without
publishing corrupt contents. `persisted` and `writeFailed` expose dirty summaries
and storage failures. Writes retain dirty generations and retry after failure.
If storage cannot open during startup, the server preserves the health/error
surface with an unopened fallback store: diagnostics remain in memory and
persistence failure is visible until storage is repaired and the process restarted.

Only stable classifications and deliberately public market metadata are accepted.
Call sites must never pass provider bodies, exception messages, tokens, wallet
contents or AI messages. Fields are length/character validated. The price path
publishes catalogued native market IDs, while contract or unknown IDs become an
aggregate `quote_unavailable` incident without copying their text.

## Missing client price identifiers

The server catalog and a mobile symbol map solve different problems. If a client
has no symbol mapping, it may never send a price request, even when the server's
catalog is correct. FLR maps to `flare-networks`; HYPE maps to `hyperliquid`.

Clients report absent mappings with:

```http
POST /v1/prices/missing-identifiers
Authorization: Bearer <enrolled-device-token>
Content-Type: application/json

{"symbols":["FLR","HYPE"]}
```

The endpoint applies device authentication, attestation policy, Origin refusal
and a cost of one quota unit. It accepts exactly one `symbols` array, 1–16 ASCII
symbols matching `[A-Z0-9.]{1,16}`, in a body no larger than 1024 bytes. It validates
the entire body before recording anything. Only native symbols present in the
project catalog are published; other symbols increment an identifier-free record.
`204` follows a successful durable flush. Invalid reports return 400/413/415;
a storage failure returns 503 `DIAGNOSTIC_UNAVAILABLE` and remains visible.

A client should send at most 16 missing symbols per report, throttle its
reports, and treat diagnostic delivery as best effort. An unavailable
diagnostic endpoint must not prevent ordinary price delivery.

## AI message acceptance and failures

A conversation open or user-message post returns **201 after the user turn and
pending job are committed atomically**. Provider generation is asynchronous.
A provider outage must not turn an accepted turn into a synchronous 503.

The newest history page reports `x-ai-reply-for` and `x-ai-reply-status`. Pending
jobs become running, then complete or a retryable failure. Failures include:

| Status | Meaning |
| --- | --- |
| `unavailable` | Provider transport/service unavailable or provider not configured |
| `invalid_reply` | A successful provider HTTP response could not be parsed as a complete reply |
| `auth_failed` | Provider authentication/permission failure |
| `no_credits` | Billing or configured usage/spend limit |
| `model_missing` | Configured provider model unavailable |
| `rate_limited` | Provider request/usage rate limit |
| `token_limit` | Provider output was cut short by its token budget |
| `failed` | Other classified generation, context or persistence failure |

Stable operational codes distinguish configuration, context, reply-write,
job-status-write and exception failures. Unexpected generation exceptions are
caught per job, mark it failed and leave the worker available for subsequent jobs.
Request credentials are wiped on every exit, including exceptions. Restart resets
unfinished jobs for recovery; retrying a failed saved turn does not duplicate it.

## What a client should do

- Validate the required acknowledgement and message fields and the
  reply-status headers.
- Treat an empty newest history page as invalid. An empty earlier page is a
  valid pagination result.
- On a malformed response, fail with a protocol error; do not replace cached
  history.
- On a poll failure, keep the saved turn visible and retry. On a provider
  failure, show that the message is saved and offer a retry.
- Never show upstream bodies or raw exception text. The node does not send
  them.
