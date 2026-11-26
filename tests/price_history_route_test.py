#!/usr/bin/env python3
"""Differential checks for the gated cached-read native history route."""
import json
from concurrent.futures import ThreadPoolExecutor
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
from urllib.parse import urlsplit, parse_qs

import compat_expectations
import tls_ca
from history_contract_expectations import FIXTURE_PROVIDER

FIXTURE = Path(__file__).resolve().parents[1] / "tests/corpus/price_history_route.json"
OHLC_FIXTURE = Path(__file__).resolve().parents[1] / "tests/corpus/price_ohlc_route.json"
OTHER_IDS_FIXTURE = FIXTURE.parent / "price_history_other_ids.json"
# TypeScript turns a thrown store read into 500 INTERNAL; native answers real
# KV I/O or corruption with 503 UPSTREAM_UNAVAILABLE (documented difference).
OTHER_IDS_NATIVE_DIFFERENCES = {
    "storeFailureJson": ((500, '{"error":"INTERNAL"}'),
                         (503, b'{"error":"UPSTREAM_UNAVAILABLE"}')),
}
TOKEN = "fintel_dev_" + "H" * 43
FIRST = 1767225600
OHLC_NOW_MS = 1767323045123
OHLC_REFRESH_MS = OHLC_NOW_MS + 1

def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]

def request(port, method, url, accept="application/json", auth=True, origin=None,
            disconnect=False):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        lines = [f"{method} {url} HTTP/1.1", "Host: localhost", "Connection: close"]
        if accept:
            lines.append("Accept: " + accept)
        if auth:
            lines.append("Authorization: Bearer " + TOKEN)
        if origin:
            lines.append("Origin: " + origin)
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
    header, _, body = bytes(data).partition(b"\r\n\r\n")
    lines = header.decode("latin1").split("\r\n")
    status = int(lines[0].split()[1])
    headers = {key.lower(): value.strip() for line in lines[1:] if ":" in line
               for key, value in [line.split(":", 1)]}
    return status, headers, body

def start(binary, directory, enabled=True, history_env=None):
    directory.mkdir(parents=True, exist_ok=True)
    kv = directory / "kv"
    kv.mkdir(exist_ok=True)
    port = free_port()
    env = os.environ.copy()
    env["FORTIS_KV_DIR"] = str(kv)
    env.pop("FORTIS_ENABLE_TEST_PRICE_HISTORY_ROUTE", None)
    if enabled:
        env["FORTIS_ENABLE_TEST_PRICE_HISTORY_ROUTE"] = "1"
        env.update(history_env or {})
    process = subprocess.Popen([binary, str(port)], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    for _ in range(120):
        if process.poll() is not None:
            raise RuntimeError(process.stderr.read().decode())
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=.1):
                return process, port
        except OSError:
            time.sleep(.03)
    raise RuntimeError("history test server did not start")

def stop(process):
    process.terminate()
    process.wait(timeout=5)

def wait_for_quota_window(seconds_needed):
    # Quota counters are per wall-clock minute; keep a segment inside one.
    left = 60 - time.time() % 60
    if left < seconds_needed:
        time.sleep(left + .2)

