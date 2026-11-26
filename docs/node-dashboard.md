# Node dashboard

This document describes the public dashboard of a node: which measurements it shows, where each comes from, how to read it and what it costs to serve. Operators who read the dashboard and developers who change `src/operations` need it.

| Address | Content |
| --- | --- |
| `/dashboard` | Live resource and request charts |
| `/dashboard#resources` | Memory, CPU, storage and database activity |
| `/dashboard#guide` | Explanations of the measurements in plain language |
| `/stats` | Request counts and latency by API group and route |
| `/statusz`, `/statsz`, `/status-history` | The JSON that the pages load |

The HTML is embedded in the program (`src/operations/dashboard_page.inc`). It is rendered once at startup and uses no CDN, remote font, tracking script or JavaScript package.

## Measurements

| Measurement | Source and scope | How to read it |
| --- | --- | --- |
| API responses and 5xx rate | Completion telemetry for tracked `/v1/` route templates | Five-minute buckets, including the current partial minute. Refusals count as responses. No traffic means no error-rate sample |
| Identified devices | Bounded sets of device hashes | Five-minute buckets. Excludes traffic without a device, can undercount and is not a count of people. No hashes are published |
| Group and route latency | Server-side completion histograms | Fifteen-minute buckets. Includes error and refusal responses. Excludes client network, connection setup and rendering time |
| 4xx, 429, 5xx | The same route, method and group aggregates | 429 is a subset of 4xx. Client refusals and server failures are separate |
| Queue and spot-owner timings | HTTP completion details | Recorded where the admission or owner path is instrumented. The phases overlap; do not add them |
| Process CPU | Process CPU clock divided by elapsed monotonic time | The card compares two successive status reads. 100% is one logical core, and the value can exceed it |
| CPU history | A separate background baseline | A sample about every five seconds; minimum, average and maximum per minute. It need not match the card's interval |
| Process memory | Linux procfs resident pages, or macOS `task_info` | Resident memory, which is more than heap allocations. It is not host-wide memory use |
| Peak RSS, virtual bytes, threads | Linux `/proc/self/status` | Null where the platform does not report them. Virtual address space is not physical memory |
| Open descriptors | Numeric entries in `/proc/self/fd` | Excludes the sampler's own descriptor. Counted up to 65,536. No targets or paths are disclosed |
| Process storage reads and writes | Linux `/proc/self/io` `read_bytes` and `write_bytes` | Cumulative process accounting. Not network bytes, device bandwidth or IOPS. Cached reads can add no physical read bytes |
| Cgroup memory and CPU quota | The process's cgroup v2 group, found through `/proc/self/cgroup` | Accounting and limits of that group, which can include cache and other processes. An ancestor group can impose a tighter limit. A missing or unlimited quota is null. No group paths are disclosed |
| Used and available storage | `statvfs` on the data filesystem | Sampled about every 30 seconds in the background. Used is total blocks minus free blocks. Available excludes reserved blocks. The filesystem can hold files that are not the node's |
| Data-tree fallback | A bounded scan of allocated bytes, when filesystem capacity is unavailable | Does not follow symbolic links; at most 4,096 entries and four levels. An incomplete scan is not published as a measurement, and no percentage is derived from it |
| Database file, live bytes, keys, compaction | In-memory counters of the key-value store | Different from filesystem use. The sample age and recovery state are shown. The dashboard reads no keys or values |
| Snapshot and cache reuse | Price publication, authorization cache and price store counters | Readiness, age, sequence and lifetime hit and miss counts. A hit count is not a hit rate. Readiness of the snapshot does not prove that every asset is present |
| Capacity events | Admission counters | Counted since process start, not over the 15-minute traffic window. Events can overlap; this is not a count of rejected users |

A measurement that the platform does not provide is shown as "Not reported", not as zero. A sampled zero is a real value. The pages do not derive drive type, temperature, host load, network bandwidth, total capacity or per-route CPU from unrelated counters, and they show no efficiency score.

