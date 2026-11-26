#!/usr/bin/env python3
"""Exercise the native binary over a real TCP socket.

Usage: smoke.py <fortis_node> <fortis_node_probe_test>
The second binary is the production composition plus the test probes; only the
response handoff checks (/probe/async, /probe/async-timeout) run against it.
"""

import json
import re
import os
from pathlib import Path
import socket
import subprocess
import sys
import threading
import tempfile
import time

import compat_expectations


def exchange(port: int, request: bytes | list[bytes], host: str = "127.0.0.1") -> tuple[int, dict[str, str], bytes]:
    with socket.create_connection((host, port), timeout=2) as client:
        client.settimeout(5)
        for part in request if isinstance(request, list) else [request]:
            client.sendall(part)
        result = bytearray()
        while chunk := client.recv(4096):
            result.extend(chunk)
    header, body = bytes(result).split(b"\r\n\r\n", 1)
    lines = header.decode("ascii").split("\r\n")
    status = int(lines[0].split()[1])
    headers = dict(line.lower().split(": ", 1) for line in lines[1:])
    assert len(body) == 0 or len(body) == int(headers["content-length"])
    return status, headers, body


def test_invalid_startup_config(binary: str) -> None:
    cases = [
        ({"PORT": "0"}, "PORT"),
        ({"DEVICE_TOKEN_TTL_SECONDS": "299"}, "DEVICE_TOKEN_TTL_SECONDS"),
        ({"ATTESTATION_MODE": "unsafe"}, "ATTESTATION_MODE"),
        ({"METRICS_TOKEN": "t" * 513}, "METRICS_TOKEN"),
        ({"NODE_ENV": "production", "ATTESTATION_MODE": "development"},
         "mandatory in production"),
        ({"FORTIS_TLS_PROBE_HOST": "localhost"}, "TLS probe"),
        ({"FORTIS_ENABLE_DIAGNOSTIC_PROBES": "true"}, "FORTIS_ENABLE_DIAGNOSTIC_PROBES"),
    ]
    for overrides, message in cases:
        env = os.environ.copy()
        for name in ("PORT", "NODE_ENV", "ATTESTATION_MODE", "DEVICE_TOKEN_TTL_SECONDS",
                     "FORTIS_TLS_PROBE_HOST", "FORTIS_TLS_PROBE_PORT", "FORTIS_TLS_PROBE_CA",
                     "FORTIS_ENABLE_PROVIDER_TEST_ROUTE", "FORTIS_TEST_UPSTREAM_PORT",
                     "FORTIS_TEST_TLS_FAILURE_PORT", "FORTIS_TEST_UPSTREAM_CA",
                     "METRICS_TOKEN", "FORTIS_ENABLE_DIAGNOSTIC_PROBES"):
            env.pop(name, None)
        env.update(overrides)
        result = subprocess.run([binary], env=env, capture_output=True, timeout=3)
        assert result.returncode == 2, (overrides, result.returncode, result.stderr)
        assert message.encode() in result.stderr, (overrides, result.stderr)


