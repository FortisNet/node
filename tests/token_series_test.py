#!/usr/bin/env python3
"""Replay the frozen TypeScript token page series contract
(tests/corpus/token_series.json, written by
the earlier TypeScript implementation's tests) against
build/fortis_node_token_route_test, then run native-only shutdown and
resource checks.

The fake upstream serves the provider replies recorded in each case (the
case's own calls first, then any recorded call with the same request), so
the native backfill owner receives the TypeScript bytes. `seed` steps write
the same candles through /probe/test-token-series-seed. Groups run in
separate processes over one KV directory, so the `gap` group reads the
candles and coverage the `cold` group stored across a restart. Every process
shares the test clock with the history owner (FORTIS_TEST_HISTORY_NOW_FILE)
and fails store reads for the fixture's failSource
(FORTIS_TEST_HISTORY_FAIL_SOURCE), as the TypeScript spy does.

Each request compares status, the fixture header set, the exact JSON body
bytes or protobuf wire bytes, and the provider request multiset.

Recorded native difference (asserted, not ignored):
  - concurrent single-flight: the TypeScript backfill lock makes the second
    request a waiter whose cache.series.hit is true; the native owner shares
    one flight and both requests report the fill (hit false, the winner's
    exact bytes). The loser's TypeScript body must differ from the winner's
    only in that flag.

Native-only checks (no TypeScript counterpart):
  - shutdown while a gap is pending: the page answers series null with
    errors.series UPSTREAM_UNAVAILABLE, the provider wait is cancelled and
    the process exits before the slow provider replies;
  - a 365-day daily series: cold and cached latency, provider calls, a
    23-request burst against the 16-slot token queue, RSS, and the same
    window served from the durable KV after SIGKILL with no provider call.
Usage: token_series_test.py <binary>
"""
import json
import os
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import quote, urlsplit

import discovery_route_harness as harness
import tls_ca
import token_routes_test as routes

FIXTURE = "token_series.json"
GROUPS = [["absent"], ["cold"], ["gap"], ["outage"], ["failure"], ["concurrent"]]


def request_key(method, host, target, body):
    if method == "POST":
        return (method, host, target or "/", json.dumps(json.loads(body or b"{}"), sort_keys=True))
    return (method, host, target, "")


def recorded_key(call):
    url = urlsplit(call["url"])
    target = url.path + (f"?{url.query}" if url.query else "")
    body = call.get("requestBody")
    return request_key(call["method"], url.netloc, target, body.encode() if body else None)


class Recorded:
    """Serves the recorded TypeScript replies: the running case's own calls
    in order first, then the first recorded reply to the same request."""

    def __init__(self, cases):
        self.fallback = {}
        for case in cases:
            for call in case.get("upstream", []):
                self.fallback.setdefault(recorded_key(call), (call["status"], call["body"]))
        self.current = {}
        self.delay = 0.0

    def use(self, case):
        self.current = {}
        for call in case.get("upstream", []):
            self.current.setdefault(recorded_key(call), []).append((call["status"], call["body"]))

    def __call__(self, method, host, target, body):
        if self.delay:
            time.sleep(self.delay)
        key = request_key(method, host, target, body)
        queue = self.current.get(key)
        if queue:
            status, payload = queue.pop(0) if len(queue) > 1 else queue[0]
        elif key in self.fallback:
            status, payload = self.fallback[key]
        else:
            status, payload = 404, '{"error":"unexpected request"}'
        return status, payload.encode()


