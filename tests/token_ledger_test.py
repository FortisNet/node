#!/usr/bin/env python3
"""Replay the frozen TypeScript Stellar/Cardano token contract
(tests/corpus/token_ledger.json, written by
the earlier TypeScript implementation's tests) against
build/fortis_node_token_route_test, then run native-only measurements.

The fake upstream serves the provider replies recorded in each case (the
case's own calls first, then the first recorded reply to the same request),
so the native token owner receives the TypeScript bytes. Network GETs (Horizon
and Koios) name their endpoint host in x-fortis-test-host.

Groups: `main` and `restart` run in two processes over one KV directory (the
restart group reads what `main` persisted); `failover` starts a third process
over a fresh KV directory with FORTIS_TEST_TOKEN_NETWORK_ENDPOINTS listing a
failing fixture endpoint before each network's catalog endpoint, as the
TypeScript fixture builds a second app whose seeds list it first; `warm`
starts a fourth process over a fresh KV directory (the fixture's third app)
for the stats flush, demand projection and holder warmer.

Native lanes refill by the monotonic clock where the TypeScript lanes used
the fixture clock, so the replay paces the CoinGecko (burst 4, 0.4/s),
horizon (burst 15, 1/s) and koios (burst 13, 50/min) lanes before each case,
and waits out the 30 s breaker a 429 opens.

Each request compares status, the fixture header set, the exact JSON body
bytes or protobuf wire bytes, and the provider request multiset. Concurrent
cases compare both answers (the upstream then answers after 0.3 s so the
second reader joins the first one's in-flight index GET). Job cases compare
the job summary and its provider requests.

Native-only checks (no TypeScript counterpart):
  - a cold and warm Stellar page: latency, provider calls, RSS;
  - one asset on `stellar` and `stellar-testnet` at once behind a 0.3 s
    upstream: two Horizon reads, one per endpoint host, each page with its
    own network's count (the in-flight key includes the endpoint host);
  - 23 simultaneous cached reads against the 2-worker/16-slot token queue;
  - 23 distinct cold Stellar assets at once behind a 0.3 s upstream: every
    answer is 200 or 429 and every served page reads Horizon exactly once;
    then 23 Cardano units whose first Koios read answers 429, opening the
    lane for 30 s: every shed read fails soft (holders null with
    errors.holders, never an invented count);
  - SIGKILL, then the cached page from the durable KV with no provider call.
Usage: token_ledger_test.py <binary>
"""
import json
import os
import sys
import tempfile
import threading
import time
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

import discovery_route_harness as harness
import token_routes_test as routes
import token_tron_test as tron

FIXTURE = "token_ledger.json"
BREAKER_COOLDOWN_S = 31.0
HORIZON_HOSTS = {"horizon.stellar.org", "horizon-a.fixture.test", "horizon-testnet.stellar.org"}
KOIOS_HOSTS = {"api.koios.rest", "koios-a.fixture.test"}


class Lane:
    """A native lane refilling by the monotonic clock over a set of hosts:
    before a case, wait until it holds a token for every request the case
    makes to them."""

    def __init__(self, hosts, burst, per_second):
        self.hosts = hosts
        self.burst = float(burst)
        self.rate = per_second
        self.tokens = float(burst)
        self.at = time.monotonic()

    def reserve(self, case):
        need = sum(1 for call in case.get("upstream", []) if urlsplit(call["url"]).netloc in self.hosts)
        if not need:
            return
        now = time.monotonic()
        self.tokens = min(self.burst, self.tokens + (now - self.at) * self.rate)
        self.at = now
        if self.tokens < need:
            time.sleep((need - self.tokens) / self.rate + .05)
            self.tokens = float(need)
            self.at = time.monotonic()
        self.tokens -= need


def run_job(server, upstream, case, failures):
    name = case["name"]
    before = len(upstream.calls())
    status, _, body = server.request("GET", f"/probe/test-token-job?job={case['job']}")
    harness.wait_for_calls(upstream, before + len(case["upstream"]), timeout=15.0)
    server.request("GET", "/probe/test-token-idle")
    time.sleep(.05)
    observed = tron.observed_calls(upstream.calls()[before:])
    if status != 200:
        failures.append(f"{name}: job status {status} {body[:200]!r}")
        return
    summary = json.loads(body)
    result = case["result"]
    if isinstance(result, dict):
        for key, value in result.items():
            field = "jobFailed" if key == "failed" else key
            if summary.get(field) != value:
                failures.append(f"{name}: job {key}: {summary.get(field)} != {value} ({summary})")
    elif case["job"] == "stats":
        # Buckets are process-scoped (the main group's process flushed its
        # own at stop): only success is comparable here.
        if summary.get("failed"):
            failures.append(f"{name}: stats flush failed ({summary})")
    elif summary.get("written") != result:
        failures.append(f"{name}: job written {summary.get('written')} != {result} ({summary})")
    expected = tron.expected_calls(case)
    if observed != expected:
        failures.append(f"{name}: upstream\n  expected {expected}\n  actual   {observed}")


