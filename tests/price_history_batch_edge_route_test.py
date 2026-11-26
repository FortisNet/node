#!/usr/bin/env python3
"""R46 POST /v1/prices/history: replay the frozen TypeScript edge fixture.

`tests/corpus/price_history_batch_edges.json` is written by
the earlier TypeScript implementation's tests. Each
group runs in a fresh process over a fresh KV directory seeded like the
TypeScript app (ethereum, the non-CoinGecko rows, coin-00…coin-49 and the
broken-coin read fault), against a loopback CoinGecko stand-in whose
market_chart/range answers are the TypeScript mock's. Every case compares
status, headers, exact body bytes (JSON text, protobuf wire, or length and
SHA-256 over 16 KiB) and the exact upstream request paths. Edge blocks then
cover what the fixture cannot: SIGKILL restart, a hung provider, the
deferred-batch bound, concurrent cached batches, the representation-specific
size policy and privacy.

usage: price_history_batch_edge_route_test.py build/fortis_node_history_route_test
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
from urllib.parse import parse_qs, urlsplit

import compat_expectations
import tls_ca
from history_contract_expectations import FIXTURE_PROVIDER, batch_edges

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/corpus/price_history_batch_edges.json"
HOUR = 3600
DAY = 86400
DEVICE_ID = b"01ARZ3NDEKTSV4RRFFQ69G5FA"
# R46_FAST=1 (mutation runs): no minute alignment, quota counters unchecked
# and the 25-second hung-provider block skipped. R46_REPLAY_ONLY=1 also skips
# the edge blocks.
FAST = os.environ.get("R46_FAST") == "1"
REPLAY_ONLY = os.environ.get("R46_REPLAY_ONLY") == "1"
UPSTREAM_UNAVAILABLE = b'{"error":"UPSTREAM_UNAVAILABLE"}'

# fastify-errors: the bounded JSON reader's refusal is this
# server's 400 after the device policy, charged at cost 1.
INVALID_JSON = b'{"error":"BAD_REQUEST"}'
# Documented native bounds; the value is the exact native reply (status,
# body, whether the quota was charged: True at the recorded cost, or the
# native cost as a string); a difference never carries Vary.
DIFFERENCES = {
    # Response-size policy: 50 IDs x 337 hourly buckets can exceed 1 MiB of
    # JSON in the worst case, so the native route refuses before provider
    # work. The same request as protobuf (the app's format) is served.
    ("appWindows", "fifty ids fourteen days hourly json"): (503, UPSTREAM_UNAVAILABLE, True),
    # The shared bounded JSON reader refuses a number that overflows a double
    # (or has 64+ characters) as invalid JSON; JSON.parse reads Infinity and
    # zod then reports it after the quota hook.
    ("fields", "from overflowing number"): (400, INVALID_JSON, "1"),
}


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

    # Concurrent fixture cases: the TypeScript batches all plan their gaps
    # before the first fetch resolves; a delay gives native the same overlap.
    SLOW = {"burst-coin": .6, "alpha-coin": .3, "beta-coin": .3, "shared-coin": .6}

    def __init__(self):
        self.mode = "ok"
        self.hang_s = 0.0
        self.delays = {}
        self.lock = threading.Lock()
        self.calls = []
        self.active = 0
        self.max_active = 0

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
                    upstream.active += 1
                    upstream.max_active = max(upstream.max_active, upstream.active)
                    mode, hang = upstream.mode, upstream.hang_s
                try:
                    if coin == "hung-coin" and hang:
                        time.sleep(hang)
                    time.sleep(Upstream.SLOW.get(coin, 0) + upstream.delays.get(coin, 0))
                    if not (parsed.path.startswith("/api/v3/coins/") and
                            parsed.path.endswith("/market_chart/range")):
                        self.reply(404, {"error": "unexpected"})
                    elif mode == "fail" or coin == "failing-coin":
                        self.reply(500, {"error": "boom"})
                    elif mode == "limited":
                        self.reply(429, {"error": "slow down"}, {"Retry-After": "30"})
                    else:
                        query = parse_qs(parsed.query)
                        self.reply(200, market_chart(coin, int(query["from"][0]),
                                                     int(query["to"][0])))
                finally:
                    with upstream.lock:
                        upstream.active -= 1

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


def post(port, request, headers, timeout=60.0, leave=False):
    """One POST /v1/prices/history; `request` is a fixture request entry."""
    sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    lines = ["POST /v1/prices/history HTTP/1.1", "Host: localhost", "Connection: close"]
    for key, value in headers.items():
        lines.append(f"{key}: {value}")
    if "contentType" in request:
        lines.append("Content-Type: " + request["contentType"])
    body = bytes.fromhex(request["hex"]) if "hex" in request else None
    if body is not None:
        lines.append(f"Content-Length: {len(body)}")
    sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("latin1") + (body or b""))
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
    head, _, payload = bytes(data).partition(b"\r\n\r\n")
    if not head:
        return 0, {}, b""
    first, *header_lines = head.decode("latin1").split("\r\n")
    reply_headers = {}
    for line in header_lines:
        if ":" in line:
            key, value = line.split(":", 1)
            reply_headers[key.strip().lower()] = value.strip()
    return int(first.split()[1]), reply_headers, payload


class Server:
    def __init__(self, binary, root, upstream_port, ca, name):
        self.binary = binary
        self.kv_dir = root / f"kv-{name}"
        self.kv_dir.mkdir()
        self.port = free_port()
        self.env = dict(os.environ)
        self.env.update(FIXTURE_PROVIDER)  # Batch semantics, not anonymous admission limits.
        for key in ("FORTIS_NATIVE_PRICE_ROUTE_CANARY", "FORTIS_TEST_HISTORY_DECADE_SEED",
                    "FORTIS_TEST_OHLC_SEED", "FORTIS_TEST_HISTORY_NOW_MS",
                    "FORTIS_TEST_HISTORY_OWNER_START_FAIL"):
            self.env.pop(key, None)
        self.env.update({
            "FORTIS_ENABLE_TEST_PRICE_HISTORY_ROUTE": "1",
            "FORTIS_TEST_HISTORY_UPSTREAM_PORT": str(upstream_port),
            "FORTIS_TEST_HISTORY_UPSTREAM_CA": str(ca),
            "FORTIS_TEST_HISTORY_OTHER_IDS_SEED": "1",
            "FORTIS_TEST_HISTORY_BATCH_SEED": "1",
            "FORTIS_TEST_HISTORY_FAIL_SOURCE": "broken-coin",
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
            self.process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
        self.stderr += self.process.stderr.read()
        self.process = None

    def post(self, request, headers, **kwargs):
        return post(self.port, request, headers, **kwargs)


def minute_room(seconds):
    """Wait until a wall-clock device-quota minute has `seconds` left."""
    if FAST:
        return
    left = 60 - (time.time() % 60)
    if left < seconds:
        time.sleep(left + .2)


def shaped(expected, reply, same_minute, quota_listed, quota_offset=0):
    """Problems between one native reply and one recorded TypeScript reply."""
    status, headers, body = reply
    # compat-decisions.md#zod-issues: a recorded zod refusal is
    # {"error":"BAD_REQUEST"} here.
    _, mapped_headers, mapped_body = compat_expectations.zod_expected(
        expected["status"], expected["headers"], expected.get("body", ""))
    if mapped_headers is not expected["headers"]:
        expected = dict(expected, headers=mapped_headers, body=mapped_body)
    problems = []
    if status != expected["status"]:
        problems.append(f"status {status} != {expected['status']}: {body[:300]!r}")
    for name, value in expected["headers"].items():
        if name == "x-rate-limit-remaining" and (quota_listed or not same_minute or FAST):
            # TypeScript's quota minute follows its frozen Date, the native
            # one the wall clock; concurrent counters are compared as a list.
            if name not in headers:
                problems.append("x-rate-limit-remaining missing")
            continue
        if name == "x-rate-limit-remaining":
            # Quota a documented difference left uncharged is still free.
            value = str(int(value) + quota_offset)
        if headers.get(name) != value:
            problems.append(f"header {name}: {headers.get(name)!r} != {value!r}")
    for name in ("vary", "retry-after", "x-rate-limit-cost", "x-rate-limit-remaining"):
        if name in headers and name not in expected["headers"]:
            problems.append(f"unexpected header {name}: {headers[name]!r}")
    if "bodySha256" in expected:
        digest = hashlib.sha256(body).hexdigest()
        if (len(body), digest) != (expected["bodyLength"], expected["bodySha256"]):
            problems.append(f"large body {len(body)} {digest} != "
                            f"{expected['bodyLength']} {expected['bodySha256']}")
    elif "bodyHex" in expected:
        if body.hex() != expected["bodyHex"]:
            problems.append(f"protobuf {body.hex()[:400]} != {expected['bodyHex'][:400]}")
    elif body.decode("utf-8", "replace") != expected["body"]:
        problems.append(f"body {body.decode('utf-8', 'replace')[:600]!r} != "
                        f"{expected['body'][:600]!r}")
    return problems


def run_together(server, requests, headers, stagger=0.0):
    replies = [None] * len(requests)
    barrier = threading.Barrier(len(requests))

    def one(index):
        barrier.wait()
        # A stagger keeps TypeScript's injection order for distinct batches.
        time.sleep(stagger * index)
        replies[index] = server.post(requests[index], headers)

    threads = [threading.Thread(target=one, args=(i,)) for i in range(len(requests))]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    return replies


def replay_group(group, server, upstream, fixture_ms, failures):
    first_minute = (fixture_ms + group["cases"][0]["at"]) // 60000
    replayed = 0
    quota_offset = 0
    for case in group["cases"]:
        label = f"{group['name']}/{case['name']}"
        same_minute = (fixture_ms + case["at"]) // 60000 == first_minute
        upstream.mode = case["upstream"]
        upstream.take_calls()
        requests = case["requests"]
        if case.get("parallel"):
            requests = requests * case["parallel"]
        problems = []
        if len(requests) == 1:
            reply = server.post(requests[0], case["headers"])
            removed = compat_expectations.is_removed_body_media(requests[0].get("contentType"))
            framing = compat_expectations.fastify_code(case["response"].get("body"))
            if removed or framing:
                # compat-decisions.md#form-bodies, #fastify-errors:
                # the body parser's refusal is answered after the device
                # policy at cost 1 (or the policy refuses first); the quota
                # the oracle charged is replaced by that cost.
                recorded = case["response"]["body"] if framing else compat_expectations.UNSUPPORTED_MEDIA_TYPE_FASTIFY
                want_status, _, want_body, charged = compat_expectations.fastify_expected(
                    case["response"]["status"], {}, recorded, case["headers"])
                if (reply[0], reply[2]) != (want_status, want_body) or \
                        reply[1].get("x-rate-limit-cost") != ("1" if charged else None):
                    problems.append(f"body framing {reply[0]} {reply[2][:200]!r} cost {reply[1].get('x-rate-limit-cost')}")
                quota_offset += int(case["response"]["headers"].get("x-rate-limit-cost", 0)) - (1 if charged else 0)
                upstream.take_calls()
                if problems:
                    failures.append((label, problems))
                replayed += 1
                continue
            if compat_expectations.is_prototype_poisoning(bytes.fromhex(requests[0].get("hex", "")),
                                                          case["response"].get("body")):
                # compat-decisions.md#js-prototype-keys: an ordinary
                # unknown key, refused by the strict schema after the quota.
                if (reply[0], reply[2]) != (400, compat_expectations.BAD_REQUEST) or \
                        "x-rate-limit-cost" not in reply[1]:
                    problems.append(f"__proto__ key {reply[0]} {reply[2][:200]!r}")
                quota_offset -= int(reply[1].get("x-rate-limit-cost", 0))
                upstream.take_calls()
                if problems:
                    failures.append((label, problems))
                replayed += 1
                continue
            difference = DIFFERENCES.get((group["name"], case["name"]))
            if difference:
                want = case["response"]
                if (reply[0], reply[2]) != difference[:2]:
                    problems.append(f"documented difference {reply[0]} {reply[2][:200]!r}")
                cost = want["headers"].get("x-rate-limit-cost") if difference[2] is True else difference[2] or None
                if difference[2] is not True:
                    quota_offset += int(want["headers"].get("x-rate-limit-cost", 0)) - int(cost or 0)
                if "vary" in reply[1] or reply[1].get("x-rate-limit-cost") != cost:
                    problems.append(f"difference headers {reply[1]}")
                upstream.take_calls()
                if problems:
                    failures.append((label, problems))
                replayed += 1
                continue
            problems = shaped(case["response"], reply, same_minute, False, quota_offset)
        else:
            replies = run_together(server, requests, case["headers"],
                                   0 if case.get("parallel") else .1)
            remaining = sorted(reply[1].get("x-rate-limit-remaining") for reply in replies)
            if same_minute and not FAST and remaining != case["remaining"]:
                problems.append(f"remaining {remaining} != {case['remaining']}")
            expected = case["responses"]
            if case.get("parallel"):
                # A multiset: every recorded reply matched by a distinct native one.
                unmatched = list(replies)
                for want in expected:
                    for index, reply in enumerate(unmatched):
                        if not shaped(want, reply, same_minute, True):
                            del unmatched[index]
                            break
                    else:
                        problems.append("no native reply matches " + json.dumps(want)[-300:] +
                                        "; native: " + "; ".join(
                                            r[2].decode("utf-8", "replace")[-200:]
                                            for r in unmatched))
            else:
                for want, reply in zip(expected, replies):
                    problems += shaped(want, reply, same_minute, True)
        calls = sorted(upstream.take_calls())
        if calls != case["upstreamCalls"]:
            problems.append(f"upstream {calls} != {case['upstreamCalls']}")
        if problems:
            failures.append((label, problems))
        replayed += 1
    return replayed


def json_request(ids, start, end, interval="1h"):
    text = json.dumps({"ids": ids, "from": start, "to": end, "interval": interval},
                      separators=(",", ":"))
    return {"contentType": "application/json", "hex": text.encode().hex()}


def varint(value):
    out = bytearray()
    while True:
        byte = value & 0x7f
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def proto_request(ids, start, end, interval="1h"):
    body = bytearray()
    for item in ids:
        raw = item.encode()
        body += b"\x0a" + varint(len(raw)) + raw
    body += b"\x10" + varint(start) + b"\x18" + varint(end)
    body += b"\x22" + varint(len(interval)) + interval.encode()
    return {"contentType": "application/protobuf", "hex": bytes(body).hex()}


def body_json(reply):
    return json.loads(reply[2]) if reply[0] == 200 else None


def edge_blocks(binary, root, upstream, upstream_port, ca, fixture):
    """Native-only checks the frozen fixture cannot express."""
    token = fixture["token"]
    first = fixture["first"]
    auth = {"Authorization": "Bearer " + token, "Accept": "application/json"}
    proto_auth = {"Authorization": "Bearer " + token, "Accept": "application/protobuf"}
    checks = []

    def check(condition, message):
        checks.append(message)
        if not condition:
            raise AssertionError(message)

    servers = []
    server = Server(binary, root, upstream_port, ca, "edges")
    servers.append(server)
    server.start()
    try:
        upstream.mode = "ok"
        upstream.take_calls()
        # Restart: a cold fetch, an empty (covered) answer and a failed gap
        # in one batch, then SIGKILL and a restart over the same KV directory.
        restart = json_request(["bitcoin", "empty-coin", "failing-coin", "ethereum"],
                               first, first + 6 * HOUR)
        cold = server.post(restart, auth)
        series = body_json(cold)["series"]
        check(cold[0] == 200 and series["bitcoin"]["cache"]["hit"] is False and
              len(series["bitcoin"]["points"]) == 7 and
              series["failing-coin"]["points"] == [] and
              series["failing-coin"]["cache"]["hit"] is True, "cold batch before restart")
        check(len(upstream.take_calls()) == 5, "cold batch: bitcoin, empty-coin, 3 failing attempts")
        server.stop(hard=True)
        server.start()
        again = server.post(restart, auth)
        series = body_json(again)["series"]
        check(again[0] == 200 and series["bitcoin"]["cache"]["hit"] is True and
              series["bitcoin"]["coverage"] == {"cachedPoints": 7, "fetchedPoints": 0} and
              series["empty-coin"]["cache"]["hit"] is True,
              "after SIGKILL the candles read back as cached")
        calls = upstream.take_calls()
        check(len(calls) == 3 and all("failing-coin" in call for call in calls),
              "coverage survives SIGKILL; only the failed gap is retried (3 attempts)")
        check(body_json(again)["cache"] == {"hit": True, "timestamp": "2026-01-01T06:00:00.000Z",
                                             "partial": False},
              "restart batch provenance is all cached")

        # Twelve concurrent cached batches (four workers plus eight pending
        # connections) are answered identically.
        cached = json_request(["ethereum", "asa:7", "spl:So11111111111111111111111111111111111111112"],
                              first, first + 6 * HOUR)
        replies = run_together(server, [cached] * 12, auth)
        check(all(r[0] == 200 for r in replies) and len({r[2] for r in replies}) == 1,
              "12 concurrent cached batches are identical: " +
              repr(sorted({(r[0], r[2][:80]) for r in replies})))

        # Deferred-batch bound: four batches waiting on a slow provider hold
        # every deferred slot; a fifth cold batch is refused with 503 (quota
        # charged, no Vary) while a cached batch is still served.
        time.sleep(11)  # lane tokens back to the burst
        for index in range(4):
            upstream.delays[f"slot-{index}"] = 3.0
        waiting = []

        def slow(index):
            waiting.append(server.post(json_request([f"slot-{index}"], first,
                                                    first + 2 * HOUR), auth))

        threads = [threading.Thread(target=slow, args=(i,)) for i in range(4)]
        for thread in threads:
            thread.start()
            time.sleep(.05)
        time.sleep(.6)
        fifth = server.post(json_request(["slot-extra", "ethereum"], first, first + 2 * HOUR),
                            auth)
        check((fifth[0], fifth[2]) == (503, UPSTREAM_UNAVAILABLE) and
              fifth[1].get("x-rate-limit-cost") == "2" and "vary" not in fifth[1],
              f"a fifth deferred batch is refused: {fifth[0]} {fifth[1]}")
        hit = server.post(cached, auth)
        check(hit[0] == 200 and hit[2] == replies[0][2],
              "a cached batch is served while four batches wait")
        for thread in threads:
            thread.join(timeout=40)
        check(len(waiting) == 4 and all(r[0] == 200 for r in waiting),
              "the four waiting batches complete")
        check(upstream.max_active <= 2, f"provider concurrency stays at the lane's 2 "
                                        f"({upstream.max_active})")
        upstream.delays.clear()
        upstream.take_calls()

        # Response-size policy per representation over stored-only IDs (no
        # provider work): 90-day hourly fits 5 IDs as JSON and 11 as protobuf;
        # ten-year daily fits 3 as JSON and 7 as protobuf.
        def asa(count):
            return [f"asa:{i}" for i in range(count)]

        span_h, span_d = (first, first + 90 * DAY), (first - 3660 * DAY, first)
        for ids, span, interval, make, headers, want, label in (
                (asa(5), span_h, "1h", json_request, auth, 200, "5 hourly JSON"),
                (asa(6), span_h, "1h", json_request, auth, 503, "6 hourly JSON"),
                (asa(11), span_h, "1h", proto_request, proto_auth, 200, "11 hourly protobuf"),
                (asa(12), span_h, "1h", proto_request, proto_auth, 503, "12 hourly protobuf"),
                (asa(3), span_d, "1d", json_request, auth, 200, "3 daily JSON"),
                (asa(4), span_d, "1d", json_request, auth, 503, "4 daily JSON"),
                (asa(7), span_d, "1d", proto_request, proto_auth, 200, "7 daily protobuf"),
                (asa(8), span_d, "1d", proto_request, proto_auth, 503, "8 daily protobuf")):
            reply = server.post(make(ids, span[0], span[1], interval), headers)
            check(reply[0] == want and (want == 200 or reply[2] == UPSTREAM_UNAVAILABLE),
                  f"size policy: {label} -> {want} ({reply[0]})")
        # JSON accepts protobuf media for the response only when asked.
        reply = server.post(proto_request(asa(12), *span_h), auth)
        check(reply[0] == 503, "a protobuf body answered as JSON uses the JSON bound")
        check(upstream.take_calls() == [], "size refusals start no provider work")

        # Hung provider: the gap plan stops at the 25 s owner deadline and the
        # batch answers the stored series; the cached item is unaffected.
        if not FAST:
            upstream.hang_s = 40
            started = time.monotonic()
            hung = server.post(json_request(["hung-coin", "ethereum"], first, first + 2 * HOUR),
                               auth, timeout=60)
            elapsed = time.monotonic() - started
            upstream.hang_s = 0
            series = body_json(hung)["series"] if hung[0] == 200 else {}
            check(hung[0] == 200 and series["hung-coin"]["points"] == [] and
                  series["hung-coin"]["cache"] == {"hit": True, "timestamp": None} and
                  len(series["ethereum"]["points"]) == 3,
                  f"hung provider answers the stored series ({hung[0]} {hung[2][:300]!r})")
            check(24 <= elapsed <= 29, f"hung provider answered at {elapsed:.1f} s")
            check(server.post(cached, auth)[0] == 200, "healthy after the hung provider")
            upstream.take_calls()
    finally:
        for item in servers:
            item.stop()
    # Privacy: no log line carries the bearer or the device id.
    log = b"".join(item.stderr for item in servers)
    check(token.encode() not in log and DEVICE_ID not in log,
          "the log never carries the bearer or device id")
    return checks


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    raw = FIXTURE.read_bytes()
    checksum = FIXTURE.with_name(FIXTURE.name + ".sha256").read_text()
    if hashlib.sha256(raw).hexdigest() + "\n" != checksum:
        print("fixture checksum mismatch")
        return 1
    fixture = batch_edges(json.loads(raw))
    fixture_ms = int(time.mktime(time.strptime(fixture["source"]["fixtureTime"][:19],
                                               "%Y-%m-%dT%H:%M:%S")) - time.timezone) * 1000
    failures = []
    replayed = 0
    with tempfile.TemporaryDirectory(prefix="history-batch-edges-") as temporary:
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
                minute_room(25)
                server.start()
                try:
                    replayed += replay_group(group, server, upstream, fixture_ms, failures)
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
                print(f"price history batch edges: replayed {replayed} cases, 0 mismatches")
                return 0
            checks = edge_blocks(binary, root, upstream, port, ca, fixture)
        finally:
            http.shutdown()
            http.server_close()
    print(f"price history batch edges: replayed {replayed} cases "
          f"({len(DIFFERENCES)} documented differences), 0 mismatches; "
          f"{len(checks)} edge checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
