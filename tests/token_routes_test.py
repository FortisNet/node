#!/usr/bin/env python3
"""Replay the frozen TypeScript token routes contract
(tests/corpus/token_routes.json) against the native routes in
build/fortis_node_token_route_test.

The fixture holds every provider table and the cases in order. The fake
upstream here applies the same reply rules as the TypeScript tests'
reply(): CoinGecko, DexScreener, GoPlus,
Ethplorer and EVM JSON-RPC, with each case's outage mode and clock. Token
owner requests name their real host in x-fortis-test-host; the spot owner's
CoinGecko requests carry none.

Each case is sent at the fixture clock. Status, the fixture header set, the
body (exact JSON bytes for successes, protobuf wire hex, parsed JSON for
errors) and the provider requests (a multiset; CoinGecko /coins/markets
batches are compared per coin id, since the TypeScript WindowBatcher merges
concurrent ids) must match. Job cases run the recurring job through the
test probe and compare its counts and provider requests. Groups run in
separate processes over one KV directory, so every later group reads what
the earlier ones persisted across a restart.

Recorded native differences (asserted, not ignored):
  - `errors` key order of a page with several failed sections follows
    promise resolution in TypeScript and facet order natively: compared as a
    mapping, with the validator/ETag and byte comparisons skipped for that
    case only;
  - the 414 reply keeps the native security header baseline (TypeScript
    routing fails before its hooks): status, body and content type are
    compared, the body as {"error":"URI_TOO_LONG"} (fastify-errors).
Usage: token_routes_test.py <binary>
"""
import json
import math
import os
import re
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlsplit

import compat_expectations  # noqa: E402
import discovery_route_harness as harness
import tls_ca

FIXTURE = "token_routes.json"
NOW = 1767323045000
DAY = 86400000
HEADERS = ["content-type", "content-length", "vary", "cache-control", "etag", "x-content-type-options",
           "referrer-policy", "permissions-policy", "x-rate-limit-cost", "retry-after"]
ORDER_INSENSITIVE_ERRORS = {"usdtOutage"}
ROUTER_ERRORS = {"tokenTooLong"}
GROUPS = [["seed"], ["cake"], ["validation"], ["failure"], ["holders"], ["jobs"]]


def dumps(value):
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode()


