#!/usr/bin/env python3
"""Replays the DashPay route contract fixture against the native test binary
(build/fortis_node_dashpay_route_test, main.cpp built with
-DFORTIS_DASHPAY_ROUTE_TEST_ONLY and the chain transport's test hooks).

tests/corpus/dashpay_route.json was produced by
the earlier TypeScript implementation's tests over the real Fastify
app and EvoDashPayNameResolver with a scenario-driven Evo SDK stub. Here the
native service runs its real client and proof verifier against one
generated-CA HTTPS upstream that stands in for the trusted quorum service and
every evonode (the transport names the real host in x-fortis-test-host):

  verified         the captured mainnet reply for the requested label
                   (tests/corpus/dashpay_upstream.json), under a
                   freshness clock at the capture time
  proof-failure    the same reply with one byte of its proof flipped
  timeout          an evonode that never answers (the group shortens the
                   native 15 s lookup bound to 3 s; the default bound is
                   checked by tests/dashpay_native_test.py)
  malformed        the verified reply, and the test hook rewriting the
                   verified identity (FORTIS_TEST_DASHPAY_IDENTITY_FILE)
  connect-failure  the quorum service's /quorums answering 500

Per case the replay compares the status, the recorded response headers
(x-rate-limit-remaining is not recorded: native minute buckets use the wall
clock), the exact JSON text or protobuf bytes, and `connects`: the TypeScript
SDK instances the group has created against the native /masternodes fetches
(one per connection). Each fixture group is a fresh process.
Usage: dashpay_route_test.py build/fortis_node_dashpay_route_test
"""
import base64
import collections
import hashlib
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import tls_ca

FIXTURES = Path(__file__).resolve().parents[1] / "tests/corpus"
RESPONSE_HEADERS = ["content-type", "vary", "cache-control", "x-content-type-options", "referrer-policy",
                    "permissions-policy", "x-rate-limit-cost", "retry-after"]
QUORUM_HOST = "quorums.mainnet.networks.dash.org"
GET_DOCUMENTS = "/org.dash.platform.dapi.v0.Platform/getDocuments"
GROUP_ENV = {"timeout": {"FORTIS_TEST_DASHPAY_LOOKUP_MS": "3000", "FORTIS_TEST_DASHPAY_REQUEST_MS": "2000"}}


