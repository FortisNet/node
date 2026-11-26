#!/usr/bin/env python3
"""Test-only native /v1/prices route against the frozen TypeScript fixture."""
import json
from pathlib import Path
import socket
import select
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

import tls_ca

FIXTURE = Path(__file__).resolve().parents[1] / "tests/corpus/price_spot_route.json"
TOKEN = "fintel_dev_" + "P" * 43
FIXTURE_TIME = "2026-01-02T03:04:05.000Z"


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def request(port, path, *, method="GET", headers=None, disconnect=False):
    sock = socket.create_connection(("127.0.0.1", port), timeout=4)
    lines = [f"{method} {path} HTTP/1.1", "Host: localhost", "Connection: close"]
    for key, value in (headers or {}).items():
        lines.append(f"{key}: {value}")
    sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
    if disconnect:
        sock.close()
        return None
    data = bytearray()
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            break
        data.extend(chunk)
    sock.close()
    head, _, body = bytes(data).partition(b"\r\n\r\n")
    first, *header_lines = head.decode("latin1").split("\r\n")
    status = int(first.split()[1])
    response_headers = {}
    for line in header_lines:
        if ":" in line:
            key, value = line.split(":", 1)
            response_headers[key.lower()] = value.strip()
    return status, response_headers, body


def normalize_times(value):
    if isinstance(value, dict):
        return {k: (FIXTURE_TIME if k in ("asOf", "timestamp") and v else normalize_times(v))
                for k, v in value.items()}
    if isinstance(value, list):
        return [normalize_times(item) for item in value]
    return value


def wait_port(port, process):
    for _ in range(100):
        if process.poll() is not None:
            raise RuntimeError("test server exited: " + process.stderr.read().decode())
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=.1):
                return
        except OSError:
            time.sleep(.03)
    raise RuntimeError("test server did not start")


