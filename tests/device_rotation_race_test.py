#!/usr/bin/env python3
"""Concurrent POST /v1/devices/rotate with one bearer over real sockets.

Atomic bearer rotation: each round fires eight rotations (JSON and protobuf
Accept alternating) that carry the same bearer and hold back their final byte
until a barrier releases them together. Exactly one may get 200 and every
other answer is a 401, never a 500. The winner's bearer carries the next
round; the old bearer never authenticates again.

HttpRouter::Dispatch runs every API handler under one route mutex, so today
the eight requests serialize and every loser authenticates after the winner
committed: 401 UNAUTHORIZED (its hash key is gone). DeviceAuthStore::Rotate's
conditional write is what keeps one winner once that lock is narrowed, and is
covered by tests/device_rotation_test.cpp. The handler's own 401
SESSION_EXPIRED (a request that authenticated, then lost the conditional
write) is driven here through build/fortis_node_rotation_test, whose
FORTIS_TEST_ROTATION_COMPETE seam commits a competing rotation between the
request's row snapshot and its write; its FORTIS_TEST_ROTATION_PAUSE_MS seam
holds a rotation there for a SIGKILL. Also covered: a SIGKILL right after a
race (the winner's bearer survives replay), the Origin 403 and required-mode
401 that refuse before any rotation, and the mode switch on one KV directory.
"""

import json
import os
import re
import socket
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from device_challenge_race_test import (exchange, free_port, json_enrollment, mint,  # noqa: E402
                                        read_response, server_env, start, stop)

