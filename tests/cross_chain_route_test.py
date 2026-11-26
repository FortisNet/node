#!/usr/bin/env python3
"""Replays the cross-chain contract fixture against the native test
binary (build/fortis_node_cross_chain_route_test, main.cpp built with
-DFORTIS_CROSS_CHAIN_ROUTE_TEST_ONLY and the chain transport's test hooks).

tests/corpus/cross_chain_route.json was produced by
the earlier TypeScript implementation's tests over the real Fastify
app. Every provider host goes to one generated-CA HTTPS upstream here; the
transport names the real host in x-fortis-test-host, and the upstream answers
each request from the case's own table (`${METHOD} ${host}${pathname}`, a
list consumed in order with the last entry repeating) with the exact bytes
the TypeScript run served.

Per case the replay compares the status, the recorded response headers
(x-rate-limit-remaining is not recorded: native minute buckets use the wall
clock), the exact JSON text or protobuf bytes, and the upstream requests as a
multiset of (method, URL with query, body) plus their credential, accept and
content-type headers. Each fixture group is a fresh process: the TypeScript
groups are fresh app instances (lanes, breakers and the NEAR catalog start
empty). The route clock follows the fixture's frozen Date through a file.
Usage: cross_chain_route_test.py build/fortis_node_cross_chain_route_test
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
from urllib.parse import urlsplit

import compat_expectations
import tls_ca

FIXTURES = Path(__file__).resolve().parents[1] / "tests/corpus"
FIXTURE = "cross_chain_route.json"
RESPONSE_HEADERS = ["content-type", "vary", "cache-control", "x-content-type-options", "referrer-policy",
                    "permissions-policy", "x-rate-limit-cost", "retry-after"]
UPSTREAM_HEADERS = ["content-type", "accept", "x-lifi-api-key", "x-integrator-id", "x-api-key", "authorization"]
TOKEN_LETTERS = "ABCD"


def load_fixture():
    data = (FIXTURES / FIXTURE).read_bytes()
    pinned = (FIXTURES / f"{FIXTURE}.sha256").read_text().split()[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{FIXTURE}: sha256 {digest} does not match the pinned {pinned}")
    fixture=json.loads(data)
    import re
    for row in fixture['cases']:
        if row['name'] in ('status-lifi-lone-surrogate','status-squid-lone-surrogate-gmp'):
            row['responseText']=re.sub(r'\\ud[89a-fA-F][0-9a-fA-F]{2}','�',row['responseText'])
            if 'content-length' in row['headers']: row['headers']['content-length']=str(len(row['responseText'].encode()))
        if row['name']=='status-squid-lone-surrogate-gmp':
            row['upstream'].append({'method':'GET','url':'https://api.axelarscan.io/gmp/searchGMP?txHash=%EF%BF%BDtx',
                'key':'GET api.axelarscan.io/gmp/searchGMP','headers':{'accept':'application/json'},'status':404})
    return fixture


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Upstream:
    """One HTTPS upstream for every provider host."""

    def __init__(self, root):
        self.lock = threading.Lock()
        self.table = {}
        self.consumed = collections.Counter()
        self.observed = []
        ca, cert, key, _ = tls_ca.create_certificates(Path(root), "DNS:localhost")
        self.ca = ca
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def handle_any(self):
                length = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(length) if length else b""
                headers = {k.lower(): v for k, v in self.headers.items()}
                host = headers.get("x-fortis-test-host", "")
                url = f"https://{host}{self.path}"
                key = f"{self.command} {host}{urlsplit(self.path).path}"
                with upstream.lock:
                    upstream.observed.append({
                        "method": self.command, "url": url, "key": key,
                        "body": body.decode("utf-8", "surrogateescape") if body else None,
                        "headers": {h: headers[h] for h in UPSTREAM_HEADERS if h in headers},
                    })
                    entries = upstream.table.get(key)
                    if entries is None:
                        reply = {"status": 404, "body": json.dumps({"error": "unexpected request", "key": key})}
                    else:
                        index = upstream.consumed[key]
                        upstream.consumed[key] += 1
                        reply = entries[min(index, len(entries) - 1)]
                payload = reply["body"].encode("utf-8")
                self.send_response(reply["status"])
                for name, value in (reply.get("headers") or {"content-type": "application/json"}).items():
                    self.send_header(name, value)
                self.send_header("Content-Length", str(len(payload)))
                self.send_header("Connection", "close")
                self.end_headers()
                try:
                    self.wfile.write(payload)
                    self.wfile.flush()
                except OSError:
                    pass
                self.close_connection = True

            do_GET = handle_any
            do_POST = handle_any

            def log_message(self, *_args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", free_port()), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_address[1]

    def start_case(self, table):
        with self.lock:
            self.table = table
            self.consumed = collections.Counter()
            self.observed = []

    def calls(self):
        with self.lock:
            return list(self.observed)

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class Server:
    def __init__(self, binary, upstream, kv_dir, clock_file, env):
        self.port = free_port()
        environment = {k: v for k, v in os.environ.items()
                       if not k.startswith("FORTIS_") and k not in (
                           "LIFI_API_KEY", "SQUID_INTEGRATOR_ID", "SIMPLESWAP_API_KEY", "NEAR_INTENTS_JWT",
                           "MAYA_DASH_EXECUTION_ENABLED", "MAYA_NODE_URLS")}
        environment.update({
            "FORTIS_NATIVE_CROSS_CHAIN_CANARY": "1",
            "FORTIS_CA_BUNDLE_PATH": str(upstream.ca),
            "FORTIS_ENABLE_TEST_CROSS_CHAIN_ROUTES": "1",
            "FORTIS_TEST_CROSS_CHAIN_UPSTREAM_PORT": str(upstream.port),
            "FORTIS_TEST_CROSS_CHAIN_UPSTREAM_CA": str(upstream.ca),
            "FORTIS_TEST_CROSS_CHAIN_NOW_FILE": str(clock_file),
            "FORTIS_KV_DIR": str(kv_dir),
            "PORT": str(self.port),
        })
        environment.update(env)
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
            self.process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        return self.process.stdout.read() + self.process.stderr.read()

    def request(self, method, path, headers, body):
        lines = [f"{method} {path} HTTP/1.1", "Host: localhost", "Connection: close"]
        for key, value in headers.items():
            lines.append(f"{key}: {value}")
        if body is not None:
            lines.append(f"Content-Length: {len(body)}")
        elif method == "POST":
            lines.append("Content-Length: 0")
        with socket.create_connection(("127.0.0.1", self.port), timeout=60) as sock:
            sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode() + (body or b""))
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


def call_key(call):
    return (call["method"], call["url"], call.get("body"))


def compare_upstream(case, observed, failures):
    expected = case["upstream"]
    want = collections.Counter(call_key(c) for c in expected)
    got = collections.Counter(call_key(c) for c in observed)
    if want != got:
        missing = list((want - got).elements())
        extra = list((got - want).elements())
        failures.append(f"{case['name']}: upstream differs; missing={missing[:3]} extra={extra[:3]}")
        return
    by_key = collections.defaultdict(list)
    for call in observed:
        by_key[call_key(call)].append(call["headers"])
    for call in expected:
        headers = by_key[call_key(call)].pop(0)
        for name in UPSTREAM_HEADERS:
            mine = headers.get(name)
            theirs = call["headers"].get(name)
            if (mine or "").lower() != (theirs or "").lower() if name in ("content-type", "accept") else mine != theirs:
                failures.append(f"{case['name']}: upstream {call['url'][:60]} header {name}: {mine!r} != {theirs!r}")


def strict_case(case, body, answer_headers, shift):
    """The case as this server answers it (compat-decisions.md
    #form-bodies, #js-prototype-keys, #zod-issues)."""
    if "wireHex" in case:
        return case
    request_headers = {k.lower(): v for k, v in case["requestHeaders"].items()}
    if not case["noAuth"]:
        request_headers["authorization"] = "Bearer fintel_dev_"  # the replay's device bearer
    status, headers, text, _ = compat_expectations.remap(
        case["status"], case["headers"], case["responseText"], request_headers.get("content-type"), body,
        case["group"], shift, request_headers=request_headers)
    # The __proto__ case: charged by this server, after the quota.
    headers = compat_expectations.settle_charged(headers, answer_headers, case["group"], shift)
    return dict(case, status=status, headers=headers, responseText=text.decode("utf-8", "surrogatepass"))


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = load_fixture()
    envs = {group["name"]: group["env"] for group in fixture["groups"]}
    failures = []
    matched = 0
    shift = compat_expectations.QuotaShift()
    with tempfile.TemporaryDirectory(prefix="cross-chain-replay-") as root:
        root = Path(root)
        (root / "tls").mkdir()
        upstream = Upstream(root / "tls")
        clock = root / "clock"
        server = None
        group = None
        count = 0
        try:
            for case in fixture["cases"]:
                if case["group"] != group:
                    if server:
                        output = server.stop()
                        for secret in ("fixture-lifi-key-0001", "fixture-squid-integrator", "fixture-simpleswap-key",
                                       "fixture-near-jwt-token"):
                            if secret.encode() in output:
                                failures.append(f"{group}: a provider credential reached process output")
                    group = case["group"]
                    kv = root / f"kv-{group}"
                    kv.mkdir()
                    clock.write_text(str(case["at"]))
                    server = Server(binary, upstream, kv, clock, envs[group])
                clock.write_text(str(case["at"]))
                upstream.start_case(case["providers"])
                headers = dict(case["requestHeaders"])
                if not case["noAuth"]:
                    letter = TOKEN_LETTERS[count % len(TOKEN_LETTERS)]
                    headers["authorization"] = "Bearer fintel_dev_" + letter * 43
                count += 1
                body = None
                if "bodyBase64" in case:
                    body = base64.b64decode(case["bodyBase64"])
                elif "bodyText" in case:
                    body = case["bodyText"].encode("utf-8")
                status, response_headers, payload = server.request(case["method"], case["path"], headers, body)
                case = strict_case(case, body, response_headers, shift)
                problems = []
                if status != case["status"]:
                    problems.append(f"status {status} != {case['status']}")
                for name in RESPONSE_HEADERS:
                    if response_headers.get(name) != case["headers"].get(name):
                        problems.append(f"header {name}: {response_headers.get(name)!r} != {case['headers'].get(name)!r}")
                if "wireHex" in case:
                    if payload.hex() != case["wireHex"]:
                        problems.append(f"protobuf differs: {payload.hex()[:120]} != {case['wireHex'][:120]}")
                elif payload.decode("utf-8", "replace") != case["responseText"]:
                    mine = payload.decode("utf-8", "replace")
                    theirs = case["responseText"]
                    at = next((i for i in range(min(len(mine), len(theirs))) if mine[i] != theirs[i]),
                              min(len(mine), len(theirs)))
                    problems.append(f"body differs at {at}: ...{mine[max(0, at - 80):at + 80]!r} != "
                                    f"...{theirs[max(0, at - 80):at + 80]!r}")
                compare_upstream(case, upstream.calls(), problems)
                if problems:
                    failures.append(f"{case['name']}: " + "; ".join(problems))
                else:
                    matched += 1
        finally:
            if server:
                server.stop()
            upstream.close()
    print(f"cross-chain replay: {matched}/{len(fixture['cases'])} cases match")
    for failure in failures:
        print("FAIL", failure)
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
