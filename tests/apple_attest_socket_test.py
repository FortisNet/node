#!/usr/bin/env python3
"""Required-mode App Attest enrollment over real sockets.

Usage: apple_attest_socket_test.py TEST_SERVER RELEASE_SERVER FIXTURE_TOOL

TEST_SERVER is build/fortis_node_apple_attest_test: the normal server
whose App Attest verifier trusts the synthetic root named by
FORTIS_TEST_APPLE_ATTEST_ROOT. RELEASE_SERVER is build/fortis_node,
which must refuse the same proofs. FIXTURE_TOOL mints synthetic attestation
tokens (tests/apple_attest_fixture_tool.cpp).

The public per-IP quota (10 per minute, persisted in the KV) bounds each
group, so every group starts a fresh KV directory.
"""

import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

TEST_SERVER, RELEASE_SERVER, TOOL = sys.argv[1], sys.argv[2], sys.argv[3]
VERSION = "1.4.2"


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def server_env(kv_dir: str, **overrides: str) -> dict[str, str]:
    env = os.environ.copy()
    for name in ("PORT", "NODE_ENV", "ATTESTATION_MODE", "DEVICE_TOKEN_TTL_SECONDS",
                 "APPLE_APP_ATTEST_ENVIRONMENT", "FORTIS_TEST_APPLE_ATTEST_ROOT",
                 "FORTIS_TLS_PROBE_HOST", "FORTIS_TLS_PROBE_PORT", "FORTIS_TLS_PROBE_CA",
                 "FORTIS_NATIVE_PRICE_ROUTE_CANARY", "METRICS_TOKEN"):
        env.pop(name, None)
    env["FORTIS_KV_DIR"] = kv_dir
    # These tests exercise strict attestation. The per-platform exceptions now
    # default to off, so pin them on unless a case overrides one.
    for name in ("FORTIS_ANDROID_ATTESTATION_REQUIRED", "FORTIS_APPLE_ATTESTATION_REQUIRED",
                 "FORTIS_DESKTOP_ATTESTATION_REQUIRED"):
        env[name] = "true"
    env.update(overrides)
    return env


def start(binary: str, port: int, env: dict[str, str]) -> subprocess.Popen:
    server = subprocess.Popen([binary, str(port)], stdout=subprocess.DEVNULL,
                              stderr=subprocess.PIPE, env=env)
    for _ in range(250):
        if server.poll() is not None:
            raise AssertionError("server exited: " + server.stderr.read().decode(errors="replace"))
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return server
        except OSError:
            time.sleep(0.02)
    server.kill()
    raise AssertionError("server did not listen")


def stop(server: subprocess.Popen, hard: bool = False) -> None:
    if server.poll() is None:
        server.send_signal(signal.SIGKILL if hard else signal.SIGTERM)
    server.communicate(timeout=10)


def read_response(client: socket.socket) -> tuple[int, dict[str, str], bytes]:
    result = bytearray()
    while chunk := client.recv(65536):
        result.extend(chunk)
    header, body = bytes(result).split(b"\r\n\r\n", 1)
    lines = header.decode("ascii").split("\r\n")
    headers = dict(line.lower().split(": ", 1) for line in lines[1:])
    assert len(body) == int(headers.get("content-length", len(body)))
    return int(lines[0].split()[1]), headers, body


def exchange(port: int, request: bytes) -> tuple[int, dict[str, str], bytes]:
    with socket.create_connection(("127.0.0.1", port), timeout=2) as client:
        client.settimeout(15)
        client.sendall(request)
        return read_response(client)


def challenge(port: int) -> str:
    status, _, body = exchange(port, b"POST /v1/devices/challenge HTTP/1.1\r\nHost: localhost\r\n\r\n")
    assert status == 200, (status, body)
    value = json.loads(body)["challenge"]
    assert re.fullmatch(r"[A-Za-z0-9_-]{43}", value)
    return value


def mint(ca_dir: str, value: str, *options: str) -> str:
    output = subprocess.run([TOOL, "mint", ca_dir, value, *options], check=True,
                            capture_output=True, text=True).stdout.strip()
    assert re.fullmatch(r"[A-Za-z0-9_-]+", output), output
    return output


def json_request(value: str, token: str | None, platform: str = "ios",
                 provider: str = "apple-app-attest", version: str | None = VERSION) -> bytes:
    attestation: dict = {"provider": provider, "challenge": value}
    if token is not None:
        attestation["token"] = token
    payload: dict = {"platform": platform, "attestation": attestation}
    if version is not None:
        payload["appVersion"] = version
    body = json.dumps(payload, separators=(",", ":")).encode()
    return (b"POST /v1/devices HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
            b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body)


