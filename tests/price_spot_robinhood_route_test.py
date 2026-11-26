#!/usr/bin/env python3
"""Replay the frozen TypeScript Robinhood stock-token route contract over local TLS."""
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import quote, unquote, urlsplit

import tls_ca
from quote_contract_expectations import retarget
from price_fallback_fixture_support import empty_fallback_reply, assert_fallback_for_case

FIXTURE = Path(__file__).resolve().parents[1] / "tests/corpus/price_spot_robinhood_route.json"
TOKEN = "fintel_dev_" + "P" * 43
NOW_MS = 1767323045000
EXPIRED_MS = NOW_MS + 700000
API_KEY = ""
A = "0x1111111111111111111111111111111111111111"
B = "0x2222222222222222222222222222222222222222"
SYMBOL = "BRK.B/US"
MINT = "So11111111111111111111111111111111111111112"
ASA = 31566705


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Upstream:
    def __init__(self, root):
        self.observed = []
        self.fallback_observed = []
        self.active_quotes = 0
        self.max_active_quotes = 0
        self.lock = threading.Lock()
        self.mode = "normal"
        ca, cert, key, _ = tls_ca.create_certificates(root, "DNS:localhost")
        self.ca = ca
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                if empty_fallback_reply(self, self.path, upstream.fallback_observed, upstream.lock): return
                parsed = urlsplit(self.path)
                status = 200
                with upstream.lock:
                    mode = upstream.mode
                    if mode == "providerFailure" and parsed.path.startswith("/rhj/"):
                        status = 503
                    upstream.observed.append((self.path, status))
                    is_quote = parsed.path.startswith("/rhj/prices/")
                    if is_quote:
                        upstream.active_quotes += 1
                        upstream.max_active_quotes = max(upstream.max_active_quotes, upstream.active_quotes)
                if status != 200:
                    body = b'{"error":"fixture"}'
                elif parsed.path == "/rhj/assets":
                    if mode == "malformedAssets":
                        body = b'{"assets":"bad"}'
                    else:
                        rows = [
                            {"id": "0x" + "ab" * 32, "tokenSymbol": "TSLA", "tokenName": "Tesla Stock Token", "deployments": [{"chainId": 4663, "contractAddress": A}], "currentMultiplier": "0.5", "pendingMultiplier": "", "status": "ASSET_STATUS_ACTIVE"},
                            {"id": "0x" + "cd" * 32, "tokenSymbol": SYMBOL, "tokenName": "BRK Stock Token", "deployments": [{"chainId": 4663, "contractAddress": B}], "currentMultiplier": "2", "pendingMultiplier": "", "status": "ASSET_STATUS_ACTIVE"},
                        ]
                        body = json.dumps({"assets": rows}, separators=(",", ":")).encode()
                elif parsed.path.startswith("/rhj/prices/"):
                    if mode == "parallelTwoSymbols":
                        time.sleep(0.35)
                    symbol = unquote(parsed.path.rsplit("/", 1)[-1])
                    if mode == "missingQuote":
                        quotes = []
                    elif mode == "invalidGeneratedAt":
                        quotes = [{"tokenSymbol": symbol, "currency": "USD", "bid": "200", "ask": "204", "generatedAt": "bad", "deployments": [{"chainId": 4663, "contractAddress": A if symbol == "TSLA" else B}]}]
                    elif mode == "duplicateMatchingQuoteFirstWins":
                        quotes = [{"tokenSymbol": symbol, "currency": "EUR", "bid": "200", "ask": "204", "generatedAt": "2026-01-02T03:04:04Z", "deployments": [{"chainId": 4663, "contractAddress": A if symbol == "TSLA" else B}]}, {"tokenSymbol": symbol, "currency": "USD", "bid": "200", "ask": "204", "generatedAt": "2026-01-02T03:04:04Z", "deployments": [{"chainId": 4663, "contractAddress": A if symbol == "TSLA" else B}]}]
                    else:
                        address = A if symbol == "TSLA" else B
                        quotes = [{"tokenSymbol": symbol, "currency": "USD", "bid": "200", "ask": "204", "dailyTradingVolume": "42", "isTradingHalt": mode == "haltedStillHasMidpoint", "generatedAt": "2026-01-02T03:04:04Z", "deployments": [{"chainId": 4663, "contractAddress": address}]}]
                    body = json.dumps({"quotes": quotes}, separators=(",", ":")).encode()
                elif parsed.path == "/price/v3":
                    body = b'{"data":{}}'
                elif parsed.path == "/api/v3/simple/price":
                    body = b'{"unrelated":{}}'
                else:
                    status, body = 404, b"{}"
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                try:
                    self.wfile.write(body)
                except OSError:
                    pass
                finally:
                    if parsed.path.startswith("/rhj/prices/"):
                        with upstream.lock:
                            upstream.active_quotes -= 1
                self.close_connection = True

            def log_message(self, *_args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", port()), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_address[1]

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


def wait_ready(app_port, process):
    for _ in range(150):
        if process.poll() is not None:
            raise RuntimeError(process.stderr.read().decode())
        try:
            with socket.create_connection(("127.0.0.1", app_port), timeout=.1):
                return
        except OSError:
            time.sleep(.03)
    raise RuntimeError("native route did not start")


def request(app_port, path, headers):
    with socket.create_connection(("127.0.0.1", app_port), timeout=20) as sock:
        lines = [f"GET {path} HTTP/1.1", "Host: localhost", "Connection: close"]
        lines.extend(f"{key}: {value}" for key, value in headers.items())
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
        data = bytearray()
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data.extend(chunk)
    head, _, body = bytes(data).partition(b"\r\n\r\n")
    lines = head.decode("latin1").split("\r\n")
    headers_out = {key.lower(): value.strip() for line in lines[1:] if ":" in line
                   for key, value in [line.split(":", 1)]}
    return int(lines[0].split()[1]), headers_out, body


def start(binary, app_port, kv_dir, upstream, now_ms, keyed):
    env = dict(os.environ)
    env.update({"FORTIS_ENABLE_TEST_PRICE_ROUTE": "1",
        "FORTIS_TEST_PRICE_UPSTREAM_PORT": str(upstream.port),
        "FORTIS_TEST_PRICE_UPSTREAM_CA": str(upstream.ca),
        "FORTIS_TEST_PRICE_NOW_MS": str(now_ms), "FORTIS_KV_DIR": str(kv_dir)})
    proc = subprocess.Popen([binary, str(app_port)], env=env,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    wait_ready(app_port, proc)
    return proc


def stop(process):
    if process and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2)


def main(binary):
    fixture = retarget(json.loads(FIXTURE.read_text()))
    cases = fixture["cases"]
    fixture_call_index = 0
    with tempfile.TemporaryDirectory(prefix="native-robinhood-route-") as temp:
        root = Path(temp)
        upstream = Upstream(root)
        app_port = port()
        shared = root / "kv"
        shared.mkdir()
        process = None
        segment = None
        observed_index = 0
        fallback_index = 0
        try:
            for name, case in cases.items():
                next_segment = case["segment"]
                if next_segment != segment:
                    stop(process)
                    kv_dir = shared if next_segment in ("coldMembershipAndQuote", "restart") else (root / ("kv-" + next_segment) if next_segment != "failure" else root / "kv-failure")
                    if kv_dir != shared and not kv_dir.exists():
                        kv_dir.mkdir()
                    process = start(binary, app_port, kv_dir, upstream, NOW_MS, False)
                    segment = next_segment
                upstream.mode = name if name in ("malformedAssets", "providerFailure", "missingQuote", "haltedStillHasMidpoint", "invalidGeneratedAt", "duplicateMatchingQuoteFirstWins", "parallelTwoSymbols") else "normal"
                req = case["request"]
                path = "/v1/prices?ids=" + quote(",".join(req["ids"]), safe="")
                if "fiat" in req:
                    path += "&fiat=" + req["fiat"]
                status, headers, body = request(app_port, path, {
                    "Authorization": "Bearer " + TOKEN,
                    "Accept": req.get("accept", "application/json")})
                assert status == case["status"], (name, status, body[:200])
                for key, value in case["headers"].items():
                    if key == "x-rate-limit-remaining":
                        assert headers.get(key, "").isdecimal(), (name, key)
                    else:
                        assert headers.get(key) == value, (name, key, headers.get(key), value)
                if req.get("accept") == "application/protobuf":
                    assert body.hex() == case["body"]["wireHex"], name
                else:
                    actual_body = json.loads(body)
                    expected_body = case["body"]
                    assert actual_body == expected_body, (name, json.dumps(actual_body)[:500], json.dumps(expected_body)[:500])
                fallback_index = assert_fallback_for_case(upstream, case, fallback_index, name)
                with upstream.lock:
                    calls = list(upstream.observed)
                fixture_total = case["providerCalls"]
                expected = [(urlsplit(item["url"]).path + ("?" + urlsplit(item["url"]).query if urlsplit(item["url"]).query else ""),
                             item["status"])
                            for item in fixture["providerRequests"][fixture_call_index:fixture_total]]
                actual = calls[observed_index:]
                assert len(calls) == fixture_total, (name, len(calls), fixture_total, calls)
                assert sorted(actual) == sorted(expected), (name, actual, expected)
                fixture_call_index = fixture_total
                if name == "parallelTwoSymbols":
                    assert upstream.max_active_quotes >= 2, upstream.max_active_quotes
                observed_index = len(calls)
            assert fixture_call_index == len(fixture["providerRequests"])
        finally:
            stop(process)
            upstream.close()
    print(f"native Robinhood stock-token route replay matched {len(cases)} frozen cases")


if __name__ == "__main__":
    main(sys.argv[1])
