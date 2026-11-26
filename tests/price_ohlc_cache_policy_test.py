#!/usr/bin/env python3
"""Deterministic OHLC cache-policy checks against the gated native route.

Replays the frozen TypeScript timeline (price_ohlc_swr_route.json) with a
file-driven cache clock and a pinned jitter sample, counting upstream requests
through fresh, stale, failure, and recovery windows on a generated local CA.
"""
import calendar
import json
import os
import ssl
import sys
import tempfile
import threading
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit, parse_qs

import socket

import tls_ca
from history_contract_expectations import FIXTURE_PROVIDER
from price_history_route_test import TOKEN, free_port, start, stop

FIXTURE = Path(__file__).resolve().parents[1] / "tests/corpus/price_ohlc_swr_route.json"
DAY = [1767323045123, 100, 110, 90, 105]
REFRESH = [1767323045124, 101, 111, 91, 106]
# A failing fetch is three attempts with 250 ms + 1 s retry pacing. Each
# attempt takes its own token from the CoinGecko lane (burst 4, 24/min), so
# a retry can queue for up to 2.5 s behind the refill. Each scenario runs in
# its own process so it starts with a full bucket.
FAILED_FETCH_TIMEOUT_S = 20


def request(port, method, url, accept="application/json"):
    with socket.create_connection(("127.0.0.1", port), timeout=FAILED_FETCH_TIMEOUT_S) as sock:
        lines = [f"{method} {url} HTTP/1.1", "Host: localhost", "Connection: close",
                 "Accept: " + accept, "Authorization: Bearer " + TOKEN]
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
        data = bytearray()
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data.extend(chunk)
    header, _, body = bytes(data).partition(b"\r\n\r\n")
    lines = header.decode("latin1").split("\r\n")
    status = int(lines[0].split()[1])
    headers = {key.lower(): value.strip() for line in lines[1:] if ":" in line
               for key, value in [line.split(":", 1)]}
    return status, headers, body


def at_ms(case):
    parsed = datetime.strptime(case["at"], "%Y-%m-%dT%H:%M:%S.%fZ")
    return calendar.timegm(parsed.timetuple()) * 1000 + parsed.microsecond // 1000


class Upstream:
    def __init__(self, root):
        self.hits = {}
        self.queries = []
        self.lock = threading.Lock()
        ca, cert, key, _ = tls_ca.create_certificates(root, "DNS:localhost")
        self.ca = ca
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                parsed = urlsplit(self.path)
                assert parsed.path.startswith("/api/v3/coins/") and parsed.path.endswith("/ohlc"), parsed.path
                slug = parsed.path.split("/")[-2]
                with upstream.lock:
                    upstream.hits[slug] = upstream.hits.get(slug, 0) + 1
                    call = upstream.hits[slug]
                    upstream.queries.append(parse_qs(parsed.query))
                # The same per-slug rules as the TypeScript fixture's fetch stub:
                # a 404 refresh failure (answered, not retried, no breaker fault)
                # and a 503 cold failure (three attempts).
                if slug == "backoff-coin":
                    status, body = ((200, [DAY]) if call == 1 else (404, {"error": "coin not found"})
                                    if call <= 3 else (200, [REFRESH]))
                elif slug == "replay-coin":
                    status, body = (503, None) if call <= 3 else (200, [DAY])
                else:
                    status, body = 200, ([DAY] if call == 1 else [REFRESH])
                encoded = (b"provider unavailable" if body is None
                           else json.dumps(body, separators=(",", ":")).encode())
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(encoded)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(encoded)
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

    def count(self, slug):
        with self.lock:
            return self.hits.get(slug, 0)

    def wait(self, slug, expected, timeout=FAILED_FETCH_TIMEOUT_S):
        deadline = time.monotonic() + timeout
        while self.count(slug) < expected and time.monotonic() < deadline:
            time.sleep(.02)
        assert self.count(slug) == expected, (slug, self.count(slug), expected)
        # Let the owner deliver the final attempt's result to the route.
        time.sleep(.3)
        assert self.count(slug) == expected, (slug, self.count(slug), expected)

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class Clock:
    def __init__(self, path):
        self.path = path

    def set(self, ms):
        temporary = self.path.with_suffix(".tmp")
        temporary.write_text(str(ms))
        os.replace(temporary, self.path)


def expect(port, slug, case, accept="application/json"):
    status, headers, body = request(port, "GET", f"/v1/prices/ohlc?id={slug}&days=1", accept=accept)
    assert status == case["status"], (slug, case["at"], status, body[:200])
    if accept == "application/json":
        assert json.loads(body) == case["body"], (slug, case["at"], body[:300])
    return headers


