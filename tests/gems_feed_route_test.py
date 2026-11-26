#!/usr/bin/env python3
"""Real-socket edges of GET /v1/gems (R07) on the native token route binary
(build/fortis_node_token_route_test), against
tests/corpus/gems_feed_edges.json, which
the earlier TypeScript implementation's tests recorded from the
real Fastify app over real sockets with the provider rules of the token
routes contract (the fake upstream here is token_routes_test's).

  - main: the three picks' pages warmed one at a time, then the feed in JSON,
    protobuf and HEAD; the `limit` matrix (missing, 0, -0, negative, 1..20,
    over 20, fractions, exponents, 0x/0o/0b, JavaScript whitespace, escapes,
    NaN/Infinity, the safe-integer edges, repeated and renamed keys, values
    and keys of several KiB); conditional requests (JSON and protobuf
    validators, HEAD, cross-representation, strong/list/star/padded/two-line
    If-None-Match); Accept forms; Origin → bearer → quota before validation;
    other methods and paths (status only: router 404 bodies differ). Status,
    headers, body bytes and the provider requests must match;
  - the oracle's 12 simultaneous reads (JSON, protobuf, HEAD): native HTTP
    admission can close excess connections, so every answered read must be
    TypeScript's bytes; then 240 reads from 12 concurrent clients must all
    answer with those bytes. No read reaches a provider;
  - device D's development minute (240 at cost 5) drained on the wall clock:
    the last 200, the 429s (JSON, protobuf, HEAD, invalid limit) with
    x-rate-limit-remaining, and another device;
  - every provider down while sections are stale, then expired (durable
    floors), then recovered, with the background revalidations;
  - SIGTERM restart over the same KV: the same bytes and a 304 for the
    pre-restart validator; then a SIGKILL restart serves the same bytes;
  - privacy: no device token or id in any response or in the process
    output, and two devices get the same bytes.

Asserted differences (checked exactly, not skipped):
  - router-wide HTTP layer: a HEAD 304 has no `content-length: 0` (Fastify's
    HEAD hook adds it); a repeated If-None-Match line (Node joins the values,
    so no match → 200) and an If-None-Match over 256 bytes (never matches →
    200) are refused by the native parser with a bare 400 before any hook;
  - the native picks are composed one after another, so the two CoinGecko
    picks' market reads are not merged by a batching window: with every
    provider down and the market expired, 7 CoinGecko requests meet the
    native lane (burst 4) where TypeScript sent 4, and depending on timing the
    XFN quote's spot read can be refused and made by a later read once the
    lane has a token (same response bytes either way). The provider requests of the two
    expired-outage cases and the recovery are therefore compared together:
    the same multiset as TypeScript's. Before each follow-up case of that
    window the native lane is given 10.5 s to refill its burst, so the
    deferred read cannot slip past the window.
The spot owner reads the same clock file as the token routes and the KV
sweeper (FORTIS_TEST_PRICE_NOW_FILE), so a cached spot miss ages exactly as
TypeScript's did (300 s), across restarts too.
Usage: gems_feed_route_test.py build/fortis_node_token_route_test
"""
import hashlib
import json
import os
import signal
import socket
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import discovery_route_harness as harness  # noqa: E402
import token_routes_test as tr  # noqa: E402

FIXTURE = "gems_feed_edges.json"
HEADERS = ["content-type", "content-length", "vary", "cache-control", "etag", "x-content-type-options",
           "referrer-policy", "permissions-policy", "x-rate-limit-cost", "retry-after",
           "access-control-allow-origin"]
# Router 404s: the status matches, the body is the native router's (router-wide).
ROUTER_404 = {"post", "trailing-slash", "upper-path", "sub-path", "double-slash"}
# Router-wide native HTTP layer differences: exact native answers.
HEAD_304_WITHOUT_LENGTH = {"inm-head", "inm-head-protobuf"}
PARSER_400 = {"inm-two-lines", "inm-long"}
# The deferred spot read lands in any of these consecutive cases (or between
# them): their provider requests are compared as one multiset.
JOINT_UPSTREAM = ("outage-expired", "outage-expired-protobuf", "recovered")
LETTERS = "ABCDEFGIJKLMNOPQRSTUVWXYZ"
CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

