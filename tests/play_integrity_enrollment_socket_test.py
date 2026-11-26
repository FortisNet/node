#!/usr/bin/env python3
"""Required-mode Android (Play Integrity) enrollment.

Usage: play_integrity_enrollment_socket_test.py UNIT_TEST TEST_SERVER RELEASE_SERVER

UNIT_TEST is build/play_integrity_enrollment_test. TEST_SERVER is
build/fortis_node_play_integrity_test: the normal server whose Google
client (the _test_seam objects) sends both Google requests to the loopback
TLS stub named by FORTIS_TEST_PLAY_INTEGRITY_HOST/PORT. RELEASE_SERVER is
build/fortis_node, which has no such seam and is never allowed to
reach a Google host here (every release request below is refused before
any network I/O).

1. Generates the service-account RSA key, its credential file (0600), a TLS
   CA with a localhost certificate and the release certificate digest with
   openssl at test time; nothing private is stored in Git.
2. Runs the unit groups (UNIT_TEST unit FIXTURE).
3. Starts tests/play_integrity_google_stub.py. Integrity tokens carry
   their scenario, challenge and appVersion ("c3.<hex>.<hex>.<hex>.<nonce>"),
   and the stub verifies the RS256 assertion, the Bearer, the path and body,
   and recomputes the requestHash independently of the C++ code.
4. Real-socket groups. The public per-IP quota (10 per minute, challenge and
   enrollment together, in process memory) bounds each server, so every
   group starts a fresh server and KV directory. POST /v1/devices runs
   outside the router's global lock (route_runs_without_state_lock), so
   the "slow" and "race" groups hold decodes in the stub and check that
   other routes and enrollments proceed meanwhile. The route admits at most
   three Play enrollments (PlayEnrollRouteSlot), one fewer than the four
   HTTP workers: the "race", "capacity" and "slot release" groups hold three
   decodes and check that the fourth worker still answers, that a further
   enrollment is refused 503 ATTESTATION_BUSY without Google I/O and can
   retry with the same challenge and token, and that every outcome returns
   its slot.
5. Startup configuration refusals on the release binary, and the release
   isolation checks (binary, objects, Makefile and guest source list).

No live Google call is made or asserted.
"""

import base64
import json
import os
import re
import secrets
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import tls_ca

UNIT, TEST_SERVER, RELEASE_SERVER = (str(Path(arg).resolve()) for arg in sys.argv[1:4])
ROOT = Path(__file__).resolve().parent.parent
VERSION = "1.4.2"
EMAIL = "play-verifier@fortis-test.iam.gserviceaccount.com"
KEY_ID = "0123456789abcdef0123456789abcdef01234567"
SENTINEL = "SECRET-SENTINEL-c3"


# ---- Fixture -----------------------------------------------------------------

def run(*args: str) -> None:
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def make_fixture(root: Path) -> dict:
    run("openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048",
        "-out", str(root / "rsa2048.pem"))
    run("openssl", "pkey", "-in", str(root / "rsa2048.pem"), "-pubout",
        "-out", str(root / "rsa2048.pub.pem"))
    assert (root / "rsa2048.pem").read_text().startswith("-----BEGIN PRIVATE KEY-----\n")
    ca, cert, key, other_ca = tls_ca.create_certificates(root, "DNS:localhost")
    digest = secrets.token_bytes(32)
    certificate = base64.urlsafe_b64encode(digest).rstrip(b"=").decode()
    (root / "certificate.b64").write_text(certificate + "\n")
    accounts = root / "accounts"
    accounts.mkdir(mode=0o700)
    account = {"type": "service_account", "project_id": "fortis-test",
               "private_key_id": KEY_ID, "private_key": (root / "rsa2048.pem").read_text(),
               "client_email": EMAIL, "token_uri": "https://oauth2.googleapis.com/token",
               "universe_domain": "googleapis.com"}
    credential = accounts / "credential.json"
    credential.write_text(json.dumps(account))
    os.chmod(credential, 0o600)
    loose = accounts / "loose.json"
    loose.write_text(json.dumps(account))
    os.chmod(loose, 0o644)
    garbage_ca = root / "garbage-ca.pem"
    garbage_ca.write_text("not a certificate\n")
    (root / "mode").write_text("c3 1\n")
    return {"dir": root, "ca": ca, "cert": cert, "key": key, "other_ca": other_ca,
            "credential": credential, "loose": loose, "garbage_ca": garbage_ca,
            "certificate": certificate, "certificate_hex": digest.hex(":").upper()}


def hexed(text: str) -> str:
    return text.encode().hex()


def token(challenge: str, mode: str = "ok", version: str = VERSION) -> str:
    return f"c3.{hexed(mode)}.{hexed(challenge)}.{hexed(version)}.{secrets.token_hex(8)}"


# ---- Processes -------------------------------------------------------------------

def free_port() -> int:
    return tls_ca.free_port()


def server_env(kv_dir: str, **overrides: str) -> dict[str, str]:
    env = {name: value for name, value in os.environ.items()
           if not name.startswith(("FORTIS_PLAY_INTEGRITY_", "FORTIS_TEST_PLAY_INTEGRITY_"))}
    for name in ("PORT", "NODE_ENV", "ATTESTATION_MODE", "FORTIS_ANDROID_ATTESTATION_REQUIRED",
                 "DEVICE_TOKEN_TTL_SECONDS",
                 "APPLE_APP_ATTEST_ENVIRONMENT", "FORTIS_TEST_APPLE_ATTEST_ROOT",
                 "FORTIS_TLS_PROBE_HOST", "FORTIS_TLS_PROBE_PORT", "FORTIS_TLS_PROBE_CA",
                 "FORTIS_NATIVE_PRICE_ROUTE_CANARY", "METRICS_TOKEN", "FORTIS_CA_BUNDLE_PATH"):
        env.pop(name, None)
    env["FORTIS_KV_DIR"] = kv_dir
    # These tests exercise strict attestation. The per-platform exceptions now
    # default to off, so pin them on unless a case overrides one.
    for name in ("FORTIS_ANDROID_ATTESTATION_REQUIRED", "FORTIS_APPLE_ATTESTATION_REQUIRED",
                 "FORTIS_DESKTOP_ATTESTATION_REQUIRED"):
        env[name] = "true"
    env.update(overrides)
    # An override of None removes the variable (to test an unset default).
    return {name: value for name, value in env.items() if value is not None}


def play_env(fixture: dict, stub_port: int, **overrides: str) -> dict[str, str]:
    env = {"ATTESTATION_MODE": "required",
           "FORTIS_PLAY_INTEGRITY_CREDENTIAL_FILE": str(fixture["credential"]),
           "FORTIS_PLAY_INTEGRITY_CERT_SHA256": fixture["certificate"],
           "FORTIS_PLAY_INTEGRITY_CERT_SHA256_FORMAT": "base64url",
           "FORTIS_PLAY_INTEGRITY_MIN_VERSION_CODE": "40",
           "FORTIS_PLAY_INTEGRITY_CA_FILE": str(fixture["ca"]),
           "FORTIS_TEST_PLAY_INTEGRITY_HOST": "localhost",
           "FORTIS_TEST_PLAY_INTEGRITY_PORT": str(stub_port)}
    env.update(overrides)
    return {name: value for name, value in env.items() if value is not None}


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


