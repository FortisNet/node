#!/usr/bin/env python3
"""Replay the frozen TypeScript Hedera SaucerSwap contract
(tests/corpus/token_saucerswap.json, written by
the earlier TypeScript implementation's tests) against
build/fortis_node_token_route_test, then run native-only measurements.

The TypeScript apps are the production composition (SaucerSwapClient built,
keyless and keyed): market, profile, pools and the insight listing section
of Hedera HTS refs come from api.saucerswap.finance / test-api.saucerswap.
finance, chain identity and holders from the Mirror Node.

The fake upstream serves the provider replies recorded in each case (the
case's own calls first, then the first recorded reply to the same request),
so the native token owner receives the TypeScript bytes. Every request
records the x-api-key it carried, compared with the fixture's.

Groups: `main` and `restart` run in two processes over one KV directory (the
restart group reads what `main` persisted); `keyed` starts a third process
with SAUCERSWAP_API_KEY (padded, as the TypeScript app's) over a fresh KV
directory; `jobs` a fourth over a fresh KV directory for the pools-route
observations, the stats flush, demand projection, liquidity warmer and the
liquidity rollup, and the history built from them.

Native lanes refill by the monotonic clock where the TypeScript lanes used
the fixture clock, so the replay paces the CoinGecko (burst 4, 0.4/s),
hedera-mirror and saucerswap (burst 15, 1/s) lanes before each case, and
waits out the 30 s breaker a 429 opens.

Each request compares status, the fixture header set, the exact JSON body
bytes or protobuf wire bytes (and that no response carries the API key), and
the provider request multiset. Concurrent cases compare both answers (the
upstream then answers after 0.3 s so the second reader joins the first one's
in-flight requests). Job cases compare the job summary and its provider
requests.

Native-only checks (no TypeScript counterpart):
  - a cold and warm default page: latency, provider calls (two Mirror GETs
    and the three SaucerSwap GETs), RSS;
  - four simultaneous cold pages of one token behind a 0.3 s upstream share
    the five in-flight GETs;
  - one id on `hedera` and `hedera-testnet` at once reads both SaucerSwap
    hosts, never one shared answer;
  - 23 simultaneous cached reads against the 2-worker/16-slot token queue;
  - 23 distinct cold tokens at once behind a 0.3 s upstream: every answer is
    200 or 429, every served page sends each of its GETs at most once and a
    page whose read waits out a lane fails soft (`errors`, never invented
    sections);
  - 23 tokens whose first SaucerSwap token GET answers 429 (opening the
    saucerswap lane): pages fail soft;
  - the keyed process: every SaucerSwap request carries x-api-key, no
    Mirror or CoinGecko request does, and no response contains the key;
  - SIGKILL, then the cached pages from the durable KV with no provider call.
Usage: token_saucerswap_test.py <binary>
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
from urllib.parse import urlsplit

import discovery_route_harness as harness
import tls_ca
import token_ledger_test as ledger
import token_routes_test as routes
import token_tron_test as tron

FIXTURE = "token_saucerswap.json"
BREAKER_COOLDOWN_S = 31.0
MIRROR = "mainnet-public.mirrornode.hedera.com"
MIRROR_TESTNET = "testnet.mirrornode.hedera.com"
SAUCER = "api.saucerswap.finance"
SAUCER_TESTNET = "test-api.saucerswap.finance"
MIRROR_HOSTS = {MIRROR, MIRROR_TESTNET}
SAUCER_HOSTS = {SAUCER, SAUCER_TESTNET}
BALANCES_QUERY = "account.balance=gt:0&limit=100&order=asc"


class Upstream:
    """HTTPS fake upstream recording (method, host, target, body, x-api-key)."""

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
                api_key = self.headers.get("x-api-key") or ""
                # The spot owner names no host: CoinGecko.
                host = self.headers.get("x-fortis-test-host") or "api.coingecko.com"
                with upstream.lock:
                    upstream.observed.append((method, host, self.path, body, api_key))
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
        keys += [k + (call.get("apiKey", ""),) for k in routes.normalize(call["method"], url.netloc, target, None)]
    return sorted(keys)


def observed_calls(calls):
    keys = []
    for method, host, target, body, api_key in calls:
        keys += [k + (api_key,) for k in routes.normalize(method, host, target, body)]
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
    if observed != expected:
        failures.append(f"{name}: upstream\n  expected {expected}\n  actual   {observed}")


def run_job(server, upstream, case, failures):
    name = case["name"]
    before = len(upstream.calls())
    status, _, body = server.request("GET", f"/probe/test-token-job?job={case['job']}")
    harness.wait_for_calls(upstream, before + len(case["upstream"]), timeout=15.0)
    server.request("GET", "/probe/test-token-idle")
    time.sleep(.05)
    observed = observed_calls(upstream.calls()[before:])
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
        if summary.get("failed"):
            failures.append(f"{name}: stats flush failed ({summary})")
    elif summary.get("written") != result:
        failures.append(f"{name}: job written {summary.get('written')} != {result} ({summary})")
    expected = expected_calls(case)
    if observed != expected:
        failures.append(f"{name}: upstream\n  expected {expected}\n  actual   {observed}")


def replay(binary, fixture, root, failures):
    cases = fixture["cases"]
    rules = tron.Recorded(cases)
    upstream = Upstream(root, rules)
    shared_kv = Path(root) / "kv"
    shared_kv.mkdir()
    clock_file = Path(root) / "clock"
    tokens = harness.TokenRotation(per_token=40)
    groups = fixture["source"]["groups"]
    api_key = fixture["source"]["apiKey"]
    ran = 0
    try:
        for group in ("main", "restart", "keyed", "jobs"):
            selected = [c for c in cases if c["group"] == group]
            if not selected:
                continue
            spec = groups[group]
            kv_dir = shared_kv
            if spec["kv"] == "fresh":
                kv_dir = Path(root) / f"kv-{group}"
                kv_dir.mkdir()
            extra = {"FORTIS_TEST_PRICE_NOW_FILE": str(clock_file)}
            if spec.get("apiKey"):
                extra["SAUCERSWAP_API_KEY"] = f" {api_key} "
            harness.set_clock(clock_file, selected[0]["at"])
            server = routes.Server(binary, kv_dir, clock_file, upstream, selected[0]["at"], extra_env=extra)
            lanes = [routes.CoinGeckoLane(), ledger.Lane(MIRROR_HOSTS, 15, 1.0), ledger.Lane(SAUCER_HOSTS, 15, 1.0)]
            try:
                for case in selected:
                    if "job" in case:
                        harness.set_clock(clock_file, case["at"])
                        for lane in lanes:
                            lane.reserve(case)
                        rules.use(case)
                        run_job(server, upstream, case, failures)
                    else:
                        run_case(server, upstream, rules, clock_file, tokens, case, failures, lanes, [api_key])
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
    return f"0.0.{6_000_000 + index}"


class Synthetic:
    """Mirror answers every token with a row named after its id and a
    two-holder page; SaucerSwap lists every token (a row, three hourly
    points, one pool against WHBAR); CoinGecko lists nothing. With
    `reject_next_token`, the first SaucerSwap token GET answers 429."""

    def __init__(self):
        self.delay = 0.0
        self.reject_next_token = False
        self.lock = threading.Lock()

    def __call__(self, method, host, target, body):
        if self.delay:
            time.sleep(self.delay)
        split = urlsplit(target)
        if method == "GET" and host in MIRROR_HOSTS and split.path.startswith("/api/v1/tokens/"):
            rest = split.path[len("/api/v1/tokens/"):]
            if rest.endswith("/balances"):
                rows = [{"account": "0.0.100", "balance": 5}, {"account": "0.0.101", "balance": 6}]
                return 200, json.dumps({"balances": rows, "links": {"next": None}}).encode()
            return 200, json.dumps({"token_id": rest, "name": f"Synthetic {rest}", "symbol": "SYN",
                                    "decimals": "2", "total_supply": "100000", "max_supply": "0",
                                    "custom_fees": {"fixed_fees": []}, "deleted": False}).encode()
        if method == "GET" and host in SAUCER_HOSTS:
            network = "testnet" if host == SAUCER_TESTNET else "mainnet"
            if split.path.startswith("/tokens/prices/"):
                to = int(dict(p.split("=") for p in split.query.split("&"))["to"])
                return 200, json.dumps([{"startTimestampSeconds": to - (3 - i) * 3600, "openUsd": 1 + i,
                                         "highUsd": 2 + i, "lowUsd": 0.5, "closeUsd": 1.5 + i,
                                         "volumeUsd": 10} for i in range(3)]).encode()
            if split.path.startswith("/tokens/associated-pools/"):
                entity = split.path[len("/tokens/associated-pools/"):]
                return 200, json.dumps([{"id": 1, "contractId": "0.0.99", "tokenReserveA": "100000000",
                                         "tokenReserveB": "500000000",
                                         "tokenA": {"id": entity, "symbol": "SYN", "decimals": 6, "priceUsd": 2},
                                         "tokenB": {"id": "0.0.1456986", "symbol": "WHBAR", "decimals": 8,
                                                    "priceUsd": 0.1}}]).encode()
            entity = split.path[len("/tokens/"):]
            with self.lock:
                reject = self.reject_next_token
                self.reject_next_token = False
            if reject:
                return 429, b'{"message":"Too Many Requests"}'
            return 200, json.dumps({"id": entity, "symbol": "SYN", "name": f"Saucer {entity} {network}",
                                    "decimals": 6, "priceUsd": 2, "icon": "/images/syn.svg",
                                    "dueDiligenceComplete": True, "description": "synthetic"}).encode()
        if host == "api.coingecko.com" and split.path.startswith("/api/v3/coins/markets"):
            return 200, b"[]"
        return 404, b'{"error":"coin not found"}'


def provider_calls(calls, hosts):
    return sorted((host, target.split("?")[0]) for _, host, target, _, _ in calls if host in hosts)


def native_checks(binary, root, failures):
    rules = Synthetic()
    upstream = Upstream(root, rules)
    clock_file = Path(root) / "native-clock"
    kv_dir = Path(root) / "kv-native"
    kv_dir.mkdir()
    now = 1768176000000  # 2026-01-12T00:00:00Z
    tokens = harness.TokenRotation(per_token=40)
    report = {}
    page_url = f"/v1/networks/hedera/token/{token(1)}/page"
    sections = "?include=identity,market,profile,liquidity,insight"
    hosts = MIRROR_HOSTS | SAUCER_HOSTS

    def get(path, token_value=None):
        return server.request("GET", path, headers={"Authorization": "Bearer " + (token_value or tokens.next()),
                                                    "Accept": "application/json"})

    def five(entity, saucer=SAUCER, mirror=MIRROR):
        return sorted([(mirror, f"/api/v1/tokens/{entity}"), (mirror, f"/api/v1/tokens/{entity}/balances"),
                       (saucer, f"/tokens/{entity}"), (saucer, f"/tokens/prices/{entity}"),
                       (saucer, f"/tokens/associated-pools/{entity}")])

    harness.set_clock(clock_file, now)
    server = routes.Server(binary, kv_dir, clock_file, upstream, now)
    try:
        rss_boot = tron.rss_kib(server.process.pid)
        before = len(upstream.calls())
        t0 = time.monotonic()
        status, _, payload = get(page_url + "?include=identity,market,profile,liquidity,holders,insight")
        cold_ms = (time.monotonic() - t0) * 1000
        observed = provider_calls(upstream.calls()[before:], hosts)
        body = routes.json_or_none(payload) or {}
        if status != 200 or body.get("errors") or (body.get("market") or {}).get("priceUsd") != 2 or \
                (body.get("liquidity") or {}).get("poolCount") != 1 or len(body.get("insight") or []) != 5 or \
                ((body.get("profile") or {}).get("logoMediaKey") is None):
            failures.append(f"measure: cold page {status} {payload[:600]!r}")
        if observed != five(token(1)):
            failures.append(f"measure: cold provider calls {observed}")
        warm = []
        warm_before = len(upstream.calls())
        for _ in range(20):
            t0 = time.monotonic()
            status, _, _ = get(page_url + "?include=identity,market,profile,liquidity,holders,insight")
            warm.append((time.monotonic() - t0) * 1000)
            if status != 200:
                failures.append(f"measure: warm status {status}")
                break
        warm_calls = len(upstream.calls()) - warm_before
        if warm_calls:
            failures.append(f"measure: {warm_calls} provider calls on warm reads")
        report["cold_and_warm"] = {"cold_ms": round(cold_ms, 1), "provider_calls": len(observed),
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
            except (OSError, IndexError, ValueError):
                # A connection the server closed without an answer.
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

        full = "?include=identity,market,profile,liquidity,holders,insight"
        # Four cold pages of one token at once: five shared GETs.
        time.sleep(6.0)
        rules.delay = 0.3
        before = len(upstream.calls())
        shared = burst([f"/v1/networks/hedera/token/{token(2)}/page{full}"] * 4)
        rules.delay = 0.0
        shared_calls = provider_calls(upstream.calls()[before:], hosts)
        report["four_readers_one_token"] = {"statuses": [s[0] for s in shared], "calls": len(shared_calls)}
        if [s[0] for s in shared] != [200] * 4 or shared_calls != five(token(2)):
            failures.append(f"shared in-flight GETs: {report['four_readers_one_token']} {shared_calls}")

        # One id on both networks: each network's own SaucerSwap host.
        time.sleep(6.0)
        rules.delay = 0.3
        before = len(upstream.calls())
        pair = burst([f"/v1/networks/{network}/token/{token(3)}/page{sections}"
                      for network in ("hedera", "hedera-testnet")])
        rules.delay = 0.0
        pair_calls = provider_calls(upstream.calls()[before:], SAUCER_HOSTS)
        names = [((routes.json_or_none(d) or {}).get("market") or {}).get("name") for _, _, d in pair]
        report["same_id_two_networks"] = {"statuses": [p[0] for p in pair], "saucer_calls": pair_calls,
                                          "market_names": names}
        if [p[0] for p in pair] != [200, 200] or names != [f"Saucer {token(3)} mainnet",
                                                           f"Saucer {token(3)} testnet"] or \
                sorted({h for h, _ in pair_calls}) != sorted(SAUCER_HOSTS) or len(pair_calls) != 6:
            failures.append(f"SaucerSwap host isolation: {report['same_id_two_networks']}")

        # 23 simultaneous cached reads, one seeded device each.
        cached_results = burst([page_url + full] * len(letters))
        cached = {}
        for status, _, _ in cached_results:
            cached[status] = cached.get(status, 0) + 1
        if cached.get(200, 0) < 16 or set(cached) - {200, 429}:
            failures.append(f"measure: cached burst answered {cached}")

        def tally(results, calls, label):
            statuses, served, failed_soft = {}, 0, 0
            allowed = {"chainMeta", "market", "profile", "liquidity", "holders", "insight"}
            for status, _, data in results:
                statuses[status] = statuses.get(status, 0) + 1
                if status != 200:
                    continue
                page = routes.json_or_none(data) or {}
                errors = page.get("errors") or {}
                complete = page.get("market") is not None and page.get("liquidity") is not None and \
                    page.get("profile") is not None and len(page.get("insight") or []) == 5
                if complete and not errors:
                    served += 1
                elif errors and set(errors) <= allowed and \
                        all(page.get(section) in (None, []) for section in errors if section in page):
                    failed_soft += 1
                elif not errors and len(page.get("insight") or []) == 4:
                    # The optional SaucerSwap listing read was refused: Mirror's
                    # sections stay and the page has no error (TypeScript logs it).
                    failed_soft += 1
                else:
                    failures.append(f"{label}: unexpected page {data[:300]!r}")
            reads = {}
            for _, host, target, _, _ in calls:
                if host in hosts:
                    reads[target] = reads.get(target, 0) + 1
            if set(statuses) - {200, 429}:
                failures.append(f"{label}: answered {statuses}")
            return {"statuses": statuses, "complete_pages": served, "failed_soft_pages": failed_soft,
                    "provider_requests": sum(reads.values()), "distinct_requests": len(reads)}

        # 23 distinct cold tokens at once behind a slow upstream.
        harness.set_clock(clock_file, now + 120_000)
        time.sleep(16.0)  # refill the hedera-mirror and saucerswap bursts
        rules.delay = 0.3
        before = len(upstream.calls())
        rss_before_cold = tron.rss_kib(server.process.pid)
        t0 = time.monotonic()
        results = burst([f"/v1/networks/hedera/token/{token(i + 10)}/page{full}" for i in range(len(letters))])
        seconds = time.monotonic() - t0
        server.request("GET", "/probe/test-token-idle")
        rules.delay = 0.0
        if server.process.poll() is not None:
            failures.append(f"saucer pressure: the server exited ({server.process.returncode})")
        first = tally(results, upstream.calls()[before:], "saucer pressure")
        first["seconds"] = round(seconds, 2)
        if first["distinct_requests"] != first["provider_requests"] or \
                first["provider_requests"] > 5 * (first["complete_pages"] + first["failed_soft_pages"]):
            failures.append(f"saucer pressure: requests per served page: {first}")

        # 23 tokens whose first SaucerSwap token GET answers 429: the lane
        # opens and the other pages' SaucerSwap reads are refused.
        time.sleep(16.0)
        rules.delay = 0.3
        rules.reject_next_token = True
        before = len(upstream.calls())
        t0 = time.monotonic()
        results = burst([f"/v1/networks/hedera/token/{token(i + 100)}/page{full}" for i in range(len(letters))])
        seconds = time.monotonic() - t0
        server.request("GET", "/probe/test-token-idle")
        rules.delay = 0.0
        second = tally(results, upstream.calls()[before:], "saucer throttled")
        second["seconds"] = round(seconds, 2)
        if not second["failed_soft_pages"]:
            failures.append(f"saucer throttled: no page failed soft after the lane opened: {second}")
        rss_after = tron.rss_kib(server.process.pid)
        report.update({
            "cached_burst_statuses": cached,
            "cached_burst_ms_max": round(max(ms for s, ms, _ in cached_results if s == 200), 1),
            "cold_tokens": first,
            "throttled_saucerswap": second,
            "rss_kib": {"boot": rss_boot, "after_warm": rss_warm, "before_cold_bursts": rss_before_cold,
                        "after_bursts": rss_after},
        })

        # SIGKILL, then the same pages from the durable KV.
        server.process.kill()
        server.process.wait()
        harness.set_clock(clock_file, now + 150_000)
        server = routes.Server(binary, kv_dir, clock_file, upstream, now)
        after = {}
        for label, url in (("first", page_url + full), ("shared", f"/v1/networks/hedera/token/{token(2)}/page{full}")):
            before = len(upstream.calls())
            status, _, payload = get(url)
            body = routes.json_or_none(payload) or {}
            cache = body.get("cache") or {}
            after[label] = {"status": status, "provider_calls": len(upstream.calls()) - before,
                            "hits": {k: (cache.get(k) or {}).get("hit")
                                     for k in ("identity", "market", "profile", "liquidity", "insight")},
                            "market": (body.get("market") or {}).get("priceUsd"),
                            "pools": (body.get("liquidity") or {}).get("poolCount")}
            if status != 200 or after[label]["provider_calls"] or not all(after[label]["hits"].values()) or \
                    after[label]["market"] != 2 or after[label]["pools"] != 1:
                failures.append(f"restart {label}: {after[label]}")
        report["after_sigkill"] = after
    finally:
        server.stop()

    # The keyed process: x-api-key on SaucerSwap requests only, never echoed.
    key = "native-check-saucerswap-key-42"
    kv_keyed = Path(root) / "kv-native-keyed"
    kv_keyed.mkdir()
    server = routes.Server(binary, kv_keyed, clock_file, upstream, now, extra_env={"SAUCERSWAP_API_KEY": f"  {key}\t"})
    try:
        before = len(upstream.calls())
        status, _, payload = get(f"/v1/networks/hedera/token/{token(500)}/page{full}")
        calls = upstream.calls()[before:]
        keyed = sorted({(host, api_key) for _, host, _, _, api_key in calls})
        report["keyed"] = {"status": status, "hosts_and_keys": keyed}
        expected = sorted({(SAUCER, key), (MIRROR, "")})
        if status != 200 or keyed != expected or key.encode() in payload:
            failures.append(f"keyed process: {report['keyed']}")
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
    with tempfile.TemporaryDirectory(prefix="fortis-token-saucerswap-") as root:
        ran = 0 if os.environ.get("TOKEN_SAUCERSWAP_NATIVE_ONLY") else replay(binary, fixture, root, failures)
        report = {} if os.environ.get("TOKEN_SAUCERSWAP_REPLAY_ONLY") else native_checks(binary, root, failures)
    if failures:
        print(f"token saucerswap replay: {len(failures)} failure(s) over {ran} cases")
        for failure in failures:
            print(" -", failure)
        raise SystemExit(1)
    print(f"token saucerswap replay: {ran} cases match the TypeScript contract")
    if report:
        print("token saucerswap native checks:", json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
