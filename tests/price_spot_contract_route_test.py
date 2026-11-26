#!/usr/bin/env python3
"""Replay the frozen TypeScript contract-address spot route over local TLS."""
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
from quote_contract_expectations import retarget
from price_fallback_fixture_support import empty_fallback_reply, assert_fallback_for_case

FIXTURE = Path(__file__).resolve().parents[1] / "tests/corpus/price_spot_contract_addresses_route.json"
TOKEN = "fintel_dev_" + "P" * 43
NOW_MS = 1767323045000
EXPIRED_MS = NOW_MS + 700000
A = "0x" + "a" * 40
B = "0x" + "b" * 40
C = "0x" + "c" * 40
TRON = "TXLAQ63Xg1NAzckPwKHvzw7CSEmLMEqcdj"
APTOS = "0x1::coin::Coin"


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Upstream:
    def __init__(self, root):
        self.observed = []
        self.fallback_observed = []
        self.lock = threading.Lock()
        self.fail_ethereum = 0
        self.null_ethereum = False
        self.reject_anonymous_multi = False
        ca, cert, key, _ = tls_ca.create_certificates(root, "DNS:localhost")
        self.ca = ca
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                if empty_fallback_reply(self, self.path, upstream.fallback_observed, upstream.lock): return
                parsed = urlsplit(self.path)
                query = parse_qs(parsed.query)
                with upstream.lock:
                    upstream.observed.append((self.path, self.headers.get("x-cg-demo-api-key"),
                                              self.headers.get("x-cg-pro-api-key")))
                    if parsed.path.endswith("/simple/token_price/ethereum") and upstream.fail_ethereum:
                        upstream.fail_ethereum -= 1
                        status, body = 503, b"upstream unavailable"
                    elif parsed.path.endswith("/simple/token_price/ethereum") and upstream.null_ethereum:
                        status, body = 200, b"null"
                    elif (upstream.reject_anonymous_multi and
                          "/simple/token_price/" in parsed.path and
                          self.headers.get("x-cg-demo-api-key") is None and
                          self.headers.get("x-cg-pro-api-key") is None and
                          len(query.get("contract_addresses", [""])[0].split(",")) > 1):
                        status, body = 400, b'{"error_code":10012}'
                    elif "/simple/token_price/" in parsed.path:
                        platform = parsed.path.rsplit("/", 1)[-1]
                        fiat = query.get("vs_currencies", ["usd"])[0]
                        rows = {
                            "ethereum": {
                                A: {"usd": 12.5, "usd_24h_change": 1.25, "eur": 11.5},
                                B: {"usd": 0.125}, "0x" + "d" * 40: {"usd": "bad"}},
                            "base": {C: {"usd": 3.75, "usd_24h_change": -2}},
                            "tron": {TRON: {"usd": 7.25}},
                            "aptos": {APTOS: {"usd": 9}},
                        }.get(platform, {})
                        addresses = query.get("contract_addresses", [""])[0].split(",")
                        body = json.dumps({a: rows[a] for a in addresses if a in rows},
                                          separators=(",", ":")).encode()
                        status = 200
                    elif parsed.path.endswith("/simple/price"):
                        fiat = query.get("vs_currencies", ["usd"])[0]
                        body = json.dumps({"ethereum": {fiat: 3000,
                            f"{fiat}_24h_change": 2, f"{fiat}_24h_vol": 100}},
                            separators=(",", ":")).encode()
                        status = 200
                    elif parsed.path == "/price/v3":
                        body, status = b'{"So11111111111111111111111111111111111111112":{"usdPrice":1}}', 200
                    elif parsed.path == "/rhj/assets":
                        body, status = b"[]", 200
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


