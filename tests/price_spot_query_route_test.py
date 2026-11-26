#!/usr/bin/env python3
"""R31 GET /v1/prices: replay the frozen TypeScript request-surface fixture.

`tests/corpus/price_spot_query_route.json` is written by
the earlier TypeScript implementation's tests. Each group runs
in a fresh process over a fresh KV directory, with the price clock file moved
to every case's instant and a loopback CoinGecko stand-in that answers from
the same quote table as the TypeScript mock. Every case compares status,
headers, exact body bytes (JSON text or protobuf wire) and the exact upstream
request paths. After the replay, edge blocks cover what the fixture cannot:
restart over the durable KV, provider timeout, oversized requests, concurrent
distinct reads, client disconnects and privacy of error replies.

usage: price_spot_query_route_test.py build/fortis_node_price_route_test
"""
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

import compat_expectations
import tls_ca

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/corpus/price_spot_query_route.json"

# Lane pacing: native lanes refill by the monotonic clock (burst 4, 0.4/s),
# TypeScript's by the frozen fixture Date. Provider cases wait for a token.
LANE_REFILL_S = 2.6


class Upstream:
    """CoinGecko stand-in mirroring the TypeScript fixture's `answer`."""

    def __init__(self, quotes):
        self.quotes = quotes
        self.mode = "ok"
        self.version = 1
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
                with upstream.lock:
                    upstream.calls.append(self.path)
                    mode, version, hang = upstream.mode, upstream.version, upstream.hang_s
                if hang:
                    time.sleep(hang)
                parsed = urlsplit(self.path)
                if mode == "fail":
                    self.reply(500, {"error": "boom"})
                    return
                if not parsed.path.endswith("/simple/price"):
                    self.reply(404, {})
                    return
                query = parse_qs(parsed.query, keep_blank_values=True)
                ids = query.get("ids", [""])[0].split(",")
                fiat = query.get("vs_currencies", ["usd"])[0]
                body = {}
                for coin in ids:
                    quote = upstream.quotes.get(coin, {}).get(fiat)
                    if not quote:
                        continue
                    entry = {fiat: quote[0] + (version - 1) * 1.25}
                    if quote[1] is not None:
                        entry[fiat + "_24h_change"] = quote[1]
                    if quote[2] is not None:
                        entry[fiat + "_24h_vol"] = quote[2]
                    body[coin] = entry
                self.reply(200, body)

            def reply(self, status, value):
                data = json.dumps(value, separators=(",", ":")).encode()
                try:
                    self.send_response(status)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(data)))
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


def exchange(port, target, *, method="GET", headers=None, timeout=40.0, raw=None):
    sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    if raw is None:
        lines = [f"{method} {target} HTTP/1.1", "Host: localhost", "Connection: close"]
        for key, value in (headers or {}).items():
            lines.append(f"{key}: {value}")
        raw = ("\r\n".join(lines) + "\r\n\r\n").encode("latin1")
    sock.sendall(raw)
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
    status = int(first.split()[1])
    reply_headers = {}
    for line in header_lines:
        if ":" in line:
            key, value = line.split(":", 1)
            reply_headers[key.strip().lower()] = value.strip()
    return status, reply_headers, body


class Server:
    def __init__(self, binary, root, upstream_port, ca, name):
        self.binary = binary
        self.kv_dir = root / f"kv-{name}"
        self.kv_dir.mkdir()
        self.clock = root / f"clock-{name}"
        self.port = free_port()
        from history_contract_expectations import FIXTURE_PROVIDER
        self.env = dict(os.environ)
        # This replay tests retries and cache behavior in the keyed provider lane.
        # Anonymous immediate rejection is covered by the back-pressure replay.
        self.env.update(FIXTURE_PROVIDER)
        self.env.update({
            "FORTIS_ENABLE_TEST_PRICE_ROUTE": "1",
            "FORTIS_TEST_PRICE_UPSTREAM_PORT": str(upstream_port),
            "FORTIS_TEST_PRICE_UPSTREAM_CA": str(ca),
            "FORTIS_TEST_PRICE_NOW_FILE": str(self.clock),
            "FORTIS_KV_DIR": str(self.kv_dir),
        })
        self.env.pop("FORTIS_TEST_PRICE_NOW_MS", None)
        self.env.pop("FORTIS_NATIVE_PRICE_ROUTE_CANARY", None)
        self.process = None

    def set_clock(self, ms):
        self.clock.write_text(str(ms))

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
        self.process = None

    def request(self, target, **kwargs):
        return exchange(self.port, target, **kwargs)


