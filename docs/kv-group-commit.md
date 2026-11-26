# Key-value store: group commit

This document describes the optional group commit of the node's key-value store: what it does, its limits, what it guarantees after a failure and what has been measured. Operators who consider enabling it and developers who change `src/storage/kv_commit.cpp` need it.

Group commit is off by default. [kv-capacity.md](kv-capacity.md) describes the store's durability protocols and the benchmark.

## Enabling

Set `FORTIS_KV_GROUP_COMMIT=1` and restart the node. Unset it or set it to `0` for the default behavior. Both modes use the same log format, so the setting can be changed at any restart. `/statusz` reports the active mode as `databaseActivity.groupCommit`.

## How it works

Without group commit, every write call appends its own frame and syncs it.

With group commit, concurrent writers queue up. The writer at the head of the queue becomes the leader. It collects the requests behind it into one group, writes them as one checksummed frame, syncs once and publishes the result to all of them.

| Limit | Value |
| --- | --- |
| Queue, including the active group | 128 requests and 1 MiB of encoded mutations |
| One group | 32 requests, 256 mutations and 256 KiB of encoded mutations |
| Wait for company | A leader that is alone waits once, 150 µs, before it takes the store mutex |

- When the queue is full, the write fails with the store's ordinary error.
- A request that is too large for a group takes the ordinary, ungrouped path.
- The 150 µs is a request to the scheduler. The actual wake-up time depends on scheduling.
- These limits bound the size of a group. They put no deadline on storage I/O and do not bound how long an admitted request takes.

## Guarantees

- **Conditional writes stay serial.** Inside a group, each conditional write sees the result of the accepted mutations before it, including deletions and expiry at the caller's timestamp. A write whose condition fails adds no mutation.
- **One frame, one sync.** All accepted mutations of a group share one frame and one sync and are published together under the store mutex. Reads can still wait for that write.
- **Failure is shared.** If the write fails, no request in the group reports success, and the store's normal recovery rules apply.
- **No writes into a recovered store.** A request that was queued before the store recovered or reopened fails instead of being written into the new generation.
- **An I/O error is ambiguous.** A complete frame can survive although its callers received an error. Callers must treat an I/O failure as "possibly written".

## Tests

`device_transaction_contract_test` runs the device contract with group commit. It checks that a one-time expectation is honored once, that a torn group of several requests is discarded, that a group is replayed after both callers received success, and the case in which the sync succeeds and publication fails. In that last case both requests report an error, access to the store is suspended, and recovery publishes both mutations. The rotation and revocation race tests and the store's concurrency, failure and compaction tests also pass with group commit, and a reader without group commit reopens the log after grouped runs.

The crash tests kill a process. They do not establish durability when the host or the storage device fails.

## Measurements

### Direct store benchmark

```sh
python3 tests/run_kv_capacity.py host --group-commit --seconds 8 --workers 4
```

Measured in November 2026 on a macOS development machine. Both variants made individual unconditional write calls: 4,096 keys, 256-byte values, 80% of traffic on hot keys, compaction running, a driver queue of 1,024. The run offered 100,000 reads and 10,000 writes a second. Latency is measured from the scheduled arrival to completion. Frame counts exclude seeding and compaction.

| Four workers, eight seconds | Default | Group commit |
| --- | ---: | ---: |
| Completed of 880,000 | 854,033 | 864,193 |
| Rejected by the driver | 25,967 | 15,807 |
| Errors after admission | 0 | 0 |
| Write frames | 77,546 | 23,317 |
| Read / write p95 | 6.75 / 6.42 ms | 1.80 / 2.09 ms |
| Read / write p99 | 14.10 / 14.06 ms | 18.62 / 19.20 ms |
| Compactions completed | 37 | 36 |

A burst of 110,000 simultaneous arrivals admitted 1,024 in both modes, as the driver's queue intends. It used 93 write frames by default and 24 with group commit.

In an earlier run with 16 workers and five seconds, both variants completed all 550,000 operations without errors or rejections. Group commit reduced the frames from 50,000 to 11,090 and write p99 from 1.73 to 1.53 ms. That run used a version of the code without the check for a recovered store.

Group commit clearly reduces the number of frames when many writers call at once. The effect on latency depends on the workload: p95 improved and p99 did not in the four-worker run.

### Over HTTPS on a small virtual machine

Measured on 14 November 2026 on a cloud virtual machine with one vCPU, with an external client. The same program ran in both modes. Each mode ran three bursts. A burst consisted of 100 snapshot reads, 100 spot reads, seven rotations, three revocations and one health request. The test checked after each successful rotation or revocation that the old token was refused. Latencies include the client's connection setup, and sync counts include background writes.

| Observation | Group commit off | Group commit on |
| --- | ---: | ---: |
| Device mutations completed, of 30 | 30 | 24 |
| Durable frames | 35 | 24 |
| Syncs | 35 | 24 |

With group commit on, the run also saw 20 snapshot requests answered `503`, eight spot requests that failed in transport, six device requests answered `401` and a slower spot p99.

In both modes the number of syncs equaled the number of frames, so this run did not show that grouping saves syncs. The two modes ran one after the other under different cache, provider and connection conditions, which limits what can be attributed to the setting. In both modes the spot responses did not have complete fresh coverage.

Conclusion: the benefit of group commit is shown only for the direct store benchmark with many concurrent writers, not for this HTTP workload. It stays off by default. [price-performance-audit.md](price-performance-audit.md) describes a repeat of this comparison.
