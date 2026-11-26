# Price store and in-memory quote cache

This document describes the optional separate price store and the in-memory cache of quotes that spot prices and the price snapshot share. Operators who enable `FORTIS_SPLIT_PRICE_STORE` and developers who work on `src/prices` need it.

## What the switch does

`FORTIS_SPLIT_PRICE_STORE=1` requires `FORTIS_NATIVE_PRICE_ROUTE_CANARY=1`. It does three things:

1. It moves price data into its own store, `prices-v1`, in a subdirectory of the data directory.
2. It enables a cache of quotes in process memory.
3. It enables background refresh of recently requested contract tokens.

The device store keeps everything that concerns device authorization: enrollment, rotation, revocation and challenges. The reason for the split is isolation. Each store has one mutex and syncs every write, so in a single store a burst of price writes delays authorization reads, and the reverse.

## The price store

`prices-v1` holds the spot quotes, their metadata, the price history and the sealed series. It has its own lock, sweeper, recovery and compaction. It is on the same filesystem as the device store and shares that filesystem's I/O capacity.

On the first start with the switch, the node copies all live `price/` records from the device store to the new store with their original expiry times, and then writes a migration marker. The copy runs before any worker or sweeper starts. If it is interrupted, the next start repeats it; rows that already exist in the new store are kept.

The old records stay in the device store, so the node can be started without the switch again. It then sees the prices as they were at the migration and refreshes them from the providers. Price writes made after the migration exist only in `prices-v1`.

A backup must contain the whole data directory, which includes both stores.

## The quote cache

The spot resolver and the snapshot publisher share one cache of 4,096 slots. Each slot holds a committed price record exactly as it is stored.

- A hit requires the complete canonical key and the current generation of the store. Recovery or reopening the store changes the generation, which invalidates every cached entry.
- Quote metadata, freshness deadlines, negative entries and last-known values have the same meaning in memory as on disk.
- A miss reads the price store.
- A write from a provider or from the ranked-coin job enters the cache after the durable write, never before.

## Refresh of requested contracts

The spot owner counts requests for contract tokens and other chain-token IDs in a bounded ranking in memory. Case is preserved where an ID is case-sensitive. Every five seconds the warm thread offers up to six IDs that are close to expiry to the spot owner. Provider admission, lane budgets, batch coalescing and negative caching apply to this work as to any other.

Quotes survive a restart. The demand ranking does not; it is rebuilt from new traffic.

## Status

`/statusz` reports lookups in the quote cache, hits for contract and chain-token IDs, durable frames and file and directory syncs, separately for each store. A cache hit counts a lookup by a request or by background work. It does not count users.

## Measurements

The figures below were measured in November 2026 (the second run on 14 November) on a cloud virtual machine with one vCPU, running one node, with an external client. Each run sent three bursts. A burst consisted of 100 spot requests and 100 snapshot requests from 100 enrolled devices, seven rotations, three revocations and one health request. Group commit was off. The runs used different device cohorts and time windows, so differences between them cannot be assigned to one cause.

**First run with the split store**

- The quote cache recorded 7,638 hits, 3,900 of them for contract or chain-token IDs.
- The price store committed two frames with two syncs. The device store committed 30 frames with 30 syncs.
- All 30 device mutations succeeded, and the old credentials were refused afterwards.
- All 300 spot requests succeeded and returned every requested ID. Each response contained at least one stale quote.
- Of 300 snapshot requests, 284 returned six fresh quotes and 16 returned `503` because authorization for the snapshot was limited to two concurrent requests at the time.
- All health requests returned 200.

**Second run, after two changes**

The limit of two concurrent snapshot authorizations was removed; the fixed pool of HTTP workers bounds them instead. Snapshot and spot reads were given one shared authorization cache. Contract refresh was changed to look for IDs that are about to expire.

- All 300 snapshots contained six fresh quotes. There were no authorization refusals.
- All 300 spot responses contained every requested ID, and 281 contained only fresh quotes.
- Snapshot p95 fell from 7.03 s to 1.49 s. Spot p95 rose from 1.43 s to 7.01 s. The p99 of both routes stayed near 7 s. These latencies include connection and TLS setup from the client.

A separate check of 29 samples over about two and a half minutes returned HTTP 200 every time. All six contract and chain-token quotes were fresh in five samples, and all snapshot quotes were fresh in 14. The provider answered `429` during that period. Consistent freshness of contract quotes under provider rate limits is therefore not established.