def poll(port, slug, case, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status, _, body = request(port, "GET", f"/v1/prices/ohlc?id={slug}&days=1")
        if status == case["status"] and json.loads(body) == case["body"]:
            return
        time.sleep(.05)
    raise AssertionError((slug, case["at"], status, body[:300]))


def scenario(binary, root, upstream, name, rand, steps):
    clock = Clock(root / f"{name}.now")
    clock.set(0)
    env = {**upstream.env, **FIXTURE_PROVIDER, "FORTIS_TEST_OHLC_NOW_FILE": str(clock.path),
           "FORTIS_TEST_OHLC_RAND": rand}
    process, port = start(binary, root / name, history_env=env)
    try:
        steps(port, clock)
    finally:
        stop(process)
    assert process.returncode == 0, (name, process.stderr.read().decode()[-500:])


def main(binary):
    cases = json.loads(FIXTURE.read_text())["cases"]
    with tempfile.TemporaryDirectory(prefix="ohlc-policy-") as temporary:
        root = Path(temporary)
        upstream = Upstream(root)

        # Jitter: rand 0 stores 270 s of freshness, rand 1 stores 330 s. The
        # boundary is exact and the stale edge schedules exactly one refresh.
        def jitter_low(port, clock):
            slug = "jitter-coin"
            clock.set(at_ms(cases["jitterLowCold"]))
            expect(port, slug, cases["jitterLowCold"])
            assert upstream.count(slug) == 1
            clock.set(at_ms(cases["jitterLowFreshEdge"]))
            expect(port, slug, cases["jitterLowFreshEdge"])
            assert expect(port, slug, cases["jitterLowFreshEdge"],
                          accept="application/protobuf")["content-type"] == "application/protobuf"
            assert upstream.count(slug) == 1
            clock.set(at_ms(cases["jitterLowStaleEdge"]))
            expect(port, slug, cases["jitterLowStaleEdge"])
            upstream.wait(slug, 2)
            clock.set(at_ms(cases["jitterLowRevalidated"]))
            poll(port, slug, cases["jitterLowRevalidated"])
            assert upstream.count(slug) == 2
        scenario(binary, root, upstream, "jitter-low", "0", jitter_low)

        def jitter_high(port, clock):
            slug = "jitter-high-coin"
            clock.set(at_ms(cases["jitterHighCold"]))
            expect(port, slug, cases["jitterHighCold"])
            clock.set(at_ms(cases["jitterHighFreshEdge"]))
            expect(port, slug, cases["jitterHighFreshEdge"])
            assert upstream.count(slug) == 1
            clock.set(at_ms(cases["jitterHighStaleEdge"]))
            expect(port, slug, cases["jitterHighStaleEdge"])
            upstream.wait(slug, 2)
        scenario(binary, root, upstream, "jitter-high", "1", jitter_high)

        # rand 0.5 stores the unjittered 300 s so the backoff and replay
        # windows line up with the frozen TypeScript clock exactly.
        def backoff(port, clock):
            slug = "backoff-coin"
            clock.set(at_ms(cases["backoffCold"]))
            expect(port, slug, cases["backoffCold"])
            assert upstream.count(slug) == 1
            clock.set(at_ms(cases["backoffStaleRefreshFails"]))
            expect(port, slug, cases["backoffStaleRefreshFails"])
            upstream.wait(slug, 2)
            clock.set(at_ms(cases["backoffHeld"]))
            for _ in range(3):
                expect(port, slug, cases["backoffHeld"])
            time.sleep(.3)
            assert upstream.count(slug) == 2, "stale reads retried a provider that just failed"
            clock.set(at_ms(cases["backoffHeldEdge"]))
            expect(port, slug, cases["backoffHeldEdge"])
            time.sleep(.3)
            assert upstream.count(slug) == 2
            clock.set(at_ms(cases["backoffRetryFails"]))
            expect(port, slug, cases["backoffRetryFails"])
            upstream.wait(slug, 3)
            clock.set(at_ms(cases["backoffRetryFails"]) + 1)
            expect(port, slug, cases["backoffRetryFails"])
            time.sleep(.3)
            assert upstream.count(slug) == 3, "a second failure did not renew the backoff"
            clock.set(at_ms(cases["backoffRecovered"]))
            expect(port, slug, cases["backoffRecovered"])
            upstream.wait(slug, 4)
            clock.set(at_ms(cases["backoffFresh"]))
            poll(port, slug, cases["backoffFresh"])
            assert upstream.count(slug) == 4
        scenario(binary, root, upstream, "backoff", "0.5", backoff)

        def replay(port, clock):
            slug = "replay-coin"
            clock.set(at_ms(cases["replayCold"]))
            expect(port, slug, cases["replayCold"])
            assert upstream.count(slug) == 3
            clock.set(at_ms(cases["replayHeld"]))
            for _ in range(3):
                headers = expect(port, slug, cases["replayHeld"])
            # runFacet's replayed 502 carries no `vary` (price_ohlc_back_pressure.json).
            assert "vary" not in headers and headers.get("x-rate-limit-cost") == "5", headers
            clock.set(at_ms(cases["replayHeldEdge"]))
            expect(port, slug, cases["replayHeldEdge"])
            assert upstream.count(slug) == 3, "a held miss queued for the provider again"
            clock.set(at_ms(cases["replayRecovered"]))
            expect(port, slug, cases["replayRecovered"])
            assert upstream.count(slug) == 4
            clock.set(at_ms(cases["replayCached"]))
            expect(port, slug, cases["replayCached"])
            assert upstream.count(slug) == 4
        scenario(binary, root, upstream, "replay", "0.5", replay)

        # Holds are process memory, like the TypeScript maps: a restart on the
        # same KV forgets the replay hold and asks the provider again.
        def restart_forgets_hold(port, clock):
            slug = "replay-coin"
            clock.set(at_ms(cases["replayCached"]) + 1)
            expect(port, slug, cases["replayCached"])
            assert upstream.count(slug) == 4
            clock.set(at_ms(cases["replayCached"]) + 1 + 3_600_000 + 300_000)
            status, _, body = request(port, "GET", f"/v1/prices/ohlc?id={slug}&days=1")
            assert status == 200 and json.loads(body)["cache"]["hit"] is False, (status, body[:200])
            assert upstream.count(slug) == 5
        scenario(binary, root, upstream, "replay", "0.5", restart_forgets_hold)

        assert all(query.get("vs_currency") == ["usd"] and query.get("days") == ["1"]
                   for query in upstream.queries), upstream.queries
        upstream.close()
    print("native OHLC cache policy: jittered freshness, stale refresh backoff, "
          "cold failure replay, and restart passed")


if __name__ == "__main__":
    main(sys.argv[1])