def replay_other_ids(binary, root, history_env, provider_hits):
    """Replays the frozen non-CoinGecko GET fixture, one process per
    TypeScript app; the restarted process reuses its KV and is not reseeded.
    Every case must leave the CoinGecko stub untouched."""
    fixture = json.loads(OTHER_IDS_FIXTURE.read_text())
    segments = []
    for name in fixture["order"]:
        segment = fixture["cases"][name]["segment"]
        if not segments or segments[-1][0] != segment:
            segments.append((segment, []))
        segments[-1][1].append(name)
    replayed = 0
    for segment, names in segments:
        env = dict(history_env)
        if segment != "restartAfter":
            env["FORTIS_TEST_HISTORY_OTHER_IDS_SEED"] = "1"
        directory = root / ("other-ids-" + ("restart" if segment == "restartAfter" else segment))
        wait_for_quota_window(15)
        process, port = start(binary, directory, history_env=env)
        try:
            for name in names:
                case = fixture["cases"][name]
                spec = case["request"]
                before = provider_hits()
                status, headers, body = request(port, spec["method"], spec["url"],
                                                accept=spec["accept"], auth=spec["auth"])
                assert case["providerCalls"] == 0 and provider_hits() == before, name
                if name in OTHER_IDS_NATIVE_DIFFERENCES:
                    (ts_status, ts_text), native = OTHER_IDS_NATIVE_DIFFERENCES[name]
                    assert (case["status"], case["body"]["text"]) == (ts_status, ts_text), name
                    assert (status, body) == native, (name, status, body)
                    continue
                assert status == case["status"], (name, status, case["status"], body[:200])
                for key, value in case["headers"].items():
                    assert headers.get(key) == value, (name, key, headers.get(key), value)
                for key in ("x-rate-limit-cost", "retry-after", "vary"):
                    assert (key in headers) == (key in case["headers"]), (name, key, headers)
                expected = case["body"]
                if "wireHex" in expected:
                    assert body.hex() == expected["wireHex"], (name, body.hex())
                else:
                    assert body == expected["text"].encode(), (name, body, expected["text"])
                replayed += 1
        finally:
            stop(process)
    return replayed

