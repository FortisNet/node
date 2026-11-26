# Node and third-party error classification

This document explains how the node classifies the operational errors it
publishes: which party's boundary a failure was observed at, what is stored,
and what is never published. Operators who read `/errors`, and developers who
report errors from new code, need it. The board itself is described in
[error-reporting.md](error-reporting.md).

`/errors` separates persistent operational summaries into three panels. The same
classification is available in `/errorsz` and in asynchronous console summaries.

| `origin` | Meaning | Examples |
| --- | --- | --- |
| `node` | A failure observed inside the node | Database writes, job processing, invalid local outbound requests |
| `third_party` | An external service returned a failed HTTP response | CoinGecko 429; upstream 401/403 or 5xx; Anthropic refusal |
| `unclassified` | The evidence does not establish either origin | DNS/TLS/connection failures, missing prices, client mapping reports, historical summaries |

Origin describes the observed boundary, not ultimate blame: an upstream 401 can
require fixing local credentials. A 429 alone does not distinguish a per-minute
rate limit from an exhausted monthly quota. No raw upstream error message is
published to infer that distinction.

Each operational record includes `origin`, a stable `provider` label, and
`httpStatus` (an observed status or null), alongside the service, code,
market identifier, count and timestamps. For example, the identifying fields of
a CoinGecko refusal are:

```json
{
  "origin": "third_party",
  "service": "providers",
  "provider": "coingecko",
  "httpStatus": 429,
  "code": "http_rate_limited"
}
```

The shared provider coordinator reports each failed completed logical job once,
after retries settle. A retry that recovers is not a failed job. Local admission
refusals, cancelled calls and shutdown do not become provider errors. Anthropic's
separate conversation transport supplies equivalent context; local AI persistence
failures retain node origin. New integrations use the borrowed `ErrorReporter`
interface and the shared provider classification helper.

Unknown outcomes stay separate. In particular, an unpriced asset alone does not
prove a node bug or a CoinGecko failure. Stored records that have no origin are
kept as unclassified and rewritten with metadata during the next flush. No
provider attribution is fabricated and no incident history is cleared.

The HTTP 5xx route tables are **all-cause HTTP response metrics**.
Operational counts persist across restarts and observe different layers; they are
not unique failed requests and must not be added to the HTTP totals.

## Bounds, privacy and concurrency

The board is limited to 64 summaries and has fixed JSON and persistence
bounds. Keys include origin, provider and status so unrelated failures do not
merge. When full, it replaces the oldest entry of the same origin if one exists;
a new origin can claim the oldest slot. Thus recurring provider incidents cannot
erase every node incident.

Reporting copies borrowed fields synchronously, performs no network/storage or
console I/O and is thread-safe. Reporter implementations can be called concurrently.
`DescribeProviderFailure(provider, result)` is a pure description helper: a null
code means no incident; its provider view borrows the supplied stable label.
Consume that description while the label is still alive. The coordinator
copies its lane before unlocking and reporting, because consumers can
immediately release or reuse the completed job.
The borrowed reporter outlives stopped owners and the coordinator. A running
coordinator slot keeps its original reporter when Start is called again; a
refused stop does not release the dependency lifetime. Flushing saves
dirty generations atomically; failed writes retain pending summaries for retry.

Callers supply only stable provider labels and public market metadata; the sink
validates bounded ASCII fields. RPC and feed lane suffixes are replaced by generic labels; URLs, upstream hosts, account
IDs, headers, credentials, wallet contents, AI messages and upstream response
bodies are not copied into diagnostics. Browser text is escaped.

After a successful asynchronous flush, changed summaries emit bounded lines such
as `operational_error origin=third_party service=providers provider=coingecko
code=http_rate_limited http_status=429 total=3`. `origin=node` identifies node
summaries; `origin=unclassified` preserves uncertainty. `http_status=0` means no
status was observed. Counts are cumulative; unchanged summaries are not logged on
every poll. Identifiers and payloads are omitted from these console lines.

## Tests

`tests/error_board_test.cpp` covers origin and status aggregation, borrowed
label lifetimes, restarts, records without origin, malformed metadata,
full-capacity bounds, counter saturation, concurrent reporting and flushing,
console tags and retention under provider incident churn. The coordinator
tests exercise real 503 and 429 responses over TLS with a generated CA,
cancellation, local lane rejection and shutdown. The AI tests distinguish a
provider refusal from a local persistence failure.
`tests/error_board_route_test.py` checks `/errors` and `/errorsz` through the
real server wiring.

## Rollback-readable persistence

Each typed summary is stored as two records in the same atomic KV frame: a
seven-field v1 base under `ops/error/v1/` and its full typed provenance under
`ops/error-meta/v2/`. An older release that knows only the v1 record can
therefore read current summary counts and timestamps without rejecting newer
records. This matters when a node is rolled back (see
[node-updates.md](node-updates.md)).

A sidecar is applied only when its complete v1 projection matches the base.
If an older writer changes or reuses a slot, the newer reader sees
unclassified evidence, never a stale provider attribution. Single-record v2
summaries written by earlier versions are imported and rewritten on the next
flush. No database reset or snapshot restore is involved.