## History and percentiles

The node keeps 480 one-minute slots in memory, which is eight hours. History starts when the process starts and is lost when it stops.

- `observed=false` marks a minute without observation, as distinct from an observed idle minute. Charts break at gaps and state the coverage they have.
- Disk history holds only fresh filesystem samples, not repeats of a cached reading. Each minute declares its `diskScope`: filesystem capacity or data-tree allocation. A change of scope resets that minute's range, and the browser compares only samples of the current scope.
- Group totals merge the counts, sums and histograms of their routes on the server and then estimate percentiles. They are not averages of route percentiles. Example: with 99 requests at 10 ms and one at 1,000 ms, the group p95 is in the 10 ms bucket.
- Histogram bounds are 5, 10, 25, 50, 100, 250, 500, 1,000, 2,500 and 5,000 ms. The overflow bucket reports the observed maximum.
- Tables show the average, p50, p95, p99, maximum, refusals, errors and sample counts.

Telemetry tables are bounded. The router holds at most 96 routes, fewer than the telemetry template table. If the sparse per-minute table fills up, the node reports the number of dropped samples as `omittedRouteSamples`. Route and group totals then undercount, platform totals do not, and the page shows a warning. An empty latency aggregate has zero values in the JSON; the browser shows "no measurement" when the request count is zero.

## Cost

In the browser:

- Status and statistics refresh about every ten seconds, history about every thirty.
- Requests do not overlap, each has a timeout, and updates can be paused.
- After a failed request or malformed JSON the page keeps the last values and shows a stale-data warning.
- Filtering and sorting of routes happen locally, 50 rows a page.
- Charts are inline SVG. They support keyboard inspection and reduced motion.

On the node:

- The eight-hour ring is about 70 KiB. The history response buffer is 384 KiB.
- Procfs and cgroup sampling runs in one background thread every five seconds. Filesystem scans run in the background too, never in an HTTP worker.
- Rendering JSON copies existing samples and counters.
- Loading the dashboard makes no provider request and reads no database values.

## Linked pages

The dashboard links to and explains the other public pages: the error board (`/errors`), the benchmark reports (`/benchmark`), network capabilities (`/networks`), token price freshness (`/tokens`), the AI model inventory (`/ai`), the cache inventory and the release history (`/releases`).

- The cache inventory hides keys and values of private categories.
- The AI inventory shows configuration only.
- `/benchmark` shows benchmark reports that are compiled into the program (`src/operations/benchmark_reports.inc`). They are records of past runs, shown separately from live activity. Its phase-rate chart uses differences between successive histograms; overlapping phase events are not unique requests and not a capacity figure.
- When the API documentation is not compiled in, both documentation links are disabled.

## Verification

- `resource_snapshot_test` covers a real zero against missing data, unsigned limits, invalid units and counters, fractional and unlimited quotas, cgroup selection, path bounds and bounded serialization.
- `api_telemetry_test` covers weighted group percentiles, omitted samples, scope changes and rollover.
- `operations_pages_test` covers buffer bounds and disabled documentation links.
- `tests/operations_routes_test.py` and `tests/statusz_route_test.py` replay the routes against the real program.

All of these run in `make test`, except the browser test, which is run by hand. It needs Node.js and Chrome or Chromium; set `CHROME` to its path if it is not found:

```sh
node tests/reference/dashboard_browser_test.mjs build/fortis_node_operations_test build/dashboard-evidence
```

It starts an isolated node and a fresh browser profile. The HTML comes from the real program; the metrics are local fixtures. It covers desktop and mobile navigation, filtering and pagination, keyboard chart inspection, gaps, known zeros, unsupported values, omitted samples, failed and malformed responses, pausing and the benchmark chart. It writes screenshots and `browser-result.json` to the output directory.

References for the operating-system counters: [Linux procfs](https://docs.kernel.org/filesystems/proc.html) and [cgroup v2](https://docs.kernel.org/admin-guide/cgroup-v2.html).
