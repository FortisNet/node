#!/usr/bin/env python3
"""R33 GET /v1/prices/ohlc: replay the frozen TypeScript fixture.

`tests/corpus/price_ohlc_query_route.json` is written by
the earlier TypeScript implementation's tests. Each group runs
in a fresh process over a fresh KV directory seeded like the TypeScript app
(the history fixture rows plus the 90-day hourly / 365-day daily maximum
sets), with the OHLC clock driven from a file to the fixture's instant for
every case and the freshness jitter pinned to 0.5. A loopback CoinGecko
stand-in serves, per provider path, exactly the answers the TypeScript mock
gave, in order. Every case compares status, headers, exact body bytes (JSON
text, protobuf wire, or length and SHA-256 for bodies over 16 KiB) and the
exact provider request paths. Edge blocks then cover what the fixture cannot:
restart over the durable KV, client disconnects, a hung provider, oversized
or malformed provider answers, concurrent reads, privacy and the native
bounds.

usage: price_ohlc_query_route_test.py build/fortis_node_history_route_test
"""
import hashlib
import json
import os
from pathlib import Path
import socket
import ssl
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import price_history_query_route_test as r32
from price_history_query_route_test import exchange, free_port, run_together
import tls_ca
from history_contract_expectations import ohlc

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/corpus/price_ohlc_query_route.json"
# R33_FAST=1 (mutation runs): no minute alignment, quota counters unchecked,
# and the 25-second hung-provider block is skipped. R33_REPLAY_ONLY=1 also
# skips the edge blocks.
FAST = os.environ.get("R33_FAST") == "1"
REPLAY_ONLY = os.environ.get("R33_REPLAY_ONLY") == "1"
r32.FAST = FAST


class Upstream:
    """CoinGecko stand-in replaying the TypeScript mock's recorded answers."""

    # Concurrent fixture cases: the TypeScript requests all join one
    # single-flight before the first fetch resolves; a short delay gives the
    # native requests the same overlap.
    SLOW = {"burst-coin": .6, "alpha-coin": .3, "beta-coin": .3, "gamma-coin": .3,
            "leave-coin": 1.0}

    def __init__(self):
        self.lock = threading.Lock()
        self.calls = []
        self.answers = {}
        self.served = {}
        self.extra = {}   # path → answer for native-only edge blocks
        self.hang_s = 0.0

    def load(self, answers):
        with self.lock:
            self.answers = answers
            self.served = {}
            self.calls = []

    def take_calls(self):
        with self.lock:
            calls, self.calls = self.calls, []
        return calls

    def count(self):
        with self.lock:
            return len(self.calls)

    def handler(self):
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                path = self.path
                parts = path.split("?")[0].split("/")
                coin = parts[-2] if len(parts) >= 2 else ""
                with upstream.lock:
                    upstream.calls.append(path)
                    nth = upstream.served.get(path, 0)
                    upstream.served[path] = nth + 1
                    recorded = upstream.answers.get(path) or []
                    answer = (upstream.extra.get(coin) or
                              (recorded[min(nth, len(recorded) - 1)] if recorded else
                               {"status": 404, "body": '{"error":"unexpected"}'}))
                    hang = upstream.hang_s if coin == "hung-coin" else 0
                if hang:
                    time.sleep(hang)
                time.sleep(Upstream.SLOW.get(coin, 0))
                data = answer["body"].encode() if isinstance(answer["body"], str) else answer["body"]
                try:
                    self.send_response(answer["status"])
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(data)))
                    for key, value in (answer.get("headers") or {}).items():
                        self.send_header(key, value)
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.wfile.write(data)
                except OSError:
                    pass
                self.close_connection = True

            def log_message(self, *_args):
                pass

        return Handler


class Server(r32.Server):
    def __init__(self, binary, root, upstream_port, ca, name, now_ms):
        super().__init__(binary, root, upstream_port, ca, name)
        self.clock = root / f"clock-{name}"
        self.set_clock(now_ms)
        self.env.update({
            "FORTIS_TEST_OHLC_QUERY_SEED": "1",
            "FORTIS_TEST_OHLC_NOW_FILE": str(self.clock),
            "FORTIS_TEST_OHLC_RAND": "0.5",
        })

    def set_clock(self, ms):
        temporary = self.clock.with_suffix(".tmp")
        temporary.write_text(str(ms))
        os.replace(temporary, self.clock)


