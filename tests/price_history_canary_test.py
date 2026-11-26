#!/usr/bin/env python3
"""Canary opt-in checks for native price history and OHLC in the normal binary.

Usage: price_history_canary_test.py <normal binary> <history route test binary>

FORTIS_NATIVE_PRICE_ROUTE_CANARY=1 composes GET /v1/prices, GET and POST
/v1/prices/history and GET /v1/prices/ohlc together. No request sent to the
normal binary can reach a provider owner: each one is answered by Origin,
bearer, quota or validation first. Its CA bundle is a freshly generated local
CA and provider keys are scrubbed from the environment. The data-path checks
stay in the existing history replays; the in-flight shutdown check runs in the
test binary, through the same composition, against a loopback generated-CA
stub.
"""

import json
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import tls_ca
from network_route_test import assert_live_catalog
from price_spot_canary_test import (
    CANARY, DEVELOPMENT_QUOTA, NETWORK_FIXTURE, SCRUBBED_ENV, assert_unauthorized,
    enroll, exchange, free_port, start, terminate, thread_count, wait_for_quota_window)

HISTORY_FIXTURE_TOKEN = "fintel_dev_" + "H" * 43
HISTORY_TEST_ENV = (
    "FORTIS_ENABLE_TEST_PRICE_HISTORY_ROUTE", "FORTIS_TEST_HISTORY_UPSTREAM_PORT",
    "FORTIS_TEST_HISTORY_UPSTREAM_CA", "FORTIS_TEST_HISTORY_FAIL_SOURCE",
    "FORTIS_TEST_HISTORY_NOW_MS", "FORTIS_TEST_HISTORY_OWNER_START_FAIL",
    "FORTIS_TEST_HISTORY_BATCH_SEED", "FORTIS_TEST_HISTORY_DECADE_SEED",
    "FORTIS_TEST_HISTORY_OTHER_IDS_SEED", "FORTIS_TEST_OHLC_SEED",
    "FORTIS_TEST_OHLC_RAND", "FORTIS_TEST_OHLC_NOW_FILE", "FORTIS_TEST_OHLC_NOW_MS")
# Present only in binaries built with the history test plane (tests/server)
# and the owners' instrumentation hooks.
TEST_SUBSTITUTION_MARKERS = tuple(name.encode() for name in HISTORY_TEST_ENV) + (
    b"HistoryTestUpstream", b"17OhlcProviderOwner23SetAdmissionHookForTest",
    b"FORTIS_HISTORY_TEST_COMMIT_MARKER", b"SeedPriceRouteFixture", b"CorruptMint")
HISTORY_PATH = "/v1/prices/history?id=ethereum&from=1&to=2"
# A schema refusal carries no zod issue list (compat-decisions.md#zod-issues).
PLAIN_BAD_REQUEST = None
OHLC_PATH = "/v1/prices/ohlc?id=bitcoin&days=1"
BATCH_BODY = b'{"ids":["bitcoin"],"from":1,"to":2}'
JSON = {"Content-Type": "application/json"}


def environment(kv_dir: Path, **overrides) -> dict:
    env = os.environ.copy()
    for name in SCRUBBED_ENV + HISTORY_TEST_ENV:
        env.pop(name, None)
    env["FORTIS_KV_DIR"] = str(kv_dir)
    env.update(overrides)
    return env


def price_requests(headers):
    """Every price route and method, including the 4a spot route."""
    return (
        ("GET", "/v1/prices?ids=ethereum", headers, None),
        ("HEAD", "/v1/prices?ids=ethereum", headers, None),
        ("GET", HISTORY_PATH, headers, None),
        ("HEAD", HISTORY_PATH, headers, None),
        ("POST", "/v1/prices/history", {**headers, **JSON}, BATCH_BODY),
        ("POST", "/v1/prices/history", headers, b""),
        ("GET", OHLC_PATH, headers, None),
        ("HEAD", OHLC_PATH, headers, None),
    )