FAILURES = []


def check(condition, message):
    if not condition:
        FAILURES.append(message)
        print("FAIL:", message, flush=True)


def token(letter):
    return "fintel_dev_" + letter * 43


def device_id(letter):
    return "01ARZ3NDEKTSV4RRFFQ69G5FT" + CROCKFORD[LETTERS.index(letter)]


def raw(port, method, target, headers, timeout=60):
    """One request with exactly these header lines; (status, headers, payload)."""
    lines = [f"{method} {target} HTTP/1.1", "Host: localhost", "Connection: close"]
    lines += [f"{key}: {value}" for key, value in headers]
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("latin1"))
        data = bytearray()
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data.extend(chunk)
    head, _, payload = bytes(data).partition(b"\r\n\r\n")
    first, *header_lines = head.decode("latin1").split("\r\n")
    response_headers = {}
    for line in header_lines:
        if ":" in line:
            key, value = line.split(":", 1)
            response_headers[key.lower()] = value.strip()
    return int(first.split()[1]), response_headers, payload


def body_of(fixture, digest):
    stored = fixture["bodies"][digest]
    return bytes.fromhex(stored[4:]) if stored.startswith("hex:") else stored[5:].encode("utf-8", "surrogatepass")


class Run:
    def __init__(self, binary, fixture, root):
        self.binary = binary
        self.fixture = fixture
        self.rules = tr.Rules(fixture["providers"])
        self.upstream = tr.Upstream(root, self.rules)
        self.kv_dir = Path(root) / "kv"
        self.kv_dir.mkdir()
        self.clock = Path(root) / "clock"
        self.lane = tr.CoinGeckoLane()
        self.server = None
        self.outputs = []
        self.responses = []
        self.joint_start = None
        self.joint_expected = []

    def start(self, at):
        harness.set_clock(self.clock, at)
        self.server = tr.Server(self.binary, self.kv_dir, self.clock, self.upstream, at,
                                {"FORTIS_TEST_PRICE_NOW_FILE": str(self.clock)})

    def stop(self, kill=False):
        process = self.server.process
        if kill:
            process.send_signal(signal.SIGKILL)
            process.wait()
        else:
            self.server.stop()
        self.outputs.append(process.stdout.read() + process.stderr.read())
        self.server = None

    def headers_for(self, case):
        headers = []
        if case["device"]:
            headers.append(("authorization", "Bearer " + token(case["device"])))
        headers += [tuple(pair) for pair in case["requestHeaders"]]
        return headers

    def run_case(self, case):
        name = case["name"]
        if name in JOINT_UPSTREAM[1:]:
            time.sleep(10.5)  # the native coingecko lane: 4 tokens at 0.4/s
        self.lane.reserve(case)
        self.rules.mode = case.get("mode") or {}
        self.rules.now = case["at"]
        harness.set_clock(self.clock, case["at"])
        before = len(self.upstream.calls())
        target = bytes.fromhex(case["targetHex"]).decode("latin1")
        status, headers, payload = raw(self.server.port, case["method"], target, self.headers_for(case))
        harness.wait_for_calls(self.upstream, before + len(case["upstream"]), timeout=10.0)
        self.server.request("GET", "/probe/test-token-idle")
        time.sleep(.05)
        self.responses.append((headers, payload))
        observed = tr.observed_calls(self.upstream.calls()[before:])
        expected = tr.expected_calls(case)
        problems = []
        if name in PARSER_400:
            check(status == 400 and payload == b'{"error":"BAD_REQUEST"}' and "x-rate-limit-cost" not in headers
                  and not observed and case["status"] == 200,
                  f"{name}: asserted parser 400, got {status} {payload[:80]!r}")
            return status, headers, payload
        if name == JOINT_UPSTREAM[0]:
            self.joint_start, self.joint_expected = before, list(expected)
            observed = expected = []
        elif name in JOINT_UPSTREAM[1:-1]:
            self.joint_expected += expected
            observed = expected = []
        elif name == JOINT_UPSTREAM[-1]:
            expected = sorted(self.joint_expected + expected)
            harness.wait_for_calls(self.upstream, self.joint_start + len(expected), timeout=15.0)
            observed = tr.observed_calls(self.upstream.calls()[self.joint_start:])
        if status != case["status"]:
            problems.append(f"status {status} != {case['status']} ({payload[:200]!r})")
        if name in ROUTER_404:
            check(not problems, f"{name}: " + "; ".join(problems))
            return status, headers, payload
        names = HEADERS + (["x-rate-limit-remaining"] if case["group"] == "quota" else [])
        for header in names:
            if header == "content-length" and name in HEAD_304_WITHOUT_LENGTH:
                if header in headers or case["headers"].get(header) != "0":
                    problems.append(f"asserted HEAD 304 difference changed: {headers.get(header)!r}")
                continue
            if headers.get(header) != case["headers"].get(header):
                problems.append(f"header {header}: {headers.get(header)!r} != {case['headers'].get(header)!r}")
        want = body_of(self.fixture, case["bodySha256"])
        if payload != want:
            index = next((i for i, (x, y) in enumerate(zip(want, payload)) if x != y), min(len(want), len(payload)))
            problems.append(f"body differs at {index}: expected …{want[max(0, index - 120):index + 120]!r}\n"
                            f"   actual …{payload[max(0, index - 120):index + 120]!r}")
        if observed != expected:
            problems.append(f"upstream\n  expected {expected}\n  actual   {observed}")
        check(not problems, f"{name}: " + "; ".join(problems))
        return status, headers, payload


