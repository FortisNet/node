#!/usr/bin/env python3
"""Real-socket edge checks of GET /v1/dashpay/names/resolve (R05) against
the native test binary (build/fortis_node_dashpay_route_test), with
the TypeScript oracle tests/corpus/dashpay_resolve_edges.json
(recorded by the earlier TypeScript implementation's tests: the same
raw request bytes sent to the real Fastify app over real sockets).

The native service runs its real client and proof verifier against the
captured mainnet upstream of tests/dashpay_route_test.py. Blocks:
  query       every oracle case, byte for byte (status, the recorded
              headers incl. content-length, JSON text / protobuf wire);
              a 200 whose name has no captured reply is left to
              tests/dashpay_resolve_test.cpp (it would be 503 here);
              documented differences are asserted as native behaviour
  concurrent  six verified lookups at once: the TypeScript bodies
  quota       device D drained to remaining 0 with invalid names, then the
              429s (query never jumps ahead of the quota) and another device
  identity    every oracle identifier forced through the identity hook:
              200 with it when validDashPlatformIdentifier, else 503
  transport   evonode HTTP 500, wrong content type, empty body, stall
              (3 s bound) → 503 JSON even when protobuf is accepted; then
              the next lookup is verified again
  cancel      a client that disconnects during a stalled lookup: the job
              still ends, the server keeps answering, then idles
  restart     a restart on the same KV directory: identical bytes, a new
              trusted connection, a fresh quota minute (TypeScript quotas
              are process-local)
  privacy     no DashPay name or identity in process output
Usage: dashpay_resolve_route_test.py build/fortis_node_dashpay_route_test
"""
import json
from pathlib import Path
import socket
import sys
import tempfile
import threading
import time

import dashpay_route_test as replay

HEADERS = ["content-type", "content-length", "vary", "cache-control", "x-content-type-options", "referrer-policy",
           "permissions-policy", "x-rate-limit-cost", "retry-after"]
PATH = "/v1/dashpay/names/resolve?name="
GRPC = "application/grpc-web+proto"
UNAVAILABLE = b'{"error":"DASHPAY_UNAVAILABLE"}'
INVALID = b'{"error":"INVALID_DASHPAY_NAME"}'
SHORT_LOOKUP = {"FORTIS_TEST_DASHPAY_LOOKUP_MS": "3000", "FORTIS_TEST_DASHPAY_REQUEST_MS": "2000"}
# Native behaviour where TypeScript differs outside the handler (asserted).
ROUTER_LEVEL = {
    # Node's parser refuses raw bytes >= 0x80 (its own 400 body); the
    # handler answers the route's 400.
    "malformed-key-escape": (400, INVALID),
    "raw-nbsp": (400, INVALID),
    "raw-utf8-nbsp": (400, INVALID),
    "raw-latin1-letter": (400, INVALID),
    "raw-nel": (400, INVALID),
    # find-my-way splits the query at `#`; the native request line refuses `#`.
    "fragment-in-query": (400, None),
    "fragment-before-query": (400, None),
    # Node allows 16 KiB of headers; the native request line stops at 12 500 bytes.
    "target-13000": (400, None),
}


def raw_request(port, method, target, headers, timeout=60):
    lines = [f"{method} ".encode() + target + b" HTTP/1.1", b"Host: localhost", b"Connection: close"]
    lines += [f"{key}: {value}".encode("latin1") for key, value in headers.items()]
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall(b"\r\n".join(lines) + b"\r\n\r\n")
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


def auth(device):
    return {"authorization": "Bearer fintel_dev_" + device * 43} if device else {}