class Rules:
    """The TypeScript reply() over the fixture's provider tables."""

    def __init__(self, providers):
        self.p = providers
        self.mode = {}
        self.now = NOW

    def down(self, name):
        return name in (self.mode.get("down") or [])

    def __call__(self, method, host, target, body):
        split = urlsplit(target)
        path = split.path
        query = parse_qs(split.query, keep_blank_values=True)
        p = self.p

        def ok(value):
            return 200, dumps(value)

        if host == "api.coingecko.com":
            if path == "/api/v3/coins/markets":
                if self.down("coingecko-markets"):
                    return 503, b"unavailable"
                ids = (query.get("ids") or [""])[0].split(",")
                return ok([p["markets"][i] for i in ids if i in p["markets"]])
            if path.startswith("/api/v3/simple/token_price/"):
                return ok({})
            if path == "/api/v3/search":
                q = (query.get("query") or [""])[0]
                return ok(p["search"].get(q, {"coins": []}))
            m = re.match(r"^/api/v3/coins/([^/]+)/contract/([^/]+)$", path)
            if m:
                found = p["contracts"].get(f"{unquote(m.group(1))}/{unquote(m.group(2))}")
                return ok(found) if found is not None else (404, dumps({"error": "coin not found"}))
            m = re.match(r"^/api/v3/coins/([^/]+)$", path)
            if m:
                if self.down("coingecko-profile"):
                    return 503, b"unavailable"
                found = p["profiles"].get(unquote(m.group(1)))
                return ok(found) if found is not None else (404, dumps({"error": "coin not found"}))
            return 404, dumps({"error": "not found"})
        if host == "api.dexscreener.com":
            if self.down("dexscreener"):
                return 503, b"unavailable"
            address = unquote(path[len("/latest/dex/tokens/"):]).lower()
            return ok(p["dex"].get(address, {"schemaVersion": "1.0.0", "pairs": []}))
        if host == "api.gopluslabs.io":
            if self.down("goplus"):
                return ok({"code": 4029, "message": "too many requests", "result": None})
            chain = path[len("/api/v1/token_security/"):]
            address = (query.get("contract_addresses") or [""])[0].lower()
            return ok(p["goplus"].get(f"{chain}/{address}", {"code": 1, "message": "OK", "result": {}}))
        if host == "api.ethplorer.io":
            if self.down("ethplorer"):
                return 500, b"error"
            address = unquote(path[len("/getTokenInfo/"):]).lower()
            counts = p["ethplorer"].get(address)
            if counts is None:
                return ok({"error": {"code": 150, "message": "Address is not a token contract"}})
            day = max(0, math.floor((self.now - NOW) / DAY))
            return ok({"address": address, "holdersCount": counts[min(day, len(counts) - 1)]})
        if method == "POST" and host in ("ethereum-rpc.publicnode.com", "bsc-dataseed.binance.org"):
            if self.down("rpc"):
                return 500, b"error"
            request = json.loads(body or b"{}")
            data = ((request.get("params") or [{}])[0] or {}).get("data") or ""
            target_address = next((a for a in p["rpc"] if a[2:] in data), None)
            if target_address is None:
                return ok({"jsonrpc": "2.0", "id": 1, "error": {"code": -32000, "message": "execution reverted"}})
            return ok({"jsonrpc": "2.0", "id": 1, "result": p["rpc"][target_address]})
        return 404, dumps({"error": "unexpected host"})


class Upstream:
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
                host = self.headers.get("x-fortis-test-host") or "api.coingecko.com"
                with upstream.lock:
                    upstream.observed.append((method, host, self.path, body))
                    status, payload = upstream.rules(method, host, self.path, body)
                self.send_response(status)
                if status < 300:
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

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


def normalize(method, host, target, body):
    """Provider requests as comparable keys; /coins/markets per coin id."""
    split = urlsplit(target)
    if host == "api.coingecko.com" and split.path == "/api/v3/coins/markets":
        query = parse_qs(split.query, keep_blank_values=True)
        ids = (query.get("ids") or [""])[0].split(",")
        rest = sorted((k, v[0]) for k, v in query.items() if k != "ids")
        return [("GET", host, split.path, json.dumps(rest), coin) for coin in ids]
    if method == "POST":
        return [(method, host, split.path or "/", json.dumps(json.loads(body or b"{}"), sort_keys=True), "")]
    return [(method, host, target, "", "")]


def expected_calls(case):
    keys = []
    for call in case["upstream"]:
        url = urlsplit(call["url"])
        target = url.path + (f"?{url.query}" if url.query else "")
        body = call.get("requestBody")
        keys += normalize(call["method"], url.netloc, target, body.encode() if body else None)
    return sorted(keys)


def observed_calls(calls):
    keys = []
    for method, host, target, body in calls:
        keys += normalize(method, host, target, body)
    return sorted(keys)


