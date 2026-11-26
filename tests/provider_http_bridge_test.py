#!/usr/bin/env python3
"""Exercise the opt-in HTTP/provider bridge against fixed local TLS fixtures."""

import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time

import tls_ca


FIXTURE = Path(__file__).with_name("provider_execution_fixture_server.py")


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def wait_port(port: int) -> None:
    for _ in range(150):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.02)
    raise RuntimeError(f"TLS fixture did not listen on {port}")


def exchange(port: int, request: bytes) -> tuple[int, dict[str, str], bytes]:
    with socket.create_connection(("127.0.0.1", port), timeout=2) as client:
        client.settimeout(5)
        client.sendall(request)
        received = bytearray()
        while chunk := client.recv(8192):
            received.extend(chunk)
    headers, body = bytes(received).split(b"\r\n\r\n", 1)
    lines = headers.decode("ascii").split("\r\n")
    result_headers = dict(line.lower().split(": ", 1) for line in lines[1:])
    return int(lines[0].split()[1]), result_headers, body


def request(mode: str, timeout_ms: int = 30000) -> bytes:
    return (f"GET /probe/provider-test?mode={mode}&timeout_ms={timeout_ms} HTTP/1.1\r\n"
            "Host: localhost\r\n\r\n").encode()


def server_env(binary: str, port: int, upstream_port: int, bad_port: int,
               ca_file: Path, kv_dir: str) -> subprocess.Popen:
    env = os.environ.copy()
    for name in ("FORTIS_TLS_PROBE_HOST", "FORTIS_TLS_PROBE_PORT", "FORTIS_TLS_PROBE_CA",
                 "FORTIS_CA_BUNDLE_PATH", "FORTIS_ENABLE_ASYNC_TEST_PROBE"):
        env.pop(name, None)
    env.update(FORTIS_KV_DIR=kv_dir,
               FORTIS_ENABLE_PROVIDER_TEST_ROUTE="1",
               FORTIS_TEST_UPSTREAM_PORT=str(upstream_port),
               FORTIS_TEST_TLS_FAILURE_PORT=str(bad_port),
               FORTIS_TEST_UPSTREAM_CA=str(ca_file))
    return subprocess.Popen([binary, str(port)], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def wait_http(port: int, server: subprocess.Popen) -> None:
    for _ in range(150):
        if server.poll() is not None:
            raise RuntimeError(f"server exited: {server.stderr.read().decode(errors='replace')}")
        try:
            status, _, body = exchange(
                port, b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n")
            if status == 200 and json.loads(body) == {"ok": True}:
                return
        except (OSError, ValueError):
            pass
        time.sleep(0.02)
    raise RuntimeError("HTTP server did not become healthy")


def run(binary: str) -> None:
    with tempfile.TemporaryDirectory(prefix="provider-http-bridge-") as directory:
        root = Path(directory)
        valid_root = root / "valid"
        bad_root = root / "bad"
        valid_root.mkdir()
        bad_root.mkdir()
        ca, cert, key, _ = tls_ca.create_certificates(valid_root, "DNS:localhost")
        _, bad_cert, bad_key, _ = tls_ca.create_certificates(bad_root, "DNS:localhost")
        upstream_port = free_port()
        bad_port = free_port()
        fixture_command = [sys.executable, str(FIXTURE), str(upstream_port), str(cert), str(key)]
        bad_fixture_command = [sys.executable, str(FIXTURE), str(bad_port),
                               str(bad_cert), str(bad_key)]
        fixtures = [subprocess.Popen(command, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL)
                    for command in (fixture_command, bad_fixture_command)]
        server = None
        try:
            wait_port(upstream_port)
            wait_port(bad_port)
            port = free_port()
            main_kv_dir = root / "kv-main"
            main_kv_dir.mkdir()
            server = server_env(binary, port, upstream_port, bad_port, ca,
                                str(main_kv_dir))
            wait_http(port, server)

            # Eight provider waits exceed the four HTTP workers. A ninth intent
            # is rejected promptly, and health remains responsive throughout.
            gate = threading.Barrier(9)
            replies: list[tuple[int, dict[str, str], bytes] | None] = [None] * 8
            errors: list[BaseException] = []

            def slow(index: int) -> None:
                try:
                    gate.wait()
                    replies[index] = exchange(port, request("slow"))
                except BaseException as error:
                    errors.append(error)

            threads = [threading.Thread(target=slow, args=(index,)) for index in range(8)]
            for thread in threads:
                thread.start()
            gate.wait()
            time.sleep(0.08)
            started = time.monotonic()
            try:
                assert exchange(port, b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n")[0] == 200
            except BaseException:
                print(f"bridge server poll={server.poll()}", file=sys.stderr)
                if server.poll() is not None:
                    print(server.stderr.read().decode(errors="replace"), file=sys.stderr)
                raise
            assert time.monotonic() - started < 0.4
            cap_status, _, cap_body = exchange(port, request("ok"))
            assert cap_status == 503 and json.loads(cap_body) == {"error": "UPSTREAM_BUSY"}
            for thread in threads:
                thread.join(timeout=5)
                assert not thread.is_alive()
            assert not errors, errors
            assert all(reply and reply[0] == 200 and reply[2] == b"ok" for reply in replies)

            ok_status, _, ok_body = exchange(port, request("ok"))
            assert ok_status == 200 and ok_body == b"ok"
            retry_status, _, retry_body = exchange(port, request("retry503"))
            assert retry_status == 200 and retry_body == b"ok"
            always_status, always_headers, always_body = exchange(port, request("always503"))
            assert always_status == 503 and always_body == b"failure"
            assert always_headers.get("retry-after") == "1"

            tls_status, _, tls_body = exchange(port, request("tlsfail"))
            assert tls_status == 502 and json.loads(tls_body) == {
                "error": "UPSTREAM_FAILURE"}

            timeout_started = time.monotonic()
            timeout_status, _, timeout_body = exchange(port, request("stall", 120))
            assert timeout_status == 504 and json.loads(timeout_body) == {
                "error": "TIMEOUT"}
            assert time.monotonic() - timeout_started < 0.6
            time.sleep(1.7)  # running TLS cancellation settles before breaker checks

            limited_status, limited_headers, limited_body = exchange(port, request("429"))
            assert limited_status == 429 and limited_body == b"busy"
            assert limited_headers.get("retry-after") == "7"
            breaker_status, breaker_headers, breaker_body = exchange(port, request("429"))
            assert breaker_status == 503 and json.loads(breaker_body) == {
                "error": "UPSTREAM_UNAVAILABLE"}
            assert int(breaker_headers.get("retry-after", "0")) >= 1

            server.terminate()
            assert server.wait(timeout=6) == 0
            if server.stderr:
                server.stderr.close()
            server = None

            # Exercise stop order with an in-flight operation on a fresh lane.
            # The listener stops before the owner cancels and drains the job.
            port = free_port()
            shutdown_kv_dir = root / "kv-shutdown"
            shutdown_kv_dir.mkdir()
            server = server_env(binary, port, upstream_port, bad_port, ca,
                                str(shutdown_kv_dir))
            wait_http(port, server)
            with socket.create_connection(("127.0.0.1", port), timeout=2) as client:
                client.sendall(request("stall", 20000))
                time.sleep(0.1)
                server.terminate()
            assert server.wait(timeout=5) == 0
            print("provider HTTP bridge local TLS integration, retries, breaker, expiry, cap, and shutdown passed")
        finally:
            if server and server.poll() is None:
                server.terminate()
                server.wait(timeout=6)
            if server and server.stderr:
                server.stderr.close()
            for fixture in fixtures:
                fixture.terminate()
                fixture.wait(timeout=3)


if __name__ == "__main__":
    run(str(Path(sys.argv[1]).resolve()))
