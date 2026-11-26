#!/usr/bin/env python3
"""Native GET /, /assets/background.jpg, /statusz, /errors and /errorsz
with native-owned page markup and frozen status fixtures, plus the API
telemetry those pages read: scope, clients, reserved completions, memo and a
refresh flood."""
import hashlib, json, os, socket, subprocess, sys, tempfile, threading, time
from pathlib import Path

FIXTURES = Path(__file__).resolve().parents[1] / "tests/corpus"
TOKEN = "fintel_dev_00" + "K" * 41
TOKEN_B = "fintel_dev_01" + "K" * 41
SECURITY = {"x-content-type-options": "nosniff", "referrer-policy": "no-referrer",
            "permissions-policy": "camera=(), microphone=(), geolocation=()"}


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def request(port, path, method="GET", headers=None, body=b""):
    lines = [f"{method} {path} HTTP/1.1", "Host: localhost", "Connection: close"]
    for key, value in (headers or {}).items():
        lines.append(f"{key}: {value}")
    if body:
        lines.append(f"Content-Length: {len(body)}")
    with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
        s.sendall(("\r\n".join(lines) + "\r\n\r\n").encode() + body)
        data = bytearray()
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data.extend(chunk)
    head, _, payload = bytes(data).partition(b"\r\n\r\n")
    status_line, *header_lines = head.decode("latin1").split("\r\n")
    parsed = {}
    for line in header_lines:
        key, _, value = line.partition(":")
        parsed[key.strip().lower()] = value.strip()
    return int(status_line.split()[1]), parsed, payload


