#!/usr/bin/env python3
"""Offline checks for tests/live_price_canary.py result classification.

Usage: live_price_canary_classification_test.py

No network and no binary: synthetic native route replies and raw provider
responses drive the pure classifiers, the native framing mirror and the
verdict table. Also checks that the live probe itself is wired into no
make target or launcher script.
"""

import json
from pathlib import Path
import re

import live_price_canary as probe

ROOT = Path(__file__).resolve().parents[1]
JSON = {"content-type": "application/json; charset=utf-8"}


def raw(status_line, headers, body=b""):
    head = status_line + "\r\n" + "".join(f"{k}: {v}\r\n" for k, v in headers) + "\r\n"
    return head.encode("latin1") + body


def control(route, data, **extra):
    return probe.classify_control(route, {"error": None, "raw": data, "truncated": False,
                                          **extra})


def check_native_classification():
    spot_live = {"fiat": "usd", "prices": {"bitcoin": {
        "sourceId": "bitcoin", "price": 97000.5, "change24hPct": 0.8, "stale": False,
        "source": "coingecko", "cache": {"hit": False, "timestamp": None}}}, "missing": []}
    cases = [
        ("spot", 200, spot_live, probe.COMPATIBLE),
        ("spot", 200, {**spot_live, "prices": {"bitcoin": {
            **spot_live["prices"]["bitcoin"], "stale": True}}}, "ambiguous"),
        ("spot", 200, {"fiat": "usd", "prices": {}, "missing": ["bitcoin"]}, "native_no_data"),
        ("spot", 504, {"error": "TIMEOUT"}, "native_timeout"),
        ("spot", 503, {"error": "UNAVAILABLE"}, "route_error"),
        ("spot", 429, {"error": "RATE_LIMITED"}, "local_quota"),
        ("spot", 401, {"error": "UNAUTHORIZED"}, "local_auth"),
        ("spot", 404, None, "route_absent"),
        ("history", 200, {"id": "bitcoin", "interval": "1d",
                          "points": [[1767225600, 97000.5], [1767312000, 97100]],
                          "cache": {"hit": False, "timestamp": None},
                          "coverage": {"cachedPoints": 0, "fetchedPoints": 2}},
         probe.COMPATIBLE),
        ("history", 200, {"id": "bitcoin", "interval": "1d", "points": [],
                          "cache": {"hit": True, "timestamp": None},
                          "coverage": {"cachedPoints": 0, "fetchedPoints": 0}},
         "native_no_data"),
        ("history", 200, {"points": [[1, 2]], "cache": {"hit": True},
                          "coverage": {"fetchedPoints": 0}}, "ambiguous"),
        ("history", 503, {"error": "UPSTREAM_UNAVAILABLE"}, "route_error"),
        ("history", 400, {"error": "BAD_REQUEST", "detail": "Invalid input"},
         "local_validation"),
        ("ohlc", 200, {"points": [[1767225600000, 1, 2, 0.5, 1.5]],
                       "cache": {"hit": False, "timestamp": None}}, probe.COMPATIBLE),
        ("ohlc", 200, {"points": [[1767225600000, 1, 2, 0.5]],
                       "cache": {"hit": False, "timestamp": None}}, "ambiguous"),
        ("ohlc", 502, {"error": "UPSTREAM_UNAVAILABLE"}, "native_no_data"),
        ("ohlc", 503, {"error": "UPSTREAM_UNAVAILABLE"}, "route_error"),
    ]
    for route, status, payload, expected in cases:
        body = json.dumps(payload).encode() if payload is not None else b""
        category, detail = probe.classify_native(route, status, JSON, body)
        assert category == expected, (route, status, payload, category, detail)
    category, _ = probe.classify_native("spot", 200, JSON, b"{not json")
    assert category == "ambiguous"
    # A server error code is bounded; a body never leaks into the detail.
    _, detail = probe.classify_native("spot", 500, JSON,
                                      json.dumps({"error": "X" * 500}).encode())
    assert len(detail) < 160 and "X" * 40 not in detail, detail


