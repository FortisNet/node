#!/usr/bin/env python3
"""Canary opt-in checks for native GET /v1/prices in the normal binary.

Usage: price_spot_canary_test.py <normal binary> <price route test binary>

No request sent to the normal binary can reach its provider owner: each one is
answered by Origin, bearer, quota or query validation first. Its CA bundle is a
freshly generated local CA, so even an accidental provider call could not
verify a public certificate, and provider keys are scrubbed from the
environment. The single data-path check runs in the test binary, through the
same composition, against a loopback generated-CA stub.
"""

import json
import os
from pathlib import Path
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import tls_ca
from network_route_test import assert_live_catalog

ROOT = Path(__file__).resolve().parents[1]
NETWORK_FIXTURE = ROOT / "tests/corpus/networks_route.json"
CANARY = "FORTIS_NATIVE_PRICE_ROUTE_CANARY"
FIXTURE_TOKEN = "fintel_dev_" + "P" * 43
TEST_ONLY_ENV = ("FORTIS_ENABLE_TEST_PRICE_ROUTE", "FORTIS_TEST_PRICE_UPSTREAM_PORT",
                 "FORTIS_TEST_PRICE_UPSTREAM_CA", "FORTIS_TEST_PRICE_NOW_MS")
SCRUBBED_ENV = TEST_ONLY_ENV + (
    CANARY, "FORTIS_CA_BUNDLE_PATH", "PORT", "NODE_ENV", "ATTESTATION_MODE",
    "DEVICE_TOKEN_TTL_SECONDS", "METRICS_TOKEN", "FORTIS_TLS_PROBE_HOST",
    "FORTIS_TLS_PROBE_PORT", "FORTIS_TLS_PROBE_CA", "FORTIS_ENABLE_PROVIDER_TEST_ROUTE",
    "FORTIS_TEST_UPSTREAM_PORT", "FORTIS_TEST_TLS_FAILURE_PORT", "FORTIS_TEST_UPSTREAM_CA",
    "FORTIS_ENABLE_ASYNC_TEST_PROBE", "COINGECKO_API_KEY", "COINGECKO_TIER",
    "JUPITER_API_KEY", "TRONSCAN_API_KEY")
# Present only in binaries built with the price test plane (tests/server):
# its fixture seeding, switches, upstream inputs, owner clock and probe route.
TEST_SUBSTITUTION_MARKERS = (
    b"PriceTestUpstream", b"SeedPriceRouteFixture", b"FORTIS_TEST_PRICE_NOW_FILE",
    b"FORTIS_ENABLE_TEST_PRICE_ROUTE", b"FORTIS_TEST_PRICE_UPSTREAM_PORT",
    b"FORTIS_TEST_PRICE_UPSTREAM_CA", b"FORTIS_TEST_PRICE_NOW_MS",
    b"/probe/test-price-owner")
DEVELOPMENT_QUOTA = 240


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def exchange(port, method, path, headers=None, body=None, timeout=5.0):
    lines = [f"{method} {path} HTTP/1.1", "Host: localhost", "Connection: close"]
    lines += [f"{name}: {value}" for name, value in (headers or {}).items()]
    if body is not None:
        lines.append(f"Content-Length: {len(body)}")
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as client:
        client.settimeout(timeout)
        client.sendall(("\r\n".join(lines) + "\r\n\r\n").encode() + (body or b""))
        received = bytearray()
        while chunk := client.recv(65536):
            received.extend(chunk)
    head, _, payload = bytes(received).partition(b"\r\n\r\n")
    first, *header_lines = head.decode("latin1").split("\r\n")
    response_headers = {}
    for line in header_lines:
        name, _, value = line.partition(":")
        response_headers[name.strip().lower()] = value.strip()
    return int(first.split()[1]), response_headers, payload


def environment(kv_dir: Path, **overrides) -> dict:
    env = os.environ.copy()
    for name in SCRUBBED_ENV:
        env.pop(name, None)
    env["FORTIS_KV_DIR"] = str(kv_dir)
    env.update(overrides)
    return env


