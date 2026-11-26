#!/usr/bin/env python3
"""Replay the frozen TypeScript Hedera HTS Mirror Node contract
(tests/corpus/token_hedera.json, written by
the earlier TypeScript implementation's tests) against
build/fortis_node_token_route_test, then run native-only measurements.

The TypeScript apps are built without the SaucerSwap client (frozen before
that batch), so every native process here runs with the test-only
FORTIS_TEST_TOKEN_NO_SAUCERSWAP=1 seam: market, profile and liquidity answer
null in both runtimes. token_saucerswap_test.py covers the client.

The fake upstream serves the provider replies recorded in each case (the
case's own calls first, then the first recorded reply to the same request),
so the native token owner receives the TypeScript bytes. Mirror GETs name
their endpoint host in x-fortis-test-host.

Groups: `main` and `restart` run in two processes over one KV directory (the
restart group reads what `main` persisted); `failover` starts a third process
over a fresh KV directory with FORTIS_TEST_TOKEN_NETWORK_ENDPOINTS listing a
failing fixture Mirror endpoint first, as the TypeScript fixture builds a
second app whose Hedera seed lists it first; `warm` starts a fourth process
over a fresh KV directory for the stats flush, demand projection and holder
warmer.

Native lanes refill by the monotonic clock where the TypeScript lanes used
the fixture clock, so the replay paces the CoinGecko (burst 4, 0.4/s) and
hedera-mirror (burst 15, 1/s) lanes before each case, and waits out the 30 s
breaker a 429 opens.

Each request compares status, the fixture header set, the exact JSON body
bytes or protobuf wire bytes, and the provider request multiset. Concurrent
cases compare both answers (the upstream then answers after 0.3 s so the
second reader joins the first one's in-flight requests). Job cases compare
the job summary and its provider requests.

Native-only checks (no TypeScript counterpart):
  - a cold and warm page (chainMeta, holders, insight): latency, provider
    calls (one token-info GET and one balances GET), RSS;
  - four simultaneous cold pages of one token behind a 0.3 s upstream share
    the two in-flight GETs;
  - host isolation: with the testnet endpoint pointed at the mainnet one
    (test-only override), one token id on `hedera` and `hedera-testnet` at
    once sends four GETs, never two shared ones;
  - 23 simultaneous cached reads against the 2-worker/16-slot token queue;
  - 23 distinct cold tokens at once behind a 0.3 s upstream: every answer is
    200 or 429 and every served page sends exactly its two GETs (a page
    whose GET waits out the lane queue fails soft);
  - 23 tokens whose first token-info GET answers 429 (opening the lane):
    pages fail soft (`errors`, never invented sections or counts);
  - SIGKILL, then the cached pages from the durable KV with no provider call.
Usage: token_hedera_test.py <binary>
"""
import json
import hashlib
import os
import sys
import tempfile
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urlsplit

import discovery_route_harness as harness
import token_ledger_test as ledger
import token_routes_test as routes
import token_tron_test as tron

FIXTURE = "token_hedera.json"
BREAKER_COOLDOWN_S = 31.0
MAINNET = "mainnet-public.mirrornode.hedera.com"
TESTNET = "testnet.mirrornode.hedera.com"
MIRROR_HOSTS = {MAINNET, TESTNET, "mirror-a.fixture.test"}
BALANCES_QUERY = "account.balance=gt:0&limit=100&order=asc"
# Cases whose outcome depends on a foreground GET still being in flight (the
# TypeScript mock answers after 25 ms): the stale holder page's background
# revalidation is refused BACKGROUND_BUSY by the hedera-mirror lane while the
# expired token-info GET is in flight, in both runtimes. The replay's upstream
# answers at once unless delayed.
IN_FLIGHT_S = {"holdersStale": 0.1}


