#!/usr/bin/env python3
"""Replay the frozen TypeScript Vestige ASA route contract over local TLS."""
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
from urllib.parse import parse_qs, quote, urlsplit

import tls_ca

FIXTURE = Path(__file__).resolve().parents[1] / "tests/corpus/price_spot_asa_vestige_route.json"
TOKEN = "fintel_dev_" + "P" * 43
NOW_MS = 1767323045000
EXPIRED_MS = NOW_MS + 700000
A, B, C, D = "31566705", "31566706", "31566707", "31566708"
MINT = "So11111111111111111111111111111111111111112"


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Upstream:
    def __init__(self, root):
        self.observed = []
        self.lock = threading.Lock()
        self.fail_vestige = 0
        self.mode = "normal"
        ca, cert, key, _ = tls_ca.create_certificates(root, "DNS:localhost")
        self.ca = ca
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                parsed = urlsplit(self.path)
                query = parse_qs(parsed.query)
                with upstream.lock:
                    upstream.observed.append(self.path)
                    if parsed.path == "/assets/price" and upstream.fail_vestige:
                        upstream.fail_vestige -= 1
                        status, body = 503, b"upstream unavailable"
                    elif parsed.path == "/assets/price":
                        ids = query.get("asset_ids", [""])[0].split(",")
                        values = {A: 1.25, B: 0, C: -2.5, D: "3.5"}
                        if upstream.mode == "nonFinite":
                            body, status = b'[{"asset_id":31566705,"price":1e999}]', 200
                            rows = None
                        else:
                            rows = [{"asset_id": int(i), "price": values[i]} for i in ids if i in values]
                        if upstream.mode == "duplicate":
                            rows = [{"asset_id": int(A), "price": 2}, {"asset_id": int(A), "price": 0}]
                        elif upstream.mode == "malformed":
                            rows.append({"asset_id": int(D), "price": None})
                        if rows is not None:
                            body, status = json.dumps(rows, separators=(",", ":")).encode(), 200
                    elif parsed.path == "/price/v3":
                        body, status = json.dumps({MINT: {"usdPrice": 1}}, separators=(",", ":")).encode(), 200
                    else:
                        body, status = b"{}", 404
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
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        self.server.socket = tls.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_address[1]

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


def wait_ready(port_number, process):
    for _ in range(150):
        if process.poll() is not None:
            raise RuntimeError(process.stderr.read().decode())
        try:
            with socket.create_connection(("127.0.0.1", port_number), timeout=.1):
                return
        except OSError:
            time.sleep(.03)
    raise RuntimeError("native route did not start")


def request(port_number, path, headers):
    with socket.create_connection(("127.0.0.1", port_number), timeout=20) as sock:
        lines = [f"GET {path} HTTP/1.1", "Host: localhost", "Connection: close"]
        lines.extend(f"{k}: {v}" for k, v in headers.items())
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
        data = bytearray()
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data.extend(chunk)
    head, _, body = bytes(data).partition(b"\r\n\r\n")
    lines = head.decode("latin1").split("\r\n")
    return int(lines[0].split()[1]), {k.lower(): v.strip() for line in lines[1:]
        if ":" in line for k, v in [line.split(":", 1)]}, body


def start(binary, app_port, kv_dir, upstream, now_ms):
    env = dict(os.environ)
    env.update({"FORTIS_ENABLE_TEST_PRICE_ROUTE": "1",
        "FORTIS_TEST_PRICE_UPSTREAM_PORT": str(upstream.port),
        "FORTIS_TEST_PRICE_UPSTREAM_CA": str(upstream.ca),
        "FORTIS_TEST_PRICE_NOW_MS": str(now_ms), "FORTIS_KV_DIR": str(kv_dir)})
    env.pop("COINGECKO_API_KEY", None)
    proc = subprocess.Popen([binary, str(app_port)], env=env,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    wait_ready(app_port, proc)
    return proc


def stop(proc):
    if proc and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2)


def main(binary):
    fixture = json.loads(FIXTURE.read_text())
    cases = fixture["cases"]
    fixture_call_index = 0
    with tempfile.TemporaryDirectory(prefix="native-vestige-route-") as temp:
        root = Path(temp)
        upstream = Upstream(root)
        app_port = port()
        shared = root / "kv"
        shared.mkdir()
        proc = None
        segment = None
        observed_index = 0
        fixture_expected_index = 0
        native_extra_calls = 0
        try:
            for name, case in cases.items():
                next_segment = case["segment"]
                if next_segment != segment:
                    stop(proc)
                    kv_dir = shared if next_segment in ("cold", "restart", "expired") else root / ("kv-" + next_segment)
                    if kv_dir != shared:
                        kv_dir.mkdir()
                    proc = start(binary, app_port, kv_dir, upstream,
                                 EXPIRED_MS if next_segment == "expired" else NOW_MS)
                    segment = next_segment
                if name == "providerFailure": upstream.fail_vestige = 3
                if name == "duplicateRowsLastWins": upstream.mode = "duplicate"
                if name == "malformedRows": upstream.mode = "malformed"
                if name == "nonFiniteNumericPrice": upstream.mode = "nonFinite"
                req = case["request"]
                url = "/v1/prices?ids=" + quote(",".join(req["ids"]), safe="")
                if "fiat" in req: url += "&fiat=" + req["fiat"]
                status, headers, body = request(app_port, url, {
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
                elif name == "nonFiniteNumericPrice":
                    # Known TS/native gap: TS accepts JSON 1e999 as Infinity,
                    # serializes the first response price as null, and caches a
                    # null envelope. Native's bounded extractor treats it as an
                    # unanswered row and writes its normal negative envelope.
                    actual = json.loads(body)
                    assert actual["prices"] == {} and "asa:31566705" in actual["missing"], actual
                else:
                    actual = json.loads(body)
                    assert actual == case["body"], (name, json.dumps(actual)[:500], json.dumps(case["body"])[:500])
                with upstream.lock:
                    calls = list(upstream.observed)
                fixture_total = case["providerCalls"]
                expected = [urlsplit(entry).path + "?" + urlsplit(entry).query
                            for entry in fixture["providerRequests"][fixture_call_index:fixture_total]]
                actual = calls[observed_index:]
                if name == "nonFiniteSecondRead":
                    # Native retains bounded JSON's finite-number rule, so it
                    # treats 1e999 as a parse failure and retries on next read;
                    # TS caches a null envelope after its first Infinity quote.
                    assert len(calls) == fixture_total + 1, (name, len(calls), fixture_total, calls)
                    assert actual == ["/assets/price?network_id=0&asset_ids=31566705&denominating_asset_id=31566704"]
                    native_extra_calls = 1
                else:
                    assert len(calls) == fixture_total + native_extra_calls, (name, len(calls), fixture_total, calls)
                    assert sorted(actual) == sorted(expected), (name, actual, expected)
                fixture_expected_index += len(expected)
                fixture_call_index = fixture_total
                observed_index = len(calls)
                if name not in ("duplicateRowsLastWins", "nonFiniteNumericPrice"):
                    upstream.mode = "normal"
            assert fixture_expected_index == len(fixture["providerRequests"])
        finally:
            stop(proc)
            upstream.close()
    print(f"native Vestige ASA route replay matched {len(cases) - 2} cases; asserted {2} Infinity divergence cases separately")


if __name__ == "__main__":
    main(sys.argv[1])