class Run:
    def __init__(self, binary, root, upstream, upstream_fixture):
        self.binary = binary
        self.root = root
        self.upstream = upstream
        self.fixture = upstream_fixture
        self.clock = root / "clock"
        self.identity = root / "identity"
        self.clock.write_text(str(upstream_fixture["clockMs"]))
        self.identity.write_text("")
        self.failures = []
        self.outputs = []
        self.count = 0

    def fail(self, message):
        self.failures.append(message)

    def server(self, env=None, kv=None):
        if kv is None:
            self.count += 1
            kv = self.root / f"kv-{self.count}"
            kv.mkdir()
        self.upstream.reset()
        return replay.Server(self.binary, self.upstream, kv, self.clock, self.identity, env), kv

    def stop(self, server):
        self.outputs.append(server.stop())

    def send(self, server, case):
        headers = dict(case["requestHeaders"])
        if case["device"] and "authorization" not in headers:
            headers.update(auth(case["device"]))
        return raw_request(server.port, case["method"], bytes.fromhex(case["targetHex"]), headers)

    def compare(self, label, case, response, remaining=False):
        status, headers, payload = response
        problems = []
        if status != case["status"]:
            problems.append(f"status {status} != {case['status']}")
        names = HEADERS + (["x-rate-limit-remaining"] if remaining else [])
        for name in names:
            if headers.get(name) != case["headers"].get(name):
                problems.append(f"{name}: {headers.get(name)!r} != {case['headers'].get(name)!r}")
        if "wireHex" in case:
            if payload.hex() != case["wireHex"]:
                problems.append(f"wire {payload.hex()[:80]} != {case['wireHex'][:80]}")
        elif payload.decode("utf-8", "replace") != case["responseText"]:
            problems.append(f"body {payload[:100]!r} != {case['responseText'][:100]!r}")
        if problems:
            self.fail(f"{label}/{case['name']}: " + "; ".join(problems))
            return False
        return True


def query_block(run, cases):
    captured = set(run.upstream.replies)
    server, _ = run.server()
    matched = skipped = differences = 0
    try:
        for case in cases:
            if case["group"] != "query":
                continue
            asked = case["resolverAsked"]
            if asked and asked[0][:-5] not in captured:
                skipped += 1
                continue
            response = run.send(server, case)
            if case["name"] in ROUTER_LEVEL:
                status, body = ROUTER_LEVEL[case["name"]]
                if response[0] != status or (body is not None and response[2] != body):
                    run.fail(f"query/{case['name']}: native {response[0]} {response[2][:80]!r}, "
                             f"documented {status} {body!r}")
                differences += 1
                continue
            if case["name"] in ("repeated", "repeated-key-only", "repeated-encoded-key"):
                # These formerly failed schema validation. Use the immutable
                # successful dash response to pin the new first-value result.
                expected = next(c for c in cases if c["name"] == "escaped-label")
                differences += run.compare("query/first-value", expected, response)
                continue
            matched += run.compare("query", case, response)
    finally:
        run.stop(server)
    print(f"query: {matched} cases match TypeScript, {differences} documented differences asserted, "
          f"{skipped} uncaptured names left to the unit test")


def concurrent_block(run, cases):
    case = next(c for c in cases if c["group"] == "concurrent")
    server, _ = run.server()
    try:
        results = [None] * len(case["labels"])

        def one(index, label):
            results[index] = raw_request(server.port, "GET", (PATH + label).encode(), auth("B"))

        threads = [threading.Thread(target=one, args=(i, label)) for i, label in enumerate(case["labels"])]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(60)
        statuses = [r[0] if r else None for r in results]
        bodies = [r[2].decode() if r else None for r in results]
        if statuses != case["statuses"] or bodies != case["bodies"]:
            run.fail(f"concurrent: {statuses} {bodies}")
        with run.upstream.lock:
            connects = run.upstream.masternodes
        # TypeScript runs all six against the SDK at once (peak 6); native
        # runs two at a time and shares one trusted connection.
        if connects != 1:
            run.fail(f"concurrent: {connects} trusted connections")
        print(f"concurrent: six lookups match (TypeScript resolver peak {case['resolverPeak']}, "
              f"native workers 2, connections {connects})")
    finally:
        run.stop(server)


