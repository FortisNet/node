#!/usr/bin/env python3
"""Differential replay of ten-year daily history on the gated native routes.

Every case frozen from TypeScript in
tests/corpus/price_history_daily_decade.json (GET and POST
/v1/prices/history) is sent to the native test binary, one process and KV
directory per TypeScript app (the restart pair shares one directory). The
CoinGecko stub is a local generated-CA TLS server that regenerates provider
bodies from the fixture's compact model. Status, every frozen header, the
response length and SHA-256 (full bytes for small bodies) and the provider
URLs must match.

Native-only checks then cover the documented bounds: the batch response-size
refusal, and provider bodies over 512 KiB or over 3,700 points, which must be
swallowed like any failed backfill (cached-only, no coverage), never an
accidental empty success that suppresses the next fetch.
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

import tls_ca
from history_contract_expectations import FIXTURE_PROVIDER

FIXTURE = (Path(__file__).resolve().parents[1] /
           "tests/corpus/price_history_daily_decade.json")
TOKEN = "fintel_dev_" + "H" * 43
HOUR = 3600
DAY = 86400
NATIVE_PROVIDER_BODY_LIMIT = 512 * 1024
NATIVE_MAX_POINTS = 3700


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def js_number(value):
    """Number.prototype.toString for the model's finite values."""
    if float(value).is_integer() and abs(value) < 1e21:
        return str(int(value))
    return repr(float(value))


def series_json(name, rows):
    return '"%s":[%s]' % (name, ",".join("[%d,%s]" % (t, js_number(v)) for t, v in rows))


class Model:
    """The TypeScript stub's marketChart(), from the fixture's source.model."""

    def __init__(self, model):
        self.base = model["base"]
        self.daily_above = model["dailyAboveSeconds"]
        self.price = model["price"]
        self.volume = model["volume"]
        self.cap = model["marketCap"]

    def body(self, start, end, points=None, with_caps=True):
        step = DAY if end - start > self.daily_above else HOUR
        prices, caps, volumes = [], [], []
        first = -(-start // step) * step
        t = first
        while (points is None and t <= end) or (points is not None and len(prices) < points):
            hours = (t - self.base) / HOUR
            prices.append((t * 1000, self.price["base"] + hours * self.price["perHour"]))
            caps.append((t * 1000, self.cap["base"] + hours * self.cap["perHour"] +
                         self.cap["offset"]))
            volumes.append((t * 1000, self.volume["base"] + hours * self.volume["perHour"]))
            t += step
        parts = [series_json("prices", prices)]
        if with_caps:
            parts.append(series_json("market_caps", caps))
        parts.append(series_json("total_volumes", volumes))
        return ("{" + ",".join(parts) + "}").encode()


class Provider:
    """CoinGecko market_chart/range stub matching the TypeScript fetchImpl,
    plus native-only coins for the provider-body bounds."""

    def __init__(self, root, model):
        self.calls = []
        self.lock = threading.Lock()
        self.model = model
        ca, cert, key, _ = tls_ca.create_certificates(root, "DNS:localhost")
        self.ca = ca
        provider = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                parsed = urlsplit(self.path)
                with provider.lock:
                    provider.calls.append(self.path)
                parts = parsed.path.split("/")
                coin = parts[4] if len(parts) > 4 else ""
                query = parse_qs(parsed.query)
                start = int(query["from"][0])
                end = int(query["to"][0])
                status = 200
                if coin.startswith("locked"):
                    status = 401
                    encoded = json.dumps({"status": {"error_code": 10012, "error_message":
                        "Your request exceeds the allowed time range."}},
                        separators=(",", ":")).encode()
                elif coin == "failing-coin":
                    status = 503
                    encoded = b"provider unavailable"
                elif coin == "oversize-coin":
                    # A valid decade body padded past the native 512 KiB bound.
                    body = provider.model.body(start, end)
                    pad = NATIVE_PROVIDER_BODY_LIMIT + 4096 - len(body)
                    encoded = body[:-1] + (',"pad":"%s"}' % ("x" * pad)).encode()
                elif coin == "toomany-coin":
                    # Under 512 KiB and under the token bound (no market caps),
                    # but more points than the native 3,700-point workspace.
                    encoded = provider.model.body(start, end, points=NATIVE_MAX_POINTS + 100,
                                                  with_caps=False)
                    assert len(encoded) < NATIVE_PROVIDER_BODY_LIMIT, len(encoded)
                else:
                    encoded = provider.model.body(start, end)
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(encoded)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(encoded)
                self.close_connection = True

            def log_message(self, *_args):
                pass

        class QuietDisconnectServer(ThreadingHTTPServer):
            def handle_error(self, request, client_address):
                # The native client deliberately closes an oversized body
                # after its byte cap; the stub may still be writing it.
                if sys.exc_info()[0] in (BrokenPipeError, ConnectionResetError):
                    return
                super().handle_error(request, client_address)

        self.server = QuietDisconnectServer(("127.0.0.1", free_port()), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certfile=str(cert), keyfile=str(key))
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def env(self):
        return {"FORTIS_ENABLE_TEST_PRICE_HISTORY_ROUTE": "1",
                "FORTIS_TEST_HISTORY_UPSTREAM_PORT": str(self.server.server_address[1]),
                "FORTIS_TEST_HISTORY_UPSTREAM_CA": str(self.ca),
                "FORTIS_TEST_HISTORY_DECADE_SEED": "1"}

    def take(self):
        with self.lock:
            calls, self.calls = self.calls, []
        return sorted(calls)


def send(port, spec, timeout=40):
    method = spec["method"]
    lines = ["%s %s HTTP/1.1" % (method, spec["url"]), "Host: localhost", "Connection: close",
             "Accept: " + spec["accept"], "Authorization: Bearer " + TOKEN]
    body = None
    if "hex" in spec:
        body = bytes.fromhex(spec["hex"])
    elif "text" in spec:
        body = spec["text"].encode("utf-8")
    if body is not None:
        lines.append("Content-Type: " + spec["contentType"])
        lines.append("Content-Length: %d" % len(body))
    payload = ("\r\n".join(lines) + "\r\n\r\n").encode() + (body or b"")
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall(payload)
        data = bytearray()
        while True:
            chunk = sock.recv(65536)
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
                "FORTIS_TEST_HISTORY_DECADE_SEED"):
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
    raise RuntimeError("history decade test server did not start")


