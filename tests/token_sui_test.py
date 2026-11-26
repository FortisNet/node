#!/usr/bin/env python3
"""Replay the frozen TypeScript Sui Coin<T> token metadata contract
(tests/corpus/token_sui.json, written by
the earlier TypeScript implementation's tests) against
build/fortis_node_token_route_test, then run native-only measurements.

The fake upstream serves the provider replies recorded in each case (the
case's own calls first, then the first recorded reply to the same request),
so the native token owner receives the TypeScript bytes. The GraphQL
`FortisCoinMetadata` POSTs name their endpoint host in x-fortis-test-host
and are matched on their JSON body.

Groups: `main` and `restart` run in two processes over one KV directory (the
restart group reads what `main` persisted); `failover` starts a third process
over a fresh KV directory with FORTIS_TEST_TOKEN_NETWORK_ENDPOINTS listing a
failing fixture GraphQL endpoint first, as the TypeScript fixture builds a
second app whose Sui seed lists it first; `warm` starts a fourth process over
a fresh KV directory for the stats flush, demand projection and holder
warmer (which selects no Sui token).

Native lanes refill by the monotonic clock where the TypeScript lanes used
the fixture clock, so the replay paces the CoinGecko (burst 4, 0.4/s) and
each rpc:{host} (burst 5, 20/s) lane before each case, and waits out the
30 s breaker a 429 opens.

Each request compares status, the fixture header set, the exact JSON body
bytes or protobuf wire bytes, and the provider request multiset (POST bodies
included). Recorded native difference: the routing 414 keeps the native
security header baseline (TypeScript routing fails before its hooks); its
status, body bytes, content type and length are still compared. Concurrent cases compare both answers (the upstream then answers
after 0.3 s so the second reader joins the first one's in-flight request).
Job cases compare the job summary and its provider requests.

Native-only checks (no TypeScript counterpart):
  - a cold and warm page: latency, provider calls, RSS;
  - four simultaneous cold readers of one coin type, and its short and
    zero-padded spellings at once, send one POST; two coin types at once send
    two (the request body is part of the in-flight key);
  - 23 simultaneous cached reads against the 2-worker/16-slot token queue;
  - 23 distinct cold coin types at once behind a 0.3 s upstream: every
    answer is 200 or 429 and every served page sends exactly one POST;
  - 23 coin types whose first POST answers 429 (opening the GraphQL lane):
    every served page fails soft (`errors.chainMeta`, chainMeta null, never
    an invented identity), and after the breaker cools down a failed type
    reads again and answers (the failure was never cached as a negative);
  - no GraphQL URL or request header text reaches a response;
  - SIGKILL, then the cached page from the durable KV with no provider call.
Usage: token_sui_test.py <binary>
"""
import json
import os
import re
import sys
import tempfile
import threading
import time
from pathlib import Path
from urllib.parse import quote, urlsplit

import compat_expectations  # noqa: E402
import discovery_route_harness as harness
import token_ledger_test as ledger
import token_routes_test as routes
import token_tron_test as tron

FIXTURE = "token_sui.json"
BREAKER_COOLDOWN_S = 31.0
GRAPHQL_HOSTS = ["graphql.mainnet.sui.io", "graphql-a.fixture.test"]
COIN_TYPE = re.compile(r'coinMetadata\(coinType: "([^"]+)"\)')


# find-my-way answers the 414 before Fastify's hooks; the native router keeps
# its security header baseline (a documented allowance, as in
# token_routes_test.py and address_routes_test.py). Only these headers, and
# only where TypeScript sent none, are excused.
SECURITY_BASELINE = ("cache-control", "x-content-type-options", "referrer-policy", "permissions-policy")


