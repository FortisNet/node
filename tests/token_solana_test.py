#!/usr/bin/env python3
"""Replay the frozen TypeScript Solana SPL token contract
(tests/corpus/token_solana.json, written by
the earlier TypeScript implementation's tests) against
build/fortis_node_token_route_test, then run native-only measurements.

The fake upstream serves the provider replies recorded in each case (the
case's own calls first, then the first recorded reply to the same request),
so the native token owner and spot owner receive the TypeScript bytes. Token
owner requests name their real host in x-fortis-test-host; the spot owner's
requests carry none, so a `/price/v3` path is attributed to the Jupiter host
its x-api-key selects (api.jup.ag keyed, lite-api.jup.ag keyless).

Groups: `lite` and `restart` run in two processes over one KV directory (the
restart group reads what `lite` persisted); `keyed` starts a third process
with JUPITER_API_KEY over a fresh KV directory, as the TypeScript fixture
uses a second app with that key.

A 429 opens the shared lane's breaker for 30 s: the native breaker cools
down on the monotonic clock while the TypeScript one used the fixture clock,
so the replay waits out the cooldown after such a case (as the CoinGecko
lane is paced).

Each request compares status, the fixture header set, the exact JSON body
bytes or protobuf wire bytes, and the provider request multiset including
the x-api-key each request carried. Concurrent cases compare both answers.

Native-only checks (no TypeScript counterpart):
  - a cold and warm SPL page: latency, provider calls, RSS;
  - 23 simultaneous cached reads against the 2-worker/16-slot token queue;
  - 24 distinct cold mints at once behind a 0.3 s upstream: the shared
    Jupiter lane (burst 15, 8 concurrent) admits what it can, every answer
    is 200 or 429, a shed chain-metadata read is reported as
    errors.chainMeta and never as a fabricated empty row;
  - SIGKILL, then the cached page from the durable KV with no provider call.
Usage: token_solana_test.py <binary>
"""
import json
import os
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, quote, urlsplit

import discovery_route_harness as harness
import tls_ca
import token_routes_test as routes

FIXTURE = "token_solana.json"
JUPITER_HOSTS = ("api.jup.ag", "lite-api.jup.ag")
BREAKER_COOLDOWN_S = 31.0
BIG_SEARCH_BYTES = 480 * 1024
BASE58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def request_key(method, host, target):
    return (method, host, target)


def recorded_key(call):
    url = urlsplit(call["url"])
    return request_key(call["method"], url.netloc, url.path + (f"?{url.query}" if url.query else ""))


class Recorded:
    """The recorded TypeScript replies: the running case's own calls in
    order first, then the first recorded reply to the same request."""

    def __init__(self, cases):
        self.fallback = {}
        for case in cases:
            for call in case.get("upstream", []):
                self.fallback.setdefault(recorded_key(call), (call["status"], call["body"]))
        self.current = {}
        self.delay = 0.0

    def use(self, case):
        self.current = {}
        for call in case.get("upstream", []):
            self.current.setdefault(recorded_key(call), []).append((call["status"], call["body"]))

    def __call__(self, method, host, target, body):
        if self.delay:
            time.sleep(self.delay)
        key = request_key(method, host, target)
        queue = self.current.get(key)
        if queue:
            status, payload = queue.pop(0) if len(queue) > 1 else queue[0]
        elif key in self.fallback:
            status, payload = self.fallback[key]
        else:
            status, payload = 404, '{"error":"unexpected request"}'
        return status, payload.encode()


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
                host = self.headers.get("x-fortis-test-host")
                if not host:
                    # The spot owner names no host: a Jupiter price path is
                    # the host its key selects, anything else CoinGecko.
                    if self.path.startswith("/price/v3"):
                        host = "api.jup.ag" if api_key else "lite-api.jup.ag"
                    else:
                        host = "api.coingecko.com"
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


