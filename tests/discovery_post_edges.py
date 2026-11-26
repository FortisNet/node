#!/usr/bin/env python3
"""Shared replay machinery of the R40/R47 discovery POST edge tests
(bittensor_analytics_edges_test.py, price_markets_edges_test.py).

Both replay a pinned TypeScript socket-oracle fixture against
build/fortis_node_discovery_route_test: the fixture's SHA-256 is checked
against its .sha256 sidecar (and a modified copy must be refused), every
request goes over a real socket with the recorded headers and body bytes,
and every provider request is answered from TypeScript's recorded replies,
in order per path, by one loopback HTTPS upstream.

The native quota, lanes and breaker run on real time where TypeScript's ran
on its frozen Date, so the replay paces each device inside its per-minute
quota and each provider lane inside its native budget.
"""
import collections
import hashlib
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compat_expectations  # noqa: E402
import strict_contract_expectations  # noqa: E402
import discovery_route_harness as harness  # noqa: E402
import tls_ca  # noqa: E402

COUNTS = collections.Counter()
FAILURES = []
FAIL_FAST = os.environ.get("R40_R47_FAIL_FAST") == "1"
HEADERS = ["content-type", "content-length", "vary", "cache-control", "etag", "x-content-type-options",
           "referrer-policy", "permissions-policy", "x-rate-limit-cost", "x-rate-limit-remaining", "retry-after",
           "access-control-allow-origin"]
NATIVE_BASELINE = {"cache-control": "no-store", "x-content-type-options": "nosniff", "referrer-policy": "no-referrer",
                   "permissions-policy": "camera=(), microphone=(), geolocation=()"}
NATIVE_NOT_FOUND = b'{"error":"NOT_FOUND"}'
# fastify-errors: the bounded JSON reader's refusal is this
# server's 400 after the device policy (charged; see native_invalid_json).
NATIVE_INVALID_JSON = b'{"error":"BAD_REQUEST"}'
# Native lane budgets (src/providers/provider_lane.cpp): (requests, interval ms, burst).
LANES = {"api.taostats.io": (30, 60000, 30), "api.coingecko.com": (24, 60000, 4),
         "pro-api.coingecko.com": (24, 60000, 4)}


def check(condition, message):
    if not condition:
        FAILURES.append(message)
        print("FAIL:", message, flush=True)
        if FAIL_FAST:
            raise SystemExit(1)
    return condition


def native_plain_error(label, actual, status, body, content_type="application/json", head=False):
    """Assert the whole native router/parser refusal, including absence of quota headers."""
    got_status, headers, payload = actual
    # The router's Server-Timing: well-formed, then excluded from the exact set.
    headers = dict(headers)
    timing = headers.pop("server-timing", "")
    if not check(re.fullmatch(r"app;dur=\d+\.\d{3}", timing) is not None,
                 f"{label}: Server-Timing {timing!r}"):
        return False
    expected_headers = {"content-type": content_type, "content-length": str(len(body)),
                        "connection": "close", **NATIVE_BASELINE}
    expected_payload = b"" if head else body
    return check(got_status == status and payload == expected_payload and headers == expected_headers,
                 f"{label}: native refusal {(got_status, headers, payload[:160])!r} != "
                 f"{(status, expected_headers, expected_payload)!r}")


def native_invalid_json(label, actual, cost):
    """fastify-errors: the bounded JSON reader's refusal is 400
    {"error":"BAD_REQUEST"} after the device policy, charged `cost`."""
    status, headers, payload = actual
    return check(status == 400 and payload == NATIVE_INVALID_JSON and headers.get("x-rate-limit-cost") == str(cost)
                 and headers.get("content-type") == "application/json; charset=utf-8",
                 f"{label}: native {status} {payload[:160]!r} cost {headers.get('x-rate-limit-cost')!r}")


