#!/usr/bin/env python3
"""The scheduled price work inside TokenService and the KV sweeper, through
build/fortis_node_token_route_test (token + price test routes, a local
HTTPS CoinGecko, one fixture clock file for routes, owner and sweeper).

  1. Manual runs (/probe/test-token-job): a fresh cached price is not
     refetched or counted; after the cache expires the recent-spot fallback
     refreshes in background mode; `price-seal` moves closed 5m rows; the
     composite `demand` job reports the flush, projection and refresh;
     sealing belongs to the composed price service maintenance timer. The process is then SIGKILLed.
  2. Hard restart with the scheduler on (FORTIS_TEST_TOKEN_JITTER=0.01, so
     the 5-minute job's first run is at 0.01 * 300 s = 3 s, as TypeScript's
     random() * intervalMs): no provider call at boot or on a read route,
     then exactly one scheduled background refresh; the 10 s sweeper tick
     seals the rows that closed; SIGTERM stops promptly.
  3. Stop during a scheduled refresh whose provider call hangs: SIGTERM
     cancels the job's spot requests and the process exits well before the
     owner's 30 s deadline.
Usage: price_jobs_service_test.py <fortis_node_token_route_test>
"""
import json
import os
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

import tls_ca

TOKEN = "fintel_dev_" + "A" * 43
MINUTE = 60000
HOUR = 60 * MINUTE


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def rss_kib(pid):
    out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    return int(out) if out else 0


class Upstream:
    def __init__(self, root):
        self.ca, cert, key, _ = tls_ca.create_certificates(root, "DNS:localhost")
        self.requests = []
        self.slow = threading.Event()
        self.lock = threading.Lock()
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                parsed = urlsplit(self.path)
                query = parse_qs(parsed.query)
                host = self.headers.get("x-fortis-test-host", "api.coingecko.com")
                ids = [i for i in query.get("ids", [""])[0].split(",") if i]
                fiat = query.get("vs_currencies", ["usd"])[0]
                with upstream.lock:
                    upstream.requests.append((time.monotonic(), host, parsed.path, fiat, ids))
                if parsed.path.endswith("/simple/price"):
                    if upstream.slow.is_set():
                        time.sleep(8)
                    body = json.dumps({i: {fiat: 2.0, f"{fiat}_24h_change": 1.0} for i in ids}).encode()
                    status = 200
                else:
                    body, status = b"{}", 404
                try:
                    self.send_response(status)
                    self.send_header("content-type", "application/json")
                    self.send_header("content-length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                except OSError:
                    pass

            def log_message(self, *_):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def spot_requests(self, since=0.0):
        with self.lock:
            return [r for r in self.requests if r[0] >= since and r[2].endswith("/simple/price")]


class Server:
    def __init__(self, binary, kv_dir, clock, upstream, extra):
        self.port = free_port()
        env = {k: v for k, v in os.environ.items()
               if not k.startswith("FORTIS_") and k not in ("COINGECKO_API_KEY", "COINGECKO_TIER")}
        env.update({
            "FORTIS_ENABLE_TEST_TOKEN_ROUTES": "1",
            "FORTIS_TEST_TOKEN_UPSTREAM_PORT": str(upstream.port),
            "FORTIS_TEST_TOKEN_UPSTREAM_CA": str(upstream.ca),
            "FORTIS_TEST_TOKEN_NOW_FILE": str(clock),
            "FORTIS_ENABLE_TEST_PRICE_ROUTE": "1",
            "FORTIS_TEST_PRICE_UPSTREAM_PORT": str(upstream.port),
            "FORTIS_TEST_PRICE_UPSTREAM_CA": str(upstream.ca),
            "FORTIS_TEST_PRICE_NOW_FILE": str(clock),
            "FORTIS_KV_DIR": str(kv_dir),
            "PORT": str(self.port),
        })
        env.update(extra)
        self.started = time.monotonic()
        self.process = subprocess.Popen([binary, str(self.port)], env=env, stdout=subprocess.DEVNULL,
                                        stderr=subprocess.PIPE)
        for _ in range(300):
            if self.process.poll() is not None:
                raise RuntimeError("server exited: " + self.process.stderr.read().decode())
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=.1):
                    return
            except OSError:
                time.sleep(.03)
        raise RuntimeError("server did not start")

    def get(self, path, auth=True, timeout=60):
        lines = [f"GET {path} HTTP/1.1", "Host: localhost", "Connection: close", "Accept: application/json"]
        if auth:
            lines.append(f"Authorization: Bearer {TOKEN}")
        with socket.create_connection(("127.0.0.1", self.port), timeout=timeout) as sock:
            sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
            data = bytearray()
            while chunk := sock.recv(65536):
                data.extend(chunk)
        head, _, body = bytes(data).partition(b"\r\n\r\n")
        return int(head.split()[1]), body

    def job(self, name):
        status, body = self.get(f"/probe/test-token-job?job={name}", auth=False)
        assert status == 200, (name, status, body)
        return json.loads(body)

    def maintenance(self):
        status, body = self.get("/probe/test-price-maintenance", auth=False)
        assert status == 200, body
        return json.loads(body)

    def stop(self, sig=signal.SIGTERM, timeout=30):
        began = time.monotonic()
        self.process.send_signal(sig)
        try:
            self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise RuntimeError("server did not stop")
        return time.monotonic() - began


