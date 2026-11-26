# Release identity and deployment history

This document describes how a running node shows which release it is, which releases ran before it and how healthy its host is. It covers the node's side only: the files the node reads and the pages it serves. Operators who want to check what a node is running, and developers who change `src/operations/release_routes.cpp` or write a host for the node, need it.

The node does not update itself. A separate program on the host, the update controller, verifies signed releases, switches between them, watches a new release and goes back to the previous one when it fails. The controller is part of [FortisNet Node OS](https://github.com/FortisNet/node-os); its [release documentation](https://github.com/FortisNet/node-os/blob/main/docs/releases.md) describes publishing, fetching and installing releases. [native-runtime.md](native-runtime.md) describes how a release is built and signed.

## Design

- **The node only reads.** The controller writes finished, sanitized files into one directory. The host gives that directory to the node read-only. The node cannot start an update, change the history or reach the controller. There is no update endpoint.
- **The executable hash is the identity.** A release is identified by the SHA-256 of its executable and by its signed version label. The source commit in the manifest is extra information; it does not replace the hash.
- **Unknown stays unknown.** A fact that was not observed, such as when an imported release first started serving, is shown as unknown. The pages never fill gaps with invented values.
- **Observations have a time.** Each snapshot carries the time it was written. The page marks a snapshot as stale when it is old, so a reader can tell an observation from a continuous proof of health.

## Files the node reads

`FORTIS_RELEASE_STATUS_DIR` names the directory. The default is `/deployment`. The value must be an absolute path.

| File | Author | Use |
| --- | --- | --- |
| `current.json` | Controller | Current release, candidate release and the number of the latest history page |
| `history-N.json` | Controller | Page `N` of the activation history |
| `index.html` | Host | Replaces the node's built-in home page at `/` |
| `health.html`, `health.json` | Host | The host's health report |
| `health.status` | Host | One line, `STATUS WRITTEN_MS`: the HTTP status to answer with and the time the report was written, in milliseconds since the Unix epoch |

Rules the node applies ([src/operations/release_routes.h](../src/operations/release_routes.h)):

- It reads only regular files and never follows a symbolic link.
- `current.json` and `history-N.json` are smaller than 64 KiB, must parse as JSON within a fixed token budget and must contain `"schemaVersion": 1`. The host's pages are at most 512 KiB.
- A JSON snapshot is cached for one second.
- It forwards file contents unchanged. It adds nothing and exposes no paths.

## Routes

All routes are public, read-only and answered with `Cache-Control: no-store`.

| Route | Answer |
| --- | --- |
| `GET /releases` | HTML page that shows the current release, a candidate and the paginated history. It loads the JSON routes below |
| `GET /releasesz` | `current.json` |
| `GET /releasesz/history?page=N` | `history-N.json`. The `page` parameter is required |
| `GET /health`, `GET /health.json` | The host's health report |

Failure cases:

| Condition | Answer |
| --- | --- |
| No `current.json` | `200` with `{"schemaVersion":1,"managed":false,"current":null,"candidate":null,"latestPage":null,"observedAtMs":null}`. The node is not managed by a controller |
| `current.json` or a history page is unreadable, too large, not JSON or has another schema version | `503` `{"error":"release_metadata_unavailable"}` |
| A history page does not exist | `404` `{"error":"history_not_found"}` |
| Invalid query | `400` `{"error":"invalid_query"}` |
| No health report | `503` `{"error":"health_report_unavailable"}` |
| `health.status` is missing, malformed, older than 60 seconds, dated in the future or states a status other than 200 | The health report is served with status `503` |

A health report that nobody refreshes is not believed: the node answers `200` on `/health` only while the host keeps rewriting `health.status`.

The home page and the dashboard link to `/releases`.

## What the history means

The controller defines the content of the JSON files; the node does not interpret it beyond the schema version. The concepts the page presents are:

- **Release**: one executable, identified by hash and version.
- **Deployment**: one activation of a release. Activating the same release again is a new deployment.
- **Instance**: one lifetime of the node process. A restart is a new instance.
- **Serving interval**: the period in which one instance served, with the events that were observed: process start, readiness, drain, stop and acceptance as stable. Wall times are UTC. A boundary that ends in a crash or power loss is marked as uncertain.

During an update the node is stopped for a short time, and these pages are unavailable until the next instance is ready.

## Compatibility of stored error summaries

A host may go back to an older release that reads the same store. The error board therefore persists each summary in two parts: a record that older releases can read, and a separate record with the newer classification fields. A newer release applies the second record only when it still matches the first. See [error-classification.md](error-classification.md).

## Tests

`tests/release_routes_test.py` covers the routes: absent, valid, oversized and malformed files, pagination, query validation and the health status rules.