def stale_holder_history(server, upstream, rules, clock_file, tokens, case, stale_page, failures, lanes):
    """The previous stale page queues a background holder revalidation. Its
    Mirror GET can finish before or after this history read, depending on the
    native worker schedule. Both are legitimate atomic KV snapshots. Accept
    only the TypeScript bytes or the same body with that one count observation
    advanced to the stale page's clock, including its exact weak ETag."""
    harness.set_clock(clock_file, case["at"])
    for lane in lanes:
        lane.reserve(case)
    rules.use(case)
    before = len(upstream.calls())
    status, headers, payload = server.request("GET", case["url"], headers={
        "Authorization": "Bearer " + tokens.next(), "Accept": case["accept"]})
    harness.wait_for_calls(upstream, before + len(case["upstream"]), timeout=10.0)
    server.request("GET", "/probe/test-token-idle")
    time.sleep(.05)
    observed = tron.observed_calls(upstream.calls()[before:])
    expected = tron.expected_calls(case)
    if observed != expected:
        failures.append(f"{case['name']}: upstream expected {expected}, actual {observed}")

    old = case["body"]["observations"][0]["at"]
    fresh = datetime.fromtimestamp(stale_page["at"] / 1000, timezone.utc).isoformat(timespec="milliseconds")
    fresh = fresh.replace("+00:00", "Z")
    alternate_body = case["bodyText"].replace(old, fresh)
    alternate_bytes = alternate_body.encode("utf-8")
    alternate = {**case, "bodyText": alternate_body, "headers": {
        **case["headers"], "content-length": str(len(alternate_bytes)),
        "etag": 'W/"token-holder-history-' + hashlib.sha256(alternate_bytes).hexdigest()[:24] + '"'}}
    exact_errors, updated_errors = [], []
    tron.compare_response(case["name"], case, status, headers, payload, exact_errors, [])
    tron.compare_response(case["name"], alternate, status, headers, payload, updated_errors, [])
    if exact_errors and updated_errors:
        failures.extend(exact_errors)
        failures.append(f"{case['name']}: not the post-revalidation atomic snapshot either: {updated_errors}")


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
            extra = {"FORTIS_TEST_PRICE_NOW_FILE": str(clock_file), "FORTIS_TEST_TOKEN_NO_SAUCERSWAP": "1"}
            if spec.get("endpoints"):
                extra["FORTIS_TEST_TOKEN_NETWORK_ENDPOINTS"] = ";".join(
                    f"{primary}={','.join(urls)}" for primary, urls in spec["endpoints"].items())
            harness.set_clock(clock_file, selected[0]["at"])
            server = routes.Server(binary, kv_dir, clock_file, upstream, selected[0]["at"], extra_env=extra)
            lanes = [routes.CoinGeckoLane(), ledger.Lane(MIRROR_HOSTS, 15, 1.0)]
            try:
                for case in selected:
                    if "job" in case:
                        harness.set_clock(clock_file, case["at"])
                        for lane in lanes:
                            lane.reserve(case)
                        rules.use(case)
                        ledger.run_job(server, upstream, case, failures)
                    else:
                        rules.delay = IN_FLIGHT_S.get(case["name"], 0.0)
                        if case["name"] == "holdersStaleHistory":
                            stale_page = next(c for c in cases if c["name"] == "holdersStale")
                            stale_holder_history(server, upstream, rules, clock_file, tokens, case,
                                                 stale_page, failures, lanes)
                        else:
                            tron.run_case(server, upstream, rules, clock_file, tokens, case, failures, lanes, [])
                        rules.delay = 0.0
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


def token(index):
    return f"0.0.{7_000_000 + index}"


