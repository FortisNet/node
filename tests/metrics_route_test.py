#!/usr/bin/env python3
"""GET/HEAD /metrics route contract (src/app/main.cpp handle_metrics).

Runs the host binary over real sockets: disabled 404, the exact bearer
policy and its 401 challenge, the 200 headers and exposition shape, HEAD
bodylessness, query handling, scrapes never counting themselves, bounded
private labels, and the 512-byte token bound.
"""

import os
import re
import socket
import subprocess
import sys
import tempfile
import time

BINARY = sys.argv[1] if len(sys.argv) > 1 else "build/fortis_node"
TOKEN = "Route-Metrics_Token.9"
UNAUTHORIZED = b'{"error":"UNAUTHORIZED"}'
CHALLENGE = 'Bearer realm="fortis-metrics"'
TEXT = "text/plain; version=0.0.4; charset=utf-8"
SAMPLE = re.compile(r'^(fortis_[a-z_]+)(\{route="([^"\\\n]*)"\})? ([0-9]+(\.[0-9]+)?)$')
LABEL = re.compile(r"^(other|(GET|HEAD|POST) /[A-Za-z0-9_./:-]*)$")


def exchange(port, raw):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as client:
        client.sendall(raw)
        data = bytearray()
        while chunk := client.recv(65536):
            data.extend(chunk)
    head, body = bytes(data).split(b"\r\n\r\n", 1)
    lines = head.decode("ascii").split("\r\n")
    status = int(lines[0].split()[1])
    headers = {}
    for line in lines[1:]:
        name, value = line.split(": ", 1)
        assert name.lower() not in headers, ("repeated header", name)
        headers[name.lower()] = value
    return status, headers, body


def request(port, method="GET", target="/metrics", headers=()):
    raw = f"{method} {target} HTTP/1.1\r\nHost: localhost\r\n"
    for header in headers:
        raw += header + "\r\n"
    return exchange(port, (raw + "\r\n").encode("latin-1"))


def bearer(value):
    return "Authorization: " + value


class Server:
    def __init__(self, token=None):
        self.kv = tempfile.TemporaryDirectory(prefix="fortis-metrics-route-kv-")
        env = os.environ.copy()
        for name in ("PORT", "NODE_ENV", "ATTESTATION_MODE", "DEVICE_TOKEN_TTL_SECONDS",
                     "FORTIS_TLS_PROBE_HOST", "FORTIS_TLS_PROBE_PORT", "FORTIS_TLS_PROBE_CA",
                     "METRICS_TOKEN"):
            env.pop(name, None)
        env["FORTIS_KV_DIR"] = self.kv.name
        if token is not None:
            env["METRICS_TOKEN"] = token
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            self.port = probe.getsockname()[1]
        self.process = subprocess.Popen([BINARY, str(self.port)], stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, env=env)
        for _ in range(250):
            if self.process.poll() is not None:
                raise AssertionError("server exited before listening")
            try:
                if request(self.port, target="/healthz")[0] == 200:
                    return
            except OSError:
                time.sleep(0.02)
        raise AssertionError("server did not listen")

    def stop(self):
        self.process.terminate()
        _, stderr = self.process.communicate(timeout=5)
        self.kv.cleanup()
        return stderr

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()


def assert_unauthorized(port, method, headers=(), target="/metrics", secrets=()):
    status, response_headers, body = request(port, method, target, headers)
    assert status == 401, (method, target, headers, status)
    assert response_headers["www-authenticate"] == CHALLENGE
    assert response_headers["content-type"] == "application/json; charset=utf-8"
    assert response_headers["content-length"] == str(len(UNAUTHORIZED))
    assert response_headers["cache-control"] == "no-store"
    assert body == (b"" if method == "HEAD" else UNAUTHORIZED), body
    for secret in (TOKEN,) + tuple(secrets):
        assert secret.encode() not in body
        assert all(secret not in value for value in response_headers.values())


def samples(text):
    """Parses the exposition, asserting every line is HELP/TYPE or a sample."""
    assert text.endswith("\n")
    parsed = {}
    for line in text[:-1].split("\n"):
        if line.startswith("# HELP fortis_") or line.startswith("# TYPE fortis_"):
            continue
        match = SAMPLE.match(line)
        assert match, ("malformed exposition line", line)
        label = match.group(3)
        if label is not None:
            assert LABEL.match(label), ("unbounded route label", label)
        key = (match.group(1), label)
        assert key not in parsed, ("duplicate series", key)
        parsed[key] = match.group(4)
    return parsed


