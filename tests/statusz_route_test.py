#!/usr/bin/env python3
"""GET/HEAD /statusz route contract (src/operations/operations_routes.cpp handle_statusz).

Real sockets against the operations test binary, whose telemetry samples and
/statusz reads share one movable clock (FORTIS_TEST_OPERATIONS_NOW_FILE), plus
the production binary for the wall-clock path. Covers the cold document
against the captured core oracle plus typed native extensions, headers, HEAD, root-scope posture
(query, Origin, Authorization ignored), 404 neighbours, the per-second memo
and its revision invalidation, 5/10/15-minute boundaries, template-only
labels, malformed requests, and concurrent readers and writers.
"""

import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
from datetime import datetime
from email.utils import parsedate_to_datetime
from pathlib import Path

FIXTURES = Path(__file__).resolve().parents[1] / "tests/corpus"
TOKEN = "fintel_dev_00" + "K" * 41
TOKEN_B = "fintel_dev_01" + "K" * 41
DEVICE_PREFIX = "01ARZ3NDEKTSV4RRFFQ69G5F"
JSON_TYPE = "application/json; charset=utf-8"
SECURITY = {"x-content-type-options": "nosniff", "referrer-policy": "no-referrer",
            "permissions-policy": "camera=(), microphone=(), geolocation=()"}
ROUTE = "/v1/test-operations/status/:status"
TEMPLATE = re.compile(r"^/v1/[A-Za-z0-9_./:-]+$")
MINUTE_MS = 60_000


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def exchange(port, raw):
    with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
        s.sendall(raw)
        data = bytearray()
        while chunk := s.recv(65536):
            data.extend(chunk)
    head, _, body = bytes(data).partition(b"\r\n\r\n")
    status_line, *lines = head.decode("latin-1").split("\r\n")
    headers = {}
    for line in lines:
        name, _, value = line.partition(":")
        assert name.lower() not in headers, ("repeated header", name)
        headers[name.strip().lower()] = value.strip()
    return int(status_line.split()[1]), headers, body


def request(port, target="/statusz", method="GET", headers=None):
    raw = f"{method} {target} HTTP/1.1\r\nHost: localhost\r\n"
    for name, value in (headers or {}).items():
        raw += f"{name}: {value}\r\n"
    return exchange(port, (raw + "\r\n").encode("latin-1"))


def statusz(port, **kwargs):
    status, headers, body = request(port, **kwargs)
    assert status == 200, (status, body[:200])
    assert headers["content-length"] == str(len(body))
    return body, json.loads(body)


class Server:
    def __init__(self, binary, now_file=None):
        self.kv = tempfile.TemporaryDirectory(prefix="fortis-statusz-kv-")
        env = os.environ.copy()
        for name in ("PORT", "METRICS_TOKEN", "FORTIS_ENABLE_TEST_OPERATIONS",
                     "FORTIS_TEST_OPERATIONS_NOW_FILE"):
            env.pop(name, None)
        env["FORTIS_KV_DIR"] = self.kv.name
        if now_file is not None:
            env["FORTIS_ENABLE_TEST_OPERATIONS"] = "1"
            env["FORTIS_TEST_OPERATIONS_NOW_FILE"] = str(now_file)
        self.port = free_port()
        self.spawned = time.monotonic()
        self.process = subprocess.Popen([binary, str(self.port)], env=env,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        for _ in range(300):
            if self.process.poll() is not None:
                raise AssertionError(self.process.stderr.read().decode())
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.1):
                    return
            except OSError:
                time.sleep(0.02)
        raise AssertionError("server did not listen")

    def close(self):
        self.process.terminate()
        self.process.communicate(timeout=10)
        self.kv.cleanup()


class Clock:
    """The test binary's wall clock, replaced atomically so no read is torn."""

    def __init__(self, directory, now_ms):
        self.path = Path(directory) / "now_ms"
        self.set(now_ms)

    def set(self, now_ms):
        self.now_ms = now_ms
        partial = self.path.with_suffix(".tmp")
        partial.write_text(str(now_ms))
        os.replace(partial, self.path)


def expected_cold_stats(pages):
    stats = dict(pages["statusz"]["stats"])
    # Native omits what it does not measure; it never sends an empty stand-in.
    del stats["databaseOperations10m"]
    return json.dumps(stats, separators=(",", ":")).encode()


