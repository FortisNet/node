#!/usr/bin/env python3
"""Shared harness for the news and media route tests.

It runs build/fortis_node_news_route_test (main.cpp built with
-DFORTIS_NEWS_ROUTE_TEST_ONLY) against one loopback upstream that answers on
a generated-CA HTTPS port and a plaintext HTTP port. The test binary sends
every publisher feed, CoinGecko and media request there: feed and media
requests carry the URL's host in x-fortis-test-host; CoinGecko requests come
from the discovery owner's loopback start (Host: localhost). `Upstream.url()`
rebuilds the URL the TypeScript service would have fetched, so replies can be
looked up by it. The binary seeds development-tier device tokens
fintel_dev_ + letter * 43 for the letters in TOKEN_LETTERS.
"""
import base64
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

FIXTURES = Path(__file__).resolve().parents[1] / "tests/corpus"
TOKEN_LETTERS = "ABCDEFGHJKMNPQRSTVWXYZ"
MEDIA_DNS = "internal.example.com=10.0.0.5,mixed.example.com=93.184.216.34|127.0.0.1,*=93.184.216.34"
SIGNATURES = {
    "png": bytes([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    "gif": b"GIF89a",
    "jpeg": bytes([0xff, 0xd8, 0xff, 0xe0]),
    "webp": b"RIFF\0\0\0\0WEBP",
    "svg": b'<svg xmlns="http://www.w3.org/2000/svg">',
}


def load_fixture(name):
    path = FIXTURES / name
    data = path.read_bytes()
    pinned = (FIXTURES / f"{name}.sha256").read_text().strip()
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{name}: sha256 {digest} does not match the pinned {pinned}")
    return json.loads(data)


def image(kind, size, seed):
    """The earlier TypeScript tests' image(): signature, then
    (i * 131 + seed) & 255 filler."""
    body = bytearray((i * 131 + seed) & 255 for i in range(size))
    signature = SIGNATURES[kind]
    body[:len(signature)] = signature
    return bytes(body)


def body_bytes(bodies, digest):
    entry = bodies[digest]
    if "base64" in entry:
        return base64.b64decode(entry["base64"])
    recipe = entry["recipe"]
    return image(recipe["kind"], recipe["size"], recipe["seed"])


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Upstream:
    """Loopback HTTPS + HTTP upstream. `respond(url, headers)` returns
    (status, headers, body bytes); every request is recorded as its URL."""

    def __init__(self, root, respond, delay=None):
        self.observed = []
        self.lock = threading.Lock()
        self.respond = respond
        self.delay = delay
        ca, cert, key, _ = tls_ca.create_certificates(Path(root), "DNS:localhost")
        self.ca = ca
        upstream = self

        def handler(scheme):
            class Handler(BaseHTTPRequestHandler):
                protocol_version = "HTTP/1.1"

                def do_GET(self):
                    headers = {k.lower(): v for k, v in self.headers.items()}
                    url = upstream.url(scheme, self.path, headers)
                    with upstream.lock:
                        upstream.observed.append(url)
                    if upstream.delay:
                        upstream.delay(url)
                    status, extra, body = upstream.respond(url, headers)
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
            return Handler

        self.tls_server = ThreadingHTTPServer(("127.0.0.1", free_port()), handler("https"))
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        self.tls_server.socket = context.wrap_socket(self.tls_server.socket, server_side=True)
        self.plain_server = ThreadingHTTPServer(("127.0.0.1", free_port()), handler("http"))
        self.threads = [threading.Thread(target=server.serve_forever, daemon=True)
                        for server in (self.tls_server, self.plain_server)]
        for thread in self.threads:
            thread.start()
        self.port = self.tls_server.server_address[1]
        self.plain_port = self.plain_server.server_address[1]

    @staticmethod
    def url(scheme, path, headers):
        target = headers.get("x-fortis-test-host")
        if target is None:
            return "https://api.coingecko.com" + path
        host, _, port = target.rpartition(":")
        if (scheme, port) in (("https", "443"), ("http", "80")):
            return f"{scheme}://{host}{path}"
        return f"{scheme}://{host}:{port}{path}"

    def calls(self):
        with self.lock:
            return list(self.observed)

    def close(self):
        for server in (self.tls_server, self.plain_server):
            server.shutdown()
            server.server_close()
        for thread in self.threads:
            thread.join(timeout=2)


class Server:
    """One news test process on a KV directory (kept across restarts)."""

    def __init__(self, binary, kv_dir, clock_file, upstream, env=None):
        self.port = free_port()
        environment = dict(os.environ)
        for name in ("COINGECKO_API_KEY", "COINGECKO_TIER", "FORTIS_NATIVE_NEWS_CANARY",
                     "FORTIS_NATIVE_PRICE_ROUTE_CANARY", "FORTIS_CA_BUNDLE_PATH"):
            environment.pop(name, None)
        environment.update({
            "FORTIS_ENABLE_TEST_NEWS_ROUTES": "1",
            "FORTIS_KV_DIR": str(kv_dir),
            "FORTIS_TEST_NEWS_NOW_FILE": str(clock_file),
            "FORTIS_TEST_NEWS_ID_FILE": str(Path(kv_dir).parent / "news-ids"),
            "FORTIS_TEST_NEWS_UPSTREAM_PORT": str(upstream.port),
            "FORTIS_TEST_NEWS_UPSTREAM_PLAIN_PORT": str(upstream.plain_port),
            "FORTIS_TEST_NEWS_UPSTREAM_CA": str(upstream.ca),
            "FORTIS_TEST_MEDIA_DNS": MEDIA_DNS,
            "PORT": str(self.port),
        })
        environment.update(env or {})
        self.stderr_path = Path(kv_dir).parent / f"server-{self.port}.log"
        self.stderr = open(self.stderr_path, "wb")
        self.process = subprocess.Popen([binary, str(self.port)], env=environment,
                                        stdout=subprocess.DEVNULL, stderr=self.stderr)
        for _ in range(300):
            if self.process.poll() is not None:
                self.stderr.close()
                raise RuntimeError("test server exited: " + self.stderr_path.read_text())
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
        self.stderr.close()

    def kill(self):
        self.process.kill()
        self.process.wait()
        self.stderr.close()

    def log(self):
        return self.stderr_path.read_text(errors="replace")

    def request(self, method, path, *, headers=None, timeout=60):
        lines = [f"{method} {path} HTTP/1.1", "Host: localhost", "Connection: close"]
        for key, value in (headers or {}).items():
            lines.append(f"{key}: {value}")
        with socket.create_connection(("127.0.0.1", self.port), timeout=timeout) as sock:
            sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
            data = bytearray()
            while True:
                chunk = sock.recv(1 << 20)
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

    def probe(self, path):
        status, _, body = self.request("GET", path, timeout=600)
        if status != 200:
            raise RuntimeError(f"{path}: {status} {body[:200]!r}")
        return json.loads(body)


class TokenRotation:
    def __init__(self, per_token):
        self.per_token = per_token
        self.count = 0

    def next(self):
        letter = TOKEN_LETTERS[(self.count // self.per_token) % len(TOKEN_LETTERS)]
        self.count += 1
        return "fintel_dev_" + letter * 43


def set_clock(clock_file, epoch_ms):
    Path(clock_file).write_text(str(int(epoch_ms)))


def wait_for_calls(upstream, count, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if len(upstream.calls()) >= count:
            return True
        time.sleep(.02)
    return len(upstream.calls()) >= count
