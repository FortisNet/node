#!/usr/bin/env python3
"""Replay the frozen Jupiter SPL spot contract against the gated native route.

The TypeScript fixture (price_spot_jupiter_route.json) is the oracle. One local
generated-CA upstream answers both CoinGecko and Jupiter paths and records every
request, so each case checks status, headers, body bytes, and the exact set of
provider calls it caused. Segments map to processes: "cold" starts an empty KV,
"restart" reopens the same KV, "expired" reopens it with the clock advanced
past every envelope, "protobuf" and "keyed" start fresh KVs.
"""
import json
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

import tls_ca

FIXTURE = Path(__file__).resolve().parents[1] / "tests/corpus/price_spot_jupiter_route.json"
TOKEN = "fintel_dev_" + "P" * 43
NOW_MS = 1767323045000          # 2026-01-02T03:04:05.000Z, the fixture clock
EXPIRED_MS = NOW_MS + 700_000   # the fixture's expiredTime
COINGECKO_QUOTES = {
    "usd": {"ethereum": (3012.44, -1.2), "bitcoin": (97000.5, 0.8), "litecoin": (88.25, 2.0)},
    "eur": {"ethereum": (2790.15, -1.1)},
}


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def request(port, path, *, headers=None):
    sock = socket.create_connection(("127.0.0.1", port), timeout=20)
    lines = [f"GET {path} HTTP/1.1", "Host: localhost", "Connection: close"]
    for key, value in (headers or {}).items():
        lines.append(f"{key}: {value}")
    sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
    data = bytearray()
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            break
        data.extend(chunk)
    sock.close()
    head, _, body = bytes(data).partition(b"\r\n\r\n")
    first, *header_lines = head.decode("latin1").split("\r\n")
    status = int(first.split()[1])
    response_headers = {}
    for line in header_lines:
        if ":" in line:
            key, value = line.split(":", 1)
            response_headers[key.lower()] = value.strip()
    return status, response_headers, body


def wait_port(port, process):
    for _ in range(150):
        if process.poll() is not None:
            raise RuntimeError("test server exited: " + process.stderr.read().decode())
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=.1):
                return
        except OSError:
            time.sleep(.03)
    raise RuntimeError("test server did not start")