def test_runtime_gate(binary: str, root: Path) -> None:
    """The test binary itself still requires explicit route opt-in."""
    kv_dir = root / "gate-kv"
    kv_dir.mkdir()
    port = free_port()
    env = dict(__import__("os").environ)
    for name in ("FORTIS_ENABLE_TEST_PRICE_ROUTE", "FORTIS_TEST_PRICE_UPSTREAM_PORT",
                 "FORTIS_TEST_PRICE_UPSTREAM_CA", "FORTIS_TEST_PRICE_NOW_MS"):
        env.pop(name, None)
    env["FORTIS_KV_DIR"] = str(kv_dir)
    process = subprocess.Popen([binary, str(port)], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        wait_port(port, process)
        assert request(port, "/v1/prices?ids=ethereum")[0] == 404
    finally:
        process.terminate()
        process.wait(timeout=5)


def run(binary):
    fixture = json.loads(FIXTURE.read_text())
    with tempfile.TemporaryDirectory(prefix="native-price-route-") as temp:
        root = Path(temp)
        test_runtime_gate(binary, root)
        ca, cert, key, _ = tls_ca.create_certificates(root, "DNS:localhost")
        upstream_hits = []

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                parsed = urlsplit(self.path)
                values = parse_qs(parsed.query)
                ids = values.get("ids", [""])[0].split(",")
                fiat = values.get("vs_currencies", ["usd"])[0]
                upstream_hits.append((ids, fiat))
                if "slowcoin" in ids:
                    time.sleep(.25)
                if "dogecoin" in ids:
                    deadline = time.monotonic() + 2.0
                    while time.monotonic() < deadline:
                        readable, _, _ = select.select([self.connection], [], [], .05)
                        if readable:
                            try:
                                if self.connection.recv(1) == b"":
                                    self.close_connection = True
                                    return
                            except OSError:
                                self.close_connection = True
                                return
                quotes = {
                    "ethereum": {fiat: 3012.44, fiat + "_24h_change": -1.2},
                    "bitcoin": {fiat: 97000.5, fiat + "_24h_change": .8},
                    "litecoin": {fiat: 88.25, fiat + "_24h_change": 2.0},
                    "dogecoin": {fiat: .14},
                    "slowcoin": {fiat: 3.5},
                }
                body = json.dumps({coin: quotes[coin] for coin in ids if coin in quotes},
                                  separators=(",", ":")).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
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

        upstream_port = free_port()
        server = ThreadingHTTPServer(("127.0.0.1", upstream_port), Handler)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        server.socket = tls.wrap_socket(server.socket, server_side=True)
        serving = threading.Thread(target=server.serve_forever, daemon=True)
        serving.start()
        app_port = free_port()
        kv_dir = root / "kv"
        kv_dir.mkdir()
        env = dict(__import__("os").environ)
        env.update({
            "FORTIS_ENABLE_TEST_PRICE_ROUTE": "1",
            "FORTIS_TEST_PRICE_UPSTREAM_PORT": str(upstream_port),
            "FORTIS_TEST_PRICE_UPSTREAM_CA": str(ca),
            "FORTIS_TEST_PRICE_NOW_MS": "1767323045000",
            "FORTIS_KV_DIR": str(kv_dir),
        })
        process = subprocess.Popen([binary, str(app_port)], env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            wait_port(app_port, process)
            auth = {"Authorization": "Bearer " + TOKEN}
            assert request(app_port, "/v1/prices", headers={})[0] == 401

            cold = request(app_port, "/v1/prices?ids=ethereum%2Cbitcoin", headers=auth)
            assert cold[0] == fixture["coldJson"]["status"], cold
            for name, value in fixture["coldJson"]["headers"].items():
                assert cold[1].get(name) == value, (name, cold[1], value)
            cold_json = normalize_times(json.loads(cold[2]))
            assert cold_json == fixture["coldJson"]["body"], cold_json
            assert len(cold[2]) == int(fixture["coldJson"]["headers"]["content-length"])

            warm = request(app_port, "/v1/prices?ids=ethereum%2Cbitcoin", headers=auth)
            assert warm[0] == fixture["cachedJson"]["status"]
            for name, value in fixture["cachedJson"]["headers"].items():
                assert warm[1].get(name) == value, (name, warm[1], value)
            assert normalize_times(json.loads(warm[2])) == fixture["cachedJson"]["body"]
            assert len(warm[2]) == int(fixture["cachedJson"]["headers"]["content-length"])
            assert len(upstream_hits) == 1, upstream_hits

            # Auth and quota run before query validation, as in Fastify.
            bad_query = request(app_port, "/v1/prices", headers=auth)
            assert bad_query[0] == 400 and json.loads(bad_query[2])["error"] == "BAD_REQUEST"
            assert bad_query[1]["x-rate-limit-cost"] == "1"
            assert bad_query[1]["x-rate-limit-remaining"] == "237"
            assert request(app_port, "/v1/prices?ids=%20%20%2C", headers=auth)[0] == 400
            repeated = request(app_port, "/v1/prices?ids=ethereum&ids=bitcoin", headers=auth)
            assert repeated[0] == 200 and list(json.loads(repeated[2])["prices"]) == ["ethereum"], repeated
            assert request(app_port, "/v1/prices?ids=ethereum&fiat=USD", headers=auth)[0] == 400
            many = ",".join(["ethereum"] * 101)
            assert request(app_port, "/v1/prices?ids=" + many, headers=auth)[0] == 400
            # Exactly 4,000 decoded chars and 100 trimmed nonempty IDs is accepted.
            segments = [" " * 15 + "ethereum" + " " * (16 if i < 99 else 17)
                        for i in range(100)]
            boundary_ids = ",".join(segments)
            assert len(boundary_ids) == 4000
            boundary = request(app_port, "/v1/prices?ids=" + boundary_ids.replace(" ", "%20"), headers=auth)
            assert boundary[0] == 200, boundary
            assert request(app_port, "/v1/prices?ids=" + ("x" * 4001), headers=auth)[0] == 400

            # Restart with a fresh cache so protobuf is compared to the fixture's cold reply.
            process.terminate(); process.wait(timeout=5)
            shutil.rmtree(kv_dir); kv_dir.mkdir()
            process = subprocess.Popen([binary, str(app_port)], env=env,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            wait_port(app_port, process)
            proto = request(app_port, "/v1/prices?ids=ethereum%2Cbitcoin", headers={
                **auth, "Accept": "application/protobuf"})
            assert proto[0] == fixture["coldProtobuf"]["status"]
            for name, value in fixture["coldProtobuf"]["headers"].items():
                if name not in ("content-length", "x-rate-limit-remaining"):
                    assert proto[1].get(name) == value, (name, proto[1], value)
            assert proto[2] and proto[1]["content-type"] == "application/protobuf"
            assert proto[2].hex() == fixture["coldProtobuf"]["wireHex"], (len(proto[2]), proto[2].hex(), fixture["coldProtobuf"]["wireHex"])
            assert len(proto[2]) == int(fixture["coldProtobuf"]["headers"]["content-length"])

            head = request(app_port, "/v1/prices?ids=ethereum%2Cbitcoin", method="HEAD", headers=auth)
            assert head[0] == 200 and head[2] == b""
            assert int(head[1]["content-length"]) == len(warm[2])

            # Exact route matching leaves the literal history path out of the spot handler.
            history = request(app_port, "/v1/prices/history?ids=ethereum", headers=auth)
            assert history[0] == 404, history

            # ASA IDs reach the gated Vestige provider; this generic upstream
            # fixture answers an empty object, so the route returns a normal miss.
            asa = request(app_port, "/v1/prices?ids=asa%3A31566704", headers=auth)
            assert asa[0] == 200 and "asa:31566704" in json.loads(asa[2])["missing"]
            # TRC-10 IDs reach the gated TronScan provider; this generic
            # upstream fixture answers an empty object, so the route returns a normal miss.
            trc10 = request(app_port, "/v1/prices?ids=tron%3Atrc10%3A123", headers=auth)
            assert trc10[0] == 200 and "tron:trc10:123" in json.loads(trc10[2])["missing"]

            # Slow upstream completion is handed off without pinning an HTTP worker.
            slow = request(app_port, "/v1/prices?ids=slowcoin", headers=auth)
            assert slow[0] == 200 and json.loads(slow[2])["prices"]["slowcoin"]["price"] == 3.5

            # A raw peer close cancels the owner request; the server stays responsive.
            request(app_port, "/v1/prices?ids=dogecoin", headers=auth, disconnect=True)
            time.sleep(.25)
            assert request(app_port, "/healthz")[0] == 200
            time.sleep(1.0)
            assert any("dogecoin" in ids for ids, _fiat in upstream_hits), upstream_hits
            for _ in range(30):
                active = request(app_port, "/probe/test-price-owner")
                if json.loads(active[2])["active"] == 0:
                    break
                time.sleep(.05)
            else:
                raise AssertionError("disconnected HTTP request remained active in SpotProviderOwner")
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait(timeout=2)
            server.shutdown(); server.server_close(); serving.join(timeout=2)

    print("test-only native /v1/prices auth, fixture, JSON/protobuf, HEAD and disconnect checks passed")


if __name__ == "__main__":
    run(sys.argv[1])
