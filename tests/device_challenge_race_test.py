#!/usr/bin/env python3
"""Concurrent development enrollments of one challenge over real sockets.

Each round mints one challenge, then fires nine
enrollments (JSON and protobuf) that all hold back their final byte until a
barrier releases them together. At most one may get 201; every other answer
must be the existing 401 ATTESTATION_EXPIRED body. The winner's bearer must
work, including after a SIGKILL restart, and the challenge stays consumed
across that restart. A required-mode server still refuses before any
challenge is touched. The per-IP public quota (10 per minute, persisted in the
KV) bounds a round to one challenge plus nine enrollments, so each round uses
a fresh server and KV directory.
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

ROUNDS = int(os.environ.get("FORTIS_RACE_ROUNDS", "4"))
CONTENDERS = 9


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def server_env(kv_dir: str, **overrides: str) -> dict[str, str]:
    env = os.environ.copy()
    for name in ("PORT", "NODE_ENV", "ATTESTATION_MODE", "DEVICE_TOKEN_TTL_SECONDS",
                 "FORTIS_TLS_PROBE_HOST", "FORTIS_TLS_PROBE_PORT", "FORTIS_TLS_PROBE_CA",
                 "FORTIS_ENABLE_PROVIDER_TEST_ROUTE", "FORTIS_TEST_UPSTREAM_PORT",
                 "FORTIS_TEST_TLS_FAILURE_PORT", "FORTIS_TEST_UPSTREAM_CA",
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
        if hard:
            server.send_signal(signal.SIGKILL)
        else:
            server.terminate()
    server.communicate(timeout=5)


def read_response(client: socket.socket) -> tuple[int, dict[str, str], bytes]:
    result = bytearray()
    while chunk := client.recv(4096):
        result.extend(chunk)
    header, body = bytes(result).split(b"\r\n\r\n", 1)
    lines = header.decode("ascii").split("\r\n")
    headers = dict(line.lower().split(": ", 1) for line in lines[1:])
    assert len(body) == int(headers.get("content-length", len(body)))
    return int(lines[0].split()[1]), headers, body


def exchange(port: int, request: bytes) -> tuple[int, dict[str, str], bytes]:
    with socket.create_connection(("127.0.0.1", port), timeout=2) as client:
        client.settimeout(10)
        client.sendall(request)
        return read_response(client)


def mint(port: int) -> str:
    status, _, body = exchange(port, b"POST /v1/devices/challenge HTTP/1.1\r\nHost: localhost\r\n\r\n")
    assert status == 200, (status, body)
    challenge = json.loads(body)["challenge"]
    assert re.fullmatch(r"[A-Za-z0-9_-]{43}", challenge)
    return challenge


def json_enrollment(challenge: str) -> bytes:
    body = json.dumps({"platform": "android", "appVersion": "4.5.6",
                       "attestation": {"provider": "development", "challenge": challenge}},
                      separators=(",", ":")).encode()
    return (b"POST /v1/devices HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
            b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body)


def protobuf_enrollment(challenge: str) -> bytes:
    nested = bytes.fromhex("0a0b646576656c6f706d656e74") + b"\x12\x2b" + challenge.encode("ascii")
    body = (bytes.fromhex("0a07616e64726f6964") + bytes.fromhex("1205342e352e36")
            + b"\x1a" + bytes([len(nested)]) + nested)
    return (b"POST /v1/devices HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/protobuf\r\n"
            b"Accept: application/protobuf\r\nContent-Length: " + str(len(body)).encode()
            + b"\r\n\r\n" + body)


def race(port: int, challenge: str, contenders: int) -> list[tuple[int, dict[str, str], bytes]]:
    """Sends every request but its last byte, then releases them together."""
    requests = [json_enrollment(challenge) if index % 2 == 0 else protobuf_enrollment(challenge)
                for index in range(contenders)]
    clients = []
    for request in requests:
        client = socket.create_connection(("127.0.0.1", port), timeout=2)
        client.settimeout(10)
        client.sendall(request[:-1])
        clients.append(client)
    barrier = threading.Barrier(contenders)
    results: list = [None] * contenders
    errors: list[BaseException] = []

    def fire(index: int) -> None:
        try:
            barrier.wait()
            clients[index].sendall(requests[index][-1:])
            results[index] = read_response(clients[index])
        except BaseException as error:  # noqa: BLE001 - reported below
            errors.append(error)
        finally:
            clients[index].close()

    threads = [threading.Thread(target=fire, args=(index,)) for index in range(contenders)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(15)
    assert not errors, errors
    return results


def check_round(results: list, label: str) -> str:
    winners = [result for result in results if result[0] == 201]
    losers = [result for result in results if result[0] != 201]
    assert len(winners) <= 1, (label, [result[0] for result in results])
    assert len(winners) == 1, (label, "no enrollment succeeded", [result[0] for result in results])
    for status, headers, body in losers:
        assert status == 401 and json.loads(body) == {"error": "ATTESTATION_EXPIRED"}, (label, status, body)
        assert headers["content-type"] == "application/json; charset=utf-8"
    status, headers, body = winners[0]
    if headers["content-type"] == "application/protobuf":
        assert len(body) == 110 and body[28:30] == b"\x12\x36"
        token = body[30:84].decode("ascii")
    else:
        token = json.loads(body)["deviceToken"]
    assert re.fullmatch(r"fintel_dev_[A-Za-z0-9_-]{43}", token)
    return token


def rotate(port: int, token: str) -> tuple[int, bytes]:
    status, _, body = exchange(port, b"POST /v1/devices/rotate HTTP/1.1\r\nHost: localhost\r\n"
                               + ("Authorization: Bearer " + token + "\r\n\r\n").encode())
    return status, body


def main() -> None:
    binary = sys.argv[1]
    # Plain rounds: one winner, the rest see the challenge as consumed.
    for round_index in range(ROUNDS):
        with tempfile.TemporaryDirectory(prefix="fortis-race-") as kv_dir:
            port = free_port()
            server = start(binary, port, server_env(kv_dir))
            try:
                challenge = mint(port)
                token = check_round(race(port, challenge, CONTENDERS), f"round {round_index}")
                status, body = rotate(port, token)
                assert status == 200 and json.loads(body)["deviceToken"] != token
            finally:
                stop(server)
        print(f"round {round_index}: 1 x 201, {CONTENDERS - 1} x 401 ATTESTATION_EXPIRED")

    # Hard restart between mint and use, then again after the race.
    with tempfile.TemporaryDirectory(prefix="fortis-race-restart-") as kv_dir:
        port = free_port()
        server = start(binary, port, server_env(kv_dir))
        try:
            challenge = mint(port)
        finally:
            stop(server, hard=True)
        server = start(binary, port, server_env(kv_dir))
        try:
            token = check_round(race(port, challenge, CONTENDERS - 1), "after restart")
        finally:
            stop(server, hard=True)
        server = start(binary, port, server_env(kv_dir))
        try:
            status, _, body = exchange(port, json_enrollment(challenge))
            assert status == 401 and json.loads(body) == {"error": "ATTESTATION_EXPIRED"}
            status, body = rotate(port, token)
            assert status == 200, (status, body)
        finally:
            stop(server)
    print("hard restart: challenge survives SIGKILL unused, is consumed once, stays consumed")

    # Required mode refuses before the challenge is read or consumed.
    with tempfile.TemporaryDirectory(prefix="fortis-race-required-") as kv_dir:
        port = free_port()
        server = start(binary, port, server_env(kv_dir))
        try:
            challenge = mint(port)
        finally:
            stop(server)
        server = start(binary, port, server_env(kv_dir, ATTESTATION_MODE="required"))
        try:
            for result in race(port, challenge, 4):
                assert result[0] == 401 and json.loads(result[2]) == {"error": "ATTESTATION_REQUIRED"}
        finally:
            stop(server)
        server = start(binary, port, server_env(kv_dir))
        try:
            check_round(race(port, challenge, 4), "development after required")
        finally:
            stop(server)
    print("required mode: ATTESTATION_REQUIRED without consuming the challenge")
    print("device challenge real-socket race test passed")


if __name__ == "__main__":
    main()
