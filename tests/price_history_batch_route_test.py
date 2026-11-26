#!/usr/bin/env python3
"""Differential replay of the gated native POST /v1/prices/history route.

Every case frozen from the TypeScript route in
tests/corpus/price_history_batch_route.json is sent to the native
test binary over a socket, segment by segment (one process and KV directory
per TypeScript app; the restart pair shares one directory). Status, headers,
body bytes and the CoinGecko URLs served by a local generated-CA TLS stub
must match, apart from the documented native bounds listed in DIFFERENCES.
"""
import hashlib
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
from urllib.parse import parse_qs, urlsplit

import compat_expectations
import tls_ca
from history_contract_expectations import FIXTURE_PROVIDER, batch as current_batch_fixture

FIXTURE = (Path(__file__).resolve().parents[1] /
           "tests/corpus/price_history_batch_route.json")
# Parser-accepted IDs the store cannot key, frozen from the real
# TypeScript route; replayed byte for byte with no documented difference.
OTHER_IDS_FIXTURE = FIXTURE.parent / "price_history_batch_other_ids.json"
TOKEN = "fintel_dev_" + "H" * 43
HOUR = 3600
MAX_NATIVE_BODY = 512 * 1024

# Native bounds that intentionally differ from TypeScript. The value is the
# exact native reply.
DIFFERENCES = {
    # Native HTTP framing refuses bodies over 512 KiB before routing.
    "bodyTooLarge": (413, b'{"error":"BODY_TOO_LARGE"}', False),
    # `dailyBeyondNativeWindow` (a 406-day daily window) matches TypeScript
    # now that the native daily bound is 10 * 366 days.
}


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def request_bytes(spec, oversize_pad):
    """Rebuilds the exact frozen request body."""
    if "hex" in spec:
        return bytes.fromhex(spec["hex"])
    if spec.get("generator") == "oversize":
        text = ('{"ids":["ethereum"],"from":%d,"to":%d,"pad":"%s"}' %
                (FIRST, FIRST + 6 * HOUR, "x" * oversize_pad))
        body = text.encode()
        assert len(body) == spec["bytes"], (len(body), spec["bytes"])
        return body
    if "text" in spec:
        return spec["text"].encode("utf-8")
    return None


def post(port, spec, body, disconnect=False, timeout=40):
    lines = ["POST /v1/prices/history HTTP/1.1", "Host: localhost", "Connection: close"]
    if spec.get("accept"):
        lines.append("Accept: " + spec["accept"])
    if spec.get("authorization", "") is not None:
        lines.append("Authorization: Bearer " + TOKEN)
    if spec.get("origin"):
        lines.append("Origin: " + spec["origin"])
    if spec.get("contentType"):
        lines.append("Content-Type: " + spec["contentType"])
    if body is not None:
        lines.append("Content-Length: %d" % len(body))
    payload = ("\r\n".join(lines) + "\r\n\r\n").encode()
    # Native framing rejects an over-limit Content-Length before reading the
    # body; sending it anyway could reset the socket before the reply.
    if body is not None and len(body) <= MAX_NATIVE_BODY:
        payload += body
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall(payload)
        if disconnect:
            return None
        data = bytearray()
        while True:
            try:
                chunk = sock.recv(65536)
            except ConnectionResetError:
                break
            if not chunk:
                break
            data.extend(chunk)
    header, _, response_body = bytes(data).partition(b"\r\n\r\n")
    head_lines = header.decode("latin1").split("\r\n")
    status = int(head_lines[0].split()[1])
    headers = {key.lower(): value.strip() for line in head_lines[1:] if ":" in line
               for key, value in [line.split(":", 1)]}
    return status, headers, response_body


def start(binary, directory, env_extra):
    directory.mkdir(parents=True, exist_ok=True)
    kv = directory / "kv"
    kv.mkdir(exist_ok=True)
    port = free_port()
    env = os.environ.copy()
    env.update(FIXTURE_PROVIDER)  # History semantics, not anonymous admission limits.
    for key in ("FORTIS_ENABLE_TEST_PRICE_HISTORY_ROUTE", "FORTIS_TEST_HISTORY_FAIL_SOURCE",
                "FORTIS_TEST_HISTORY_BATCH_SEED", "FORTIS_TEST_HISTORY_NOW_MS",
                "FORTIS_TEST_HISTORY_OTHER_IDS_SEED"):
        env.pop(key, None)
    env["FORTIS_KV_DIR"] = str(kv)
    env.update(env_extra)
    process = subprocess.Popen([binary, str(port)], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    for _ in range(150):
        if process.poll() is not None:
            raise RuntimeError(process.stderr.read().decode())
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=.1):
                return process, port
        except OSError:
            time.sleep(.03)
    raise RuntimeError("history batch test server did not start")


