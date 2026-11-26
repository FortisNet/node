#!/usr/bin/env python3
"""Native POST /v1/crash-reports against its frozen TypeScript oracle
(crash_reports_route.json): every JSON, protobuf, auth and validation case,
the stored (redacted) record, restart persistence, seven-day expiry, the
expiry sweep, the production binary, size bounds and concurrency."""
import json, os, re, socket, subprocess, sys, tempfile, threading, time
from pathlib import Path

import compat_expectations

FIXTURES = Path(__file__).resolve().parents[1] / "tests/corpus"
FIXTURE_NOW_MS = 1767323045000  # 2026-01-02T03:04:05.000Z
RETENTION_MS = 7 * 24 * 60 * 60 * 1000
ULID = re.compile(r"^[0-9A-HJKMNP-TV-Z]{26}$")
COMPARED_HEADERS = ("content-type", "vary", "cache-control", "x-content-type-options",
                    "referrer-policy", "permissions-policy", "x-rate-limit-cost",
                    "x-rate-limit-remaining", "retry-after", "www-authenticate", "etag",
                    "content-security-policy")


def token(index):
    return f"fintel_dev_{index:02d}" + "K" * 41


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def request(port, path, method="GET", headers=None, body=b""):
    lines = [f"{method} {path} HTTP/1.1", "Host: localhost", "Connection: close"]
    for key, value in (headers or {}).items():
        lines.append(f"{key}: {value}")
    if body or method == "POST":
        lines.append(f"Content-Length: {len(body)}")
    with socket.create_connection(("127.0.0.1", port), timeout=20) as s:
        try:
            s.sendall(("\r\n".join(lines) + "\r\n\r\n").encode() + body)
        except (BrokenPipeError, ConnectionResetError):
            pass  # an early refusal (413) may close before the upload ends
        data = bytearray()
        while True:
            try:
                chunk = s.recv(65536)
            except ConnectionResetError:
                break
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


def start(binary, kv, clock=None, extra=None, test=True):
    kv.mkdir(parents=True, exist_ok=True)
    port = free_port()
    env = os.environ.copy()
    env["FORTIS_KV_DIR"] = str(kv)
    env.pop("FORTIS_ENABLE_TEST_OPERATIONS", None)
    if test:
        env["FORTIS_ENABLE_TEST_OPERATIONS"] = "1"
    if clock:
        env["FORTIS_TEST_OPERATIONS_NOW_FILE"] = str(clock)
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


def varint(data, offset):
    value = shift = 0
    while True:
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if byte < 0x80:
            return value, offset
        shift += 7


def decode_response(data):
    result, offset = {}, 0
    while offset < len(data):
        key, offset = varint(data, offset)
        number, wire = key >> 3, key & 7
        if wire == 0:
            value, offset = varint(data, offset)
            if number == 1:
                result["accepted"] = bool(value)
        elif wire == 2:
            length, offset = varint(data, offset)
            if number == 2:
                result["id"] = data[offset:offset + length].decode()
            offset += length
        else:
            raise ValueError("unexpected wire type")
    return result


def body_bytes(body):
    if body["kind"] == "none":
        return b""
    if body["kind"] == "text":
        return body["value"].encode("utf-8")
    return bytes.fromhex(body["value"])


def post(port, case_headers, body, device):
    headers = dict(case_headers)
    if device is not None:
        headers["Authorization"] = "Bearer " + token(device)
    return request(port, "/v1/crash-reports", method="POST", headers=headers, body=body)


def stored(port, report_id, raw=False):
    status, _, body = request(port, f"/probe/test-crash-report/{report_id}")
    if status != 200:
        return None
    return body if raw else json.loads(body)


def stringify(value):
    """JSON.stringify bytes for parsed fixture values (key order preserved)."""
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def strict_expected(case, shift, headers):
    """The recorded answer as this server gives it
    (compat-decisions.md#zod-issues, #form-bodies, #js-prototype-keys).
    Returns (expected, unknown_remaining)."""
    ce = compat_expectations
    expected = case["response"]
    bearer = case["device"]
    content_type = {k.lower(): v for k, v in case["headers"].items()}.get("content-type")
    if ce.is_prototype_poisoning(body_bytes(case["body"]), expected.get("json")):
        if headers.get("x-rate-limit-cost"):
            shift.charged(bearer, headers["x-rate-limit-cost"])
        added = dict(expected["headers"], **{"x-rate-limit-cost": "5"})
        return dict(expected, json=json.loads(ce.BAD_REQUEST),
                    headers=ce.rewrite_headers(added, len(ce.BAD_REQUEST))), True
    framing = ce.fastify_code(expected.get("json"))
    if ce.is_removed_body_media(content_type) or framing:
        # form-bodies + fastify-errors: the body parser's refusal after the
        # device policy (cost 5), or the policy's own refusal first.
        if expected["headers"].get("x-rate-limit-cost"):
            shift.uncharged(bearer, expected["headers"]["x-rate-limit-cost"])
        recorded = json.dumps(expected["json"]) if framing else ce.UNSUPPORTED_MEDIA_TYPE_FASTIFY
        request_headers = dict(case["headers"])
        if bearer is not None:
            request_headers["Authorization"] = "Bearer " + token(bearer)
        status, kept, body, charged = ce.fastify_expected(expected["status"], expected["headers"], recorded,
                                                          request_headers)
        if charged:
            kept = dict(kept, **{"x-rate-limit-cost": "5"})
            shift.charged(bearer, 5)
        return dict(expected, status=status, json=json.loads(body), headers=kept), charged
    if expected["status"] == 400 and ce.is_zod_refusal(expected.get("json")):
        expected = dict(expected, json=json.loads(ce.BAD_REQUEST),
                        headers=ce.rewrite_headers(expected["headers"], len(ce.BAD_REQUEST)))
    if "x-rate-limit-remaining" in expected["headers"]:
        remaining = shift.remaining(bearer, expected["headers"]["x-rate-limit-remaining"])
        expected = dict(expected, headers=dict(expected["headers"], **{"x-rate-limit-remaining": remaining}))
    return expected, False