class Upstream:
    """Generated-CA HTTPS upstream serving the fixture's provider replies."""

    def __init__(self, root, fixture):
        self.observed = []
        self.lock = threading.Lock()
        self.fail_second_batch = False
        self.first_failed_batch_page = threading.Event()
        mints = fixture["mints"]
        quotes = {}
        # Rebuild the quote table from the frozen cold replies so the replay
        # cannot drift from what the TypeScript oracle was shown.
        for case in ("oneMint", "multipleMints", "mixed", "batch"):
            for key, price in fixture["cases"][case]["body"]["prices"].items():
                if price["source"] == "jupiter" and not price["cache"]["hit"]:
                    row = {"usdPrice": price["price"]}
                    if price["change24hPct"] is not None:
                        row["priceChange24h"] = price["change24hPct"]
                    quotes[key[len("spl:"):]] = row
        self.quotes = quotes
        self.mints = mints
        ca, cert, key, _ = tls_ca.create_certificates(root, "DNS:localhost")
        self.ca = ca
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                parsed = urlsplit(self.path)
                query = parse_qs(parsed.query)
                ids = query.get("ids", [""])[0].split(",")
                with upstream.lock:
                    upstream.observed.append((self.path, self.headers.get("x-api-key")))
                if upstream.fail_second_batch and parsed.path == "/price/v3" and len(ids) == 50:
                    upstream.first_failed_batch_page.set()
                status, body = 200, b"{}"
                if parsed.path == "/price/v3":
                    status, body = upstream.jupiter(ids)
                elif parsed.path == "/api/v3/simple/price":
                    fiat = query.get("vs_currencies", ["usd"])[0]
                    table = COINGECKO_QUOTES.get(fiat, {})
                    body = json.dumps({slug: {fiat: table[slug][0], f"{fiat}_24h_change": table[slug][1]}
                                       for slug in ids if slug in table},
                                      separators=(",", ":")).encode()
                else:
                    status, body = 404, b"{}"
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                try:
                    self.wfile.write(body)
                    self.wfile.flush()
                except OSError:
                    pass
                self.close_connection = True

            def log_message(self, *_args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", free_port()), Handler)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        self.server.socket = tls.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_address[1]

    def jupiter(self, ids):
        m = self.mints
        if m["serverError"] in ids or (self.fail_second_batch and ids == [m["batchSecond"]]):
            return 503, b"upstream unavailable"
        if m["notFound"] in ids:
            return 404, b'{"error":"not found"}'
        if m["malformed"] in ids:
            return 200, b'{"x":'
        if m["arrayReply"] in ids:
            return 200, b"[]"
        if m["nullReply"] in ids:
            return 200, b"null"
        body = {}
        for mint in ids:
            if mint == m["stringPrice"]:
                body[mint] = {"usdPrice": "12.5"}
            elif mint == m["nullRow"]:
                body[mint] = None
            elif mint == m["noPrice"]:
                body[mint] = {"priceChange24h": 2}
            elif mint == m["stringChange"]:
                body[mint] = {"usdPrice": 3.25, "priceChange24h": "x"}
            elif mint in self.quotes:
                body[mint] = self.quotes[mint]
        return 200, json.dumps(body, separators=(",", ":")).encode()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


def start_process(binary, port, kv_dir, upstream, now_ms, *, api_key=None):
    env = dict(os.environ)
    env.pop("JUPITER_API_KEY", None)
    env.update({
        "FORTIS_ENABLE_TEST_PRICE_ROUTE": "1",
        "FORTIS_TEST_PRICE_UPSTREAM_PORT": str(upstream.port),
        "FORTIS_TEST_PRICE_UPSTREAM_CA": str(upstream.ca),
        "FORTIS_TEST_PRICE_NOW_MS": str(now_ms),
        "FORTIS_KV_DIR": str(kv_dir),
    })
    if api_key is not None:
        env["JUPITER_API_KEY"] = api_key
    process = subprocess.Popen([binary, str(port)], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    wait_port(port, process)
    return process


def stop_process(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)


def provider_paths(entries):
    return sorted(urlsplit(entry["url"]).path + "?" + urlsplit(entry["url"]).query
                  for entry in entries)


def run(binary):
    fixture = json.loads(FIXTURE.read_text())
    cases = fixture["cases"]
    provider_requests = fixture["providerRequests"]
    assert fixture["source"]["fixtureTime"] == "2026-01-02T03:04:05.000Z"
    assert fixture["source"]["expiredTime"] == "2026-01-02T03:15:45.000Z"
    with tempfile.TemporaryDirectory(prefix="native-jupiter-route-") as temp:
        root = Path(temp)
        upstream = Upstream(root, fixture)
        process = None
        try:
            shared_kv = root / "kv"
            shared_kv.mkdir()
            segment_clock = {"cold": NOW_MS, "restart": NOW_MS, "expired": EXPIRED_MS,
                             "protobuf": NOW_MS, "keyed": NOW_MS}
            current_segment = None
            app_port = free_port()
            previous_calls = 0
            def request_case(case):
                query = "ids=" + ",".join(case["request"]["ids"]).replace(":", "%3A")
                if "fiat" in case["request"]:
                    query += "&fiat=" + case["request"]["fiat"]
                headers = {}
                if "authorization" not in case["request"]:
                    headers["Authorization"] = "Bearer " + TOKEN
                headers["Accept"] = case["request"].get("accept", "application/json")
                return request(app_port, "/v1/prices?" + query, headers=headers)

            def verify_reply(name, response):
                case = cases[name]
                status, response_headers, body = response
                assert status == case["status"], (name, status, body[:200])
                for header, value in case["headers"].items():
                    if header == "x-rate-limit-remaining":
                        # The fixture freezes the price clock, while device quota
                        # uses wall-clock minute buckets. A minute can roll over
                        # during replay, so check the valid quota range instead
                        # of a stale absolute counter from the TS fixture.
                        remaining = response_headers.get(header)
                        assert remaining is not None and remaining.isdecimal(), (name, header, remaining)
                        assert 0 <= int(remaining) < 240, (name, header, remaining)
                        continue
                    assert response_headers.get(header) == value, (name, header,
                                                                   response_headers.get(header), value)
                if case["request"].get("accept") == "application/protobuf":
                    assert body.hex() == case["body"]["wireHex"], (name, body.hex())
                else:
                    actual = json.loads(body)
                    assert actual == case["body"], (name, json.dumps(actual)[:600])
                    assert len(body) == int(case["headers"]["content-length"]), name
                    if status == 200:
                        assert list(actual["prices"]) == list(case["body"]["prices"]), name

            def verify_provider_calls(name):
                nonlocal previous_calls
                with upstream.lock:
                    observed = list(upstream.observed)
                expected_calls = cases[name]["providerCalls"]
                assert len(observed) == expected_calls, (name, len(observed), expected_calls,
                                                         observed[previous_calls:])
                expected_slice = provider_requests[previous_calls:expected_calls]
                assert sorted(path for path, _key in observed[previous_calls:]) == \
                    provider_paths(expected_slice), (name, observed[previous_calls:])
                assert sorted(key or "" for _path, key in observed[previous_calls:]) == \
                    sorted(entry["apiKey"] or "" for entry in expected_slice), name
                previous_calls = expected_calls

            names = list(cases)
            index = 0
            while index < len(names):
                name = names[index]
                case = cases[name]
                segment = case["segment"]
                if segment != current_segment:
                    stop_process(process)
                    if segment in ("protobuf", "keyed"):
                        kv_dir = root / f"kv-{segment}"
                        kv_dir.mkdir()
                    else:
                        kv_dir = shared_kv
                    api_key = fixture["source"]["jupiterApiKey"] if segment == "keyed" else None
                    process = start_process(binary, app_port, kv_dir, upstream,
                                            segment_clock[segment], api_key=api_key)
                    current_segment = segment
                upstream.fail_second_batch = name in ("expiredBatchSecondFails",
                    "expiredSharedFlightJoin", "expiredBatchStillFailing")
                if name == "expiredBatchSecondFails":
                    assert names[index + 1] == "expiredSharedFlightJoin"
                    upstream.first_failed_batch_page.clear()
                    with ThreadPoolExecutor(max_workers=2) as pool:
                        batch = pool.submit(request_case, case)
                        assert upstream.first_failed_batch_page.wait(timeout=5), \
                            "first Jupiter page never reached the provider"
                        joined = pool.submit(request_case, cases[names[index + 1]])
                        batch_response = batch.result(timeout=20)
                        joined_response = joined.result(timeout=20)
                    verify_reply(name, batch_response)
                    verify_reply(names[index + 1], joined_response)
                    verify_provider_calls(names[index + 1])
                    index += 2
                    continue
                verify_reply(name, request_case(case))
                verify_provider_calls(name)
                index += 1
            assert previous_calls == len(provider_requests)

            # No Jupiter quote may carry a fabricated zero price or volume: the
            # series store keeps the live row with volume null (absent).
        finally:
            stop_process(process)
            upstream.close()
    print("native Jupiter SPL spot route matched the frozen contract: "
          f"{len(cases)} cases, {len(provider_requests)} provider calls")


if __name__ == "__main__":
    run(sys.argv[1])