def quota_block(run, cases):
    quota = [c for c in cases if c["group"] == "quota"]
    for attempt in range(3):
        # The native minute bucket is wall-clock: start early in a minute so
        # the drain (~0.1 s) and the cases share it.
        if time.time() % 60 > 50:
            time.sleep(61 - time.time() % 60)
        server, _ = run.server()
        try:
            # Drain device D to remaining 1 with invalid names (cost 1 each);
            # retry if the minute still rolled over mid-drain.
            minute = int(time.time() // 60)
            remaining = None
            while remaining != 1:
                status, headers, _ = raw_request(server.port, "GET", (PATH + "ab").encode(), auth("D"))
                if status != 400:
                    run.fail(f"quota drain: {status}")
                    return
                remaining = int(headers["x-rate-limit-remaining"])
            if int(time.time() // 60) != minute or time.time() % 60 > 55:
                continue
            ok = all(run.compare("quota", case, run.send(server, case), remaining=True) for case in quota)
            print(f"quota: drained device D, {len(quota)} cases {'match' if ok else 'differ'} "
                  "(429 before the query; other devices unaffected)")
            return
        finally:
            run.stop(server)
    run.fail("quota: the minute rolled over on every attempt")


def identity_block(run, oracle):
    server, _ = run.server()
    try:
        for row in oracle["identifiers"]:
            run.identity.write_text(row["text"])
            status, headers, body = raw_request(server.port, "GET", (PATH + "dash").encode(), auth("A"))
            if not row["text"]:
                # An empty hook file leaves the verified identity.
                expected = (200, oracle["verified"]["dash.dash"])
            elif row["valid"]:
                expected = (200, row["text"])
            else:
                expected = (503, None)
            if status != expected[0]:
                run.fail(f"identity {row['text']!r}: {status} != {expected[0]}")
            elif status == 200 and json.loads(body) != {"name": "dash.dash", "identityId": expected[1]}:
                run.fail(f"identity {row['text']!r}: {body!r}")
            elif status == 503 and (body != UNAVAILABLE or "vary" in headers):
                run.fail(f"identity {row['text']!r}: 503 {headers} {body!r}")
            # A rejected identity is the route's own check: the connection stays.
        with run.upstream.lock:
            connects = run.upstream.masternodes
        if connects != 1:
            run.fail(f"identity: route-level rejections dropped the connection ({connects} connects)")
    finally:
        run.identity.write_text("")
        run.stop(server)
    print(f"identity: {len(oracle['identifiers'])} oracle identifiers (fail closed on invalid)")


def transport_block(run):
    server, _ = run.server(SHORT_LOOKUP)
    protobuf = {**auth("A"), "accept": "application/protobuf"}
    try:
        overrides = [
            ("http-500", (500, "text/plain", b"internal")),
            ("wrong-type", (200, "application/json", b"{}")),
            ("empty-body", (200, GRPC, b"")),
            ("stall", "stall"),
        ]
        run.upstream.stall_s = 6
        for name, override in overrides:
            run.upstream.override["alice"] = override
            started = time.monotonic()
            status, headers, body = raw_request(server.port, "GET", (PATH + "alice").encode(), protobuf)
            elapsed = time.monotonic() - started
            if status != 503 or body != UNAVAILABLE or headers.get("content-type") != "application/json; charset=utf-8":
                run.fail(f"transport {name}: {status} {headers.get('content-type')} {body!r}")
            if elapsed > 5.0:
                run.fail(f"transport {name}: answered after {elapsed:.1f}s (3 s bound)")
            run.upstream.override.pop("alice")
            status, headers, body = raw_request(server.port, "GET", (PATH + "alice").encode(), protobuf)
            if status != 200 or body != bytes.fromhex("0a0a616c6963652e64617368"):
                run.fail(f"transport {name} recovery: {status} {body!r}")
        print(f"transport: {len(overrides)} failures answered 503 JSON, each followed by a verified lookup")
    finally:
        run.upstream.stall_s = 20
        run.stop(server)


def cancel_block(run):
    server, _ = run.server(SHORT_LOOKUP)
    try:
        raw_request(server.port, "GET", (PATH + "x-y").encode(), auth("A"))  # connect first
        run.upstream.override["dash"] = "stall"
        run.upstream.stall_s = 6
        sockets = []
        for _ in range(3):
            sock = socket.create_connection(("127.0.0.1", server.port), timeout=10)
            sock.sendall(f"GET {PATH}dash HTTP/1.1\r\nHost: localhost\r\nauthorization: Bearer fintel_dev_{'A' * 43}"
                         "\r\n\r\n".encode())
            sockets.append(sock)
        time.sleep(0.5)
        for sock in sockets:
            sock.close()
        status, _, body = raw_request(server.port, "GET", (PATH + "alice").encode(), auth("A"))
        if status != 200 or json.loads(body)["identityId"] is not None:
            run.fail(f"cancel: lookup beside abandoned ones {status} {body!r}")
        status, _, body = raw_request(server.port, "GET", b"/probe/test-dashpay-idle", {})
        if status != 200:
            run.fail(f"cancel: the abandoned lookups never ended ({status} {body!r})")
        run.upstream.override.pop("dash")
        status, _, body = raw_request(server.port, "GET", (PATH + "dash").encode(), auth("A"))
        if status != 200:
            run.fail(f"cancel: after abandoned lookups {status} {body!r}")
        print("cancel: three abandoned lookups ended, the server kept answering")
    finally:
        run.upstream.stall_s = 20
        run.stop(server)


def restart_block(run):
    server, kv = run.server()
    try:
        first = [raw_request(server.port, "GET", (PATH + label).encode(), {**auth("A"), "accept": accept})
                 for label, accept in (("dash", "application/protobuf"), ("alice", "application/json"))]
        remaining_before = first[-1][1].get("x-rate-limit-remaining")
    finally:
        run.stop(server)
    with run.upstream.lock:
        before = run.upstream.masternodes
    server, _ = run.server(kv=kv)
    try:
        second = [raw_request(server.port, "GET", (PATH + label).encode(), {**auth("A"), "accept": accept})
                  for label, accept in (("dash", "application/protobuf"), ("alice", "application/json"))]
        with run.upstream.lock:
            after = run.upstream.masternodes
        if [(s, b) for s, _, b in first] != [(s, b) for s, _, b in second]:
            run.fail(f"restart: answers changed {first} {second}")
        if before != 1 or after != 1:
            run.fail(f"restart: trusted connections {before}/{after}, expected one per process")
        remaining_after = second[-1][1].get("x-rate-limit-remaining")
        # Two requests in a fresh minute (1199 if they straddle a minute);
        # a persisted bucket would be at most 1196.
        if remaining_after not in ("1198", "1199"):
            run.fail(f"restart: quota minute not fresh ({remaining_before} → {remaining_after})")
        print(f"restart: identical answers, one connection per process, quota {remaining_before} → {remaining_after}")
    finally:
        run.stop(server)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    oracle = replay.load("dashpay_resolve_edges.json")
    upstream_fixture = replay.load("dashpay_upstream.json")
    with tempfile.TemporaryDirectory(prefix="dashpay-resolve-") as root:
        root = Path(root)
        (root / "tls").mkdir()
        upstream = replay.Upstream(root / "tls", upstream_fixture)
        run = Run(binary, root, upstream, upstream_fixture)
        try:
            query_block(run, oracle["cases"])
            concurrent_block(run, oracle["cases"])
            quota_block(run, oracle["cases"])
            identity_block(run, oracle)
            transport_block(run)
            cancel_block(run)
            restart_block(run)
        finally:
            upstream.close()
        output = b"".join(run.outputs)
        secrets = [value.encode() for value in oracle["verified"].values()] + [b"dash.dash", b"alice.dash", b"x-y"]
        leaked = [secret for secret in secrets if secret in output]
        if leaked:
            run.fail(f"privacy: process output contains {leaked}")
        else:
            print("privacy: no DashPay name or identity in process output")
    for failure in run.failures:
        print("FAIL", failure)
    if run.failures:
        raise SystemExit(1)
    print("dashpay resolve route edges: all passed")


if __name__ == "__main__":
    main()