class Synthetic:
    """Mirror answers every token with a row named after its id and a
    balances page of (num % 7) holders; CoinGecko lists nothing. With
    `reject_next_info`, the first token-info GET answers 429."""

    def __init__(self):
        self.delay = 0.0
        self.reject_next_info = False
        self.lock = threading.Lock()

    def __call__(self, method, host, target, body):
        if self.delay:
            time.sleep(self.delay)
        split = urlsplit(target)
        if method == "GET" and host in MIRROR_HOSTS and split.path.startswith("/api/v1/tokens/"):
            rest = split.path[len("/api/v1/tokens/"):]
            if rest.endswith("/balances"):
                entity = rest[:-len("/balances")]
                if split.query != BALANCES_QUERY:
                    return 400, b'{"_status":{"messages":[{"message":"bad query"}]}}'
                count = int(entity.rsplit(".", 1)[1]) % 7
                rows = [{"account": f"0.0.{100 + i}", "balance": 1 + i} for i in range(count)]
                return 200, json.dumps({"balances": rows, "links": {"next": None}}).encode()
            with self.lock:
                reject = self.reject_next_info
                self.reject_next_info = False
            if reject:
                return 429, b'{"_status":{"messages":[{"message":"Too many requests"}]}}'
            network = "testnet" if host == TESTNET else "mainnet"
            return 200, json.dumps({
                "token_id": rest, "name": f"Synthetic {rest} {network}", "symbol": "SYN", "decimals": "2",
                "total_supply": rest.rsplit(".", 1)[1], "max_supply": "0", "admin_key": None,
                "custom_fees": {"fixed_fees": [], "fractional_fees": []}, "deleted": False,
                "created_timestamp": "1700000000.000000000"}).encode()
        if host == "api.coingecko.com" and split.path.startswith("/api/v3/coins/markets"):
            return 200, b"[]"
        return 404, b'{"error":"coin not found"}'


def mirror_calls(calls):
    return sorted((host, target) for _, host, target, _, _ in calls if host in MIRROR_HOSTS)