def stop(process):
    process.send_signal(signal.SIGTERM)
    process.wait(timeout=15)
    assert process.returncode == 0, process.stderr.read().decode()


class Provider:
    """CoinGecko market_chart/range stub matching the TypeScript fetchImpl."""

    def __init__(self, root):
        self.calls = []
        self.lock = threading.Lock()
        self.delays = {}
        ca, cert, key, _ = tls_ca.create_certificates(root, "DNS:localhost")
        self.ca = ca
        provider = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                parsed = urlsplit(self.path)
                with provider.lock:
                    provider.calls.append(self.path)
                coin = parsed.path.split("/")[4] if len(parsed.path.split("/")) > 4 else ""
                time.sleep(provider.delays.get(coin, 0))
                if coin == "failing-coin":
                    encoded = b"provider unavailable"
                    self.send_response(503)
                else:
                    query = parse_qs(parsed.query)
                    start = int(query["from"][0])
                    end = int(query["to"][0])
                    if coin == "empty-coin":
                        body = {"prices": [], "total_volumes": []}
                    elif coin == "sparse-coin":
                        # Odd hours only, as the TypeScript stub answers.
                        odd = [i for i in range(1, (end - start) // HOUR + 1, 2)
                               if start + i * HOUR < end]
                        body = {"prices": [[(start + i * HOUR) * 1000, 100 + i] for i in odd],
                                "total_volumes": [[(start + i * HOUR) * 1000, .5 + i]
                                                  for i in odd]}
                    else:
                        body = {"prices": [[(start + i * HOUR) * 1000, 100 + i]
                                           for i in range(6)],
                                "total_volumes": [[(start + i * HOUR) * 1000, .5 + i]
                                                  for i in range(6)]}
                    encoded = json.dumps(body, separators=(",", ":")).encode()
                    self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(encoded)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(encoded)
                self.close_connection = True

            def log_message(self, *_args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", free_port()), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certfile=str(cert), keyfile=str(key))
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def env(self):
        return {"FORTIS_ENABLE_TEST_PRICE_HISTORY_ROUTE": "1",
                "FORTIS_TEST_HISTORY_UPSTREAM_PORT": str(self.server.server_address[1]),
                "FORTIS_TEST_HISTORY_UPSTREAM_CA": str(self.ca)}

    def take(self):
        with self.lock:
            calls, self.calls = self.calls, []
        return sorted(calls)


QUOTA_SHIFT = compat_expectations.QuotaShift()


def strict_case(case):
    """The recorded case as this server answers it
    (compat-decisions.md#zod-issues, #form-bodies). None for a text/plain or
    form body, which is the parser's 415 (checked by the caller)."""
    ce = compat_expectations
    if ce.is_removed_body_media(case.get("request", {}).get("contentType")) or \
            ce.fastify_code(case["body"].get("text")):
        # form-bodies + fastify-errors: the parser's refusal after the device
        # policy, at cost 1 (checked by the caller).
        QUOTA_SHIFT.uncharged("device", case["headers"].get("x-rate-limit-cost", 0))
        QUOTA_SHIFT.charged("device", 1)
        return None
    headers = dict(case["headers"])
    if "x-rate-limit-remaining" in headers:
        headers["x-rate-limit-remaining"] = QUOTA_SHIFT.remaining("device", headers["x-rate-limit-remaining"])
    body = case["body"]
    if "text" in body:
        _, headers, text = ce.zod_expected(case["status"], headers, body["text"])
        body = dict(body, text=text)
    return dict(case, headers=headers, body=body)


def check_case(name, case, actual, calls):
    status, headers, body = actual
    mapped = strict_case(case) if name not in DIFFERENCES else case
    if mapped is None:
        recorded = case["body"].get("text") if compat_expectations.fastify_code(case["body"].get("text")) \
            else compat_expectations.UNSUPPORTED_MEDIA_TYPE_FASTIFY
        want_status, _, want_body, _ = compat_expectations.fastify_expected(case["status"], {}, recorded)
        assert status == want_status and body == want_body, (name, status, body)
        assert headers.get("x-rate-limit-cost") == "1", (name, headers)
        assert calls == [], (name, calls)
        return
    case = mapped
    if name in DIFFERENCES:
        want_status, want_body, has_quota = DIFFERENCES[name]
        assert status == want_status, (name, status, body)
        assert body == want_body, (name, body)
        assert ("x-rate-limit-cost" in headers) == has_quota, (name, headers)
        assert calls == [], (name, calls)
        return
    assert status == case["status"], (name, status, case["status"], body[:400])
    for key, value in case["headers"].items():
        assert headers.get(key) == value, (name, key, headers.get(key), value)
    for key in ("x-rate-limit-cost", "retry-after", "vary"):
        assert (key in headers) == (key in case["headers"]), (name, key, headers)
    expected = case["body"]
    if "wireHex" in expected:
        assert body.hex() == expected["wireHex"], (name, body.hex(), expected["wireHex"])
    else:
        assert body == expected["text"].encode("utf-8"), (
            name, body.decode("utf-8", "replace"), expected["text"])
    assert calls == case["providerCalls"], (name, calls, case["providerCalls"])


def wait_for_quota_window(seconds_needed):
    # Quota counters are per wall-clock minute; keep a segment inside one.
    left = 60 - time.time() % 60
    if left < seconds_needed:
        time.sleep(left + .2)


# Coverage segments share one KV directory, each in a fresh process (so the
# durable coverage record crosses restarts), with the owner's clock fixed at
# the same offset from a common base as the TypeScript fake clock from NOW.
COVERAGE_CLOCK_OFFSETS_MS = {
    "coverageSlide": 0,
    "coverageEmpty": 60_000,
    "coverageSparse": 120_000,
    "coverageLater": 5 * HOUR * 1000,
    "coverageExpired": 7 * HOUR * 1000,
}


def segment_directory(root, segment):
    if segment == "restartAfter":
        return root / "restart"
    if segment in COVERAGE_CLOCK_OFFSETS_MS:
        return root / "coverage"
    return root / segment


def segment_env(segment, provider, coverage_base_ms):
    env = provider.env()
    if segment == "main":
        env["FORTIS_TEST_HISTORY_FAIL_SOURCE"] = "broken-coin"
    # The quota segment's 50-ID protobuf batch reads the same seeded coins.
    if segment in ("fifty", "quota"):
        env["FORTIS_TEST_HISTORY_BATCH_SEED"] = "1"
    if segment in COVERAGE_CLOCK_OFFSETS_MS:
        env["FORTIS_TEST_HISTORY_NOW_MS"] = str(
            coverage_base_ms + COVERAGE_CLOCK_OFFSETS_MS[segment])
    return env


def replay(binary, fixture, root, provider):
    oversize = fixture["source"]["oversizePadBytes"]
    segments = []
    for name in fixture["order"]:
        segment = fixture["cases"][name]["segment"]
        if not segments or segments[-1][0] != segment:
            segments.append((segment, []))
        segments[-1][1].append(name)
    replayed = 0
    coverage_base_ms = int(time.time() * 1000)
    for segment, names in segments:
        # Each segment starts a process with a fresh in-memory device quota.
        QUOTA_SHIFT.reset()
        directory = segment_directory(root, segment)
        wait_for_quota_window(25)
        process, port = start(binary, directory,
                              segment_env(segment, provider, coverage_base_ms))
        try:
            provider.take()
            for name in names:
                case = fixture["cases"][name]
                spec = case["request"]
                actual = post(port, spec, request_bytes(spec, oversize))
                check_case(name, case, actual, provider.take())
                replayed += 1
        finally:
            stop(process)
    return replayed


def replay_other_ids(binary, root, provider):
    """Replays the other-ID fixture in one seeded process. The seed writes the
    SPL rows and a real undecodable open row for the corrupt mint, so its
    failure is a genuine kCorrupt read. No case may reach the provider."""
    raw = OTHER_IDS_FIXTURE.read_bytes()
    checksum = OTHER_IDS_FIXTURE.with_name(OTHER_IDS_FIXTURE.name + ".sha256").read_text()
    assert hashlib.sha256(raw).hexdigest() + "\n" == checksum, "fixture checksum mismatch"
    fixture = current_batch_fixture(json.loads(raw))
    names = fixture["order"]
    QUOTA_SHIFT.reset()
    assert {fixture["cases"][name]["segment"] for name in names} == {"otherIds"}
    env = provider.env()
    env["FORTIS_TEST_HISTORY_OTHER_IDS_SEED"] = "1"
    wait_for_quota_window(25)
    process, port = start(binary, root / "other-ids", env)
    try:
        provider.take()
        for name in names:
            case = fixture["cases"][name]
            assert case["providerCalls"] == [], name
            spec = case["request"]
            actual = post(port, spec, request_bytes(spec, 0))
            check_case(name, case, actual, provider.take())
    finally:
        stop(process)
    return len(names)


def native_only(binary, root, provider):
    # Disabled by default in every binary.
    env = {key: value for key, value in provider.env().items()
           if key != "FORTIS_ENABLE_TEST_PRICE_HISTORY_ROUTE"}
    process, port = start(binary, root / "disabled", env)
    try:
        spec = {"contentType": "application/json", "accept": "application/json"}
        body = json.dumps({"ids": ["ethereum"], "from": FIRST, "to": FIRST + 6 * HOUR}).encode()
        assert post(port, spec, body)[0] == 404
    finally:
        stop(process)

    spec = {"contentType": "application/json", "accept": "application/json"}

    def batch(ids, start_at=None, end_at=None):
        payload = {"ids": ids, "from": start_at or FIRST, "to": end_at or FIRST + 6 * HOUR,
                   "interval": "1h"}
        return json.dumps(payload, separators=(",", ":")).encode()

    process, port = start(binary, root / "native-only", provider.env())
    try:
        provider.take()
        # Response-size policy: 20 IDs x 721 hourly buckets cannot fit the
        # 1 MiB worst case, so no provider work starts.
        ids = ["size-coin-%02d" % i for i in range(20)]
        status, headers, body = post(port, spec, batch(ids, FIRST, FIRST + 30 * 86400))
        assert status == 503 and body == b'{"error":"UPSTREAM_UNAVAILABLE"}', (status, body)
        assert headers["x-rate-limit-cost"] == "20"
        assert provider.take() == []
        # The same IDs over seven days fit and are fetched.
        status, _, body = post(port, spec, batch(ids[:3], FIRST, FIRST + 7 * 86400))
        assert status == 200, (status, body[:200])
        assert len(provider.take()) == 3

        # A JSON body between the native 256 KiB parser bound and the 512 KiB
        # framing bound is refused natively (TypeScript would parse it).
        padded = ('{"ids":["ethereum"],"from":%d,"to":%d,"pad":"%s"}' %
                  (FIRST, FIRST + 6 * HOUR, "x" * (300 * 1024))).encode()
        status, headers, body = post(port, spec, padded)
        assert status == 413 and body == b'{"error":"BODY_TOO_LARGE"}', (status, body)
        # Framing accepts this size; authenticated route decoding charges once.
        assert headers["x-rate-limit-cost"] == "1"

        # A client that disconnects while its gap is fetched does not strand
        # its slot: afterwards all four slots serve concurrent provider-backed
        # batches, and the abandoned ID itself still completes.
        provider.delays["slow-coin"] = 1.0
        post(port, spec, batch(["slow-coin"]), disconnect=True)
        time.sleep(1.5)
        for coin in ("parallel-a", "parallel-b", "parallel-c", "parallel-d"):
            provider.delays[coin] = .5
        results = []

        def concurrent(coin):
            results.append(post(port, spec, batch([coin, "ethereum"])))

        workers = [threading.Thread(target=concurrent, args=(coin,))
                   for coin in ("parallel-a", "parallel-b", "parallel-c", "parallel-d")]
        for worker in workers:
            worker.start()
        for worker in workers:
            worker.join(timeout=40)
        assert len(results) == 4 and all(result[0] == 200 for result in results), \
            [(result[0], result[2][:120]) for result in results]
        status, _, body = post(port, spec, batch(["slow-coin", "ethereum"]))
        assert status == 200, (status, body)
        # Lane pacing may defer or shed this late fetch; either way the ID is
        # a history entry (TypeScript swallows a failed backfill), not a hang.
        series = json.loads(body)["series"]
        assert "points" in series["slow-coin"] and "error" not in series["slow-coin"], series
        provider.take()
    finally:
        stop(process)

    # Orderly shutdown answers a batch that waits on the provider and exits 0.
    process, port = start(binary, root / "shutdown", provider.env())
    provider.delays["shutdown-coin"] = 2.0
    replies = []

    def pending():
        try:
            replies.append(post(port, spec, batch(["shutdown-coin"])))
        except (OSError, IndexError, ValueError) as error:
            replies.append(error)

    waiter = threading.Thread(target=pending, daemon=True)
    waiter.start()
    deadline = time.monotonic() + 5
    while not any("shutdown-coin" in call for call in provider.calls) and \
            time.monotonic() < deadline:
        time.sleep(.02)
    assert any("shutdown-coin" in call for call in provider.calls), "batch never reached TLS"
    stop(process)
    waiter.join(timeout=5)
    assert not waiter.is_alive(), "batch client leaked after shutdown"
    if replies and isinstance(replies[0], tuple):
        assert replies[0][0] == 503, replies[0]


def main(binary):
    global FIRST
    fixture = current_batch_fixture(json.loads(FIXTURE.read_text()))
    FIRST = fixture["source"]["first"]
    with tempfile.TemporaryDirectory(prefix="history-batch-") as temporary:
        root = Path(temporary)
        provider = Provider(root)
        try:
            replayed = replay(binary, fixture, root, provider)
            other_ids = replay_other_ids(binary, root, provider)
            native_only(binary, root, provider)
        finally:
            provider.server.shutdown()
            provider.server.server_close()
    print("native POST /v1/prices/history replayed %d frozen TypeScript cases "
          "(%d documented differences) and %d store-unkeyable ID cases; size policy, "
          "cancellation and shutdown passed" % (replayed, len(DIFFERENCES), other_ids))


FIRST = 0

if __name__ == "__main__":
    main(sys.argv[1])