def check_document_bytes(body, document, pages, stats_bytes=None):
    """The exact native layout: oracle key order, then the stats block."""
    memory = document["memory"]
    rss = "null" if memory["rss"] is None else str(memory["rss"])
    cpu = document["cpu"]["percent"]
    cpu_text = "null" if cpu is None else f"{cpu:.2f}"
    disk = document["disk"]
    disk_total = "null" if disk is None or disk["total"] is None else str(disk["total"])
    disk_text = "null" if disk is None else \
        f'{{"used":{disk["used"]},"total":{disk_total}}}'
    stats = stats_bytes if stats_bytes is not None else \
        json.dumps(document["stats"], separators=(",", ":")).encode()
    expected = (f'{{"ok":true,"version":"{pages["statusz"]["versionWhenRead"]}",'
                f'"node":"native-cpp","uptimeSec":{document["uptimeSec"]},'
                f'"memory":{{"rss":{rss},"heapUsed":null}},'
                f'"cpu":{{"percent":{cpu_text}}},"disk":{disk_text}').encode()
    assert body.startswith(expected), (body[:300], expected[:300])
    assert body.endswith(b',"stats":' + stats + b"}"), "telemetry bytes differ"
    base = ['ok', 'version', 'node', 'uptimeSec', 'memory', 'cpu', 'disk']
    extensions = ['database', 'databaseActivity', 'httpTiming', 'snapshotAuth', 'resources']
    assert list(document) == base + [key for key in extensions if key in document] + ['stats']
    resources = document.get('resources')
    assert isinstance(resources, dict)
    assert resources['diskScope'] in ('filesystem', 'data-tree', 'unavailable')
    assert all(v is None or (isinstance(v, (int, float)) and v >= 0) for k, v in resources.items() if k != 'diskScope')
    database = document.get('database')
    if database is not None:
        counters = {'indexedKeys', 'fileBytes', 'liveBytes', 'compactionsCompleted',
                    'compactionsFailed', 'maxLockHoldUs', 'sampleAgeSec'}
        assert set(database) == counters | {'compactionPhase', 'recoveryPending'}
        assert all(type(database[key]) is int and database[key] >= 0 for key in counters)
        assert database['compactionPhase'] in ('idle', 'copying', 'catching up', 'remapping')
        assert type(database['recoveryPending']) is bool
    for key in ('databaseActivity', 'httpTiming'):
        activity = document.get(key)
        if activity is None:
            continue
        bounds = activity['boundsUs']
        assert bounds == sorted(set(bounds)) and all(type(value) is int and value >= 0 for value in bounds)
        for name, values in activity.items():
            if name == 'boundsUs':
                continue
            if isinstance(values, list):
                assert len(values) == len(bounds) + 1
                assert all(type(value) is int and value >= 0 for value in values)
            else:
                assert type(values) in (int, bool) and values >= 0
    auth = document['snapshotAuth']
    assert set(auth) == {'hits', 'misses', 'invalidations'}
    assert all(type(value) is int and value >= 0 for value in auth.values())


def check_memory(document):
    # Host builds: no allocator statistics, so heapUsed is null, never 0;
    # rss comes from /proc/self/statm on Linux or task_info on macOS.
    assert document["memory"]["heapUsed"] is None
    if os.path.exists("/proc/self/statm") or sys.platform == "darwin":
        assert isinstance(document["memory"]["rss"], int) and document["memory"]["rss"] > 0
    else:
        assert document["memory"]["rss"] is None