def replay(port, cases):
    """Returns {case name: stored id} for every accepted case."""
    accepted = {}
    shift = compat_expectations.QuotaShift()
    for case in cases:
        status, headers, payload = post(port, case["headers"], body_bytes(case["body"]),
                                        case["device"])
        expected, unknown_remaining = strict_expected(case, shift, headers)
        name = case["name"]
        assert status == expected["status"], (name, status, payload[:300])
        for key in COMPARED_HEADERS:
            if key == "x-rate-limit-remaining" and unknown_remaining:
                assert key in headers, name
                continue
            want = expected["headers"].get(key)
            got = headers.get(key)
            if key in ("x-content-type-options", "referrer-policy", "permissions-policy",
                       "cache-control"):
                assert got == want, (name, key, got, want)  # TS sets these on /v1 too
            else:
                assert got == want, (name, key, got, want)
        assert headers["content-length"] == str(len(payload))
        # Same bytes as TypeScript, not just the same parsed value.
        assert headers["content-length"] == expected["headers"]["content-length"], name
        report_id = None
        if "protobuf" in expected:
            assert len(payload) == expected["wireLength"], name
            decoded = decode_response(payload)
            report_id = decoded["id"]
            assert dict(decoded, id="<ulid>") == expected["protobuf"], (name, decoded)
        else:
            document = json.loads(payload)
            if "id" in document:
                report_id = document["id"]
                document["id"] = "<ulid>"
            assert document == expected["json"], (name, document, expected["json"])
            wanted = stringify(expected["json"])
            if report_id:
                wanted = wanted.replace(b"<ulid>", report_id.encode())
            assert payload == wanted, (name, payload[:400], wanted[:400])
        if report_id is None:
            assert case["stored"] is None
            continue
        assert ULID.match(report_id), report_id
        # ulid(): the first ten characters encode the creation millisecond.
        raw = stored(port, report_id, raw=True)
        assert raw is not None, name
        record = json.loads(raw)
        assert record["id"] == report_id
        assert dict(record, id="<ulid>") == case["stored"], (name, record, case["stored"])
        assert raw == stringify(case["stored"]).replace(b"<ulid>", report_id.encode()), name
        accepted[name] = report_id
    return accepted


def check_bounds(port):
    # A declared body over the 512 KiB request bound is refused from the
    # header alone, before routing or reading it.
    with socket.create_connection(("127.0.0.1", port), timeout=20) as s:
        s.sendall(b"POST /v1/crash-reports HTTP/1.1\r\nHost: localhost\r\n"
                  b"Content-Type: application/json\r\nContent-Length: 600000\r\n\r\n")
        reply = s.recv(4096)
    assert reply.startswith(b"HTTP/1.1 413 "), reply[:80]
    # More JSON tokens than the native parser's 8,192 bound: 413, not a guess.
    many = ("[" + ",".join(["1"] * 9000) + "]").encode()
    status, _, payload = post(port, {"content-type": "application/json"}, many, 60)
    assert status == 413 and json.loads(payload) == {"error": "BODY_TOO_LARGE"}
    # Deeply nested JSON beyond the native depth bound (16): also 413.
    deep = ("[" * 20 + "]" * 20).encode()
    assert post(port, {"content-type": "application/json"}, deep, 60)[0] == 413
    # A maximal valid report (4,000 / 24,000 units of placeholder-expanding
    # text) is stored with its redaction applied.
    message = "1.1.1 " * 666 + "abcd"
    stack = "1.1.1 " * 4000
    report = {"platform": "ios", "buildMode": "release", "source": "flutter", "fatal": False,
              "errorType": "E" * 160, "message": message, "stack": stack,
              "occurredAt": "2026-01-02T03:04:05Z"}
    status, _, payload = post(port, {"content-type": "application/json"},
                              json.dumps(report).encode(), 61)
    assert status == 202, payload
    record = stored(port, json.loads(payload)["id"])
    assert record["stack"].count("[REDACTED_ADDRESS]") == 4000
    assert "1.1.1" not in record["message"]