def stop(process):
    process.send_signal(signal.SIGTERM)
    process.wait(timeout=15)
    if process.returncode != 0 and sys.exc_info()[0] is None:
        raise AssertionError(process.stderr.read().decode())


def summarize(value):
    """The TypeScript fixture's summary of a decoded body (JSON bodies only)."""
    if isinstance(value, list):
        if len(value) <= 6:
            return [summarize(item) for item in value]
        return {"length": len(value), "head": [summarize(item) for item in value[:2]],
                "tail": [summarize(item) for item in value[-2:]]}
    if isinstance(value, dict):
        return {key: summarize(item) for key, item in value.items()}
    return value


def check_case(name, case, actual, calls):
    status, headers, body = actual
    expected = case["body"]
    detail = None
    if status == 200 and not headers.get("content-type", "").startswith("application/protobuf"):
        try:
            detail = summarize(json.loads(body))
        except ValueError:
            detail = body[:400]
    assert status == case["status"], (name, status, case["status"], detail or body[:400])
    for key, value in case["headers"].items():
        assert headers.get(key) == value, (name, key, headers.get(key), value)
    for key in ("x-rate-limit-cost", "retry-after", "vary"):
        assert (key in headers) == (key in case["headers"]), (name, key, headers)
    if "wireHex" in expected:
        assert body.hex() == expected["wireHex"], (name, body.hex(), expected["wireHex"])
    elif "text" in expected:
        assert body == expected["text"].encode("utf-8"), (
            name, body.decode("utf-8", "replace"), expected["text"])
    assert len(body) == expected["bytes"], (name, len(body), expected["bytes"], detail,
                                            expected.get("summary"))
    assert hashlib.sha256(body).hexdigest() == expected["sha256"], (
        name, detail, expected.get("summary"))
    assert calls == case["providerCalls"], (name, calls, case["providerCalls"])


def wait_for_quota_window(seconds_needed):
    # Quota counters are per wall-clock minute; keep a segment inside one.
    left = 60 - time.time() % 60
    if left < seconds_needed:
        time.sleep(left + .2)


def segment_directory(root, segment):
    return root / ("restart" if segment == "restartAfter" else segment)


