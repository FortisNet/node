#!/usr/bin/env python3
"""Exercise both test-only price routes through one process coordinator."""
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
from urllib.parse import parse_qs, urlsplit

import tls_ca

SPOT_TOKEN = "fintel_dev_" + "P" * 43
HISTORY_TOKEN = "fintel_dev_" + "H" * 43
FIRST = 1767225600


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def request(port, path, token, disconnect=False):
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    sock.sendall((f"GET {path} HTTP/1.1\r\nHost: localhost\r\n"
                  f"Authorization: Bearer {token}\r\nConnection: close\r\n\r\n").encode())
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
    return int(head.split(b" ", 2)[1]), body


def verify_runtime_gates(binary, root):
    kv = root / "gate-kv"
    kv.mkdir()
    port = free_port()
    env = os.environ.copy()
    env["FORTIS_KV_DIR"] = str(kv)
    env.pop("FORTIS_ENABLE_TEST_PRICE_ROUTE", None)
    env.pop("FORTIS_ENABLE_TEST_PRICE_HISTORY_ROUTE", None)
    process = subprocess.Popen([binary, str(port)], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        for _ in range(150):
            if process.poll() is not None:
                raise RuntimeError(process.stderr.read().decode())
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.1):
                    break
            except OSError:
                time.sleep(.03)
        else:
            raise RuntimeError("combined route host did not start with gates closed")
        assert request(port, "/v1/prices?ids=ethereum", SPOT_TOKEN)[0] == 404
        assert request(port, f"/v1/prices/history?id=bitcoin&from={FIRST}&to={FIRST + 3600}",
                       HISTORY_TOKEN)[0] == 404
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=8)