def check_framing_mirror():
    body = b'{"bitcoin":{"usd":97000.5}}'
    ok = probe.parse_http_response(raw("HTTP/1.1 200 OK", [
        ("Content-Type", "application/json"), ("Content-Length", len(body))], body))
    assert ok["framing"] is None and ok["status"] == 200 and ok["body"] == body, ok
    chunked = probe.parse_http_response(raw("HTTP/1.1 200 OK", [
        ("Transfer-Encoding", "chunked")], b"5\r\n{\"bit\r\n0\r\n\r\n"))
    assert chunked["framing"] is None and chunked["body"] == b'{"bit', chunked
    closed = probe.parse_http_response(raw("HTTP/1.1 200 OK", [("Connection", "close")], body))
    assert closed["framing"] is None and closed["body"] == body
    http10 = probe.parse_http_response(raw("HTTP/1.0 200 OK", [], body))
    assert http10["framing"] is None and http10["body"] == body
    no_reason = probe.parse_http_response(raw("HTTP/1.1 204", []))
    assert no_reason["framing"] is None and no_reason["status"] == 204
    rejected = [
        (raw("HTTP/1.1 200 OK", [("Content-Encoding", "gzip"), ("Content-Length", 2)], b"{}"),
         "content_encoding=gzip"),
        (raw("HTTP/1.1 200 OK", [("Transfer-Encoding", "gzip, chunked")]),
         "transfer_encoding=gzip, chunked"),
        (raw("HTTP/1.1 200 OK", [("Content-Length", 2), ("Transfer-Encoding", "chunked")],
             b"{}"), "content_length_and_chunked"),
        (raw("HTTP/1.1 200 OK", [("Content-Length", 2), ("Content-Length", 3)], b"{}"),
         "conflicting_content_length"),
        (raw("HTTP/1.1 200 OK", [("Connection", "keep-alive")], body),
         "no_length_and_not_close_delimited"),
        (raw("HTTP/1.1 200 OK", [("Content-Length", 50)], body), "truncated_body"),
        (raw("HTTP/1.1 100 Continue", []), "informational_response"),
        (raw("HTTP/2 200", []), "status_line"),
        (raw("HTTP/1.1 200 OK", [("X-Big", "a" * probe.NATIVE_HEADER_CAP)]),
         "header_too_large"),
        (b"HTTP/1.1 200 OK\r\nX-A: 1\nX-B: 2\r\nContent-Length: 0\r\n\r\n", "bare_cr_or_lf"),
        (raw("HTTP/1.1 200 OK", [("Transfer-Encoding", "chunked")], b"zz\r\n"),
         "chunk_size_line"),
        (raw("HTTP/1.1 200 OK", [("Transfer-Encoding", "chunked")],
             b"0\r\nContent-Length: 1\r\n\r\n"), "forbidden_trailer"),
        (b"HTTP/1.1 200 OK\r\n" + b"X: y\r\n" * 4000, "header_too_large"),
    ]
    for data, reason in rejected:
        parsed = probe.parse_http_response(data)
        assert parsed["framing"] == reason, (reason, parsed["framing"])


