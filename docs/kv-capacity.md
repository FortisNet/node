# Key-value store: capacity and durability

This document describes how to benchmark the node's embedded key-value store, how to read its timing measurements, how its two durability protocols work and what has been measured so far. Developers who change `src/storage` and operators who choose a durability mode or plan a backup need it.

[kv-group-commit.md](kv-group-commit.md) describes the optional group commit.

## Running the benchmark

The benchmark compiles a driver, creates synthetic data in a temporary directory and runs against the store directly, without HTTP:

```sh
python3 tests/run_kv_capacity.py host --seconds 5
```

| Option | Effect |
| --- | --- |
| `--workers N` | Caller threads (default 4) |
| `--queue N` | Driver queue length (default 1,024). Arrivals beyond it are rejected by the driver |
| `--hot-percent N` | Share of requests that go to eight hot keys (default 80) |
| `--batch N` | Size of the batches in the batching variant (default 32) |
| `--parallel-point-reads` | Compare serialized reads with file-pinned reads |
| `--group-commit` | Compare the default store with group commit |
| `--commit-slots` | Compare the two durability protocols |

The benchmark forces compaction and injects failures inside a durable frame. It kills processes; it does not cut power. A process kill does not test what the storage device does on power loss.

The script also has a `guest` mode that runs the workload over HTTP inside a Unikraft virtual machine. That mode needs a guest runtime image that this repository does not build.

## Reading the measurements

`/statusz` reports cumulative histograms under `databaseActivity`. The dashboard shows their p95.

| Measurement | Meaning |
| --- | --- |
| Read and write lock hold | Time a foreground call holds the store mutex |
| Read and write call latency | The whole call, including the wait for the mutex |
| Sweep, compaction and recovery lock hold | Time maintenance holds the mutex |
| File read | One logical positional read, including retries |
| Sync | File and directory syncs, including those of maintenance |
| Timer wake lag | How late a periodic timer in the process woke up |

These intervals overlap; do not add them. Timer wake lag indicates scheduling pressure on the whole process. It cannot tell how long one caller waited to run. The read-hold histogram combines point reads with scans. Look at the overflow bucket and at p99 or the maximum bucket as well as p95.

## Durability protocols

A write is acknowledged only after its frame is durable and the index is updated. Two protocols establish that.

### Rollback marker (default)

Before writing a frame, the store persists the last confirmed log offset in a marker file, `fortis.kv.recover`. It then writes and syncs the frame, removes the marker and syncs the directory. If the marker is still present at startup, the store rebuilds the confirmed prefix in a new file.

The reason for the marker: after a failed `fsync`, Linux can leave the unwritten frame in the page cache, where a later read returns it although the device does not hold it. This was reproduced on ext4 with a device-mapper error target under the log: `fsync` returned `EIO`, a read through the page cache returned the failed frame, and a direct read of the device returned only the previous frame. With the marker, a kill immediately after the failed sync preserved the acknowledged prefix and discarded the failed frame, and later writes survived a reopen.

The cost is four syncs per frame: the marker file, the frame and two directory syncs.

A backup taken while the node is stopped must contain the whole store directory, including `fortis.kv.recover` if it exists.

### Commit slots (optional)

`FORTIS_KV_DURABILITY=commit-slots` replaces the marker file with a commit pointer inside the log. The default value is `rollback-marker`. Any other value stops startup.

The log starts with two slots of 4 KiB each; frames start at byte 8,192. A slot holds a generation, a sequence number and the published end of the log, padded and covered by a CRC-32. Sequence `s` is always stored in slot `s & 1`.

An append writes and syncs the frame, then overwrites the slot that holds the older commit and syncs again. The newest commit is in the other slot and is not touched. Replay uses the newest valid slot and ignores every byte after its published end. A frame that a failed sync left in the page cache is therefore never published, even when it is complete.

The normal path uses two file syncs per frame and no directory sync. With group commit, a group is still one frame.

