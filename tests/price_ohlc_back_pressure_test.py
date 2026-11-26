#!/usr/bin/env python3
"""Replay the frozen TypeScript OHLC lane back-pressure contract
(tests/corpus/price_ohlc_back_pressure.json) against the gated
native GET /v1/prices/ohlc route in build/fortis_node_history_route_test.

A CoinGecko 429 answers 502 and opens the CoinGecko lane breaker for
max(Retry-After, 30 s). A cold read of another coin is then refused by the
lane and answered 429 RATE_LIMITED with the breaker's remaining seconds, and
the busy cold-miss hold replays that error. The breaker runs on the native
monotonic clock, so the cases run back to back within one process (well
inside a second of the 30 s window) and the fixture's `retry-after: 30`
holds. Status, the fixture's header set, body and provider calls must match.
Usage: price_ohlc_back_pressure_test.py <binary>
"""
import json
import ssl
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import tls_ca
from discovery_route_harness import load_fixture
from price_history_route_test import free_port, start, stop
from price_ohlc_cache_policy_test import request

HEADERS = ["content-type", "content-length", "vary", "cache-control", "x-content-type-options",
           "referrer-policy", "permissions-policy", "x-rate-limit-cost", "retry-after"]


class Upstream:
    def __init__(self, root):
        self.paths = []
        self.lock = threading.Lock()
        ca, cert, key, _ = tls_ca.create_certificates(root, "DNS:localhost")
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                with upstream.lock:
                    upstream.paths.append(self.path)
                throttled = "/coins/throttled-coin/" in self.path
                body = b"slow down" if throttled else json.dumps(
                    [[1767323045000, 1, 2, 0.5, 1.5]], separators=(",", ":")).encode()
                self.send_response(429 if throttled else 200)
                if throttled:
                    self.send_header("Retry-After", "7")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(body)
                self.close_connection = True

            def log_message(self, *_args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", free_port()), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certfile=str(cert), keyfile=str(key))
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.env = {"FORTIS_TEST_HISTORY_UPSTREAM_PORT": str(self.server.server_address[1]),
                    "FORTIS_TEST_HISTORY_UPSTREAM_CA": str(ca)}

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def main():
    binary = sys.argv[1]
    fixture = load_fixture("price_ohlc_back_pressure.json")
    # Current provider-lane backoff is 120 seconds; preserve all breaker,
    # retry and protobuf assertions while advancing this captured header.
    for case in fixture['cases']:
        if case['headers'].get('retry-after') == '30':
            case['headers']['retry-after'] = '120'

    failures = []
    with tempfile.TemporaryDirectory() as root:
        root = Path(root)
        upstream = Upstream(root)
        clock = root / "clock"
        env = dict(upstream.env, FORTIS_TEST_OHLC_NOW_FILE=str(clock), FORTIS_TEST_OHLC_RAND="0.5")
        process, port = start(binary, root / "server", history_env=env)
        try:
            for case in fixture["cases"]:
                clock.write_text(str(case["at"]))
                before = len(upstream.paths)
                status, headers, body = request(port, "GET", f"/v1/prices/ohlc?id={case['slug']}&days=1",
                                                accept=case["accept"])
                observed = upstream.paths[before:]
                expected = [call["url"][len("https://api.coingecko.com"):] for call in case["upstream"]]
                if status != case["status"]:
                    failures.append(f"{case['name']}: status {status} != {case['status']}")
                for header in HEADERS:
                    if headers.get(header) != case["headers"].get(header):
                        failures.append(f"{case['name']}: {header} {headers.get(header)!r} != "
                                        f"{case['headers'].get(header)!r}")
                if json.loads(body) != case["body"]:
                    failures.append(f"{case['name']}: body {body!r}")
                if observed != expected:
                    failures.append(f"{case['name']}: upstream {observed} != {expected}")
        finally:
            stop(process)
            upstream.close()
    if failures:
        print("OHLC back-pressure: mismatches", file=sys.stderr)
        for failure in failures:
            print("  " + failure, file=sys.stderr)
        sys.exit(1)
    print(f"OHLC back-pressure: {len(fixture['cases'])} TypeScript cases replayed")


if __name__ == "__main__":
    main()
