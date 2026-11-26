#!/usr/bin/env python3
"""Compare mixed Portfolio traffic and device writes on an enrolled canary cohort."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import json
import secrets
import ssl
import time
from pathlib import Path
from threading import Event

import portfolio_load_test as load

def counters(client):
    code, _, body, _ = client.request("/statusz")
    if code != 200:
        raise RuntimeError(f"status sample failed: HTTP {code}")
    status = json.loads(body)
    stats_code, _, stats_body, _ = client.request("/statsz")
    if stats_code != 200:
        raise RuntimeError(f"admission sample failed: HTTP {stats_code}")
    admission = json.loads(stats_body)["admissionSinceBoot"]
    activity = status["databaseActivity"]
    snapshot = status["priceSnapshot"]
    auth = status["snapshotAuth"]
    dispatch = status["httpTiming"]["dispatch"]
    return {"sample_ms": activity["sampleMs"], "uptime_sec": status["uptimeSec"],
            "mode": activity["groupCommit"], "frames": activity["writeFrames"],
            "syncs": sum(activity["fileSync"]),
            "snapshot_unavailable": snapshot["unavailable"],
            "auth_hits": auth["hits"], "auth_misses": auth["misses"],
            "dispatch": dispatch, "timing_bounds_us": status["httpTiming"]["boundsUs"],
            "admission": admission, "price_store": status.get("priceStore")}


def run(args, tokens):
    context = ssl.create_default_context()
    run_id = secrets.token_hex(8)

    def reader(token=None, request_id=None):
        return load.Client(args.base_url, context, 15, token, request_id)

    status_client = reader()
    all_samples = {name: [] for name in ("snapshot", "spot", "rotate", "revoke", "health")}
    transport_errors = {}
    rounds = []
    warm_client = reader(tokens[0])
    warm_spot = warm_client.measure("spot")[0]
    warm_snapshot = warm_client.measure("snapshot")[0]
    preconditions = {"spot_http": warm_spot.status, "spot_fresh_ids": len(warm_spot.ids),
                     "snapshot_http": warm_snapshot.status, "snapshot_fresh_ids": len(warm_snapshot.ids)}
    if warm_spot.status != 200 or warm_snapshot.status != 200:
        raise RuntimeError(f"price warm-up failed: {preconditions}")
    # Check credentials with the same authority before each block. Two
    # workers keep the preflight lightweight.
    def preflight(token):
        client = reader(token)
        status = client.request("/v1/prices/snapshot", extra={"If-None-Match": "*"})[0]
        if status == 503:
            time.sleep(1)
            status = client.request("/v1/prices/snapshot", extra={"If-None-Match": "*"})[0]
        return status

    active = [i for i, token in enumerate(tokens) if token]
    with ThreadPoolExecutor(max_workers=2) as pool:
        for i, status in zip(active, pool.map(preflight, (tokens[i] for i in active)), strict=True):
            if status == 401 and i >= 100:
                tokens[i] = None
            elif status != 304:
                raise RuntimeError(f"credential preflight returned HTTP {status} for slot {i}")
    spares = [i for i, token in enumerate(tokens[100:], 100) if token]
    if len(spares) < 10 + 3 * (args.rounds - 1):
        raise RuntimeError("not enough valid mutation devices after credential preflight")
    rotations = spares[:7]
    revocations = spares[7:7 + 3 * args.rounds]
    for index in range(args.rounds):
        if index:
            time.sleep(5)
        before = counters(status_client)
        if before["mode"] != args.group_commit:
            raise RuntimeError("unexpected group commit mode")
        gate = Event()
        def price(i, route):
            gate.wait()
            return reader(tokens[i], f"{run_id}-{index}-{route}-{i}").measure(route)[0]

        def mutate(i, name):
            gate.wait()
            client = reader(tokens[i], f"{run_id}-{index}-{name}-{i}")
            code, _, body, started = client.request("/v1/devices/" + name, method="POST")
            elapsed = (time.monotonic() - started) * 1000
            expected = 200 if name == "rotate" else 204
            if code == expected:
                tokens[i] = json.loads(body)["deviceToken"] if name == "rotate" else None
                outcome = "ok"
            else:
                # The mutation might have committed before the connection
                # closed. Never replay an ambiguous write with this bearer.
                if code == 401 or not code or code >= 500:
                    tokens[i] = None
                outcome = "transport_error" if not code else "http_error"
            return load.Sample(code, elapsed, result=outcome,
                               error_code=load.response_error(body) if code >= 400 else "",
                               **client.trace()), client

        def health():
            gate.wait()
            client = reader(request_id=f"{run_id}-{index}-health-0")
            code, _, _, started = client.request("/healthz")
            return load.Sample(code, (time.monotonic() - started) * 1000,
                               result="ok" if code == 200 else "health_error", **client.trace())

        mutations = [(i, "rotate") for i in rotations if tokens[i]]
        mutations += [(i, "revoke") for i in revocations[3 * index:3 * index + 3]]
        with ThreadPoolExecutor(max_workers=212) as pool:
            jobs = [(route, i, pool.submit(price, i, route))
                    for i in range(100) for route in ("snapshot", "spot")]
            jobs += [(name, i, pool.submit(mutate, i, name)) for i, name in mutations]
            jobs.append(("health", 0, pool.submit(health)))
            started_ms = time.time_ns() // 1_000_000
            gate.set()
            probes = []
            for name, slot, future in jobs:
                result = future.result()
                if name in ("rotate", "revoke"):
                    sample, client = result
                    if sample.ok:
                        probes.append((sample, client))
                else:
                    sample = result
                all_samples[name].append(sample)
                if sample.transport_failure:
                    transport_errors[f"{run_id}-{index}-{name}-{slot}"] = {
                        "phase": sample.transport_failure, "local_port": sample.local_port}
            finished_ms = time.time_ns() // 1_000_000
        # /statusz is memoized. Give it a new second to reflect the full burst.
        time.sleep(1.05)
        after = counters(status_client)
        if (after["mode"] != before["mode"] or after["sample_ms"] <= before["sample_ms"]
                or after["uptime_sec"] < before["uptime_sec"]):
            raise RuntimeError("canary mode changed, restarted or returned an outdated sample")
        deltas = {name: after[name] - before[name] for name in
                  ("frames", "syncs", "snapshot_unavailable", "auth_hits", "auth_misses")}
        deltas["dispatch_buckets"] = [end - start for start, end in
                                     zip(before["dispatch"], after["dispatch"], strict=True)]
        deltas["admission"] = {name: value - before["admission"][name]
                               for name, value in after["admission"].items()}
        counters_to_check = (*deltas["dispatch_buckets"], *deltas["admission"].values())
        if min(counters_to_check) < 0 or after["frames"] < before["frames"] or after["syncs"] < before["syncs"]:
            raise RuntimeError("canary counters reset")
        deltas["timing_bounds_us"] = after["timing_bounds_us"]
        if before["price_store"] is not None and after["price_store"] is not None:
            deltas["price_store"] = {key: value - before["price_store"][key]
                                     for key, value in after["price_store"].items()}
            if min(deltas["price_store"].values()) < 0:
                raise RuntimeError("price store counters reset")
        deltas["started_ms"] = started_ms
        deltas["finished_ms"] = finished_ms
        rounds.append(deltas)
        # Check atomic invalidation outside the measured window so the
        # validation traffic does not compete with the 211 burst requests.
        for sample, client in probes:
            client.benchmark_request_id = None
            denied, _, _, _ = client.request("/v1/prices/snapshot", extra={"If-None-Match": "*"})
            if denied != 401:
                sample.result = "old_bearer_accepted" if denied in (200, 304) else "authorization_probe_error"
        args.cohort.write_text(json.dumps(tokens))
    routes = {name: load.summarize(samples) for name, samples in all_samples.items()}
    for name, samples in all_samples.items():
        routes[name]["error_codes"] = dict(Counter(sample.error_code for sample in samples if sample.error_code))
        if name in ("spot", "snapshot"):
            routes[name]["responses_with_stale"] = sum(sample.stale_ids > 0 for sample in samples)
            routes[name]["responses_with_missing"] = sum(sample.missing_ids > 0 for sample in samples)
            quote_split = load.CORE if name == "snapshot" else load.CORE | set(load.DEFAULT_IDS)
            routes[name]["fresh_by_id"] = {
                key: sum(key in sample.ids for sample in samples) for key in sorted(quote_split)}
    skipped_rotations = args.rounds * 7 - len(all_samples["rotate"])
    errors = {name: sum(sample.status != (204 if name == "revoke" else 200) or
                        sample.result not in ("ok", "incomplete") for sample in samples)
              for name, samples in all_samples.items()}
    price_store = Counter()
    for window in rounds:
        price_store.update(window.get("price_store", {}))
    return {"mode": "group-commit-canary", "group_commit": args.group_commit,
            "benchmark_run_id": run_id,
            "transport_errors_by_request_id": transport_errors,
            "users": 100, "rounds": args.rounds, "routes": routes, "errors": errors,
            "flushes": sum(row["syncs"] for row in rounds), "frames": sum(row["frames"] for row in rounds),
            "windows": rounds, "preconditions": preconditions,
            **({"price_store": dict(price_store)} if price_store else {}), "skipped_rotations": skipped_rotations,
            **({"latency_samples_ms": {
                name: {"all": [sample.ms for sample in samples],
                       "successful": [sample.ms for sample in samples if sample.ok]}
                for name, samples in all_samples.items()}} if args.latency_samples else {})}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--cohort", type=Path, required=True, help="private JSON token list, updated after mutations")
    parser.add_argument("--rounds", type=int, default=3, choices=range(1, 4))
    parser.add_argument("--group-commit", action="store_true")
    parser.add_argument("--latency-samples", action="store_true",
                        help="include unrounded timings for combining repeated blocks")
    args = parser.parse_args()
    args.base_url = load.base_url(args.base_url)
    if args.cohort.stat().st_mode & 0o077:
        parser.error("cohort file must be owner-only")
    tokens = json.loads(args.cohort.read_text())
    if len(tokens) < 100 or not all(tokens[:100]):
        parser.error("100 active reader tokens required")
    required_spares = 10 + 3 * (args.rounds - 1)
    if sum(bool(token) for token in tokens[100:]) < required_spares:
        parser.error(f"{required_spares} active mutation tokens required for {args.rounds} bursts")
    try:
        report = run(args, tokens)
    finally:
        args.cohort.write_text(json.dumps(tokens))
    print(json.dumps(report))


if __name__ == "__main__":
    main()