# R31_FAST=1 (mutation runs): no minute alignment, quota counters unchecked.
FAST = os.environ.get("R31_FAST") == "1"


def minute_room(seconds):
    """Wait until a wall-clock device-quota minute has `seconds` left."""
    if FAST:
        return
    left = 60 - (time.time() % 60)
    if left < seconds:
        time.sleep(left + .2)


def parse_reply(status, headers, body, method):
    out = {"status": status, "headers": headers}
    if method == "HEAD":
        out["empty"] = body == b""
    return out


BOUNDED_BODY = b'{"error":"BAD_REQUEST","detail":"ids are limited to 512 bytes each"}'


# GET URLs whose recorded answer is a zod refusal: their HEAD twins map too.
ZOD_URLS = set()


def strict_expected(case):
    """The recorded answer as this server gives it
    (compat-decisions.md#zod-issues): a schema refusal has no issue list."""
    expected = case["response"]
    head = case["method"] == "HEAD"
    status, headers, body = compat_expectations.zod_expected(
        expected["status"], expected["headers"], expected.get("body", ""), head, case["url"] in ZOD_URLS)
    if headers is expected["headers"] and body == expected.get("body", ""):
        return expected
    return dict(expected, headers=headers, body=body)


def compare_case(group, case, reply, failures, allow, same_minute=True):
    expected = strict_expected(case)
    status, headers, body = reply
    label = f"{group}/{case['name']}"
    if label in allow:
        # The documented native bound: a 400 with rate headers and no Vary.
        if (status, body, headers.get("x-rate-limit-cost"), "vary" in headers) != \
                (400, BOUNDED_BODY, "1", False):
            failures.append((label, [f"bounded reply {status} {headers} {body[:120]!r}"]))
        return
    problems = []
    if status != expected["status"]:
        problems.append(f"status {status} != {expected['status']}")
    for name, value in expected["headers"].items():
        if name == "x-rate-limit-remaining" and (case.get("parallel") or not same_minute or FAST):
            # TypeScript's quota minute follows its frozen Date; the native one
            # is the wall clock. Only same-minute counts are comparable.
            if name not in headers:
                problems.append("x-rate-limit-remaining missing")
            continue
        if headers.get(name) != value:
            problems.append(f"header {name}: {headers.get(name)!r} != {value!r}")
    for name in ("vary", "retry-after", "x-rate-limit-cost", "x-rate-limit-remaining"):
        if name in headers and name not in expected["headers"]:
            problems.append(f"unexpected header {name}: {headers[name]!r}")
    if case["method"] == "HEAD":
        if body:
            problems.append("HEAD body not empty")
    elif "bodyHex" in expected:
        if body.hex() != expected["bodyHex"]:
            problems.append(f"protobuf {body.hex()} != {expected['bodyHex']}")
    elif body.decode("utf-8", "replace") != expected["body"]:
        problems.append(f"body {body.decode('utf-8', 'replace')[:400]!r} != {expected['body'][:400]!r}")
    if problems:
        failures.append((label, problems))


