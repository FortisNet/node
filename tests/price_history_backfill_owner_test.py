#!/usr/bin/env python3
"""Generated-CA integration for the standalone native price-history owner."""
from collections import Counter
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import urlsplit, parse_qs

import tls_ca


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


DECADE_LIMIT_BYTES = 512 * 1024


def decade_body(start: int, end: int, points=None, caps: bool = True) -> bytes:
    """A CoinGecko daily feed from `start`, one sample per midnight through
    `end` (or `points` samples), shaped like the ten-year fixture model."""
    first = -(-start // 86400) * 86400
    count = points if points is not None else (end - first) // 86400 + 1
    stamps = [(first + d * 86400) * 1000 for d in range(count)]
    parts = ['"prices":[%s]' % ",".join("[%d,%s]" % (t, repr(100 + d * 0.375))
                                         for d, t in enumerate(stamps))]
    if caps:
        parts.append('"market_caps":[%s]' % ",".join(
            "[%d,%s]" % (t, repr(1e12 + 29628 * d + 0.25)) for d, t in enumerate(stamps)))
    parts.append('"total_volumes":[%s]' % ",".join("[%d,%s]" % (t, repr(1000 + 1.5 * d))
                                                   for d, t in enumerate(stamps)))
    return ("{" + ",".join(parts) + "}").encode()


def run(binary: str, fixture_path: str, daily_fixture_path: str) -> None:
    fixture = json.loads(Path(fixture_path).read_text())
    wire = fixture["wireResponse"]
    daily_wire = json.loads(Path(daily_fixture_path).read_text())["wireResponse"]
    with tempfile.TemporaryDirectory(prefix="history-owner-") as temp:
        root = Path(temp)
        ca, cert, key, _ = tls_ca.create_certificates(root, "DNS:localhost")
        port = free_port()
        counts: Counter[str] = Counter()
        requested: list[tuple[str, int, int, str]] = []
        lock = threading.Lock()

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self) -> None:
                parsed = urlsplit(self.path)
                query = parse_qs(parsed.query)
                coin = parsed.path.split("/")[4]
                start, end = int(query["from"][0]), int(query["to"][0])
                with lock:
                    counts[coin] += 1
                    attempt = counts[coin]
                    requested.append((coin, start, end, self.path))
                if coin == "rate-limit":
                    status, body = 429, b'{"error":"rate limited"}'
                    self.send_response(status)
                    self.send_header("Retry-After", "7")
                elif coin == "server-fails" or (coin == "server-retry" and attempt < 3):
                    status, body = 503, b'{"error":"retry fixture"}'
                    self.send_response(status)
                elif coin == "malformed":
                    status, body = 200, b'{"prices":"wrong type","total_volumes":[]}'
                    self.send_response(status)
                elif coin == "decade-401":
                    status, body = 401, b'{"status":{"error_code":10012}}'
                    self.send_response(status)
                elif coin.startswith("decade-") or coin == "slow-decade-cancel":
                    if coin == "slow-decade-cancel":
                        (root / coin).touch()
                        time.sleep(1.5)
                    if coin == "decade-oversize":
                        body = decade_body(start, end)
                        body = body[:-1] + (',"pad":"%s"}' % (
                            "x" * (DECADE_LIMIT_BYTES + 4096 - len(body)))).encode()
                    elif coin == "decade-toomany":
                        body = decade_body(start, end, points=3800, caps=False)
                        assert len(body) < DECADE_LIMIT_BYTES
                    else:
                        body = decade_body(start, end)
                        # Over the old 256 KiB body and 32,768-token bounds.
                        assert len(body) > 256 * 1024
                    status = 200
                    self.send_response(status)
                else:
                    if coin in ("slow-cancel", "slow-daily-cancel", "slow-timeout", "shared-slow-cancel",
                                "shared-slow-shutdown") or coin.startswith("slow-queue-"):
                        (root / coin).touch()
                        time.sleep(1.5)
                    status = 200
                    if coin in ("large", "partial-large"):
                        first_ms = start * 1000
                        body_obj = {
                            "prices": [[first_ms + i * 3600000, 100 + i]
                                       for i in range(2160)],
                            "total_volumes": [[first_ms + i * 3600000, -i]
                                              for i in range(2160)],
                        }
                    else:
                        if coin in ("daily-365", "partial-365", "slow-daily-cancel",
                                    "shared-daily-365"):
                            if coin == "daily-365":
                                time.sleep(0.35)  # let identical callers join one flight
                            body_obj = daily_wire
                        else:
                            if coin == "mixed-interval":
                                time.sleep(0.35)  # let the 1d caller join the 1h flight
                            body_obj = wire
                    body = json.dumps(body_obj, separators=(",", ":")).encode()
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

            def log_message(self, *_args) -> None:
                pass

        server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        server.socket = tls.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            subprocess.run([str(Path(binary).resolve()), str(port), str(ca), str(root)],
                           check=True, timeout=150)
            by_coin = Counter(item[0] for item in requested)
            assert by_coin["ethereum"] == 1, by_coin
            assert by_coin["commit-deadline"] == 1, by_coin
            assert by_coin["commit-stop"] == 1, by_coin
            assert by_coin["rate-limit"] == 1, by_coin
            assert by_coin["server-retry"] == 3, by_coin
            assert by_coin["server-fails"] == 3, by_coin
            assert by_coin["malformed"] == 1, by_coin
            assert by_coin["slow-cancel"] == 1, by_coin
            assert by_coin["slow-timeout"] == 1, by_coin
            assert 0 <= sum(count for coin, count in by_coin.items()
                            if coin.startswith("slow-queue-")) <= 8, by_coin
            assert by_coin["partial-large"] == 3, by_coin
            assert by_coin["daily-365"] == 1, by_coin
            assert by_coin["partial-365"] == 2, by_coin
            assert by_coin["slow-daily-cancel"] == 1, by_coin
            assert by_coin["shared-daily-365"] == 1, by_coin
            assert by_coin["shared-success"] == 1, by_coin
            assert by_coin["shared-slow-cancel"] == 1, by_coin
            assert by_coin["shared-slow-shutdown"] == 1, by_coin
            # Coverage: 1h then 1d fetch, then a covered sub-window (no call).
            assert by_coin["interval-split"] == 2, by_coin
            assert by_coin["mixed-interval"] == 1, by_coin
            assert by_coin["corrupt-coin"] == 1, by_coin
            assert by_coin["v1-coin"] == 1, by_coin
            # Torn coverage write, then the refetch after restart.
            assert by_coin["coverage-write-fail"] == 2, by_coin
            # Ten-year daily window: one request each; 401, over-bound bodies
            # and too many points are never retried.
            for coin in ("decade-coin", "decade-401", "decade-oversize", "decade-toomany",
                         "slow-decade-cancel"):
                assert by_coin[coin] == 1, (coin, by_coin)
            decade = [item for item in requested if item[0] == "decade-coin"]
            assert decade[0][2] - decade[0][1] == 10 * 366 * 86400, decade
            assert all(path.startswith("/api/v3/coins/") and
                       path.endswith("/market_chart/range?vs_currency=usd&from=" + str(a) + "&to=" + str(b))
                       for _, a, b, path in requested)
            expected_url = urlsplit(fixture["request"]["url"])
            expected_path = expected_url.path + "?" + expected_url.query
            assert ("ethereum", fixture["request"]["from"], fixture["request"]["to"],
                    expected_path) in requested
            print("local TLS history owner fixture: private and shared modes, completion, cancellation, shutdown, retries, chunks, interval coverage and restart passed")
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)


if __name__ == "__main__":
    run(sys.argv[1], sys.argv[2], sys.argv[3])