def native_checks(binary, root, failures):
    rules = Synthetic()
    upstream = tron.Upstream(root, rules)
    clock_file = Path(root) / "native-clock"
    kv_dir = Path(root) / "kv-native"
    kv_dir.mkdir()
    now = 1768176000000  # 2026-01-12T00:00:00Z
    tokens = harness.TokenRotation(per_token=40)
    report = {}
    narrow = "?include=identity,holders,insight"
    page_url = f"/v1/networks/hedera/token/{token(1)}/page{narrow}"
    # The testnet endpoint is pointed at the mainnet one, so only the network
    # part of the in-flight key keeps the two apart.
    extra = {"FORTIS_TEST_TOKEN_NETWORK_ENDPOINTS": f"https://{TESTNET}=https://{MAINNET}",
             "FORTIS_TEST_TOKEN_NO_SAUCERSWAP": "1"}

    def get(path, token_value=None):
        return server.request("GET", path, headers={"Authorization": "Bearer " + (token_value or tokens.next()),
                                                    "Accept": "application/json"})

    harness.set_clock(clock_file, now)
    server = routes.Server(binary, kv_dir, clock_file, upstream, now, extra_env=extra)
    try:
        rss_boot = tron.rss_kib(server.process.pid)
        before = len(upstream.calls())
        t0 = time.monotonic()
        status, _, payload = get(page_url)
        cold_ms = (time.monotonic() - t0) * 1000
        observed = mirror_calls(upstream.calls()[before:])
        body = routes.json_or_none(payload) or {}
        expected = sorted([(MAINNET, f"/api/v1/tokens/{token(1)}"),
                           (MAINNET, f"/api/v1/tokens/{token(1)}/balances?{BALANCES_QUERY}")])
        if status != 200 or len(body.get("insight") or []) != 4 or (body.get("holders") or {}).get("count") != 1 \
                or body.get("errors") or (body.get("identity") or {}).get("decimals") != 2:
            failures.append(f"measure: cold page {status} {payload[:400]!r}")
        if observed != expected:
            failures.append(f"measure: cold provider calls {observed}")
        warm = []
        warm_before = len(upstream.calls())
        for _ in range(20):
            t0 = time.monotonic()
            status, _, _ = get(page_url)
            warm.append((time.monotonic() - t0) * 1000)
            if status != 200:
                failures.append(f"measure: warm status {status}")
                break
        warm_calls = len(upstream.calls()) - warm_before
        if warm_calls:
            failures.append(f"measure: {warm_calls} provider calls on warm reads")
        report["cold_and_warm"] = {"cold_ms": round(cold_ms, 1), "provider_calls": observed,
                                   "warm_ms_median": round(sorted(warm)[len(warm) // 2], 1),
                                   "warm_ms_max": round(max(warm), 1), "warm_provider_calls": warm_calls}
        rss_warm = tron.rss_kib(server.process.pid)

        letters = "CDEFGIJKLMNOPQRSTUVWXYZ"

        def one(index, path, out):
            t = time.monotonic()
            try:
                status, _, data = server.request("GET", path, headers={
                    "Authorization": "Bearer fintel_dev_" + letters[index % len(letters)] * 43,
                    "Accept": "application/json"})
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

        # Four cold pages of one token at once: two shared GETs.
        rules.delay = 0.3
        before = len(upstream.calls())
        shared = burst([f"/v1/networks/hedera/token/{token(2)}/page{narrow}"] * 4)
        rules.delay = 0.0
        shared_calls = mirror_calls(upstream.calls()[before:])
        report["four_readers_one_token"] = {"statuses": [s[0] for s in shared], "calls": shared_calls}
        if [s[0] for s in shared] != [200] * 4 or shared_calls != sorted(
                [(MAINNET, f"/api/v1/tokens/{token(2)}"),
                 (MAINNET, f"/api/v1/tokens/{token(2)}/balances?{BALANCES_QUERY}")]):
            failures.append(f"shared in-flight GETs: {report['four_readers_one_token']}")

        # One token id on two networks whose endpoint is the same: four GETs.
        time.sleep(4.0)
        rules.delay = 0.3
        before = len(upstream.calls())
        pair = burst([f"/v1/networks/{network}/token/{token(3)}/page{narrow}"
                      for network in ("hedera", "hedera-testnet")])
        rules.delay = 0.0
        pair_calls = mirror_calls(upstream.calls()[before:])
        names = [((routes.json_or_none(d) or {}).get("identity") or {}).get("name") for _, _, d in pair]
        report["same_id_two_networks"] = {"statuses": [p[0] for p in pair], "calls": pair_calls, "names": names}
        if [p[0] for p in pair] != [200, 200] or len(pair_calls) != 4 or \
                names != [f"Synthetic {token(3)} mainnet"] * 2:
            failures.append(f"in-flight key (network): {report['same_id_two_networks']}")

        # 23 simultaneous cached reads, one seeded device each.
        cached_results = burst([page_url] * len(letters))
        cached = {}
        for status, _, _ in cached_results:
            cached[status] = cached.get(status, 0) + 1
        if cached.get(200, 0) < 16 or set(cached) - {200, 429}:
            failures.append(f"measure: cached burst answered {cached}")

        def tally(results, calls, label):
            statuses, served, failed_soft = {}, 0, 0
            for status, _, data in results:
                statuses[status] = statuses.get(status, 0) + 1
                if status != 200:
                    continue
                page = routes.json_or_none(data) or {}
                errors = page.get("errors") or {}
                complete = len(page.get("insight") or []) == 4 and page.get("holders") is not None
                if complete and not errors:
                    served += 1
                elif errors and set(errors) <= {"chainMeta", "insight", "holders"} and \
                        (("insight" in errors) == (page.get("insight") == [])) and \
                        (("holders" in errors) == (page.get("holders") is None)):
                    failed_soft += 1
                else:
                    failures.append(f"{label}: unexpected page {data[:300]!r}")
            reads = {}
            for _, host, target, _, _ in calls:
                if host in MIRROR_HOSTS:
                    reads[target] = reads.get(target, 0) + 1
            if set(statuses) - {200, 429}:
                failures.append(f"{label}: answered {statuses}")
            return {"statuses": statuses, "complete_pages": served, "failed_soft_pages": failed_soft,
                    "provider_requests": sum(reads.values()), "distinct_requests": len(reads)}

        # 23 distinct cold tokens at once behind a slow upstream.
        harness.set_clock(clock_file, now + 120_000)
        time.sleep(16.0)  # refill the hedera-mirror burst
        rules.delay = 0.3
        before = len(upstream.calls())
        rss_before_cold = tron.rss_kib(server.process.pid)
        t0 = time.monotonic()
        results = burst([f"/v1/networks/hedera/token/{token(i + 10)}/page{narrow}" for i in range(len(letters))])
        seconds = time.monotonic() - t0
        server.request("GET", "/probe/test-token-idle")
        rules.delay = 0.0
        first = tally(results, upstream.calls()[before:], "mirror pressure")
        first["seconds"] = round(seconds, 2)
        if first["distinct_requests"] != first["provider_requests"] or \
                first["provider_requests"] > 2 * (first["complete_pages"] + first["failed_soft_pages"]) or \
                first["provider_requests"] < 2 * first["complete_pages"]:
            failures.append(f"mirror pressure: requests per served page: {first}")

        # 23 tokens whose first token-info GET answers 429: the lane opens
        # and the other pages' reads are refused (single endpoint).
        time.sleep(16.0)
        rules.delay = 0.3
        rules.reject_next_info = True
        before = len(upstream.calls())
        t0 = time.monotonic()
        results = burst([f"/v1/networks/hedera/token/{token(i + 100)}/page{narrow}" for i in range(len(letters))])
        seconds = time.monotonic() - t0
        server.request("GET", "/probe/test-token-idle")
        rules.delay = 0.0
        second = tally(results, upstream.calls()[before:], "mirror throttled")
        second["seconds"] = round(seconds, 2)
        if not second["failed_soft_pages"]:
            failures.append(f"mirror throttled: no page failed soft after the lane opened: {second}")
        rss_after = tron.rss_kib(server.process.pid)
        report.update({
            "cached_burst_statuses": cached,
            "cached_burst_ms_max": round(max(ms for s, ms, _ in cached_results if s == 200), 1),
            "cold_tokens": first,
            "throttled_primary": second,
            "rss_kib": {"boot": rss_boot, "after_warm": rss_warm, "before_cold_bursts": rss_before_cold,
                        "after_bursts": rss_after},
        })

        # SIGKILL, then the same pages from the durable KV.
        server.process.kill()
        server.process.wait()
        harness.set_clock(clock_file, now + 180_000)
        server = routes.Server(binary, kv_dir, clock_file, upstream, now, extra_env=extra)
        after = {}
        for label, url in (("narrow", page_url), ("shared", f"/v1/networks/hedera/token/{token(2)}/page{narrow}")):
            before = len(upstream.calls())
            status, _, payload = get(url)
            body = routes.json_or_none(payload) or {}
            cache = body.get("cache") or {}
            after[label] = {"status": status, "provider_calls": len(upstream.calls()) - before,
                            "hits": {k: (cache.get(k) or {}).get("hit") for k in ("identity", "holders", "insight")},
                            "holders": body.get("holders"), "sections": len(body.get("insight") or [])}
            if status != 200 or after[label]["provider_calls"] or not all(after[label]["hits"].values()) or \
                    after[label]["sections"] != 4:
                failures.append(f"restart {label}: {after[label]}")
        report["after_sigkill"] = after
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
    with tempfile.TemporaryDirectory(prefix="fortis-token-hedera-") as root:
        ran = 0 if os.environ.get("TOKEN_HEDERA_NATIVE_ONLY") else replay(binary, fixture, root, failures)
        report = {} if os.environ.get("TOKEN_HEDERA_REPLAY_ONLY") else native_checks(binary, root, failures)
    if failures:
        print(f"token hedera replay: {len(failures)} failure(s) over {ran} cases")
        for failure in failures:
            print(" -", failure)
        raise SystemExit(1)
    print(f"token hedera replay: {ran} cases match the TypeScript contract")
    if report:
        print("token hedera native checks:", json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