def replay_group(group, server, upstream, fixture_ms, failures, allow):
    provider_cases = 0
    first_minute = (fixture_ms + group["cases"][0]["at"]) // 60000
    for case in group["cases"]:
        same_minute = (fixture_ms + case["at"]) // 60000 == first_minute
        server.set_clock(fixture_ms + case["at"])
        upstream.mode = case["upstream"]
        upstream.version = case["version"]
        if case["upstreamCalls"]:
            if provider_cases:
                time.sleep(LANE_REFILL_S)
            provider_cases += 1
        upstream.take_calls()
        headers = case["headers"]
        if case.get("parallel"):
            replies = [None] * case["parallel"]
            barrier = threading.Barrier(case["parallel"])

            def one(index):
                barrier.wait()
                replies[index] = server.request(case["url"], method=case["method"], headers=headers)

            threads = [threading.Thread(target=one, args=(i,)) for i in range(case["parallel"])]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            for reply in replies:
                compare_case(group["name"], case, reply, failures, allow, same_minute)
            remaining = sorted(reply[1].get("x-rate-limit-remaining") for reply in replies)
            if same_minute and not FAST and remaining != case["remaining"]:
                failures.append((f"{group['name']}/{case['name']}",
                                 [f"remaining {remaining} != {case['remaining']}"]))
        else:
            reply = server.request(case["url"], method=case["method"], headers=headers)
            compare_case(group["name"], case, reply, failures, allow, same_minute)
        # Background revalidation and retries finish after the reply.
        deadline = time.monotonic() + (3.0 if case["upstreamCalls"] else 0)
        calls = []
        while time.monotonic() < deadline:
            time.sleep(.05)
            with upstream.lock:
                calls = list(upstream.calls)
            if len(calls) >= len(case["upstreamCalls"]) and case["upstreamCalls"]:
                time.sleep(.2)
                break
        calls = upstream.take_calls()
        label = f"{group['name']}/{case['name']}"
        if calls != case["upstreamCalls"] and label not in allow:
            failures.append((label, [f"upstream {calls} != {case['upstreamCalls']}"]))


# Documented, bounded differences (see the contract comment in
# src/prices/price_spot_route.cpp). Every other case must match byte for byte.
ALLOWED = {
    "query/id of 600 bytes": "native ids are bounded at 512 bytes: 400 instead of an invalid entry",
    "query/contract on a 600-byte network": "native ids are bounded at 512 bytes: 400 instead of a missing entry",
}


def body_json(reply):
    return json.loads(reply[2]) if reply[0] == 200 else None