def run(binary):
    with tempfile.TemporaryDirectory(prefix="combined-price-routes-") as temp:
        root = Path(temp)
        verify_runtime_gates(binary, root)
        ca, cert, key, _ = tls_ca.create_certificates(root, "DNS:localhost")
        gate = threading.Event()
        lock = threading.Lock()
        active = 0
        max_active = 0
        hits = []
        waiting = threading.Event()

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                nonlocal active, max_active
                parsed = urlsplit(self.path)
                query = parse_qs(parsed.query)
                with lock:
                    active += 1
                    max_active = max(max_active, active)
                    hits.append(self.path)
                    if active >= 2:
                        waiting.set()
                try:
                    if "market_chart" in parsed.path:
                        if "shutdown" in parsed.path:
                            # Hold the TLS operation until owner shutdown cancels it.
                            self.close_connection = True
                            while not gate.wait(.05):
                                try:
                                    self.connection.settimeout(.01)
                                    if self.connection.recv(1) == b"":
                                        return
                                except (TimeoutError, socket.timeout):
                                    pass
                                except OSError:
                                    return
                            return
                        gate.wait(5)
                        body = json.dumps({
                            "prices": [[int(query.get("from", [FIRST])[0]) * 1000 + i * 3600000, 100 + i]
                                       for i in range(7)],
                            "total_volumes": [[int(query.get("from", [FIRST])[0]) * 1000 + i * 3600000, 10 + i]
                                              for i in range(7)],
                        }, separators=(",", ":")).encode()
                    else:
                        gate.wait(5)
                        ids = query.get("ids", [""])[0].split(",")
                        fiat = query.get("vs_currencies", ["usd"])[0]
                        body = json.dumps({coin: {fiat: 123.4} for coin in ids},
                                          separators=(",", ":")).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError, OSError):
                    pass
                finally:
                    with lock:
                        active -= 1

            def log_message(self, *_args):
                pass

        upstream = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        upstream_port = upstream.server_address[1]
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        upstream.socket = context.wrap_socket(upstream.socket, server_side=True)
        thread = threading.Thread(target=upstream.serve_forever, daemon=True)
        thread.start()

        port = free_port()
        kv = root / "kv"
        kv.mkdir()
        env = os.environ.copy()
        env.update({
            "FORTIS_KV_DIR": str(kv),
            "FORTIS_ENABLE_TEST_PRICE_ROUTE": "1",
            "FORTIS_ENABLE_TEST_PRICE_HISTORY_ROUTE": "1",
            "FORTIS_TEST_PRICE_UPSTREAM_PORT": str(upstream_port),
            "FORTIS_TEST_HISTORY_UPSTREAM_PORT": str(upstream_port),
            "FORTIS_TEST_PRICE_UPSTREAM_CA": str(ca),
            "FORTIS_TEST_HISTORY_UPSTREAM_CA": str(ca),
        })
        process = subprocess.Popen([binary, str(port)], env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            for _ in range(150):
                if process.poll() is not None:
                    raise RuntimeError(process.stderr.read().decode())
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=.1):
                        break
                except OSError:
                    time.sleep(.03)
            else:
                raise RuntimeError("combined route host did not start")

            jobs = []
            results = [None] * 8
            for i in range(4):
                jobs.append((f"/v1/prices?ids=spotcoin{i}", SPOT_TOKEN))
                jobs.append((f"/v1/prices/history?id=bitcoin&from={FIRST + i * 12 * 3600}"
                             f"&to={FIRST + i * 12 * 3600 + 6 * 3600}&interval=1h", HISTORY_TOKEN))
            errors = []
            def run_request(index, path, token):
                try:
                    results[index] = request(port, path, token)
                except Exception as error:  # surfaced on the main thread below
                    errors.append(error)
            callers = [threading.Thread(target=run_request, args=(i, *job), daemon=True)
                       for i, job in enumerate(jobs)]
            for caller in callers:
                caller.start()
            if not waiting.wait(5):
                gate.set()
                for caller in callers:
                    caller.join(timeout=2)
                raise AssertionError(f"both routes did not reach TLS: hits={hits}, "
                                     f"results={results}, errors={errors}")
            time.sleep(.35)
            with lock:
                observed_peak = max_active
            gate.set()
            for caller in callers:
                caller.join(timeout=12)
                assert not caller.is_alive(), "concurrent route request did not finish"
            assert not errors, errors
            assert observed_peak == 2, f"shared lane admitted {observed_peak} upstream calls"
            assert all(result and result[0] == 200 for result in results), results
            assert len(hits) >= 5, hits

            # A disconnected spot waiter must not strand a history peer.
            gate.clear()
            waiting.clear()
            with lock:
                hits_before_cancel = len(hits)
            request(port, "/v1/prices?ids=dogecoin", SPOT_TOKEN, disconnect=True)
            peer = []
            peer_thread = threading.Thread(target=lambda: peer.append(request(
                port, f"/v1/prices/history?id=bitcoin&from={FIRST + 4 * 86400}"
                f"&to={FIRST + 4 * 86400 + 6 * 3600}"
                "&interval=1h", HISTORY_TOKEN)), daemon=True)
            peer_thread.start()
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                with lock:
                    if len(hits) > hits_before_cancel:
                        break
                time.sleep(.02)
            assert len(hits) > hits_before_cancel, "cancellation fixture did not reach provider"
            gate.set()
            peer_thread.join(timeout=10)
            assert not peer_thread.is_alive() and peer and peer[0][0] == 200, peer

            # Stop with both owners waiting on upstream TLS; shutdown must cancel
            # their jobs, drain response callbacks, and join without a leaked thread.
            gate.clear()
            shutdown_calls = []
            def pending(path, token):
                try:
                    shutdown_calls.append(request(port, path, token))
                except (OSError, ValueError, IndexError):
                    pass
            pending_threads = [
                threading.Thread(target=pending, args=(
                    f"/v1/prices?ids=shutdownspot", SPOT_TOKEN), daemon=True),
                threading.Thread(target=pending, args=(
                    f"/v1/prices/history?id=shutdowncoin&from={FIRST}&to={FIRST + 6 * 3600}"
                    "&interval=1h", HISTORY_TOKEN), daemon=True),
            ]
            for caller in pending_threads:
                caller.start()
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                with lock:
                    if any("shutdown" in path for path in hits):
                        break
                time.sleep(.02)
            assert any("shutdown" in path for path in hits), "shutdown requests never reached TLS"
            process.terminate()
            process.wait(timeout=10)
            for caller in pending_threads:
                caller.join(timeout=3)
                assert not caller.is_alive(), "shutdown left a route request thread blocked"
            assert process.returncode == 0, process.stderr.read().decode()
            print("combined price routes shared the process TLS budget; cancellation and orderly shutdown passed")
        finally:
            gate.set()
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
            upstream.shutdown()
            upstream.server_close()
            thread.join(timeout=3)


if __name__ == "__main__":
    run(sys.argv[1])