def start(binary: str, env: dict):
    port = free_port()
    process = subprocess.Popen([binary, str(port)], env=env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    for _ in range(200):
        if process.poll() is not None:
            raise AssertionError("server exited before serving: " +
                                 process.stderr.read().decode(errors="replace"))
        try:
            # /healthz is answered by a worker, so the pool and owners are up.
            if exchange(port, "GET", "/healthz", timeout=0.5)[0] == 200:
                return process, port
        except OSError:
            pass
        time.sleep(0.03)
    process.kill()
    process.wait(timeout=5)
    raise AssertionError("server did not become healthy")


def terminate(process, timeout=10.0) -> int:
    """SIGTERM must drain owners, coordinator and handoff and exit 0."""
    process.send_signal(signal.SIGTERM)
    try:
        return process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)
        raise AssertionError("server did not exit after SIGTERM")


def thread_count(pid: int):
    status = Path(f"/proc/{pid}/status")
    if status.exists():
        for line in status.read_text().splitlines():
            if line.startswith("Threads:"):
                return int(line.split()[1])
        return None
    try:  # macOS: one row per thread after the header.
        result = subprocess.run(["ps", "-M", "-p", str(pid)], capture_output=True,
                                text=True, timeout=5, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return None
    rows = [line for line in result.stdout.splitlines()[1:] if line.strip()]
    return len(rows) if result.returncode == 0 and rows else None


def enroll(port: int) -> str:
    status, _, body = exchange(port, "POST", "/v1/devices/challenge", body=b"")
    assert status == 200, (status, body)
    payload = json.dumps({
        "platform": "android", "appVersion": "4.5.6",
        "attestation": {"provider": "development",
                        "challenge": json.loads(body)["challenge"]},
    }, separators=(",", ":")).encode()
    status, _, body = exchange(port, "POST", "/v1/devices",
                               {"Content-Type": "application/json"}, payload)
    assert status == 201, (status, body)
    return json.loads(body)["deviceToken"]


def wait_for_quota_window() -> None:
    """Device quotas are fixed epoch-minute windows; avoid a mid-test reset."""
    into_minute = time.time() % 60
    if into_minute > 40:
        time.sleep(60.2 - into_minute)


def assert_unauthorized(status, headers, body, head=False, error="UNAUTHORIZED"):
    assert status == 401, (status, body)
    assert "x-rate-limit-cost" not in headers, headers
    if head:
        assert body == b"", body
    else:
        assert json.loads(body) == {"error": error}, body


def check_binary_hygiene(normal: str, test_binary: str) -> None:
    normal_bytes = Path(normal).read_bytes()
    test_bytes = Path(test_binary).read_bytes()
    for marker in TEST_SUBSTITUTION_MARKERS:
        assert marker in test_bytes, f"positive control missing from test binary: {marker!r}"
        assert marker not in normal_bytes, f"normal binary contains test-only {marker!r}"
    assert CANARY.encode() in normal_bytes


def check_startup_failures(normal: str, test_binary: str, root: Path,
                           ca: Path, invalid_ca: Path) -> None:
    kv_dir = root / "startup-kv"
    kv_dir.mkdir()
    cases = [
        (normal, {CANARY: "1"}, "requires FORTIS_CA_BUNDLE_PATH"),
        (normal, {CANARY: "yes", "FORTIS_CA_BUNDLE_PATH": str(ca)}, "must be 0 or 1"),
        (normal, {CANARY: "1", "FORTIS_CA_BUNDLE_PATH": str(invalid_ca)},
         "Invalid CA bundle"),
        # The coordinator has started when the KV open fails: it must stop
        # cleanly (exit 2, not an abort) before the process returns.
        (normal, {CANARY: "1", "FORTIS_CA_BUNDLE_PATH": str(ca),
                  "FORTIS_KV_DIR": str(root / "absent" / "kv")},
         "Failed to open the native price route KV"),
        (test_binary, {CANARY: "1", "FORTIS_CA_BUNDLE_PATH": str(ca),
                       "FORTIS_ENABLE_TEST_PRICE_ROUTE": "1",
                       "FORTIS_TEST_PRICE_UPSTREAM_PORT": "9",
                       "FORTIS_TEST_PRICE_UPSTREAM_CA": str(ca)}, "mutually exclusive"),
    ]
    for binary, overrides, message in cases:
        result = subprocess.run([binary, str(free_port())],
                                env=environment(kv_dir, **overrides),
                                capture_output=True, timeout=10, check=False)
        assert result.returncode == 2, (overrides, result.returncode, result.stderr)
        assert message.encode() in result.stderr, (overrides, result.stderr)


def check_disabled(normal: str, root: Path, ca: Path):
    variants = (
        ("unset", {}),
        ("zero", {CANARY: "0", "FORTIS_CA_BUNDLE_PATH": str(ca)}),
        # The normal binary has no test target substitution to opt into.
        ("test-only flags", {"FORTIS_ENABLE_TEST_PRICE_ROUTE": "1",
                             "FORTIS_TEST_PRICE_UPSTREAM_PORT": "9",
                             "FORTIS_TEST_PRICE_UPSTREAM_CA": str(ca)}),
    )
    threads = {}
    for label, overrides in variants:
        kv_dir = root / f"disabled-{label.replace(' ', '-')}"
        kv_dir.mkdir()
        process, port = start(normal, environment(kv_dir, **overrides))
        try:
            # Sampled at the same point as the opt-in server's count.
            threads[label] = thread_count(process.pid)
            token = enroll(port)
            for headers in ({}, {"Authorization": "Bearer " + token},
                            {"Authorization": "Bearer " + FIXTURE_TOKEN}):
                for method in ("GET", "HEAD"):
                    status, _, _ = exchange(port, method, "/v1/prices?ids=ethereum", headers)
                    assert status == 404, (label, method, headers, status)
            assert exchange(port, "GET", "/probe/test-price-owner")[0] == 404, label
        finally:
            returncode = terminate(process)
        assert returncode == 0, (label, returncode)
    if threads["unset"] is not None:
        assert threads["unset"] == threads["zero"] == threads["test-only flags"], threads
    return threads["unset"]


def check_enabled(normal: str, root: Path, ca: Path, disabled_threads) -> None:
    kv_dir = root / "canary-kv"
    kv_dir.mkdir()
    canary_env = {CANARY: "1", "FORTIS_CA_BUNDLE_PATH": str(ca)}
    process, port = start(normal, environment(kv_dir, **canary_env))
    try:
        enabled_threads = thread_count(process.pid)
        if disabled_threads is not None and enabled_threads is not None:
            # The coordinator and owner threads start only with the opt-in.
            assert enabled_threads > disabled_threads, (disabled_threads, enabled_threads)

        # Registered: bearer auth answers before any provider work, and no
        # development fixture credential was seeded as a side effect.
        for method in ("GET", "HEAD"):
            assert_unauthorized(*exchange(port, method, "/v1/prices?ids=ethereum"),
                                head=method == "HEAD")
        for authorization in ("Bearer malformed", "Bearer " + FIXTURE_TOKEN,
                              "Bearer fintel_dev_" + "A" * 43):
            assert_unauthorized(*exchange(port, "GET", "/v1/prices?ids=ethereum",
                                          {"Authorization": authorization}))
        assert exchange(port, "GET", "/probe/test-price-owner")[0] == 404

        token = enroll(port)
        auth = {"Authorization": "Bearer " + token}
        status, _, body = exchange(port, "GET", "/v1/prices?ids=ethereum",
                                   {**auth, "Origin": "https://example.test"})
        assert status == 403 and json.loads(body) == {"error": "BROWSER_ORIGIN_FORBIDDEN"}

        wait_for_quota_window()
        remaining = DEVELOPMENT_QUOTA

        def charged(status, headers, expected_status):
            nonlocal remaining
            remaining -= 1
            assert status == expected_status, (status, headers)
            assert headers["x-rate-limit-cost"] == "1", headers
            assert headers["x-rate-limit-remaining"] == str(max(remaining, 0)), headers

        # The normal server advertises its composed live planes.
        status, headers, body = exchange(port, "GET", "/v1/networks", auth)
        charged(status, headers, 200)
        networks = json.loads(body)
        assert_live_catalog(networks, json.loads(NETWORK_FIXTURE.read_text())["json"]["body"])
        assert networks["prices"]["spot"] is True

        # Query validation after auth and quota. None of these reach the owner.
        invalid_queries = [
            ("/v1/prices", None),
            ("/v1/prices?ids=", None),
            ("/v1/prices?ids=%20%20%2C", "1–100 ids"),
            ("/v1/prices?ids=ethereum&fiat=USD", None),
            ("/v1/prices?ids=ethereum&fiat=us", None),
            ("/v1/prices?ids=" + ",".join(["ethereum"] * 101), "1–100 ids"),
            ("/v1/prices?ids=" + "x" * 4001, None),
        ]
        for path, detail in invalid_queries:
            status, headers, body = exchange(port, "GET", path, auth)
            charged(status, headers, 400)
            assert headers["content-type"] == "application/json; charset=utf-8"
            # TypeScript adds Vary only on the success path (sendProtobufOrJson).
            assert "vary" not in headers, headers
            reply = json.loads(body)
            assert reply["error"] == "BAD_REQUEST", (path, body)
            if detail:
                assert reply == {"error": "BAD_REQUEST", "detail": detail}, (path, body)
            else:  # a schema refusal: no zod issue list
                assert reply == {"error": "BAD_REQUEST"}, (path, body)
        # The first repeated value wins; unknown source data remains missing.
        status, headers, body = exchange(port, "GET", "/v1/prices?ids=ethereum&ids=bitcoin", auth)
        charged(status, headers, 200)
        assert json.loads(body) == {"fiat":"usd","prices":{},"missing":["ethereum"],"invalid":[]}, body
        # Malformed percent encoding is now a charged request refusal.
        status, headers, body = exchange(port, "GET", "/v1/prices?ids=%ZZ", auth)
        charged(status, headers, 400)
        assert json.loads(body) == {"error":"BAD_REQUEST"}, body
        status, headers, body = exchange(port, "HEAD", "/v1/prices", auth)
        charged(status, headers, 400)
        assert body == b""

        # Exhaust the development quota with requests that fail validation.
        while remaining > 0:
            charged(*exchange(port, "GET", "/v1/prices", auth)[:2], 400)
        for _ in range(2):  # quota is charged before validation, as in Fastify
            status, headers, body = exchange(port, "GET", "/v1/prices", auth)
            assert status == 429 and json.loads(body) == {"error": "RATE_LIMITED"}
            assert headers["retry-after"] == "60"
            assert headers["x-rate-limit-cost"] == "1"
            assert headers["x-rate-limit-remaining"] == "0"
    finally:
        returncode = terminate(process)
    assert returncode == 0, returncode

    # The route applies the configured attestation mode on a restart over the
    # same KV: the development device enrolled above is refused.
    process, port = start(normal, environment(kv_dir, ATTESTATION_MODE="required",
                                              **canary_env))
    try:
        assert_unauthorized(*exchange(port, "GET", "/v1/prices?ids=ethereum", auth),
                            error="ATTESTATION_REQUIRED")
    finally:
        returncode = terminate(process)
    assert returncode == 0, returncode


def check_test_binary_canary(test_binary: str, root: Path, ca: Path) -> None:
    """Without its test flag the test binary also uses production startup."""
    kv_dir = root / "test-binary-canary-kv"
    kv_dir.mkdir()
    process, port = start(test_binary, environment(
        kv_dir, **{CANARY: "1", "FORTIS_CA_BUNDLE_PATH": str(ca)}))
    try:
        assert_unauthorized(*exchange(port, "GET", "/v1/prices?ids=ethereum",
                                      {"Authorization": "Bearer " + FIXTURE_TOKEN}))
        assert exchange(port, "GET", "/probe/test-price-owner")[0] == 404
    finally:
        returncode = terminate(process)
    assert returncode == 0, returncode


def check_shutdown_with_inflight_request(test_binary: str, root: Path) -> None:
    """SIGTERM while a provider call is open: owner, then coordinator, then handoff."""
    stub_dir = root / "stub"
    stub_dir.mkdir()
    ca, cert, key, _ = tls_ca.create_certificates(stub_dir, "DNS:localhost")
    arrived = threading.Event()
    release = threading.Event()
    upstream_paths = []

    class StallingHandler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            upstream_paths.append(self.path)
            arrived.set()
            release.wait(timeout=20)
            self.close_connection = True

        def log_message(self, *_args):
            pass

    stub = ThreadingHTTPServer(("127.0.0.1", free_port()), StallingHandler)
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(cert, key)
    stub.socket = tls.wrap_socket(stub.socket, server_side=True)
    serving = threading.Thread(target=stub.serve_forever, daemon=True)
    serving.start()
    kv_dir = root / "inflight-kv"
    kv_dir.mkdir()
    process, port = start(test_binary, environment(
        kv_dir, FORTIS_ENABLE_TEST_PRICE_ROUTE="1",
        FORTIS_TEST_PRICE_UPSTREAM_PORT=str(stub.server_address[1]),
        FORTIS_TEST_PRICE_UPSTREAM_CA=str(ca)))
    outcome = {}

    def client():
        try:
            outcome["response"] = exchange(
                port, "GET", "/v1/prices?ids=stallcoin",
                {"Authorization": "Bearer " + FIXTURE_TOKEN}, timeout=30)
        except BaseException as error:  # reported by the main thread
            outcome["error"] = error

    waiter = threading.Thread(target=client)
    try:
        waiter.start()
        assert arrived.wait(timeout=10), "provider request did not reach the stub"
        status, _, body = exchange(port, "GET", "/probe/test-price-owner")
        assert status == 200 and json.loads(body) == {"active": 1}, body
        returncode = terminate(process, timeout=25)
        assert returncode == 0, returncode
        waiter.join(timeout=10)
        assert not waiter.is_alive() and "error" not in outcome, outcome
        status, _, body = outcome["response"]
        assert status == 503 and json.loads(body) == {"error": "SHUTDOWN"}, (status, body)
        assert len(upstream_paths) == 1, upstream_paths
        assert upstream_paths[0].startswith("/api/v3/simple/price?"), upstream_paths
        assert "stallcoin" in upstream_paths[0], upstream_paths
    finally:
        release.set()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        waiter.join(timeout=5)
        stub.shutdown()
        stub.server_close()
        serving.join(timeout=5)


def main() -> None:
    normal = str(Path(sys.argv[1]).resolve())
    test_binary = str(Path(sys.argv[2]).resolve())
    check_binary_hygiene(normal, test_binary)
    with tempfile.TemporaryDirectory(prefix="native-price-canary-") as temporary:
        root = Path(temporary)
        ca_dir = root / "ca"
        ca_dir.mkdir()
        ca, _, _, _ = tls_ca.create_certificates(ca_dir, "DNS:localhost")
        invalid_ca = root / "invalid-ca.pem"
        invalid_ca.write_text("not a PEM CA bundle\n")
        check_startup_failures(normal, test_binary, root, ca, invalid_ca)
        disabled_threads = check_disabled(normal, root, ca)
        check_enabled(normal, root, ca, disabled_threads)
        check_test_binary_canary(test_binary, root, ca)
        check_shutdown_with_inflight_request(test_binary, root)
    print("native /v1/prices canary: flag-off 404, opt-in auth/validation/quota, "
          "startup failures, clean shutdown and test-substitution hygiene passed")


if __name__ == "__main__":
    main()