def check_binary_hygiene(normal: str, test_binary: str) -> None:
    normal_bytes = Path(normal).read_bytes()
    test_bytes = Path(test_binary).read_bytes()
    for marker in TEST_SUBSTITUTION_MARKERS:
        assert marker in test_bytes, f"positive control missing from test binary: {marker!r}"
        assert marker not in normal_bytes, f"normal binary contains test-only {marker!r}"
    # The routes themselves are compiled into the normal binary.
    for route in (b"/v1/prices/history", b"/v1/prices/ohlc", CANARY.encode()):
        assert route in normal_bytes, route


def check_startup_failures(test_binary: str, root: Path, ca: Path) -> None:
    kv_dir = root / "history-startup-kv"
    kv_dir.mkdir()
    upstream = {"FORTIS_ENABLE_TEST_PRICE_HISTORY_ROUTE": "1",
                "FORTIS_TEST_HISTORY_UPSTREAM_PORT": str(free_port()),
                "FORTIS_TEST_HISTORY_UPSTREAM_CA": str(ca)}
    cases = [
        ({CANARY: "1", "FORTIS_CA_BUNDLE_PATH": str(ca), **upstream}, "mutually exclusive"),
        # Nothing started: the coordinator stops with no lease.
        ({**upstream, "FORTIS_TEST_HISTORY_OWNER_START_FAIL": "backfill"},
         "Failed to start the native price history test owners"),
        # The backfill owner started and holds a lease: the composition must
        # stop it before the coordinator, or the coordinator refuses to stop.
        ({**upstream, "FORTIS_TEST_HISTORY_OWNER_START_FAIL": "ohlc"},
         "Failed to start the native price history test owners"),
    ]
    for overrides, message in cases:
        port = free_port()
        result = subprocess.run([test_binary, str(port)],
                                env=environment(kv_dir, **overrides),
                                capture_output=True, timeout=20, check=False)
        assert result.returncode == 2, (overrides, result.returncode, result.stderr)
        assert message.encode() in result.stderr, (overrides, result.stderr)
        assert b"Failed to stop the provider execution coordinator" not in result.stderr
        assert b"listening" not in result.stdout, "startup failure must precede bind"


def check_disabled(normal: str, root: Path, ca: Path):
    variants = (
        ("unset", {}),
        ("zero", {CANARY: "0", "FORTIS_CA_BUNDLE_PATH": str(ca)}),
        # The normal binary has no history test substitution to opt into.
        ("test-only flags", {"FORTIS_ENABLE_TEST_PRICE_HISTORY_ROUTE": "1",
                             "FORTIS_TEST_HISTORY_UPSTREAM_PORT": "9",
                             "FORTIS_TEST_HISTORY_UPSTREAM_CA": str(ca),
                             "FORTIS_TEST_OHLC_SEED": "1"}),
    )
    threads = {}
    for label, overrides in variants:
        kv_dir = root / f"history-disabled-{label.replace(' ', '-')}"
        kv_dir.mkdir()
        process, port = start(normal, environment(kv_dir, **overrides))
        try:
            threads[label] = thread_count(process.pid)
            token = enroll(port)
            for headers in ({}, {"Authorization": "Bearer " + token},
                            {"Authorization": "Bearer " + HISTORY_FIXTURE_TOKEN}):
                for method, path, request_headers, body in price_requests(headers):
                    status, _, _ = exchange(port, method, path, request_headers, body)
                    assert status == 404, (label, method, path, headers, status)
        finally:
            returncode = terminate(process)
        assert returncode == 0, (label, returncode)
    if threads["unset"] is not None:
        # No provider coordinator, owner, HTTPS worker or batch driver thread.
        assert threads["unset"] == threads["zero"] == threads["test-only flags"], threads
    return threads["unset"]