def compare_response(name, expected, status, response_headers, payload, failures):
    def fail(message):
        failures.append(f"{name}: {message}")

    if status != expected["status"]:
        fail(f"status {status} != {expected['status']} ({payload[:400]!r})")
        return
    for header in routes.HEADERS:
        if response_headers.get(header) != expected["headers"].get(header):
            fail(f"header {header}: {response_headers.get(header)!r} != {expected['headers'].get(header)!r}")
    if "wireHex" in expected:
        if payload.hex() != expected["wireHex"]:
            fail(f"protobuf wire\n  expected {expected['wireHex'][:600]}\n  actual   {payload.hex()[:600]}")
    elif "bodyText" in expected:
        if payload != expected["bodyText"].encode("utf-8", "surrogatepass"):
            text, actual = expected["bodyText"], payload.decode("utf-8", "replace")
            index = next((i for i, (x, y) in enumerate(zip(text, actual)) if x != y), min(len(text), len(actual)))
            fail(f"body differs at {index}\n  expected …{text[max(0, index - 200):index + 200]}\n"
                 f"  actual   …{actual[max(0, index - 200):index + 200]}")
    elif expected["status"] != 304:
        if routes.json_or_none(payload) != expected["body"]:
            fail(f"body {payload[:600]!r} != {json.dumps(expected['body'])[:600]}")


def run_case(server, upstream, rules, clock_file, tokens, case, failures, lane):
    name = case["name"]
    harness.set_clock(clock_file, case["at"])
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
        # Keep each provider read in flight long enough for the second
        # worker's identical request to join it (the TypeScript mock answers
        # after 25 ms; the TypeScript SWR single flight shares one read).
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
        compare_response(label, entry, status, response_headers, payload, failures)
    if observed != expected:
        failures.append(f"{name}: upstream\n  expected {expected}\n  actual   {observed}")


def replay(binary, fixture, root, failures):
    cases = fixture["cases"]
    rules = Recorded(cases)
    upstream = Upstream(root, rules)
    shared_kv = Path(root) / "kv"
    shared_kv.mkdir()
    clock_file = Path(root) / "clock"
    tokens = harness.TokenRotation(per_token=40)
    groups = fixture["source"]["groups"]
    ran = 0
    try:
        for group in ("lite", "restart", "keyed"):
            selected = [c for c in cases if c["group"] == group]
            if not selected:
                continue
            spec = groups[group]
            kv_dir = shared_kv
            if spec["kv"] == "fresh":
                kv_dir = Path(root) / f"kv-{group}"
                kv_dir.mkdir()
            extra = {k: v for k, v in spec.items() if k != "kv"}
            # The spot owner's quote asOf follows the case clock.
            extra["FORTIS_TEST_PRICE_NOW_FILE"] = str(clock_file)
            harness.set_clock(clock_file, selected[0]["at"])
            server = routes.Server(binary, kv_dir, clock_file, upstream, selected[0]["at"], extra_env=extra)
            lane = routes.CoinGeckoLane()
            try:
                for case in selected:
                    run_case(server, upstream, rules, clock_file, tokens, case, failures, lane)
                    ran += 1
                    if any(call["status"] == 429 for call in case.get("upstream", [])):
                        # A 429 opens the lane breaker for 30 s; the native
                        # breaker cools down on the monotonic clock, the
                        # TypeScript one on the fixture clock the next case
                        # advances.
                        time.sleep(BREAKER_COOLDOWN_S)
            finally:
                server.stop()
    finally:
        upstream.close()
    return ran


# ---- native-only checks ------------------------------------------------------


def mint(index):
    digits = []
    n = index
    for _ in range(6):
        digits.append(BASE58[n % 58])
        n //= 58
    return ("Mint" + "".join(digits)).ljust(44, "z")