def edge_blocks(binary, root, upstream, upstream_port, ca, fixture_ms, token):
    """Native-only checks the frozen fixture cannot express."""
    auth = {"Authorization": "Bearer " + token, "Accept": "application/json"}
    checks = []

    def check(condition, message):
        if not condition:
            raise AssertionError(message)
        checks.append(message)

    upstream.mode, upstream.version, upstream.hang_s = "ok", 1, 0.0
    server = Server(binary, root, upstream_port, ca, "edges")
    t0 = fixture_ms + 5 * 3_600_000
    server.set_clock(t0)
    server.start()
    try:
        upstream.take_calls()
        # Restart: positive envelopes, negatives and the durable floor are KV
        # rows; a SIGKILL after the reply loses none of them.
        cold = server.request("/v1/prices?ids=ethereum,unlisted", headers=auth)
        cold_body = body_json(cold)
        check(cold_body and cold_body["prices"]["ethereum"]["cache"]["hit"] is False
              and cold_body["missing"] == ["unlisted"], "cold read before restart")
        check(len(upstream.take_calls()) == 1, "one provider call before restart")
        server.stop(hard=True)
        server.set_clock(t0 + 10_000)
        server.start()
        warm = body_json(server.request("/v1/prices?ids=ethereum,unlisted", headers=auth))
        check(warm["prices"]["ethereum"]["cache"] == {"hit": True, "timestamp": cold_body["prices"]["ethereum"]["asOf"]}
              and warm["prices"]["ethereum"]["stale"] is False and warm["missing"] == ["unlisted"],
              "envelope and negative survive SIGKILL")
        check(upstream.take_calls() == [], "no provider call after restart")
        server.stop(hard=True)
        upstream.mode = "fail"
        server.set_clock(t0 + 1_000_000)
        server.start()
        time.sleep(LANE_REFILL_S)
        floored = body_json(server.request("/v1/prices?ids=ethereum", headers=auth))
        entry = floored["prices"]["ethereum"]
        check(entry["stale"] is True and entry["cache"] == {"hit": True, "timestamp": cold_body["prices"]["ethereum"]["asOf"]}
              and entry["asOf"] == cold_body["prices"]["ethereum"]["asOf"]
              and entry["price"] == cold_body["prices"]["ethereum"]["price"],
              "durable floor survives restart and answers a failing provider")
        time.sleep(.3)
        check(len(upstream.take_calls()) == 3, "a failing provider is tried three times")

        # A client that disconnects does not cancel the shared fetch: the
        # answer still fills the cache for the next caller.
        upstream.mode, upstream.hang_s = "ok", .4
        time.sleep(LANE_REFILL_S)
        sock = socket.create_connection(("127.0.0.1", server.port), timeout=4)
        sock.sendall(("GET /v1/prices?ids=litecoin HTTP/1.1\r\nHost: localhost\r\n"
                      f"Authorization: Bearer {token}\r\nConnection: close\r\n\r\n").encode())
        sock.close()
        time.sleep(1.2)
        upstream.hang_s = 0.0
        check(len(upstream.take_calls()) == 1, "disconnected request still fetched once")
        after = body_json(server.request("/v1/prices?ids=litecoin", headers=auth))
        check(after["prices"]["litecoin"]["cache"]["hit"] is True, "disconnected fetch filled the cache")
        check(upstream.take_calls() == [], "no second fetch after a disconnect")

        # Concurrency: eight distinct cold requests (the owner's capacity)
        # all answer; each id is fetched exactly once in total.
        upstream.hang_s = .3
        time.sleep(LANE_REFILL_S * 2)
        coins = [f"coin-{i:03d}" for i in range(8)]
        replies = [None] * len(coins)
        barrier = threading.Barrier(len(coins))

        def one(index):
            barrier.wait()
            replies[index] = server.request(f"/v1/prices?ids={coins[index]},bitcoin", headers=auth)

        threads = [threading.Thread(target=one, args=(i,)) for i in range(len(coins))]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        upstream.hang_s = 0.0
        check(all(reply[0] == 200 and coins[i] in body_json(reply)["prices"]
                  and "bitcoin" in body_json(reply)["prices"] for i, reply in enumerate(replies)),
              "eight concurrent distinct cold requests all priced")
        time.sleep(.3)
        fetched = []
        for call in upstream.take_calls():
            fetched += parse_qs(urlsplit(call).query)["ids"][0].split(",")
        check(sorted(fetched) == sorted(coins + ["bitcoin"]), f"each id fetched once: {fetched}")

        # Privacy: no reply carries the bearer or the device id.
        bad = server.request("/v1/prices?ids=", headers=auth)
        unauthorized = server.request("/v1/prices?ids=ethereum", headers={"Authorization": "Bearer fintel_dev_" + "Z" * 43})
        good = server.request("/v1/prices?ids=ethereum,bitcoin", headers=auth)
        for reply in (bad, unauthorized, good):
            blob = reply[2] + json.dumps(reply[1]).encode()
            check(b"fintel_dev_" not in blob and b"01ARZ3NDEK" not in blob, "no credential or device id echoed")

        # Router bound: a target over 12,500 bytes never reaches the handler.
        long_target = "/v1/prices?ids=" + "x" * 12_600
        check(server.request(long_target, headers=auth)[0] in (400, 414, 431), "oversized target refused")
        check(server.request("/healthz")[0] == 200, "server healthy after the oversized target")

        # The composed interactive price service has a four-second deadline
        # (price_spot_service.cpp), shorter than the owner's 30-second maximum.
        # A hung upstream returns the durable floor and explicit missing IDs;
        # it must not cache a negative result or strand the next request.
        upstream.hang_s = 11.0
        upstream.take_calls()
        time.sleep(LANE_REFILL_S)
        server.set_clock(t0 + 2_000_000)
        started = time.monotonic()
        timed_out = server.request("/v1/prices?ids=coin-050,ethereum", headers=auth, timeout=45)
        elapsed = time.monotonic() - started
        upstream.hang_s = 0.0
        late = body_json(timed_out)
        check(late is not None and late["missing"] == ["coin-050"]
              and late["prices"]["ethereum"]["stale"] is True
              and late["prices"]["ethereum"]["asOf"] == body_json(good)["prices"]["ethereum"]["asOf"]
              and late["prices"]["ethereum"]["cache"]["timestamp"] == late["prices"]["ethereum"]["asOf"]
              and timed_out[1].get("vary") == "Accept" and 3.5 <= elapsed <= 6.0,
              f"hung provider answers from the floor at {elapsed:.1f}s: {timed_out[0]} {timed_out[2][:200]!r}")
        hung_calls = upstream.take_calls()
        check(1 <= len(hung_calls) <= 3, f"hung provider attempts: {len(hung_calls)}")
        time.sleep(LANE_REFILL_S)
        retried = body_json(server.request("/v1/prices?ids=coin-050", headers=auth))
        check(retried["prices"]["coin-050"]["cache"]["hit"] is False,
              "no negative cached after a timeout; the next read refetches")
        check(server.request("/healthz")[0] == 200, "server healthy after a provider timeout")
    finally:
        upstream.hang_s = 0.0
        server.stop()
    return checks