class Server:
    def __init__(self, binary, kv_dir, clock_file, upstream, now_ms, extra_env=None):
        self.port = harness.free_port()
        env = dict(os.environ)
        for name in ("COINGECKO_API_KEY", "COINGECKO_TIER", "JUPITER_API_KEY", "ETHERSCAN_API_KEY",
                     "ETHPLORER_API_KEY", "TAOSTATS_API_KEY", "TRONSCAN_API_KEY", "TRONGRID_API_KEY",
                     "SAUCERSWAP_API_KEY", "FORTIS_NATIVE_PRICE_ROUTE_CANARY",
                     "FORTIS_NATIVE_NEWS_CANARY"):
            env.pop(name, None)
        env.update({
            "FORTIS_ENABLE_TEST_TOKEN_ROUTES": "1",
            "FORTIS_TEST_TOKEN_UPSTREAM_PORT": str(upstream.port),
            "FORTIS_TEST_TOKEN_UPSTREAM_CA": str(upstream.ca),
            "FORTIS_TEST_TOKEN_NOW_FILE": str(clock_file),
            "FORTIS_ENABLE_TEST_PRICE_ROUTE": "1",
            "FORTIS_TEST_PRICE_UPSTREAM_PORT": str(upstream.port),
            "FORTIS_TEST_PRICE_UPSTREAM_CA": str(upstream.ca),
            "FORTIS_TEST_PRICE_NOW_MS": str(now_ms),
            "FORTIS_KV_DIR": str(kv_dir),
            "PORT": str(self.port),
        })
        env.update(extra_env or {})
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
            self.process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()


def json_or_none(data):
    try:
        return json.loads(data)
    except (ValueError, UnicodeDecodeError):
        return None


class CoinGeckoLane:
    """The native "coingecko" lane refills by the monotonic clock (24/min,
    burst 4) where the TypeScript lane refilled by the frozen fixture Date.
    Before a case, wait until the lane holds a token for every CoinGecko
    request the case may make (markets counted per coin id: an upper bound,
    since the native port sends one id per request)."""

    def __init__(self):
        self.tokens = 4.0
        self.at = time.monotonic()

    def reserve(self, case):
        need = sum(1 for key in expected_calls(case) if key[1] == "api.coingecko.com")
        if not need:
            return
        now = time.monotonic()
        self.tokens = min(4.0, self.tokens + (now - self.at) * 0.4)
        self.at = now
        if self.tokens < need:
            time.sleep((need - self.tokens) / 0.4 + .05)
            self.tokens = float(need)
            self.at = time.monotonic()
        self.tokens -= need


