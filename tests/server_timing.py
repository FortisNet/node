"""Shared Server-Timing assertions for the native routers.

Every response head the public router and the support staff bridge send
carries exactly one `Server-Timing: app;dur=<ms>` line (src/http/http_server.cpp,
kServerTimingLineBytes): milliseconds from HttpRouter::Dispatch entry until
the head is formatted, printed as digits, '.', three digits. These helpers
parse raw response bytes (never a header dict, which would hide a duplicate)
and are used by the host test (server_timing_native_test.py) and by guest
checks against the same case list.
"""

import re
import socket

TIMING = re.compile(r"app;dur=(\d+)\.(\d{3})")
ASYNC_BODY = b"async-ready"


class Response:
    def __init__(self, status, headers, body, duration_ms):
        self.status = status
        self.headers = headers
        self.body = body
        self.duration_ms = duration_ms


def exchange(host, port, raw, timeout=30):
    """Sends `raw` and reads until the server closes."""
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.sendall(raw)
        data = bytearray()
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                return bytes(data)
            data.extend(chunk)


def build(method, path, headers=None, body=b"", host="127.0.0.1"):
    lines = [f"{method} {path} HTTP/1.1", f"Host: {host}", "Connection: close"]
    for key, value in (headers or {}).items():
        lines.append(f"{key}: {value}")
    if body or method == "POST":
        lines.append(f"Content-Length: {len(body)}")
    return ("\r\n".join(lines) + "\r\n\r\n").encode("latin1") + body


def parse(raw, head=False, label=""):
    """Parses one response and asserts its single, well-formed timing line,
    an unchanged Content-Length and (for HEAD) an empty body."""
    assert b"\r\n\r\n" in raw, f"{label}: no complete response head ({raw[:80]!r})"
    head_bytes, _, body = raw.partition(b"\r\n\r\n")
    first, *lines = head_bytes.decode("latin1").split("\r\n")
    status = int(first.split()[1])
    headers = {}
    timings = []
    for line in lines:
        key, sep, value = line.partition(":")
        assert sep, f"{label}: malformed header line {line!r}"
        name = key.strip().lower()
        headers.setdefault(name, []).append(value.strip())
        if name == "server-timing":
            timings.append(value.strip())
    assert len(timings) == 1, f"{label}: expected one Server-Timing, got {timings}"
    match = TIMING.fullmatch(timings[0])
    assert match, f"{label}: Server-Timing {timings[0]!r} is not app;dur=<ms>"
    duration = int(match.group(1)) + int(match.group(2)) / 1000
    assert len(headers.get("content-length", [])) <= 1, f"{label}: duplicate Content-Length"
    if head:
        assert body == b"", f"{label}: HEAD response has {len(body)} body bytes"
    elif "content-length" in headers:
        assert len(body) == int(headers["content-length"][0]), \
            f"{label}: body {len(body)} bytes != Content-Length {headers['content-length'][0]}"
    return Response(status, headers, body, duration)


def call(host, port, method, path, headers=None, body=b"", label=None, raw=None, timeout=30):
    label = label or f"{method} {path}"
    data = exchange(host, port, raw if raw is not None else build(method, path, headers, body),
                    timeout=timeout)
    return parse(data, head=method == "HEAD", label=label)


def expect(response, status, label):
    assert response.status == status, f"{label}: status {response.status} != {status}"
    return response