def check_production(binary, pages):
    server = Server(binary)
    try:
        docs_status, docs_headers, docs_body = request(server.port, target="/documentation")
        assert docs_status == 200 and docs_headers["content-type"] == "text/html"
        assert b"FortisNet Intelligence API" in docs_body
        assert b"/v1/prices/history" in docs_body and b"/v1/ai/conversations" in docs_body
        spec_status, spec_headers, spec_body = request(server.port, target="/openapi.yaml")
        assert spec_status == 200 and spec_headers["content-type"].startswith("application/yaml")
        assert spec_body.startswith(b"openapi: 3.0.3")
        assert b"/v1/ai/conversations" in spec_body
        status, headers, body = request(server.port)
        assert status == 200
        # The router's Server-Timing (one line; exchange refuses repeats).
        timing = headers.pop("server-timing", "")
        assert re.fullmatch(r"app;dur=\d+\.\d{3}", timing), timing
        date = headers.pop("date", "")
        assert re.fullmatch(r"[A-Z][a-z]{2}, \d{2} [A-Z][a-z]{2} \d{4} \d{2}:\d{2}:\d{2} GMT", date), date
        assert abs(parsedate_to_datetime(date).timestamp() - time.time()) < 30, date
        assert headers == {"content-type": JSON_TYPE, "content-length": str(len(body)),
                           "connection": "close", "cache-control": "no-store", **SECURITY}, headers
        document = json.loads(body)
        check_document_bytes(body, document, pages)
        check_memory(document)
        # uptimeSec counts from process start, bounded by what really elapsed.
        assert 0 <= document["uptimeSec"] <= int(time.monotonic() - server.spawned) + 1
        stats = document["stats"]
        now_minute = int(time.time()) // 60
        last = stats["perMinute"][-1]["startSec"] // 60
        assert last in (now_minute, now_minute - 1)
        assert [p["startSec"] for p in stats["perMinute"]] == \
            [(last - 14 + i) * 60 for i in range(15)]
        assert stats["window5m"] == stats["window10m"] == {"clients": 0, "requests": 0, "errors": 0}
        assert stats["topRoutes10m"] == []
        status, head_headers, head_body = request(server.port, method="HEAD")
        assert status == 200 and head_body == b""
        # Same headers as GET except length, timing and the wall-clock Date.
        head_date = head_headers.pop("date", "")
        assert re.fullmatch(r"[A-Z][a-z]{2}, \d{2} [A-Z][a-z]{2} \d{4} \d{2}:\d{2}:\d{2} GMT", head_date), head_date
        assert abs(parsedate_to_datetime(head_date).timestamp() - time.time()) < 30, head_date
        assert re.fullmatch(r"app;dur=\d+\.\d{3}", head_headers.pop("server-timing", "")), head_headers
        assert {k: v for k, v in head_headers.items() if k != "content-length"} == \
            {k: v for k, v in headers.items() if k != "content-length"}
    finally:
        server.close()


def check_cold_and_posture(port, pages, clock):
    body, document = statusz(port)
    check_document_bytes(body, document, pages, expected_cold_stats(pages))
    check_memory(document)
    status, headers, _ = request(port)
    assert headers["content-type"] == pages["statusz"]["headers"]["content-type"] == JSON_TYPE
    assert not any(name.startswith("access-control-") for name in headers)
    assert "www-authenticate" not in headers and "set-cookie" not in headers

    # Same frozen second, no API traffic: every variant replays the same
    # bytes. Query, Origin, credentials and cookies are ignored, not echoed.
    long_query = "?pad=" + "q" * 8000
    variants = [
        {"target": "/statusz?deviceId=" + DEVICE_PREFIX + "00&address=0xdeadbeefcafe"},
        {"target": "/statusz" + long_query},
        {"headers": {"Origin": "https://evil.example"}},
        {"headers": {"Authorization": "Bearer " + TOKEN, "Cookie": "session=secret-cookie"}},
        {"headers": {"Authorization": "Bearer junk", "X-Forwarded-For": "203.0.113.9"}},
    ]
    for variant in variants:
        again, _ = statusz(port, **variant)
        assert again == body, variant
        for secret in (b"deadbeef", b"evil.example", b"secret-cookie", b"fintel_dev_",
                       DEVICE_PREFIX.encode(), b"qqqq", b"203.0.113"):
            assert secret not in again

    # HEAD: GET's headers and Content-Length, no body.
    status, head_headers, head_body = request(port, method="HEAD")
    assert status == 200 and head_body == b""
    assert head_headers["content-length"] == str(len(body))
    assert head_headers["content-type"] == JSON_TYPE

    # Neighbours are the router's 404, never this document.
    for target, method in (("/statusz/", "GET"), ("/STATUSZ", "GET"), ("/%73tatusz", "GET"),
                           ("/statusz.json", "GET"), ("/statusz", "POST"), ("/statusz", "PUT"),
                           ("/statusz", "OPTIONS"), ("/statusz", "DELETE")):
        status, _, payload = request(port, target=target, method=method)
        assert status == 404 and payload == b'{"error":"NOT_FOUND"}', (target, method, status)

    # Malformed requests stop in the parser and never echo their input. The
    # server answers an oversized head before reading all of it, so the
    # close may reset the socket; whatever arrived must be the 431.
    oversized = f"GET /statusz?{'a' * 17000}SECRET HTTP/1.1\r\nHost: x\r\n\r\n".encode()
    with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
        received = bytearray()
        try:
            s.sendall(oversized)
            while chunk := s.recv(65536):
                received.extend(chunk)
        except (ConnectionResetError, BrokenPipeError):
            pass
    assert received.startswith(b"HTTP/1.1 431 ") or not received, bytes(received[:40])
    assert b"SECRET" not in received
    for raw in (b"GET /statusz HTTP/1.0\r\n\r\n", b"GET  /statusz HTTP/1.1\r\n\r\n",
                b"GET /statusz HTTP/1.1\r\nHost: a\r\nBad Header\r\n\r\n"):
        status, _, payload = exchange(port, raw)
        assert status == 400 and b"statusz" not in payload, raw

    # None of that was API traffic, and the cold document is unchanged.
    assert statusz(port)[0] == body
    return body


