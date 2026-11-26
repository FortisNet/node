#!/usr/bin/env python3
"""R32 GET /v1/prices/history: replay the frozen TypeScript fixture.

`tests/corpus/price_history_query_route.json` is written by
the earlier TypeScript implementation's tests. Each group
runs in a fresh process over a fresh KV directory seeded like the TypeScript
app (ethereum plus the non-CoinGecko rows), against a loopback CoinGecko
stand-in whose market_chart/range answers are the TypeScript mock's. Every
case compares status, headers, exact body bytes (JSON text, protobuf wire,
or length and SHA-256 for bodies over 16 KiB) and the exact upstream request
paths. Edge blocks then cover what the fixture cannot: restart over the
durable KV, client disconnects, a hung provider, concurrent reads, privacy,
oversized targets and the documented native bounds.

usage: price_history_query_route_test.py build/fortis_node_history_route_test
"""
import hashlib
import json
import math
import os
from pathlib import Path
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, quote, urlsplit

import compat_expectations
import tls_ca
from history_contract_expectations import FIXTURE_PROVIDER, history

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/corpus/price_history_query_route.json"
# GET URLs whose recorded answer is a zod refusal: their HEAD twins map too.
ZOD_URLS = set()
HOUR = 3600
DAY = 86400
# R32_FAST=1 (mutation runs): no minute alignment, quota counters unchecked,
# and the 25-second hung-provider block is skipped. R32_REPLAY_ONLY=1 also
# skips the edge blocks.
FAST = os.environ.get("R32_FAST") == "1"
REPLAY_ONLY = os.environ.get("R32_REPLAY_ONLY") == "1"


def market_chart(coin, start, end):
    """The TypeScript fixture's marketChart(coin, from, to)."""
    if coin == "empty-coin":
        return {"prices": [], "total_volumes": []}
    step = DAY if end - start > 90 * DAY else HOUR
    prices, volumes = [], []
    t = math.ceil(start / step) * step
    while t <= end:
        k = t // step
        if not (coin == "sparse-coin" and k % 2 == 1):
            prices.append([t * 1000, (400 + k % 997) / 4])
            if k % 13 != 0:
                volumes.append([t * 1000, 1000 + k % 101 + 0.5])
        t += step
    return {"prices": prices, "total_volumes": volumes}


class Upstream:
    """CoinGecko stand-in mirroring the TypeScript fixture's `answer`."""

    # Concurrent fixture cases: the TypeScript requests all plan their gaps
    # before the first fetch resolves; a short delay gives the native
    # requests the same overlap.
    SLOW = {"burst-coin": .6, "alpha-coin": .3, "beta-coin": .3, "gamma-coin": .3,
            "leave-coin": 1.0}

    def __init__(self):
        self.mode = "ok"
        self.hang_s = 0.0
        self.lock = threading.Lock()
        self.calls = []

    def take_calls(self):
        with self.lock:
            calls, self.calls = self.calls, []
        return calls

    def handler(self):
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                parsed = urlsplit(self.path)
                parts = parsed.path.split("/")
                coin = parts[-3] if len(parts) >= 3 else ""
                with upstream.lock:
                    upstream.calls.append(self.path)
                    mode, hang = upstream.mode, upstream.hang_s
                if coin == "hung-coin" and hang:
                    time.sleep(hang)
                time.sleep(Upstream.SLOW.get(coin, 0))
                if not (parsed.path.startswith("/api/v3/coins/") and
                        parsed.path.endswith("/market_chart/range")):
                    self.reply(404, {"error": "unexpected"})
                    return
                if mode == "fail" or coin == "failing-coin":
                    self.reply(500, {"error": "boom"})
                    return
                if mode == "limited":
                    self.reply(429, {"error": "slow down"}, {"Retry-After": "30"})
                    return
                query = parse_qs(parsed.query)
                self.reply(200, market_chart(coin, int(query["from"][0]), int(query["to"][0])))

            def reply(self, status, value, extra=None):
                data = json.dumps(value, separators=(",", ":")).encode()
                try:
                    self.send_response(status)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(data)))
                    for key, item in (extra or {}).items():
                        self.send_header(key, item)
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.wfile.write(data)
                except OSError:
                    pass
                self.close_connection = True

            def log_message(self, *_args):
                pass

        return Handler


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def exchange(port, target, *, method="GET", headers=None, timeout=40.0, leave=False):
    sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    lines = [f"{method} {target} HTTP/1.1", "Host: localhost", "Connection: close"]
    for key, value in (headers or {}).items():
        lines.append(f"{key}: {value}")
    sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("latin1"))
    if leave:
        sock.close()
        return None
    data = bytearray()
    while True:
        try:
            chunk = sock.recv(65536)
        except ConnectionResetError:
            break
        if not chunk:
            break
        data.extend(chunk)
    sock.close()
    head, _, body = bytes(data).partition(b"\r\n\r\n")
    if not head:
        return 0, {}, b""
    first, *header_lines = head.decode("latin1").split("\r\n")
    reply_headers = {}
    for line in header_lines:
        if ":" in line:
            key, value = line.split(":", 1)
            reply_headers[key.strip().lower()] = value.strip()
    return int(first.split()[1]), reply_headers, body