def varint(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def field(tag: int, data: bytes) -> bytes:
    return bytes([tag << 3 | 2]) + varint(len(data)) + data


def protobuf_request(value: str, token: str) -> bytes:
    attestation = (field(1, b"apple-app-attest") + field(2, value.encode())
                   + field(3, token.encode()))
    body = field(1, b"ios") + field(2, VERSION.encode()) + field(3, attestation)
    return (b"POST /v1/devices HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/protobuf\r\n"
            b"Accept: application/protobuf\r\nContent-Length: " + str(len(body)).encode()
            + b"\r\n\r\n" + body)


def enroll(port: int, request: bytes) -> tuple[int, dict]:
    status, headers, body = exchange(port, request)
    if headers["content-type"] == "application/protobuf":
        assert status == 201 and body[28:30] == b"\x12\x36", (status, body)
        return status, {"deviceToken": body[30:84].decode("ascii")}
    return status, json.loads(body)


def expect_error(port: int, request: bytes, status: int, error: str) -> None:
    got, body = enroll(port, request)
    assert (got, body) == (status, {"error": error}), (got, body, error)


def expect_enrolled(port: int, request: bytes) -> str:
    status, body = enroll(port, request)
    assert status == 201, (status, body)
    token = body["deviceToken"]
    assert re.fullmatch(r"fintel_dev_[A-Za-z0-9_-]{43}", token), body
    return token


def rotate(port: int, token: str) -> str:
    status, _, body = exchange(port, b"POST /v1/devices/rotate HTTP/1.1\r\nHost: localhost\r\n"
                               + ("Authorization: Bearer " + token + "\r\n\r\n").encode())
    assert status == 200, (status, body)
    return json.loads(body)["deviceToken"]


def rotate_status(port: int, token: str) -> tuple[int, bytes]:
    status, _, body = exchange(port, b"POST /v1/devices/rotate HTTP/1.1\r\nHost: localhost\r\n"
                               + ("Authorization: Bearer " + token + "\r\n\r\n").encode())
    return status, body


class Server:
    def __init__(self, binary: str, kv_dir: str, **env: str) -> None:
        self.binary, self.kv_dir, self.env = binary, kv_dir, env
        self.port = free_port()
        self.process = start(binary, self.port, server_env(kv_dir, **env))

    def restart(self, hard: bool) -> None:
        stop(self.process, hard=hard)
        self.process = start(self.binary, self.port, server_env(self.kv_dir, **self.env))

    def close(self, hard: bool = False) -> None:
        stop(self.process, hard=hard)


def required(ca_dir: str) -> dict[str, str]:
    return {"ATTESTATION_MODE": "required", "FORTIS_TEST_APPLE_ATTEST_ROOT": ca_dir + "/root.der"}


def group_contract(ca_dir: str) -> None:
    with tempfile.TemporaryDirectory() as kv:
        server = Server(TEST_SERVER, kv, **required(ca_dir))
        try:
            port = server.port
            value = challenge(port)
            valid = mint(ca_dir, value)
            # Refusals that must leave the challenge live.
            expect_error(port, json_request(value, "AAAA"), 401, "ATTESTATION_INVALID")
            expect_error(port, json_request(value, mint(ca_dir, value, "--bundle", "58",
                                                        "--envelope-bundle", "57")),
                         401, "ATTESTATION_INVALID")
            expect_error(port, json_request(value, valid, platform="android"), 401,
                         "ATTESTATION_INVALID")
            expect_error(port, json_request(value, valid, version=None), 401, "ATTESTATION_INVALID")
            # This server has no FORTIS_PLAY_INTEGRITY_* configuration, so the
            # Play Integrity branch answers its unconfigured ATTESTATION_REQUIRED
            # (tests/play_integrity_enrollment_socket_test.py covers it configured).
            expect_error(port, json_request(value, valid, provider="play-integrity"), 401,
                         "ATTESTATION_REQUIRED")
            # The earlier refusals did not consume it.
            status, body = enroll(port, json_request(value, valid))
            assert status == 201 and set(body) == {"deviceId", "deviceToken", "expiresAt"}, body
            expect_error(port, json_request(value, valid), 401, "ATTESTATION_EXPIRED")
            rotated = rotate(port, body["deviceToken"])
            assert rotate_status(port, body["deviceToken"])[0] == 401
            rotate(port, rotated)
            second = challenge(port)
            expect_enrolled(port, protobuf_request(second, mint(ca_dir, second)))
        finally:
            server.close()
    with tempfile.TemporaryDirectory() as kv:
        server = Server(TEST_SERVER, kv, **required(ca_dir))
        try:
            port = server.port
            value = challenge(port)
            expect_error(port, json_request(value, None, provider="development"), 401,
                         "ATTESTATION_REQUIRED")
            payload = json.dumps({"platform": "ios", "appVersion": VERSION}).encode()
            expect_error(port, b"POST /v1/devices HTTP/1.1\r\nHost: localhost\r\nContent-Type: "
                         b"application/json\r\nContent-Length: " + str(len(payload)).encode()
                         + b"\r\n\r\n" + payload, 401, "ATTESTATION_REQUIRED")
            expect_error(port, json_request(value, None), 401, "ATTESTATION_INVALID")
            expect_error(port, json_request("A" * 43, mint(ca_dir, "A" * 43)), 401,
                         "ATTESTATION_EXPIRED")
            expect_error(port, json_request(value, mint(ca_dir, value, "--expired")), 401,
                         "ATTESTATION_INVALID")
            expect_error(port, json_request(value, mint(ca_dir, value, "--raw-client-data")), 401,
                         "ATTESTATION_INVALID")
            expect_error(port, json_request(value, mint(ca_dir, value, "--app-id",
                                                        "C7Z98WBK8H.io.fortisnet.other")),
                         401, "ATTESTATION_INVALID")
            expect_error(port, json_request(value, mint(ca_dir, value, "--category", "3")), 401,
                         "ATTESTATION_INVALID")
            expect_enrolled(port, json_request(value, mint(ca_dir, value)))
        finally:
            server.close()
    print("contract: refusals keep the challenge, 201 JSON and protobuf, replay, rotation, "
          "non-Apple providers keep ATTESTATION_REQUIRED: ok")


def race_round(ca_dir: str, label: str) -> tuple[int, int]:
    with tempfile.TemporaryDirectory() as kv:
        server = Server(TEST_SERVER, kv, **required(ca_dir))
        try:
            port = server.port
            value = challenge(port)
            request = json_request(value, mint(ca_dir, value))
            contenders = 9
            clients = []
            for _ in range(contenders):
                client = socket.create_connection(("127.0.0.1", port), timeout=2)
                client.settimeout(15)
                client.sendall(request[:-1])
                clients.append(client)
            barrier = threading.Barrier(contenders)
            results: list = [None] * contenders
            errors: list[BaseException] = []

            def fire(index: int) -> None:
                try:
                    barrier.wait()
                    clients[index].sendall(request[-1:])
                    results[index] = read_response(clients[index])
                except BaseException as error:  # noqa: BLE001 - reported below
                    errors.append(error)
                finally:
                    clients[index].close()

            threads = [threading.Thread(target=fire, args=(i,)) for i in range(contenders)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(20)
            assert not errors, errors
            winners = [r for r in results if r[0] == 201]
            assert len(winners) == 1, (label, [r[0] for r in results])
            busy = 0
            for status, headers, body in results:
                if status == 201:
                    continue
                error = json.loads(body)
                if status == 503:
                    assert error == {"error": "ATTESTATION_BUSY"} and headers["retry-after"] == "1"
                    busy += 1
                else:
                    assert (status, error) == (401, {"error": "ATTESTATION_EXPIRED"}), (status, body)
            rotate(port, json.loads(winners[0][2])["deviceToken"])
            return contenders - 1 - busy, busy
        finally:
            server.close()


def group_race(ca_dir: str) -> None:
    expired = busy = 0
    for index in range(4):
        e, b = race_round(ca_dir, f"race {index}")
        expired += e
        busy += b
    print(f"race: 4 rounds x 9 simultaneous uses of one proof, one 201 each "
          f"({expired} ATTESTATION_EXPIRED, {busy} ATTESTATION_BUSY): ok")


def group_restart(ca_dir: str) -> None:
    with tempfile.TemporaryDirectory() as kv, tempfile.TemporaryDirectory() as keys:
        server = Server(TEST_SERVER, kv, **required(ca_dir))
        try:
            port = server.port
            key = keys + "/leaf.pem"
            first = challenge(port)
            # A SIGKILL after minting keeps the challenge.
            server.restart(hard=True)
            first_request = json_request(first, mint(ca_dir, first, "--save-key", key))
            token = expect_enrolled(port, first_request)
            server.restart(hard=True)
            token = rotate(port, token)
            server.restart(hard=True)
            expect_error(port, first_request, 401, "ATTESTATION_EXPIRED")
            token = rotate(port, token)
            # The attested key is spent: re-attesting it for a new challenge is
            # refused and leaves that challenge live for a fresh key.
            second = challenge(port)
            expect_error(port, json_request(second, mint(ca_dir, second, "--key", key)), 401,
                         "ATTESTATION_INVALID")
            server.restart(hard=True)
            expect_enrolled(port, json_request(second, mint(ca_dir, second)))
            rotate(port, token)
        finally:
            server.close(hard=True)
    print("hard restart: challenge after SIGKILL, bearer and rotation after SIGKILL, replay and "
          "key reuse refused after SIGKILL: ok")


def group_legacy(ca_dir: str) -> None:
    with tempfile.TemporaryDirectory() as kv:
        server = Server(TEST_SERVER, kv, **required(ca_dir))
        try:
            value = challenge(server.port)
            request = json_request(value, mint(ca_dir, value, "--legacy"))
            token = expect_enrolled(server.port, request)
            server.restart(hard=True)
            expect_error(server.port, request, 401, "ATTESTATION_EXPIRED")
            rotate(server.port, token)
        finally:
            server.close()
    print("legacy App Attest object: socket enrollment, replay refusal and restart: ok")


def group_release_and_development(ca_dir: str) -> None:
    with tempfile.TemporaryDirectory() as kv:
        # The release binary ignores the test-root variable and refuses the
        # synthetic proof without consuming the challenge...
        release = Server(RELEASE_SERVER, kv, **required(ca_dir))
        try:
            value = challenge(release.port)
            request = json_request(value, mint(ca_dir, value))
            expect_error(release.port, request, 401, "ATTESTATION_INVALID")
        finally:
            release.close()
        # ...which the test-root server then accepts on the same KV.
        server = Server(TEST_SERVER, kv, **required(ca_dir))
        try:
            expect_enrolled(server.port, request)
        finally:
            server.close()
    with tempfile.TemporaryDirectory() as kv:
        # The server accepts release clients by verifying their App Attest
        # proof, while loopback development enrollment still works.
        server = Server(TEST_SERVER, kv, ATTESTATION_MODE="development",
                        FORTIS_TEST_APPLE_ATTEST_ROOT=ca_dir + "/root.der")
        try:
            port = server.port
            value = challenge(port)
            expect_error(port, json_request(value, "AAAA"), 401, "ATTESTATION_INVALID")
            expect_enrolled(port, json_request(value, mint(ca_dir, value)))
            expect_error(port, json_request(value, mint(ca_dir, value)), 401,
                         "ATTESTATION_EXPIRED")
            development_challenge = challenge(port)
            expect_enrolled(port, json_request(development_challenge, None,
                                               provider="development"))
        finally:
            server.close()
    with tempfile.TemporaryDirectory() as kv:
        server = Server(TEST_SERVER, kv, APPLE_APP_ATTEST_ENVIRONMENT="development",
                        **required(ca_dir))
        try:
            port = server.port
            value = challenge(port)
            expect_error(port, json_request(value, mint(ca_dir, value)), 401, "ATTESTATION_INVALID")
            expect_enrolled(port, json_request(value, mint(ca_dir, value, "--development",
                                                           "--category", "3")))
        finally:
            server.close()
    with tempfile.TemporaryDirectory() as kv:
        refused = subprocess.run([RELEASE_SERVER, str(free_port())], capture_output=True,
                                 env=server_env(kv, NODE_ENV="production",
                                                APPLE_APP_ATTEST_ENVIRONMENT="development"),
                                 timeout=10)
        assert refused.returncode != 0
        assert b"APPLE_APP_ATTEST_ENVIRONMENT=development is not allowed in production" in (
            refused.stderr + refused.stdout), refused
    print("release binary refuses the synthetic root without consuming; development mode "
          "verifies App Attest and preserves local enrollment: ok")


def main() -> None:
    with tempfile.TemporaryDirectory() as ca_dir:
        subprocess.run([TOOL, "init", ca_dir], check=True)
        group_contract(ca_dir)
        group_race(ca_dir)
        group_restart(ca_dir)
        group_legacy(ca_dir)
        group_release_and_development(ca_dir)
    print("Apple App Attest real-socket test passed")


if __name__ == "__main__":
    main()