ROUNDS = int(os.environ.get("FORTIS_ROTATION_ROUNDS", "8"))
CONTENDERS = 8
TOKEN = re.compile(r"fintel_dev_[A-Za-z0-9_-]{43}")
EXPIRES = re.compile(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z")


def rotation(token: str, protobuf: bool, origin: bool = False) -> bytes:
    return (b"POST /v1/devices/rotate HTTP/1.1\r\nHost: localhost\r\n"
            + (b"Accept: application/protobuf\r\n" if protobuf else b"")
            + (b"Origin: https://example.test\r\n" if origin else b"")
            + ("Authorization: Bearer " + token + "\r\n\r\n").encode())


def enroll(port: int) -> str:
    status, _, body = exchange(port, json_enrollment(mint(port)))
    assert status == 201, (status, body)
    return json.loads(body)["deviceToken"]


def winner_token(status: int, headers: dict[str, str], body: bytes) -> str:
    assert status == 200 and headers["vary"] == "accept", (status, headers)
    assert headers["x-rate-limit-cost"] == "1" and "x-rate-limit-remaining" in headers
    if headers["content-type"] == "application/protobuf":
        assert len(body) == 82 and body[:2] == b"\x0a\x36" and body[56:58] == b"\x12\x18", body
        token, expires = body[2:56].decode("ascii"), body[58:82].decode("ascii")
    else:
        assert headers["content-type"] == "application/json; charset=utf-8"
        decoded = json.loads(body)
        assert set(decoded) == {"deviceToken", "expiresAt"}
        token, expires = decoded["deviceToken"], decoded["expiresAt"]
    assert TOKEN.fullmatch(token) and EXPIRES.fullmatch(expires), (token, expires)
    return token


def rotate(port: int, token: str, protobuf: bool = False) -> tuple[int, dict[str, str], bytes]:
    return exchange(port, rotation(token, protobuf))


def race(port: int, token: str) -> list[tuple[int, dict[str, str], bytes]]:
    """Sends every request but its last byte, then releases them together."""
    requests = [rotation(token, index % 2 == 1) for index in range(CONTENDERS)]
    clients = []
    for request in requests:
        client = socket.create_connection(("127.0.0.1", port), timeout=2)
        client.settimeout(15)
        client.sendall(request[:-1])
        clients.append(client)
    barrier = threading.Barrier(CONTENDERS)
    results: list = [None] * CONTENDERS
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

    threads = [threading.Thread(target=fire, args=(index,)) for index in range(CONTENDERS)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(20)
    assert not errors, errors
    return results


def check_round(port: int, token: str, results: list, label: str) -> tuple[str, int, int]:
    """Returns the winner's bearer and the SESSION_EXPIRED/UNAUTHORIZED counts."""
    statuses = [result[0] for result in results]
    winners = [result for result in results if result[0] == 200]
    assert len(winners) == 1, (label, statuses)
    expired = unauthorized = 0
    for status, headers, body in results:
        if status == 200:
            continue
        assert status == 401, (label, statuses, body)
        assert headers["content-type"] == "application/json; charset=utf-8"
        error = json.loads(body)
        if error == {"error": "SESSION_EXPIRED"}:
            # A rotation can win during the two policy reads (uncharged)
            # or during the conditional write (already charged).
            expired += 1
            if "x-rate-limit-cost" in headers:
                assert headers["x-rate-limit-cost"] == "1" and "x-rate-limit-remaining" in headers
        else:
            # Arrived after the winner committed: the old hash key is gone.
            assert error == {"error": "UNAUTHORIZED"}, (label, body)
            unauthorized += 1
            assert "x-rate-limit-cost" not in headers
    new_token = winner_token(*winners[0])
    assert new_token != token
    # The consumed bearer never authenticates again.
    status, _, body = rotate(port, token)
    assert status == 401 and json.loads(body) == {"error": "UNAUTHORIZED"}, (label, status, body)
    return new_token, expired, unauthorized


def run_rounds(port: int, token: str, rounds: int, label: str) -> str:
    total_expired = total_unauthorized = 0
    for round_index in range(rounds):
        token, expired, unauthorized = check_round(port, token, race(port, token),
                                                   f"{label} round {round_index}")
        total_expired += expired
        total_unauthorized += unauthorized
    print(f"{label}: {rounds} rounds x {CONTENDERS}: {rounds} x 200, "
          f"{total_expired} x 401 SESSION_EXPIRED, {total_unauthorized} x 401 UNAUTHORIZED, 0 x 500")
    return token


def expect_session_expired(status: int, headers: dict[str, str], body: bytes,
                           remaining: str | None = None) -> None:
    assert status == 401 and json.loads(body) == {"error": "SESSION_EXPIRED"}, (status, body)
    assert headers["content-type"] == "application/json; charset=utf-8"
    # The policy charged the quota before the conditional write lost.
    assert headers["x-rate-limit-cost"] == "1", headers
    if remaining is not None:
        assert headers["x-rate-limit-remaining"] == remaining, headers


def main() -> None:
    production, seam = sys.argv[1], sys.argv[2]

    # Production object: free races, then a protobuf follow-up and Origin.
    with tempfile.TemporaryDirectory(prefix="fortis-rotate-race-") as kv_dir:
        port = free_port()
        server = start(production, port, server_env(kv_dir))
        try:
            token = run_rounds(port, enroll(port), ROUNDS, "production server")
            status, headers, body = rotate(port, token, protobuf=True)
            token = winner_token(status, headers, body)
            # Origin policy refuses before any rotation: the bearer stays current.
            status, headers, body = exchange(port, rotation(token, False, origin=True))
            assert status == 403 and json.loads(body) == {"error": "BROWSER_ORIGIN_FORBIDDEN"}
            assert "x-rate-limit-cost" not in headers
            winner_token(*rotate(port, token))
        finally:
            stop(server)

    # A competing rotation commits inside the request's snapshot window: the
    # request answers 401 SESSION_EXPIRED (JSON body for both Accepts) and
    # reveals no bearer; the consumed bearer is then UNAUTHORIZED.
    with tempfile.TemporaryDirectory(prefix="fortis-rotate-compete-") as kv_dir:
        port = free_port()
        server = start(seam, port, server_env(kv_dir, FORTIS_TEST_ROTATION_COMPETE="1"))
        try:
            for protobuf in (False, True):
                token = enroll(port)
                expect_session_expired(*rotate(port, token, protobuf), remaining="239")
                status, headers, body = rotate(port, token, protobuf)
                assert status == 401 and json.loads(body) == {"error": "UNAUTHORIZED"}
                assert "x-rate-limit-cost" not in headers
            token = enroll(port)
            results = race(port, token)
            assert all(result[0] == 401 for result in results), [r[0] for r in results]
            expired = [result for result in results
                       if json.loads(result[2]) == {"error": "SESSION_EXPIRED"}]
            assert expired, [result[2] for result in results]
            charged = [result for result in expired if "x-rate-limit-cost" in result[1]]
            assert charged
            for result in charged:
                expect_session_expired(*result)
            assert sum(json.loads(result[2]) == {"error": "UNAUTHORIZED"}
                       for result in results) == CONTENDERS - len(expired)
        finally:
            stop(server)
    print("competing commit: 401 SESSION_EXPIRED with quota headers (JSON and protobuf "
          "requests), no bearer revealed, 0 x 500")

    # Hard restarts on the pause seam.
    with tempfile.TemporaryDirectory(prefix="fortis-rotate-paused-") as kv_dir:
        port = free_port()
        server = start(seam, port, server_env(kv_dir, FORTIS_TEST_ROTATION_PAUSE_MS="100"))
        try:
            token = run_rounds(port, enroll(port), 2, "pause seam server")
            # One more race, then SIGKILL before anything else is written.
            results = race(port, token)
        finally:
            stop(server, hard=True)
        token = check_round_after_restart(production, kv_dir, token, results)

        # SIGKILL while a rotation sits between its snapshot and its commit:
        # the client never receives a bearer and the old one stays current.
        server = start(seam, port, server_env(kv_dir, FORTIS_TEST_ROTATION_PAUSE_MS="3000"))
        try:
            client = socket.create_connection(("127.0.0.1", port), timeout=2)
            client.settimeout(10)
            client.sendall(rotation(token, False))
            time.sleep(0.8)
        finally:
            stop(server, hard=True)
        leftover = b""
        try:
            leftover = client.recv(4096)
        except OSError:
            pass
        client.close()
        assert leftover == b"", leftover
        server = start(production, port, server_env(kv_dir))
        try:
            token = winner_token(*rotate(port, token, protobuf=True))
        finally:
            stop(server)
        print("hard restart mid-rotation: nothing committed, no bearer revealed, old bearer rotates")

        # Required mode refuses a development bearer before rotating it.
        server = start(production, port, server_env(kv_dir, ATTESTATION_MODE="required"))
        try:
            for protobuf in (False, True):
                status, headers, body = rotate(port, token, protobuf)
                assert status == 401 and json.loads(body) == {"error": "ATTESTATION_REQUIRED"}
                assert "x-rate-limit-cost" not in headers
        finally:
            stop(server)
        server = start(production, port, server_env(kv_dir))
        try:
            winner_token(*rotate(port, token))
        finally:
            stop(server)
        print("required mode: ATTESTATION_REQUIRED without rotating; development bearer intact")
    print("device rotation real-socket race test passed")


def check_round_after_restart(binary: str, kv_dir: str, token: str, results: list) -> str:
    """Checks a race whose server was SIGKILLed right after it answered."""
    port = free_port()
    server = start(binary, port, server_env(kv_dir))
    try:
        new_token, expired, unauthorized = check_round(port, token, results, "before SIGKILL")
        rotated = winner_token(*rotate(port, new_token))
    finally:
        stop(server)
    print(f"hard restart after race: winner's bearer survived replay "
          f"({expired} SESSION_EXPIRED, {unauthorized} UNAUTHORIZED)")
    return rotated


if __name__ == "__main__":
    main()