def run_case(server, upstream, rules, clock_file, tokens, case, failures, lane):
    name = case["name"]
    lane.reserve(case)
    rules.mode = case.get("mode") or {}
    rules.now = case["at"]
    harness.set_clock(clock_file, case["at"])
    before = len(upstream.calls())
    expected = expected_calls(case)

    def fail(message):
        failures.append(f"{name}: {message}")

    if "job" in case:
        status, _, body = server.request("GET", f"/probe/test-token-job?job={case['job']}")
        harness.wait_for_calls(upstream, before + len(case["upstream"]), timeout=10.0)
        server.request("GET", "/probe/test-token-idle")
        observed = observed_calls(upstream.calls()[before:])
        if status != 200:
            fail(f"job status {status} {body[:200]!r}")
            return
        summary = json.loads(body)
        result = case["result"]
        if isinstance(result, dict):
            for key, value in result.items():
                field = "jobFailed" if key == "failed" else key
                if summary.get(field) != value:
                    fail(f"job {key}: {summary.get(field)} != {value} ({summary})")
        elif case["job"] == "stats":
            # Buckets are process-scoped: every earlier group's process flushed
            # its own at stop (the TypeScript onClose flush), so only success
            # is comparable; the demand projection below checks the content.
            if summary.get("failed"):
                fail(f"stats flush failed ({summary})")
        elif summary.get("written") != result:
            fail(f"job written: {summary.get('written')} != {result} ({summary})")
        if observed != expected:
            fail(f"upstream\n  expected {expected}\n  actual   {observed}")
        return

    headers = {"Authorization": "Bearer " + tokens.next(), "Accept": case["accept"]}
    if case.get("ifNoneMatch"):
        headers["If-None-Match"] = case["ifNoneMatch"]
    body = case.get("requestBody", "").encode()
    if body:
        headers["Content-Type"] = "application/json"
    status, response_headers, payload = server.request(case["method"], case["url"], body=body, headers=headers)
    harness.wait_for_calls(upstream, before + len(case["upstream"]), timeout=10.0)
    server.request("GET", "/probe/test-token-idle")
    time.sleep(.05)
    observed = observed_calls(upstream.calls()[before:])

    if status != case["status"]:
        fail(f"status {status} != {case['status']} ({payload[:400]!r})")
        return
    if name in ROUTER_ERRORS:
        # fastify-errors: the 414 is {"error":"URI_TOO_LONG"} as
        # application/json; charset=utf-8.
        if compat_expectations.fastify_code(case["body"]):
            mapped = compat_expectations.fastify_case(dict(case, body=json.dumps(case["body"])), text_key="body")
            case = dict(mapped, body=json.loads(mapped["body"]))
        if response_headers.get("content-type") != case["headers"].get("content-type"):
            fail(f"content-type {response_headers.get('content-type')!r}")
        if json_or_none(payload) != case["body"]:
            fail(f"body {payload[:400]!r} != {case['body']}")
        return
    skip = {"etag", "content-length"} if name in ORDER_INSENSITIVE_ERRORS else set()
    for header in HEADERS:
        if header in skip:
            continue
        if response_headers.get(header) != case["headers"].get(header):
            fail(f"header {header}: {response_headers.get(header)!r} != {case['headers'].get(header)!r}")
    if name in ORDER_INSENSITIVE_ERRORS:
        if json_or_none(payload) != case["body"]:
            fail("body differs beyond error key order")
    elif "wireHex" in case:
        if payload.hex() != case["wireHex"]:
            fail(f"protobuf wire\n  expected {case['wireHex'][:600]}\n  actual   {payload.hex()[:600]}")
    elif "bodyText" in case:
        if payload != case["bodyText"].encode("utf-8", "surrogatepass"):
            expected_text = case["bodyText"]
            actual_text = payload.decode("utf-8", "replace")
            index = next((i for i, (x, y) in enumerate(zip(expected_text, actual_text)) if x != y),
                         min(len(expected_text), len(actual_text)))
            fail(f"body differs at {index}\n  expected …{expected_text[max(0, index - 200):index + 200]}\n"
                 f"  actual   …{actual_text[max(0, index - 200):index + 200]}")
    elif case["status"] != 304:
        if json_or_none(payload) != case["body"]:
            fail(f"body {payload[:600]!r} != {json.dumps(case['body'])[:600]}")
    if observed != expected:
        fail(f"upstream\n  expected {expected}\n  actual   {observed}")


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = harness.load_fixture(FIXTURE)
    cases = fixture["cases"]
    only = os.environ.get("TOKEN_ROUTES_ONLY_GROUPS")
    failures = []
    with tempfile.TemporaryDirectory(prefix="fortis-token-routes-") as root:
        rules = Rules(fixture["providers"])
        upstream = Upstream(root, rules)
        kv_dir = Path(root) / "kv"
        kv_dir.mkdir()
        clock_file = Path(root) / "clock"
        tokens = harness.TokenRotation(per_token=40)
        ran = 0
        try:
            for groups in GROUPS:
                selected = [c for c in cases if c["group"] in groups]
                if not selected or (only and not any(g in only.split(",") for g in groups)):
                    continue
                harness.set_clock(clock_file, selected[0]["at"])
                server = Server(binary, kv_dir, clock_file, upstream, selected[0]["at"])
                lane = CoinGeckoLane()
                try:
                    for case in selected:
                        run_case(server, upstream, rules, clock_file, tokens, case, failures, lane)
                        ran += 1
                finally:
                    server.stop()
        finally:
            upstream.close()
    if failures:
        print(f"token routes replay: {len(failures)} failure(s) over {ran} cases")
        for failure in failures:
            print(" -", failure)
        raise SystemExit(1)
    print(f"token routes replay: {ran} cases match the TypeScript contract")


if __name__ == "__main__":
    main()