def settle(upstream, expected, timeout=8.0):
    """Wait for background provider work the reply did not wait for."""
    deadline = time.monotonic() + timeout
    while upstream.count() < expected and time.monotonic() < deadline:
        time.sleep(.02)
    time.sleep(.3)  # the owner stores the refreshed entry


def replay_group(group, server, upstream, now_ms, failures):
    first_minute = (now_ms + group["cases"][0]["at"]) // 60000
    upstream.load(group["upstreamAnswers"])
    replayed = 0
    left_first_minute = False
    for case in group["cases"]:
        label = f"{group['name']}/{case['name']}"
        # TypeScript's quota minute follows its fake Date, the native one the
        # wall clock: counters agree only until the fixture leaves (even
        # briefly) the group's first minute.
        left_first_minute |= (now_ms + case["at"]) // 60000 != first_minute
        same_minute = not left_first_minute
        server.set_clock(now_ms + case["at"])
        upstream.take_calls()
        urls = case["urls"]
        if case.get("parallel"):
            urls = urls * case["parallel"]
        if len(urls) == 1:
            reply = server.request(urls[0], method=case["method"], headers=case["headers"])
            problems = r32.shaped(case["response"], reply, case["method"], same_minute, False)
        else:
            replies = run_together(server, urls, case["method"], case["headers"])
            problems = []
            remaining = sorted(reply[1].get("x-rate-limit-remaining") for reply in replies)
            if same_minute and not FAST and remaining != case["remaining"]:
                problems.append(f"remaining {remaining} != {case['remaining']}")
            expected = case["responses"]
            if case.get("parallel"):
                unmatched = list(replies)
                for want in expected:
                    for index, reply in enumerate(unmatched):
                        if not r32.shaped(want, reply, case["method"], same_minute, True):
                            del unmatched[index]
                            break
                    else:
                        problems.append("no native reply matches " + json.dumps(want)[:300] +
                                        "; native: " + "; ".join(
                                            r[2].decode("utf-8", "replace")[:200] for r in unmatched))
            else:
                for want, reply in zip(expected, replies):
                    problems += r32.shaped(want, reply, case["method"], same_minute, True)
        if case.get("settle"):
            settle(upstream, len(case["upstreamCalls"]))
        calls = sorted(upstream.take_calls())
        if calls != case["upstreamCalls"]:
            problems.append(f"upstream {calls} != {case['upstreamCalls']}")
        if problems:
            failures.append((label, problems))
        replayed += 1
    return replayed


def fresh_minute():
    """Start a new device-quota minute (240 units = 48 OHLC requests)."""
    time.sleep(60.2 - time.time() % 60)


def body_json(reply):
    return json.loads(reply[2]) if reply[0] == 200 else None