def check_memo(port, clock):
    base = clock.now_ms
    first, document = statusz(port)
    # The memo key is the injected wall-clock second: real time passing does
    # not rebuild it (uptimeSec is real monotonic time and stays frozen).
    time.sleep(1.2)
    assert statusz(port)[0] == first
    clock.set(base + 999)  # floor(now / 1000) unchanged
    assert statusz(port)[0] == first
    # A new second rebuilds even without traffic: uptime moves on.
    clock.set(base + 1000)
    rebuilt, later = statusz(port)
    assert later["uptimeSec"] > document["uptimeSec"], (later["uptimeSec"], document["uptimeSec"])
    assert later["stats"] == document["stats"]
    # New API traffic bumps the telemetry revision: the very next read in the
    # same second shows it, never a stale window.
    assert request(port, target="/v1/test-operations/status/200",
                   headers={"Authorization": "Bearer " + TOKEN})[0] == 200
    after, fresh = statusz(port)
    assert after != rebuilt
    assert fresh["stats"]["window5m"] == {"clients": 1, "requests": 1, "errors": 0}
    assert fresh["stats"]["topRoutes10m"][0]["route"] == ROUTE
    assert fresh["stats"]["perMinute"][-1]["requests"] == 1
    # Reading it again in that second replays the rebuilt bytes.
    assert statusz(port)[0] == after
    # Root pages, probes, /metrics and unknown paths do not move the revision.
    for target in ("/", "/statusz", "/errors", "/errorsz", "/healthz", "/metrics",
                   "/assets/background.jpg", "/v1/unknown-route", "/v2/statusz"):
        request(port, target=target)
        request(port, target=target, method="HEAD")
    assert statusz(port)[0] == after


def stats_at(port, clock, now_ms):
    clock.set(now_ms)
    stats = statusz(port)[1]["stats"]
    now_minute = now_ms // MINUTE_MS
    assert [p["startSec"] for p in stats["perMinute"]] == \
        [(now_minute - 14 + i) * 60 for i in range(15)]
    return stats


