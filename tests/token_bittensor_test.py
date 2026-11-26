#!/usr/bin/env python3
"""Replay the frozen TypeScript Bittensor subnet token contract
(tests/corpus/token_bittensor.json, written by
the earlier TypeScript implementation's tests) against
build/fortis_node_token_route_test, then run native-only measurements.

The fake upstream serves the provider replies recorded in each case (the
case's own calls first, then the first recorded reply to the same request),
so the native token owner receives the TypeScript bytes. Taostats GETs name
their endpoint host in x-fortis-test-host; every request's Authorization
header is recorded and compared with the one TypeScript sent (the configured
TAOSTATS_API_KEY on Taostats requests, none elsewhere).

Groups: `keyed` and `restart` run in two processes over one KV directory with
TAOSTATS_API_KEY (the restart group reads what `keyed` persisted, with an
empty driver working set); `failover` starts a third keyed process over a
fresh KV directory with FORTIS_TEST_TOKEN_NETWORK_ENDPOINTS listing a failing
fixture endpoint first, as the TypeScript fixture builds a second app whose
Bittensor seed lists it first; `unkeyed` starts a fourth process without the
key over a fresh KV directory (no Bittensor driver).

Native lanes refill by the monotonic clock where the TypeScript lanes used
the fixture clock, so the replay paces the CoinGecko (burst 4, 0.4/s) and
taostats (burst 30, 0.5/s, both Taostats hosts) lanes before each case, and
waits out the 30 s breaker a 429 opens.

Each request compares status, the fixture header set, the exact JSON body
bytes or protobuf wire bytes, and the provider request multiset with each
request's Authorization header. No response may contain the key. Concurrent
cases compare both answers (the upstream then answers after 0.3 s so the
second reader joins the first one's in-flight GETs).

Native-only checks (no TypeScript counterpart):
  - a cold and warm keyed subnet page: latency, provider calls, RSS;
  - four simultaneous cold readers of one subnet send one pool GET and one
    TAO/USD GET; two subnets at once send two pool GETs and one TAO/USD GET;
  - the driver's 15 s working set across sub-facets, and its expiry;
  - 23 simultaneous cached reads against the 2-worker/16-slot token queue;
  - 23 distinct cold subnets at once behind a 0.3 s upstream: every answer is
    200 or 429 and every served page reads its pool exactly once;
  - 23 subnets whose first pool GET answers 429 (opening the taostats lane):
    every served page fails soft (errors on identity, market and insight,
    never an invented value), and after the breaker cools down a failed
    subnet reads again and answers (the failure was never cached);
  - the key appears in no response, in no KV file and in no log line;
  - an unkeyed process sends no Taostats request and answers honest nulls;
  - SIGKILL, then the cached page from the durable KV with no provider call.
Usage: token_bittensor_test.py <binary>
"""
import json
import os
import ssl
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, quote, urlsplit

import discovery_route_harness as harness
import tls_ca
import token_ledger_test as ledger
import token_routes_test as routes
import token_tron_test as tron

FIXTURE = "token_bittensor.json"
BREAKER_COOLDOWN_S = 31.0
TAOSTATS_HOSTS = {"api.taostats.io", "taostats-a.fixture.test"}
POOL_PATH = "/api/dtao/pool/latest/v1"
PRICE_PATH = "/api/price/latest/v1"