def replay(binary, fixture, root, failures):
    cases = fixture["cases"]
    rules = tron.Recorded(cases)
    upstream = tron.Upstream(root, rules)
    shared_kv = Path(root) / "kv"
    shared_kv.mkdir()
    clock_file = Path(root) / "clock"
    tokens = harness.TokenRotation(per_token=40)
    groups = fixture["source"]["groups"]
    ran = 0
    try:
        for group in ("main", "restart", "failover", "warm"):
            selected = [c for c in cases if c["group"] == group]
            if not selected:
                continue
            spec = groups[group]
            kv_dir = shared_kv
            if spec["kv"] == "fresh":
                kv_dir = Path(root) / f"kv-{group}"
                kv_dir.mkdir()
            extra = {"FORTIS_TEST_PRICE_NOW_FILE": str(clock_file)}
            if spec.get("endpoints"):
                extra["FORTIS_TEST_TOKEN_NETWORK_ENDPOINTS"] = ";".join(
                    f"{primary}={','.join(urls)}" for primary, urls in spec["endpoints"].items())
            harness.set_clock(clock_file, selected[0]["at"])
            server = routes.Server(binary, kv_dir, clock_file, upstream, selected[0]["at"], extra_env=extra)
            lanes = [routes.CoinGeckoLane(), Lane(HORIZON_HOSTS, 15, 1.0), Lane(KOIOS_HOSTS, 13, 50 / 60)]
            try:
                for case in selected:
                    if "job" in case:
                        harness.set_clock(clock_file, case["at"])
                        for lane in lanes:
                            lane.reserve(case)
                        rules.use(case)
                        run_job(server, upstream, case, failures)
                    else:
                        tron.run_case(server, upstream, rules, clock_file, tokens, case, failures, lanes, [])
                    ran += 1
                    if any(call["status"] == 429 for call in case.get("upstream", [])):
                        # A 429 opens the lane breaker for 30 s on the
                        # monotonic clock (the TypeScript one cools down on
                        # the fixture clock the next case advances).
                        time.sleep(BREAKER_COOLDOWN_S)
            finally:
                server.stop()
    finally:
        upstream.close()
    return ran


# ---- native-only checks ------------------------------------------------------

B32 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"


def stellar_asset(index):
    digits = []
    n = index
    for _ in range(10):
        digits.append(B32[n % 32])
        n //= 32
    return f"SYN{index % 1000}-" + ("GSYN" + "".join(digits)).ljust(56, "A")


def cardano_unit(index):
    return format(index, "x").rjust(56, "e") + "53594e"


class Synthetic:
    """Horizon and Koios answer every asset with a holder count; CoinGecko
    and DexScreener list nothing."""

    def __init__(self):
        self.delay = 0.0
        self.reject_next_index = False
        self.lock = threading.Lock()

    def __call__(self, method, host, target, body):
        if self.delay:
            time.sleep(self.delay)
        split = urlsplit(target)
        query = parse_qs(split.query, keep_blank_values=True)
        if host in HORIZON_HOSTS and split.path == "/assets":
            code = (query.get("asset_code") or [""])[0]
            # Testnet answers a different count: a shared answer would show.
            accounts = 1000 + len(code) + (500 if host == "horizon-testnet.stellar.org" else 0)
            row = {"asset_code": code, "asset_issuer": (query.get("asset_issuer") or [""])[0],
                   "num_accounts": accounts, "amount": "1000.0000000", "num_claimable_balances": 0,
                   "flags": {"auth_required": False, "auth_revocable": False, "auth_immutable": False,
                             "auth_clawback_enabled": False}}
            return 200, json.dumps({"_embedded": {"records": [row]}}).encode()
        if host in KOIOS_HOSTS and split.path.endswith("/asset_summary"):
            with self.lock:
                reject, self.reject_next_index = self.reject_next_index, False
            if reject:
                return 429, b'{"message":"rate limited"}'
            return 200, b'[{"total_transactions":5,"staked_wallets":10,"unstaked_addresses":2}]'
        if host == "api.dexscreener.com":
            return 200, b'{"schemaVersion":"1.0.0","pairs":null}'
        if host == "api.coingecko.com" and split.path.startswith("/api/v3/coins/markets"):
            return 200, b"[]"
        return 404, b'{"error":"coin not found"}'


