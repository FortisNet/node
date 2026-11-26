#!/usr/bin/env python3
"""Replay the frozen TypeScript Tron TRC-20/TRC-10 token contract
(tests/corpus/token_tron.json, written by
the earlier TypeScript implementation's tests) against
build/fortis_node_token_route_test, then run native-only measurements.

The fake upstream serves the provider replies recorded in each case (the
case's own calls first, then the first recorded reply to the same request),
keyed by method, host, target and the POST body, so the native token owner
and spot owner receive the TypeScript bytes. Token owner requests name their
real host in x-fortis-test-host; the spot owner's requests carry none, so a
`/api/getAssetWithPriceList` path is TronScan and anything else CoinGecko.

Groups: `keyed` and `restart` run in two processes over one KV directory with
TRONSCAN_API_KEY and TRONGRID_API_KEY (the restart group reads what `keyed`
persisted); `keyless` starts a third process with neither key over a fresh
KV directory, as the TypeScript fixture uses a second app.

Native lanes refill by the monotonic clock where the TypeScript lanes used
the fixture clock, so the replay paces the CoinGecko lane (burst 4, 0.4/s)
and the TronScan lane (burst 20, one per 3 s) before each case, and waits out
the 30 s breaker a TronScan 429 opens.

Each request compares status, the fixture header set, the exact JSON body
bytes or protobuf wire bytes, and the provider request multiset including the
POST body and the TRON-PRO-API-KEY each request carried. Concurrent cases
compare both answers (the upstream then answers after 0.3 s so the second
reader joins the first one's snapshot flight).

Native-only checks (no TypeScript counterpart):
  - a cold and warm keyed TRC-20 page: latency, provider calls, RSS;
  - 23 simultaneous cached reads against the 2-worker/16-slot token queue;
  - 23 distinct cold TRC-20 contracts at once behind a 0.3 s upstream: every
    answer is 200 or 429 and every served page reads TronScan at most once;
    then a second round of 23 whose first TronScan read answers 429, opening
    the lane for 30 s: every page whose index read is shed falls back to the
    chain without a holder count (never an invented one);
  - SIGKILL, then the cached page from the durable KV with no provider call.
Usage: token_tron_test.py <binary>
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
from urllib.parse import parse_qs, urlsplit

import discovery_route_harness as harness
import tls_ca
import token_routes_test as routes

FIXTURE = "token_tron.json"
TRONSCAN = "apilist.tronscanapi.com"
TRONGRID = "api.trongrid.io"
BREAKER_COOLDOWN_S = 31.0
BASE58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def body_key(body):
    if not body:
        return ""
    try:
        return json.dumps(json.loads(body), sort_keys=True)
    except ValueError:
        return body if isinstance(body, str) else body.decode("utf-8", "replace")


def request_key(method, host, target, body):
    return (method, host, target, body_key(body) if method == "POST" else "")


def recorded_key(call):
    url = urlsplit(call["url"])
    return request_key(call["method"], url.netloc, url.path + (f"?{url.query}" if url.query else ""),
                       call.get("requestBody"))


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
        self.lock = threading.Lock()

    def use(self, case):
        with self.lock:
            self.current = {}
            for call in case.get("upstream", []):
                self.current.setdefault(recorded_key(call), []).append((call["status"], call["body"]))

    def __call__(self, method, host, target, body):
        if self.delay:
            time.sleep(self.delay)
        key = request_key(method, host, target, body)
        with self.lock:
            queue = self.current.get(key)
            if queue:
                status, payload = queue.pop(0) if len(queue) > 1 else queue[0]
            elif key in self.fallback:
                status, payload = self.fallback[key]
            else:
                status, payload = 404, '{"error":"unexpected request"}'
        return status, payload.encode()


class Upstream:
    """HTTPS fake upstream recording (method, host, target, body, key)."""

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
                tron_key = self.headers.get("TRON-PRO-API-KEY") or ""
                host = self.headers.get("x-fortis-test-host")
                if not host:
                    # The spot owner names no host: TronScan's TRC-10 price
                    # list, else CoinGecko.
                    host = TRONSCAN if self.path.startswith("/api/getAssetWithPriceList") else "api.coingecko.com"
                with upstream.lock:
                    upstream.observed.append((method, host, self.path, body, tron_key))
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
        body = call.get("requestBody")
        keys += [k + (call.get("tronKey", ""),)
                 for k in routes.normalize(call["method"], url.netloc, target, body.encode() if body else None)]
    return sorted(keys)


def observed_calls(calls):
    keys = []
    for method, host, target, body, tron_key in calls:
        keys += [k + (tron_key,) for k in routes.normalize(method, host, target, body)]
    return sorted(keys)


class Lane:
    """A native lane refilling by the monotonic clock: before a case, wait
    until it holds a token for every request the case makes to `host`."""

    def __init__(self, host, burst, per_second):
        self.host = host
        self.burst = float(burst)
        self.rate = per_second
        self.tokens = float(burst)
        self.at = time.monotonic()

    def reserve(self, case):
        need = sum(1 for call in case.get("upstream", []) if urlsplit(call["url"]).netloc == self.host)
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


def compare_response(name, expected, status, response_headers, payload, failures, secrets):
    def fail(message):
        failures.append(f"{name}: {message}")

    for secret in secrets:
        if secret.encode() in payload:
            fail("a provider key reached the response")
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
        # Keep each provider read in flight long enough for the second
        # worker to join the first one's flight (the TypeScript mock answers
        # after 25 ms; its SWR and snapshot flights share one read).
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
        compare_response(label, entry, status, response_headers, payload, failures, secrets)
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
    secrets = [fixture["source"]["tronscanKey"], fixture["source"]["trongridKey"]]
    ran = 0
    try:
        for group in ("keyed", "restart", "keyless"):
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
            lanes = [routes.CoinGeckoLane(), Lane(TRONSCAN, 20, 1 / 3)]
            try:
                for case in selected:
                    run_case(server, upstream, rules, clock_file, tokens, case, failures, lanes, secrets)
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


def contract(index):
    digits = []
    n = index
    for _ in range(8):
        digits.append(BASE58[n % 58])
        n //= 58
    return ("TSyn" + "".join(digits)).ljust(34, "z")


def word(value):
    return format(value, "064x")


def abi_string(text):
    raw = text.encode().hex()
    return word(32) + word(len(raw) // 2) + raw.ljust(max(64, -(-len(raw) // 64) * 64), "0")


class Synthetic:
    """TronScan lists every contract with a holder count; TronGrid answers
    every view with a distinct chain symbol; nothing else is listed."""

    def __init__(self):
        self.delay = 0.0
        self.reject_next_index = False
        self.lock = threading.Lock()

    def __call__(self, method, host, target, body):
        if self.delay:
            time.sleep(self.delay)
        split = urlsplit(target)
        query = parse_qs(split.query)
        if host == TRONSCAN and split.path == "/api/token_trc20":
            with self.lock:
                reject, self.reject_next_index = self.reject_next_index, False
            if reject:
                return 429, b'{"Error":"rate limited"}'
            c = (query.get("contract") or [""])[0]
            row = {"contract_address": c, "tokenType": "trc20", "tokenAbbr": "SYN", "tokenName": "Synthetic " + c[:8],
                   "tokenDecimal": 6, "total_supply": "1000000000", "nrOfTokenHolders": 1000 + len(c),
                   "transferCount": 5, "issue_address": c, "vip": True}
            return 200, json.dumps({"total": 1, "data": [row]}).encode()
        if host == TRONGRID and split.path == "/wallet/triggerconstantcontract":
            selector = json.loads(body or b"{}").get("function_selector")
            value = {"totalSupply()": word(10 ** 12), "decimals()": word(6), "symbol()": abi_string("CHN"),
                     "name()": abi_string("Chain")}.get(selector)
            return 200, json.dumps({"result": {"result": True}, "constant_result": [value]}).encode()
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
    keys = {"TRONSCAN_API_KEY": "tronscan-native-check-key", "TRONGRID_API_KEY": "trongrid-native-check-key"}
    url = f"/v1/networks/tron/token/{contract(0)}/page?include=identity,holders,insight"

    def get(path, token=None):
        return server.request("GET", path, headers={"Authorization": "Bearer " + (token or tokens.next()),
                                                    "Accept": "application/json"})

    harness.set_clock(clock_file, now)
    server = routes.Server(binary, kv_dir, clock_file, upstream, now, extra_env=keys)
    try:
        rss_boot = rss_kib(server.process.pid)
        before = len(upstream.calls())
        t0 = time.monotonic()
        status, _, payload = get(url)
        cold_ms = (time.monotonic() - t0) * 1000
        cold_calls = sorted((c[1], urlsplit(c[2]).path) for c in upstream.calls()[before:])
        body = routes.json_or_none(payload) or {}
        if status != 200 or (body.get("holders") or {}).get("source") != "tronscan" or not body.get("insight"):
            failures.append(f"measure: cold page {status} {payload[:300]!r}")
        if [c for c in cold_calls if c[0] == TRONSCAN] != [(TRONSCAN, "/api/token_trc20")]:
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

        # 23 distinct cold contracts at once behind a slow upstream.
        harness.set_clock(clock_file, now + 120_000)
        time.sleep(1.0)
        rules.delay = 0.3
        cold_before = len(upstream.calls())
        rss_before_cold = rss_kib(server.process.pid)
        cold = [None] * len(letters)
        paths = [f"/v1/networks/tron/token/{contract(i + 1)}/page?include=identity,holders,insight"
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

        def tally(results, calls, label):
            statuses, indexed, chain_only = {}, 0, 0
            for status, _, data in results:
                statuses[status] = statuses.get(status, 0) + 1
                if status != 200:
                    continue
                page = routes.json_or_none(data) or {}
                symbol = (page.get("identity") or {}).get("symbol")
                holders = page.get("holders")
                if symbol == "SYN" and (holders or {}).get("source") == "tronscan":
                    indexed += 1
                elif symbol == "CHN" or symbol is None:
                    chain_only += 1
                    if holders is not None:
                        failures.append(f"{label}: a chain-only snapshot carried holders {holders}")
                else:
                    failures.append(f"{label}: unexpected page {data[:300]!r}")
            tronscan_calls = {}
            for c in calls:
                if c[1] == TRONSCAN:
                    target = parse_qs(urlsplit(c[2]).query).get("contract", [""])[0]
                    tronscan_calls[target] = tronscan_calls.get(target, 0) + 1
            if set(statuses) - {200, 429}:
                failures.append(f"{label}: answered {statuses}")
            if any(n > 1 for n in tronscan_calls.values()):
                failures.append(f"{label}: a contract read TronScan more than once: {tronscan_calls}")
            return {"statuses": statuses, "tronscan_pages": indexed, "chain_only_pages": chain_only,
                    "tronscan_reads": sum(tronscan_calls.values()),
                    "trongrid_reads": sum(1 for c in calls if c[1] == TRONGRID)}

        first = tally(cold, upstream.calls()[cold_before:], "pressure")
        first["seconds"] = round(burst_seconds, 2)
        # The same burst again at once; its first index read is throttled.
        rules.delay = 0.3
        rules.reject_next_index = True
        shed_before = len(upstream.calls())
        shed = [None] * len(letters)
        shed_paths = [f"/v1/networks/tron/token/{contract(100 + i)}/page?include=identity,holders,insight"
                      for i in range(len(letters))]
        t0 = time.monotonic()
        threads = [threading.Thread(target=one, args=(i, shed_paths[i], shed)) for i in range(len(letters))]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        shed_seconds = time.monotonic() - t0
        server.request("GET", "/probe/test-token-idle")
        rules.delay = 0.0
        second = tally(shed, upstream.calls()[shed_before:], "lane shed")
        second["seconds"] = round(shed_seconds, 2)
        if not second["chain_only_pages"] or not second["tronscan_reads"]:
            failures.append(f"lane shed: expected a throttled index read and chain-only pages: {second}")
        rss_after = rss_kib(server.process.pid)
        report.update({
            "cold_ms": round(cold_ms, 1),
            "cold_provider_calls": cold_calls,
            "warm_ms_median": round(sorted(warm)[len(warm) // 2], 1),
            "warm_ms_max": round(max(warm), 1),
            "warm_provider_calls": warm_calls,
            "cached_burst_statuses": cached,
            "cached_burst_ms_max": round(max(ms for s, ms, _ in burst if s == 200), 1),
            "cold_contracts": first,
            "cold_contracts_throttled_index": second,
            "rss_kib": {"boot": rss_boot, "after_warm": rss_warm, "before_cold_burst": rss_before_cold,
                        "after_cold_burst": rss_after},
        })
        if warm_calls:
            failures.append(f"measure: {warm_calls} provider calls on warm reads")

        # SIGKILL, then the same page from the durable KV.
        server.process.kill()
        server.process.wait()
        harness.set_clock(clock_file, now + 180_000)
        server = routes.Server(binary, kv_dir, clock_file, upstream, now, extra_env=keys)
        before = len(upstream.calls())
        status, _, payload = get(url)
        body = routes.json_or_none(payload) or {}
        after = {"status": status, "provider_calls": len(upstream.calls()) - before,
                 "identity_hit": (body.get("cache") or {}).get("identity", {}).get("hit"),
                 "holders": body.get("holders")}
        report["after_sigkill"] = after
        if status != 200 or after["provider_calls"] or (body.get("holders") or {}).get("source") != "tronscan":
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
    with tempfile.TemporaryDirectory(prefix="fortis-token-tron-") as root:
        ran = replay(binary, fixture, root, failures)
        report = {} if os.environ.get("TOKEN_TRON_REPLAY_ONLY") else native_checks(binary, root, failures)
    if failures:
        print(f"token tron replay: {len(failures)} failure(s) over {ran} cases")
        for failure in failures:
            print(" -", failure)
        raise SystemExit(1)
    print(f"token tron replay: {ran} cases match the TypeScript contract")
    if report:
        print("token tron native checks:", json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
