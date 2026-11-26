#!/usr/bin/env python3
"""Replay the frozen TypeScript address/transaction/network contracts
(tests/corpus/address_*.json, written by
the earlier TypeScript implementation's tests)
against build/fortis_node_address_route_test.

Each fixture carries its REQUEST-KEYED provider table: `${METHOD} ${url}`
(plus ` ${body}` for requests with a body) -> one reply or a list consumed in
order. The fake upstream here answers exactly those bytes: chain transport
requests name their real host in x-fortis-test-host; the price test route's
spot owner sends none, so its host is recovered from the table by method,
path and body. A request missing from the table answers 404 and fails the
comparison, as it fails the TypeScript test.

Every case is sent at its fixture clock (the address and spot clocks share
one file). Status, the fixture header set, the body (exact JSON bytes for
successes, protobuf wire hex, parsed JSON for errors) and the multiset of
provider requests must match. Groups run in separate processes over one KV
directory per fixture, so later groups read what earlier ones persisted
across a restart.

The native provider lanes refill by the monotonic clock where the TypeScript
lanes refilled by the frozen fixture Date: before a case the replay waits
until each tight lane holds a token for every request the case makes.

Usage: address_routes_test.py <binary> [fixture.json ...]
"""
import json
import os
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

import compat_expectations  # noqa: E402
import discovery_route_harness as harness
import tls_ca

HEADERS = ["content-type", "content-length", "vary", "cache-control", "etag", "x-content-type-options",
           "referrer-policy", "permissions-policy", "x-rate-limit-cost", "retry-after"]
FIXTURES = ["address_evm.json", "address_utxo.json", "address_solana_tron.json", "address_ledger.json",
            "address_move_cosmos.json", "address_near_vechain_tao.json", "address_core.json"]
# Native lane budgets (src/providers/provider_lane.cpp): (requests, interval ms, burst).
LANES = {
    "coingecko": (24, 60000, 4), "geckoterminal": (10, 60000, 2), "4byte": (30, 60000, 30),
    "ethplorer": (30, 60000, 30), "etherscan": (3, 1000, 2), "alchemy": (100, 1000, 25),
    "blockscout": (30, 60000, 30), "koios": (50, 60000, 13), "horizon": (60, 60000, 15),
    "esplora": (30, 60000, 30), "algod": (60, 60000, 15), "algorand-indexer": (60, 60000, 15),
    "hedera-mirror": (60, 60000, 15), "vestige": (30, 60000, 30), "jupiter": (60, 60000, 15),
    "helius": (50, 1000, 13), "trongrid": (10, 1000, 3), "tronscan": (20, 60000, 20),
    "taostats": (30, 60000, 30), "nearblocks": (5, 60000, 5),
}
RPC_DEFAULT = (20, 1000, 5)


def lane_for(host):
    if host in ("api.coingecko.com", "pro-api.coingecko.com"):
        return "coingecko"
    table = {
        "api.geckoterminal.com": "geckoterminal", "api.etherscan.io": "etherscan",
        "api.ethplorer.io": "ethplorer", "www.4byte.directory": "4byte", "api.koios.rest": "koios",
        "horizon.stellar.org": "horizon", "horizon-testnet.stellar.org": "horizon",
        "mainnet-api.algonode.cloud": "algod", "mainnet-idx.algonode.cloud": "algorand-indexer",
        "mainnet-public.mirrornode.hedera.com": "hedera-mirror", "mempool.space": "esplora",
        "apilist.tronscanapi.com": "tronscan", "api.trongrid.io": "trongrid",
        "api.nearblocks.io": "nearblocks", "api.taostats.io": "taostats",
        "mainnet.helius-rpc.com": "helius", "api.helius.xyz": "helius", "lite-api.jup.ag": "jupiter",
        "api.jup.ag": "jupiter", "api.vestigelabs.org": "vestige",
    }
    if host in table:
        return table[host]
    if host.endswith(".g.alchemy.com"):
        return "alchemy"
    if "blockscout" in host or host == "flare-explorer.flare.network":
        return "blockscout"
    return "rpc:" + host


def normalize_url(url):
    split = urlsplit(url)
    path = split.path or "/"
    return f"https://{split.netloc}{path}" + (f"?{split.query}" if split.query else "")


def request_key(method, url, body):
    url = normalize_url(url)
    return f"{method} {url} {body}" if body else f"{method} {url}"