class Synthetic:
    """Every mint is listed on Jupiter with a holder count; nothing else."""

    def __init__(self):
        self.delay = 0.0

    def __call__(self, method, host, target, body):
        if self.delay:
            time.sleep(self.delay)
        split = urlsplit(target)
        query = parse_qs(split.query)
        if host in JUPITER_HOSTS and split.path == "/tokens/v2/search":
            q = (query.get("query") or [""])[0]
            row = {"id": q, "symbol": "SYN", "name": "Synthetic " + q[:8], "decimals": 6,
                   "holderCount": 1000 + len(q), "isVerified": True, "icon": f"https://img.example/{q}.png"}
            if q.startswith("Big"):
                # A search reply just under the 512 KiB response bound, the
                # exact mint last: every row is parsed before the match.
                filler = {"id": "", "symbol": "FILL", "name": "Filler row", "decimals": 9, "holderCount": 7,
                          "isVerified": False, "tags": ["community"], "icon": "https://img.example/f.png",
                          "organicScore": 12.5, "usdPrice": 0.000123, "mcap": 1234.5}
                rows = []
                size = 0
                while size < BIG_SEARCH_BYTES:
                    item = dict(filler, id=f"Fi11{len(rows):040d}".replace("0", "z")[:44])
                    rows.append(item)
                    size += len(json.dumps(item)) + 1
                rows.append(row)
                return 200, json.dumps(rows).encode()
            return 200, json.dumps([row]).encode()
        if host in JUPITER_HOSTS and split.path == "/price/v3":
            return 200, b"{}"
        if host == "api.gopluslabs.io":
            return 200, b'{"code":1,"message":"OK","result":{}}'
        if host == "api.dexscreener.com":
            return 200, b'{"schemaVersion":"1.0.0","pairs":[]}'
        if host == "api.coingecko.com" and split.path.startswith("/api/v3/coins/markets"):
            return 200, b"[]"
        return 404, b'{"error":"coin not found"}'


def rss_kib(pid):
    out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    return int(out) if out else -1