def edge_blocks(binary, root, upstream, upstream_port, ca, fixture):
    """Native-only checks the frozen fixture cannot express."""
    token = fixture["token"]
    now = fixture["now"]
    auth = {"Authorization": "Bearer " + token, "Accept": "application/json"}
    checks = []

    def check(condition, message):
        checks.append(message)
        if not condition:
            raise AssertionError(message)

    def url(coin, days=1):
        return f"/v1/prices/ohlc?id={coin}&days={days}"

    def rows(count, first=1767225600000):
        return json.dumps([[first + i * 1800000, 1 + i, 2 + i, 0.5 + i, 1.5 + i]
                           for i in range(count)], separators=(",", ":"))

    upstream.load({})
    upstream.extra = {
        "edge-coin": {"status": 200, "body": rows(3)},
        "leave-coin": {"status": 200, "body": rows(2)},
        "limit-coin": {"status": 200, "body": rows(400)},
        "over-coin": {"status": 200, "body": rows(401)},
        "short-row-coin": {"status": 200, "body": "[[1767225600000,1,2,3]]"},
        "string-row-coin": {"status": 200, "body": '[[1767225600000,"1",2,3,4]]'},
        "fraction-ts-coin": {"status": 200, "body": "[[1767225600000.5,1,2,3,4]]"},
        "huge-coin": {"status": 200, "body": "[" + ",".join(["[1,1,1,1,1]"] * 50000) + "]"},
        "hung-coin": {"status": 200, "body": rows(1)},
    }
    # The edge blocks span minutes. The KV expiry sweeper follows the wall
    # clock, so the OHLC clock does too here (an unparsable clock file falls
    # back to it): entries written at the fixture instant would be swept.
    server = Server(binary, root, upstream_port, ca, "edges", now)
    server.set_clock("wall")
    server.start()
    try:
        # Restart: a cached slug and its held failure, then SIGKILL and a
        # restart over the same KV directory and clock.
        cold = server.request(url("edge-coin"), headers=auth)
        check(cold[0] == 200 and body_json(cold)["cache"] == {"hit": False, "timestamp": None},
              "cold slug fetched live")
        cached = server.request(url("edge-coin"), headers=auth)
        check(body_json(cached)["cache"]["hit"] is True, "slug cached")
        upstream.take_calls()
        server.stop(hard=True)
        server.start()
        again = server.request(url("edge-coin"), headers=auth)
        check(again[2] == cached[2], "after SIGKILL the cached slug reads back byte for byte")
        check(upstream.take_calls() == [], "the durable entry answers without the provider")

        # The largest provider answer the route serves, then the native
        # refusals TypeScript would echo: each is an uncached 502, held 10 s.
        limit = server.request(url("limit-coin", 30), headers=auth)
        check(limit[0] == 200 and len(body_json(limit)["points"]) == 400, "400 provider rows served")
        upstream.take_calls()
        time.sleep(4)  # lane tokens
        for coin in ("over-coin", "short-row-coin", "string-row-coin", "fraction-ts-coin"):
            refused = server.request(url(coin, 30), headers=auth)
            check((refused[0], refused[2]) == (502, b'{"error":"UPSTREAM_UNAVAILABLE"}') and
                  "vary" not in refused[1], f"{coin}: native 502")
            held = server.request(url(coin, 30), headers=auth)
            check(held[0] == 502, f"{coin}: failure held")
            check(len(upstream.take_calls()) == 1, f"{coin}: one provider request")
            time.sleep(2.6)  # one lane token
        # A body over the 256 KiB provider bound.
        huge = server.request(url("huge-coin", 30), headers=auth)
        check(huge[0] == 502, f"over-sized provider body is a 502 ({huge[0]})")
        upstream.take_calls()

        # Disconnects. A caller that leaves while another waits on the same
        # flight does not cancel it: the stayer gets the fetched rows from
        # one provider request.
        time.sleep(6)
        leave = url("leave-coin")
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
        check(stayer[0][0] == 200 and len(body_json(stayer[0])["points"]) == 2,
              "stayer gets the shared fetch")
        check(len(upstream.take_calls()) == 1, "one provider request for leaver and stayer")
        # A sole caller that leaves cancels its flight (documented: nothing
        # is stored); the next caller is answered fully.
        alone = url("leave-coin", 7)
        server.request(alone, headers=auth, leave=True)
        time.sleep(1.5)
        after = server.request(alone, headers=auth)
        check(after[0] == 200 and len(body_json(after)["points"]) == 2,
              "the next caller after a sole leaver is answered")
        check(1 <= len(upstream.take_calls()) <= 2, "at most one refetch after a sole leaver")

        # Concurrent cached reads (four workers plus eight pending
        # connections): identical bodies, cached slug and stored series.
        fresh_minute()
        spl = "/v1/prices/ohlc?id=spl%3ASo11111111111111111111111111111111111111112&days=7"
        for target in (url("edge-coin"), spl):
            replies = run_together(server, [target] * 12, "GET", auth)
            check(all(r[0] == 200 for r in replies) and len({r[2] for r in replies}) == 1,
                  f"12 concurrent reads of {target[:40]} are identical: "
                  f"{sorted({(r[0], r[2][:60]) for r in replies})}")
        # Twelve distinct cold slugs exceed the owner's eight waiters. Keep
        # the first eight in flight before starting four more: launching all
        # twelve on the same scheduler tick can fill the HTTP pool's four
        # workers and eight pending sockets before any worker dequeues, so an
        # admission close would test the transport bound instead of this owner.
        fresh_minute()
        many = [url(f"many-{i:02d}") for i in range(12)]
        upstream.extra.update({f"many-{i:02d}": {"status": 200, "body": rows(1)} for i in range(12)})
        Upstream.SLOW.update({f"many-{i:02d}": 2.0 for i in range(12)})
        upstream.take_calls()
        replies = [None] * len(many)
        first_wave = threading.Barrier(8)
        def many_call(index, barrier=None):
            if barrier:
                barrier.wait()
            replies[index] = server.request(many[index], headers=auth)
        threads = [threading.Thread(target=many_call, args=(i, first_wave)) for i in range(8)]
        for thread in threads:
            thread.start()
        deadline = time.monotonic() + 5
        while upstream.count() == 0 and time.monotonic() < deadline:
            time.sleep(.01)
        check(upstream.count() > 0, "first wave reached the provider")
        time.sleep(.2)  # let the HTTP workers release deferred sockets
        threads += [threading.Thread(target=many_call, args=(i,)) for i in range(8, 12)]
        for thread in threads[8:]:
            thread.start()
        for thread in threads:
            thread.join(timeout=40)
        check(all(not thread.is_alive() for thread in threads), "cold reads do not hang")
        Upstream.SLOW.update({f"many-{i:02d}": 0 for i in range(12)})
        shapes = sorted((r[0], r[2][:40]) for r in replies)
        check(all(r[0] in (200, 429, 502, 503) for r in replies) and
              all(r[0] != 200 or body_json(r)["cache"]["hit"] is False for r in replies),
              f"12 concurrent cold slugs answer live or typed errors: {shapes}")
        check(len(upstream.take_calls()) <= 12, "no more than one request per cold slug")

        # Router bound: a target over 12,500 bytes never reaches the handler.
        long_target = url("x" * 13000)
        check(server.request(long_target, headers=auth)[0] in (400, 414, 431),
              "oversized target refused")
        check(server.request(spl, headers=auth)[0] == 200, "healthy after the oversized target")

        # Hung provider: 502 at the 25 s owner deadline (TypeScript: after
        # its third 10 s attempt), held like any upstream failure.
        if not FAST:
            fresh_minute()
            upstream.hang_s = 40
            started = time.monotonic()
            hung = server.request(url("hung-coin"), headers=auth, timeout=60)
            elapsed = time.monotonic() - started
            check((hung[0], hung[2]) == (502, b'{"error":"UPSTREAM_UNAVAILABLE"}'),
                  f"hung provider answers 502 ({hung[0]} {hung[2][:60]!r})")
            check(24 <= elapsed <= 29, f"hung provider answered at {elapsed:.1f} s")
            held = server.request(url("hung-coin"), headers=auth)
            check(held[0] == 502, "the deadline failure is held")
            upstream.hang_s = 0
            check(server.request(spl, headers=auth)[0] == 200, "healthy after the hung provider")
            upstream.take_calls()
    finally:
        upstream.extra = {}
        server.stop()
    # Privacy: no log line carries the bearer or the device id.
    check(token.encode() not in server.stderr and b"01ARZ3NDEKTSV4RRFFQ69G5FAW" not in server.stderr,
          "the log never carries the bearer or device id")
    return checks


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    fixture = ohlc(json.loads(FIXTURE.read_text()))
    now_ms = fixture["now"]
    failures = []
    replayed = 0
    with tempfile.TemporaryDirectory(prefix="ohlc-query-") as temporary:
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
                server = Server(binary, root, port, ca, group["name"], now_ms)
                r32.minute_room(25)
                server.start()
                try:
                    replayed += replay_group(group, server, upstream, now_ms, failures)
                finally:
                    server.stop()
                if fixture["token"].encode() in server.stderr:
                    failures.append((group["name"], ["bearer token in the log"]))
            for label, problems in failures:
                print(f"MISMATCH {label}:")
                for problem in problems:
                    print("   ", problem)
            if failures:
                print(f"{len(failures)} mismatching case(s) of {replayed}")
                return 1
            if REPLAY_ONLY:
                print(f"price ohlc query route: replayed {replayed} cases, 0 mismatches")
                return 0
            checks = edge_blocks(binary, root, upstream, port, ca, fixture)
        finally:
            http.shutdown()
            http.server_close()
    print(f"price ohlc query route: replayed {replayed} cases, 0 mismatches; "
          f"{len(checks)} edge checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