def check_control_classification():
    spot = b'{"bitcoin":{"usd":97000.5,"usd_24h_change":0.8}}'
    history = json.dumps({"prices": [[1767225600000, 97000.5]], "market_caps": [],
                          "total_volumes": []}).encode()
    ohlc = b"[[1767225600000,1,2,0.5,1.5]]"
    for route, body in (("spot", spot), ("history", history), ("ohlc", ohlc)):
        category, _, size = control(route, raw("HTTP/1.1 200 OK", [
            ("Content-Length", len(body))], body))
        assert category == "provider_ok" and size == len(body), (route, category)
        category, _, _ = control(route, raw("HTTP/1.1 200 OK", [
            ("Content-Length", 2)], b"{}"))
        assert category == "provider_payload_shape", (route, category)
    category, detail, _ = control("spot", raw("HTTP/1.1 429 Too Many Requests", [
        ("Retry-After", "60"), ("Content-Length", 0)]))
    assert category == "provider_rate_limited" and "retry-after=60" in detail, detail
    # A rate limit outranks a framing the native client would also reject.
    category, _, _ = control("spot", raw("HTTP/1.1 429 Too Many Requests", [
        ("Content-Encoding", "gzip"), ("Content-Length", 0)]))
    assert category == "provider_rate_limited", category
    category, detail, _ = control("ohlc", raw("HTTP/1.1 403 Forbidden", [
        ("Server", "cloudflare"), ("cf-mitigated", "challenge"), ("Content-Length", 0)]))
    assert category == "provider_rejected" and "cf-mitigated" in detail, detail
    assert control("spot", raw("HTTP/1.1 401 Unauthorized", [("Content-Length", 0)]))[0] \
        == "provider_rejected"
    category, detail, _ = control("spot", raw("HTTP/1.1 301 Moved", [
        ("Location", "https://www.coingecko.com/secret?x=1"), ("Content-Length", 0)]))
    assert category == "provider_redirect" and "www.coingecko.com" in detail, detail
    assert "secret" not in detail
    assert control("spot", raw("HTTP/1.1 502 Bad Gateway", [("Content-Length", 0)]))[0] \
        == "provider_error"
    assert control("spot", raw("HTTP/1.1 200 OK", [("Content-Encoding", "br"),
                                                   ("Content-Length", 2)], b"{}"))[0] \
        == "framing_unsupported"
    big = b"[" + b",".join([b"[1,1,1,1,1]"] * 30000) + b"]"
    assert len(big) > probe.NATIVE_BODY_CAPS["ohlc"]
    category, _, size = control("ohlc", raw("HTTP/1.1 200 OK", [
        ("Content-Length", len(big))], big))
    assert category == "body_cap" and size == len(big), category
    assert control("history", b"", truncated=True)[0] == "body_cap"
    for error, expected in (("tls_verify", "ca_bundle_mismatch"),
                            ("private_address", "private_address"),
                            ("resolve", "control_network"), ("connect", "control_network"),
                            ("timeout", "control_network"), ("tls", "control_network")):
        assert probe.classify_control("spot", {"error": error, "raw": b"",
                                               "truncated": False})[0] == expected, error


def check_verdicts():
    table = [
        (probe.COMPATIBLE, 0.5, None, "COMPATIBLE"),
        (probe.COMPATIBLE, 0.5, "provider_rate_limited", "COMPATIBLE"),
        ("local_quota", 0.1, None, "HARNESS_ERROR"),
        ("route_absent", 0.1, "provider_ok", "HARNESS_ERROR"),
        ("skipped", None, None, "UNKNOWN"),
        ("ambiguous", 0.5, "provider_ok", "UNKNOWN"),
        ("route_error", 0.5, "provider_rate_limited", "UNKNOWN"),
        ("native_no_data", 0.5, None, "UNKNOWN"),
        ("native_no_data", 0.5, "control_network", "UNKNOWN"),
        ("native_no_data", 0.5, "provider_ok", "NATIVE_TRANSPORT_OR_PARSER"),
        ("native_no_data", 31.0, "provider_ok", "NATIVE_TIMEOUT"),
        ("native_timeout", 30.1, "provider_ok", "NATIVE_TIMEOUT"),
        ("native_no_data", 0.4, "provider_rate_limited", "PROVIDER_RATE_LIMITED"),
        ("native_no_data", 0.4, "provider_rejected", "PROVIDER_REJECTED"),
        ("native_no_data", 0.4, "provider_error", "PROVIDER_ERROR"),
        ("native_no_data", 0.4, "provider_redirect", "REDIRECT"),
        ("native_no_data", 0.4, "framing_unsupported", "FRAMING"),
        ("native_no_data", 0.4, "body_cap", "BODY_CAP"),
        ("native_no_data", 0.4, "provider_payload_shape", "PARSER"),
        ("native_no_data", 0.4, "ca_bundle_mismatch", "CA_BUNDLE"),
        ("native_no_data", 0.4, "private_address", "PRIVATE_ADDRESS"),
        ("native_no_data", 0.4, "something_new", "UNKNOWN"),
    ]
    for native, elapsed, control_category, expected in table:
        verdict, _ = probe.combine(native, elapsed, control_category)
        assert verdict == expected, (native, elapsed, control_category, verdict)