def public_checks(host, port, metrics_token=None, async_probe=True, log=print):
    """The public router: success, HEAD, auth and validation refusals,
    parser errors, unknown routes, deferred and reserved async responses,
    /metrics. Returns {label: duration_ms}."""
    samples = {}

    def run(label, method, path, status, **kwargs):
        response = expect(call(host, port, method, path, label=label, **kwargs), status, label)
        samples[label] = response.duration_ms
        return response

    ok = run("GET /healthz", "GET", "/healthz", 200)
    assert ok.body == b'{"ok":true}', ok.body
    head = run("HEAD /healthz", "HEAD", "/healthz", 200)
    assert head.headers["content-length"] == ok.headers["content-length"]
    # Existing standard headers are kept alongside the new one.
    for name in ("cache-control", "x-content-type-options", "referrer-policy", "permissions-policy"):
        assert name in ok.headers, name
    run("unknown route 404", "GET", "/timing-unknown-route", 404)
    run("HEAD unknown route 404", "HEAD", "/timing-unknown-route", 404)
    run("wrong method 404", "DELETE", "/healthz", 404)
    run("malformed request 400", None, None, 400, raw=b"BROKEN\r\n\r\n")
    run("HEAD missing Host 400", "HEAD", None, 400, raw=b"HEAD /healthz HTTP/1.1\r\n\r\n")
    # Exactly kMaxHeaders bytes without a terminator: 431 with nothing left
    # unread, so the close cannot reset the connection before the answer.
    prefix = b"GET /healthz HTTP/1.1\r\nHost: x\r\nX-Fill: "
    run("oversized headers 431", None, None, 431, raw=prefix + b"a" * (16 * 1024 - len(prefix)))
    run("oversized body 413", None, None, 413,
        raw=b"POST /v1/devices HTTP/1.1\r\nHost: x\r\nContent-Length: 999999999\r\n\r\n")
    unauthorized = run("networks without bearer 401", "GET", "/v1/networks", 401)
    assert unauthorized.body, "401 keeps its body"
    run("enrollment validation 400", "POST", "/v1/devices",
        400, headers={"Content-Type": "application/json"}, body=b"{}")
    if metrics_token:
        run("metrics without token 401", "GET", "/metrics", 401)
        metrics = run("GET /metrics", "GET", "/metrics", 200,
                      headers={"Authorization": f"Bearer {metrics_token}"})
        # Route metrics still count the requests above.
        assert b'fortis_native_http_requests_total{route="GET /healthz"}' in metrics.body
        assert b'fortis_native_http_requests_total{route="other"}' in metrics.body
    if async_probe:
        fast = run("deferred 20 ms", "GET", "/probe/async?delay_ms=20", 200)
        slow = run("deferred 400 ms", "GET", "/probe/async?delay_ms=400", 200)
        assert fast.body == ASYNC_BODY and slow.body == ASYNC_BODY
        # The handoff schedules whole milliseconds: allow 1 ms truncation.
        assert slow.duration_ms >= 399, slow.duration_ms
        assert slow.duration_ms - fast.duration_ms >= 300, (fast.duration_ms, slow.duration_ms)
        deferred_head = run("HEAD deferred 200 ms", "HEAD", "/probe/async?delay_ms=200", 200)
        assert deferred_head.headers["content-length"] == [str(len(ASYNC_BODY))]
        assert deferred_head.duration_ms >= 199, deferred_head.duration_ms
        timeout = run("reserved 504 after 300 ms", "GET", "/probe/async-timeout?timeout_ms=300", 504)
        assert timeout.duration_ms >= 299, timeout.duration_ms
        reserved_head = run("HEAD reserved 504 after 200 ms", "HEAD",
                            "/probe/async-timeout?timeout_ms=200", 504)
        assert reserved_head.duration_ms >= 199, reserved_head.duration_ms
    log("public router Server-Timing: " + ", ".join(f"{k}={v:.3f}" for k, v in samples.items()))
    return samples


def bridge_checks(host, port, token, log=print):
    """The staff bridge listener: authorized list (GET and HEAD), the
    credential and browser refusals, an unknown route, a malformed id and a
    non-JSON write. Returns {label: duration_ms}."""
    samples = {}
    tickets = "/internal/support/v1/tickets"
    auth = {"Authorization": f"Bearer {token}"}

    def run(label, method, path, status, **kwargs):
        response = expect(call(host, port, method, path, label=label, **kwargs), status, label)
        samples[label] = response.duration_ms
        return response

    listed = run("bridge list", "GET", tickets, 200, headers=auth)
    assert b'"tickets"' in listed.body, listed.body[:120]
    head = run("bridge HEAD list", "HEAD", tickets, 200, headers=auth)
    assert head.headers["content-length"] == listed.headers["content-length"]
    run("bridge without token 401", "GET", tickets, 401)
    run("bridge wrong token 401", "GET", tickets, 401, headers={"Authorization": "Bearer wrong"})
    run("bridge browser 403", "GET", tickets, 403, headers={**auth, "Origin": "https://evil.example"})
    run("bridge unknown route 404", "GET", "/internal/support/v1/unknown", 404, headers=auth)
    run("bridge malformed id 404", "GET", f"{tickets}/bad-id!", 404, headers=auth)
    run("bridge non-JSON write 415", "POST", f"{tickets}/01JZ0000000000000000000000/reply", 415,
        headers={**auth, "Content-Type": "text/plain"}, body=b"hello")
    log("staff bridge Server-Timing: " + ", ".join(f"{k}={v:.3f}" for k, v in samples.items()))
    return samples