def set_clock(path, value):
    path.write_text(f"{value}\n")


def main(binary):
    failures = []

    def check(label, condition, detail=""):
        print(("ok   " if condition else "FAIL ") + label + (f": {detail}" if detail else ""))
        if not condition:
            failures.append(label)

    with tempfile.TemporaryDirectory(prefix="price-jobs-service-") as temp:
        root = Path(temp)
        upstream = Upstream(root)
        kv = root / "kv"
        kv.mkdir()
        clock = root / "clock"
        t0 = int(time.time() * 1000)
        set_clock(clock, t0)

        # 1. Manual runs.
        server = Server(binary, kv, clock, upstream, {})
        status, body = server.get("/v1/prices?ids=bitcoin,ethereum")
        check("foreground spot read", status == 200 and len(upstream.spot_requests()) == 1, f"{status}")
        mark = time.monotonic()
        fresh = server.job("active-prices")["prices"]
        check("fresh cache hit is not refetched or counted",
              fresh["refreshed"] == 0 and fresh["chosen"] == 2 and not upstream.spot_requests(mark), json.dumps(fresh))
        set_clock(clock, t0 + 11 * MINUTE)
        mark = time.monotonic()
        refreshed = server.job("active-prices")["prices"]
        calls = upstream.spot_requests(mark)
        check("expired cache: recent fallback refreshes in background",
              refreshed["refreshed"] == 2 and refreshed["fromRecent"] == 2 and len(calls) == 1 and
              sorted(calls[0][4]) == ["bitcoin", "ethereum"], json.dumps(refreshed))
        set_clock(clock, t0 + 2 * HOUR)
        seal = server.job("price-seal")["seal"]
        check("price-seal moves the closed 5m rows", seal["sealed"] >= 4 and not seal["failed"], json.dumps(seal))
        mark = time.monotonic()
        composite = server.job("demand")
        check("demand job refreshes once and leaves sealing to the composed price service",
              "prices" in composite and "seal" not in composite and composite["prices"]["refreshed"] == 2 and
              len(upstream.spot_requests(mark)) == 1, json.dumps(composite))
        server.stop(signal.SIGKILL)

        # 2. Hard restart, scheduler on.
        set_clock(clock, t0 + 2 * HOUR + 20 * MINUTE)
        mark = time.monotonic()
        server = Server(binary, kv, clock, upstream, {"FORTIS_TEST_TOKEN_MANUAL_JOBS": "0",
                                                      "FORTIS_TEST_TOKEN_JITTER": "0.01"})
        boot_rss = rss_kib(server.process.pid)
        status, _ = server.get("/v1/networks/eth-mainnet/token/native/holders/history")
        time.sleep(max(0.0, 2.0 - (time.monotonic() - server.started)))
        check("no provider refresh at boot or on a read route", not upstream.spot_requests(mark),
              f"read route status {status}")
        first = None
        while time.monotonic() - server.started < 20 and first is None:
            calls = upstream.spot_requests(mark)
            first = calls[0] if calls else None
            time.sleep(0.05)
        delay = first[0] - server.started if first else None
        check("scheduled first run near 0.01 * 300 s", first is not None and 2.5 <= delay <= 8.0,
              f"{delay and round(delay, 2)} s ids={first and sorted(first[4])}")
        time.sleep(1.0)
        check("one scheduled refresh call", len(upstream.spot_requests(mark)) == 1,
              f"{len(upstream.spot_requests(mark))}")
        # Close the hour the refreshed rows sit in; the 10 s tick seals them.
        set_clock(clock, t0 + 4 * HOUR)
        ticks = {"ticks": 0, "sealed": 0}
        while time.monotonic() - server.started < 40 and ticks["sealed"] < 2:
            ticks = server.maintenance()
            time.sleep(0.5)
        check("10 s sweeper tick seals closed rows", ticks["ticks"] >= 1 and ticks["sealed"] >= 2, json.dumps(ticks))
        idle_rss = rss_kib(server.process.pid)
        took = server.stop()
        check("SIGTERM stops promptly", took < 10 and server.process.returncode == 0,
              f"{took:.2f} s rc={server.process.returncode} rss {boot_rss}->{idle_rss} KiB")

        # 3. Stop while the scheduled refresh's provider call hangs.
        set_clock(clock, t0 + 6 * HOUR)
        upstream.slow.set()
        mark = time.monotonic()
        server = Server(binary, kv, clock, upstream, {"FORTIS_TEST_TOKEN_MANUAL_JOBS": "0",
                                                      "FORTIS_TEST_TOKEN_JITTER": "0.01"})
        while time.monotonic() - server.started < 20 and not upstream.spot_requests(mark):
            time.sleep(0.05)
        check("hung scheduled refresh reached the provider", bool(upstream.spot_requests(mark)))
        time.sleep(0.3)
        took = server.stop()
        check("SIGTERM during a hung refresh exits before the provider answers", took < 6,
              f"{took:.2f} s rc={server.process.returncode}")
        upstream.slow.clear()
        upstream.server.shutdown()

    if failures:
        raise SystemExit(f"{len(failures)} failure(s)")
    print("price jobs service: all checks pass")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    main(sys.argv[1])