class Table:
    """The fixture's request-keyed provider replies with consumption order.
    Replies are served as the exact bytes the TypeScript run sent (recorded
    per call, in order); the table entry is the fallback for a request the
    TypeScript run never made."""

    def __init__(self, providers, cases):
        self.replies = {}
        self.by_path = {}
        self.recorded = {}
        for case in cases:
            down = (case.get("mode") or {}).get("down") or []
            for call in case.get("upstream", []):
                # An outage-mode 503 never consumed the provider's reply list.
                if urlsplit(call["url"]).netloc in down and call["status"] == 503:
                    continue
                key = request_key(call["method"], call["url"], call.get("requestBody") or "")
                self.recorded.setdefault(key, []).append((call["status"], call["body"]))
        for key, reply in providers.items():
            method, _, rest = key.partition(" ")
            url, _, body = rest.partition(" ")
            normalized = request_key(method, url, body)
            self.replies[normalized] = reply
            split = urlsplit(normalize_url(url))
            target = split.path + (f"?{split.query}" if split.query else "")
            self.by_path.setdefault((method, target, body), split.netloc)
        self.consumed = {}
        self.mode = {}
        self.lock = threading.Lock()

    def host_for(self, method, target, body):
        return self.by_path.get((method, target, body), "api.coingecko.com")

    def answer(self, method, host, target, body):
        if host in (self.mode.get("down") or []):
            return 503, b"unavailable", 25, False
        key = request_key(method, f"https://{host}{target}", body)
        reply = self.replies.get(key)
        index = self.consumed.get(key, 0)
        self.consumed[key] = index + 1
        first = reply[0] if isinstance(reply, list) and reply else reply
        delay = (first or {}).get("delayMs", 25)
        is_json = not first or "text" not in first
        recorded = self.recorded.get(key)
        if recorded:
            status, text = recorded[min(index, len(recorded) - 1)]
            if isinstance(reply, list) and reply:
                entry = reply[min(index, len(reply) - 1)]
                delay = entry.get("delayMs", 25)
                is_json = "text" not in entry
            if status == 503 and text == "unavailable":
                is_json = False
            return status, text.encode("utf-8", "surrogatepass"), delay, is_json
        if reply is None:
            return 404, json.dumps({"error": "unexpected request"}).encode(), 25, True
        if isinstance(reply, list):
            reply = reply[min(index, len(reply) - 1)]
        text = reply["text"] if "text" in reply else json.dumps(reply.get("json"), separators=(",", ":"),
                                                                ensure_ascii=False)
        return reply["status"], text.encode("utf-8", "surrogatepass"), reply.get("delayMs", 25), "text" not in reply


class Upstream:
    def __init__(self, root, table):
        self.table = table
        self.observed = []
        # Request headers (lowercase names) parallel to `observed`.
        self.observed_headers = []
        self.misses = []
        self.lock = threading.Lock()
        ca, cert, key, _ = tls_ca.create_certificates(Path(root), "DNS:localhost")
        self.ca = ca
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def handle_any(self, method):
                length = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(length).decode("utf-8", "surrogateescape") if length else ""
                host = self.headers.get("x-fortis-test-host")
                with upstream.table.lock:
                    if not host:
                        host = upstream.table.host_for(method, self.path, body)
                    status, payload, delay, is_json = upstream.table.answer(method, host, self.path, body)
                with upstream.lock:
                    upstream.observed.append((method, host, self.path, body))
                    upstream.observed_headers.append({k.lower(): v for k, v in self.headers.items()})
                    if status == 404 and payload.startswith(b'{"error":"unexpected request"}'):
                        upstream.misses.append(request_key(method, f"https://{host}{self.path}", body))
                time.sleep(delay / 1000)
                self.send_response(status)
                if status < 300 and is_json:
                    self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.send_header("Connection", "close")
                self.end_headers()
                try:
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
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        self.server.socket = tls.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_address[1]

    def calls(self):
        with self.lock:
            return list(self.observed)

    def call_headers(self):
        with self.lock:
            return list(self.observed_headers)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


def expected_calls(case):
    return sorted(request_key(c["method"], c["url"], c.get("requestBody") or "") for c in case.get("upstream", []))


def observed_calls(calls):
    return sorted(request_key(method, f"https://{host}{target}", body) for method, host, target, body in calls)


class Lanes:
    """Per-lane token buckets with the native budgets, in real time."""

    def __init__(self):
        self.state = {}

    def reserve(self, case):
        need = {}
        for call in case.get("upstream", []):
            lane = lane_for(urlsplit(call["url"]).netloc)
            need[lane] = need.get(lane, 0) + 1
        wait = 0.0
        now = time.monotonic()
        for lane, count in need.items():
            rate, interval, burst = LANES.get(lane, RPC_DEFAULT)
            per_ms = rate / interval
            tokens, at = self.state.get(lane, (float(burst), now))
            tokens = min(float(burst), tokens + (now - at) * 1000 * per_ms)
            want = min(float(count), float(burst))
            if tokens < want:
                wait = max(wait, (want - tokens) / per_ms / 1000 + 0.05)
            self.state[lane] = (tokens, now)
        if wait:
            time.sleep(wait)
        now = time.monotonic()
        for lane, count in need.items():
            rate, interval, burst = LANES.get(lane, RPC_DEFAULT)
            per_ms = rate / interval
            tokens, at = self.state[lane]
            tokens = min(float(burst), tokens + (now - at) * 1000 * per_ms)
            self.state[lane] = (max(0.0, tokens - count), now)