class Server:
    def __init__(self, binary, root, upstream_port, ca, name):
        self.binary = binary
        self.kv_dir = root / f"kv-{name}"
        self.kv_dir.mkdir()
        self.port = free_port()
        self.env = dict(os.environ)
        for key in ("FORTIS_NATIVE_PRICE_ROUTE_CANARY", "FORTIS_TEST_HISTORY_BATCH_SEED",
                    "FORTIS_TEST_HISTORY_DECADE_SEED", "FORTIS_TEST_OHLC_SEED",
                    "FORTIS_TEST_HISTORY_FAIL_SOURCE", "FORTIS_TEST_HISTORY_OWNER_START_FAIL"):
            self.env.pop(key, None)
        self.env.update(FIXTURE_PROVIDER)  # Query semantics use a credentialed fixture lane.
        self.env.update({
            "FORTIS_ENABLE_TEST_PRICE_HISTORY_ROUTE": "1",
            "FORTIS_TEST_HISTORY_UPSTREAM_PORT": str(upstream_port),
            "FORTIS_TEST_HISTORY_UPSTREAM_CA": str(ca),
            "FORTIS_TEST_HISTORY_OTHER_IDS_SEED": "1",
            "FORTIS_KV_DIR": str(self.kv_dir),
        })
        self.process = None
        self.stderr = b""

    def start(self):
        self.process = subprocess.Popen([self.binary, str(self.port)], env=self.env,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        for _ in range(200):
            if self.process.poll() is not None:
                raise RuntimeError("server exited: " + self.process.stderr.read().decode())
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=.1):
                    return
            except OSError:
                time.sleep(.03)
        raise RuntimeError("server did not start")

    def stop(self, hard=False):
        if not self.process:
            return
        if hard:
            self.process.send_signal(signal.SIGKILL)
        else:
            self.process.terminate()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
        self.stderr += self.process.stderr.read()
        self.process = None

    def request(self, target, **kwargs):
        return exchange(self.port, target, **kwargs)


def minute_room(seconds):
    """Wait until a wall-clock device-quota minute has `seconds` left."""
    if FAST:
        return
    left = 60 - (time.time() % 60)
    if left < seconds:
        time.sleep(left + .2)