def check_response_handoff(port: int) -> None:
    """The bounded response handoff, through the replay binaries' /probe/async
    and /probe/async-timeout probes (FORTIS_ENABLE_ASYNC_TEST_PROBE=1)."""
    # Delayed replies are held by the bounded response handoff, leaving all
    # four HTTP workers available while more than four requests are pending.
    async_results: list[tuple[int, dict[str, str], bytes] | None] = [None] * 8
    async_errors: list[BaseException] = []
    barrier = threading.Barrier(9)

    def async_request(index: int) -> None:
        try:
            barrier.wait()
            async_results[index] = exchange(
                port, b"GET /probe/async?delay_ms=700 HTTP/1.1\r\n"
                      b"Host: localhost\r\n\r\n")
        except BaseException as error:
            async_errors.append(error)

    async_threads = [threading.Thread(target=async_request, args=(i,))
                     for i in range(len(async_results))]
    for thread in async_threads:
        thread.start()
    barrier.wait()
    health_started = time.monotonic()
    assert exchange(port, b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n")[0] == 200
    assert time.monotonic() - health_started < 0.5
    for thread in async_threads:
        thread.join(timeout=3)
        assert not thread.is_alive()
    assert not async_errors, async_errors
    assert all(result and result[0] == 200 and result[2] == b"async-ready"
               for result in async_results)

    owned_body = b"request-body-owned-by-the-handoff"
    owned_status, _, owned_response = exchange(
        port, b"POST /probe/async?delay_ms=250 HTTP/1.1\r\nHost: localhost\r\n"
              b"Content-Length: " + str(len(owned_body)).encode() + b"\r\n\r\n" + owned_body)
    assert owned_status == 200 and owned_response == owned_body
    head_status, head_headers, head_body = exchange(
        port, b"HEAD /probe/async?status=502&delay_ms=100 HTTP/1.1\r\n"
              b"Host: localhost\r\n\r\n")
    assert head_status == 502 and head_body == b""
    assert head_headers["content-length"] == str(len(b"async-ready"))
    timeout_status, _, timeout_body = exchange(
        port, b"GET /probe/async-timeout?timeout_ms=80 HTTP/1.1\r\n"
              b"Host: localhost\r\n\r\n")
    assert timeout_status == 504 and json.loads(timeout_body) == {"error": "TIMEOUT"}

    # Hold all response slots with pending delayed replies, then verify a
    # new async request receives a nonblocking overload response.
    held_clients = []
    try:
        for _ in range(16):
            client = socket.create_connection(("127.0.0.1", port), timeout=2)
            client.settimeout(3)
            client.sendall(b"GET /probe/async?delay_ms=1200 HTTP/1.1\r\n"
                           b"Host: localhost\r\n\r\n")
            held_clients.append(client)
        time.sleep(0.2)
        overload_status, _, overload_body = exchange(
            port, b"GET /probe/async?delay_ms=1200 HTTP/1.1\r\n"
                  b"Host: localhost\r\n\r\n")
        assert overload_status == 503 and json.loads(overload_body) == {
            "error": "OVERLOADED"}
    finally:
        for client in held_clients:
            client.close()
    time.sleep(1.3)  # let writer threads release the aborted pending slots


def run_response_handoff_checks(probe_binary: str) -> None:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix="fortis-kv-async-") as kv_dir:
        env = {**os.environ, "FORTIS_KV_DIR": kv_dir, "FORTIS_ENABLE_ASYNC_TEST_PROBE": "1"}
        for name in ("FORTIS_ENABLE_PROVIDER_TEST_ROUTE", "FORTIS_TEST_UPSTREAM_PORT",
                     "FORTIS_TEST_TLS_FAILURE_PORT", "FORTIS_TEST_UPSTREAM_CA",
                     "FORTIS_NATIVE_PRICE_ROUTE_CANARY", "METRICS_TOKEN"):
            env.pop(name, None)
        server = subprocess.Popen([probe_binary, str(port)], stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, env=env)
        try:
            for _ in range(100):
                if server.poll() is not None:
                    raise AssertionError("probe test server exited before listening")
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                        break
                except OSError:
                    time.sleep(0.02)
            else:
                raise AssertionError("probe test server did not listen")
            check_response_handoff(port)
            assert exchange(port, b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n")[0] == 200
        finally:
            server.terminate()
            server.communicate(timeout=10)


def main() -> None:
    test_invalid_startup_config(sys.argv[1])
    run_response_handoff_checks(sys.argv[2])
    fixture = json.loads((Path(__file__).resolve().parents[1] /
                          "tests/corpus/device_routes.json").read_text())
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]

    kv_dir = tempfile.TemporaryDirectory(prefix="fortis-kv-http-")
    server_env = {**os.environ, "FORTIS_KV_DIR": kv_dir.name}
    for name in ("FORTIS_ENABLE_PROVIDER_TEST_ROUTE", "FORTIS_TEST_UPSTREAM_PORT",
                 "FORTIS_TEST_TLS_FAILURE_PORT", "FORTIS_TEST_UPSTREAM_CA",
                 "FORTIS_NATIVE_PRICE_ROUTE_CANARY"):
        server_env.pop(name, None)
    server_env["METRICS_TOKEN"] = " \t "
    server_env["FORTIS_ENABLE_ASYNC_TEST_PROBE"] = "1"
    # This instance exercises the diagnostic probes, so it opts in; the
    # required-mode instance below checks the default restriction.
    server_env["FORTIS_ENABLE_DIAGNOSTIC_PROBES"] = "1"
    server = subprocess.Popen(
        [sys.argv[1], str(port)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env=server_env,
    )
    try:
        for _ in range(100):
            if server.poll() is not None:
                raise AssertionError("server exited before listening")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                    break
            except OSError:
                time.sleep(0.02)
        else:
            raise AssertionError("server did not listen")

        status, headers, body = exchange(
            port, b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n"
        )
        assert status == 200 and json.loads(body) == {"ok": True}
        ai_status, ai_headers, ai_body = exchange(
            port, b"GET /ai HTTP/1.1\r\nHost: localhost\r\n\r\n")
        assert ai_status == 200 and ai_headers["content-type"] == "text/html; charset=utf-8"
        assert b"AI models" in ai_body and b"No AI models are configured" in ai_body
        assert ai_headers["cache-control"] == "no-store"
        ai_head_status, ai_head_headers, ai_head_body = exchange(
            port, b"HEAD /ai HTTP/1.1\r\nHost: localhost\r\n\r\n")
        assert ai_head_status == 200 and ai_head_body == b""
        assert ai_head_headers["content-length"] == str(len(ai_body))
        # The spot endpoint stays unregistered without its canary opt-in
        # (tests/price_spot_canary_test.py covers both states).
        assert exchange(port, b"GET /v1/prices?ids=ethereum HTTP/1.1\r\nHost: localhost\r\n\r\n")[0] == 404
        assert exchange(port, b"GET /v1/prices/history HTTP/1.1\r\nHost: localhost\r\n\r\n")[0] == 404
        assert exchange(port, b"GET /v1/prices/history?id=ethereum&from=1&to=2 HTTP/1.1\r\nHost: localhost\r\n\r\n")[0] == 404
        assert exchange(port, b"HEAD /v1/prices/history?id=ethereum&from=1&to=2 HTTP/1.1\r\nHost: localhost\r\n\r\n")[0] == 404
        assert exchange(port, b"GET /v1/networks HTTP/1.1\r\nHost: localhost\r\n\r\n")[0] == 401
        assert exchange(port, b"HEAD /v1/networks HTTP/1.1\r\nHost: localhost\r\n\r\n")[0] == 401
        disabled_status, disabled_headers, disabled_body = exchange(
            port, b"GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n")
        assert disabled_status == 404 and disabled_body == b""
        assert "content-type" not in disabled_headers and disabled_headers["content-length"] == "0"
        assert headers["content-type"] == "application/json"
        assert headers["x-content-type-options"] == "nosniff"
        assert headers["referrer-policy"] == "no-referrer"
        assert headers["permissions-policy"] == "camera=(), microphone=(), geolocation=()"
        assert headers["cache-control"] == "no-store"
        assert headers["content-length"] == str(len(body))

        # The async test probes are not part of the production binary, whatever
        # FORTIS_ENABLE_ASYNC_TEST_PROBE says (check_response_handoff runs them
        # in the probe test binary).
        for request in (b"GET /probe/async?delay_ms=1", b"POST /probe/async",
                        b"HEAD /probe/async", b"GET /probe/async-timeout?timeout_ms=1"):
            missing_status, _, missing_body = exchange(
                port, request + b" HTTP/1.1\r\nHost: localhost\r\n\r\n")
            assert missing_status == 404, (request, missing_status)
            assert request.startswith(b"HEAD") or json.loads(missing_body) == {"error": "NOT_FOUND"}

        assert exchange(port, [b"GET /healthz HTTP/1.1\r\nHost: local",
                               b"host\r\n\r", b"\n"])[0] == 200
        queried_status, _, queried_body = exchange(
            port, b"GET /healthz?check=1 HTTP/1.1\r\nHost: localhost\r\n\r\n")
        assert queried_status == 200 and json.loads(queried_body) == {"ok": True}

        head_status, head_headers, head_body = exchange(
            port, b"HEAD /missing?check=1 HTTP/1.1\r\nHost: localhost\r\n\r\n")
        assert head_status == 404 and head_body == b""
        assert head_headers["content-length"] == str(len(b'{"error":"NOT_FOUND"}'))
        assert exchange(port, b"HEAD /probe/protobuf HTTP/1.1\r\nHost: localhost\r\n\r\n")[0] == 404

        platform_status, _, platform_body = exchange(
            port, b"GET /probe/platform HTTP/1.1\r\nHost: localhost\r\n\r\n"
        )
        platform = json.loads(platform_body)
        assert platform_status in (200, 503)
        assert platform["clock"] is True and platform["entropy"] is True
        assert isinstance(platform["dns"], bool)
        assert isinstance(platform["tcp"], bool)

        status, _, body = exchange(
            port, b"GET /probe HTTP/1.1\r\nHost: localhost\r\n\r\n"
        )
        assert status == 200
        assert json.loads(body) == {
            "service": "fortis_node",
            "stage": "bootstrap",
        }
        provider_fixture_status, _, _ = exchange(
            port, b"GET /probe/provider-test?mode=ok HTTP/1.1\r\n"
                  b"Host: localhost\r\n\r\n")
        assert provider_fixture_status == 404  # no provider route without its test flag

        slow_request = b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n"
        with socket.create_connection(("127.0.0.1", port), timeout=2) as slow:
            slow.sendall(slow_request)
            started = time.monotonic()
            status, _, body = exchange(
                port, b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n")
            assert status == 200 and json.loads(body) == {"ok": True}
            assert time.monotonic() - started < 1.0

        # Four workers plus 256 queued sockets form the application
        # admission bound. An additional connection gets a 503.
        slow_clients = []
        try:
            for _ in range(260):
                client = socket.create_connection(("127.0.0.1", port), timeout=2)
                client.settimeout(3)
                client.sendall(slow_request)
                slow_clients.append(client)
            time.sleep(0.2)
            with socket.create_connection(("127.0.0.1", port), timeout=2) as excess:
                with excess.makefile("rb") as response:
                    assert response.readline().startswith(b"HTTP/1.1 503 ")
        finally:
            for client in slow_clients:
                client.close()

        status, _, body = exchange(
            port, b"GET /probe/evm HTTP/1.1\r\nHost: localhost\r\n\r\n"
        )
        assert status == 200 and json.loads(body) == {"valid": True}

        status, _, body = exchange(
            port, b"GET /probe/kv HTTP/1.1\r\nHost: localhost\r\n\r\n"
        )
        assert status == 200 and json.loads(body) == {"counter": 0}
        status, _, body = exchange(
            port, b"POST /probe/kv HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n"
        )
        assert status == 200 and json.loads(body) == {"counter": 1}

        status, headers, body = exchange(
            port, b"HEAD /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n"
        )
        assert status == 200 and body == b""
        assert headers["content-length"] == str(len(b'{"ok":true}'))

        negotiated = b"GET /probe/representation HTTP/1.1\r\nHost: localhost\r\nAccept: application/protobuf\r\n\r\n"
        status, headers, body = exchange(port, negotiated)
        assert status == 200 and headers["content-type"] == "application/protobuf"
        assert headers["vary"] == "accept" and body == bytes.fromhex("0a0570726f6265")
        for accept in (b"application/protobuf;q=0", b"application/json"):
            status, headers, body = exchange(
                port, b"GET /probe/representation HTTP/1.1\r\nHost: localhost\r\nAccept: "
                + accept + b"\r\n\r\n")
            assert status == 200 and headers["content-type"] == "application/json"
            assert headers["vary"] == "accept"
            assert json.loads(body) == {"representation": "json"}
        mixed = b"GET /probe/representation HTTP/1.1\r\nHost: localhost\r\nAccept: application/json, application/protobuf; q=0.4\r\n\r\n"
        status, headers, body = exchange(port, mixed)
        assert status == 200 and headers["content-type"] == "application/protobuf"
        assert headers["vary"] == "accept" and body == bytes.fromhex("0a0570726f6265")

        challenge_request = b"POST /v1/devices/challenge HTTP/1.1\r\nHost: localhost\r\n\r\n"
        status, headers, body = exchange(port, challenge_request)
        result = json.loads(body)
        public_hits = 1
        assert status == 200 and headers["content-type"] == "application/json; charset=utf-8"
        assert headers["vary"] == "accept" and result["expiresIn"] == 300
        assert re.fullmatch(r"[A-Za-z0-9_-]{43}", result["challenge"])
        assert headers["content-length"] == "75"
        assert headers["cache-control"] == "no-store"
        assert headers["x-content-type-options"] == "nosniff"
        assert headers["referrer-policy"] == "no-referrer"
        assert headers["permissions-policy"] == "camera=(), microphone=(), geolocation=()"
        protobuf_request = (b"POST /v1/devices/challenge HTTP/1.1\r\nHost: localhost\r\n"
                           b"Accept: application/protobuf\r\n\r\n")
        status, headers, wire = exchange(port, protobuf_request)
        public_hits += 1
        assert status == 200 and headers["content-type"] == "application/protobuf"
        assert headers["vary"] == "accept" and len(wire) == 48
        assert wire[:2] == b"\x0a\x2b" and wire[-3:] == b"\x10\xac\x02"
        protobuf_challenge = wire[2:45].decode("ascii")
        assert re.fullmatch(r"[A-Za-z0-9_-]{43}", protobuf_challenge)
        assert protobuf_challenge != result["challenge"]
        status, headers, body = exchange(port, challenge_request[:-2] + b"Accept: application/protobuf;q=0\r\n\r\n")
        public_hits += 1
        assert status == 200 and headers["content-type"] == "application/json; charset=utf-8"
        assert headers["vary"] == "accept" and json.loads(body)["expiresIn"] == 300
        origin = challenge_request[:-2] + b"Origin: https://example.test\r\n\r\n"
        status, headers, body = exchange(port, origin)
        assert status == 403 and json.loads(body) == {"error": "BROWSER_ORIGIN_FORBIDDEN"}
        assert headers["content-type"] == "application/json; charset=utf-8"
        assert "vary" not in headers and headers["cache-control"] == "no-store"
        # Origin runs in the public preHandler before the route's Zod validation.
        invalid_with_origin = (b"POST /v1/devices HTTP/1.1\r\nHost: localhost\r\n"
                               b"Origin: https://example.test\r\nContent-Type: application/json\r\n"
                               b"Content-Length: 2\r\n\r\n{}")
        status, _, body = exchange(port, invalid_with_origin)
        assert status == 403 and json.loads(body) == {"error": "BROWSER_ORIGIN_FORBIDDEN"}
        plain_origin = (b"POST /v1/devices HTTP/1.1\r\nHost: localhost\r\nOrigin: https://example.test\r\n"
                        b"Content-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello")
        status, _, body = exchange(port, plain_origin)
        # text/plain is an unsupported media type
        # (compat-decisions.md#form-bodies): the parser answers before Origin.
        assert status == 415 and json.loads(body) == {"error": "UNSUPPORTED_MEDIA_TYPE"}  # fastify-errors
        unsupported_origin = (b"POST /v1/devices HTTP/1.1\r\nHost: localhost\r\nOrigin: https://example.test\r\n"
                             b"Content-Type: application/octet-stream\r\nContent-Length: 5\r\n\r\nhello")
        status, _, body = exchange(port, unsupported_origin)
        assert status == 415 and json.loads(body) == {"error": "UNSUPPORTED_MEDIA_TYPE"}
        invalid_json_origin = (b"POST /v1/devices HTTP/1.1\r\nHost: localhost\r\nOrigin: https://example.test\r\n"
                               b"Content-Type: application/json\r\nContent-Length: 4\r\n\r\n{bad")
        status, _, body = exchange(port, invalid_json_origin)
        assert status == 400 and json.loads(body) == {"error": "BAD_REQUEST"}

        enrollment = {"platform": "android", "appVersion": "4.5.6",
                      "attestation": {"provider": "development", "challenge": result["challenge"]}}
        enrollment_body = json.dumps(enrollment, separators=(",", ":")).encode()
        enrollment_request = (b"POST /v1/devices HTTP/1.1\r\nHost: localhost\r\n"
                              b"Content-Type: application/json\r\nContent-Length: "
                              + str(len(enrollment_body)).encode() + b"\r\n\r\n" + enrollment_body)
        status, headers, body = exchange(port, enrollment_request)
        public_hits += 1
        created = json.loads(body)
        assert status == 201 and headers["content-type"] == "application/json; charset=utf-8"
        assert headers["vary"] == "accept" and len(created["deviceId"]) == 26
        assert re.fullmatch(r"fintel_dev_[A-Za-z0-9_-]{43}", created["deviceToken"])
        assert re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z", created["expiresAt"])
        rotate_path = b"POST /v1/devices/rotate HTTP/1.1\r\nHost: localhost\r\n"
        for authorization in (None, "Bearer malformed", "Bearer fintel_dev_" + "A" * 43):
            auth_header = b"" if authorization is None else ("Authorization: " + authorization + "\r\n").encode()
            status, headers, body = exchange(port, rotate_path + auth_header + b"\r\n")
            assert status == 401 and json.loads(body) == {"error": "UNAUTHORIZED"}
            assert "x-rate-limit-cost" not in headers
        old_token = created["deviceToken"]
        rotate_request = rotate_path + ("Authorization: Bearer " + old_token + "\r\n\r\n").encode()
        status, headers, body = exchange(port, rotate_request)
        rotated = json.loads(body)
        assert status == 200 and headers["vary"] == "accept"
        assert headers["x-rate-limit-cost"] == "1" and headers["x-rate-limit-remaining"] == "239"
        assert rotated["deviceToken"] != old_token
        status, _, body = exchange(port, rotate_path + ("Authorization: Bearer " + old_token + "\r\n\r\n").encode())
        assert status == 401 and json.loads(body) == {"error": "UNAUTHORIZED"}
        status, headers, wire = exchange(port, rotate_path +
            ("Authorization: Bearer " + rotated["deviceToken"] + "\r\nAccept: application/protobuf\r\n\r\n").encode())
        assert status == 200 and headers["content-type"] == "application/protobuf"
        assert headers["vary"] == "accept" and headers["x-rate-limit-cost"] == "1"
        assert len(wire) == 82 and wire[:2] == b"\x0a\x36" and wire[56:58] == b"\x12\x18"
        quota_token = wire[2:56].decode("ascii")
        for index in range(238):
            status, quota_headers, quota_body = exchange(port, rotate_path +
                ("Authorization: Bearer " + quota_token + "\r\n\r\n").encode())
            assert status == 200, index
            assert quota_headers["x-rate-limit-cost"] == "1"
            assert quota_headers["x-rate-limit-remaining"] == str(237 - index)
            quota_token = json.loads(quota_body)["deviceToken"]
        status, quota_headers, quota_body = exchange(port, rotate_path +
            ("Authorization: Bearer " + quota_token + "\r\n\r\n").encode())
        assert status == 429 and json.loads(quota_body) == {"error": "RATE_LIMITED"}
        assert quota_headers["retry-after"] == "60"
        assert quota_headers["x-rate-limit-cost"] == "1"
        assert quota_headers["x-rate-limit-remaining"] == "0"
        status, _, body = exchange(port, enrollment_request)
        public_hits += 1
        assert status == 401 and json.loads(body) == {"error": "ATTESTATION_EXPIRED"}

        status, _, body = exchange(port, challenge_request)
        public_hits += 1
        protobuf_attestation_challenge = json.loads(body)["challenge"].encode("ascii")
        nested = (bytes.fromhex("0a0b646576656c6f706d656e74") + b"\x12\x2b"
                  + protobuf_attestation_challenge)
        protobuf_body = (bytes.fromhex("0a07616e64726f6964") + bytes.fromhex("1205342e352e36")
                         + b"\x1a" + bytes([len(nested)]) + nested)
        protobuf_enroll = (b"POST /v1/devices HTTP/1.1\r\nHost: localhost\r\n"
                           b"Content-Type: application/protobuf\r\nAccept: application/protobuf\r\n"
                           b"Content-Length: " + str(len(protobuf_body)).encode() + b"\r\n\r\n" + protobuf_body)
        status, headers, wire = exchange(port, protobuf_enroll)
        public_hits += 1
        assert status == 201 and headers["content-type"] == "application/protobuf"
        assert headers["vary"] == "accept" and len(wire) == 110
        assert wire[:2] == b"\x0a\x1a" and wire[28:30] == b"\x12\x36"
        assert wire[84:86] == b"\x1a\x18"

        unsupported = enrollment.copy()
        unsupported["attestation"] = {"provider": "apple-app-attest", "challenge": "C" * 43}
        unsupported_body = json.dumps(unsupported, separators=(",", ":")).encode()
        unsupported_request = (b"POST /v1/devices HTTP/1.1\r\nHost: localhost\r\n"
                               b"Content-Type: application/json\r\nContent-Length: "
                               + str(len(unsupported_body)).encode() + b"\r\n\r\n" + unsupported_body)
        status, _, body = exchange(port, unsupported_request)
        public_hits += 1
        assert status == 401 and json.loads(body) == {"error": "ATTESTATION_INVALID"}

        plain_body = b'{"platform":"ios"}'
        plain_request = (b"POST /v1/devices HTTP/1.1\r\nHost: localhost\r\n"
                         b"Content-Type: text/plain\r\nContent-Length: "
                         + str(len(plain_body)).encode() + b"\r\n\r\n" + plain_body)
        status, _, body = exchange(port, plain_request)
        # text/plain is an unsupported media type, refused before the IP
        # quota (compat-decisions.md#form-bodies).
        assert status == 415 and json.loads(body) == {"error": "UNSUPPORTED_MEDIA_TYPE"}

        invalid_platform = b'{"platform":"bad"}'
        invalid_request = (b"POST /v1/devices HTTP/1.1\r\nHost: localhost\r\n"
                           b"Content-Type: application/json\r\nContent-Length: "
                           + str(len(invalid_platform)).encode() + b"\r\n\r\n" + invalid_platform)
        status, _, body = exchange(port, invalid_request)
        public_hits += 1
        assert status == fixture["behavior"]["invalidEnrollmentBody"]["status"]
        expected = fixture["behavior"]["invalidEnrollmentBody"]["body"]
        assert json.loads(body) == (compat_expectations.zod_refusal(expected) or expected)

        while public_hits < 10:
            assert exchange(port, challenge_request)[0] == 200
            public_hits += 1
        status, headers, body = exchange(port, challenge_request)
        assert status == 429 and json.loads(body) == {"error": "RATE_LIMITED"}
        assert headers["retry-after"] == "60"
        status, _, body = exchange(port, b"POST /v1/devices/rotate HTTP/1.1\r\nHost: localhost\r\n\r\n")
        assert status == 401 and json.loads(body) == {"error": "UNAUTHORIZED"}

        # Generated from the existing TS/Buf DashPayNameResponse schema.
        wire = bytes.fromhex("0a0a616c6963652e6461736812056162636465")
        request = (b"POST /probe/protobuf HTTP/1.1\r\nHost: localhost\r\n"
                   b"Content-Type: application/protobuf\r\nContent-Length: "
                   + str(len(wire)).encode("ascii") + b"\r\n\r\n" + wire)
        status, headers, body = exchange(port, request)
        assert status == 200 and headers["content-type"] == "application/protobuf"
        assert headers["x-content-type-options"] == "nosniff"
        assert headers["content-length"] == str(len(wire))
        assert body == wire
        assert exchange(port, request[:-len(wire)] + b"\xff" + wire[1:])[0] == 400
        assert exchange(port, request.replace(
            b"Content-Length: 19", b"Content-Length: 19\r\nContent-Length: 19"))[0] == 400

        assert exchange(port, b"GET /healthz HTTP/1.1\r\n\r\n")[0] == 400
        assert exchange(port, b"GET /healthz HTTP/1.1\r\nHost: x\r\n"
                        b"Transfer-Encoding: chunked\r\n\r\n")[0] == 400
        assert exchange(port, b"GET /healthz HTTP/1.1\r\nHost: x\r\n"
                        b"Content-Length: 999999\r\n\r\n")[0] == 413
        assert exchange(port, b"GET /healthz HTTP/1.1\r\nHost: x\r\n"
                        b"Content-Length: 1\r\nContent-Length: 1\r\n\r\nx")[0] == 400
        large_body = b"x" * 300_000
        assert exchange(port, b"POST /missing HTTP/1.1\r\nHost: x\r\n"
                        b"Content-Length: 300000\r\n\r\n" + large_body)[0] == 404

        assert exchange(port, b"GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n")[0] == 404
        assert exchange(port, b"POST /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n")[0] == 404
        assert exchange(port, b"PUT /missing HTTP/1.1\r\nHost: localhost\r\n\r\n")[0] == 404
        assert exchange(port, b"GET /probe/kv HTTP/1.1\r\nHost: localhost\r\n"
                        b"Content-Length: 1\r\n\r\nx")[0] == 405
        assert exchange(port, b"POST /probe/protobuf HTTP/1.1\r\nHost: localhost\r\n"
                        b"Content-Length: 0\r\n\r\n")[0] == 400
        with socket.create_connection(("127.0.0.1", port), timeout=2) as slow:
            slow.settimeout(5)
            slow.sendall(b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n")
            time.sleep(3.2)
            timed_out = bytearray()
            while chunk := slow.recv(4096):
                timed_out.extend(chunk)
            assert b" 408 Request Timeout\r\n" in timed_out
        incomplete = b"GET /healthz HTTP/1.1\r\nHost: "
        assert exchange(port, incomplete + b"x" * (16384 - len(incomplete)))[0] == 431
    finally:
        server.terminate()
        server.communicate(timeout=2)
        kv_dir.cleanup()

    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        unavailable_port = probe.getsockname()[1]
    unavailable = subprocess.Popen(
        [sys.argv[1], str(unavailable_port)], stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={name: value for name, value in os.environ.items()
             if name != "FORTIS_ENABLE_DIAGNOSTIC_PROBES"}
            | {"FORTIS_KV_DIR": os.path.join(kv_dir.name, "absent")},
    )
    try:
        for _ in range(100):
            try:
                status, _, body = exchange(
                    unavailable_port, b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n"
                )
                assert status == 503 and json.loads(body) == {"ok": False}
                break
            except OSError:
                time.sleep(0.02)
        else:
            raise AssertionError("unavailable-KV server did not listen")
        status, _, body = exchange(unavailable_port,
            b"GET /errorsz HTTP/1.1\r\nHost: localhost\r\n\r\n")
        assert status == 200
        diagnostics = json.loads(body)["operational"]
        assert not diagnostics["persisted"] and diagnostics["writeFailed"]
        assert any(row["code"] == "error_board_read_failed" for row in diagnostics["records"])
        for auth_header in (b"", b"Authorization: Bearer malformed\r\n"):
            status, _, body = exchange(unavailable_port,
                b"POST /v1/devices/rotate HTTP/1.1\r\nHost: localhost\r\n" + auth_header + b"\r\n")
            assert status == 401 and json.loads(body) == {"error": "UNAUTHORIZED"}
    finally:
        unavailable.terminate()
        unavailable.communicate(timeout=2)

    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        required_port = probe.getsockname()[1]
    required_kv = tempfile.TemporaryDirectory(prefix="fortis-required-")
    required = subprocess.Popen(
        [sys.argv[1], str(required_port)], stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={name: value for name, value in os.environ.items()
             if name != "FORTIS_ENABLE_DIAGNOSTIC_PROBES"}
            | {"FORTIS_KV_DIR": required_kv.name, "ATTESTATION_MODE": "required"},
    )
    try:
        for _ in range(100):
            if required.poll() is not None:
                raise AssertionError("required-mode server exited before listening")
            try:
                if exchange(required_port, b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n")[0] == 200:
                    break
            except OSError:
                time.sleep(0.02)
        else:
            raise AssertionError("required-mode server did not listen")

        # Default configuration: the diagnostic probes are not registered, so
        # every method gets the router's generic 404 and nothing reaches the
        # KV counter, storage probe, outbound network or signature recovery.
        # Liveness and the request-local self-checks stay available.
        for method in (b"GET", b"HEAD", b"POST"):
            for path in (b"/probe/kv", b"/probe/storage", b"/probe/platform",
                         b"/probe/tls", b"/probe/evm"):
                request = (method + b" " + path + b" HTTP/1.1\r\nHost: localhost\r\n"
                           + (b"Content-Length: 0\r\n" if method == b"POST" else b"")
                           + b"\r\n")
                status, _, body = exchange(required_port, request)
                assert status == 404, (method, path, status)
                assert method == b"HEAD" or json.loads(body) == {"error": "NOT_FOUND"}, (method, path)
        assert exchange(required_port,
            b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n")[0] == 200
        status, _, body = exchange(required_port,
            b"GET /probe HTTP/1.1\r\nHost: localhost\r\n\r\n")
        assert status == 200 and json.loads(body)["service"] == "fortis_node"
        assert exchange(required_port,
            b"GET /probe/representation HTTP/1.1\r\nHost: localhost\r\n\r\n")[0] == 200
        default_wire = bytes.fromhex("0a0a616c6963652e6461736812056162636465")
        status, _, body = exchange(required_port,
            b"POST /probe/protobuf HTTP/1.1\r\nHost: localhost\r\n"
            b"Content-Type: application/protobuf\r\nContent-Length: "
            + str(len(default_wire)).encode("ascii") + b"\r\n\r\n" + default_wire)
        assert status == 200 and body == default_wire

        def required_enroll(payload: bytes, host: str = "127.0.0.1") -> tuple[int, dict[str, str], bytes]:
            request = (b"POST /v1/devices HTTP/1.1\r\nHost: localhost\r\n"
                       b"Content-Type: application/json\r\nContent-Length: "
                       + str(len(payload)).encode() + b"\r\n\r\n" + payload)
            return exchange(required_port, request, host)

        payload_cases = [
            ("appVersion51", {"platform": "android", "appVersion": "A" * 51}),
            ("challenge15", {"platform": "android", "attestation": {
                "provider": "development", "challenge": "B" * 15}}),
            ("challenge201", {"platform": "android", "attestation": {
                "provider": "development", "challenge": "B" * 201}}),
            ("attestationToken32001", {"platform": "android", "attestation": {
                "provider": "development", "challenge": "B" * 16, "token": "T" * 32001}}),
        ]
        for name, payload in payload_cases:
            status, _, body = required_enroll(json.dumps(payload, separators=(",", ":")).encode())
            expected = fixture["behavior"]["payloadBounds"][name]
            expected_body = compat_expectations.zod_refusal(expected["body"]) or expected["body"]
            assert status == expected["status"] and json.loads(body) == expected_body, name
        unicode_cases = [
            {"platform": "android", "attestation": {"provider": "development", "challenge": "界" * 201}},
            {"platform": "android", "attestation": {"provider": "development", "challenge": "B" * 16, "token": "界" * 32001}},
        ]
        expected_unicode = [400, 400]
        for payload, expected_status in zip(unicode_cases, expected_unicode):
            status, _, _ = required_enroll(json.dumps(payload, separators=(",", ":")).encode())
            assert status == expected_status
        shape_cases = [
            (b'{"platform":"ios","appVersion":null}',
             {"error": "BAD_REQUEST"}),
            (b'{"platform":"ios","surprise":true}',
             {"error": "BAD_REQUEST"}),
            (b'{"platform":"ios","attestation":{"provider":"development"}}',
             {"error": "BAD_REQUEST"}),
        ]
        for payload, expected in shape_cases:
            status, _, body = required_enroll(payload)
            assert status == 400 and json.loads(body) == expected
        valid_unicode = json.dumps({"platform": "android", "attestation": {
            "provider": "development", "challenge": "界" * 200}}, separators=(",", ":")).encode()
        status, _, body = required_enroll(valid_unicode)
        assert status == 401 and json.loads(body) == {"error": "ATTESTATION_REQUIRED"}

    finally:
        required.terminate()
        required.communicate(timeout=2)
        required_kv.cleanup()

    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        metrics_port = probe.getsockname()[1]
    metrics_kv = tempfile.TemporaryDirectory(prefix="fortis-metrics-kv-")
    metrics_token = "metrics-test-token"
    metrics_env = os.environ.copy()
    for name in ("PORT", "NODE_ENV", "ATTESTATION_MODE", "DEVICE_TOKEN_TTL_SECONDS",
                 "FORTIS_TLS_PROBE_HOST", "FORTIS_TLS_PROBE_PORT", "FORTIS_TLS_PROBE_CA",
                 "METRICS_TOKEN"):
        metrics_env.pop(name, None)
    # GET /probe/tls supplies the 5xx route sample below.
    metrics_env.update(METRICS_TOKEN=metrics_token, FORTIS_KV_DIR=metrics_kv.name,
                       FORTIS_ENABLE_DIAGNOSTIC_PROBES="1")
    metrics_server = subprocess.Popen(
        [sys.argv[1], str(metrics_port)], stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, env=metrics_env)
    try:
        for _ in range(100):
            if metrics_server.poll() is not None:
                raise AssertionError("metrics server exited before listening")
            try:
                if exchange(metrics_port,
                    b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n")[0] == 200:
                    break
            except OSError:
                time.sleep(0.02)
        else:
            raise AssertionError("metrics server did not listen")

        metrics_path = b"GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n"
        status, headers, body = exchange(metrics_port, metrics_path)
        assert status == 401 and json.loads(body) == {"error": "UNAUTHORIZED"}
        assert headers["www-authenticate"] == 'bearer realm="fortis-metrics"'
        assert b"fortis_guest_allocator" not in body
        wrong = b"GET /metrics HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer wrong\r\n\r\n"
        assert exchange(metrics_port, wrong)[0] == 401

        assert exchange(metrics_port,
            b"GET /probe/tls HTTP/1.1\r\nHost: localhost\r\n\r\n")[0] == 503
        private_path = b"GET /missing/private-wallet-address-should-not-leak HTTP/1.1\r\nHost: localhost\r\n\r\n"
        assert exchange(metrics_port, private_path)[0] == 404
        authorized = (b"GET /metrics HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer "
                      + metrics_token.encode() + b"\r\n\r\n")
        status, headers, body = exchange(metrics_port, authorized)
        text = body.decode("utf-8")
        assert status == 200
        assert headers["content-type"] == "text/plain; version=0.0.4; charset=utf-8"
        assert headers["cache-control"] == "no-store"
        assert "fortis_native_http_requests_total{route=\"GET /healthz\"}" in text
        assert "fortis_native_http_errors_total{route=\"GET /probe/tls\"} 1" in text
        assert "fortis_native_http_request_duration_ms_sum{route=\"GET /healthz\"}" in text
        assert "fortis_native_http_requests_total{route=\"other\"}" in text
        assert metrics_token not in text and "private-wallet-address-should-not-leak" not in text
        # Host builds have no Unikraft allocator and keep the route-only contract.
        assert "fortis_guest_allocator" not in text
        # The open process store adds its compaction counters (idle at boot).
        assert "fortis_kv_compaction_phase 0\n" in text
        assert "fortis_kv_compaction_runs_started_total 0\n" in text
        _, _, health = exchange(metrics_port, b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n")
        assert b"fortis_" not in health
        _, _, second_scrape = exchange(metrics_port, authorized)
        assert 'route="GET /metrics"' not in text
        assert 'route="GET /metrics"' not in second_scrape.decode()
    finally:
        metrics_server.terminate()
        metrics_server.communicate(timeout=2)
        metrics_kv.cleanup()
    print("native HTTP, KV health, and generated-protobuf smoke test passed")


if __name__ == "__main__":
    main()