def main():
    binary = sys.argv[1]
    from history_contract_expectations import spot
    fixture = spot(json.loads(FIXTURE.read_text()))
    fixture_ms = fixture["fixtureTimeMs"]
    groups = {group["name"]: group for group in fixture["groups"]}
    ZOD_URLS.update(case["url"] for group in fixture["groups"] for case in group["cases"]
                    if case["method"] == "GET" and compat_expectations.is_zod_refusal(case["response"].get("body", "")))
    with tempfile.TemporaryDirectory(prefix="r31-spot-") as temp:
        root = Path(temp)
        ca, cert, key, _ = tls_ca.create_certificates(root, "DNS:localhost")
        upstream = Upstream(fixture["quotes"])
        upstream_port = free_port()
        httpd = ThreadingHTTPServer(("127.0.0.1", upstream_port), upstream.handler())
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        httpd.socket = tls.wrap_socket(httpd.socket, server_side=True)
        serving = threading.Thread(target=httpd.serve_forever, daemon=True)
        serving.start()
        failures = []
        try:
            only = set(sys.argv[2:])
            for name, room in (("query", 45), ("auth", 10), ("cache", 50), ("quota", 30)):
                if only and name not in only:
                    continue
                server = Server(binary, root, upstream_port, ca, name)
                server.set_clock(fixture_ms)
                server.start()
                try:
                    minute_room(room)
                    replay_group(groups[name], server, upstream, fixture_ms, failures, ALLOWED)
                finally:
                    server.stop()
            if not only or "edges" in only:
                minute_room(50)
                for line in edge_blocks(binary, root, upstream, upstream_port, ca,
                                        fixture_ms, fixture["token"]):
                    print("edge:", line)
            for label, problems in failures:
                print("MISMATCH", label)
                for problem in problems:
                    print("   ", problem)
            total = sum(len(group["cases"]) for group in fixture["groups"])
            print(f"replayed {total} cases, {len(failures)} mismatches")
            if failures:
                sys.exit(1)
        finally:
            httpd.shutdown()
            httpd.server_close()


if __name__ == "__main__":
    main()