def replay(binary, fixture, root, provider):
    segments = []
    for name in fixture["order"]:
        segment = fixture["cases"][name]["segment"]
        if not segments or segments[-1][0] != segment:
            segments.append((segment, []))
        segments[-1][1].append(name)
    replayed = 0
    for segment, names in segments:
        wait_for_quota_window(30)
        process, port = start(binary, segment_directory(root, segment), provider.env())
        try:
            provider.take()
            for name in names:
                case = fixture["cases"][name]
                try:
                    actual = send(port, case["request"])
                    check_case(name, case, actual, provider.take())
                except Exception as error:
                    raise AssertionError("failed case %s: %s" % (name, error)) from error
                replayed += 1
        finally:
            stop(process)
    return replayed


def get_json(port, url):
    status, headers, body = send(port, {"method": "GET", "url": url,
                                        "accept": "application/json"})
    return status, headers, json.loads(body)


def native_only(binary, root, provider, source):
    first, decade_from = source["first"], source["decadeFrom"]
    # The cold decade body really exercises the raised bounds: over the old
    # 256 KiB body limit and the old 32,768-token parser arena.
    cold = provider.model.body(decade_from, first)
    assert len(cold) > 256 * 1024, len(cold)
    points = (cold.count(b"[") - 3) // 3
    assert points == 3661 and 7 + 9 * points > 32768, points

    def decade(coin):
        return ("/v1/prices/history?id=%s&from=%d&to=%d&interval=1d" %
                (coin, decade_from, first))

    # Response-size policy: four full decade IDs cannot fit the 1 MiB
    # worst case, so the batch is refused before any provider work.
    process, port = start(binary, root / "native-size", provider.env())
    try:
        provider.take()
        body = json.dumps({"ids": ["size-a", "size-b", "size-c", "size-d"],
                           "from": decade_from, "to": first, "interval": "1d"},
                          separators=(",", ":"))
        status, headers, response = send(port, {"method": "POST", "url": "/v1/prices/history",
                                                "accept": "application/json",
                                                "contentType": "application/json",
                                                "text": body})
        assert status == 503 and response == b'{"error":"UPSTREAM_UNAVAILABLE"}', (status, response)
        assert headers["x-rate-limit-cost"] == "4"
        assert provider.take() == []
    finally:
        stop(process)

    # Provider bodies over the native bounds fail the gap like any provider
    # error: TypeScript would store them, native answers the cached points
    # (here none) with cache.hit true and marks no coverage, so the next
    # request asks the provider again. One process per coin keeps each within
    # the CoinGecko lane burst.
    for coin in ("oversize-coin", "toomany-coin"):
        process, port = start(binary, root / ("native-" + coin), provider.env())
        try:
            provider.take()
            for attempt in range(2):
                status, _, history = get_json(port, decade(coin))
                assert status == 200, (coin, attempt, status, history)
                assert history["points"] == [] and history["volumes"] == [], (coin, history)
                assert history["cache"] == {"hit": True, "timestamp": None}, (coin, history)
                assert history["coverage"] == {"cachedPoints": 0, "fetchedPoints": 0}
                assert history["availability"]["resolutions"] == [], (coin, history)
                # kTooLarge and an unparsable body are not retried.
                calls = provider.take()
                assert len(calls) == 1 and ("/coins/%s/" % coin) in calls[0], (coin, calls)
        finally:
            stop(process)

    # A swallowed failure on a seeded coin serves the cached year (cache.hit
    # true, timestamp of the newest cached point) and stays uncovered.
    process, port = start(binary, root / "native-locked", provider.env())
    try:
        provider.take()
        status, _, history = get_json(port, decade("locked-coin"))
        assert status == 200 and len(history["points"]) == 365, (status, history.get("points"))
        assert history["cache"]["hit"] is True and history["cache"]["timestamp"] is not None
        assert history["coverage"] == {"cachedPoints": 365, "fetchedPoints": 0}
        assert len(provider.take()) == 1
    finally:
        stop(process)


def main(binary):
    fixture = json.loads(FIXTURE.read_text())
    source = fixture["source"]
    with tempfile.TemporaryDirectory(prefix="history-decade-") as temporary:
        root = Path(temporary)
        provider = Provider(root, Model(source["model"]))
        try:
            replayed = replay(binary, fixture, root, provider)
            native_only(binary, root, provider, source)
        finally:
            provider.server.shutdown()
            provider.server.server_close()
    print("native ten-year daily history replayed %d frozen TypeScript cases; size policy "
          "and over-bound provider bodies passed" % replayed)


if __name__ == "__main__":
    main(sys.argv[1])
