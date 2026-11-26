#!/usr/bin/env python3
"""RecurringJob 8 (coin price popularity) inside the composed services, through
build/fortis_node_token_route_test (token + price test routes, a local
HTTPS CoinGecko serving /simple/price and /coins/markets, one fixture clock).

  1. Foreground recording: GET /v1/prices from two devices records the
     CoinGecko ids exactly as the frozen TypeScript `recording` scenario
     (states compared bit for bit); the background active-price job records
     nothing.
  2. The manual `coin-prices` job refreshes the recorded coins in one
     foreground /coins/markets call and primes px: so a following GET
     /v1/prices and the active-price job make no /simple/price call (the
     dedupe between the two jobs); the dirty counters are saved.
  3. Hard restart (SIGKILL) with the scheduler on (FORTIS_TEST_TOKEN_JITTER=
     0.01): Start restores the ranking with no provider call at boot; the
     first scheduled coin run is near 0.01 * 60 s; the daily prune leaves a
     live ranking alone.
  4. SIGTERM while the scheduled /coins/markets call hangs exits promptly.
  5. Combined budget: the scheduler (coin prices, active prices, market
     warmers) plus a foreground /v1/prices stream of cold ids for 20 s; the
     upstream count of CoinGecko requests in every window stays within the
     shared lane's burst + rate (4 + 24/min).
Usage: coin_popularity_service_test.py <fortis_node_token_route_test>
"""
import json
import math
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
from urllib.parse import parse_qs, quote, urlsplit

import tls_ca

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "tests/corpus/coin_popularity.json"
TOKEN_A = "fintel_dev_" + "A" * 43
TOKEN_B = "fintel_dev_" + "B" * 43
NOW = 1767323045000
SECOND = 1000
MINUTE = 60000


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
        self.hang_markets = threading.Event()
        self.lock = threading.Lock()
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                parsed = urlsplit(self.path)
                query = parse_qs(parsed.query)
                host = self.headers.get("x-fortis-test-host", "api.coingecko.com")
                ids = [i for i in query.get("ids", [""])[0].split(",") if i]
                with upstream.lock:
                    upstream.requests.append((time.monotonic(), host, parsed.path, ids))
                if parsed.path.endswith("/simple/price"):
                    fiat = query.get("vs_currencies", ["usd"])[0]
                    body = json.dumps({i: {fiat: 2.0, f"{fiat}_24h_change": 1.0} for i in ids
                                       if not i.startswith("cold-")}).encode()
                    status = 200
                elif parsed.path.endswith("/coins/markets"):
                    if upstream.hang_markets.is_set():
                        time.sleep(8)
                    rows = [{"id": i, "symbol": i[:3], "name": i, "current_price": 3.5 + n,
                             "price_change_percentage_24h": 1.25, "total_volume": 1000.5,
                             "sparkline_in_7d": {"price": [1, 2, 3]}} for n, i in enumerate(ids)]
                    body, status = json.dumps(rows).encode(), 200
                elif parsed.path == "/hold-lane":
                    time.sleep(3)
                    body, status = b"{}", 200
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

    def matching(self, suffix, since=0.0):
        with self.lock:
            return [r for r in self.requests if r[0] >= since and r[2].endswith(suffix)]

    def coingecko(self, since=0.0):
        with self.lock:
            return [r for r in self.requests if r[0] >= since and r[1] in ("api.coingecko.com", "pro-api.coingecko.com")]


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

    def get(self, path, token=TOKEN_A, timeout=60):
        lines = [f"GET {path} HTTP/1.1", "Host: localhost", "Connection: close", "Accept: application/json"]
        if token:
            lines.append(f"Authorization: Bearer {token}")
        with socket.create_connection(("127.0.0.1", self.port), timeout=timeout) as sock:
            sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
            data = bytearray()
            while chunk := sock.recv(65536):
                data.extend(chunk)
        head, _, body = bytes(data).partition(b"\r\n\r\n")
        return int(head.split()[1]), body

    def job(self, name):
        status, body = self.get(f"/probe/test-token-job?job={name}", token=None)
        assert status == 200, (name, status, body)
        return json.loads(body)

    def popularity(self):
        status, body = self.get("/probe/test-coin-popularity", token=None)
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


def prices_path(ids, fiat):
    return "/v1/prices?ids=" + ",".join(quote(i, safe="") for i in ids) + f"&fiat={fiat}"