def load(name):
    data = (FIXTURES / name).read_bytes()
    pinned = (FIXTURES / f"{name}.sha256").read_text().split()[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{name}: sha256 {digest} does not match the pinned {pinned}")
    return json.loads(data)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def read_varint(b, i):
    shift = value = 0
    while True:
        c = b[i]
        i += 1
        value |= (c & 0x7F) << shift
        shift += 7
        if not c & 0x80:
            return value, i


def fields(b):
    out, i = {}, 0
    while i < len(b):
        key, i = read_varint(b, i)
        number, wire = key >> 3, key & 7
        if wire == 0:
            value, i = read_varint(b, i)
        elif wire == 2:
            length, i = read_varint(b, i)
            value, i = b[i:i + length], i + length
        else:
            raise ValueError(wire)
        out.setdefault(number, []).append(value)
    return out


def requested_label(body):
    """The normalizedLabel of a gRPC-web GetDocumentsRequest V1."""
    message = body[5:5 + int.from_bytes(body[1:5], "big")]
    v1 = fields(fields(message)[2][0])
    for clause in v1[3]:
        parsed = fields(clause)
        if parsed[1][0] == b"normalizedLabel":
            return fields(parsed[3][0])[5][0].decode()
    raise ValueError("no normalizedLabel clause")


def corrupt(reply):
    data = bytearray(reply)
    data[len(data) // 2] ^= 0x01
    return bytes(data)


class Upstream:
    """The trusted quorum service and every evonode."""

    def __init__(self, root, upstream):
        self.lock = threading.Lock()
        self.fixture = upstream
        self.scenario = "verified"
        self.replies = {k: base64.b64decode(v) for k, v in upstream["replies"].items()}
        self.override = {}  # label -> (status, content type, body) or "stall"
        self.trusted_sequence = {}  # path -> texts served in order (the last repeats)
        self.trusted_served = collections.Counter()
        self.masternodes = 0
        self.observed = []
        self.stall_s = 20
        ca, cert, key, _ = tls_ca.create_certificates(Path(root), "DNS:localhost")
        self.ca = ca
        upstream_self = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def reply(self, status, content_type, payload):
                self.send_response(status)
                self.send_header("content-type", content_type)
                self.send_header("Content-Length", str(len(payload)))
                self.send_header("Connection", "close")
                self.end_headers()
                try:
                    self.wfile.write(payload)
                    self.wfile.flush()
                except OSError:
                    pass
                self.close_connection = True

            def do_GET(self):
                host = self.headers.get("x-fortis-test-host", "")
                with upstream_self.lock:
                    upstream_self.observed.append(("GET", host, self.path, None))
                    if host == QUORUM_HOST and self.path == "/masternodes":
                        upstream_self.masternodes += 1
                    scenario = upstream_self.scenario
                    text = upstream_self.fixture["trusted"].get(self.path)
                    sequence = upstream_self.trusted_sequence.get(self.path)
                    if sequence:
                        served = upstream_self.trusted_served[self.path]
                        upstream_self.trusted_served[self.path] += 1
                        text = sequence[min(served, len(sequence) - 1)]
                if host != QUORUM_HOST or text is None:
                    return self.reply(404, "application/json", b'{"error":"unexpected"}')
                if scenario == "connect-failure" and self.path == "/quorums":
                    return self.reply(500, "application/json", b'{"success":false}')
                self.reply(200, "application/json", text.encode())

            def do_POST(self):
                length = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(length) if length else b""
                host = self.headers.get("x-fortis-test-host", "")
                try:
                    label = requested_label(body)
                except (ValueError, KeyError, IndexError):
                    label = None
                with upstream_self.lock:
                    upstream_self.observed.append(("POST", host, self.path, label))
                    scenario = upstream_self.scenario
                    override = upstream_self.override.get(label)
                    reply = upstream_self.replies.get(label)
                if self.path != GET_DOCUMENTS or host == QUORUM_HOST or reply is None:
                    return self.reply(404, "application/json", b'{"error":"unexpected"}')
                if self.headers.get("content-type") != "application/grpc-web+proto":
                    return self.reply(415, "text/plain", b"unsupported media type")
                if override == "stall" or scenario == "timeout":
                    time.sleep(upstream_self.stall_s)
                    self.close_connection = True
                    return
                if override is not None:
                    return self.reply(*override)
                if scenario == "proof-failure":
                    reply = corrupt(reply)
                self.reply(200, "application/grpc-web+proto", reply)

            def log_message(self, *_args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", free_port()), Handler)
        self.server.daemon_threads = True
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_address[1]

    def set(self, scenario):
        with self.lock:
            self.scenario = scenario

    def reset(self):
        with self.lock:
            self.scenario = "verified"
            self.override = {}
            self.trusted_sequence = {}
            self.trusted_served = collections.Counter()
            self.masternodes = 0
            self.observed = []

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class Server:
    def __init__(self, binary, upstream, kv_dir, clock_file, identity_file, env=None, canary=True):
        self.port = free_port()
        environment = {k: v for k, v in os.environ.items() if not k.startswith("FORTIS_")}
        environment.update({
            "FORTIS_CA_BUNDLE_PATH": str(upstream.ca),
            "FORTIS_KV_DIR": str(kv_dir),
            "PORT": str(self.port),
        })
        if canary:
            environment.update({
                "FORTIS_NATIVE_DASHPAY_CANARY": "1",
                "FORTIS_ENABLE_TEST_DASHPAY_ROUTES": "1",
                "FORTIS_TEST_DASHPAY_UPSTREAM_PORT": str(upstream.port),
                "FORTIS_TEST_DASHPAY_UPSTREAM_CA": str(upstream.ca),
                "FORTIS_TEST_DASHPAY_NOW_FILE": str(clock_file),
                "FORTIS_TEST_DASHPAY_IDENTITY_FILE": str(identity_file),
            })
        environment.update(env or {})
        self.process = subprocess.Popen([binary, str(self.port)], env=environment,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        for _ in range(300):
            if self.process.poll() is not None:
                raise RuntimeError("test server exited: " + self.process.stderr.read().decode())
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=.1):
                    return
            except OSError:
                time.sleep(.03)
        raise RuntimeError("test server did not start")

    def stop(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        return self.process.stdout.read() + self.process.stderr.read()

    def request(self, method, path, headers, timeout=60):
        lines = [f"{method} {path} HTTP/1.1", "Host: localhost", "Connection: close"]
        for key, value in headers.items():
            lines.append(f"{key}: {value}")
        with socket.create_connection(("127.0.0.1", self.port), timeout=timeout) as sock:
            sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
            data = bytearray()
            while True:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                data.extend(chunk)
        head, _, payload = bytes(data).partition(b"\r\n\r\n")
        first, *header_lines = head.decode("latin1").split("\r\n")
        status = int(first.split()[1])
        response_headers = {}
        for line in header_lines:
            if ":" in line:
                key, value = line.split(":", 1)
                response_headers[key.lower()] = value.strip()
        return status, response_headers, payload


def compare(case, response, connects):
    status, headers, payload = response
    problems = []
    if status != case["status"]:
        problems.append(f"status {status} != {case['status']}")
    for name in RESPONSE_HEADERS:
        if headers.get(name) != case["headers"].get(name):
            problems.append(f"header {name}: {headers.get(name)!r} != {case['headers'].get(name)!r}")
    if "wireHex" in case:
        if payload.hex() != case["wireHex"]:
            problems.append(f"protobuf {payload.hex()[:120]} != {case['wireHex'][:120]}")
    elif payload.decode("utf-8", "replace") != case["responseText"]:
        problems.append(f"body {payload[:160]!r} != {case['responseText'][:160]!r}")
    if connects != case["connects"]:
        problems.append(f"connections {connects} != TypeScript SDK instances {case['connects']}")
    return problems


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = load("dashpay_route.json")
    upstream_fixture = load("dashpay_upstream.json")
    failures = []
    matched = 0
    with tempfile.TemporaryDirectory(prefix="dashpay-replay-") as root:
        root = Path(root)
        (root / "tls").mkdir()
        upstream = Upstream(root / "tls", upstream_fixture)
        clock = root / "clock"
        identity = root / "identity"
        clock.write_text(str(upstream_fixture["clockMs"]))
        server = None
        group = None
        try:
            for case in fixture["cases"]:
                if case["group"] != group:
                    if server:
                        output = server.stop()
                        if b"Bt5vvxCx1bHJuKfP9gX2uMx6JZhuQBjgLYR8hXG2FjF" in output:
                            failures.append(f"{group}: an identity reached process output")
                    group = case["group"]
                    upstream.reset()
                    kv = root / f"kv-{group}"
                    kv.mkdir()
                    server = Server(binary, upstream, kv, clock, identity, GROUP_ENV.get(group))
                upstream.set(case["scenario"] or "verified")
                identity.write_text("not-a-platform-id" if case["scenario"] == "malformed" else "")
                headers = dict(case["requestHeaders"])
                if case["device"] and "authorization" not in headers:
                    headers["authorization"] = "Bearer fintel_dev_" + case["device"] * 43
                response = server.request(case["method"], case["path"], headers)
                with upstream.lock:
                    connects = upstream.masternodes
                expected = case
                if case["name"] == "invalid-repeated":
                    # query-string: repeated name takes the first value.
                    expected = next(c for c in fixture["cases"] if c["name"] == "found-label")
                problems = compare(expected, response, connects)
                if problems:
                    failures.append(f"{case['group']}/{case['name']}: " + "; ".join(problems))
                else:
                    matched += 1
        finally:
            if server:
                server.stop()
            upstream.close()
    print(f"dashpay replay: {matched}/{len(fixture['cases'])} cases verified (including task 17 query changes)")
    for failure in failures:
        print("FAIL", failure)
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
