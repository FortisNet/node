#!/usr/bin/env python3
"""Shared harness for the discovery route replays.

It runs build/fortis_node_discovery_route_test (main.cpp built with
-DFORTIS_DISCOVERY_ROUTE_TEST_ONLY) against one generated-CA HTTPS upstream
that records every provider request, with the route clock in a file the test
moves between cases. The binary seeds one development-tier device token per
letter (fintel_dev_ + letter * 43); TokenRotation spreads requests over them
to stay inside the 240/min development quota.
"""
import hashlib
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import tls_ca
from quote_contract_expectations import retarget

FIXTURES = Path(__file__).resolve().parents[1] / "tests/corpus"
TOKEN_LETTERS = "ABCDEFGIJKLMNOPQRSTUVWXYZ"


def load_fixture(name):
    path = FIXTURES / name
    data = path.read_bytes()
    pinned = (FIXTURES / f"{name}.sha256").read_text().strip()
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{name}: sha256 {digest} does not match the pinned {pinned}")
    return retarget(json.loads(data))


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Upstream:
    """Generated-CA HTTPS upstream; `respond(path, headers)` returns
    (status, headers, body bytes). Every request is recorded."""

    def __init__(self, root, respond):
        self.observed = []
        self.lock = threading.Lock()
        self.respond = respond
        ca, cert, key, _ = tls_ca.create_certificates(Path(root), "DNS:localhost")
        self.ca = ca
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                headers = {k.lower(): v for k, v in self.headers.items()}
                with upstream.lock:
                    upstream.observed.append((self.path, headers))
                status, extra, body = upstream.respond(self.path, headers)
                self.send_response(status)
                for name, value in extra.items():
                    self.send_header(name, value)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                try:
                    self.wfile.write(body)
                    self.wfile.flush()
                except OSError:
                    pass
                self.close_connection = True

            def log_message(self, *_args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", free_port()), Handler)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        self.server.socket = tls.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_address[1]

    def calls(self):
        with self.lock:
            return list(self.observed)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


class Server:
    """One discovery test process on a KV directory (kept across restarts)."""

    def __init__(self, binary, kv_dir, clock_file, upstream=None, env=None):
        self.port = free_port()
        environment = dict(os.environ)
        for name in ("TAOSTATS_API_KEY", "COINGECKO_API_KEY", "COINGECKO_TIER", "JUPITER_API_KEY",
                     "FORTIS_NATIVE_PRICE_ROUTE_CANARY"):
            environment.pop(name, None)
        environment.update({
            "FORTIS_ENABLE_TEST_DISCOVERY_ROUTES": "1",
            "FORTIS_KV_DIR": str(kv_dir),
            "FORTIS_TEST_DISCOVERY_NOW_FILE": str(clock_file),
            "PORT": str(self.port),
        })
        if upstream is not None:
            environment["FORTIS_TEST_DISCOVERY_UPSTREAM_PORT"] = str(upstream.port)
            environment["FORTIS_TEST_DISCOVERY_UPSTREAM_CA"] = str(upstream.ca)
        environment.update(env or {})
        self.process = subprocess.Popen([binary, str(self.port)], env=environment,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        for _ in range(200):
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
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()

    def request(self, method, path, *, body=b"", headers=None, timeout=40):
        lines = [f"{method} {path} HTTP/1.1", "Host: localhost", "Connection: close"]
        for key, value in (headers or {}).items():
            lines.append(f"{key}: {value}")
        if body or method == "POST":
            lines.append(f"Content-Length: {len(body)}")
        with socket.create_connection(("127.0.0.1", self.port), timeout=timeout) as sock:
            sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode() + body)
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


class TokenRotation:
    """Spreads requests over the seeded development tokens: at most
    `per_token` requests each, so no token exceeds its per-minute quota."""

    def __init__(self, per_token):
        self.per_token = per_token
        self.count = 0

    def next(self):
        letter = TOKEN_LETTERS[(self.count // self.per_token) % len(TOKEN_LETTERS)]
        self.count += 1
        return "fintel_dev_" + letter * 43


def set_clock(clock_file, epoch_ms):
    """Atomically: the binary reads the file on every clock call (the KV
    sweeper too, every 10 s), and an empty file mid-rewrite reads as "no
    test clock", the wall clock — under which every fixture-dated entry is
    long expired and swept. A rename replaces it whole."""
    path = Path(clock_file)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(str(int(epoch_ms)))
    os.replace(temporary, path)


def wait_for_calls(upstream, count, timeout=8.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if len(upstream.calls()) >= count:
            return True
        time.sleep(.02)
    return len(upstream.calls()) >= count