def main(binary):
    fixture = json.loads(FIXTURE.read_text())
    recording = fixture["recording"]
    failures = []

    def check(label, condition, detail=""):
        print(("ok   " if condition else "FAIL ") + label + (f": {detail}" if detail else ""))
        if not condition:
            failures.append(label)

    with tempfile.TemporaryDirectory(prefix="coin-popularity-service-") as temp:
        root = Path(temp)
        upstream = Upstream(root)
        kv = root / "kv"
        kv.mkdir()
        clock = root / "clock"
        set_clock(clock, NOW)

        # 1. Foreground recording through GET /v1/prices.
        server = Server(binary, kv, clock, upstream, {})
        boot = server.popularity()
        check("empty tracker at first boot", boot["coins"] == [] and not boot["stats"]["seenAllocated"],
              json.dumps(boot["stats"]))
        tokens = {"device-a": TOKEN_A, "device-b": TOKEN_B}
        # The TypeScript scenario's device calls (steps 0, 3 and 4, 15 s apart).
        for step, call in enumerate(recording["prices"]):
            if "deviceId" not in call["options"]:
                continue  # background / device-less calls are not route reads
            set_clock(clock, NOW + 15 * SECOND * step)
            status, body = server.get(prices_path(call["ids"], call["fiat"]), tokens[call["options"]["deviceId"]])
            check(f"GET /v1/prices step {step}", status == 200, f"{status} {body[:120]!r}")
        states = {c["id"]: c for c in server.popularity()["coins"]}
        expected = {c["id"]: c for c in recording["afterPrices"]}
        for coin in ("bitcoin", "ethereum"):
            check(f"recorded {coin} matches TypeScript", states.get(coin) == expected[coin],
                  f"native {states.get(coin)} ts {expected[coin]}")
        check("cardano recorded once from device-a", states.get("cardano", {}).get("users") == 1 and
              states["cardano"]["hits"] == 1, json.dumps(states.get("cardano")))
        check("non-CoinGecko and invalid ids are not recorded",
              set(states) == {"bitcoin", "ethereum", "cardano"}, sorted(states))
        before = server.popularity()["coins"]
        set_clock(clock, NOW + 2 * MINUTE)
        server.job("active-prices")
        check("background active-price refresh records nothing", server.popularity()["coins"] == before)

        # 2. The coin price job, its dedupe with the spot paths and its save.
        set_clock(clock, NOW + 3 * MINUTE)
        mark = time.monotonic()
        coins = server.job("coin-prices")["coins"]
        markets = upstream.matching("/coins/markets", mark)
        check("coin-prices refreshes the ranking in one /coins/markets call",
              coins["refreshed"] == 3 and len(markets) == 1 and sorted(markets[0][3]) == ["bitcoin", "cardano", "ethereum"],
              json.dumps(coins))
        check("dirty counters saved in one frame", coins["saved"] == 3 and coins["saveFrames"] == 1, json.dumps(coins))
        mark = time.monotonic()
        status, body = server.get(prices_path(["bitcoin", "cardano"], "usd"))
        served = json.loads(body).get("prices", {})
        check("GET /v1/prices answers from the primed px: rows",
              status == 200 and not upstream.matching("/simple/price", mark) and
              served.get("bitcoin", {}).get("price") == 3.5, f"{status} {body[:160]!r}")
        active = server.job("active-prices")
        check("active-price job makes no /simple/price call for primed coins",
              not upstream.matching("/simple/price", mark), json.dumps(active.get("prices")))
        again = server.job("coin-prices")["coins"]
        check("a second run inside the bucket interval refreshes nothing", again["due"] == 0 and again["calls"] == 0,
              json.dumps(again))
        server.stop(signal.SIGKILL)

        # 3. Hard restart with the scheduler on.
        set_clock(clock, NOW + 10 * MINUTE)
        mark = time.monotonic()
        server = Server(binary, kv, clock, upstream, {"FORTIS_TEST_TOKEN_MANUAL_JOBS": "0",
                                                      "FORTIS_TEST_TOKEN_JITTER": "0.01"})
        restored = server.popularity()
        check("Start restores the persisted ranking", sorted(c["id"] for c in restored["coins"]) ==
              ["bitcoin", "cardano", "ethereum"] and all(c["refreshedAt"] is None for c in restored["coins"]),
              json.dumps(restored["coins"])[:300])
        boot_calls = [r for r in upstream.coingecko(mark) if r[0] - server.started < 0.4]
        check("no provider call at boot", not boot_calls, str(boot_calls))
        first = None
        while time.monotonic() - server.started < 15 and first is None:
            calls = upstream.matching("/coins/markets", mark)
            first = calls[0] if calls else None
            time.sleep(0.02)
        delay = first[0] - server.started if first else None
        check("first scheduled coin run near 0.01 * 60 s", first is not None and 0.4 <= delay <= 4.0,
              f"{delay and round(delay, 2)} s ids={first and first[3]}")
        time.sleep(0.5)
        check("one scheduled coin call", len(upstream.matching("/coins/markets", mark)) == 1)
        prune = server.job("coin-prune")["prune"]
        check("daily prune keeps a live ranking", prune["before"] == 3 and prune["after"] == 3, json.dumps(prune))
        rss = rss_kib(server.process.pid)
        took = server.stop()
        check("SIGTERM stops promptly", took < 10 and server.process.returncode == 0,
              f"{took:.2f} s rc={server.process.returncode} rss {rss} KiB")

        # 4. SIGTERM while the scheduled /coins/markets call hangs.
        set_clock(clock, NOW + 20 * MINUTE)
        upstream.hang_markets.set()
        mark = time.monotonic()
        server = Server(binary, kv, clock, upstream, {"FORTIS_TEST_TOKEN_MANUAL_JOBS": "0",
                                                      "FORTIS_TEST_TOKEN_JITTER": "0.01"})
        while time.monotonic() - server.started < 15 and not upstream.matching("/coins/markets", mark):
            time.sleep(0.05)
        check("hung scheduled coin refresh reached the provider", bool(upstream.matching("/coins/markets", mark)))
        time.sleep(0.3)
        took = server.stop()
        check("SIGTERM during a hung coin refresh exits before the provider answers", took < 6,
              f"{took:.2f} s rc={server.process.returncode}")
        upstream.hang_markets.clear()

        # 5. Combined CoinGecko budget under mixed demand.
        kv2 = root / "kv-budget"
        kv2.mkdir()
        set_clock(clock, NOW)
        server = Server(binary, kv2, clock, upstream, {"FORTIS_TEST_TOKEN_MANUAL_JOBS": "0",
                                                       "FORTIS_TEST_TOKEN_JITTER": "0.01"})
        # 300 popular coins (three 100-id reads), then a stream of cold reads.
        for n in range(3):
            server.get(prices_path([f"pop-{n}-{i:02d}" for i in range(100)], "usd"))
        mark = time.monotonic()
        latencies, statuses = [], {}
        stop_at = mark + 20
        n = 0
        while time.monotonic() < stop_at:
            began = time.monotonic()
            status, _ = server.get(prices_path([f"cold-{n}"], "usd"), timeout=60)
            latencies.append(time.monotonic() - began)
            statuses[status] = statuses.get(status, 0) + 1
            n += 1
            time.sleep(0.1)
        calls = upstream.coingecko(mark)
        times = sorted(r[0] for r in calls)
        worst = 0.0
        for i, start in enumerate(times):
            for j in range(i, len(times)):
                window = max(times[j] - start, 0.001)
                allowed = 4 + math.ceil(0.4 * window) + 1  # burst + 24/min, one in flight at the edge
                worst = max(worst, (j - i + 1) / allowed)
        coin_calls = [r for r in calls if r[2].endswith("/coins/markets")]
        spot_calls = [r for r in calls if r[2].endswith("/simple/price")]
        check("combined CoinGecko requests stay within the shared lane budget", worst <= 1.0,
              f"{len(calls)} calls in 20 s (coins/markets {len(coin_calls)}, simple/price {len(spot_calls)}); "
              f"worst window ratio {worst:.2f}")
        check("the coin run ran inside the window", len(coin_calls) >= 1, f"{len(coin_calls)}")
        latencies.sort()
        p50 = latencies[len(latencies) // 2]
        p99 = latencies[min(len(latencies) - 1, int(len(latencies) * 0.99))]
        print(f"     foreground cold reads: {len(latencies)} statuses {statuses} p50 {p50 * 1000:.0f} ms "
              f"p99 {p99 * 1000:.0f} ms")
        server.stop()
        upstream.server.shutdown()

    if failures:
        raise SystemExit(f"{len(failures)} failure(s)")
    print("coin popularity service: all checks pass")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    main(sys.argv[1])