def native_checks(binary, root, failures):
    rules = Synthetic()
    upstream = tron.Upstream(root, rules)
    clock_file = Path(root) / "native-clock"
    kv_dir = Path(root) / "kv-native"
    kv_dir.mkdir()
    now = 1768176000000  # 2026-01-12T00:00:00Z
    tokens = harness.TokenRotation(per_token=40)
    report = {}
    url = f"/v1/networks/stellar/token/{stellar_asset(0)}/page?include=identity,holders,insight"

    def get(path, token=None):
        return server.request("GET", path, headers={"Authorization": "Bearer " + (token or tokens.next()),
                                                    "Accept": "application/json"})

    harness.set_clock(clock_file, now)
    server = routes.Server(binary, kv_dir, clock_file, upstream, now)
    try:
        rss_boot = tron.rss_kib(server.process.pid)
        before = len(upstream.calls())
        t0 = time.monotonic()
        status, _, payload = get(url)
        cold_ms = (time.monotonic() - t0) * 1000
        cold_calls = sorted((c[1], urlsplit(c[2]).path) for c in upstream.calls()[before:])
        body = routes.json_or_none(payload) or {}
        if status != 200 or (body.get("holders") or {}).get("source") != "horizon" or not body.get("insight"):
            failures.append(f"measure: cold page {status} {payload[:300]!r}")
        if cold_calls != [("horizon.stellar.org", "/assets")]:
            failures.append(f"measure: cold page provider calls {cold_calls}")
        warm = []
        warm_before = len(upstream.calls())
        for _ in range(20):
            t0 = time.monotonic()
            status, _, _ = get(url)
            warm.append((time.monotonic() - t0) * 1000)
            if status != 200:
                failures.append(f"measure: warm status {status}")
                break
        warm_calls = len(upstream.calls()) - warm_before
        rss_warm = tron.rss_kib(server.process.pid)

        letters = "CDEFGIJKLMNOPQRSTUVWXYZ"

        def one(index, path, out):
            t = time.monotonic()
            try:
                status, _, data = server.request("GET", path, headers={
                    "Authorization": "Bearer fintel_dev_" + letters[index] * 43, "Accept": "application/json"})
            except OSError:
                status, data = "reset", b""
            out[index] = (status, (time.monotonic() - t) * 1000, data)

        def burst(paths):
            out = [None] * len(paths)
            threads = [threading.Thread(target=one, args=(i, paths[i], out)) for i in range(len(paths))]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            return out

        # The same asset path on two networks at once never shares a read.
        asset = stellar_asset(500)
        rules.delay = 0.3
        pair_before = len(upstream.calls())
        pair = burst([f"/v1/networks/{network}/token/{asset}/page?include=holders"
                      for network in ("stellar", "stellar-testnet")])
        rules.delay = 0.0
        pair_hosts = sorted(c[1] for c in upstream.calls()[pair_before:])
        counts = [((routes.json_or_none(data) or {}).get("holders") or {}).get("count") for _, _, data in pair]
        expected_count = 1000 + len(asset.split("-")[0])
        report["same_path_two_networks"] = {"hosts": pair_hosts, "counts": counts}
        if pair_hosts != ["horizon-testnet.stellar.org", "horizon.stellar.org"] or \
                counts != [expected_count, expected_count + 500]:
            failures.append(f"in-flight key: {pair_hosts} {counts} {[p[0] for p in pair]}")

        # 23 simultaneous cached reads, one seeded device each.
        cached_results = burst([url] * len(letters))
        cached = {}
        for status, _, _ in cached_results:
            cached[status] = cached.get(status, 0) + 1
        if cached.get(200, 0) < 16 or set(cached) - {200, 429}:
            failures.append(f"measure: cached burst answered {cached}")

        def tally(results, calls, label, source):
            statuses, served, failed_soft = {}, 0, 0
            for status, _, data in results:
                statuses[status] = statuses.get(status, 0) + 1
                if status != 200:
                    continue
                page = routes.json_or_none(data) or {}
                holders = page.get("holders")
                errors = page.get("errors") or {}
                if (holders or {}).get("source") == source:
                    served += 1
                elif holders is None and errors.get("holders") == "UPSTREAM_UNAVAILABLE":
                    failed_soft += 1
                else:
                    failures.append(f"{label}: unexpected page {data[:300]!r}")
            reads = {}
            for c in calls:
                if c[1] in HORIZON_HOSTS | KOIOS_HOSTS:
                    reads[c[2]] = reads.get(c[2], 0) + 1
            if set(statuses) - {200, 429}:
                failures.append(f"{label}: answered {statuses}")
            if any(n > 1 for n in reads.values()):
                failures.append(f"{label}: an asset was read more than once: {reads}")
            return {"statuses": statuses, "served_pages": served, "failed_soft_pages": failed_soft,
                    "index_reads": sum(reads.values())}

        # 23 distinct cold Stellar assets at once behind a slow upstream.
        harness.set_clock(clock_file, now + 120_000)
        time.sleep(1.0)
        rules.delay = 0.3
        cold_before = len(upstream.calls())
        rss_before_cold = tron.rss_kib(server.process.pid)
        t0 = time.monotonic()
        cold = burst([f"/v1/networks/stellar/token/{stellar_asset(i + 1)}/page?include=identity,holders,insight"
                      for i in range(len(letters))])
        cold_seconds = time.monotonic() - t0
        server.request("GET", "/probe/test-token-idle")
        rules.delay = 0.0
        first = tally(cold, upstream.calls()[cold_before:], "pressure", "horizon")
        first["seconds"] = round(cold_seconds, 2)
        if first["served_pages"] != first["index_reads"]:
            failures.append(f"pressure: served pages and Horizon reads differ: {first}")

        # 23 Cardano units at once; the first Koios read answers 429.
        time.sleep(16.0)  # refill the CoinGecko lane (contract lookups)
        rules.delay = 0.3
        rules.reject_next_index = True
        shed_before = len(upstream.calls())
        t0 = time.monotonic()
        shed = burst([f"/v1/networks/cardano/token/{cardano_unit(i)}/page?include=holders,insight"
                      for i in range(len(letters))])
        shed_seconds = time.monotonic() - t0
        server.request("GET", "/probe/test-token-idle")
        rules.delay = 0.0
        second = tally(shed, upstream.calls()[shed_before:], "lane shed", "koios")
        second["seconds"] = round(shed_seconds, 2)
        if not second["failed_soft_pages"]:
            failures.append(f"lane shed: expected pages failing soft behind the open lane: {second}")
        rss_after = tron.rss_kib(server.process.pid)
        report.update({
            "cold_ms": round(cold_ms, 1),
            "cold_provider_calls": cold_calls,
            "warm_ms_median": round(sorted(warm)[len(warm) // 2], 1),
            "warm_ms_max": round(max(warm), 1),
            "warm_provider_calls": warm_calls,
            "cached_burst_statuses": cached,
            "cached_burst_ms_max": round(max(ms for s, ms, _ in cached_results if s == 200), 1),
            "cold_stellar_assets": first,
            "cold_cardano_units_throttled_index": second,
            "rss_kib": {"boot": rss_boot, "after_warm": rss_warm, "before_cold_burst": rss_before_cold,
                        "after_bursts": rss_after},
        })
        if warm_calls:
            failures.append(f"measure: {warm_calls} provider calls on warm reads")

        # SIGKILL, then the same page from the durable KV.
        server.process.kill()
        server.process.wait()
        harness.set_clock(clock_file, now + 180_000)
        server = routes.Server(binary, kv_dir, clock_file, upstream, now)
        before = len(upstream.calls())
        status, _, payload = get(url)
        body = routes.json_or_none(payload) or {}
        after = {"status": status, "provider_calls": len(upstream.calls()) - before,
                 "holders_hit": (body.get("cache") or {}).get("holders", {}).get("hit"),
                 "holders": body.get("holders")}
        report["after_sigkill"] = after
        if status != 200 or after["provider_calls"] or (body.get("holders") or {}).get("source") != "horizon":
            failures.append(f"restart: {after}")
    finally:
        server.stop()
        upstream.close()
    return report


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = harness.load_fixture(FIXTURE)
    failures = []
    with tempfile.TemporaryDirectory(prefix="fortis-token-ledger-") as root:
        ran = replay(binary, fixture, root, failures)
        report = {} if os.environ.get("TOKEN_LEDGER_REPLAY_ONLY") else native_checks(binary, root, failures)
    if failures:
        print(f"token ledger replay: {len(failures)} failure(s) over {ran} cases")
        for failure in failures:
            print(" -", failure)
        raise SystemExit(1)
    print(f"token ledger replay: {ran} cases match the TypeScript contract")
    if report:
        print("token ledger native checks:", json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