class Upstream(routes.Upstream):
    """routes.Upstream without holding its lock while a reply is delayed."""

    def __init__(self, root, rules):
        self.rules = rules
        self.observed = []
        self.lock = threading.Lock()
        ca, cert, key, _ = tls_ca.create_certificates(Path(root), "DNS:localhost")
        self.ca = ca
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def handle_any(self, method):
                length = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(length) if length else b""
                host = self.headers.get("x-fortis-test-host") or "api.coingecko.com"
                with upstream.lock:
                    upstream.observed.append((method, host, self.path, body))
                status, payload = upstream.rules(method, host, self.path, body)
                try:
                    self.send_response(status)
                    if status < 300:
                        self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(payload)))
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.wfile.write(payload)
                    self.wfile.flush()
                except OSError:
                    pass
                self.close_connection = True

            def do_GET(self):
                self.handle_any("GET")

            def do_POST(self):
                self.handle_any("POST")

            def log_message(self, *_args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", harness.free_port()), Handler)
        self.server.daemon_threads = True
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        self.server.socket = tls.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_address[1]


def seed(server, case, failures):
    spec = case["seed"]
    rows = ";".join(
        ",".join([str(c["ts"]), c["open"], c["high"], c["low"], c["close"],
                  "n" if c["volume"] is None else c["volume"]])
        for c in spec["candles"])
    path = (f"/probe/test-token-series-seed?interval={spec['interval']}"
            f"&source={quote(spec['sourceId'], safe='')}&rows={quote(rows, safe='')}")
    status, _, body = server.request("GET", path)
    if status != 200:
        failures.append(f"{case['name']}: seed {status} {body[:200]!r}")


def compare_response(name, expected, status, response_headers, payload, failures, exact=True):
    def fail(message):
        failures.append(f"{name}: {message}")

    if status != expected["status"]:
        fail(f"status {status} != {expected['status']} ({payload[:400]!r})")
        return
    for header in routes.HEADERS:
        if not exact and header in ("etag", "content-length"):
            continue
        if response_headers.get(header) != expected["headers"].get(header):
            fail(f"header {header}: {response_headers.get(header)!r} != {expected['headers'].get(header)!r}")
    if "wireHex" in expected:
        if payload.hex() != expected["wireHex"]:
            fail(f"protobuf wire\n  expected {expected['wireHex'][:600]}\n  actual   {payload.hex()[:600]}")
    elif "bodyText" in expected and exact:
        if payload != expected["bodyText"].encode():
            text, actual = expected["bodyText"], payload.decode("utf-8", "replace")
            index = next((i for i, (x, y) in enumerate(zip(text, actual)) if x != y), min(len(text), len(actual)))
            fail(f"body differs at {index}\n  expected …{text[max(0, index - 200):index + 200]}\n"
                 f"  actual   …{actual[max(0, index - 200):index + 200]}")
    elif expected["status"] != 304:
        if routes.json_or_none(payload) != expected["body"]:
            fail(f"body {payload[:600]!r} != {json.dumps(expected['body'])[:600]}")


def run_case(server, upstream, rules, clock_file, tokens, case, failures, lane):
    name = case["name"]
    harness.set_clock(clock_file, case["at"])
    if "seed" in case:
        seed(server, case, failures)
        return
    lane.reserve(case)
    rules.use(case)
    before = len(upstream.calls())
    expected_calls = routes.expected_calls(case)

    def send(entry, out, index):
        headers = {"Authorization": "Bearer " + tokens.next(), "Accept": entry["accept"]}
        if entry.get("ifNoneMatch"):
            headers["If-None-Match"] = entry["ifNoneMatch"]
        out[index] = server.request("GET", entry["url"], headers=headers)

    if "concurrent" in case:
        entries = case["concurrent"]
        results = [None] * len(entries)
        threads = [threading.Thread(target=send, args=(entry, results, i)) for i, entry in enumerate(entries)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
    else:
        entries = [case]
        results = [None]
        send(case, results, 0)
    harness.wait_for_calls(upstream, before + len(case["upstream"]), timeout=10.0)
    server.request("GET", "/probe/test-token-idle")
    time.sleep(.05)
    observed = routes.observed_calls(upstream.calls()[before:])

    if "concurrent" in case:
        winner, loser = entries
        # The TypeScript loser differs from the winner only in cache.series.hit.
        loser_body = json.loads(json.dumps(loser["body"]))
        if loser_body["cache"]["series"]["hit"] is not True or winner["body"]["cache"]["series"]["hit"] is not False:
            failures.append(f"{name}: fixture no longer shows the TypeScript lock-loser shape")
        loser_body["cache"]["series"]["hit"] = False
        if loser_body != winner["body"]:
            failures.append(f"{name}: TypeScript loser differs from the winner beyond cache.series.hit")
        for index, (status, response_headers, payload) in enumerate(results):
            compare_response(f"{name}[{index}]", winner, status, response_headers, payload, failures)
    else:
        status, response_headers, payload = results[0]
        compare_response(name, case, status, response_headers, payload, failures)
    if observed != expected_calls:
        failures.append(f"{name}: upstream\n  expected {expected_calls}\n  actual   {observed}")


def start_server(binary, kv_dir, clock_file, upstream, now_ms, fail_source):
    os.environ["FORTIS_TEST_HISTORY_NOW_FILE"] = str(clock_file)
    os.environ["FORTIS_TEST_HISTORY_FAIL_SOURCE"] = fail_source
    return routes.Server(binary, kv_dir, clock_file, upstream, now_ms)


def replay(binary, fixture, root, failures):
    cases = fixture["cases"]
    rules = Recorded(cases)
    upstream = Upstream(root, rules)
    kv_dir = Path(root) / "kv"
    kv_dir.mkdir()
    clock_file = Path(root) / "clock"
    tokens = harness.TokenRotation(per_token=40)
    ran = 0
    try:
        for groups in GROUPS:
            selected = [c for c in cases if c["group"] in groups]
            if not selected:
                continue
            harness.set_clock(clock_file, selected[0]["at"])
            server = start_server(binary, kv_dir, clock_file, upstream, selected[0]["at"],
                                  fixture["source"]["failSource"])
            lane = routes.CoinGeckoLane()
            try:
                for case in selected:
                    run_case(server, upstream, rules, clock_file, tokens, case, failures, lane)
                    ran += 1
            finally:
                server.stop()
    finally:
        upstream.close()
    return ran


def rss_kib(pid):
    out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    return int(out) if out else -1


def native_checks(binary, fixture, root, failures):
    """Shutdown with a pending gap, then the 365-day daily measurement."""
    cases = {c["name"]: c for c in fixture["cases"] if "seed" not in c}
    rules = Recorded(fixture["cases"])
    upstream = Upstream(root, rules)
    clock_file = Path(root) / "native-clock"
    tokens = harness.TokenRotation(per_token=40)
    report = {}
    try:
        # ── shutdown: the provider answers after 6 s; SIGTERM after 0.5 s ──
        kv_dir = Path(root) / "kv-shutdown"
        kv_dir.mkdir()
        cold = cases["bitcoinCold"]
        harness.set_clock(clock_file, cold["at"])
        server = start_server(binary, kv_dir, clock_file, upstream, cold["at"], "none")
        rules.use(cold)
        rules.delay = 6.0
        result = {}

        def page():
            try:
                result["response"] = server.request(
                    "GET", cold["url"], headers={"Authorization": "Bearer " + tokens.next(),
                                                 "Accept": "application/json"}, timeout=20)
            except OSError as error:
                result["error"] = repr(error)

        thread = threading.Thread(target=page)
        started = time.monotonic()
        thread.start()
        harness.wait_for_calls(upstream, 1, timeout=5.0)
        time.sleep(.5)
        server.process.send_signal(signal.SIGTERM)
        thread.join(timeout=20)
        try:
            server.process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            server.process.kill()
            server.process.wait()
            failures.append("shutdown: process did not exit within 20 s")
        elapsed = time.monotonic() - started
        rules.delay = 0.0
        report["shutdown_seconds"] = round(elapsed, 3)
        response = result.get("response")
        if not response:
            failures.append(f"shutdown: no response ({result.get('error')})")
        else:
            status, _, payload = response
            body = routes.json_or_none(payload) or {}
            if status != 200 or body.get("series") is not None or \
                    body.get("errors", {}).get("series") != "UPSTREAM_UNAVAILABLE":
                failures.append(f"shutdown: unexpected answer {status} {payload[:300]!r}")
        if elapsed >= 5.5:
            failures.append(f"shutdown: took {elapsed:.2f} s, the provider wait was not cancelled")

        # ── a 365-day daily series ──
        kv_dir = Path(root) / "kv-measure"
        kv_dir.mkdir()
        year = cases["bitcoinYear"]
        harness.set_clock(clock_file, cold["at"])
        server = start_server(binary, kv_dir, clock_file, upstream, cold["at"], "none")
        try:
            rss_boot = rss_kib(server.process.pid)
            # The fixture's sequence: 30 cached days, then the 365-day page
            # whose older 335 days are one provider gap.
            rules.use(cold)
            server.request("GET", cold["url"], headers={"Authorization": "Bearer " + tokens.next(),
                                                        "Accept": "application/json"})
            time.sleep(2.6)  # the native CoinGecko lane refills 0.4 tokens/s
            url = year["url"]
            harness.set_clock(clock_file, year["at"])
            rules.use(year)
            before = len(upstream.calls())
            t0 = time.monotonic()
            status, _, payload = server.request("GET", url, headers={"Authorization": "Bearer " + tokens.next(),
                                                                     "Accept": "application/json"})
            cold_ms = (time.monotonic() - t0) * 1000
            cold_calls = len(upstream.calls()) - before
            body = routes.json_or_none(payload) or {}
            points = len((body.get("series") or {}).get("points") or [])
            if status != 200 or points == 0 or body.get("cache", {}).get("series", {}).get("hit") is not False:
                failures.append(f"measure: cold 365-day answer {status} points={points} {payload[:200]!r}")
            rss_cold = rss_kib(server.process.pid)
            warm = []
            for _ in range(20):
                t0 = time.monotonic()
                status, _, payload = server.request("GET", url, headers={
                    "Authorization": "Bearer " + tokens.next(), "Accept": "application/json"})
                warm.append((time.monotonic() - t0) * 1000)
                if status != 200:
                    failures.append(f"measure: warm status {status}")
                    break
            warm_calls = len(upstream.calls()) - before - cold_calls
            # Queue pressure: 23 simultaneous cached reads against 2 workers
            # and 16 queue slots, one seeded device each (no device quota).
            # A full queue answers the token service's 429 RATE_LIMITED.
            letters = "CDEFGIJKLMNOPQRSTUVWXYZ"  # seeded by the test main; A and B served above
            burst = [None] * len(letters)

            def one(index):
                t = time.monotonic()
                try:
                    status, _, _ = server.request("GET", url, headers={
                        "Authorization": "Bearer fintel_dev_" + letters[index] * 43, "Accept": "application/json"})
                except OSError:
                    status = "reset"
                burst[index] = (status, (time.monotonic() - t) * 1000)

            threads = [threading.Thread(target=one, args=(i,)) for i in range(len(burst))]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            rss_after = rss_kib(server.process.pid)
            statuses = {}
            for status, _ in burst:
                statuses[status] = statuses.get(status, 0) + 1
            served = sorted(ms for status, ms in burst if status == 200)
            report.update({
                "points": points,
                "body_bytes": len(payload),
                "cold_ms": round(cold_ms, 1),
                "cold_provider_calls": cold_calls,
                "warm_ms_median": round(sorted(warm)[len(warm) // 2], 1),
                "warm_ms_max": round(max(warm), 1),
                "warm_provider_calls": warm_calls,
                "burst_statuses": statuses,
                "burst_ms_max": round(served[-1], 1) if served else None,
                "rss_kib": {"boot": rss_boot, "after_cold": rss_cold, "after_burst": rss_after},
            })
            if cold_calls != 1 or warm_calls != 0:
                failures.append(f"measure: provider calls cold={cold_calls} warm={warm_calls}")
            if statuses.get(200, 0) < 16 or set(statuses) - {200, 429}:
                failures.append(f"measure: burst answered {statuses}")
            # Hard restart: SIGKILL, then the same window from the durable KV.
            server.process.kill()
            server.process.wait()
            harness.set_clock(clock_file, year["at"] + 1000)
            server = start_server(binary, kv_dir, clock_file, upstream, year["at"], "none")
            before = len(upstream.calls())
            status, _, payload = server.request("GET", url, headers={
                "Authorization": "Bearer fintel_dev_" + "B" * 43, "Accept": "application/json"})
            body = routes.json_or_none(payload) or {}
            restarted = len((body.get("series") or {}).get("points") or [])
            report["after_sigkill"] = {"points": restarted, "hit": body.get("cache", {}).get("series", {}).get("hit"),
                                       "provider_calls": len(upstream.calls()) - before}
            if status != 200 or restarted != points or report["after_sigkill"]["hit"] is not True or \
                    report["after_sigkill"]["provider_calls"]:
                failures.append(f"measure: after SIGKILL {status} {report['after_sigkill']}")
        finally:
            server.stop()
    finally:
        upstream.close()
    return report


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = harness.load_fixture(FIXTURE)
    failures = []
    with tempfile.TemporaryDirectory(prefix="fortis-token-series-") as root:
        ran = replay(binary, fixture, root, failures)
        report = {} if os.environ.get("TOKEN_SERIES_REPLAY_ONLY") else native_checks(binary, fixture, root, failures)
    if failures:
        print(f"token series replay: {len(failures)} failure(s) over {ran} cases")
        for failure in failures:
            print(" -", failure)
        raise SystemExit(1)
    print(f"token series replay: {ran} cases match the TypeScript contract")
    if report:
        print("token series native checks:", json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