class Server:
    def __init__(self, binary: str, kv_dir: str, env: dict[str, str]) -> None:
        self.binary, self.kv_dir, self.env = binary, kv_dir, env
        self.port = free_port()
        self.stderr = ""
        self.process = start(binary, self.port, server_env(kv_dir, **env))

    def stop(self, hard: bool = False) -> None:
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGKILL if hard else signal.SIGTERM)
        self.stderr += self.process.communicate(timeout=10)[1].decode(errors="replace")

    def restart(self, hard: bool) -> None:
        self.stop(hard)
        self.process = start(self.binary, self.port, server_env(self.kv_dir, **self.env))


# ---- HTTP --------------------------------------------------------------------------

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
        client.settimeout(30)
        client.sendall(request)
        return read_response(client)


def challenge(port: int) -> str:
    status, _, body = exchange(port, b"POST /v1/devices/challenge HTTP/1.1\r\nHost: localhost\r\n\r\n")
    assert status == 200, (status, body)
    value = json.loads(body)["challenge"]
    assert re.fullmatch(r"[A-Za-z0-9_-]{43}", value)
    return value


def json_request(value: str, integrity: str | None, platform: str = "android",
                 provider: str = "play-integrity", version: str | None = VERSION,
                 attestation: bool = True) -> bytes:
    payload: dict = {"platform": platform}
    if attestation:
        body: dict = {"provider": provider, "challenge": value}
        if integrity is not None:
            body["token"] = integrity
        payload["attestation"] = body
    if version is not None:
        payload["appVersion"] = version
    data = json.dumps(payload, separators=(",", ":")).encode()
    return (b"POST /v1/devices HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
            b"Content-Length: " + str(len(data)).encode() + b"\r\n\r\n" + data)


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


def protobuf_request(value: str = "", integrity: str | None = None) -> bytes:
    body = field(1, b"android") + field(2, VERSION.encode())
    if integrity is not None:
        attestation = (field(1, b"play-integrity") + field(2, value.encode())
                       + field(3, integrity.encode()))
        body += field(3, attestation)
    return (b"POST /v1/devices HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/protobuf\r\n"
            b"Accept: application/protobuf\r\nContent-Length: " + str(len(body)).encode()
            + b"\r\n\r\n" + body)


def expect_error(port: int, request: bytes, status: int, error: str,
                 retry_after: str | None = None) -> None:
    got, headers, body = exchange(port, request)
    assert (got, json.loads(body)) == (status, {"error": error}), (got, body, error)
    assert headers.get("retry-after") == retry_after, (error, headers)


def expect_enrolled(port: int, request: bytes) -> dict:
    status, headers, body = exchange(port, request)
    assert status == 201, (status, body)
    assert headers.get("vary", "").lower() == "accept", headers
    if headers["content-type"] == "application/protobuf":
        # EnrollDeviceResponse: device_id (26), device_token (54), expires_at (24).
        assert body[0:2] == b"\x0a\x1a" and body[28:30] == b"\x12\x36", body
        assert body[84:86] == b"\x1a\x18" and len(body) == 110, body
        result = {"deviceId": body[2:28].decode(), "deviceToken": body[30:84].decode(),
                  "expiresAt": body[86:110].decode()}
    else:
        result = json.loads(body)
        assert set(result) == {"deviceId", "deviceToken", "expiresAt"}, result
    assert re.fullmatch(r"fintel_dev_[A-Za-z0-9_-]{43}", result["deviceToken"]), result
    assert re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z", result["expiresAt"]), result
    return result


def rotate(port: int, bearer: str) -> str:
    status, _, body = exchange(port, b"POST /v1/devices/rotate HTTP/1.1\r\nHost: localhost\r\n"
                               + ("Authorization: Bearer " + bearer + "\r\n\r\n").encode())
    assert status == 200, (status, body)
    return json.loads(body)["deviceToken"]


def rotate_status(port: int, bearer: str) -> int:
    return exchange(port, b"POST /v1/devices/rotate HTTP/1.1\r\nHost: localhost\r\n"
                    + ("Authorization: Bearer " + bearer + "\r\n\r\n").encode())[0]


def wait_for(condition, seconds: float, what: str) -> None:
    deadline = time.monotonic() + seconds
    while not condition():
        assert time.monotonic() < deadline, f"timed out waiting for {what}"
        time.sleep(0.01)


def timed(port: int, request: bytes) -> tuple[float, tuple[int, dict[str, str], bytes]]:
    started = time.monotonic()
    result = exchange(port, request)
    return time.monotonic() - started, result


class Background:
    """One request on its own socket and thread; `result` once answered."""

    def __init__(self, port: int, request: bytes) -> None:
        self.result = None
        self.error: BaseException | None = None
        self.started = time.monotonic()
        self.elapsed = 0.0
        self.thread = threading.Thread(target=self._run, args=(port, request))
        self.thread.start()

    def _run(self, port: int, request: bytes) -> None:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=2) as client:
                client.settimeout(40)
                client.sendall(request)
                self.result = read_response(client)
        except BaseException as error:  # noqa: BLE001 - reported by join
            self.error = error
        self.elapsed = time.monotonic() - self.started

    def done(self) -> bool:
        return not self.thread.is_alive()

    def join(self) -> tuple[int, dict[str, str], bytes]:
        self.thread.join(60)
        assert not self.thread.is_alive() and self.error is None, self.error
        return self.result


# A locked route answers well inside this while an enrollment waits on Google
# for 10 s; generous for CI scheduling, far below the stall it rules out.
PROMPT_SECONDS = 2.0


# ---- Stub ----------------------------------------------------------------------------