def load_fixture(name, directory=None):
    directory = Path(directory or harness.FIXTURES)
    data = (directory / name).read_bytes()
    pinned = (directory / f"{name}.sha256").read_text().split()[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{name}: sha256 {digest} does not match the pinned {pinned}")
    print(f"{name}: sha256 {digest} matches the pin", flush=True)
    return strict_contract_expectations.apply(name, json.loads(data))


def pin_self_check(name):
    """A copy with one changed byte must be refused by load_fixture."""
    with tempfile.TemporaryDirectory(prefix="fortis-r40-r47-pin-") as root:
        shutil.copy(harness.FIXTURES / f"{name}.sha256", Path(root) / f"{name}.sha256")
        data = bytearray((harness.FIXTURES / name).read_bytes())
        data[len(data) // 2] ^= 1
        (Path(root) / name).write_bytes(bytes(data))
        try:
            load_fixture(name, root)
        except SystemExit as refused:
            print(f"  pin self-check: a modified copy is refused ({str(refused)[:60]}…)", flush=True)
            return True
    return check(False, f"a modified {name} copy was accepted")


def token(letter):
    return "fintel_dev_" + letter * 43


def raw(port, method, target, headers, body=None, timeout=90):
    lines = [f"{method} {target} HTTP/1.1", "Host: localhost", "Connection: close"]
    lines += [f"{key}: {value}" for key, value in headers]
    if body is not None:
        lines.append(f"Content-Length: {len(body)}")
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("latin1") + (body or b""))
        data = bytearray()
        while True:
            chunk = sock.recv(1 << 20)
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


def expected_body(record):
    if "wireHex" in record:
        return bytes.fromhex(record["wireHex"])
    return record.get("responseText", "").encode("utf-8", "surrogatepass")


def first_difference(expected, actual):
    index = next((i for i, (x, y) in enumerate(zip(expected, actual)) if x != y), min(len(expected), len(actual)))
    return f"at {index}: expected …{expected[max(0, index - 120):index + 120]!r}\n   actual   …" \
           f"{actual[max(0, index - 120):index + 120]!r}"


def request_header(record, name):
    """A recorded request header ([name, value] pairs or a mapping)."""
    pairs = record.get("requestHeaders") or []
    items = pairs.items() if isinstance(pairs, dict) else pairs
    for key, value in items:
        if key.lower() == name:
            return value
    return None


def strict_compare(label, record, status, headers, payload):
    """True when `record` recorded a removed behaviour; checks this
    server's answer instead (compat-decisions.md#form-bodies,
    #js-prototype-keys)."""
    ce = compat_expectations
    body = bytes.fromhex(record["bodyHex"]) if record.get("bodyHex") else b""
    if ce.is_prototype_poisoning(body, record.get("responseText")):
        # The body now parses, so authentication answers first (auth-order):
        # without a device the request is 401, with one the schema refuses
        # the unknown key after the quota.
        if status == 401:
            check(payload == b'{"error":"UNAUTHORIZED"}' and not record.get("device"),
                  f"{label}: __proto__ key {status} {payload[:200]!r} with a device bearer")
            return True
        check(status == 400 and payload == ce.BAD_REQUEST and "x-rate-limit-cost" in headers,
              f"{label}: __proto__ key {status} {payload[:200]!r} is not the schema's refusal after the hooks")
        return True
    removed = ce.is_removed_body_media(request_header(record, "content-type"))
    if removed or ce.fastify_code(record.get("responseText")):
        # form-bodies + fastify-errors: the body parser's refusal is judged
        # after the device policy (charged), or the policy refuses first.
        recorded = ce.UNSUPPORTED_MEDIA_TYPE_FASTIFY if removed else record["responseText"]
        if "device" in record:
            request = {"authorization": "Bearer " + token(record["device"]) if record["device"] else ""}
            if request_header(record, "origin") is not None:
                request["origin"] = request_header(record, "origin")
        else:
            request = None  # a concurrent sub-request: always sent with the group's bearer
        want_status, _, want_body, charged = ce.fastify_expected(record["status"], {}, recorded, request)
        if record.get("name", "").startswith("quota-over") and want_status in (400, 415):
            # auth-first: the device's quota is spent, so the policy answers.
            want_status, want_body, charged = 429, b'{"error":"RATE_LIMITED"}', True
        elif record.get("name", "").startswith("attestation-") and want_status in (400, 415):
            # auth-first: attestation is required and the device has none.
            want_status, want_body, charged = 401, b'{"error":"ATTESTATION_REQUIRED"}', False
        check(status == want_status and payload == want_body and ("x-rate-limit-cost" in headers) == charged,
              f"{label}: body framing {status} {payload[:200]!r} is not {want_status} {want_body!r}"
              f" ({'charged' if charged else 'uncharged'})")
        return True
    return False


def compare(label, record, status, headers, payload, remaining=False, skip=()):
    if strict_compare(label, record, status, headers, payload):
        return True
    if not check(status == record["status"], f"{label}: status {status} != {record['status']} {payload[:300]!r}"):
        return False
    ok = True
    for name in HEADERS:
        if name in skip or (name == "x-rate-limit-remaining" and not remaining):
            continue
        ok &= check(headers.get(name) == record["headers"].get(name),
                    f"{label}: header {name} {headers.get(name)!r} != {record['headers'].get(name)!r}")
    if "body" not in skip:
        expected = expected_body(record)
        ok &= check(payload == expected, f"{label}: body {first_difference(expected, payload)}")
    return ok


def path_of(url):
    split = urlsplit(url)
    return split.path + (f"?{split.query}" if split.query else "")


class Replies:
    """Per case, TypeScript's replies per path in the order they were served;
    a path asked again after its queue is empty gets its last reply (counted
    as an extra request). `fallback(path)` may answer paths TypeScript never
    asked (None = 599)."""

    def __init__(self, fallback=None):
        self.lock = threading.Lock()
        self.queues = {}
        self.last = {}
        self.extra = []
        self.fallback = fallback

    def start(self, calls):
        with self.lock:
            self.queues = collections.defaultdict(collections.deque)
            for call in calls:
                self.queues[path_of(call["url"])].append(call)

    def __call__(self, path, headers):
        with self.lock:
            queue = self.queues.get(path)
            call = None
            if queue:
                call = queue.popleft()
                self.last[path] = call
            elif path in self.last:
                call = self.last[path]
                self.extra.append(path)
            fallback = self.fallback
        if call is None:
            # Outside the lock: a fallback may sleep (concurrency checks).
            answer = fallback(path, headers) if fallback else None
            if answer is not None:
                return answer
            with self.lock:
                self.extra.append(path)
            return 599, {}, b"unexpected request"
        reply_headers = {k: v for k, v in call.get("replyHeaders", {}).items()}
        # The oracle's provider answered after its round trip (25 ms, or the
        # recorded delay): whether a stale read's background revalidation
        # meets a request in flight on the lane depends on it.
        time.sleep(call.get("delayMs", 25) / 1000)
        return call["status"], reply_headers, call["body"].encode("utf-8", "surrogatepass")


class Lanes:
    """Per-lane token buckets with the native budgets, in real time; a 429
    opens the native breaker for 30 s on its monotonic clock."""

    def __init__(self):
        self.state = {}
        self.open_until = {}

    def reserve(self, calls, minimum=0):
        """Waits until the lanes hold a token per distinct provider request
        of `calls` (retries of one path ride one lane run), at least
        `minimum` on the first host."""
        need = collections.Counter(netloc for netloc, _ in
                                   {(urlsplit(call["url"]).netloc, path_of(call["url"])) for call in calls})
        if minimum and calls:
            host = urlsplit(calls[0]["url"]).netloc
            need[host] = max(need[host], minimum)
        now = time.monotonic()
        wait = 0.0
        for lane, count in need.items():
            if lane not in LANES:
                continue
            wait = max(wait, self.open_until.get(lane, 0.0) - now)
            rate, interval, burst = LANES[lane]
            per_ms = rate / interval
            tokens, at = self.state.get(lane, (float(burst), now))
            tokens = min(float(burst), tokens + (now - at) * 1000 * per_ms)
            want = min(float(count), float(burst))
            if tokens < want:
                wait = max(wait, (want - tokens) / per_ms / 1000 + 0.05)
            self.state[lane] = (tokens, now)
        if wait > 0:
            time.sleep(wait)
        now = time.monotonic()
        for lane, count in need.items():
            if lane not in LANES:
                continue
            rate, interval, burst = LANES[lane]
            tokens, at = self.state[lane]
            tokens = min(float(burst), tokens + (now - at) * rate / interval * 1000)
            self.state[lane] = (max(0.0, tokens - count), now)

    def breaker(self, calls):
        for call in calls:
            if call["status"] == 429:
                self.open_until[urlsplit(call["url"]).netloc] = time.monotonic() + 31.0


class Quota:
    """The native per-device quota runs on the wall clock (development: 240
    per minute); each device is kept `margin` units below it per minute."""

    def __init__(self, limit=240, margin=20):
        self.limit = limit - margin
        self.used = collections.Counter()
        self.lock = threading.Lock()

    def reserve(self, device, cost):
        if not device:
            return
        while True:
            with self.lock:
                minute = int(time.time() // 60)
                if self.used[(device, minute)] + cost <= self.limit:
                    self.used[(device, minute)] += cost
                    return
            time.sleep(61 - time.time() % 60)


class Server:
    def __init__(self, binary, kv_dir, clock_file, upstream, env):
        self.inner = harness.Server(binary, kv_dir, clock_file, upstream, env=env)
        self.port = self.inner.port

    def stop(self, kill=False):
        if kill:
            self.inner.process.send_signal(signal.SIGKILL)
            self.inner.process.wait()
        else:
            self.inner.stop()

    def text(self):
        try:
            out, err = self.inner.process.communicate(timeout=5)
        except (subprocess.TimeoutExpired, ValueError):
            return ""
        return (out or b"").decode("utf-8", "replace") + (err or b"").decode("utf-8", "replace")

    def probe(self, method, path, body=b""):
        lines = [f"{method} {path} HTTP/1.1", "Host: localhost", "Connection: close",
                 f"Content-Length: {len(body)}", "Content-Type: application/json"]
        with socket.create_connection(("127.0.0.1", self.port), timeout=30) as sock:
            sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode() + body)
            data = bytearray()
            while True:
                chunk = sock.recv(1 << 20)
                if not chunk:
                    break
                data.extend(chunk)
        head, _, payload = bytes(data).partition(b"\r\n\r\n")
        return int(head.split(b" ")[1]), payload

    def state(self, keys, logos=()):
        body = json.dumps({"keys": list(keys), "logos": list(logos)}).encode()
        status, payload = self.probe("POST", "/probe/test-discovery-state", body)
        return json.loads(payload) if status == 200 else None


# google.protobuf.Value number_value (field 2, fixed64) of -0 and of 0.
NEGATIVE_ZERO = "11" + "0000000000000080"
ZERO = "11" + "0000000000000000"


def negative_zero_l1(record, status, payload):
    """negative_zero_l1: a provider -0 is protobuf number_value -0 in
    TypeScript only while its in-process L1 holds the parsed value (≤ 30 s);
    natively, and in TypeScript after the L1, the stored JSON text makes it
    0. True when the answer equals TypeScript's with every -0 number_value
    read as 0 (and differs from it)."""
    if "wireHex" not in record or NEGATIVE_ZERO not in record["wireHex"]:
        return False
    want = dict(record, wireHex=record["wireHex"].replace(NEGATIVE_ZERO, ZERO))
    return expected_body(want) == payload and status == record["status"]


def grace_for(calls, base=0.25):
    """The settle grace of a case: long enough for its slowest recorded
    provider reply to land and be stored."""
    return max([base] + [call.get("delayMs", 25) / 1000 + 0.2 for call in calls])


def settle(upstream, before, expected, grace=0.25, timeout=12):
    deadline = time.time() + timeout
    while time.time() < deadline and len(upstream.calls()) < before + expected:
        time.sleep(.02)
    time.sleep(grace)
    return upstream.calls()[before:]


def last_writes(writes):
    """The final TypeScript envelope per key of a case's KV writes."""
    final = collections.OrderedDict()
    for write in writes:
        final[write["key"]] = write
    return final


def compare_state(label, server, writes, rename=lambda key: key):
    """Every key TypeScript wrote in the case holds TypeScript's final
    envelope natively: asOf, freshUntil, staleUntil and the value's JSON."""
    final = last_writes(writes)
    if not final:
        return True
    state = server.state([rename(key) for key in final])
    if not check(state is not None, f"{label}: state probe failed"):
        return False
    ok = True
    for (key, want), got in zip(final.items(), state["envelopes"]):
        envelope = got["envelope"]
        expected = {"asOf": want["asOf"], "freshUntil": want["freshUntil"], "staleUntil": want["staleUntil"],
                    "d": want["d"]}
        ok &= check(envelope == expected, f"{label}: durable {key}\n   expected {json.dumps(expected)[:600]}\n"
                                          f"   actual   {json.dumps(envelope)[:600]}")
    return ok


def production(binary, root, probes):
    """The production composition: the routes ride the price canary; unset
    or 0 → the router's 404; on, each probe's (status, body) is answered
    before any provider work. `probes`: [(label, headers, body, status,
    expected body or None)]."""
    root = Path(root)
    (root / "prod-tls").mkdir(exist_ok=True)
    ca, _, _, _ = tls_ca.create_certificates(root / "prod-tls", "DNS:localhost")
    ok = True

    def start(name, extra):
        kv_dir = root / name
        kv_dir.mkdir()
        port = harness.free_port()
        env = {k: v for k, v in os.environ.items()
               if not k.startswith("FORTIS_") and k not in ("ALCHEMY_API_KEY", "ATTESTATION_MODE", "NODE_ENV",
                                                            "TAOSTATS_API_KEY", "COINGECKO_API_KEY")}
        env.update({"FORTIS_KV_DIR": str(kv_dir), "PORT": str(port), "FORTIS_CA_BUNDLE_PATH": str(ca)}, **extra)
        process = subprocess.Popen([binary, str(port)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(400):
            if process.poll() is not None:
                raise RuntimeError(f"production server {name} exited")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.1):
                    return process, port
            except OSError:
                time.sleep(.03)
        raise RuntimeError("production server did not start")

    route, first = probes[0], probes[1:]
    for value in (None, "0"):
        process, port = start(f"kv-prod-{value}", {} if value is None else {"FORTIS_NATIVE_PRICE_ROUTE_CANARY": value})
        try:
            status, _, _ = raw(port, "POST", route, [("authorization", "Bearer " + token("A")),
                                                     ("content-type", "application/json")], b"{}")
            ok &= check(status == 404, f"production canary {value!r}: {status}, not 404")
        finally:
            process.terminate()
            process.wait(timeout=30)
    process, port = start("kv-prod-on", {"FORTIS_NATIVE_PRICE_ROUTE_CANARY": "1"})
    try:
        for label, headers, body, want_status, want_body in first:
            status, _, payload = raw(port, "POST", route, headers, body)
            ok &= check(status == want_status and (want_body is None or payload == want_body),
                        f"production {label}: {status} {payload[:160]!r}")
    finally:
        process.terminate()
        process.wait(timeout=30)
    print(f"  production: canary unset/0 answers 404; on, {len(first)} hook probes answered", flush=True)
    return ok


def drain_quota(server, route, letter, remaining_after, body=b'{"ids":[]}', max_requests=60,
                expected_cost=None):
    """Device `letter`'s wall-clock minute drained to `remaining_after` with
    requests that fail validation after the quota (no provider work)."""
    while True:
        if time.time() % 60 > 40:
            time.sleep(61 - time.time() % 60)
        minute = int(time.time() // 60)
        remaining = None
        for _ in range(max_requests):
            status, headers, _ = raw(server.port, "POST", route,
                                     [("authorization", "Bearer " + token(letter)),
                                      ("content-type", "application/json")], body)
            if expected_cost is not None and headers.get("x-rate-limit-cost") != str(expected_cost):
                raise AssertionError(f"quota setup cost {headers.get('x-rate-limit-cost')!r} != {expected_cost}")
            remaining = int(headers.get("x-rate-limit-remaining", "-1"))
            if remaining <= remaining_after:
                break
        if remaining == remaining_after:
            return
        if int(time.time() // 60) == minute:
            raise AssertionError(f"quota setup stopped at {remaining}, expected {remaining_after}")
        time.sleep(61 - time.time() % 60)


def summary(title):
    for (group, kind), count in sorted(COUNTS.items()):
        print(f"  {group:14} {kind:9} {count}")
    matched = sum(v for (g, k), v in COUNTS.items() if k == "matched")
    asserted = sum(v for (g, k), v in COUNTS.items() if k == "asserted")
    failed = sum(v for (g, k), v in COUNTS.items() if k == "failed")
    print(f"{title}: {matched} matched, {asserted} asserted differences, {failed} failed "
          f"({len(FAILURES)} failure messages)")
    if FAILURES:
        raise SystemExit(1)