class Server:
    def __init__(self, binary, kv_dir, clock_file, upstream, env_keys):
        self.port = harness.free_port()
        env = dict(os.environ)
        for name in ("COINGECKO_API_KEY", "COINGECKO_TIER", "JUPITER_API_KEY", "ETHERSCAN_API_KEY",
                     "ETHPLORER_API_KEY", "TAOSTATS_API_KEY", "TRONSCAN_API_KEY", "ALCHEMY_API_KEY",
                     "HELIUS_API_KEY", "TRONGRID_API_KEY", "NEARBLOCKS_API_KEY", "GETBLOCK_DOGE_TOKEN",
                     "GETBLOCK_LTC_TOKEN", "GETBLOCK_ZEC_TOKEN", "DASH_BLOCKBOOK_URLS",
                     "FORTIS_NATIVE_PRICE_ROUTE_CANARY", "FORTIS_NATIVE_NEWS_CANARY",
                     "FORTIS_NATIVE_ADDRESS_CANARY"):
            env.pop(name, None)
        env.update(env_keys)
        env.update({
            "FORTIS_ENABLE_TEST_ADDRESS_ROUTES": "1",
            "FORTIS_TEST_ADDRESS_UPSTREAM_PORT": str(upstream.port),
            "FORTIS_TEST_ADDRESS_UPSTREAM_CA": str(upstream.ca),
            "FORTIS_TEST_ADDRESS_NOW_FILE": str(clock_file),
            "FORTIS_ENABLE_TEST_PRICE_ROUTE": "1",
            "FORTIS_TEST_PRICE_UPSTREAM_PORT": str(upstream.port),
            "FORTIS_TEST_PRICE_UPSTREAM_CA": str(upstream.ca),
            "FORTIS_TEST_PRICE_NOW_FILE": str(clock_file),
            "FORTIS_TEST_HISTORY_NOW_FILE": str(clock_file),
            "FORTIS_KV_DIR": str(kv_dir),
            "PORT": str(self.port),
        })
        self.process = subprocess.Popen([binary, str(self.port)], env=env, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE)
        for _ in range(300):
            if self.process.poll() is not None:
                raise RuntimeError("test server exited: " + self.process.stderr.read().decode())
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=.1):
                    return
            except OSError:
                time.sleep(.03)
        raise RuntimeError("test server did not start")

    def request(self, *args, **kwargs):
        return harness.Server.request(self, *args, **kwargs)

    def stop(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()


def json_or_none(data):
    try:
        return json.loads(data)
    except (ValueError, UnicodeDecodeError):
        return None


JOBS = {"pending-poll": "pending", "pending": "pending", "change": "change", "repair": "repair",
        "retention": "retention"}


def run_case(server, upstream, table, clock_file, token, case, failures, lanes):
    name = case["name"]
    lanes.reserve(case)
    table.mode = case.get("mode") or {}
    harness.set_clock(clock_file, case["at"])
    before = len(upstream.calls())
    expected = expected_calls(case)

    def fail(message):
        failures.append(f"{name}: {message}")

    if "job" in case:
        status, _, body = server.request("GET", f"/probe/test-address-job?job={JOBS.get(case['job'], case['job'])}")
        harness.wait_for_calls(upstream, before + len(case.get("upstream", [])), timeout=15.0)
        server.request("GET", "/probe/test-address-idle")
        observed = observed_calls(upstream.calls()[before:])
        if status != 200:
            fail(f"job status {status} {body[:200]!r}")
            return
        summary = json.loads(body)
        result = case.get("result")
        if isinstance(result, dict):
            for key, value in result.items():
                if summary.get(key) != value:
                    fail(f"job {key}: {summary.get(key)} != {value} ({summary})")
        elif isinstance(result, (int, float)) and summary:
            value = next(iter(summary.values()))
            if value != result:
                fail(f"job result {summary} != {result}")
        if observed != expected:
            fail(f"upstream\n  expected {expected}\n  actual   {observed}")
        return

    headers = {"Accept": case["accept"]}
    if not case.get("noAuth"):
        headers["Authorization"] = "Bearer " + token
    if case.get("ifNoneMatch"):
        headers["If-None-Match"] = case["ifNoneMatch"]
    status, response_headers, payload = server.request("GET", case["url"], headers=headers, timeout=90)
    harness.wait_for_calls(upstream, before + len(case.get("upstream", [])), timeout=15.0)
    server.request("GET", "/probe/test-address-idle")
    time.sleep(.05)
    observed = observed_calls(upstream.calls()[before:])

    if status != case["status"]:
        fail(f"status {status} != {case['status']} ({payload[:400]!r})")
        if observed != expected:
            fail(f"upstream\n  expected {expected}\n  actual   {observed}")
        return
    if case["status"] == 414:
        # find-my-way answers before Fastify's hooks; the native router keeps
        # its security header baseline (a documented allowance).
        # fastify-errors: {"error":"URI_TOO_LONG"}, charset=utf-8.
        mapped = compat_expectations.fastify_case(dict(case, body=json.dumps(case["body"])), text_key="body")
        case = dict(mapped, body=json.loads(mapped["body"]))
        if response_headers.get("content-type") != case["headers"].get("content-type"):
            fail(f"content-type {response_headers.get('content-type')!r}")
        if json_or_none(payload) != case["body"]:
            fail(f"body {payload[:400]!r} != {case['body']}")
        if observed != expected:
            fail(f"upstream\n  expected {expected}\n  actual   {observed}")
        return
    for header in HEADERS:
        if response_headers.get(header) != case["headers"].get(header):
            fail(f"header {header}: {response_headers.get(header)!r} != {case['headers'].get(header)!r}")
    if "wireHex" in case:
        if payload.hex() != case["wireHex"]:
            fail(f"protobuf wire\n  expected {case['wireHex'][:800]}\n  actual   {payload.hex()[:800]}")
    elif "bodyText" in case:
        if payload != case["bodyText"].encode("utf-8", "surrogatepass"):
            expected_text = case["bodyText"]
            actual_text = payload.decode("utf-8", "replace")
            index = next((i for i, (x, y) in enumerate(zip(expected_text, actual_text)) if x != y),
                         min(len(expected_text), len(actual_text)))
            fail(f"body differs at {index}\n  expected …{expected_text[max(0, index - 300):index + 300]}\n"
                 f"  actual   …{actual_text[max(0, index - 300):index + 300]}")
    elif case["status"] != 304:
        if json_or_none(payload) != case["body"]:
            fail(f"body {payload[:600]!r} != {json.dumps(case['body'])[:600]}")
    if observed != expected:
        fail(f"upstream\n  expected {expected}\n  actual   {observed}")


def load_fixture(name):
    """The fixture and its pinned sha256 (`sha256sum` format)."""
    import hashlib
    path = harness.FIXTURES / name
    data = path.read_bytes()
    pinned = (harness.FIXTURES / f"{name}.sha256").read_text().split()[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{name}: sha256 {digest} does not match the pinned {pinned}")
    import strict_contract_expectations
    from quote_contract_expectations import retarget
    return retarget(strict_contract_expectations.apply(name, json.loads(data)))


def replay(binary, fixture_name, failures):
    fixture = load_fixture(fixture_name)
    cases = fixture["cases"]
    only = os.environ.get("ADDRESS_ROUTES_ONLY_CASES")
    groups = []
    for case in cases:
        if case["group"] not in groups:
            groups.append(case["group"])
    ran = 0
    with tempfile.TemporaryDirectory(prefix="fortis-address-routes-") as root:
        table = Table(fixture["providers"], cases)
        upstream = Upstream(root, table)
        kv_dir = Path(root) / "kv"
        kv_dir.mkdir()
        clock_file = Path(root) / "clock"
        try:
            for group in groups:
                selected = [c for c in cases if c["group"] == group]
                harness.set_clock(clock_file, selected[0]["at"])
                server = Server(binary, kv_dir, clock_file, upstream, fixture.get("env") or {})
                lanes = Lanes()
                try:
                    for case in selected:
                        if only and case["name"] not in only.split(","):
                            continue
                        run_case(server, upstream, table, clock_file, fixture["token"], case, failures, lanes)
                        ran += 1
                finally:
                    server.stop()
        finally:
            upstream.close()
        if upstream.misses:
            failures.append(f"{fixture_name}: unmatched provider requests {sorted(set(upstream.misses))[:20]}")
    return ran


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixtures = sys.argv[2:] or FIXTURES
    total_failures = 0
    total = 0
    for name in fixtures:
        failures = []
        ran = replay(binary, name, failures)
        total += ran
        if failures:
            total_failures += len(failures)
            print(f"{name}: {len(failures)} failure(s) over {ran} cases")
            for failure in failures:
                print(" -", failure)
        else:
            print(f"{name}: {ran} cases match the TypeScript contract")
    if total_failures:
        raise SystemExit(1)
    print(f"address routes replay: {total} cases match")


if __name__ == "__main__":
    main()