def check_enabled(normal: str, root: Path, ca: Path, disabled_threads) -> None:
    kv_dir = root / "history-canary-kv"
    kv_dir.mkdir()
    canary_env = {CANARY: "1", "FORTIS_CA_BUNDLE_PATH": str(ca)}
    process, port = start(normal, environment(kv_dir, **canary_env))
    try:
        enabled_threads = thread_count(process.pid)
        if disabled_threads is not None and enabled_threads is not None:
            # Coordinator, three owners and the batch driver start with the opt-in.
            assert enabled_threads > disabled_threads, (disabled_threads, enabled_threads)

        # Registered: every route and method answers bearer auth before any
        # provider work, and no fixture credential was seeded.
        for authorization in (None, "Bearer malformed", "Bearer " + HISTORY_FIXTURE_TOKEN,
                              "Bearer fintel_dev_" + "A" * 43):
            headers = {"Authorization": authorization} if authorization else {}
            for method, path, request_headers, body in price_requests(headers):
                assert_unauthorized(*exchange(port, method, path, request_headers, body),
                                    head=method == "HEAD")
        # fastify-errors: a malformed JSON body without a bearer is
        # 401, authentication first.
        status, headers, body = exchange(port, "POST", "/v1/prices/history", JSON, b"{")
        assert status == 401 and "x-rate-limit-cost" not in headers, (status, headers)
        assert json.loads(body) == {"error": "UNAUTHORIZED"}, body

        token = enroll(port)
        auth = {"Authorization": "Bearer " + token}
        origin = {**auth, "Origin": "https://example.test"}
        for method, path, request_headers, body in price_requests(origin):
            if method == "HEAD":
                continue
            status, _, response = exchange(port, method, path, request_headers, body)
            assert status == 403, (method, path, status)
            assert json.loads(response) == {"error": "BROWSER_ORIGIN_FORBIDDEN"}

        wait_for_quota_window()
        remaining = DEVELOPMENT_QUOTA

        def charged(status, headers, expected_status, cost):
            nonlocal remaining
            remaining -= cost
            assert status == expected_status, (status, headers)
            assert headers["x-rate-limit-cost"] == str(cost), headers
            assert headers["x-rate-limit-remaining"] == str(max(remaining, 0)), headers

        # The normal server advertises its composed live planes.
        status, headers, body = exchange(port, "GET", "/v1/networks", auth)
        remaining -= 1
        assert status == 200 and headers["x-rate-limit-remaining"] == str(remaining)
        networks = json.loads(body)
        assert_live_catalog(networks, json.loads(NETWORK_FIXTURE.read_text())["json"]["body"])
        assert networks["prices"]["history"] is True

        # GET /v1/prices/history costs 1. None of these reach the owner: a
        # valid CoinGecko query with a missing window would, so none is sent.
        # Schema refusals (PLAIN_BAD_REQUEST) carry no detail member.
        history_cases = [
            ("/v1/prices/history", PLAIN_BAD_REQUEST),
            ("/v1/prices/history?id=ethereum&from=1&to=2&interval=5m", PLAIN_BAD_REQUEST),
            # Current query parsing rejects malformed escapes before ID lookup.
            ("/v1/prices/history?id=%ZZ&from=1&to=2", PLAIN_BAD_REQUEST),
            ("/v1/prices/history?id=" + "a" * 201 + "&from=1&to=2", PLAIN_BAD_REQUEST),
            ("/v1/prices/history?id=ethereum&from=10&to=5", "range for 1h must be 1s–7776000s"),
            ("/v1/prices/history?id=ethereum&from=1&to=7776002",
             "range for 1h must be 1s–7776000s"),
            ("/v1/prices/history?id=ethereum&from=1&to=316224002&interval=1d",
             "range for 1d must be 1s–316224000s"),
            ("/v1/prices/history?id=!!!&from=1&to=2", "unrecognized id"),
        ]
        for path, detail in history_cases:
            status, headers, body = exchange(port, "GET", path, auth)
            charged(status, headers, 400, 1)
            assert headers["content-type"] == "application/json; charset=utf-8", headers
            expected = {"error": "BAD_REQUEST"}
            if detail is not PLAIN_BAD_REQUEST:
                expected["detail"] = detail
            assert json.loads(body) == expected, (path, body)
        status, headers, body = exchange(
            port, "GET", "/v1/prices/history?id=ethereum&from=abc&to=2", auth)
        charged(status, headers, 400, 1)
        assert json.loads(body) == {"error": "BAD_REQUEST"}, body
        status, headers, body = exchange(port, "HEAD", "/v1/prices/history", auth)
        charged(status, headers, 400, 1)
        assert body == b""

        # GET /v1/prices/ohlc costs 5; days and fiat are validated before any
        # cache read or provider work.
        for path in ("/v1/prices/ohlc", "/v1/prices/ohlc?id=bitcoin",
                     "/v1/prices/ohlc?id=bitcoin&days=2",
                     "/v1/prices/ohlc?id=bitcoin&days=1&fiat=eur",
                     "/v1/prices/ohlc?id=bitcoin&days=2&days=1",
                     "/v1/prices/ohlc?id=&days=1"):
            status, headers, body = exchange(port, "GET", path, auth)
            charged(status, headers, 400, 5)
            assert json.loads(body) == {"error": "BAD_REQUEST"}, (path, body)

        # POST /v1/prices/history costs max(1, ids) and validates before the
        # range and size policy, all before any provider submission.
        batch_cases = [
            (auth, b"", 1, None),
            ({**auth, **JSON}, b'{"ids":[],"from":1,"to":2}', 1, None),
            ({**auth, **JSON}, b'{"ids":["bitcoin"],"from":2,"to":1}', 1,
             "invalid history range"),
            ({**auth, **JSON}, b'{"ids":["bitcoin","ethereum"],"from":1,"to":7776002}', 2,
             "invalid history range"),
            ({**auth, "Content-Type": "application/protobuf"}, b"\xff", 1,
             "invalid protobuf body"),
        ]
        for request_headers, request_body, cost, detail in batch_cases:
            status, headers, body = exchange(port, "POST", "/v1/prices/history",
                                             request_headers, request_body)
            charged(status, headers, 400, cost)
            payload = json.loads(body)
            assert payload["error"] == "BAD_REQUEST", (request_body, payload)
            if detail:
                assert payload["detail"] == detail, (request_body, payload)
            else:  # a schema refusal: no zod issue list
                assert payload == {"error": "BAD_REQUEST"}, payload
    finally:
        returncode = terminate(process)
    assert returncode == 0, returncode

    # The routes apply the configured attestation mode on a restart over the
    # same KV: the development device enrolled above is refused.
    process, port = start(normal, environment(kv_dir, ATTESTATION_MODE="required",
                                              **canary_env))
    try:
        for method, path, request_headers, body in price_requests(auth):
            if method == "HEAD":
                continue
            assert_unauthorized(*exchange(port, method, path, request_headers, body),
                                error="ATTESTATION_REQUIRED")
    finally:
        returncode = terminate(process)
    assert returncode == 0, returncode