def shaped(expected_headers, reply, method, same_minute, quota_listed, zod_head=False):
    """Problems between one native reply and one recorded TypeScript reply."""
    # compat-decisions.md#zod-issues: a recorded zod refusal is
    # {"error":"BAD_REQUEST"} here.
    status_, headers_, body_ = compat_expectations.zod_expected(
        expected_headers["status"], expected_headers["headers"], expected_headers.get("body", ""),
        method == "HEAD", zod_head)
    if headers_ is not expected_headers["headers"]:
        expected_headers = dict(expected_headers, headers=headers_, body=body_)
    status, headers, body = reply
    problems = []
    if status != expected_headers["status"]:
        problems.append(f"status {status} != {expected_headers['status']}")
    for name, value in expected_headers["headers"].items():
        if name == "x-rate-limit-remaining" and (quota_listed or not same_minute or FAST):
            # TypeScript's quota minute follows its frozen Date, the native
            # one the wall clock; concurrent counters are compared as a list.
            if name not in headers:
                problems.append("x-rate-limit-remaining missing")
            continue
        if headers.get(name) != value:
            problems.append(f"header {name}: {headers.get(name)!r} != {value!r}")
    for name in ("vary", "retry-after", "x-rate-limit-cost", "x-rate-limit-remaining"):
        if name in headers and name not in expected_headers["headers"]:
            problems.append(f"unexpected header {name}: {headers[name]!r}")
    if method == "HEAD":
        if body:
            problems.append("HEAD body not empty")
    elif "bodySha256" in expected_headers:
        digest = hashlib.sha256(body).hexdigest()
        if (len(body), digest) != (expected_headers["bodyLength"], expected_headers["bodySha256"]):
            problems.append(f"large body {len(body)} {digest} != "
                            f"{expected_headers['bodyLength']} {expected_headers['bodySha256']}")
    elif "bodyHex" in expected_headers:
        if body.hex() != expected_headers["bodyHex"]:
            problems.append(f"protobuf {body.hex()[:400]} != {expected_headers['bodyHex'][:400]}")
    elif body.decode("utf-8", "replace") != expected_headers["body"]:
        problems.append(f"body {body.decode('utf-8', 'replace')[:500]!r} != "
                        f"{expected_headers['body'][:500]!r}")
    return problems


def run_together(server, urls, method, headers):
    replies = [None] * len(urls)
    barrier = threading.Barrier(len(urls))

    def one(index):
        barrier.wait()
        replies[index] = server.request(urls[index], method=method, headers=headers)

    threads = [threading.Thread(target=one, args=(i,)) for i in range(len(urls))]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    return replies


def replay_group(group, server, upstream, fixture_ms, failures):
    first_minute = (fixture_ms + group["cases"][0]["at"]) // 60000
    replayed = 0
    for case in group["cases"]:
        label = f"{group['name']}/{case['name']}"
        same_minute = (fixture_ms + case["at"]) // 60000 == first_minute
        upstream.mode = case["upstream"]
        upstream.take_calls()
        urls = case["urls"]
        if case.get("parallel"):
            urls = urls * case["parallel"]
        if len(urls) == 1:
            reply = server.request(urls[0], method=case["method"], headers=case["headers"])
            problems = shaped(case["response"], reply, case["method"], same_minute, False,
                              urls[0] in ZOD_URLS)
        else:
            replies = run_together(server, urls, case["method"], case["headers"])
            problems = []
            remaining = sorted(reply[1].get("x-rate-limit-remaining") for reply in replies)
            if same_minute and not FAST and remaining != case["remaining"]:
                problems.append(f"remaining {remaining} != {case['remaining']}")
            expected = case["responses"]
            if case.get("parallel"):
                # A multiset: every recorded reply matched by a distinct native one.
                unmatched = list(replies)
                for want in expected:
                    for index, reply in enumerate(unmatched):
                        if not shaped(want, reply, case["method"], same_minute, True):
                            del unmatched[index]
                            break
                    else:
                        problems.append("no native reply matches " +
                                        json.dumps(want)[:300] + "; native: " +
                                        "; ".join(r[2].decode("utf-8", "replace")[:200]
                                                  for r in unmatched))
            else:
                for want, reply in zip(expected, replies):
                    problems += shaped(want, reply, case["method"], same_minute, True)
        # Every native upstream request is issued before its reply.
        calls = sorted(upstream.take_calls())
        if calls != case["upstreamCalls"]:
            problems.append(f"upstream {calls} != {case['upstreamCalls']}")
        if problems:
            failures.append((label, problems))
        replayed += 1
    return replayed


def body_json(reply):
    return json.loads(reply[2]) if reply[0] == 200 else None