def check_environment_and_redaction():
    inherited = {"PATH": "/usr/bin", "HOME": "/home/x", "COINGECKO_API_KEY": "cg-secret",
                 "COINGECKO_TIER": "pro", "JUPITER_API_KEY": "j", "TRONSCAN_API_KEY": "t",
                 "HTTPS_PROXY": "http://proxy", "https_proxy": "http://proxy",
                 "FORTIS_TEST_PRICE_UPSTREAM_PORT": "9", "FORTIS_TLS_PROBE_HOST": "h",
                 "NODE_ENV": "production", "PORT": "1", "SSL_CERT_FILE": "/x",
                 "ATTESTATION_MODE": "required", "METRICS_TOKEN": "m", "EDITOR": "vi"}
    env, scrubbed = probe.build_child_env(inherited, Path("/tmp/kv"), Path("/tmp/ca.pem"))
    assert env == {"PATH": "/usr/bin", "HOME": "/home/x", "FORTIS_KV_DIR": "/tmp/kv",
                   probe.CANARY: "1", "FORTIS_CA_BUNDLE_PATH": "/tmp/ca.pem",
                   "ATTESTATION_MODE": "development"}, env
    for name in ("COINGECKO_API_KEY", "HTTPS_PROXY", "https_proxy", "NODE_ENV",
                 "FORTIS_TEST_PRICE_UPSTREAM_PORT", "SSL_CERT_FILE", "METRICS_TOKEN"):
        assert name in scrubbed, name
    assert "EDITOR" not in scrubbed and "cg-secret" not in " ".join(scrubbed)

    token = "fintel_dev_" + "Q" * 43
    assert token not in probe.redact(f"Authorization: Bearer {token}")
    assert "fintel_<redacted>" in probe.redact("fintel_live_abcdefgh123")


def check_plan():
    now = 1767323045
    plan = probe.probe_plan(now, 7)
    assert [route for route, _, _ in plan] == ["spot", "history", "ohlc"]
    to = now // 86400 * 86400
    assert plan[1][1] == f"/v1/prices/history?id=bitcoin&interval=1d&from={to - 7 * 86400}&to={to}"
    assert plan[0][2] == ("/api/v3/simple/price?ids=bitcoin&vs_currencies=usd"
                          "&include_24hr_change=true&include_24hr_vol=true")
    assert plan[2][2] == "/api/v3/coins/bitcoin/ohlc?vs_currency=usd&days=1"


def check_not_wired_into_automation():
    # Matches both the host probe and the guest probe built on it.
    live = re.compile(r"live_price_canary\.py")
    # The guest metrics script hosts the opt-in --live-price mode;
    # that flag must stay out of automation too.
    own = ("live_price_canary.py", "guest_live_price_canary.py",
           "guest_live_price_canary_test.py", "live_price_batch_probe.py",
           "live_price_batch_probe_test.py", "guest_allocator_metrics.py",
           Path(__file__).name)
    for path in [ROOT / "Makefile", *sorted((ROOT / "scripts").glob("*"))]:
        if path.is_file():
            text = path.read_text(errors="replace")
            assert not live.search(text), path
            assert "--live-price" not in text, path
    for path in (ROOT / "tests").glob("*.py"):
        if path.name not in own:
            assert not live.search(path.read_text(errors="replace")), path


def main():
    check_native_classification()
    check_framing_mirror()
    check_control_classification()
    check_verdicts()
    check_environment_and_redaction()
    check_plan()
    check_not_wired_into_automation()
    print("live price canary classification: native, control, framing mirror, verdicts, "
          "environment allowlist, redaction and opt-in wiring passed")


if __name__ == "__main__":
    main()