| Failure | Recovery | The failed frame |
| --- | --- | --- |
| Frame write | Reload the published state in place | Discarded |
| Index update after publication | Reload the published state in place | Kept |
| Frame sync | Copy the published prefix to a new file with a new generation | Discarded |
| Slot write or slot sync | Copy the published prefix, including the synced frame, to a new file | Kept, but the write is not acknowledged |

- A failed sync retires the file. The store does not retry a sync on the same file. The replacement is synced and renamed, and its directory is synced, before the store serves again. If that directory sync fails, the retry publishes a newer generation.
- At every open, the store copies the published frames into a new file with two slots that name the same end, validates the copy, syncs it, renames it over the original and syncs the directory. This makes the state durable even if an earlier process left a readable but unsynced slot in the page cache. An interruption before the rename leaves the original file untouched.
- Compaction writes the new generation's slots into its temporary file before that file is synced and renamed. The rename publishes the log and its pointer together.

**Migration.** The first open with commit slots converts a rollback-marker log. A pending `fortis.kv.recover` is resolved first. The frames are copied to their final offsets in a temporary file and replayed once; a torn tail is trimmed on the copy. The store writes the slots, syncs the file and renames it over the log. A crash before the rename leaves the original log intact. Migration needs free space equal to the size of the log and must finish within the 60-second replay budget.

After migration the default mode refuses to open the log, because it no longer starts with `FKV1`. Releases that do not know commit slots refuse it for the same reason. The store also refuses to open when two valid slots have different generations, when a slot points beyond the end of the file or when a published end lies inside a frame.

**Going back.** [scripts/kv_backup.py](../scripts/kv_backup.py) reads both formats. `backup` and `restore` validate the frames up to the published end and report later bytes as tail. To return to the default mode, stop the node, run `kv_backup.py export-legacy DIR OUT` and then `kv_backup.py restore DIR OUT`, and start the node without the variable. Restore keeps the commit-slot log as `fortis.kv.rollback`.

**Limits.**

- Slot isolation assumes filesystem blocks of 4 KiB or smaller. Consecutive frames can share a block, so the storage stack must preserve acknowledged bytes in a block while it writes later bytes of the same block.
- Opening the store and recovering from a failed sync copy the published log. Both need free space for the copy and use the 60-second replay budget. A failed frame write can discard the unpublished tail in place, also when space is short. If bounded recovery uses up its attempts, the store stays closed.
- Tests kill processes at every stage of an append and of compaction, also on ext4. A device-mapper fault returned a real `EIO` at each sync. The acknowledged prefix survived, and later writes survived another restart. Loss of power on a physical host has not been tested.
- Compaction can fall behind when foreground writes add committed bytes faster than it copies them.

**Tests.** `tests/kv_commit_slots_test.cpp` covers the slot format, the sync count, unpublished tails, each append and slot failure, `SIGKILL` after every append stage, damaged slots, opening in the wrong mode, crashes during migration and compaction, a second crash during restart and group commit. `kv_compaction_test --commit-slots` runs the compaction tests in this mode. The device transaction contract test also runs with commit slots, group commit and parallel reads together.

## Experimental file-pinned point reads

By default every read holds the store mutex for the duration of its file read. The benchmark can enable an experiment in which a reader, while holding the mutex, pins the active file by duplicating its descriptor and records the value's offset. It then releases the mutex and reads on its own thread. At most four readers per store do this at once; beyond that, or when pinning fails, a read takes the normal path. Compaction can replace the active file while pinned readers finish on the old one. Conditional writes and scans are unchanged.

A read that arrives during a write still waits for that write's sync before it can pin the file.

The node itself always uses serialized reads. The experiment is available only in the benchmark, until it has been validated on real virtual machines and storage.

Measured in November 2026, three runs of three seconds per mode, 3,500 reads and 1,000 writes a second, eight callers, 80% hot keys, compaction on, group commit off:

| Environment | Read p99, serialized | Read p99, pinned | Process CPU time per run |
| --- | ---: | ---: | --- |
| Development machine, run directly | 2.87 ms | 1.73 ms | not recorded |
| Linux amd64 container with a quota of one CPU | 8.00 ms | 5.71 ms | 505 ms, then 518 ms |