class Upstream:
    """HTTPS fake upstream recording (method, host, target, body,
    authorization)."""

    def __init__(self, root, rules):
        self.rules = rules
        self.observed = []
        self.lock = threading.Lock()
        ca, cert, key, _ = tls_ca.create_certificates(Path(root), "DNS:localhost")
        self.ca = ca
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def handle_any(self, method):
                length = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(length) if length else b""
                authorization = self.headers.get("Authorization") or ""
                # The spot owner names no host: CoinGecko.
                host = self.headers.get("x-fortis-test-host") or "api.coingecko.com"
                with upstream.lock:
                    upstream.observed.append((method, host, self.path, body, authorization))
                status, payload = upstream.rules(method, host, self.path, body)
                try:
                    self.send_response(status)
                    if status < 300:
                        self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(payload)))
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.wfile.write(payload)
                    self.wfile.flush()
                except OSError:
                    pass
                self.close_connection = True

            def do_GET(self):
                self.handle_any("GET")

            def do_POST(self):
                self.handle_any("POST")

            def log_message(self, *_args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", harness.free_port()), Handler)
        self.server.daemon_threads = True
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        self.server.socket = tls.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_address[1]

    def calls(self):
        with self.lock:
            return list(self.observed)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


def expected_calls(case):
    keys = []
    for call in case["upstream"]:
        url = urlsplit(call["url"])
        target = url.path + (f"?{url.query}" if url.query else "")
        keys += [k + (call.get("authorization", ""),)
                 for k in routes.normalize(call["method"], url.netloc, target, None)]
    return sorted(keys)


def observed_calls(calls):
    keys = []
    for method, host, target, body, authorization in calls:
        keys += [k + (authorization,) for k in routes.normalize(method, host, target, body)]
    return sorted(keys)


def run_case(server, upstream, rules, clock_file, tokens, case, failures, lanes, secrets):
    name = case["name"]
    harness.set_clock(clock_file, case["at"])
    for lane in lanes:
        lane.reserve(case)
    rules.use(case)
    before = len(upstream.calls())
    expected = expected_calls(case)

    def send(entry, out, index):
        headers = {"Authorization": "Bearer " + tokens.next(), "Accept": entry["accept"]}
        if entry.get("ifNoneMatch"):
            headers["If-None-Match"] = entry["ifNoneMatch"]
        out[index] = server.request("GET", entry["url"], headers=headers)

    if "concurrent" in case:
        entries = case["concurrent"]
        results = [None] * len(entries)
        # Keep each provider read in flight long enough for the second worker
        # to join it (the TypeScript mock answers after 25 ms; the driver's
        # single flights share one read).
        rules.delay = 0.3
        threads = [threading.Thread(target=send, args=(entry, results, i)) for i, entry in enumerate(entries)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        rules.delay = 0.0
    else:
        entries = [case]
        results = [None]
        send(case, results, 0)
    harness.wait_for_calls(upstream, before + len(case["upstream"]), timeout=10.0)
    server.request("GET", "/probe/test-token-idle")
    time.sleep(.05)
    observed = observed_calls(upstream.calls()[before:])
    for index, (entry, (status, response_headers, payload)) in enumerate(zip(entries, results)):
        label = f"{name}[{index}]" if len(entries) > 1 else name
        tron.compare_response(label, entry, status, response_headers, payload, failures, secrets)
        for header, value in response_headers.items():
            if any(secret in f"{header}: {value}" for secret in secrets):
                failures.append(f"{label}: the key reached a response header")
    if observed != expected:
        failures.append(f"{name}: upstream\n  expected {expected}\n  actual   {observed}")


def key_leaks(root, secret, server, failures, label):
    """The key may live only in provider requests: not in a KV file, not in
    the server's output."""
    needle = secret.encode()
    for path in Path(root).rglob("*"):
        if path.is_file() and path.suffix not in (".pem", ".key", ".crt") and needle in path.read_bytes():
            failures.append(f"{label}: the key is stored in {path.name}")
    output = b""
    for stream in (server.process.stdout, server.process.stderr):
        if stream is not None:
            output += stream.read() or b""
    if needle in output:
        failures.append(f"{label}: the key reached the server output")


def replay(binary, fixture, root, failures):
    cases = fixture["cases"]
    rules = tron.Recorded(cases)
    upstream = Upstream(root, rules)
    shared_kv = Path(root) / "kv"
    shared_kv.mkdir()
    clock_file = Path(root) / "clock"
    tokens = harness.TokenRotation(per_token=40)
    groups = fixture["source"]["groups"]
    secrets = [fixture["source"]["taostatsKey"]]
    ran = 0
    try:
        for group in ("keyed", "restart", "failover", "unkeyed"):
            selected = [c for c in cases if c["group"] == group]
            if not selected:
                continue
            spec = groups[group]
            kv_dir = shared_kv
            if spec["kv"] == "fresh":
                kv_dir = Path(root) / f"kv-{group}"
                kv_dir.mkdir()
            extra = {"FORTIS_TEST_PRICE_NOW_FILE": str(clock_file)}
            if spec.get("TAOSTATS_API_KEY"):
                extra["TAOSTATS_API_KEY"] = spec["TAOSTATS_API_KEY"]
            if spec.get("endpoints"):
                extra["FORTIS_TEST_TOKEN_NETWORK_ENDPOINTS"] = ";".join(
                    f"{primary}={','.join(urls)}" for primary, urls in spec["endpoints"].items())
            harness.set_clock(clock_file, selected[0]["at"])
            server = routes.Server(binary, kv_dir, clock_file, upstream, selected[0]["at"], extra_env=extra)
            lanes = [routes.CoinGeckoLane(), ledger.Lane(TAOSTATS_HOSTS, 30, 0.5)]
            try:
                for case in selected:
                    run_case(server, upstream, rules, clock_file, tokens, case, failures, lanes, secrets)
                    ran += 1
                    if any(call["status"] == 429 for call in case.get("upstream", [])):
                        # A 429 opens the lane breaker for 30 s on the
                        # monotonic clock (the TypeScript one cools down on
                        # the fixture clock the next case advances).
                        time.sleep(BREAKER_COOLDOWN_S)
            finally:
                server.stop()
            key_leaks(kv_dir, secrets[0], server, failures, f"group {group}")
    finally:
        upstream.close()
    return ran


# ---- native-only checks ------------------------------------------------------

KEY = "tao-native-check-key-5b2c"


class Synthetic:
    """Taostats answers every netuid with a pool row and TAO/USD with 400;
    CoinGecko lists nothing. `reject_next` makes the next pool GET answer
    429."""

    def __init__(self):
        self.delay = 0.0
        self.reject_next = False
        self.lock = threading.Lock()

    def __call__(self, method, host, target, body):
        if self.delay:
            time.sleep(self.delay)
        split = urlsplit(target)
        if host in TAOSTATS_HOSTS and split.path == POOL_PATH:
            with self.lock:
                reject = self.reject_next
                self.reject_next = False
            if reject:
                return 429, b'{"message":"rate limited"}'
            netuid = int(parse_qs(split.query)["netuid"][0])
            return 200, json.dumps({"data": [{
                "netuid": netuid, "name": f"subnet {netuid}", "symbol": "SYN", "price": "0.01",
                "market_cap": "1000000000000000", "liquidity": "5000000000000", "total_tao": "7000000000000",
                "total_alpha": "2000000000000000", "alpha_in_pool": "100000000000", "alpha_staked": "300000000000",
                "price_change_1_hour": "1", "price_change_1_day": "2", "price_change_1_week": "3"}]}).encode()
        if host in TAOSTATS_HOSTS and split.path == PRICE_PATH:
            return 200, b'{"data":[{"price":"400"}]}'
        if host == "api.coingecko.com" and split.path.startswith("/api/v3/coins/markets"):
            return 200, b"[]"
        return 404, b'{"error":"not found"}'


def native_checks(binary, root, failures):
    rules = Synthetic()
    upstream = Upstream(root, rules)
    clock_file = Path(root) / "native-clock"
    kv_dir = Path(root) / "kv-native"
    kv_dir.mkdir()
    now = 1768176000000  # 2026-01-12T00:00:00Z
    tokens = harness.TokenRotation(per_token=40)
    report = {}
    full = "identity,market,insight"

    def page(netuid, include=full):
        return f"/v1/networks/bittensor/token/{quote(str(netuid), safe='')}/page?include={include}"

    def get(path, token=None):
        return server.request("GET", path, headers={"Authorization": "Bearer " + (token or tokens.next()),
                                                    "Accept": "application/json"})

    def taostats_since(before):
        out = []
        for method, host, target, _, authorization in upstream.calls()[before:]:
            if host in TAOSTATS_HOSTS:
                split = urlsplit(target)
                query = parse_qs(split.query)
                out.append((split.path, int(query["netuid"][0]) if "netuid" in query else None,
                            authorization == KEY))
        return sorted(out, key=lambda c: (c[0], c[1] or 0))

    def leaks(payload):
        return any(text in payload for text in (KEY.encode(), b"api.taostats.io", b"x-fortis-test-host"))

    harness.set_clock(clock_file, now)
    extra = {"TAOSTATS_API_KEY": KEY, "FORTIS_TEST_PRICE_NOW_FILE": str(clock_file)}
    server = routes.Server(binary, kv_dir, clock_file, upstream, now, extra_env=extra)
    try:
        rss_boot = tron.rss_kib(server.process.pid)
        before = len(upstream.calls())
        t0 = time.monotonic()
        status, _, payload = get(page(1))
        cold_ms = (time.monotonic() - t0) * 1000
        calls = taostats_since(before)
        body = routes.json_or_none(payload) or {}
        identity = body.get("identity") or {}
        market = body.get("market") or {}
        if status != 200 or identity.get("symbol") != "SYN" or identity.get("decimals") != 9 or \
                market.get("priceUsd") != 4.0 or len(body.get("insight") or []) != 1 or body.get("errors"):
            failures.append(f"measure: cold page {status} {payload[:300]!r}")
        if calls != [(POOL_PATH, 1, True), (PRICE_PATH, None, True)] or \
                len(upstream.calls()) - before != 2:
            failures.append(f"measure: cold provider calls {calls}")
        if leaks(payload):
            failures.append("measure: key or endpoint in a response")
        warm = []
        warm_before = len(upstream.calls())
        for _ in range(20):
            t0 = time.monotonic()
            status, _, _ = get(page(1))
            warm.append((time.monotonic() - t0) * 1000)
            if status != 200:
                failures.append(f"measure: warm status {status}")
                break
        warm_calls = len(upstream.calls()) - warm_before
        if warm_calls:
            failures.append(f"measure: {warm_calls} provider calls on warm reads")
        report["cold_and_warm"] = {"cold_ms": round(cold_ms, 1), "taostats_calls": len(calls),
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

        clock = now

        def advance(seconds):
            nonlocal clock
            clock += int(seconds * 1000)
            harness.set_clock(clock_file, clock)

        # One subnet, four readers at once: one pool GET and one TAO/USD GET.
        advance(20)
        rules.delay = 0.3
        before = len(upstream.calls())
        shared = burst([page(2)] * 4)
        rules.delay = 0.0
        report["four_readers_one_subnet"] = {"statuses": [s[0] for s in shared], "taostats": taostats_since(before)}
        if [s[0] for s in shared] != [200] * 4 or \
                taostats_since(before) != [(POOL_PATH, 2, True), (PRICE_PATH, None, True)]:
            failures.append(f"in-flight sharing: {report['four_readers_one_subnet']}")

        # Two subnets at once: two pool GETs (the netuid is in the path), one
        # shared TAO/USD GET.
        advance(20)
        rules.delay = 0.3
        before = len(upstream.calls())
        pair = burst([page(3), page(4)])
        rules.delay = 0.0
        report["two_subnets"] = {"statuses": [s[0] for s in pair], "taostats": taostats_since(before)}
        if [s[0] for s in pair] != [200, 200] or taostats_since(before) != [
                (POOL_PATH, 3, True), (POOL_PATH, 4, True), (PRICE_PATH, None, True)]:
            failures.append(f"two subnets: {report['two_subnets']}")

        # The driver's 15 s working set across sub-facets, then its expiry.
        advance(20)
        before = len(upstream.calls())
        get(page(5, "identity"))
        after_identity = taostats_since(before)
        advance(5)
        before = len(upstream.calls())
        get(page(5, "market"))
        after_market = taostats_since(before)
        advance(5)
        before = len(upstream.calls())
        get(page(5, "insight"))
        after_insight = taostats_since(before)
        advance(1)
        get(page(6, "identity"))
        advance(20)
        before = len(upstream.calls())
        get(page(6, "insight"))
        after_expiry = taostats_since(before)
        report["working_set"] = {"identity": after_identity, "market_5s": after_market,
                                 "insight_10s": after_insight, "insight_after_20s": after_expiry}
        if after_identity != [(POOL_PATH, 5, True)] or after_market != [(PRICE_PATH, None, True)] or \
                after_insight or after_expiry != [(POOL_PATH, 6, True)]:
            failures.append(f"working set: {report['working_set']}")

        # 23 simultaneous cached reads, one seeded device each.
        cached_results = burst([page(1)] * len(letters))
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
                    failures.append(f"{label}: key or endpoint in a response")
                body = routes.json_or_none(data) or {}
                errors = body.get("errors") or {}
                identity = body.get("identity") or {}
                if identity.get("symbol") == "SYN" and not errors and len(body.get("insight") or []) == 1:
                    served += 1
                elif identity.get("symbol") is None and body.get("market") is None and \
                        body.get("insight") == [] and \
                        errors == {k: "UPSTREAM_UNAVAILABLE" for k in ("chainMeta", "insight", "market")}:
                    failed_soft += 1
                else:
                    failures.append(f"{label}: unexpected page {data[:300]!r}")
            pools = [c for c in calls if c[1] in TAOSTATS_HOSTS and urlsplit(c[2]).path == POOL_PATH]
            netuids = {parse_qs(urlsplit(c[2]).query)["netuid"][0] for c in pools}
            if any(c[4] != KEY for c in calls if c[1] in TAOSTATS_HOSTS):
                failures.append(f"{label}: a Taostats request without the key")
            if set(statuses) - {200, 429}:
                failures.append(f"{label}: answered {statuses}")
            return {"statuses": statuses, "served_pages": served, "failed_soft_pages": failed_soft,
                    "pool_gets": len(pools), "distinct_pool_gets": len(netuids),
                    "price_gets": sum(1 for c in calls if c[1] in TAOSTATS_HOSTS
                                      and urlsplit(c[2]).path == PRICE_PATH)}

        # 23 distinct cold subnets at once behind a slow upstream.
        advance(120)
        time.sleep(1.0)
        rules.delay = 0.3
        before = len(upstream.calls())
        rss_before_cold = tron.rss_kib(server.process.pid)
        t0 = time.monotonic()
        results = burst([page(100 + i) for i in range(len(letters))])
        seconds = time.monotonic() - t0
        server.request("GET", "/probe/test-token-idle")
        rules.delay = 0.0
        first = tally(results, upstream.calls()[before:], "cold pressure")
        first["seconds"] = round(seconds, 2)
        if first["pool_gets"] != first["served_pages"] + first["failed_soft_pages"] or \
                first["distinct_pool_gets"] != first["pool_gets"] or not first["served_pages"]:
            failures.append(f"cold pressure: pool GETs per served page: {first}")

        # 23 subnets whose first pool GET answers 429: the lane opens and the
        # rest fail soft (one endpoint, nothing to fail over to).
        advance(120)
        time.sleep(1.0)
        rules.delay = 0.3
        rules.reject_next = True
        before = len(upstream.calls())
        t0 = time.monotonic()
        throttled = [200 + i for i in range(len(letters))]
        results = burst([page(n) for n in throttled])
        seconds = time.monotonic() - t0
        server.request("GET", "/probe/test-token-idle")
        rules.delay = 0.0
        second = tally(results, upstream.calls()[before:], "throttled")
        second["seconds"] = round(seconds, 2)
        if not second["failed_soft_pages"] or second["pool_gets"] > 2:
            failures.append(f"throttled: the opened lane did not shed: {second}")
        failed_netuid = None
        for (status, _, data), netuid in zip(results, throttled):
            body = routes.json_or_none(data) or {} if status == 200 else {}
            if (body.get("errors") or {}).get("chainMeta"):
                failed_netuid = netuid
                break
        recovered = None
        if failed_netuid:
            time.sleep(BREAKER_COOLDOWN_S)
            advance(120)
            before = len(upstream.calls())
            status, _, data = get(page(failed_netuid))
            body = routes.json_or_none(data) or {}
            recovered = {"status": status, "symbol": (body.get("identity") or {}).get("symbol"),
                         "errors": body.get("errors"), "taostats": taostats_since(before)}
            if status != 200 or recovered["symbol"] != "SYN" or recovered["errors"] or \
                    recovered["taostats"] != [(POOL_PATH, failed_netuid, True), (PRICE_PATH, None, True)]:
                failures.append(f"throttled: failure was cached or did not recover: {recovered}")
        second["recovered_after_cooldown"] = recovered
        rss_after = tron.rss_kib(server.process.pid)
        report.update({
            "cached_burst_statuses": cached,
            "cached_burst_ms_max": round(max(ms for s, ms, _ in cached_results if s == 200), 1),
            "cold_subnets": first,
            "throttled_lane": second,
            "rss_kib": {"boot": rss_boot, "after_warm": rss_warm, "before_cold_bursts": rss_before_cold,
                        "after_bursts": rss_after},
        })

        # SIGKILL, then the same page from the durable KV.
        server.process.kill()
        server.process.wait()
        server = routes.Server(binary, kv_dir, clock_file, upstream, now, extra_env=extra)
        before = len(upstream.calls())
        status, _, payload = get(page(1))
        body = routes.json_or_none(payload) or {}
        after = {"status": status, "provider_calls": len(upstream.calls()) - before,
                 "identity_hit": (body.get("cache") or {}).get("identity", {}).get("hit"),
                 "insight_hit": (body.get("cache") or {}).get("insight", {}).get("hit"),
                 "symbol": (body.get("identity") or {}).get("symbol")}
        if status != 200 or after["symbol"] != "SYN" or not after["identity_hit"] or not after["insight_hit"]:
            failures.append(f"restart: {after}")
        report["after_sigkill"] = after
    finally:
        server.stop()
    key_leaks(kv_dir, KEY, server, failures, "native checks")

    # Unkeyed: no driver, no Taostats request, honest nulls.
    kv_unkeyed = Path(root) / "kv-native-unkeyed"
    kv_unkeyed.mkdir()
    server = routes.Server(binary, kv_unkeyed, clock_file, upstream, now,
                           extra_env={"FORTIS_TEST_PRICE_NOW_FILE": str(clock_file)})
    try:
        before = len(upstream.calls())
        status, _, payload = get(page(7))
        body = routes.json_or_none(payload) or {}
        unkeyed = {"status": status, "taostats": taostats_since(before),
                   "identity_symbol": (body.get("identity") or {}).get("symbol"),
                   "market": body.get("market"), "insight": body.get("insight"), "errors": body.get("errors")}
        if status != 200 or unkeyed["taostats"] or unkeyed["identity_symbol"] is not None or \
                unkeyed["market"] is not None or unkeyed["insight"] != [] or unkeyed["errors"]:
            failures.append(f"unkeyed: {unkeyed}")
        report["unkeyed"] = unkeyed
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
    with tempfile.TemporaryDirectory(prefix="fortis-token-bittensor-") as root:
        ran = 0 if os.environ.get("TOKEN_BITTENSOR_NATIVE_ONLY") else replay(binary, fixture, root, failures)
        report = {} if os.environ.get("TOKEN_BITTENSOR_REPLAY_ONLY") else native_checks(binary, root, failures)
    if failures:
        print(f"token bittensor replay: {len(failures)} failure(s) over {ran} cases")
        for failure in failures:
            print(" -", failure)
        raise SystemExit(1)
    print(f"token bittensor replay: {ran} cases match the TypeScript contract")
    if report:
        print("token bittensor native checks:", json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