def edge_blocks(binary, root, upstream, upstream_port, ca, fixture, cached_case):
    """Native-only checks the frozen fixture cannot express."""
    token = fixture["token"]
    first = fixture["first"]
    auth = {"Authorization": "Bearer " + token, "Accept": "application/json"}
    checks = []

    def check(condition, message):
        checks.append(message)
        if not condition:
            raise AssertionError(message)

    def url(coin, start, end, interval="1h"):
        return f"/v1/prices/history?id={coin}&from={start}&to={end}&interval={interval}"

    server = Server(binary, root, upstream_port, ca, "edges")
    server.start()
    try:
        upstream.mode = "ok"
        upstream.take_calls()
        # Restart: a cold fetch, an empty (covered) answer and a failed gap,
        # then SIGKILL and a restart over the same KV directory.
        bitcoin = url("bitcoin", first, first + 6 * HOUR)
        cold = server.request(bitcoin, headers=auth)
        check(cold[0] == 200 and body_json(cold)["cache"]["hit"] is False, "cold fetch")
        check(server.request(url("empty-coin", first, first + 3 * HOUR), headers=auth)[0] == 200,
              "empty answer")
        server.request(url("failing-coin", first, first + 3 * HOUR), headers=auth)
        upstream.take_calls()
        server.stop(hard=True)
        server.start()
        again = server.request(bitcoin, headers=auth)
        check(again[2].decode() == cached_case["response"]["body"],
              "after SIGKILL the window reads back byte for byte as cached")
        check(server.request(url("empty-coin", first, first + 3 * HOUR), headers=auth)[0] == 200,
              "covered empty answer after restart")
        check(upstream.take_calls() == [], "candles and coverage survive SIGKILL")
        failed = server.request(url("failing-coin", first, first + 3 * HOUR), headers=auth)
        check(failed[0] == 200 and len(upstream.take_calls()) == 3,
              "a failed gap is retried after restart (three attempts)")

        # Disconnects. A caller that leaves while another waits on the same
        # flight does not cancel it: the stayer gets the fetched window from
        # one upstream request (TypeScript's lock loser: cache.hit true).
        leave = url("leave-coin", first, first + 6 * HOUR)
        stayer = []
        leader = socket.create_connection(("127.0.0.1", server.port), timeout=10)
        leader.sendall((f"GET {leave} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
                        f"Authorization: Bearer {token}\r\n\r\n").encode())
        time.sleep(.3)  # the leader's flight is at the (1 s) provider
        thread = threading.Thread(target=lambda: stayer.append(server.request(leave, headers=auth)))
        thread.start()
        time.sleep(.3)  # the stayer has joined it
        leader.close()
        thread.join()
        body = body_json(stayer[0])
        check(stayer[0][0] == 200 and len(body["points"]) == 7, "stayer gets the shared fetch")
        check(len(upstream.take_calls()) == 1, "one upstream request for leaver and stayer")
        # A sole caller that leaves: the native owner cancels its flight, so
        # nothing is stored and the next caller fetches again (TypeScript
        # finishes the fetch). Either way the next caller is answered fully.
        alone = url("leave-coin", first + DAY, first + DAY + 6 * HOUR)
        server.request(alone, headers=auth, leave=True)
        time.sleep(1.5)
        after = server.request(alone, headers=auth)
        check(after[0] == 200 and len(body_json(after)["points"]) == 7,
              "the next caller after a sole leaver is answered fully")
        check(1 <= len(upstream.take_calls()) <= 2, "at most one refetch after a sole leaver")

        # Concurrent reads: twelve cached reads (the router admits four
        # workers plus eight pending connections) answer identical bodies.
        cached = url("ethereum", first, first + 6 * HOUR)
        replies = run_together(server, [cached] * 12, "GET", auth)
        check(all(r[0] == 200 for r in replies) and len({r[2] for r in replies}) == 1,
              "12 concurrent cached reads are identical: " +
              repr(sorted({(r[0], r[2][:80]) for r in replies})))
        # Twelve distinct cold coins at once exceed the owner's eight
        # requests: each is answered 200, fetched or (owner full) the empty
        # stored series with cache.hit true, never an error.
        time.sleep(11)  # lane tokens back to the burst
        urls = [url(f"many-{i:02d}", first, first + 4 * HOUR) for i in range(12)]
        replies = run_together(server, urls, "GET", auth)
        shapes = [(r[0], len(body_json(r)["points"]) if r[0] == 200 else None,
                   body_json(r)["cache"]["hit"] if r[0] == 200 else None) for r in replies]
        check(all(s in ((200, 5, False), (200, 0, True)) for s in shapes),
              f"12 concurrent cold coins answer fetched or empty: {shapes}")
        check(len(upstream.take_calls()) <= 12, "no more than one request per cold coin")

        # The documented native bound: a NUL inside a contract network.
        nul = url(quote("a\x00b:erc20:0x" + "22" * 20, safe=""), first, first + HOUR)
        reply = server.request(nul, headers=auth)
        check((reply[0], reply[2]) ==
              (400, b'{"error":"BAD_REQUEST","detail":"unrecognized id"}') and
              "vary" not in reply[1] and reply[1].get("x-rate-limit-cost") == "1",
              "NUL in a network is an unrecognized id natively")

        # Router bound: a target over 12,500 bytes never reaches the handler.
        long_target = url("x" * 13000, first, first + HOUR)
        check(server.request(long_target, headers=auth)[0] in (400, 414, 431),
              "oversized target refused")
        check(server.request(cached, headers=auth)[0] == 200, "healthy after the oversized target")

        # Hung provider: the gap plan stops at the 25 s owner deadline and the
        # stored (here empty) series answers 200, uncovered.
        if not FAST:
            upstream.hang_s = 40
            started = time.monotonic()
            hung = server.request(url("hung-coin", first, first + 2 * HOUR), headers=auth,
                                  timeout=60)
            elapsed = time.monotonic() - started
            upstream.hang_s = 0
            check(hung[0] == 200 and body_json(hung)["points"] == [] and
                  body_json(hung)["cache"] == {"hit": True, "timestamp": None},
                  f"hung provider answers the stored series ({hung[0]})")
            check(24 <= elapsed <= 29, f"hung provider answered at {elapsed:.1f} s")
            check(server.request(cached, headers=auth)[0] == 200, "healthy after the hung provider")
            upstream.take_calls()
    finally:
        server.stop()
    # Privacy: no reply or log line carries the bearer or the device id.
    check(token.encode() not in server.stderr and b"01ARZ3NDEKTSV4RRFFQ69G5FAW" not in server.stderr,
          "the log never carries the bearer or device id")
    return checks


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    fixture = history(json.loads(FIXTURE.read_text()))
    ZOD_URLS.update(case["urls"][0] for group in fixture["groups"] for case in group["cases"]
                    if case["method"] == "GET" and len(case["urls"]) == 1 and "response" in case and
                    compat_expectations.is_zod_refusal(case["response"].get("body", "")))
    fixture_ms = int(time.mktime(time.strptime(fixture["source"]["fixtureTime"][:19],
                                               "%Y-%m-%dT%H:%M:%S")) - time.timezone) * 1000
    failures = []
    replayed = 0
    with tempfile.TemporaryDirectory(prefix="history-query-") as temporary:
        root = Path(temporary)
        ca, cert, key, _ = tls_ca.create_certificates(root, "DNS:localhost")
        upstream = Upstream()
        http = ThreadingHTTPServer(("127.0.0.1", free_port()), upstream.handler())
        http.daemon_threads = True
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certfile=str(cert), keyfile=str(key))
        http.socket = context.wrap_socket(http.socket, server_side=True)
        threading.Thread(target=http.serve_forever, daemon=True).start()
        port = http.server_address[1]
        try:
            for group in fixture["groups"]:
                server = Server(binary, root, port, ca, group["name"])
                minute_room(40 if group["name"] in ("boundary", "quota") else 25)
                server.start()
                try:
                    replayed += replay_group(group, server, upstream, fixture_ms, failures)
                finally:
                    server.stop()
                token = fixture["token"].encode()
                if token in server.stderr:
                    failures.append((group["name"], ["bearer token in the log"]))
            for label, problems in failures:
                print(f"MISMATCH {label}:")
                for problem in problems:
                    print("   ", problem)
            if failures:
                print(f"{len(failures)} mismatching case(s) of {replayed}")
                return 1
            if REPLAY_ONLY:
                print(f"price history query route: replayed {replayed} cases, 0 mismatches")
                return 0
            cache = next(g for g in fixture["groups"] if g["name"] == "cache")
            cached_case = next(c for c in cache["cases"] if c["name"] == "reread is cached")
            checks = edge_blocks(binary, root, upstream, port, ca, fixture, cached_case)
        finally:
            http.shutdown()
            http.server_close()
    print(f"price history query route: replayed {replayed} cases, 0 mismatches; "
          f"{len(checks)} edge checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