def start_stub(port: int, fixture_dir: Path, fixture: dict) -> subprocess.Popen:
    stub = subprocess.Popen([sys.executable, str(Path(__file__).with_name(
        "play_integrity_google_stub.py")), str(port), str(fixture["cert"]), str(fixture["key"]),
        str(fixture_dir)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    for _ in range(200):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return stub
        except OSError:
            time.sleep(0.02)
    raise RuntimeError("stub did not listen")


class StubLog:
    def __init__(self, fixture_dir: Path) -> None:
        self.path = fixture_dir / "stub.log"

    def take(self) -> list[dict]:
        entries = ([json.loads(line) for line in self.path.read_text().splitlines()]
                   if self.path.exists() else [])
        self.path.unlink(missing_ok=True)
        failures = [entry for entry in entries if not entry.get("ok")]
        assert not failures, failures
        return entries

    def peek(self) -> list[dict]:
        """The entries so far, left in place (safe while the stub writes)."""
        if not self.path.exists():
            return []
        return [json.loads(line) for line in self.path.read_text().splitlines()
                if line.endswith("}")]

    def decodes(self, mode: str) -> list[dict]:
        return [e for e in self.peek() if e.get("kind") == "decode" and e.get("mode") == mode]

    def counts(self) -> tuple[int, int]:
        entries = self.take()
        return (sum(e.get("kind") == "oauth" for e in entries),
                sum(e.get("kind") == "decode" for e in entries))


# ---- Groups ------------------------------------------------------------------------

def group_contract(fixture: dict, stub_port: int, log: StubLog) -> None:
    log.take()
    with tempfile.TemporaryDirectory() as kv:
        server = Server(TEST_SERVER, kv, play_env(fixture, stub_port))
        try:
            port = server.port
            value = challenge(port)
            # Refusals that must leave the challenge live, none reaching Google.
            expect_error(port, json_request(value, token(value), platform="ios"), 401,
                         "ATTESTATION_INVALID")
            expect_error(port, json_request(value, None), 401, "ATTESTATION_INVALID")
            expect_error(port, json_request(value, token(value), version=None), 401,
                         "ATTESTATION_INVALID")
            assert log.counts() == (0, 0)
            # Refusals after one Google decode each.
            expect_error(port, json_request(value, token(value, "decode_wrong_cert")), 401,
                         "ATTESTATION_INVALID")
            expect_error(port, json_request(value, token(value, "decode_500")), 503,
                         "ATTESTATION_UNAVAILABLE", retry_after="5")
            assert log.counts() == (1, 2)
            # The earlier refusals did not consume it.
            enrolled = expect_enrolled(port, json_request(value, token(value)))
            assert log.counts() == (0, 1)
            expect_error(port, json_request(value, token(value)), 401, "ATTESTATION_EXPIRED")
            assert log.counts() == (0, 0)
            # Authenticated in required mode: rotation works and spends the bearer.
            rotated = rotate(port, enrolled["deviceToken"])
            assert rotate_status(port, enrolled["deviceToken"]) == 401
            rotate(port, rotated)
            second = challenge(port)
            expect_enrolled(port, protobuf_request(second, token(second)))
            assert log.counts() == (0, 1)
        finally:
            server.stop()
        assert server.stderr.count("play-integrity enrollment unavailable: UpstreamUnavailable") == 1
        assert "Play Integrity Android enrollment: configured (1 release certificate digest(s), " \
               "minimum versionCode 40)" in server.stderr, server.stderr
        assert "c3." not in server.stderr and "BEGIN PRIVATE KEY" not in server.stderr
    with tempfile.TemporaryDirectory() as kv:
        server = Server(TEST_SERVER, kv, play_env(fixture, stub_port))
        try:
            port = server.port
            value = challenge(port)
            refusals = [("decode_low_version", 401, "ATTESTATION_INVALID", None),
                        ("decode_basic_only", 401, "ATTESTATION_INVALID", None),
                        ("decode_other_challenge", 401, "ATTESTATION_INVALID", None),
                        ("decode_stale", 401, "ATTESTATION_INVALID", None),
                        ("decode_replayed", 401, "ATTESTATION_INVALID", None),
                        ("decode_400", 401, "ATTESTATION_INVALID", None),
                        ("decode_429", 503, "ATTESTATION_BUSY", "1"),
                        ("decode_403", 503, "ATTESTATION_UNAVAILABLE", "5")]
            for mode, status, error, retry in refusals:
                expect_error(port, json_request(value, token(value, mode)), status, error, retry)
            assert log.counts() == (1, len(refusals))
            expect_enrolled(port, json_request(value, token(value)))
            assert log.counts() == (0, 1)
        finally:
            server.stop()
        assert "PermissionDenied" in server.stderr
    with tempfile.TemporaryDirectory() as kv:
        server = Server(TEST_SERVER, kv, play_env(fixture, stub_port))
        try:
            port = server.port
            value = challenge(port)
            sent = token(value, "decode_html")
            expect_error(port, json_request(value, sent), 503, "ATTESTATION_UNAVAILABLE", "5")
            # The same token again: burned locally, no second decode.
            expect_error(port, json_request(value, sent), 401, "ATTESTATION_INVALID")
            # A token bound to another appVersion than the request's.
            expect_error(port, json_request(value, token(value, version="1.4.3")), 401,
                         "ATTESTATION_INVALID")
            assert log.counts() == (1, 2)
            expect_enrolled(port, json_request(value, token(value)))
            assert log.counts() == (0, 1)
        finally:
            server.stop()
    with tempfile.TemporaryDirectory() as kv:
        server = Server(TEST_SERVER, kv, play_env(fixture, stub_port))
        try:
            port = server.port
            value = challenge(port)
            # Never minted, too short for the schema, and other providers.
            expect_error(port, json_request("A" * 43, token("A" * 43)), 401, "ATTESTATION_EXPIRED")
            status, _, body = exchange(port, json_request("short", token(value)))
            # A schema refusal has no zod issue list.
            assert status == 400 and json.loads(body) == {"error": "BAD_REQUEST"}, body
            expect_error(port, json_request(value, None, provider="development"), 401,
                         "ATTESTATION_REQUIRED")
            expect_error(port, json_request(value, None, attestation=False), 401,
                         "ATTESTATION_REQUIRED")
            expect_error(port, json_request(value, token(value), provider="apple-app-attest"),
                         401, "ATTESTATION_INVALID")
            expect_error(port, json_request(value, token(value), platform="macos"), 401,
                         "ATTESTATION_INVALID")
            assert log.counts() == (0, 0)
            expect_enrolled(port, json_request(value, token(value)))
        finally:
            server.stop()
    print("contract: pre-I/O refusals, verdict/upstream refusals keep the challenge "
          "(401 INVALID, 503 BUSY/UNAVAILABLE with Retry-After), 201 JSON and protobuf, replayed "
          "challenge and token, required-mode auth and rotation, non-Android and other "
          "providers: ok")


def group_slow_decode(fixture: dict, stub_port: int, log: StubLog) -> None:
    """One Play decode delayed 10 s in the stub while other routes run.

    Under the old global route lock every request below waited for the
    decode; now each answers within PROMPT_SECONDS while the enrollment is
    still in flight.
    """
    log.take()
    with tempfile.TemporaryDirectory() as kv:
        server = Server(TEST_SERVER, kv, play_env(fixture, stub_port))
        try:
            port = server.port
            first = challenge(port)
            bearer = expect_enrolled(port, json_request(first, token(first)))["deviceToken"]
            held = challenge(port)
            slow = Background(port, json_request(held, token(held, "decode_delay_10s")))
            wait_for(lambda: log.decodes("decode_delay_10s"), 5, "the delayed decode")
            measured = {}
            elapsed, (status, _, body) = timed(
                port, b"POST /v1/devices/challenge HTTP/1.1\r\nHost: localhost\r\n\r\n")
            assert status == 200 and elapsed < PROMPT_SECONDS, (status, body, elapsed)
            measured["challenge"] = elapsed
            second = json.loads(body)["challenge"]
            elapsed, (status, _, body) = timed(
                port, b"GET /v1/networks HTTP/1.1\r\nHost: localhost\r\n"
                + ("Authorization: Bearer " + bearer + "\r\n\r\n").encode())
            assert status == 200 and elapsed < PROMPT_SECONDS, (status, body[:200], elapsed)
            measured["networks"] = elapsed
            elapsed, (status, _, body) = timed(
                port, b"POST /v1/devices/rotate HTTP/1.1\r\nHost: localhost\r\n"
                + ("Authorization: Bearer " + bearer + "\r\n\r\n").encode())
            assert status == 200 and elapsed < PROMPT_SECONDS, (status, body, elapsed)
            measured["rotate"] = elapsed
            bearer = json.loads(body)["deviceToken"]
            # Another enrollment is not serialized behind the held one either:
            # its decode overlaps it at the stub.
            elapsed, (status, headers, body) = timed(
                port, protobuf_request(second, token(second)))
            assert status == 201 and headers["content-type"] == "application/protobuf", (status, body)
            assert elapsed < PROMPT_SECONDS, elapsed
            measured["play enrollment"] = elapsed
            overlap = [e for e in log.decodes("ok") if e["in_flight"] == 2]
            assert len(overlap) == 1, log.peek()
            # App Attest verification runs meanwhile too (this server trusts
            # only Apple's root, so the proof is refused after verification).
            elapsed, result = timed(port, json_request(held, "A" * 64, platform="ios",
                                                       provider="apple-app-attest"))
            assert (result[0], json.loads(result[2])) == (401, {"error": "ATTESTATION_INVALID"})
            assert elapsed < PROMPT_SECONDS, elapsed
            measured["apple refusal"] = elapsed
            # The held enrollment is still waiting on Google.
            assert not slow.done() and time.monotonic() - slow.started < 9.0, slow.elapsed
            in_flight_at = time.monotonic() - slow.started
            status, headers, body = slow.join()
            assert (status, json.loads(body)) == (503, {"error": "ATTESTATION_UNAVAILABLE"}), body
            assert headers.get("retry-after") == "5" and slow.elapsed >= 9.5, slow.elapsed
            # Refused upstream: the challenge is still live, and the bearer
            # rotated meanwhile still authenticates.
            expect_enrolled(port, json_request(held, token(held)))
            rotate(port, bearer)
            assert [e["mode"] for e in log.take() if e.get("kind") == "decode"] == [
                "ok", "decode_delay_10s", "ok", "ok"]
        finally:
            server.stop()
        assert (server.stderr.count("play-integrity enrollment unavailable: Timeout")
                + server.stderr.count("play-integrity enrollment unavailable: UpstreamUnavailable")
                == 1), server.stderr
    print("slow decode: with one decode held 10 s in the stub, "
          + ", ".join(f"{name} {seconds * 1000:.1f} ms" for name, seconds in measured.items())
          + f"; all answered while the enrollment was in flight ({in_flight_at:.2f} s in); "
          f"it answered 503 ATTESTATION_UNAVAILABLE after {slow.elapsed:.2f} s, the challenge "
          "then enrolled: ok")


def fire_together(port: int, requests: list[bytes]):
    """Sends every request but its last byte, then the last bytes together.

    The start gate (a barrier across one thread per request) makes the
    server read the requests at once. Returns (threads, results, errors);
    results[i] is set once request i is answered.
    """
    clients = []
    for request in requests:
        client = socket.create_connection(("127.0.0.1", port), timeout=2)
        client.settimeout(60)
        client.sendall(request[:-1])
        clients.append(client)
    barrier = threading.Barrier(len(requests))
    results: list = [None] * len(requests)
    errors: list[BaseException] = []

    def fire(index: int) -> None:
        try:
            barrier.wait(10)
            clients[index].sendall(requests[index][-1:])
            results[index] = read_response(clients[index])
        except BaseException as error:  # noqa: BLE001 - reported by the caller
            errors.append(error)
        finally:
            clients[index].close()

    threads = [threading.Thread(target=fire, args=(i,)) for i in range(len(requests))]
    for thread in threads:
        thread.start()
    return threads, results, errors


def is_busy(result) -> bool:
    status, headers, body = result
    return (status == 503 and json.loads(body) == {"error": "ATTESTATION_BUSY"}
            and headers.get("retry-after") == "1")


def max_in_flight(log: StubLog) -> int:
    return max((e["in_flight"] for e in log.peek() if e.get("kind") == "decode"), default=0)


def prompt_routes(port: int, bearer: str) -> tuple[dict, str]:
    """/healthz, challenge, networks and rotate, each within PROMPT_SECONDS.

    Returns the timings and the rotated bearer.
    """
    measured = {}
    authorization = ("Authorization: Bearer " + bearer + "\r\n\r\n").encode()
    for name, request in (
            ("healthz", b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n"),
            ("challenge", b"POST /v1/devices/challenge HTTP/1.1\r\nHost: localhost\r\n\r\n"),
            ("networks", b"GET /v1/networks HTTP/1.1\r\nHost: localhost\r\n" + authorization),
            ("rotate", b"POST /v1/devices/rotate HTTP/1.1\r\nHost: localhost\r\n" + authorization)):
        elapsed, (status, _, body) = timed(port, request)
        assert status == 200 and elapsed < PROMPT_SECONDS, (name, status, body[:200], elapsed)
        measured[name] = elapsed
        if name == "rotate":
            bearer = json.loads(body)["deviceToken"]
    return measured, bearer


def race_round(fixture: dict, stub_port: int, log: StubLog, shared_token: bool) -> dict:
    """Simultaneous enrollments of one challenge, decodes held at the stub.

    Distinct tokens (six contenders): the route admits three, whose decodes
    the stub holds together (in_flight 3, never 4); the other three are
    answered 503 ATTESTATION_BUSY at once by the fourth worker, which then
    also answers /healthz, challenge, networks and rotate. Released together,
    the three verified decodes race into the conditional KV frame, which
    admits exactly one; the other two are 401 ATTESTATION_EXPIRED. Shared
    token (nine contenders): one decode; the others are refused as replays
    (or BUSY when they found the three slots taken) while it is held.

    Per-IP quota (10 per minute): distinct = bearer device 2 + challenge 1 +
    6 contenders + 1 challenge probe; shared = challenge 1 + 9 contenders.
    """
    log.take()
    (fixture["dir"] / "release").unlink(missing_ok=True)
    with tempfile.TemporaryDirectory() as kv:
        server = Server(TEST_SERVER, kv, play_env(fixture, stub_port))
        try:
            port = server.port
            bearer = None
            if not shared_token:
                first = challenge(port)
                bearer = expect_enrolled(port, json_request(first, token(first)))["deviceToken"]
                log.take()
            value = challenge(port)
            contenders = 9 if shared_token else 6
            shared = token(value, "decode_hold")
            requests = [(protobuf_request if index % 2 else json_request)(
                            value, shared if shared_token else token(value, "decode_hold"))
                        for index in range(contenders)]
            threads, results, errors = fire_together(port, requests)
            held = 1 if shared_token else 3
            measured = {}
            wait_for(lambda: len(log.decodes("decode_hold")) == held, 8, f"{held} held decodes")
            # Every request that is not held is answered while the held ones
            # wait: replays/BUSY (shared) or BUSY (distinct).
            wait_for(lambda: sum(r is not None for r in results) == contenders - held, 8,
                     "the prompt refusals")
            answered = [r for r in results if r is not None]
            if shared_token:
                for status, _, body in answered:
                    assert ((status, json.loads(body)) == (401, {"error": "ATTESTATION_INVALID"})
                            or is_busy((status, _, body))), body
                assert any(r[0] == 401 for r in answered), answered
            else:
                assert all(is_busy(r) for r in answered), answered
                # No fourth decode began: every other contender has its answer.
                assert len(log.decodes("decode_hold")) == 3 and max_in_flight(log) == 3, log.peek()
                measured, bearer = prompt_routes(port, bearer)
                assert len(log.decodes("decode_hold")) == 3, log.peek()
            assert sum(r is None for r in results) == held, results
            (fixture["dir"] / "release").write_text("")
            for thread in threads:
                thread.join(60)
            assert not errors, errors
            winners = [(index, r) for index, r in enumerate(results) if r[0] == 201]
            assert len(winners) == 1, [r[0] for r in results]
            winner, (_, winner_headers, winner_body) = winners[0]
            protobuf = winner % 2 == 1
            assert winner_headers["content-type"] == (
                "application/protobuf" if protobuf else "application/json; charset=utf-8")
            outcome = {"expired": 0, "busy": 0, "invalid": 0, "measured": measured}
            for status, headers, body in results:
                if status == 201:
                    continue
                error = json.loads(body)
                if status == 503:
                    assert error == {"error": "ATTESTATION_BUSY"} and headers["retry-after"] == "1"
                    outcome["busy"] += 1
                elif shared_token:
                    assert (status, error) == (401, {"error": "ATTESTATION_INVALID"}), body
                    outcome["invalid"] += 1
                else:
                    assert (status, error) == (401, {"error": "ATTESTATION_EXPIRED"}), body
                    outcome["expired"] += 1
            if not shared_token:
                assert (outcome["expired"], outcome["busy"]) == (2, 3), outcome
            # Distinct tokens: three verified decodes, one device. A shared
            # token: decoded once.
            outcome["decodes"] = log.counts()[1]
            assert outcome["decodes"] == held, outcome
            rotate(port, winner_body[30:84].decode() if protobuf
                   else json.loads(winner_body)["deviceToken"])
            if bearer:
                rotate(port, bearer)
        finally:
            (fixture["dir"] / "release").unlink(missing_ok=True)
            server.stop()
    return outcome


def group_race(fixture: dict, stub_port: int, log: StubLog) -> None:
    rounds = [race_round(fixture, stub_port, log, shared_token=False) for _ in range(3)]
    shared = race_round(fixture, stub_port, log, shared_token=True)
    slowest = max(seconds for r in rounds for seconds in r["measured"].values())
    print("race: 3 rounds x 6 simultaneous enrollments of one challenge with distinct tokens: "
          "3 decodes held together (route cap; never a 4th), 3 BUSY answered meanwhile, and "
          f"/healthz, challenge, networks and rotate each answered (slowest {slowest * 1000:.1f} "
          "ms) while the three were held; then one 201 each and "
          + ", ".join(f"{r['expired']} EXPIRED/{r['busy']} BUSY" for r in rounds)
          + f"; shared token: {shared['decodes']} decode, {shared['invalid']} replays refused "
          f"({shared['busy']} BUSY) while it was held, one 201: ok")


def group_capacity(fixture: dict, stub_port: int, log: StubLog) -> None:
    """Three held enrollments of three challenges; a fourth is BUSY, then retries.

    The fourth valid request (its own challenge and a passing token) is
    answered 503 ATTESTATION_BUSY within PROMPT_SECONDS, with no OAuth or
    decode request at the stub (the race group covers JSON busy answers).
    A request the route refuses before admission (platform ios) still gets
    its own 401 ATTESTATION_INVALID while the slots are full. After the
    three held ones enroll, the same bytes (same challenge, same integrity
    token) enroll: the busy answer consumed neither.
    Per-IP quota: 4 challenges + 3 held + 1 busy + 1 ios + 1 retry = 10.
    """
    log.take()
    (fixture["dir"] / "release").unlink(missing_ok=True)
    with tempfile.TemporaryDirectory() as kv:
        server = Server(TEST_SERVER, kv, play_env(fixture, stub_port))
        try:
            port = server.port
            values = [challenge(port) for _ in range(4)]
            builders = (json_request, protobuf_request, json_request)
            held_requests = [build(value, token(value, "decode_hold"))
                             for build, value in zip(builders, values[:3])]
            threads, results, errors = fire_together(port, held_requests)
            wait_for(lambda: len(log.decodes("decode_hold")) == 3, 8, "3 held decodes")
            assert max_in_flight(log) == 3, log.peek()
            stub_entries = len(log.peek())
            fourth = token(values[3], "ok")
            busy_protobuf = protobuf_request(values[3], fourth)
            measured = {}
            elapsed, result = timed(port, busy_protobuf)
            assert is_busy(result) and elapsed < PROMPT_SECONDS, (result, elapsed)
            assert result[1]["content-type"].startswith("application/json"), result
            measured["busy"] = elapsed
            # Shape checks come before admission: not BUSY.
            elapsed, (status, _, body) = timed(port, json_request(values[3], fourth, platform="ios"))
            assert (status, json.loads(body)) == (401, {"error": "ATTESTATION_INVALID"}), body
            assert elapsed < PROMPT_SECONDS, elapsed
            elapsed, (status, _, _) = timed(port, b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n")
            assert status == 200 and elapsed < PROMPT_SECONDS, elapsed
            measured["healthz"] = elapsed
            # Refused before any Google I/O, and the held three are still held.
            assert len(log.peek()) == stub_entries, log.peek()[stub_entries:]
            assert all(r is None for r in results), results
            (fixture["dir"] / "release").write_text("")
            for thread in threads:
                thread.join(60)
            assert not errors, errors
            for index, (status, headers, body) in enumerate(results):
                assert status == 201, (index, status, body)
                assert headers["content-type"] == ("application/protobuf" if index == 1
                                                   else "application/json; charset=utf-8")
            # Capacity is back: the same challenge and token enroll now.
            retried = expect_enrolled(port, busy_protobuf)
            rotate(port, retried["deviceToken"])
            assert len(log.decodes("ok")) == 1, log.peek()
            assert log.counts() == (1, 4)
        finally:
            (fixture["dir"] / "release").unlink(missing_ok=True)
            server.stop()
    print("capacity: 3 enrollments held at Google; a 4th valid enrollment answered 503 "
          f"ATTESTATION_BUSY (Retry-After 1) in {measured['busy'] * 1000:.1f} ms with no "
          "Google request, an ios-platform request still 401 ATTESTATION_INVALID, "
          f"/healthz {measured['healthz'] * 1000:.1f} ms; the three enrolled (JSON, protobuf, "
          "JSON) and the busy request then enrolled with the same challenge and token "
          "(1 decode of it, 1 OAuth): ok")


def group_slot_release(fixture: dict, stub_port: int, log: StubLog) -> None:
    """Upstream, verdict, Google-busy and challenge refusals return their slot.

    Four refused enrollments in a row (each would leak a slot if its path
    kept one; three slots exist), then three decodes must still be held at
    once. Storage and clock failures cannot be injected into a socket
    server; the unit "admission" group covers them.
    Per-IP quota: 3 challenges + 4 refusals + 3 held = 10.
    """
    log.take()
    (fixture["dir"] / "release").unlink(missing_ok=True)
    with tempfile.TemporaryDirectory() as kv:
        server = Server(TEST_SERVER, kv, play_env(fixture, stub_port))
        try:
            port = server.port
            first = challenge(port)
            expect_error(port, json_request(first, token(first, "decode_500")), 503,
                         "ATTESTATION_UNAVAILABLE", "5")
            expect_error(port, json_request(first, token(first, "decode_wrong_cert")), 401,
                         "ATTESTATION_INVALID")
            expect_error(port, protobuf_request(first, token(first, "decode_429")), 503,
                         "ATTESTATION_BUSY", "1")
            never = secrets.token_urlsafe(32)
            expect_error(port, json_request(never, token(never)), 401, "ATTESTATION_EXPIRED")
            assert log.counts() == (1, 3)
            values = [first, challenge(port), challenge(port)]
            threads, results, errors = fire_together(
                port, [json_request(value, token(value, "decode_hold")) for value in values])
            wait_for(lambda: len(log.decodes("decode_hold")) == 3, 8, "3 held decodes")
            assert max_in_flight(log) == 3 and all(r is None for r in results), results
            (fixture["dir"] / "release").write_text("")
            for thread in threads:
                thread.join(60)
            assert not errors, errors
            assert [r[0] for r in results] == [201, 201, 201], results
            log.take()
        finally:
            (fixture["dir"] / "release").unlink(missing_ok=True)
            server.stop()
        assert server.stderr.count("play-integrity enrollment unavailable: UpstreamUnavailable") == 1
    print("slot release: after 503 UNAVAILABLE, 401 INVALID (verdict), 503 BUSY (Google 429) "
          "and 401 EXPIRED, three decodes were still held together and all three enrolled: ok")


def group_restart(fixture: dict, stub_port: int, log: StubLog) -> None:
    with tempfile.TemporaryDirectory() as kv:
        server = Server(TEST_SERVER, kv, play_env(fixture, stub_port))
        try:
            port = server.port
            first = challenge(port)
            # A SIGKILL after minting keeps the challenge.
            server.restart(hard=True)
            first_request = json_request(first, token(first))
            bearer = expect_enrolled(port, first_request)["deviceToken"]
            server.restart(hard=True)
            bearer = rotate(port, bearer)
            # A refusal upstream, then SIGKILL: the challenge survives both.
            second = challenge(port)
            expect_error(port, json_request(second, token(second, "decode_500")), 503,
                         "ATTESTATION_UNAVAILABLE", "5")
            server.restart(hard=True)
            # The replay ring is empty after a restart; the spent challenge
            # still refuses the first request before any decode.
            log.take()
            expect_error(port, first_request, 401, "ATTESTATION_EXPIRED")
            assert log.counts() == (0, 0)
            expect_enrolled(port, json_request(second, token(second)))
            server.restart(hard=True)
            rotate(port, bearer)
            # SIGKILL while a decode is held outside the route lock: nothing
            # was committed, so the challenge enrolls after the restart.
            (fixture["dir"] / "release").unlink(missing_ok=True)
            third = challenge(port)
            killed = Background(port, json_request(third, token(third, "decode_hold")))
            wait_for(lambda: log.decodes("decode_hold"), 5, "the held decode")
            server.restart(hard=True)
            killed.thread.join(10)
            assert killed.done() and killed.result is None, killed.result
            expect_enrolled(port, protobuf_request(third, token(third)))
            (fixture["dir"] / "release").write_text("")
            log.take()
        finally:
            (fixture["dir"] / "release").unlink(missing_ok=True)
            server.stop(hard=True)
    print("hard restart: challenge after SIGKILL, bearer and rotation after SIGKILL, upstream "
          "refusal leaves the challenge across SIGKILL, replay refused after SIGKILL, SIGKILL "
          "during a held decode leaves the challenge to enroll: ok")


def group_modes(fixture: dict, stub_port: int, log: StubLog) -> None:
    # Absent Android configuration, release binary, required mode: Android
    # answers ATTESTATION_REQUIRED, iOS App Attest still runs.
    with tempfile.TemporaryDirectory() as kv:
        server = Server(RELEASE_SERVER, kv, {"ATTESTATION_MODE": "required"})
        try:
            port = server.port
            value = challenge(port)
            expect_error(port, json_request(value, token(value)), 401, "ATTESTATION_REQUIRED")
            expect_error(port, protobuf_request(value, token(value)), 401, "ATTESTATION_REQUIRED")
            expect_error(port, json_request(value, "AAAA", platform="ios",
                                            provider="apple-app-attest"), 401,
                         "ATTESTATION_INVALID")
        finally:
            server.stop()
        assert "Play Integrity Android enrollment: not configured; required-mode Android " \
               "enrollment answers ATTESTATION_REQUIRED" in server.stderr, server.stderr
    # Configured release binary (CA from the FORTIS_CA_BUNDLE_PATH fallback).
    # Only pre-I/O refusals are sent, so it never contacts Google; the
    # test-endpoint variables are present and must not matter.
    log.take()
    with tempfile.TemporaryDirectory() as kv:
        env = play_env(fixture, stub_port, FORTIS_PLAY_INTEGRITY_CA_FILE=None,
                       FORTIS_CA_BUNDLE_PATH=str(fixture["ca"]),
                       FORTIS_PLAY_INTEGRITY_CERT_SHA256=fixture["certificate_hex"],
                       FORTIS_PLAY_INTEGRITY_CERT_SHA256_FORMAT="hex-colon")
        server = Server(RELEASE_SERVER, kv, env)
        try:
            port = server.port
            value = challenge(port)
            expect_error(port, json_request("B" * 43, token("B" * 43)), 401, "ATTESTATION_EXPIRED")
            expect_error(port, json_request(value, "not a valid token"), 401, "ATTESTATION_INVALID")
            expect_error(port, json_request(value, token(value), platform="ios"), 401,
                         "ATTESTATION_INVALID")
            expect_error(port, json_request(value, token(value), version=None), 401,
                         "ATTESTATION_INVALID")
        finally:
            server.stop()
        assert "Play Integrity Android enrollment: configured (1 release certificate digest(s)" \
            in server.stderr, server.stderr
        assert log.counts() == (0, 0)
    # Development mode with Android configured: unchanged development path.
    with tempfile.TemporaryDirectory() as kv:
        server = Server(TEST_SERVER, kv, play_env(fixture, stub_port,
                                                  ATTESTATION_MODE="development"))
        try:
            port = server.port
            value = challenge(port)
            expect_error(port, json_request(value, token(value)), 401, "ATTESTATION_INVALID")
            expect_enrolled(port, json_request(value, None, provider="development"))
        finally:
            server.stop()
        assert log.counts() == (0, 0)
    print("modes: absent config keeps ATTESTATION_REQUIRED and the Apple path; configured "
          "release binary refuses pre-I/O cases without contacting Google; development mode "
          "unchanged: ok")


def group_android_attestation_disabled() -> None:
    with tempfile.TemporaryDirectory() as kv:
        env = {"ATTESTATION_MODE": "required", "FORTIS_ANDROID_ATTESTATION_REQUIRED": "false"}
        server = Server(RELEASE_SERVER, kv, env)
        try:
            port = server.port
            enrolled = expect_enrolled(port, protobuf_request())
            expect_enrolled(port, json_request("B" * 43, "unverified-proof"))
            expect_error(port, json_request("", None, platform="ios", attestation=False),
                         401, "ATTESTATION_REQUIRED")
            networks = b"GET /v1/networks HTTP/1.1\r\nHost: localhost\r\n"
            expect_error(port, networks + b"\r\n", 401, "UNAUTHORIZED")
            bearer = rotate(port, enrolled["deviceToken"])
            authorized = networks + f"Authorization: Bearer {bearer}\r\n\r\n".encode()
            status, _, body = exchange(port, authorized)
            assert status == 200, (status, body)

            env["FORTIS_ANDROID_ATTESTATION_REQUIRED"] = "true"
            server.restart(hard=False)
            expect_error(port, authorized, 401, "ATTESTATION_REQUIRED")
            expect_error(port, protobuf_request(), 401, "ATTESTATION_REQUIRED")
        finally:
            server.stop()
    print("Android exception: enrolls and rotates without proof, keeps iOS and bearer checks, "
          "and rejects unverified credentials when re-enabled: ok")


def group_attestation_off_by_default() -> None:
    """Unset per-platform switches mean attestation is not required anywhere,
    even in required mode: every platform enrolls unverified, whatever proof
    it sends (macOS, Windows and Linux release builds send "development"),
    and the unverified credential authorizes requests."""
    with tempfile.TemporaryDirectory() as kv:
        env = {"ATTESTATION_MODE": "required",
               "FORTIS_ANDROID_ATTESTATION_REQUIRED": None,
               "FORTIS_APPLE_ATTESTATION_REQUIRED": None,
               "FORTIS_DESKTOP_ATTESTATION_REQUIRED": None}
        server = Server(RELEASE_SERVER, kv, env)
        try:
            port = server.port
            for platform in ("macos", "windows", "linux"):
                expect_enrolled(port, json_request("C" * 43, None, platform=platform,
                                                   provider="development"))
            for platform in ("ios", "android"):
                expect_enrolled(port, json_request("", None, platform=platform,
                                                   attestation=False))
            mac = expect_enrolled(port, json_request("D" * 43, None, platform="macos",
                                                     provider="development"))
            networks = (b"GET /v1/networks HTTP/1.1\r\nHost: localhost\r\n"
                        + f"Authorization: Bearer {mac['deviceToken']}\r\n\r\n".encode())
            status, _, body = exchange(port, networks)
            assert status == 200, (status, body)
        finally:
            server.stop()
    print("attestation off by default: macOS, Windows, Linux, iOS and Android enroll "
          "unverified in required mode, and the macOS credential authorizes: ok")


def group_startup(fixture: dict, stub_port: int) -> None:
    pasted = json.dumps({"type": "service_account", "private_key": SENTINEL})
    base = play_env(fixture, stub_port)
    cases = [
        ("empty credential", {"FORTIS_PLAY_INTEGRITY_CREDENTIAL_FILE": ""},
         "FORTIS_PLAY_INTEGRITY_CREDENTIAL_FILE is empty"),
        ("pasted key", {"FORTIS_PLAY_INTEGRITY_CREDENTIAL_FILE": pasted},
         "FORTIS_PLAY_INTEGRITY_CREDENTIAL_FILE must be an absolute path"),
        ("missing credential", {"FORTIS_PLAY_INTEGRITY_CREDENTIAL_FILE":
                                str(fixture["credential"]) + ".missing"}, "OpenFailed"),
        ("0644 credential", {"FORTIS_PLAY_INTEGRITY_CREDENTIAL_FILE": str(fixture["loose"])},
         "PermissionsTooOpen"),
        ("malformed digest", {"FORTIS_PLAY_INTEGRITY_CERT_SHA256": "AB:CD"},
         "FORTIS_PLAY_INTEGRITY_CERT_SHA256 must be"),
        ("digest in another form", {"FORTIS_PLAY_INTEGRITY_CERT_SHA256_FORMAT": "hex"},
         "FORTIS_PLAY_INTEGRITY_CERT_SHA256 must be"),
        ("missing format", {"FORTIS_PLAY_INTEGRITY_CERT_SHA256_FORMAT": None},
         "requires FORTIS_PLAY_INTEGRITY_CERT_SHA256_FORMAT"),
        ("version 0", {"FORTIS_PLAY_INTEGRITY_MIN_VERSION_CODE": "0"},
         "FORTIS_PLAY_INTEGRITY_MIN_VERSION_CODE must be"),
        ("no CA", {"FORTIS_PLAY_INTEGRITY_CA_FILE": None},
         "requires FORTIS_PLAY_INTEGRITY_CA_FILE"),
        ("garbage CA", {"FORTIS_PLAY_INTEGRITY_CA_FILE": str(fixture["garbage_ca"])},
         "Invalid CA bundle configured for Play Integrity"),
        ("stale window", {"FORTIS_PLAY_INTEGRITY_MAX_AGE_SECONDS": "7200"},
         "FORTIS_PLAY_INTEGRITY_MAX_AGE_SECONDS must be"),
    ]
    with tempfile.TemporaryDirectory() as kv:
        for label, edits, message in cases:
            env = dict(base)
            env.update(edits)
            env = {name: value for name, value in env.items() if value is not None}
            refused = subprocess.run([RELEASE_SERVER, str(free_port())], capture_output=True,
                                     env=server_env(kv, **env), timeout=10)
            output = (refused.stdout + refused.stderr).decode(errors="replace")
            assert refused.returncode == 2 and message in output, (label, refused.returncode, output)
            assert SENTINEL not in output and str(fixture["credential"]) not in output, output
            assert "listening" not in output, (label, output)
        # Only one variable of the set: never silently "off".
        refused = subprocess.run([RELEASE_SERVER, str(free_port())], capture_output=True,
                                 env=server_env(kv, ATTESTATION_MODE="required",
                                                FORTIS_PLAY_INTEGRITY_CERT_SHA256="AA"),
                                 timeout=10)
        assert refused.returncode == 2 and b"requires FORTIS_PLAY_INTEGRITY_CREDENTIAL_FILE" in \
            refused.stderr, refused
        # The test server refuses half a test endpoint.
        env = dict(base)
        env.pop("FORTIS_TEST_PLAY_INTEGRITY_PORT")
        refused = subprocess.run([TEST_SERVER, str(free_port())], capture_output=True,
                                 env=server_env(kv, **env), timeout=10)
        assert refused.returncode == 2 and b"FORTIS_TEST_PLAY_INTEGRITY_PORT" in refused.stderr
    print(f"startup: {len(cases) + 2} malformed or partial configurations stop the server with a "
          "message naming the variable, never the value or path: ok")


def symbols(path: str) -> str:
    return subprocess.run(["nm", "-C", path], check=True, capture_output=True, text=True).stdout


def group_release_isolation() -> None:
    release = Path(RELEASE_SERVER).read_bytes()
    release_symbols = symbols(RELEASE_SERVER)
    test_bytes = Path(TEST_SERVER).read_bytes()
    # The production code is in the release server...
    for name in ("fortis::auth::EnrollWithPlayIntegrity", "fortis::auth::PlayIntegrityGoogleClient::Decode",
                 "fortis::auth::VerifyPlayIntegrityVerdict", "fortis::auth::LoadPlayServiceAccount",
                 "fortis::auth::StartPlayIntegrityRuntime"):
        assert name in release_symbols, name
    for text in (b"playintegrity.googleapis.com", b"oauth2.googleapis.com",
                 b"/v1/io.fortisnet.wallet:decodeIntegrityToken"):
        assert text in release, text
    # ...without its test seam: no endpoint variables and no loopback policy
    # (provider_http_bridge, which has its own allow_test_loopback, is linked
    # only into the replay binaries under tests/server; the seam server has
    # play_integrity_google's).
    assert b"FORTIS_TEST_PLAY_INTEGRITY_HOST" not in release
    assert b"FORTIS_TEST_PLAY_INTEGRITY_HOST" in test_bytes
    assert "PlayIntegrityGoogleTestSeam" not in release_symbols
    loopbacks = release_symbols.count("allow_test_loopback(void*, unsigned int)")
    bridge = symbols(str(ROOT / "build/provider_http_bridge.o")).count("allow_test_loopback(")
    assert loopbacks == 0 and bridge == 1, (loopbacks, bridge)
    assert symbols(TEST_SERVER).count("allow_test_loopback(void*, unsigned int)") == 1
    for release_object in ("build/play_integrity_google.o", "build/play_integrity_enrollment.o"):
        text = symbols(str(ROOT / release_object))
        assert "allow_test_loopback" not in text and "TestSeam" not in text, release_object
        assert b"FORTIS_TEST_PLAY_INTEGRITY" not in (ROOT / release_object).read_bytes()
    # Build lists: release objects only in OBJECTS and the guest.
    makefile = (ROOT / "Makefile").read_text()
    objects = "\n".join(line for line in makefile.splitlines()
                        if re.match(r"(OBJECTS|PLAY_INTEGRITY_OBJECTS)\s*\+?=", line))
    assert "build/play_integrity_enrollment.o" in objects and "_test_seam" not in objects, objects
    # sources.mk is the source list of both builds (OBJECTS and the guest).
    uk = (ROOT / "sources.mk").read_text()
    # The shared source list is partitioned into component libraries. Verify
    # the auth partition and its guest wiring without requiring one monolith.
    assert "include $(APPINTELLIGENCECPP_BASE)/sources.mk" in uk
    assert "FORTIS_COMPONENT_SOURCES_auth := $(filter src/auth/%,$(FORTIS_SOURCES))" in uk
    assert "FORTIS$$(call uc,$(1))_SRCS-y += $$(addprefix $$(APPINTELLIGENCECPP_BASE)/,$$(FORTIS_COMPONENT_SOURCES_$(1)))" in uk
    assert "$(foreach component,$(filter-out app,$(FORTIS_COMPONENTS)),$(eval $(call fortis_component_guest,$(component))))" in uk
    partitions = subprocess.check_output(
        ["make", "--no-print-directory", "-s", "print-component-sources"],
        cwd=ROOT, text=True)
    auth_sources = dict(line.split(":", 1) for line in partitions.splitlines())["auth"].split()
    for source in ("play_integrity.cpp", "play_integrity_google.cpp", "play_integrity_enrollment.cpp"):
        assert auth_sources.count(f"src/auth/{source}") == 1, source
    for source in ("play_integrity.cpp", "play_integrity_google.cpp",
                   "play_integrity_enrollment.cpp"):
        assert f"FORTIS_SOURCES += src/auth/{source}\n" in uk, source
    assert "FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT" not in uk and "_test_seam" not in uk
    # One issuer definition each, one Play issuer call, guards in place.
    sources = {path.name: path.read_text() for path in (ROOT / "src").rglob("*.cpp")}
    play = [name for name, text in sources.items()
            if re.search(r"^class PlayIntegrityAttestationIssuer\s*\{", text, re.M)]
    apple = [name for name, text in sources.items()
             if re.search(r"^class DeviceAttestationIssuer\s*\{", text, re.M)]
    assert play == ["play_integrity_enrollment.cpp"] and apple == ["apple_app_attest.cpp"], (play, apple)
    calls = sum(text.count("PlayIntegrityAttestationIssuer::Issue(") for text in sources.values())
    assert calls == 1, calls
    enrollment = sources["play_integrity_enrollment.cpp"]
    assert ("#if defined(FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT) && defined(FORTIS_UNIKRAFT)\n#error"
            in enrollment)
    header = (ROOT / "src/auth/play_integrity_enrollment.h").read_text()
    assert '#include "play_integrity_google.h"' not in header
    assert "friend class PlayIntegrityAttestationIssuer;" in (ROOT / "src/auth/device_auth.h").read_text()
    print("release isolation: Play client, validator and issuer in build/fortis_node and "
          "Makefile.uk; no test endpoint, loopback policy or seam symbol; one issuer of each "
          "kind, one Play issuer call: ok")


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="play-c3-") as temporary:
        fixture_dir = Path(temporary)
        os.chmod(fixture_dir, 0o700)
        fixture = make_fixture(fixture_dir)
        started = time.monotonic()
        subprocess.run([UNIT, "unit", str(fixture_dir)], check=True, timeout=600)
        print(f"unit groups: {time.monotonic() - started:.1f} s")
        stub_port = free_port()
        stub = start_stub(stub_port, fixture_dir, fixture)
        log = StubLog(fixture_dir)
        try:
            group_contract(fixture, stub_port, log)
            group_slow_decode(fixture, stub_port, log)
            group_race(fixture, stub_port, log)
            group_capacity(fixture, stub_port, log)
            group_slot_release(fixture, stub_port, log)
            group_restart(fixture, stub_port, log)
            group_modes(fixture, stub_port, log)
            group_android_attestation_disabled()
            group_attestation_off_by_default()
            group_startup(fixture, stub_port)
        finally:
            stub.terminate()
            stub.wait(timeout=5)
        group_release_isolation()
    print("play_integrity_enrollment_socket_test.py: passed")


if __name__ == "__main__":
    main()