def check_test_binary_canary(test_binary: str, root: Path, ca: Path) -> None:
    """Without its test flag the history test binary uses production startup."""
    kv_dir = root / "history-test-binary-canary-kv"
    kv_dir.mkdir()
    process, port = start(test_binary, environment(
        kv_dir, **{CANARY: "1", "FORTIS_CA_BUNDLE_PATH": str(ca),
                   "FORTIS_TEST_OHLC_SEED": "1"}))
    try:
        for method, path, request_headers, body in price_requests(
                {"Authorization": "Bearer " + HISTORY_FIXTURE_TOKEN}):
            assert_unauthorized(*exchange(port, method, path, request_headers, body),
                                head=method == "HEAD")
    finally:
        returncode = terminate(process)
    assert returncode == 0, returncode


def check_shutdown_with_inflight_requests(test_binary: str, root: Path) -> None:
    """SIGTERM with a history GET, an OHLC GET and a batch POST all waiting.

    Workers join, the batch driver answers its batch, both owners answer their
    requests through the handoff, then the coordinator and handoff stop.
    """
    stub_dir = root / "history-stub"
    stub_dir.mkdir()
    ca, cert, key, _ = tls_ca.create_certificates(stub_dir, "DNS:localhost")
    release = threading.Event()
    arrivals = threading.Semaphore(0)
    upstream_paths = []

    class StallingHandler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            upstream_paths.append(self.path)
            arrivals.release()
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
    kv_dir = root / "history-inflight-kv"
    kv_dir.mkdir()
    process, port = start(test_binary, environment(
        kv_dir, FORTIS_ENABLE_TEST_PRICE_HISTORY_ROUTE="1",
        # This shutdown scenario needs two active calls. Anonymous CoinGecko
        # intentionally admits only one; use a credentialed local fixture lane.
        COINGECKO_API_KEY="fixture-coingecko-pro-key", COINGECKO_TIER="pro",
        FORTIS_TEST_HISTORY_UPSTREAM_PORT=str(stub.server_address[1]),
        FORTIS_TEST_HISTORY_UPSTREAM_CA=str(ca)))
    auth = {"Authorization": "Bearer " + HISTORY_FIXTURE_TOKEN}
    now = int(time.time()) // 3600 * 3600
    requests = {
        "history": ("GET", f"/v1/prices/history?id=stallcoin&from={now - 7200}&to={now}",
                    auth, None),
        "ohlc": ("GET", "/v1/prices/ohlc?id=stallcoin-ohlc&days=1", auth, None),
        "batch": ("POST", "/v1/prices/history", {**auth, **JSON},
                  json.dumps({"ids": ["stallcoin-batch"], "from": now - 7200,
                              "to": now}).encode()),
    }
    outcomes = {}
    waiters = []

    def client(name):
        method, path, headers, body = requests[name]
        try:
            outcomes[name] = exchange(port, method, path, headers, body, timeout=30)
        except BaseException as error:  # reported by the main thread
            outcomes[name] = error

    try:
        # The CoinGecko lane admits two concurrent calls; the batch queues
        # behind them in the lane or the owner.
        for name in ("history", "ohlc"):
            waiter = threading.Thread(target=client, args=(name,))
            waiter.start()
            waiters.append(waiter)
            assert arrivals.acquire(timeout=10), f"{name} request did not reach the stub"
        waiter = threading.Thread(target=client, args=("batch",))
        waiter.start()
        waiters.append(waiter)
        time.sleep(0.75)
        assert all(name not in outcomes for name in requests), outcomes
        returncode = terminate(process, timeout=25)
        assert returncode == 0, returncode
        for waiter in waiters:
            waiter.join(timeout=10)
            assert not waiter.is_alive()
        expected = {
            "history": (503, {"error": "UPSTREAM_UNAVAILABLE"}),
            # A shutdown cancellation is not a provider verdict: 503, as
            # for history and batch (a deadline or provider failure is 502).
            "ohlc": (503, {"error": "UPSTREAM_UNAVAILABLE"}),
            "batch": (503, {"error": "UPSTREAM_UNAVAILABLE"}),
        }
        for name, (status, body) in expected.items():
            outcome = outcomes.get(name)
            assert isinstance(outcome, tuple), (name, outcome)
            assert outcome[0] == status and json.loads(outcome[2]) == body, (name, outcome)
        assert any(path.startswith("/api/v3/coins/stallcoin/market_chart/range?")
                   for path in upstream_paths), upstream_paths
        assert any(path.startswith("/api/v3/coins/stallcoin-ohlc/ohlc?")
                   for path in upstream_paths), upstream_paths
        # The queued batch job may be dispatched while the OHLC job is torn down.
        assert len(upstream_paths) <= 3, upstream_paths
        for path in upstream_paths:
            assert "/coins/stallcoin" in path, upstream_paths
    finally:
        release.set()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        for waiter in waiters:
            waiter.join(timeout=5)
        stub.shutdown()
        stub.server_close()
        serving.join(timeout=5)


def main() -> None:
    normal = str(Path(sys.argv[1]).resolve())
    test_binary = str(Path(sys.argv[2]).resolve())
    check_binary_hygiene(normal, test_binary)
    with tempfile.TemporaryDirectory(prefix="native-history-canary-") as temporary:
        root = Path(temporary)
        ca_dir = root / "ca"
        ca_dir.mkdir()
        ca, _, _, _ = tls_ca.create_certificates(ca_dir, "DNS:localhost")
        check_startup_failures(test_binary, root, ca)
        disabled_threads = check_disabled(normal, root, ca)
        check_enabled(normal, root, ca, disabled_threads)
        check_test_binary_canary(test_binary, root, ca)
        check_shutdown_with_inflight_requests(test_binary, root)
    print("native price history/OHLC canary: flag-off 404 on every route, opt-in "
          "auth/validation/quota, startup failures, clean in-flight shutdown and "
          "test-substitution hygiene passed")


if __name__ == "__main__":
    main()