def start(binary, kv, extra=None):
    kv.mkdir(parents=True, exist_ok=True)
    port = free_port()
    env = os.environ.copy()
    env["FORTIS_KV_DIR"] = str(kv)
    env.pop("FORTIS_ENABLE_TEST_OPERATIONS", None)
    env.update(extra or {})
    process = subprocess.Popen([binary, str(port)], env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    for _ in range(200):
        if process.poll() is not None:
            raise RuntimeError(process.stderr.read().decode())
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return process, port
        except OSError:
            time.sleep(0.03)
    raise RuntimeError("server failed to start")


def stop(process):
    process.terminate()
    process.communicate(timeout=10)


def outline(value):
    if isinstance(value, list):
        return [] if not value else [outline(value[0])]
    if isinstance(value, dict):
        return {key: outline(item) for key, item in value.items()}
    if value is None:
        return "null"
    return {bool: "boolean", int: "number", float: "number", str: "string"}[type(value)]


def check_public_pages(port, pages):
    status, home_headers, home = request(port, "/")
    assert status == 200 and b"One node." in home and b"What this node provides" in home
    assert b'href="/dashboard"' in home and b'property="og:image"' in home
    assert b'not a live health check' in home and b'Not enabled' in home
    status, headers, catalog = request(port, "/node-features.json")
    expected = Path(__file__).resolve().parents[1] / "contracts/node-future-features.json"
    assert status == 200 and catalog == expected.read_bytes()
    assert headers["content-type"] == "application/json; charset=utf-8"
    assert json.loads(catalog)["status"] == "possible"
    for path, mime in [("node-hall.webp", "image/webp"), ("fortis-favicon.svg", "image/svg+xml"),
                       ("fortisnet-lockup.svg", "image/svg+xml"), ("social-preview.jpg", "image/jpeg")]:
        status, headers, asset = request(port, "/assets/" + path)
        assert status == 200 and headers["content-type"] == mime
        assert asset == (expected.parents[1] / "assets" / path).read_bytes()
        assert headers["cache-control"] == "public, max-age=86400"
        status, headers, body = request(port, "/assets/" + path, method="HEAD")
        assert status == 200 and not body and int(headers["content-length"]) == len(asset)
    for path, body in [("/", home), ("/node-features.json", catalog)]:
        status, headers, payload = request(port, path, method="HEAD")
        assert status == 200 and not payload and int(headers["content-length"]) == len(body)
        assert request(port, path + "?x=1")[2] == body
        assert request(port, path, method="POST")[0] == 404
    assert request(port, "/assets/../contracts/node-future-features.json")[0] == 404
    landing = pages["landing"]["docsEnabled"]
    status, headers, body = request(port, "/dashboard")
    assert status == 200, status
    assert headers["content-type"] == landing["headers"]["content-type"] == "text/html"
    assert headers["cache-control"] == landing["headers"]["cache-control"] == "public, max-age=60"
    assert headers["content-length"] == str(len(body))
    for key, value in SECURITY.items():
        assert headers[key] == value
    text = body.decode("utf-8")
    # The native markup has native counts and separate resource charts.
    assert text.startswith("<!DOCTYPE html>") and text.rstrip().endswith("</html>")
    assert 'href="/doc"' in text
    assert 'id="history-periods"' in text and "'/status-history'" in text
    assert 'id="r-memory-chart"' in text and 'id="r-cpu-chart"' in text
    assert "numeric(rss) ? rss : heap" in text and "No quota reported / unavailable" in text
    assert "43 networks" in text
    assert 'href="/tokens"' in text
    # HEAD answers the GET headers without a body, as Fastify does.
    status, head_headers, head_body = request(port, "/dashboard", method="HEAD")
    assert status == 200 and head_body == b""
    assert head_headers["content-length"] == str(len(body))
    # Root scope: no bearer, no Origin refusal, query strings ignored.
    status, _, again = request(port, "/dashboard?x=1", headers={"Origin": "https://example.com",
                                                       "Authorization": "Bearer nope"})
    assert status == 200 and again == body
    assert request(port, "/dashboard", method="POST")[0] == 404

    status, headers, token_page = request(port, "/tokens")
    assert status == 200 and headers["content-type"] == "text/html; charset=utf-8"
    for period in (b'5m', b'15m', b'30m', b'1h', b'8h'):
        assert b'data-range="' + period + b'"' in token_page
    assert b'Cached Tokens' in token_page and b'id="token-list"' in token_page
    status, head_headers, body = request(port, "/tokens", method="HEAD")
    assert status == 200 and not body and head_headers["content-length"] == str(len(token_page))
    status, _, body = request(port, "/tokens/data")
    assert status == 200 and json.loads(body) == {"tokens": []}
    assert request(port, "/tokens/data/ethereum?range=wrong")[0] == 400

    # /doc is the public, generated API reference linked from the footer.
    status, doc_headers, docs = request(port, "/doc")
    assert status == 200 and doc_headers["content-type"] == "text/html"
    assert b"/v1/" in docs and len(docs) > 10000
    assert request(port, "/documentation")[2] == docs
    status, head_headers, head_body = request(port, "/doc", method="HEAD")
    assert status == 200 and head_body == b"" and head_headers["content-length"] == str(len(docs))

    # The history is already on the server when the page opens, independent
    # of a browser polling /statusz. Selection in the page slices these slots.
    status, headers, body = request(port, "/status-history")
    assert status == 200 and headers["content-type"] == "application/json; charset=utf-8"
    history = json.loads(body)
    assert history["slotDurationSec"] == 60 and history["maxMinutes"] == 480
    assert len(history["slots"]) == 480
    assert all(item["duration"] == 60 for item in history["slots"])
    assert history["slots"][-1]["created"] >= int(time.time()) // 60 * 60 - 60
    assert set(history["slots"][-1]) == {"created", "duration", "observed", "memoryMb", "cpuPercent", "diskScope", "diskUsedMb", "requests"}
    assert set(history["slots"][-1]["requests"]) == {"total", "errors", "avgDurationMs", "maxDurationMs"}
    status, head_headers, head_body = request(port, "/status-history", method="HEAD")
    assert status == 200 and head_body == b"" and head_headers["content-length"] == str(len(body))

    background = pages["background"]
    status, headers, body = request(port, "/assets/background.jpg")
    assert status == 200 and headers["content-type"] == "image/jpeg"
    assert headers["cache-control"] == background["headers"]["cache-control"]
    assert len(body) == background["length"]
    assert hashlib.sha256(body).hexdigest() == background["sha256"]
    status, headers, body = request(port, "/assets/background.jpg", method="HEAD")
    assert status == 200 and body == b"" and headers["content-length"] == str(background["length"])
    assert request(port, "/assets/missing.jpg")[0] == 404

    errors = pages["errors"]
    status, headers, body = request(port, "/errors")
    assert status == 200 and b"Errors &amp; dependencies" in body
    assert all(label in body for label in (b"Node errors", b"Third-party errors", b"Unclassified outcomes"))
    assert b"fetch('/errorsz')" in body and b"writeFailed" in body
    length = str(len(body))
    assert headers["content-length"] == length
    for key in ("content-type", "cache-control"):
        assert headers[key] == errors["headers"][key], key
    status, headers, body = request(port, "/errors", method="HEAD")
    assert status == 200 and body == b"" and headers["content-length"] == length

    errorsz = pages["errorsz"]
    status, headers, body = request(port, "/errorsz")
    assert status == 200
    document = json.loads(body)
    operational = document.pop("operational")
    assert document == json.loads(errorsz["body"])
    assert operational == {"persisted": True, "writeFailed": False, "records": []}
    assert headers["content-type"] == errorsz["headers"]["content-type"]
    # TypeScript sends no Cache-Control on its root JSON routes; the native
    # server's default is no-store.
    assert headers["cache-control"] == "no-store"

    status, headers, body = request(port, "/benchmark")
    assert status == 200 and headers["content-type"].startswith("text/html")
    assert b"Benchmark Stats" in body and b"Benchmark results" in body
    embedded = body.split(b'<script type="application/json" id="published-reports">', 1)[1]
    reports = json.loads(embedded.split(b'</script>', 1)[0])
    assert len(reports) >= 6
    assert reports[0]["records"][0]["method"]
    split = next(record for report in reports for record in report["records"]
                 if record.get("label", "").startswith("Updated"))
    assert len(split["routes"]["snapshot"]["fresh_by_id"]) == 6
    assert len(split["routes"]["spot"]["fresh_by_id"]) == 12
    for route in ("snapshot", "spot"):
        values = split["routes"][route]
        assert all(0 <= count <= values["requests"] for count in values["fresh_by_id"].values())
    assert split["users"] == 100 and split["rounds"] == 3
    assert split["price_store"]["contractMemoryHits"] > 0
    for key, count in split["price_store"].items():
        assert count == sum(window["price_store"][key] for window in split["windows"])
    if split.get("public_ingress"):
        ingress = split["public_ingress"]
        assert ingress["requests"] == sum(route["requests"] for route in split["routes"].values())
        assert ingress["nginx_seen"] <= ingress["requests"]
        assert ingress["transport_with_nginx_record"] <= sum(
            sum(route["transport_failures"].values()) for route in split["routes"].values())
        for route in ("snapshot", "spot", "health"):
            result = split["routes"][route]
            if result["status"].get("200", 0):
                assert set(result["milestones_ms"]) == {"tcp", "tls", "headers", "body"}
                if "tls_handshake_ms" in result:
                    assert 0 <= result["tls_handshake_ms"]["p99"] <= result["milestones_ms"]["tls"]["p99"]
    if split.get("http_controls"):
        control = split["http_controls"]["HTTP/2 after rollout"]
        assert control["protocol"] == "2" and control["requests"] == 300
        assert b"TLS handshake after TCP" in body
        assert b"WikiCat direct CoinGecko probe" in body
    matched = next(report for report in reports if report["label"].endswith("matched repeat"))
    comparison = [record for record in matched["records"] if record.get("mode") == "group-commit-canary"]
    assert [record["group_commit"] for record in comparison] == [False, True]
    for run in comparison:
        assert run["users"] == 100 and run["rounds"] == 3
        assert run["flushes"] == sum(window["syncs"] for window in run["windows"])
        assert run["frames"] == sum(window["frames"] for window in run["windows"])
        assert set(run["routes"]) == set(run["errors"]) == {"snapshot", "spot", "rotate", "revoke", "health"}
        assert len(run["preconditions"]) == 2
        assert all("admission" in window for window in run["windows"])
        assert all("responses_with_stale" in run["routes"][price] for price in ("snapshot", "spot"))
    slots = next(report for report in reports if " · two-slot commit · " in report["label"])
    rollback, commit_slots = slots["records"][1:]
    assert [rollback["kv_durability"], commit_slots["kv_durability"]] == ["rollback-marker", "commit-slots"]
    for run in (rollback, commit_slots):
        assert run["users"] == 100 and run["rounds"] == 3
        assert run["flushes"] == sum(window["syncs"] for window in run["windows"])
        assert set(run["routes"]) == set(run["errors"]) == {"snapshot", "spot", "rotate", "revoke", "health"}
        assert set(run["external_health"]["all_latency_ms"]) >= {"p95", "p99"}
    assert {record["mode"] for report in reports for record in report["records"] if "mode" in record} >= {
        "burst", "sustained", "guest-crash-replay", "process-crash"}
    status, _, body = request(port, "/benchmark", method="HEAD")
    assert status == 200 and body == b""

    statusz = pages["statusz"]
    status, headers, body = request(port, "/statusz")
    assert status == 200 and headers["content-type"] == statusz["headers"]["content-type"]
    document = json.loads(body)
    expected = dict(statusz["outline"])
    # Native truth: `node` names the native runtime (a string, as OpenAPI
    # requires) rather than a Node.js version, memory only where measurable,
    # and no coordinator/tokenRefresh/databaseOperations10m subsystems.
    for absent in ("coordinator", "tokenRefresh"):
        expected.pop(absent)
    expected["stats"] = {key: value for key, value in expected["stats"].items()
                         if key != "databaseOperations10m"}
    actual = outline(document)
    assert document.get("sockets") is None  # guest-only
    assert [key for key in document if key not in ("database", "databaseActivity", "httpTiming", "snapshotAuth", "resources")] == list(expected)
    actual.pop("database", None)  # optional until KV is opened
    actual.pop("databaseActivity", None)
    resource_outline = actual.pop("resources")
    assert resource_outline["diskScope"] == "string"
    assert document["resources"]["diskScope"] in ("filesystem", "data-tree", "unavailable")
    assert all(value is None or isinstance(value, (int, float)) and value >= 0
               for key, value in document["resources"].items() if key != "diskScope")
    assert actual.pop("snapshotAuth") == {"hits": "number", "misses": "number", "invalidations": "number"}
    timings = document["httpTiming"]
    assert len(timings["dispatch"]) == len(timings["boundsUs"]) + 1
    actual.pop("httpTiming")
    assert actual["memory"]["heapUsed"] in ("null", "number")
    assert actual["memory"]["rss"] in ("null", "number")
    actual["memory"] = expected["memory"]
    assert actual == expected, (actual, expected)
    assert document["ok"] is True and document["version"] == statusz["versionWhenRead"]
    assert document["node"] == "native-cpp" and isinstance(document["uptimeSec"], int)
    stats = document["stats"]
    assert len(stats["perMinute"]) == 15
    assert stats["perMinute"][-1]["startSec"] == int(time.time()) // 60 * 60 or \
        stats["perMinute"][-1]["startSec"] == (int(time.time()) // 60 - 1) * 60
    status, _, body = request(port, "/statusz", method="HEAD")
    assert status == 200 and body == b""


def errorsz(port):
    status, _, body = request(port, "/errorsz")
    assert status == 200
    return json.loads(body)


def statusz(port):
    status, _, body = request(port, "/statusz")
    assert status == 200
    return json.loads(body)


def check_telemetry(port):
    before = errorsz(port)
    assert before["totalRequests"] == 0 and before["recent"] == []
    status, headers, body = request(port, "/tokens/data")
    assert status == 200 and headers["cache-control"] == "no-store"
    assert json.loads(body)["tokens"][0]["id"] == "ethereum"
    for period in ("5m", "15m", "30m", "1h", "8h"):
        status, _, body = request(port, "/tokens/data/ethereum?range=" + period)
        assert status == 200
        document = json.loads(body)
        assert document["id"] == "ethereum" and document["range"] == period
        assert document["bucketMinutes"] == 5 and document["spot"]["price"] == 120
        if period == "8h":
            assert len(document["points"]) == 3
    # Root-scoped pages, probes and unknown paths are not API traffic.
    for path in ("/", "/statusz", "/errors", "/errorsz", "/healthz", "/assets/background.jpg",
                 "/v1/unknown-route", "/probe/test-kv-sweeps"):
        request(port, path)
    assert errorsz(port)["totalRequests"] == 0
    route = "/v1/test-operations/status/:status"
    for _ in range(5):
        status, _, _ = request(port, "/v1/test-operations/status/503",
                               headers={"Authorization": "Bearer " + TOKEN})
        assert status == 503
    assert request(port, "/v1/test-operations/status/200",
                   headers={"Authorization": "Bearer " + TOKEN_B})[0] == 200
    # A rejected bearer is a request without a client.
    assert request(port, "/v1/test-operations/status/200",
                   headers={"Authorization": "Bearer fintel_dev_" + "Z" * 43})[0] == 401
    # A reserved socket is recorded when the handoff answers 504.
    status, _, _ = request(port, "/v1/test-operations/reserve?timeout_ms=150")
    assert status == 504
    deadline = time.time() + 5
    while errorsz(port)["totalRequests"] < 8 and time.time() < deadline:
        time.sleep(0.05)
    feed = errorsz(port)
    assert feed["totalRequests"] == 8 and feed["totalErrors"] == 6, feed
    assert feed["routes"][0] == {"service": "API", "route": route, "requests": 7,
                                 "errors": 5, "overThreshold": True}, feed["routes"]
    assert feed["routes"][1]["route"] == "/v1/test-operations/reserve"
    assert feed["routes"][1]["errors"] == 1 and feed["routes"][1]["overThreshold"] is False
    assert feed["recent"][0]["status"] == 504 and feed["recent"][0]["route"] == "/v1/test-operations/reserve"
    assert [item["status"] for item in feed["recent"][1:]] == [503] * 5
    assert all(item["service"] == "API" for item in feed["recent"])
    stats = statusz(port)["stats"]
    assert stats["window5m"] == {"clients": 2, "requests": 8, "errors": 6}, stats["window5m"]
    top = stats["topRoutes10m"][0]
    assert top["route"] == route and top["requests"] == 7 and top["errors"] == 5
    assert len(top["durationBuckets"]) == 11 and sum(top["durationBuckets"]) == 7
    status, _, page = request(port, "/stats")
    assert status == 200 and b"Node request statistics" in page
    detailed = json.loads(request(port, "/statsz")[2])
    assert detailed["windowMinutes"] == 15
    details = {(row["method"], row["route"]): row for row in detailed["routes"]}
    assert details["GET", route]["requests"] == 7
    assert details["GET", route]["errors5xx"] == 5
    assert details["GET", "/v1/test-operations/reserve"]["errors5xx"] == 1
    assert sum(row["requests"] for row in detailed["platforms"] if row["os"] != "unknown") == 6
    assert sum(row["requests"] for row in detailed["platforms"] if row["os"] == "unknown") == 2
    # No device identifier, token or raw URL reaches either document.
    for text in (json.dumps(feed), json.dumps(stats), json.dumps(detailed)):
        assert "01ARZ3NDEKTSV4RRFFQ69G5F" not in text and "fintel_dev_" not in text
        assert "/status/503" not in text and "timeout_ms" not in text
    # Memoised while quiet, rebuilt by new API traffic.
    first = request(port, "/errorsz")[2]
    second = request(port, "/errorsz")[2]
    assert json.loads(first) == json.loads(second)
    request(port, "/v1/test-operations/status/500")
    assert errorsz(port)["totalErrors"] == 7
    history = json.loads(request(port, "/status-history")[2])["slots"]
    assert sum(slot["requests"]["total"] for slot in history) == 9
    assert sum(slot["requests"]["errors"] for slot in history) == 7


def check_flood(port):
    landing = request(port, "/")[2]
    failures = []

    def worker():
        for index in range(40):
            path = ("/", "/statusz", "/errorsz", "/errors")[index % 4]
            try:
                status, _, body = request(port, path)
                if status != 200 or (path == "/" and body != landing):
                    failures.append((path, status))
            except OSError as error:
                failures.append((path, str(error)))

    threads = [threading.Thread(target=worker) for _ in range(6)]
    started = time.time()
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    assert not failures, failures[:5]
    print(f"refresh flood: 240 public requests in {time.time() - started:.2f}s")
    # The flood counted as no API traffic.
    assert errorsz(port)["totalRequests"] == 9
    history = json.loads(request(port, "/status-history")[2])["slots"]
    assert sum(slot["requests"]["total"] for slot in history) == 9


def main(binary):
    pages = json.loads((FIXTURES / "operations_pages.json").read_text())
    normal = str(Path(binary).with_name("fortis_node"))
    with tempfile.TemporaryDirectory(prefix="operations-routes-") as directory:
        root = Path(directory)
        # The production binary registers every route, with no test flag.
        process, port = start(normal, root / "normal")
        try:
            check_public_pages(port, pages)
        finally:
            stop(process)
        process, port = start(binary, root / "telemetry", {"FORTIS_ENABLE_TEST_OPERATIONS": "1"})
        try:
            status = statusz(port)
            database = status["database"]
            assert database["indexedKeys"] > 0
            assert database["fileBytes"] > database["liveBytes"] > 0
            assert database["sampleAgeSec"] == 0 and database["recoveryPending"] is False
            assert database["compactionPhase"] == "idle"
            activity = status["databaseActivity"]
            assert sum(activity["writes"]) > 0  # device fixture writes
            assert len(activity["reads"]) == len(activity["boundsUs"]) + 1
            assert activity["lockWaiters"] == 0
            assert sum(activity["writeHold"]) == sum(activity["writes"])
            assert sum(activity["fileSync"]) >= sum(activity["writes"])
            assert all(len(activity[key]) == len(activity["reads"]) for key in (
                "readHold", "scanHold", "sweepHold", "compactionHold", "recoveryHold", "fileRead",
                "samplerWakeLag"))
            check_telemetry(port)
            check_flood(port)
        finally:
            stop(process)
        # With the split price store (production), the token pages read the
        # series rows from prices-v1/, where they are seeded; the main store
        # has none.
        # The price canary needs a parseable CA bundle; no provider is called.
        ca_bundle = next(Path(path) for path in ("/etc/ssl/certs/ca-certificates.crt",
                                                 "/etc/ssl/cert.pem") if Path(path).is_file())
        process, port = start(binary, root / "split", {
            "FORTIS_ENABLE_TEST_OPERATIONS": "1", "FORTIS_NATIVE_PRICE_ROUTE_CANARY": "1",
            "FORTIS_SPLIT_PRICE_STORE": "1", "FORTIS_CA_BUNDLE_PATH": str(ca_bundle)})
        try:
            assert (root / "split" / "prices-v1").is_dir()
            status, _, body = request(port, "/tokens/data")
            assert status == 200 and json.loads(body)["tokens"][0]["id"] == "ethereum", body
            status, _, body = request(port, "/tokens/data/ethereum?range=8h")
            document = json.loads(body)
            assert status == 200 and document["spot"]["price"] == 120 and len(document["points"]) == 3
        finally:
            stop(process)
        # In-memory telemetry, like TypeScript: a restart starts empty.
        process, port = start(binary, root / "telemetry", {"FORTIS_ENABLE_TEST_OPERATIONS": "1"})
        try:
            assert errorsz(port)["totalRequests"] == 0
        finally:
            stop(process)
    print("operations routes: public pages, statusz, errorsz and telemetry OK")


if __name__ == "__main__":
    main(sys.argv[1])
