#!/usr/bin/env python3
"""Verify both owners share one CoinGecko lane and preserve lease isolation."""
from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import resource
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import parse_qs, urlsplit

import tls_ca


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def run(binary: str) -> None:
    with tempfile.TemporaryDirectory(prefix="combined-provider-owners-") as temp:
        root = Path(temp)
        ca, cert, key, _ = tls_ca.create_certificates(root, "DNS:localhost")
        lock = threading.Lock()
        burst_barrier = threading.Event()
        requests: list[tuple[float, str, str]] = []
        active = 0
        max_active = 0

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self) -> None:
                nonlocal active, max_active
                parsed = urlsplit(self.path)
                query = parse_qs(parsed.query)
                if "/simple/price" in parsed.path:
                    ids = query.get("ids", [""])[0].split(",")
                    fiat = query.get("vs_currencies", ["usd"])[0]
                    kind = "spot"
                    name = ids[0]
                else:
                    ids = [parsed.path.split("/")[4]]
                    fiat = "usd"
                    kind = "history"
                    name = ids[0]
                with lock:
                    active += 1
                    max_active = max(max_active, active)
                    requests.append((time.monotonic(), kind, name))
                    if name.startswith(("spot-burst-", "hist-burst-")):
                        if active >= 2:
                            burst_barrier.set()
                if name.startswith("hist-full-"):
                    release = root / "history_saturation_release"
                elif name.startswith("spot-full-"):
                    release = root / "spot_saturation_release"
                else:
                    release = None
                if release is not None:
                    deadline = time.monotonic() + 5
                    while not release.exists() and time.monotonic() < deadline:
                        time.sleep(.01)
                if name.startswith(("spot-burst-", "hist-burst-")):
                    burst_barrier.wait(3)
                if name in ("spot-cancel", "hist-cancel-peer"):
                    (root / f"{name.replace('-', '_')}-active").touch()
                    time.sleep(.45)
                if name in ("spot-stop", "hist-survivor"):
                    (root / f"{name.replace('-', '_')}-active").touch()
                    time.sleep(.8 if name == "spot-stop" else 1.1)

                if kind == "spot" and name == "spot-429":
                    status, body = 429, b'{"error":"rate limited"}'
                elif kind == "spot":
                    status = 200
                    body = json.dumps({coin: {fiat: 1.25,
                        fiat + "_24h_change": 0.5, fiat + "_24h_vol": 10}
                        for coin in ids}, separators=(",", ":")).encode()
                else:
                    status = 200
                    start = int(query["from"][0]) * 1000
                    body = json.dumps({"prices": [[start, 10], [start + 1_000, 11]],
                        "total_volumes": []}, separators=(",", ":")).encode()
                with lock:
                    active -= 1
                if name in ("spot-cancel", "spot-stop"):
                    # Cancel and Stop abandon these requests client-side at
                    # once; the fixture waits for the abandoned handler before
                    # its next step so the server-side count stays per step.
                    (root / f"{name.replace('-', '_')}-done").touch()
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

        server = ThreadingHTTPServer(("127.0.0.1", free_port()), Handler)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        server.socket = tls.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            usage_before = resource.getrusage(resource.RUSAGE_CHILDREN)
            subprocess.run([str(Path(binary).resolve()), str(server.server_address[1]),
                            str(ca), str(root)], check=True, timeout=100)
            usage_after = resource.getrusage(resource.RUSAGE_CHILDREN)
            child_cpu = ((usage_after.ru_utime + usage_after.ru_stime) -
                         (usage_before.ru_utime + usage_before.ru_stime))
            with lock:
                burst = sorted((row for row in requests
                                if row[2].startswith(("spot-burst-", "hist-burst-"))),
                               key=lambda row: row[0])
                spot_blocked_sent = any(name == "spot-after-429" for _, kind, name in requests if kind == "spot")
                history_after_429 = any(name == "hist-after-spot-429" for _, kind, name in requests if kind == "history")
                spot_after_history_capacity = any(name == "spot-after-history-full" for _, kind, name in requests if kind == "spot")
                history_after_spot_capacity = any(name == "hist-after-spot-full" for _, kind, name in requests if kind == "history")
                observed_max = max_active
            assert len(burst) == 8, burst
            assert observed_max == 2, observed_max
            # The first four requests consume the shared burst. Every later
            # request waits for the 24/minute lane's ~2.5-second refill.
            assert burst[3][0] - burst[0][0] < 2.0, burst
            assert all(burst[i][0] - burst[i - 1][0] >= 2.2
                       for i in range(4, len(burst))), burst
            assert not spot_blocked_sent
            assert not history_after_429
            assert spot_after_history_capacity
            assert history_after_spot_capacity
            assert child_cpu < 5.0, child_cpu
            required = {"spot_cancel", "hist_cancel_peer", "spot_stop", "hist_survivor"}
            with lock:
                markers = {name.replace("-", "_") for _, _, name in requests}
            assert required <= markers, markers
            print("two owner leases shared max-concurrency=2, burst=4, refill pacing, and the 429 breaker; capacity saturation, cancellation, and Stop preserved peer progress")
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)


if __name__ == "__main__":
    run(sys.argv[1])