The values are medians of the three runs. In the container, write p99 moved from 6.51 ms to 5.19 ms. There were no errors and no rejections, and each run completed two compactions. CPU time includes the driver. Runs this short show that the prototype works; they do not establish capacity.

## Measurements

All measurements are from November 2026. None is from production hardware.

### Direct store benchmark

Run directly on a development machine: five seconds, 100,000 reads and 10,000 writes offered per second, four workers, a queue of 1,024, 4,096 keys with 256-byte values, 80% of requests on eight hot keys. This is a storage microbenchmark; threading and sync behavior differ between machines.

| Measurement | One mutation per frame | Batches of up to 32 |
| --- | ---: | ---: |
| Completed reads and writes | 548,322 | 550,000 |
| Rejected by the driver | 1,678 | 0 |
| Durable write frames | 49,844 | 4,997 |
| Read / write p95 | 1.43 / 1.44 ms | 1.45 / 1.44 ms |
| Read / write p99 | 7.54 / 7.56 ms | 1.57 / 1.55 ms |
| Compactions completed | 24 | 23 |

A burst of 110,000 simultaneous arrivals admitted 1,024 operations, as the queue length intends. Crash injection discarded a truncated batch and replayed both mutations of an acknowledged batch.

The batching variant combines up to 32 unconditional mutations in one frame. It measures how much a shared sync saves. It is not the group commit of the store and does not combine conditional writes.

### Over HTTP, Linux process

The diagnostic route `/probe/kv` (enabled with `FORTIS_ENABLE_DIAGNOSTIC_PROBES=1`) was driven on Docker Desktop: the node as an amd64 Linux process with a quota of one CPU and 512 MiB, three windows of six seconds per rate, 24 client workers, about 9% conditional counter writes, group commit off. This run also predates the rollback marker.

| Offered requests per second | Read / write p99 |
| --- | ---: |
| 100 | 7.46 / 15.87 ms |
| 250 | 4.93 / 5.61 ms |

The values are medians of the HTTP percentiles of each window. All requests and all health probes returned 200. After each forced restart the store held all acknowledged increments. The probe route runs under the global route lock, and no compaction ran during these windows, so the run says little about concurrent storage callers.

### Cost of the rollback marker

The same `/probe/kv` workload was run once for six seconds per program version and rate in an emulated x86-64 virtual machine (QEMU without hardware acceleration, one vCPU, 512 MiB). The number of syncs for the two workloads rose from 54 and 136 to 216 and 544, which is the expected factor of four. Write p99 was 17.57 to 36.02 ms at 100 requests a second and 6.80 to 17.58 ms at 250. All requests and health probes completed. Emulation dominates these latencies; the sync counts are the result.

## Open questions

- **Target.** 100,000 reads and 10,000 durable writes a second is a design target for sustained load. The same numbers arriving at one instant are an overload test: the node must reject predictably and recover.
- **Where to measure.** Capacity must be measured on the kind of machine and storage that will run the node, with a separate load generator. Compare offered and completed work, queue age, p99 and p99.9, health, sync time and resource use. Keep the offered rate independent of the responses. Include realistic key counts and value sizes, concurrent rotations and revocations, hot and uniform keys, and compaction.
- **Sync barriers.** Verify that a sync reaches the device, and test failure of the host as well as a kill of the process.
- **Lock split.** Separating the writer from index publication would let reads proceed during a sync. It is deferred until concurrent readers and writers with compaction show material read waits in a representative run. Conditional writes and replay must keep their meaning.
- **Route lock.** Authenticated write routes that still run under the global route lock are serialized before they reach the store. Measure the wait at that lock before judging the store.
- **Alternatives.** Before sharding, compare this store with a mature transactional engine on Linux at equal durability. Sharding is an option only if a per-node limit remains and device changes can stay atomic inside one shard.
- **Cold authorization.** Measure bursts of distinct and of repeated tokens. If repeated cache misses prove expensive, consider coalescing lookups per token hash while keeping the rotation and generation checks.