def start(binary, app_port, kv_dir, upstream, now_ms, api_key=None, tier="demo"):
    env = dict(os.environ)
    env.update({"FORTIS_ENABLE_TEST_PRICE_ROUTE": "1",
        "FORTIS_TEST_PRICE_UPSTREAM_PORT": str(upstream.port),
        "FORTIS_TEST_PRICE_UPSTREAM_CA": str(upstream.ca),
        "FORTIS_TEST_PRICE_NOW_MS": str(now_ms), "FORTIS_KV_DIR": str(kv_dir),
        "COINGECKO_TIER": tier})
    if api_key:
        env["COINGECKO_API_KEY"] = api_key
    else:
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
    fixture = retarget(json.loads(FIXTURE.read_text()))
    cases = fixture["cases"]
    fixture_call_index = 0
    with tempfile.TemporaryDirectory(prefix="native-contract-route-") as temp:
        root = Path(temp)
        upstream = Upstream(root)
        app_port = port()
        shared = root / "kv"
        shared.mkdir()
        proc = None
        segment = None
        observed_index = 0
        fallback_index = 0
        actual_expected_index = 0
        try:
            for name, case in cases.items():
                next_segment = case["segment"]
                if next_segment != segment:
                    stop(proc)
                    kv_dir = shared if next_segment in ("cold", "restart", "expired") else root / ("kv-" + next_segment)
                    if kv_dir != shared:
                        kv_dir.mkdir()
                    now = EXPIRED_MS if next_segment == "expired" else NOW_MS
                    api_key = fixture["source"]["demoKey"] if next_segment == "demoKeyed" else (
                        fixture["source"]["proKey"] if next_segment == "proKeyed" else None)
                    tier = "pro" if next_segment == "proKeyed" else "demo"
                    proc = start(binary, app_port, kv_dir, upstream, now, api_key, tier)
                    segment = next_segment
                if name == "providerFailure":
                    upstream.fail_ethereum = 3
                if name == "nullProviderBody":
                    upstream.null_ethereum = True
                req = case["request"]
                url = "/v1/prices?ids=" + quote(",".join(req["ids"]), safe="")
                if "fiat" in req:
                    url += "&fiat=" + req["fiat"]
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
                else:
                    actual = json.loads(body)
                    assert actual == case["body"], (name, json.dumps(actual)[:500], json.dumps(case["body"])[:500])
                fallback_index = assert_fallback_for_case(upstream, case, fallback_index, name)
                with upstream.lock:
                    calls = list(upstream.observed)
                fixture_total = case["providerCalls"]
                expected_total = fixture_total
                assert len(calls) == expected_total, (name, len(calls), expected_total)
                expected_slice = fixture["providerRequests"][fixture_call_index:fixture_total]
                actual_slice = calls[observed_index:]
                actual_rows = sorted((path, demo or "", pro or "") for path, demo, pro in actual_slice)
                expected_rows = sorted((urlsplit(entry["url"]).path + ("?" + urlsplit(entry["url"]).query if urlsplit(entry["url"]).query else ""),
                    entry["demoKey"] or "", entry["proKey"] or "") for entry in expected_slice)
                assert actual_rows == expected_rows, (name, actual_rows, expected_rows)
                actual_expected_index += len(expected_slice)
                fixture_call_index = fixture_total
                observed_index = len(calls)
                if name == "nullProviderBody":
                    upstream.null_ethereum = False
            assert actual_expected_index == len(fixture["providerRequests"])
            stop(proc)
            proc = None
            with upstream.lock:
                upstream.observed.clear()
                upstream.reject_anonymous_multi = True
            singleton_kv = root / "kv-anonymous-singletons"
            singleton_kv.mkdir()
            proc = start(binary, app_port, singleton_kv, upstream, NOW_MS)
            ids = [f"eth-mainnet:erc20:{A}", f"eth-mainnet:erc20:{B}"]
            url = "/v1/prices?ids=" + quote(",".join(ids), safe="")
            status, _, body = request(app_port, url, {
                "Authorization": "Bearer " + TOKEN, "Accept": "application/json"})
            assert status == 200, (status, body[:200])
            answer = json.loads(body)
            assert set(answer["prices"]) == set(ids), answer
            assert answer["missing"] == [], answer
            with upstream.lock:
                calls = [urlsplit(entry[0]) for entry in upstream.observed]
            assert len(calls) == 3, calls
            addresses = [parse_qs(call.query)["contract_addresses"][0].split(",")
                         for call in calls]
            assert sorted(map(len, addresses)) == [1, 1, 2], addresses
        finally:
            stop(proc)
            upstream.close()
    print(f"native CoinGecko contract route replay passed {len(cases)} fixture cases")


if __name__ == "__main__":
    main(sys.argv[1])