def routing_case(server, upstream, rules, clock_file, tokens, case, failures, lanes):
    # fastify-errors: the 414 is {"error":"URI_TOO_LONG"} as
    # application/json; charset=utf-8.
    if "body" in case and compat_expectations.fastify_code(case["body"]):
        mapped = compat_expectations.fastify_case(dict(case, body=json.dumps(case["body"])), text_key="body")
        case = dict(mapped, body=json.loads(mapped["body"]))
    own = []
    tron.run_case(server, upstream, rules, clock_file, tokens, case, own, lanes, [])
    excused = tuple(f"{case['name']}: header {h}: " for h in SECURITY_BASELINE if h not in case["headers"])
    failures.extend(f for f in own if not (f.startswith(excused) and f.endswith("!= None")))


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
            lanes = [routes.CoinGeckoLane()] + [ledger.Lane({host}, 5, 20.0) for host in GRAPHQL_HOSTS]
            try:
                for case in selected:
                    if "job" in case:
                        harness.set_clock(clock_file, case["at"])
                        for lane in lanes:
                            lane.reserve(case)
                        rules.use(case)
                        ledger.run_job(server, upstream, case, failures)
                    elif case.get("status") == 414:
                        routing_case(server, upstream, rules, clock_file, tokens, case, failures, lanes)
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

def coin(index):
    return f"0xe5{index:06x}::synthetic::S{index}"