def native_checks(binary, root, failures):
    rules = Synthetic()
    upstream = Upstream(root, rules)
    clock_file = Path(root) / "native-clock"
    kv_dir = Path(root) / "kv-native"
    kv_dir.mkdir()
    now = 1768176000000  # 2026-01-12T00:00:00Z
    tokens = harness.TokenRotation(per_token=40)
    report = {}
    url = f"/v1/networks/solana/token/{quote(mint(0), safe='')}/page?include=identity,holders,insight"

    def get(path, token=None):
        return server.request("GET", path, headers={"Authorization": "Bearer " + (token or tokens.next()),
                                                    "Accept": "application/json"})

    harness.set_clock(clock_file, now)
    server = routes.Server(binary, kv_dir, clock_file, upstream, now)
    try:
        rss_boot = rss_kib(server.process.pid)
        before = len(upstream.calls())
        t0 = time.monotonic()
        status, _, payload = get(url)
        cold_ms = (time.monotonic() - t0) * 1000
        cold_calls = sorted({(c[1], urlsplit(c[2]).path) for c in upstream.calls()[before:]})
        body = routes.json_or_none(payload) or {}
        if status != 200 or (body.get("holders") or {}).get("source") != "jupiter" or not body.get("insight"):
            failures.append(f"measure: cold page {status} {payload[:300]!r}")
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
        rss_warm = rss_kib(server.process.pid)

        # 23 simultaneous cached reads, one seeded device each.
        letters = "CDEFGIJKLMNOPQRSTUVWXYZ"
        burst = [None] * len(letters)

        def one(index, path, out):
            t = time.monotonic()
            try:
                status, _, data = server.request("GET", path, headers={
                    "Authorization": "Bearer fintel_dev_" + letters[index] * 43, "Accept": "application/json"})
            except OSError:
                status, data = "reset", b""
            out[index] = (status, (time.monotonic() - t) * 1000, data)

        threads = [threading.Thread(target=one, args=(i, url, burst)) for i in range(len(letters))]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        cached = {}
        for status, _, _ in burst:
            cached[status] = cached.get(status, 0) + 1
        if cached.get(200, 0) < 16 or set(cached) - {200, 429}:
            failures.append(f"measure: cached burst answered {cached}")

        # 23 distinct cold mints at once behind a slow upstream.
        harness.set_clock(clock_file, now + 120_000)
        time.sleep(1.0)
        rules.delay = 0.3
        cold_before = len(upstream.calls())
        rss_before_cold = rss_kib(server.process.pid)
        cold = [None] * len(letters)
        paths = [f"/v1/networks/solana/token/{quote(mint(i + 1), safe='')}/page?include=identity,insight"
                 for i in range(len(letters))]
        t0 = time.monotonic()
        threads = [threading.Thread(target=one, args=(i, paths[i], cold)) for i in range(len(letters))]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        burst_seconds = time.monotonic() - t0
        server.request("GET", "/probe/test-token-idle")
        rules.delay = 0.0
        statuses, shed, listed = {}, 0, 0
        for status, _, data in cold:
            statuses[status] = statuses.get(status, 0) + 1
            if status != 200:
                continue
            page = routes.json_or_none(data) or {}
            if (page.get("errors") or {}).get("chainMeta") == "UPSTREAM_UNAVAILABLE":
                shed += 1
                if page.get("insight") or (page.get("identity") or {}).get("symbol"):
                    failures.append("pressure: a failed chain-metadata read still produced token-list data")
            elif (page.get("identity") or {}).get("symbol") == "SYN":
                listed += 1
            else:
                failures.append(f"pressure: unexpected page {data[:300]!r}")
        jupiter_calls = sum(1 for c in upstream.calls()[cold_before:]
                            if c[1] in JUPITER_HOSTS and c[2].startswith("/tokens/v2/search"))
        if set(statuses) - {200, 429}:
            failures.append(f"pressure: answered {statuses}")
        if jupiter_calls > len(letters):
            failures.append(f"pressure: {jupiter_calls} Jupiter searches for {len(letters)} mints")
        rss_after = rss_kib(server.process.pid)
        report.update({
            "cold_ms": round(cold_ms, 1),
            "cold_provider_calls": cold_calls,
            "warm_ms_median": round(sorted(warm)[len(warm) // 2], 1),
            "warm_ms_max": round(max(warm), 1),
            "warm_provider_calls": warm_calls,
            "cached_burst_statuses": cached,
            "cached_burst_ms_max": round(max(ms for s, ms, _ in burst if s == 200), 1),
            "cold_mints": {"statuses": statuses, "listed": listed, "chain_meta_unavailable": shed,
                           "jupiter_searches": jupiter_calls, "seconds": round(burst_seconds, 2)},
            "rss_kib": {"boot": rss_boot, "after_warm": rss_warm, "before_cold_burst": rss_before_cold,
                        "after_cold_burst": rss_after},
        })
        if warm_calls:
            failures.append(f"measure: {warm_calls} provider calls on warm reads")

        # A search reply near the response bound composes inside the arena.
        harness.set_clock(clock_file, now + 150_000)
        big = ("Big" + "".join(BASE58[(7 * i) % 58] for i in range(41)))[:44]
        t0 = time.monotonic()
        status, _, payload = get(f"/v1/networks/solana/token/{big}/page?include=identity,holders,insight")
        big_ms = (time.monotonic() - t0) * 1000
        page = routes.json_or_none(payload) or {}
        report["large_search"] = {"status": status, "ms": round(big_ms, 1),
                                  "symbol": (page.get("identity") or {}).get("symbol"),
                                  "errors": page.get("errors"), "rss_kib": rss_kib(server.process.pid)}
        if status != 200 or report["large_search"]["symbol"] != "SYN" or page.get("errors"):
            failures.append(f"large search: {report['large_search']}")

        # SIGKILL, then the same page from the durable KV.
        server.process.kill()
        server.process.wait()
        harness.set_clock(clock_file, now + 180_000)
        server = routes.Server(binary, kv_dir, clock_file, upstream, now)
        before = len(upstream.calls())
        status, _, payload = get(url)
        body = routes.json_or_none(payload) or {}
        after = {"status": status, "provider_calls": len(upstream.calls()) - before,
                 "identity_hit": (body.get("cache") or {}).get("identity", {}).get("hit"),
                 "holders": body.get("holders")}
        report["after_sigkill"] = after
        if status != 200 or after["provider_calls"] or (body.get("holders") or {}).get("source") != "jupiter":
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
    with tempfile.TemporaryDirectory(prefix="fortis-token-solana-") as root:
        ran = replay(binary, fixture, root, failures)
        report = {} if os.environ.get("TOKEN_SOLANA_REPLAY_ONLY") else native_checks(binary, root, failures)
    if failures:
        print(f"token solana replay: {len(failures)} failure(s) over {ran} cases")
        for failure in failures:
            print(" -", failure)
        raise SystemExit(1)
    print(f"token solana replay: {ran} cases match the TypeScript contract")
    if report:
        print("token solana native checks:", json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
