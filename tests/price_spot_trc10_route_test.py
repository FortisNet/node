#!/usr/bin/env python3
"""Replay the frozen TypeScript TronScan TRC-10 route contract over local TLS."""
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
from urllib.parse import quote, urlsplit

import tls_ca

FIXTURE = Path(__file__).resolve().parents[1] / "tests/corpus/price_spot_trc10_tronscan_route.json"
TOKEN = "fintel_dev_" + "P" * 43
NOW_MS = 1767323045000
EXPIRED_MS = NOW_MS + 700000
API_KEY = "fixture-tronscan-key"
A, B, C, D, E, F, G, H = 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008
MINT = "So11111111111111111111111111111111111111112"
ASA = 31566705


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Upstream:
    def __init__(self, root):
        self.observed = []
        self.lock = threading.Lock()
        self.mode = "normal"
        ca, cert, key, _ = tls_ca.create_certificates(root, "DNS:localhost")
        self.ca = ca
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                parsed = urlsplit(self.path)
                key = self.headers.get("TRON-PRO-API-KEY")
                status = 200
                with upstream.lock:
                    mode = upstream.mode
                    if key and mode in ("retry401", "retry403", "retry429"):
                        status = {"retry401": 401, "retry403": 403, "retry429": 429}[mode]
                    elif mode == "failure":
                        status = 503
                    upstream.observed.append((self.path, bool(key), status))
                if status != 200:
                    body = b'{"error":"fixture"}'
                elif parsed.path == "/api/getAssetWithPriceList":
                    if mode == "malformedData":
                        body = b'{"data":null}'
                    elif mode == "duplicateRowsLastWins":
                        body = json.dumps({"data": [
                            {"id": A, "type": "trc10", "priceInUsd": 1},
                            {"id": str(A), "type": "TRC10", "priceInUsd": "2.75"},
                        ]}, separators=(",", ":")).encode()
                    else:
                        rows = [
                            {"id": A, "type": "trc10", "priceInUsd": 1.25},
                            {"id": B, "type": "TRC10", "priceInUsd": " 2.5 "},
                            {"id": C, "type": "trc10", "priceInUsd": 0},
                            {"id": D, "type": "trc10", "priceInUsd": -3},
                            {"id": E, "type": "trc10", "priceInUsd": "not-a-number"},
                            {"id": F, "type": "trc10", "priceInUsd": "Infinity"},
                            {"id": G, "type": "trc10", "priceInUsd": "1.25e2"},
                            {"id": H, "type": "trc10", "priceInUsd": "0x10"},
                            {"id": A, "type": "trc20", "priceInUsd": 999},
                        ]
                        body = json.dumps({"data": rows}, separators=(",", ":")).encode()
                elif parsed.path == "/price/v3":
                    body = json.dumps({MINT: {"usdPrice": 1}}, separators=(",", ":")).encode()
                elif parsed.path == "/assets/price":
                    body = json.dumps([{"asset_id": ASA, "price": 3.5}], separators=(",", ":")).encode()
                elif parsed.path == "/api/v3/simple/price":
                    body = b'{"bitcoin":{"usd":10}}'
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
    if keyed:
        env["TRONSCAN_API_KEY"] = API_KEY
    else:
        env.pop("TRONSCAN_API_KEY", None)
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
    fixture = json.loads(FIXTURE.read_text())
    cases = fixture["cases"]
    fixture_call_index = 0
    with tempfile.TemporaryDirectory(prefix="native-tronscan-route-") as temp:
        root = Path(temp)
        upstream = Upstream(root)
        app_port = port()
        shared = root / "kv"
        shared.mkdir()
        process = None
        segment = None
        observed_index = 0
        try:
            for name, case in cases.items():
                next_segment = case["segment"]
                if next_segment != segment:
                    stop(process)
                    kv_dir = shared if next_segment in ("keyed", "restart", "expired", "negativeCached", "negativeRestartHit") else root / ("kv-" + next_segment)
                    if kv_dir != shared:
                        kv_dir.mkdir()
                    keyed = next_segment in ("keyed", "restart", "expired", "retry401", "retry403", "retry429")
                    process = start(binary, app_port, kv_dir, upstream,
                        EXPIRED_MS if next_segment == "expired" else NOW_MS, keyed)
                    segment = next_segment
                upstream.mode = ("failure" if name == "providerFailure" else name) if name in ("malformedData", "duplicateRowsLastWins", "providerFailure", "retry401", "retry403", "retry429") else "normal"
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
                with upstream.lock:
                    calls = list(upstream.observed)
                fixture_total = case["providerCalls"]
                expected = [(urlsplit(item["url"]).path + ("?" + urlsplit(item["url"]).query if urlsplit(item["url"]).query else ""),
                             item["keyed"], item["status"])
                            for item in fixture["providerRequests"][fixture_call_index:fixture_total]]
                actual = calls[observed_index:]
                assert len(calls) == fixture_total, (name, len(calls), fixture_total, calls)
                assert sorted(actual) == sorted(expected), (name, actual, expected)
                fixture_call_index = fixture_total
                observed_index = len(calls)
            assert fixture_call_index == len(fixture["providerRequests"])
        finally:
            stop(process)
            upstream.close()
    print(f"native TronScan TRC-10 route replay matched {len(cases)} frozen cases")


if __name__ == "__main__":
    main(sys.argv[1])