def route_series(parsed):
    return {key: value for key, value in parsed.items() if key[1] is not None}


def scrape(port, target="/metrics"):
    status, headers, body = request(port, target=target, headers=[bearer("Bearer " + TOKEN)])
    assert status == 200, (target, status, body[:200])
    return headers, body


def test_disabled():
    for token in (None, "", " \t\r\n "):
        with Server(token) as server:
            for method in ("GET", "HEAD"):
                for headers in ((), (bearer("Bearer " + TOKEN),), (bearer("Bearer "),)):
                    status, response_headers, body = request(server.port, method, headers=headers)
                    assert status == 404 and body == b"", (token, method, headers, status)
                    assert "content-type" not in response_headers
                    assert response_headers["content-length"] == "0"
                    assert "www-authenticate" not in response_headers
            # A disabled scrape is not a route request either.
            status, _, body = request(server.port, target="/statusz")
            assert status == 200 and b"/metrics" not in body


def test_enabled():
    # Surrounding SP/HTAB/CR/LF in METRICS_TOKEN is trimmed, as zod's trim().
    with Server(" \t" + TOKEN + "\r\n") as server:
        port = server.port
        # Startup readiness polls already hit /healthz; assert deltas from here.
        start = samples(scrape(port)[1].decode("ascii"))

        def delta(parsed, name, label):
            return int(parsed.get((name, label), "0")) - int(start.get((name, label), "0"))

        wrong_last = TOKEN[:-1] + chr(ord(TOKEN[-1]) ^ 1)
        rejected = [
            (),
            (bearer(""),),
            (bearer("Bearer"),),
            (bearer("Bearer "),),
            (bearer("bearer " + TOKEN),),
            (bearer("BEARER " + TOKEN),),
            (bearer("Basic " + TOKEN),),
            (bearer(TOKEN),),
            (bearer("Bearer  " + TOKEN),),
            (bearer("Bearer " + TOKEN.lower()),),
            (bearer("Bearer " + TOKEN[:-1]),),
            (bearer("Bearer " + TOKEN + "x"),),
            (bearer("Bearer x" + TOKEN),),
            (bearer("Bearer " + wrong_last),),
            (bearer("Bearer " + " \t" + TOKEN),),
            (bearer("Bearer " + "A" * 12000),),
            ("X-Metrics-Token: " + TOKEN,),
            ("Cookie: token=" + TOKEN,),
        ]
        for headers in rejected:
            for method in ("GET", "HEAD"):
                assert_unauthorized(port, method, headers, secrets=("A" * 32,))
        # The token in the query string never authenticates, nor is it echoed.
        assert_unauthorized(port, "GET", target="/metrics?token=" + TOKEN)
        assert_unauthorized(port, "GET", (bearer("Bearer wrong"),),
                            target="/metrics?authorization=Bearer%20" + TOKEN)
        # The request parser strips optional whitespace around a field value
        # (Node does too over a real socket), and the field name is
        # case-insensitive.
        for headers in ([bearer("Bearer " + TOKEN + "  ")], ["Authorization:\tBearer " + TOKEN + "\t"],
                        ["AUTHORIZATION: Bearer " + TOKEN]):
            assert request(port, headers=headers)[0] == 200, headers
        # A repeated Authorization header is a malformed request natively
        # (Node keeps the first one): 400 before the route runs, never 200.
        for pair in (["Bearer " + TOKEN, "Bearer wrong"], ["Bearer wrong", "Bearer " + TOKEN]):
            status, _, body = request(port, headers=[bearer(value) for value in pair])
            assert status == 400 and TOKEN.encode() not in body
        # Paths and methods outside the literal route are the generic 404.
        authorized = [bearer("Bearer " + TOKEN)]
        for method, target in (("GET", "/metrics/"), ("GET", "/METRICS"), ("GET", "/metrics.txt"),
                               ("GET", "/%6Detrics"), ("POST", "/metrics"), ("PUT", "/metrics")):
            extra = ["Content-Length: 0"] if method in ("POST", "PUT") else []
            status, _, body = request(port, method, target, authorized + extra)
            assert status == 404 and b"fortis_" not in body, (method, target, status)

        # Traffic the exposition must summarize: matched routes (one with a
        # private-looking query) and unmatched private-looking paths.
        for target in ("/healthz", "/healthz", "/healthz?wallet=0xprivatequery"):
            assert request(port, target=target)[0] == 200
        assert request(port, "HEAD", "/healthz")[0] == 200
        assert request(port, target="/missing/0xprivatepath")[0] == 404

        headers, body = scrape(port)
        assert headers["content-type"] == TEXT
        assert headers["cache-control"] == "no-store"
        assert headers["x-content-type-options"] == "nosniff"
        assert headers["referrer-policy"] == "no-referrer"
        assert headers["content-length"] == str(len(body))
        for absent in ("www-authenticate", "etag", "vary", "set-cookie"):
            assert absent not in headers
        assert 0 < len(body) <= 64 * 1024
        text = body.decode("ascii")
        parsed = samples(text)
        for secret in (TOKEN, "0xprivatequery", "0xprivatepath", "wallet", "?"):
            assert secret not in text, secret
        assert delta(parsed, "fortis_native_http_requests_total", "GET /healthz") == 3
        assert delta(parsed, "fortis_native_http_requests_total", "HEAD /healthz") == 1
        assert parsed[("fortis_native_http_errors_total", "GET /healthz")] == "0"
        # other: /missing, the six 404 path/method variants and the two 400s.
        assert delta(parsed, "fortis_native_http_requests_total", "other") == 9
        assert parsed[("fortis_native_http_errors_total", "other")] == "0"
        for _, label in parsed:
            assert label is None or "/metrics" not in label
        # The open process store adds label-free compaction gauges; host
        # builds have no allocator gauges.
        assert ("fortis_kv_compaction_phase", None) in parsed
        assert not any(name.startswith("fortis_guest_allocator") for name, _ in parsed)

        # Scrapes, 401s, HEADs and query variants do not move a route counter.
        baseline = route_series(parsed)
        assert_unauthorized(port, "GET")
        assert_unauthorized(port, "HEAD", (bearer("Bearer wrong"),))
        _, query_body = scrape(port, "/metrics?format=json&name[]=x&token=" + TOKEN)
        assert TOKEN.encode() not in query_body
        assert route_series(samples(query_body.decode("ascii"))) == baseline
        before_head = len(scrape(port)[1])
        status, head_headers, head_body = request(port, "HEAD", headers=authorized)
        after_head = len(scrape(port)[1])
        assert status == 200 and head_body == b""
        assert head_headers["content-type"] == TEXT and head_headers["cache-control"] == "no-store"
        # HEAD advertises the GET body's length; only the KV lock-hold gauge
        # may grow between the surrounding scrapes.
        assert before_head <= int(head_headers["content-length"]) <= after_head
        assert route_series(samples(scrape(port)[1].decode("ascii"))) == baseline

        # The scrape still counts later traffic.
        assert request(port, target="/healthz")[0] == 200
        later = samples(scrape(port)[1].decode("ascii"))
        assert delta(later, "fortis_native_http_requests_total", "GET /healthz") == 4


def test_maximum_token():
    token = "".join(chr(0x21 + (i * 7) % 94) for i in range(512)).replace('"', "#").replace("\\", "#")
    with Server(token) as server:
        assert request(server.port, headers=[bearer("Bearer " + token)])[0] == 200
        last = token[:-1] + ("A" if token[-1] != "A" else "B")
        for value in ("Bearer " + token[:-1], "Bearer " + token + "A", "Bearer " + last):
            status, _, body = request(server.port, headers=[bearer(value)])
            assert status == 401 and token.encode() not in body
    # One byte over the bound refuses to start and does not print the token.
    env = os.environ.copy()
    env.pop("METRICS_TOKEN", None)
    env["METRICS_TOKEN"] = "s" * 200 + "Q" * 313
    result = subprocess.run([BINARY, "0"], env=env, capture_output=True, timeout=10)
    assert result.returncode != 0
    assert b"METRICS_TOKEN is too long" in result.stderr
    assert b"Q" * 32 not in result.stderr + result.stdout


def main():
    test_disabled()
    test_enabled()
    test_maximum_token()
    print("GET/HEAD /metrics route test passed")


if __name__ == "__main__":
    main()