def main(binary):
    fixture = json.loads(FIXTURE.read_text())
    ohlc_fixture = json.loads(OHLC_FIXTURE.read_text())
    cases = fixture["cases"]
    base = f"/v1/prices/history?id=ethereum&from={FIRST}"
    hourly = base + f"&to={FIRST + 6 * 3600}&interval=1h"
    daily = base + f"&to={FIRST + 6 * 86400}&interval=1d"
    query_error = f"/v1/prices/history?id=ethereum&from=nope&to={FIRST + 6 * 3600}&interval=1h"
    range_error = base + f"&to={FIRST}&interval=1h"
    unknown = f"/v1/prices/history?id=unknown%20id&from={FIRST}&to={FIRST + 6 * 3600}&interval=1h"
    with tempfile.TemporaryDirectory(prefix="history-route-") as temporary:
        root = Path(temporary)
        ca, cert, key, _ = tls_ca.create_certificates(root, "DNS:localhost")
        upstream_hits = []
        ohlc_hits = []
        class HistoryHandler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"
            def do_GET(self):
                parsed = urlsplit(self.path)
                values = parse_qs(parsed.query)
                if parsed.path.startswith("/api/v3/coins/") and parsed.path.endswith("/ohlc"):
                    slug = parsed.path.split("/")[-2]
                    ohlc_hits.append((slug, values))
                    time.sleep(.5 if slug == "shutdown-coin" else .1)
                    if slug == "failing-coin":
                        encoded = b"provider unavailable"
                        self.send_response(503)
                    else:
                        point = ([OHLC_NOW_MS, 100, 110, 90, 105]
                                 if len([hit for hit in ohlc_hits if hit[0] == slug]) == 1
                                 else [OHLC_REFRESH_MS, 101, 111, 91, 106])
                        encoded = json.dumps([point], separators=(",", ":")).encode()
                        self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(encoded)))
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.wfile.write(encoded)
                    self.close_connection = True
                    return
                upstream_hits.append((parsed.path, values))
                time.sleep(.2)
                full_year = (parsed.path.endswith("/bitcoin/market_chart/range")
                             and int(values.get("to", ["0"])[0]) -
                             int(values.get("from", ["0"])[0]) == 365 * 86400)
                first_ms = (int(values["from"][0]) if full_year else FIRST) * 1000
                width = 86400000 if full_year else 3600000
                count = 365 if full_year else 6
                body = {"prices": [[first_ms + i * width, 100 + i] for i in range(count)],
                        "total_volumes": [[first_ms + i * width, .5 + i] for i in range(count)]}
                encoded = json.dumps(body, separators=(",", ":")).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(encoded)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(encoded)
                self.close_connection = True
            def log_message(self, *_args): pass
        upstream = ThreadingHTTPServer(("127.0.0.1", free_port()), HistoryHandler)
        upstream_port = upstream.server_address[1]
        tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls_context.load_cert_chain(certfile=str(cert), keyfile=str(key))
        upstream.socket = tls_context.wrap_socket(upstream.socket, server_side=True)
        upstream_thread = threading.Thread(target=upstream.serve_forever, daemon=True)
        upstream_thread.start()
        history_env = {**FIXTURE_PROVIDER, "FORTIS_TEST_HISTORY_UPSTREAM_PORT": str(upstream_port),
                       "FORTIS_TEST_HISTORY_UPSTREAM_CA": str(ca)}
        process, port = start(binary, root / "disabled", enabled=False)
        try:
            assert request(port, "GET", hourly, auth=False)[0] == 404
        finally:
            stop(process)

        # Orderly shutdown must cancel/drain an OHLC TLS job before unregistering
        # its coordinator lease. The response socket is allowed to disappear.
        shutdown_env = {**history_env, "FORTIS_TEST_OHLC_NOW_MS": str(OHLC_NOW_MS)}
        process, port = start(binary, root / "ohlc-shutdown", history_env=shutdown_env)
        with ThreadPoolExecutor(max_workers=1) as pool:
            pending = pool.submit(request, port, "GET",
                "/v1/prices/ohlc?id=shutdown-coin&days=1")
            deadline = time.monotonic() + 3
            while not any(hit[0] == "shutdown-coin" for hit in ohlc_hits) and time.monotonic() < deadline:
                time.sleep(.01)
            assert any(hit[0] == "shutdown-coin" for hit in ohlc_hits), "OHLC request did not reach TLS"
            stop(process)
            try:
                pending.result(timeout=2)
            except OSError:
                pass
        assert process.returncode == 0, "OHLC provider owner did not shut down cleanly"

        # Each representation starts with a clean KV so both frozen cases are
        # true cold misses (the first response writes the window's coverage).
        for name, accept in (("coldJson", "application/json"),
                             ("coldProtobuf", "application/protobuf")):
            process, port = start(binary, root / name, history_env=history_env)
            try:
                url = f"/v1/prices/history?id=bitcoin&from={FIRST}&to={FIRST + 6 * 3600}&interval=1h"
                status, headers, body = request(port, "GET", url, accept=accept)
                expected = cases[name]
                assert status == expected["status"], (name, status)
                for key, value in expected["headers"].items():
                    if key not in ("content-length",):
                        assert headers.get(key) == value, (name, key, headers.get(key), value)
                if "wireHex" in expected:
                    assert body.hex() == expected["wireHex"], name
                else:
                    actual = json.loads(body)
                    assert actual == expected["body"], (name, actual)
                assert len(upstream_hits) == (1 if name == "coldJson" else 2)
            finally:
                stop(process)

        process, port = start(binary, root / "cancelled-waiter", history_env=history_env)
        try:
            duplicate = f"/v1/prices/history?id=bitcoin&from={FIRST}&to={FIRST + 6 * 3600}&interval=1h"
            before_disconnect = len(upstream_hits)
            request(port, "GET", duplicate, disconnect=True)
            status, _, body = request(port, "GET", duplicate)
            assert status == 200 and len(json.loads(body)["points"]) == 6, (status, body)
            # Depending on when the first socket closes, the second caller can
            # join its live flight or start a new one after it was cancelled.
            # Neither timing may strand the second caller on a cancelled job.
            assert 1 <= len(upstream_hits) - before_disconnect <= 2, upstream_hits
        finally:
            stop(process)

        process, port = start(binary, root / "shutdown", history_env=history_env)
        route_errors = []
        shutdown_url = f"/v1/prices/history?id=shutdowncoin&from={FIRST}&to={FIRST + 6 * 3600}&interval=1h"
        def pending_request():
            try:
                request(port, "GET", shutdown_url)
            except (OSError, IndexError, ValueError) as error:
                route_errors.append(error)
        route_thread = threading.Thread(target=pending_request, daemon=True)
        before_shutdown = len(upstream_hits)
        route_thread.start()
        try:
            deadline = time.monotonic() + 3
            while len(upstream_hits) <= before_shutdown and time.monotonic() < deadline:
                time.sleep(.02)
            assert len(upstream_hits) > before_shutdown, "shutdown test never reached provider"
        finally:
            stop(process)
            route_thread.join(timeout=3)
            assert not route_thread.is_alive(), "pending route request leaked after shutdown"

        process, port = start(binary, root / "enabled", history_env=history_env)
        try:
            assert request(port, "GET", hourly, auth=False)[0] == 401
            assert request(port, "GET", hourly, origin="https://example.com")[0] == 403
            requests = [
                ("hourlyJson", "GET", hourly, "application/json"),
                ("hourlyProtobuf", "GET", hourly, "application/protobuf"),
                ("dailyJson", "GET", daily, "application/json"),
                ("dailyProtobuf", "GET", daily, "application/protobuf"),
                ("headJson", "HEAD", hourly, "application/json"),
                ("headProtobuf", "HEAD", hourly, "application/protobuf"),
                ("queryError", "GET", query_error, "application/json"),
                ("rangeError", "GET", range_error, "application/json"),
                ("unknownId", "GET", unknown, "application/json"),
            ]
            for name, method, url, accept in requests:
                expected = cases[name]
                # compat-decisions.md#zod-issues: no zod issue list.
                _, mapped_headers, mapped_body = compat_expectations.zod_expected(
                    expected["status"], expected["headers"], expected.get("body"))
                if mapped_headers is not expected["headers"]:
                    expected = dict(expected, headers=mapped_headers, body=mapped_body)
                status, headers, body = request(port, method, url, accept)
                assert status == expected["status"], (name, status)
                for key, value in expected["headers"].items():
                    assert headers.get(key) == value, (name, key, headers.get(key), value)
                if method == "HEAD":
                    assert body == b"" and expected["body"] is None
                elif "wireHex" in expected:
                    assert body.hex() == expected["wireHex"], name
                else:
                    assert json.loads(body) == expected["body"], name

            bad_to = f"/v1/prices/history?id=ethereum&from={FIRST}&to=nope&interval=1h"
            status, _, body = request(port, "GET", bad_to)
            assert status == 400 and json.loads(body) == {"error": "BAD_REQUEST"}  # no zod issues

            missing = f"/v1/prices/history?id=ethereum&from={FIRST - 20 * 86400}&to={FIRST - 14 * 86400}&interval=1h"
            status, headers, body = request(port, "GET", missing)
            assert status == 200 and json.loads(body)["points"] == []
            assert headers["content-type"] == "application/json; charset=utf-8"
            # A non-CoinGecko miss is the empty cached series, with
            # no provider call; the frozen other-ID replay below checks bytes.
            non_coingecko = f"/v1/prices/history?id=spl:{'A' * 32}&from={FIRST}&to={FIRST + 3600}&interval=1h"
            before_non_coingecko = len(upstream_hits)
            status, _, body = request(port, "GET", non_coingecko)
            history = json.loads(body)
            assert status == 200 and history["points"] == [], (status, body)
            assert history["cache"] == {"hit": True, "timestamp": None}, history
            assert len(upstream_hits) == before_non_coingecko
            year_from = FIRST - 365 * 86400
            year_daily = f"/v1/prices/history?id=bitcoin&from={year_from}&to={FIRST}&interval=1d"
            before_year = len(upstream_hits)
            status, _, body = request(port, "GET", year_daily)
            assert status == 200, (status, body[:200])
            year_points = json.loads(body)["points"]
            assert len(year_points) == 365 and year_points[0] == [year_from, 100]
            assert year_points[-1] == [year_from + 364 * 86400, 464]
            assert len(upstream_hits) == before_year + 1
            # Daily windows beyond 365 days are fetched. The cached
            # year leaves one missing bucket, so only that gap is requested; the
            # stub answers six hourly samples from FIRST, which fill FIRST's daily
            # bucket (the window's inclusive `to`) but not the missing day.
            beyond_from = FIRST - 366 * 86400
            beyond_year = f"/v1/prices/history?id=bitcoin&from={beyond_from}&to={FIRST}&interval=1d"
            before_beyond = len(upstream_hits)
            status, _, body = request(port, "GET", beyond_year)
            assert status == 200, (status, body[:200])
            assert len(upstream_hits) == before_beyond + 1
            gap_path, gap_query = upstream_hits[-1]
            assert gap_path.endswith("/bitcoin/market_chart/range")
            assert gap_query["from"] == [str(beyond_from)]
            assert gap_query["to"] == [str(beyond_from + 86400)], gap_query
            beyond = json.loads(body)
            assert len(beyond["points"]) == 366, len(beyond["points"])
            assert beyond["points"][0] == [year_from, 100]
            assert beyond["points"][-1] == [FIRST, 105]
            assert beyond["cache"]["hit"] is False
            assert beyond["coverage"] == {"cachedPoints": 365, "fetchedPoints": 1}
            # The answered gap is covered for six hours: no refetch.
            status, _, body = request(port, "GET", beyond_year)
            assert status == 200 and len(json.loads(body)["points"]) == 366
            assert len(upstream_hits) == before_beyond + 1
            # The largest JS-safe seconds still fit signed native milliseconds.
            huge_from = 9007199254737391
            huge_to = 9007199254740991
            huge = (f"/v1/prices/history?id=ethereum&from={huge_from}"
                    f"&to={huge_to}&interval=1h")
            status, _, body = request(port, "GET", huge)
            assert status == 200 and json.loads(body)["points"] == []
            unsafe = (f"/v1/prices/history?id=ethereum&from=9007199254740992"
                      f"&to={huge_to}&interval=1h")
            status, _, body = request(port, "GET", unsafe)
            assert status == 400 and json.loads(body) == {"error": "BAD_REQUEST"}  # no zod issues
            assert request(port, "GET", hourly)[0] == 200
            upper_id = hourly.replace("id=ethereum", "id=Ethereum")
            status, _, body = request(port, "GET", upper_id)
            assert status == 200 and json.loads(body)["id"] == "ethereum"
            inclusive_to = base + f"&to={FIRST + 5 * 3600}&interval=1h"
            status, _, body = request(port, "GET", inclusive_to)
            assert status == 200 and len(json.loads(body)["points"]) == 6
        finally:
            stop(process)

        other_ids = replay_other_ids(binary, root, history_env,
                                     lambda: len(upstream_hits) + len(ohlc_hits))

        # OHLC uses a fresh authenticated store so quota headers match the
        # independently frozen TypeScript request sequence exactly.
        process, port = start(binary, root / "ohlc-disabled", enabled=False)
        try:
            assert request(port, "GET", "/v1/prices/ohlc?id=ethereum&days=365")[0] == 404
        finally:
            stop(process)

        ohlc_env = {**history_env, "FORTIS_TEST_OHLC_NOW_MS": str(OHLC_NOW_MS),
                    "FORTIS_TEST_OHLC_SEED": "1"}
        process, port = start(binary, root / "ohlc-enabled", history_env=ohlc_env)
        try:
            ohlc_cases = ohlc_fixture["cases"]
            assert request(port, "GET", "/v1/prices/ohlc?id=spl%3A" + "A" * 32 + "&days=1",
                           auth=False)[0] == 401
            assert request(port, "GET", "/v1/prices/ohlc?id=spl%3A" + "A" * 32 + "&days=1",
                           origin="https://example.com")[0] == 403
            urls = {
                "days1": ("GET", f"/v1/prices/ohlc?id=spl%3A{'A' * 32}&days=1", "application/json"),
                "days90": ("GET", f"/v1/prices/ohlc?id=spl%3A{'A' * 32}&days=90", "application/json"),
                "days180": ("GET", f"/v1/prices/ohlc?id=spl%3A{'A' * 32}&days=180", "application/json"),
                "days365": ("GET", f"/v1/prices/ohlc?id=spl%3A{'A' * 32}&days=365", "application/json"),
                "days365Protobuf": ("GET", f"/v1/prices/ohlc?id=spl%3A{'A' * 32}&days=365", "application/protobuf"),
                "empty": ("GET", "/v1/prices/ohlc?id=asa%3A12345&days=7", "application/json"),
                "head": ("HEAD", f"/v1/prices/ohlc?id=spl%3A{'A' * 32}&days=1", "application/json"),
            }
            provider_before = len(upstream_hits)
            for name, (method, url, accept) in urls.items():
                expected = ohlc_cases[name]
                status, headers, body = request(port, method, url, accept)
                assert status == expected["status"], (name, status)
                for key, value in expected["headers"].items():
                    assert headers.get(key) == value, (name, key, headers.get(key), value)
                if method == "HEAD":
                    assert body == b"" and expected["body"] is None
                elif "wireHex" in expected:
                    assert body.hex() == expected["wireHex"], name
                else:
                    assert json.loads(body) == expected["body"], name

            # Preserve the two slug request quota charges in the frozen route
            # sequence; the provider path should be cold once, then cached.
            for _case in ("slugLive", "slugCached"):
                status, _, body = request(port, "GET",
                    "/v1/prices/ohlc?id=ethereum&days=365")
                assert status == 200 and json.loads(body)["points"]

            uppercase = ohlc_cases["uppercaseSlugStore"]
            status, headers, body = request(port, "GET",
                "/v1/prices/ohlc?id=Binancecoin&days=1")
            assert status == uppercase["status"]
            for key, value in uppercase["headers"].items():
                assert headers.get(key) == value, ("uppercaseSlugStore", key, headers.get(key), value)
            assert json.loads(body) == uppercase["body"]

            urls = {
                "invalidId": ("GET", "/v1/prices/ohlc?id=bad%20id&days=1", "application/json"),
                "invalidDays": ("GET", f"/v1/prices/ohlc?id=spl%3A{'A' * 32}&days=2", "application/json"),
                "missingDays": ("GET", "/v1/prices/ohlc?id=eth-mainnet%3Aerc20%3A0x" + "1" * 40, "application/json"),
                "invalidFiat": ("GET", f"/v1/prices/ohlc?id=spl%3A{'A' * 32}&days=1&fiat=eur", "application/json"),
            }
            for name, (method, url, accept) in urls.items():
                expected = ohlc_cases[name]
                status, headers, body = request(port, method, url, accept)
                assert status == expected["status"], (name, status)
                for key, value in expected["headers"].items():
                    assert headers.get(key) == value, (name, key, headers.get(key), value)
                if method == "HEAD":
                    assert body == b"" and expected["body"] is None
                elif "wireHex" in expected:
                    assert body.hex() == expected["wireHex"], name
                else:
                    assert json.loads(body) == expected["body"], name

            assert len(upstream_hits) == provider_before, "OHLC read unexpectedly fetched history"
        finally:
            stop(process)

        # Simple CoinGecko slugs use the deterministic provider fixture. Cold
        # and fresh hits share a durable envelope; a process restart past the
        # five-minute freshness boundary serves stale while one revalidation
        # runs on the same shared CoinGecko coordinator lane.
        swr_fixture = json.loads((FIXTURE.parent / "price_ohlc_swr_route.json").read_text())
        swr_cases = swr_fixture["cases"]
        swr_root = root / "ohlc-swr"
        now_env = {**history_env, "FORTIS_TEST_OHLC_NOW_MS": str(OHLC_NOW_MS)}
        process, port = start(binary, swr_root, history_env=now_env)
        try:
            assert request(port, "GET", "/v1/prices/ohlc?id=revalidate-coin&days=1",
                           auth=False)[0] == 401
            assert request(port, "GET", "/v1/prices/ohlc?id=revalidate-coin&days=1",
                           origin="https://example.com")[0] == 403
            with ThreadPoolExecutor(max_workers=2) as pool:
                cold = list(pool.map(lambda _: request(port, "GET",
                    "/v1/prices/ohlc?id=revalidate-coin&days=1"), range(2)))
            for status, _, body in cold:
                assert status == swr_cases["cold"]["status"]
                assert json.loads(body) == swr_cases["cold"]["body"]
            case_name = "cached"
            status, _, body = request(port, "GET",
                "/v1/prices/ohlc?id=revalidate-coin&days=1")
            assert status == swr_cases[case_name]["status"]
            assert json.loads(body) == swr_cases[case_name]["body"]
            assert request(port, "GET", "/v1/prices/ohlc?id=revalidate-coin&days=1",
                           accept="application/protobuf")[0] == 200
            head_status, head_headers, head_body = request(port, "HEAD",
                "/v1/prices/ohlc?id=revalidate-coin&days=1")
            assert head_status == 200 and head_body == b"" and int(head_headers["content-length"]) > 0
            assert [hit[0] for hit in ohlc_hits if hit[0] == "revalidate-coin"] == [
                "revalidate-coin"]
        finally:
            stop(process)
        stale_env = {**history_env,
                     "FORTIS_TEST_OHLC_NOW_MS": str(OHLC_NOW_MS + 600_000)}
        process, port = start(binary, swr_root, history_env=stale_env)
        try:
            status, _, body = request(port, "GET",
                "/v1/prices/ohlc?id=revalidate-coin&days=1")
            assert status == swr_cases["stale"]["status"]
            assert json.loads(body) == swr_cases["stale"]["body"]
            time.sleep(.3)
            if process.poll() is not None:
                raise RuntimeError(f"OHLC server exited ({process.returncode}): " +
                                   process.stderr.read().decode())
            status, _, body = request(port, "GET",
                "/v1/prices/ohlc?id=revalidate-coin&days=1")
            assert status == swr_cases["revalidated"]["status"]
            assert json.loads(body) == swr_cases["revalidated"]["body"]
            before_error = len([hit for hit in ohlc_hits if hit[0] == "failing-coin"])
            status, _, body = request(port, "GET",
                "/v1/prices/ohlc?id=failing-coin&days=1")
            assert status == swr_cases["providerError"]["status"]
            assert json.loads(body) == swr_cases["providerError"]["body"]
            assert len([hit for hit in ohlc_hits if hit[0] == "failing-coin"]) - before_error == 3
            assert all(hit[1].get("vs_currency") == ["usd"] for hit in ohlc_hits)
            assert all(hit[1].get("days") == ["1"] for hit in ohlc_hits
                       if hit[0] in ("revalidate-coin", "failing-coin"))
        finally:
            stop(process)

        # Hard process restart must retain the non-slug cached series. Do not
        # seed the OHLC keys on this second boot.
        restart_env = {**history_env, "FORTIS_TEST_OHLC_NOW_MS": str(OHLC_NOW_MS)}
        process, port = start(binary, root / "ohlc-enabled", history_env=restart_env)
        try:
            status, _, body = request(port, "GET",
                f"/v1/prices/ohlc?id=spl%3A{'A' * 32}&days=365")
            assert status == 200 and json.loads(body) == ohlc_cases["days365"]["body"]
            assert len(upstream_hits) == provider_before
        finally:
            stop(process)
        upstream.shutdown()
        upstream.server_close()
    print("native price history/OHLC gated routes, cached/cold JSON/protobuf, restart, HEAD and "
          "validation passed; %d frozen non-CoinGecko history cases replayed" % other_ids)

if __name__ == "__main__":
    main(sys.argv[1])