def check_windows_and_labels(port, clock):
    # Traffic at T0 (10 s into a fresh minute, far from the memo test's).
    t0_minute = clock.now_ms // MINUTE_MS + 100
    t0 = t0_minute * MINUTE_MS + 10_000
    clock.set(t0)
    hits = [
        ("/v1/test-operations/status/503?address=0xdeadbeefcafe", TOKEN, 503),
        ("/v1/test-operations/status/200", TOKEN_B, 200),
        ("/v1/test-operations/status/200", "fintel_dev_" + "Z" * 43, 401),  # no client
        ("/v1/test-operations/status/abc", None, 400),  # bad value, same template
        ("/v1/test-operations/status/599", None, 599),
    ]
    for target, token, expected in hits:
        headers = {"Authorization": "Bearer " + token} if token else {}
        assert request(port, target=target, headers=headers)[0] == expected, target
    totals = {"clients": 2, "requests": 5, "errors": 2}

    stats = stats_at(port, clock, t0)
    assert stats["window5m"] == stats["window10m"] == totals
    assert stats["perMinute"][-1] == {"startSec": t0_minute * 60, "requests": 5, "errors": 2,
                                      "clients": 2}
    top = stats["topRoutes10m"]
    assert [entry["route"] for entry in top] == [ROUTE]
    assert top[0]["requests"] == 5 and top[0]["errors"] == 2
    assert len(top[0]["durationBuckets"]) == 11 and sum(top[0]["durationBuckets"]) == 5
    assert top[0]["maxDurationMs"] >= 0 and top[0]["durationP95Ms"] >= 0
    text = json.dumps(stats)
    for secret in ("deadbeef", "fintel_dev_", DEVICE_PREFIX, "/status/503", "/status/abc",
                   "ZZZZ", "KKKK"):
        assert secret not in text, secret

    # Whole epoch minutes: T0's minute is in the 5-minute window through the
    # last millisecond of minute +4, leaves it at minute +5, and leaves the
    # 10-minute window at minute +10.
    edges = [
        (t0_minute * MINUTE_MS + 4 * MINUTE_MS + MINUTE_MS - 1, totals, totals, 10),
        ((t0_minute + 5) * MINUTE_MS, None, totals, 9),
        ((t0_minute + 10) * MINUTE_MS - 1, None, totals, 5),
        ((t0_minute + 10) * MINUTE_MS, None, None, 4),
        ((t0_minute + 15) * MINUTE_MS - 1, None, None, 0),
    ]
    zero = {"clients": 0, "requests": 0, "errors": 0}
    for now_ms, five, ten, index in edges:
        stats = stats_at(port, clock, now_ms)
        assert stats["window5m"] == (five or zero), (now_ms, stats["window5m"])
        assert stats["window10m"] == (ten or zero), (now_ms, stats["window10m"])
        assert stats["perMinute"][index]["requests"] == 5, (now_ms, index)
        assert sum(p["requests"] for p in stats["perMinute"]) == 5
        assert (stats["topRoutes10m"] != []) == (ten is not None)
    # Fifteen minutes on the ring slot is stale: no trace remains.
    stats = stats_at(port, clock, (t0_minute + 15) * MINUTE_MS)
    assert stats["window10m"] == zero and stats["topRoutes10m"] == []
    assert all(p["requests"] == 0 and p["clients"] == 0 for p in stats["perMinute"])
    # Every label ever published is a registered template.
    for entry in stats_at(port, clock, t0)["topRoutes10m"]:
        assert TEMPLATE.match(entry["route"]) and entry["route"] == ROUTE


def check_concurrency(port, clock):
    clock.set(clock.now_ms // MINUTE_MS * MINUTE_MS + 30 * MINUTE_MS + 5_000)
    before = statusz(port)[1]["stats"]["window5m"]["requests"]
    failures = []

    def reader(index):
        last = -1
        for i in range(30):
            method = "HEAD" if (i + index) % 3 == 0 else "GET"
            try:
                status, headers, body = request(port, method=method)
                if status != 200:
                    failures.append((method, status))
                    continue
                if method == "HEAD":
                    if body != b"" or int(headers["content-length"]) < 900:
                        failures.append(("HEAD", headers.get("content-length")))
                    continue
                if headers["content-length"] != str(len(body)):
                    failures.append(("length", headers["content-length"], len(body)))
                requests = json.loads(body)["stats"]["window5m"]["requests"]
                if requests < last:
                    failures.append(("went backwards", last, requests))
                last = requests
            except (OSError, ValueError) as error:
                failures.append((method, repr(error)))

    def writer(token):
        for i in range(40):
            status = request(port, target=f"/v1/test-operations/status/{500 if i % 8 == 0 else 200}",
                             headers={"Authorization": "Bearer " + token})[0]
            if status not in (200, 500):
                failures.append(("writer", status))

    threads = [threading.Thread(target=reader, args=(i,)) for i in range(8)]
    threads += [threading.Thread(target=writer, args=(t,)) for t in (TOKEN, TOKEN_B)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    assert not failures, failures[:5]
    final = statusz(port)[1]["stats"]["window5m"]
    assert final == {"clients": 2, "requests": before + 80, "errors": 10}, final


def main(binary):
    pages = json.loads((FIXTURES / "operations_pages.json").read_text())
    fixture_ms = int(datetime.fromisoformat(
        pages["source"]["fixtureTime"].replace("Z", "+00:00")).timestamp() * 1000)
    check_production(str(Path(binary).with_name("fortis_node")), pages)
    with tempfile.TemporaryDirectory(prefix="fortis-statusz-clock-") as directory:
        # The TypeScript oracle was frozen at fixtureTime: start the clock
        # there so the cold stats block can be compared byte for byte.
        clock = Clock(directory, fixture_ms)
        server = Server(binary, clock.path)
        try:
            check_cold_and_posture(server.port, pages, clock)
            check_memo(server.port, clock)
            check_windows_and_labels(server.port, clock)
            check_concurrency(server.port, clock)
        finally:
            server.close()
    print("GET/HEAD /statusz route test passed")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "build/fortis_node_operations_test")