def check_concurrency(port):
    results, failures = [], []

    def worker(device):
        report = {"platform": "android", "buildMode": "debug", "source": "platform",
                  "fatal": True, "errorType": f"E{device}", "message": "m", "stack": "s",
                  "occurredAt": "2026-01-02T03:04:05Z"}
        for _ in range(5):
            status, _, payload = post(port, {"content-type": "application/json",
                                             "accept": "application/protobuf"},
                                      json.dumps(report).encode(), device)
            if status != 202:
                failures.append(status)
            else:
                results.append((device, decode_response(payload)["id"]))

    threads = [threading.Thread(target=worker, args=(device,)) for device in range(52, 58)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    assert not failures, failures
    assert len(results) == 30 and len({report_id for _, report_id in results}) == 30
    for device, report_id in results:
        record = stored(port, report_id)
        assert record["errorType"] == f"E{device}"
        assert record["deviceId"] == f"01ARZ3NDEKTSV4RRFFQ69G5F{device:02d}"


def main(binary):
    fixture = json.loads((FIXTURES / "crash_reports_route.json").read_text())
    cases = fixture["cases"]
    normal = str(Path(binary).with_name("fortis_node"))
    with tempfile.TemporaryDirectory(prefix="crash-reports-") as directory:
        root = Path(directory)
        clock = root / "now"
        clock.write_text(str(FIXTURE_NOW_MS))
        kv = root / "kv"
        process, port = start(binary, kv, clock)
        try:
            accepted = replay(port, cases)
            assert len(accepted) == sum(1 for case in cases if case["stored"] is not None)
            print(f"crash reports: {len(cases)} TypeScript cases replayed, "
                  f"{len(accepted)} stored records identical")
            check_bounds(port)
            check_concurrency(port)
            records = {name: stored(port, report_id) for name, report_id in accepted.items()}
        finally:
            stop(process)

        # Durable across a restart, byte for byte.
        process, port = start(binary, kv, clock)
        try:
            for name, report_id in accepted.items():
                assert stored(port, report_id) == records[name], name
            # The production binary accepts reports from the same KV.
            pass
        finally:
            stop(process)
        process, port = start(normal, kv, test=False)
        try:
            valid = next(case for case in cases if case["name"] == "json valid")
            status, headers, payload = post(port, valid["headers"], body_bytes(valid["body"]), 62)
            assert status == 202 and headers["x-rate-limit-cost"] == "5"
            production_id = json.loads(payload)["id"]
            assert request(port, f"/probe/test-crash-report/{production_id}")[0] == 404
        finally:
            stop(process)
        clock.write_text(str(int(time.time() * 1000)))
        process, port = start(binary, kv, clock)
        try:
            record = stored(port, production_id)
            assert record and record["deviceId"] == "01ARZ3NDEKTSV4RRFFQ69G5F62"
            assert record["message"] == "failed for [REDACTED_TOKEN] and [REDACTED_HEX]"
        finally:
            stop(process)

        # Seven-day retention: visible up to expiresAt, invisible after.
        sample = next(iter(accepted.values()))
        clock.write_text(str(FIXTURE_NOW_MS + RETENTION_MS - 1))
        process, port = start(binary, kv, clock, {"FORTIS_TEST_KV_SWEEP_INTERVAL_MS": "3600000"})
        try:
            assert stored(port, sample) is not None
            clock.write_text(str(FIXTURE_NOW_MS + RETENTION_MS))
            assert stored(port, sample) is None
            assert request(port, "/probe/test-kv-sweeps")[2] == b'{"swept":0}'
        finally:
            stop(process)
        # The expiry sweep (TypeScript: every 10 s, 256 keys) then deletes them.
        process, port = start(binary, kv, clock, {"FORTIS_TEST_KV_SWEEP_INTERVAL_MS": "100"})
        try:
            deadline = time.time() + 10
            swept = 0
            while time.time() < deadline:
                swept = json.loads(request(port, "/probe/test-kv-sweeps")[2])["swept"]
                if swept >= len(accepted):
                    break
                time.sleep(0.1)
            assert swept >= len(accepted), swept
        finally:
            stop(process)
        # Physically gone: even a clock back at creation time finds nothing.
        clock.write_text(str(FIXTURE_NOW_MS))
        process, port = start(binary, kv, clock, {"FORTIS_TEST_KV_SWEEP_INTERVAL_MS": "3600000"})
        try:
            assert all(stored(port, report_id) is None for report_id in accepted.values())
        finally:
            stop(process)
    print("crash reports: auth, validation, redaction, persistence, expiry and sweep OK")


if __name__ == "__main__":
    main(sys.argv[1])