class Synthetic:
    """GraphQL answers every coin type with a registered row; CoinGecko lists
    nothing. `reject_next` makes the next GraphQL POST answer 429."""

    def __init__(self):
        self.delay = 0.0
        self.reject_next = False
        self.lock = threading.Lock()

    def __call__(self, method, host, target, body):
        if self.delay:
            time.sleep(self.delay)
        if method == "POST" and host in GRAPHQL_HOSTS:
            with self.lock:
                reject = self.reject_next
                self.reject_next = False
            if reject:
                return 429, b'{"errors":[{"message":"too many requests"}]}'
            query = (json.loads(body or b"{}").get("query") or "")
            match = COIN_TYPE.search(query)
            name = match.group(1) if match else "?"
            return 200, json.dumps({"data": {"coinMetadata": {
                "decimals": 9, "symbol": "SYN", "name": f"Synthetic {name.rsplit('::', 1)[-1]}"}}}).encode()
        if host == "api.coingecko.com" and urlsplit(target).path.startswith("/api/v3/coins/markets"):
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

    def page(ref, include="identity,holders,insight"):
        return f"/v1/networks/sui/token/{quote(ref, safe='')}/page?include={include}"

    url = page(coin(0))

    def get(path, token=None):
        return server.request("GET", path, headers={"Authorization": "Bearer " + (token or tokens.next()),
                                                    "Accept": "application/json"})

    def posts_since(before):
        out = []
        for method, host, _, body, _ in upstream.calls()[before:]:
            if method == "POST":
                match = COIN_TYPE.search(json.loads(body or b"{}").get("query") or "")
                out.append((host, match.group(1) if match else None))
        return sorted(out)

    def leaks(payload):
        return any(text in payload for text in (b"graphql.mainnet.sui.io", b"graphql-a.fixture.test",
                                                b"FortisCoinMetadata", b"x-fortis-test-host"))

    harness.set_clock(clock_file, now)
    server = routes.Server(binary, kv_dir, clock_file, upstream, now)
    try:
        rss_boot = tron.rss_kib(server.process.pid)
        before = len(upstream.calls())
        t0 = time.monotonic()
        status, _, payload = get(url)
        cold_ms = (time.monotonic() - t0) * 1000
        observed = sorted((c[0], c[1], urlsplit(c[2]).path) for c in upstream.calls()[before:])
        body = routes.json_or_none(payload) or {}
        identity = body.get("identity") or {}
        if status != 200 or identity.get("symbol") != "SYN" or identity.get("decimals") != 9 or \
                body.get("holders") is not None or body.get("insight") != [] or body.get("errors"):
            failures.append(f"measure: cold page {status} {payload[:300]!r}")
        # The coin type also resolves its CoinGecko contract (sui platform)
        # on its first read, as TypeScript does.
        expected = [("GET", "api.coingecko.com", f"/api/v3/coins/sui/contract/{quote(coin(0), safe='')}"),
                    ("POST", "graphql.mainnet.sui.io", "/graphql")]
        if observed != expected:
            failures.append(f"measure: cold provider calls {observed}")
        if leaks(payload):
            failures.append("measure: GraphQL endpoint or query text in a response")
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

        # One coin type, four readers at once: one POST.
        rules.delay = 0.3
        before = len(upstream.calls())
        shared = burst([page(coin(1))] * 4)
        rules.delay = 0.0
        report["four_readers_one_type"] = {"statuses": [s[0] for s in shared], "posts": posts_since(before)}
        if [s[0] for s in shared] != [200] * 4 or posts_since(before) != [("graphql.mainnet.sui.io", coin(1))]:
            failures.append(f"in-flight sharing: {report['four_readers_one_type']}")

        # Short and zero-padded spellings at once: one canonical POST.
        short = coin(2)
        padded = short.replace("0xe5", "0x" + "0" * 40 + "e5")
        rules.delay = 0.3
        before = len(upstream.calls())
        spellings = burst([page(short), page(padded)])
        rules.delay = 0.0
        report["spellings_one_type"] = {"statuses": [s[0] for s in spellings], "posts": posts_since(before)}
        if [s[0] for s in spellings] != [200, 200] or posts_since(before) != [("graphql.mainnet.sui.io", short)]:
            failures.append(f"spelling sharing: {report['spellings_one_type']}")

        # Two coin types at once: the body is in the in-flight key.
        rules.delay = 0.3
        before = len(upstream.calls())
        pair = burst([page(coin(3)), page(coin(4))])
        rules.delay = 0.0
        report["two_types_two_posts"] = {"statuses": [s[0] for s in pair], "posts": posts_since(before)}
        if [s[0] for s in pair] != [200, 200] or posts_since(before) != sorted(
                [("graphql.mainnet.sui.io", coin(3)), ("graphql.mainnet.sui.io", coin(4))]):
            failures.append(f"in-flight key (body): {report['two_types_two_posts']}")

        # 23 simultaneous cached reads, one seeded device each.
        cached_results = burst([url] * len(letters))
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
                if leaks(data):
                    failures.append(f"{label}: GraphQL endpoint or query text in a response")
                body = routes.json_or_none(data) or {}
                errors = body.get("errors") or {}
                identity = body.get("identity") or {}
                if body.get("holders") is not None or "holders" in errors or body.get("insight") != []:
                    failures.append(f"{label}: a holder or insight answer {data[:300]!r}")
                if identity.get("symbol") == "SYN" and not errors:
                    served += 1
                elif identity.get("symbol") is None and identity.get("decimals") is None and \
                        errors == {"chainMeta": "UPSTREAM_UNAVAILABLE"}:
                    failed_soft += 1
                else:
                    failures.append(f"{label}: unexpected page {data[:300]!r}")
            posts = [c for c in calls if c[0] == "POST"]
            types = {COIN_TYPE.search(json.loads(c[3] or b"{}").get("query") or "").group(1) for c in posts}
            if set(statuses) - {200, 429}:
                failures.append(f"{label}: answered {statuses}")
            return {"statuses": statuses, "served_pages": served, "failed_soft_pages": failed_soft,
                    "posts": len(posts), "distinct_posts": len(types)}

        # 23 distinct cold coin types at once behind a slow upstream.
        harness.set_clock(clock_file, now + 120_000)
        time.sleep(1.0)
        rules.delay = 0.3
        before = len(upstream.calls())
        rss_before_cold = tron.rss_kib(server.process.pid)
        t0 = time.monotonic()
        results = burst([page(coin(100 + i)) for i in range(len(letters))])
        seconds = time.monotonic() - t0
        server.request("GET", "/probe/test-token-idle")
        rules.delay = 0.0
        first = tally(results, upstream.calls()[before:], "cold pressure")
        first["seconds"] = round(seconds, 2)
        if first["posts"] != first["served_pages"] + first["failed_soft_pages"] or \
                first["distinct_posts"] != first["posts"] or not first["served_pages"]:
            failures.append(f"cold pressure: posts per served page: {first}")

        # 23 coin types whose first POST answers 429: the lane opens and the
        # rest fail soft (one endpoint, nothing to fail over to).
        time.sleep(1.0)
        rules.delay = 0.3
        rules.reject_next = True
        before = len(upstream.calls())
        t0 = time.monotonic()
        throttled_types = [coin(200 + i) for i in range(len(letters))]
        results = burst([page(c) for c in throttled_types])
        seconds = time.monotonic() - t0
        server.request("GET", "/probe/test-token-idle")
        rules.delay = 0.0
        second = tally(results, upstream.calls()[before:], "throttled")
        second["seconds"] = round(seconds, 2)
        if not second["failed_soft_pages"] or second["posts"] > 2:
            failures.append(f"throttled: the opened lane did not shed: {second}")
        failed_type = None
        for (status, _, data), ref in zip(results, throttled_types):
            body = routes.json_or_none(data) or {} if status == 200 else {}
            if (body.get("errors") or {}).get("chainMeta"):
                failed_type = ref
                break
        # After the breaker cools down (and the miss hold expires) the failed
        # type reads again: nothing negative was cached.
        recovered = None
        if failed_type:
            time.sleep(BREAKER_COOLDOWN_S)
            harness.set_clock(clock_file, now + 240_000)
            before = len(upstream.calls())
            status, _, data = get(page(failed_type))
            body = routes.json_or_none(data) or {}
            recovered = {"status": status, "symbol": (body.get("identity") or {}).get("symbol"),
                         "errors": body.get("errors"), "posts": posts_since(before)}
            if status != 200 or recovered["symbol"] != "SYN" or recovered["errors"] or \
                    recovered["posts"] != [("graphql.mainnet.sui.io", failed_type)]:
                failures.append(f"throttled: failure was cached or did not recover: {recovered}")
        second["recovered_after_cooldown"] = recovered
        rss_after = tron.rss_kib(server.process.pid)
        report.update({
            "cached_burst_statuses": cached,
            "cached_burst_ms_max": round(max(ms for s, ms, _ in cached_results if s == 200), 1),
            "cold_coin_types": first,
            "throttled_lane": second,
            "rss_kib": {"boot": rss_boot, "after_warm": rss_warm, "before_cold_bursts": rss_before_cold,
                        "after_bursts": rss_after},
        })

        # SIGKILL, then the same page from the durable KV.
        server.process.kill()
        server.process.wait()
        harness.set_clock(clock_file, now + 300_000)
        server = routes.Server(binary, kv_dir, clock_file, upstream, now)
        before = len(upstream.calls())
        status, _, payload = get(url)
        body = routes.json_or_none(payload) or {}
        after = {"status": status, "provider_calls": len(upstream.calls()) - before,
                 "identity_hit": (body.get("cache") or {}).get("identity", {}).get("hit"),
                 "symbol": (body.get("identity") or {}).get("symbol")}
        if status != 200 or after["provider_calls"] or after["symbol"] != "SYN" or not after["identity_hit"]:
            failures.append(f"restart: {after}")
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
    with tempfile.TemporaryDirectory(prefix="fortis-token-sui-") as root:
        ran = 0 if os.environ.get("TOKEN_SUI_NATIVE_ONLY") else replay(binary, fixture, root, failures)
        report = {} if os.environ.get("TOKEN_SUI_REPLAY_ONLY") else native_checks(binary, root, failures)
    if failures:
        print(f"token sui replay: {len(failures)} failure(s) over {ran} cases")
        for failure in failures:
            print(" -", failure)
        raise SystemExit(1)
    print(f"token sui replay: {ran} cases match the TypeScript contract")
    if report:
        print("token sui native checks:", json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