def concurrency(run, fixture):
    spec = fixture["concurrent"]
    harness.set_clock(run.clock, spec["at"])
    before = len(run.upstream.calls())
    port = run.server.port
    results = [None] * len(spec["kinds"])
    barrier = threading.Barrier(len(results))

    def one(index, kind):
        headers = [("authorization", "Bearer " + token("B"))]
        if kind == "protobuf":
            headers.append(("accept", "application/protobuf"))
        barrier.wait()
        try:
            results[index] = raw(port, "HEAD" if kind == "head" else "GET", "/v1/gems", headers)
        except (OSError, IndexError, ValueError) as error:
            results[index] = error

    threads = [threading.Thread(target=one, args=(i, kind)) for i, kind in enumerate(spec["kinds"])]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    answered = 0
    refused = 0
    for index, result in enumerate(results):
        if isinstance(result, Exception):
            # The native 4-worker + 8-pending limit is a ceiling, not a
            # promise that all twelve slots are free at this instant. An
            # excess connection can close before an HTTP response.
            refused += 1
            continue
        check(result is not None, f"concurrent {index}: no result")
        if result is None:
            continue
        status, headers, payload = result
        answered += 1
        run.responses.append((headers, payload))
        check(status == spec["statuses"][index], f"concurrent {index}: status {status}")
        check(payload == body_of(fixture, spec["bodySha256"][index]), f"concurrent {index}: body")
        check(headers.get("etag") == spec["etags"][index], f"concurrent {index}: etag")
    check(answered >= 4, f"concurrent admission: only {answered}/12 answered")

    # Sustained: 240 reads from 12 clients (20 each = 100 of a device's 240;
    # letters the case rotation never reached), every answer the oracle's JSON
    # or protobuf.
    json_body = body_of(fixture, spec["bodySha256"][0])
    proto_body = body_of(fixture, spec["bodySha256"][1])
    errors = []
    lock = threading.Lock()
    letters = "KLMNOPQRSTUV"

    def client(letter, slot):
        for i in range(20):
            protobuf = (i + slot) % 2 == 1
            headers = [("authorization", "Bearer " + token(letter))]
            if protobuf:
                headers.append(("accept", "application/protobuf"))
            try:
                status, _, payload = raw(port, "GET", "/v1/gems", headers)
            except OSError as error:
                with lock:
                    errors.append(f"{letter}{i}: {error}")
                continue
            if status != 200 or payload != (proto_body if protobuf else json_body):
                with lock:
                    errors.append(f"{letter}{i}: status {status} body {len(payload)}")

    started = time.monotonic()
    threads = [threading.Thread(target=client, args=(letter, slot)) for slot, letter in enumerate(letters)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    elapsed = time.monotonic() - started
    check(not errors, f"sustained reads: {errors[:5]}")
    check(len(run.upstream.calls()) == before, "concurrent reads reached a provider")
    print(f"concurrency: {answered}/12 simultaneous matched, {refused} refused; 240 sustained reads in {elapsed:.2f}s, "
          f"{len(errors)} errors", flush=True)


def wait_for_fresh_minute():
    """Device quota windows are wall-clock minutes: start a drain early in one."""
    second = time.time() % 60
    if second > 40:
        time.sleep(60 - second + .2)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = harness.load_fixture(FIXTURE)
    cases = fixture["cases"]
    by_group = {}
    for case in cases:
        by_group.setdefault(case["group"], []).append(case)
    with tempfile.TemporaryDirectory(prefix="fortis-gems-feed-") as root:
        run = Run(binary, fixture, root)
        try:
            run.start(by_group["main"][0]["at"])
            for case in by_group["main"]:
                run.run_case(case)
            print(f"main: {len(by_group['main'])} cases", flush=True)
            concurrency(run, fixture)

            wait_for_fresh_minute()
            harness.set_clock(run.clock, by_group["quota"][0]["at"])
            for i in range(47):
                status, _, _ = raw(run.server.port, "GET", "/v1/gems", [("authorization", "Bearer " + token("D"))])
                check(status == 200, f"quota drain {i}: {status}")
            for case in by_group["quota"]:
                run.run_case(case)
            print(f"quota: {len(by_group['quota'])} cases", flush=True)

            for case in by_group["failure"]:
                run.run_case(case)
            print(f"failure: {len(by_group['failure'])} cases", flush=True)

            run.stop()
            run.start(by_group["restart"][0]["at"])
            for case in by_group["restart"]:
                run.run_case(case)
            print(f"restart (SIGTERM): {len(by_group['restart'])} cases", flush=True)

            # SIGKILL: nothing flushed at exit; the persisted facts still serve
            # the same feed (and nothing is fetched).
            run.stop(kill=True)
            run.start(by_group["restart"][0]["at"])
            before = len(run.upstream.calls())
            again = dict(by_group["restart"][0], name="after-sigkill", device="E")
            run.run_case(again)
            check(len(run.upstream.calls()) == before, "SIGKILL restart reached a provider")
            print("restart (SIGKILL): 1 case", flush=True)
        finally:
            if run.server:
                run.stop()
            run.upstream.close()

        # Privacy: no credential or device id in any response or process output,
        # and the feed is the same bytes for every device.
        secrets = [token(letter)[11:].encode() for letter in LETTERS] + [device_id(l).encode() for l in LETTERS]
        leaked = [secret for secret in secrets
                  if any(secret in payload or secret in json.dumps(headers).encode() for headers, payload in run.responses)
                  or any(secret in output for output in run.outputs)]
        check(not leaked, f"device secret in a response or log: {leaked[:2]}")
        named = {case["name"]: case for case in cases}
        check(named["json"]["bodySha256"] == named["quota-other-device"]["bodySha256"]
              and named["json"]["device"] != "C", "two devices, one feed")
    if FAILURES:
        print(f"gems feed route: {len(FAILURES)} failure(s)")
        raise SystemExit(1)
    print(f"gems feed route: all {len(cases) + 1} cases and checks passed")


if __name__ == "__main__":
    main()
