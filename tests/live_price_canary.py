#!/usr/bin/env python3
"""Opt-in live-provider compatibility probe for the native price canary.

Usage:
  python3 tests/live_price_canary.py --ca-bundle <public CA bundle> build/fortis_node

Run this only on purpose: it sends real requests to the public, keyless
api.coingecko.com. It is not part of `make test` or the guest checks. Pure
classification is covered offline by live_price_canary_classification_test.py.

The normal binary runs its production Start() on an ephemeral localhost port
with FORTIS_NATIVE_PRICE_ROUTE_CANARY=1, the given CA bundle, a fresh
temporary KV and ATTESTATION_MODE=development. Its environment is built from
an allowlist, so no provider key, tier, proxy, endpoint or test-only override
is inherited. The probe enrolls one development device and sends one GET each:
  1. /v1/prices?ids=bitcoin
  2. /v1/prices/history for bitcoin: interval 1d, a recent bounded window
  3. /v1/prices/ohlc?id=bitcoin&days=1

If a route returns no live data, a direct HTTPS control repeats the provider
request from Python. It uses the same CA bundle, IPv4, TLS 1.2 or later and
the native client's request headers (including its fixed User-Agent), then
applies the native header-size, framing, redirect and body-cap rules. That
separates provider rejection from a
native failure. Without a control, or when either side is ambiguous, the
verdict is UNKNOWN; compatibility is never inferred.

The probe never prints a device token, an API key (none is forwarded) or a
provider or route body. It reports only status, category, elapsed time, sizes
and counts.

Exit status:
  0  all three routes COMPATIBLE and the server exited 0 on SIGTERM
  1  at least one route not proven compatible (provider limits and UNKNOWN too)
  2  setup or harness error (binary, CA bundle, startup, enrollment, or a
     probe request refused by the native server's own auth/quota/validation)
  3  unclean shutdown, or the temporary KV could not be removed
A higher code takes precedence.
"""

import argparse
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import urlsplit

PUBLIC_HOST = "api.coingecko.com"
CANARY = "FORTIS_NATIVE_PRICE_ROUTE_CANARY"
# The child environment is built from this allowlist. Everything else,
# including provider keys and tiers, proxies, endpoint and test-only
# overrides, never reaches the server.
ENV_ALLOWLIST = ("PATH", "HOME", "TMPDIR", "LANG", "LC_ALL", "LC_CTYPE")
# Inherited names reported as scrubbed (names only, never values).
SCRUB_REPORT = re.compile(
    r"API_KEY|TOKEN|SECRET|PROXY|COINGECKO|JUPITER|TRONSCAN|VESTIGE|ROBINHOOD|"
    r"^FORTIS_|^SSL_CERT|^REQUESTS_CA|^CURL_CA|^NODE_|^PORT$|^ATTESTATION_MODE$|"
    r"^DEVICE_TOKEN_TTL", re.IGNORECASE)
TOKEN_PATTERN = re.compile(r"fintel_[A-Za-z0-9_\-]{8,}")

# Mirrors of the native client (src/http/outbound_https.cpp) and the owners.
NATIVE_HEADER_CAP = 16 * 1024                 # kMaxHeaderBytes
NATIVE_BODY_CAPS = {
    "spot": 256 * 1024,                       # kBoundedJsonMaxInputBytes
    "history": 512 * 1024,                    # kPriceHistoryMaxProviderBodyBytes
    "ohlc": 256 * 1024,                       # kProviderExecutionDefaultResponseBody
}
NATIVE_ATTEMPT_TIMEOUT_S = 10.0               # per provider attempt
NATIVE_CA_BUNDLE_MAX = 1024 * 1024            # OutboundHttpsValidateCaBundle
FORBIDDEN_TRAILERS = (b"content-length", b"transfer-encoding", b"host",
                      b"connection", b"content-encoding")

# Probe bounds.
START_TIMEOUT_S = 15.0
ROUTE_TIMEOUT_S = 35.0       # owner deadlines are 25-30 s
CONTROL_TIMEOUT_S = 12.0
SHUTDOWN_TIMEOUT_S = 25.0
SHUTDOWN_RESERVE_S = SHUTDOWN_TIMEOUT_S + 5.0
LOCAL_READ_CAP = 1024 * 1024  # above every native provider cap
STDERR_TAIL_BYTES = 600

COMPATIBLE = "compatible"
# Native categories meaning the request never reached provider work.
HARNESS_CATEGORIES = ("route_absent", "local_auth", "local_quota", "local_validation")
# Native categories that a control may attribute; every other non-compatible
# native outcome stays UNKNOWN whatever the control saw.
ATTRIBUTABLE_CATEGORIES = ("native_no_data", "native_timeout")
CONTROL_VERDICTS = {
    "provider_rejected": ("PROVIDER_REJECTED", "the provider refused the same request (401/403)"),
    "provider_rate_limited": ("PROVIDER_RATE_LIMITED", "the provider rate-limited the same request (429)"),
    "provider_error": ("PROVIDER_ERROR", "the provider answered the same request with an error status"),
    "provider_redirect": ("REDIRECT", "the provider redirected; the native client follows no redirects"),
    "framing_unsupported": ("FRAMING", "the provider used framing the native client rejects"),
    "body_cap": ("BODY_CAP", "the provider body exceeds the native response cap"),
    "provider_payload_shape": ("PARSER", "the provider payload lacks the shape the native parser accepts"),
    "ca_bundle_mismatch": ("CA_BUNDLE", "the public chain does not verify against --ca-bundle"),
    "private_address": ("PRIVATE_ADDRESS", "the host resolved only to non-public IPv4, which the native client refuses"),
    "control_network": ("UNKNOWN", "the control could not reach the provider either"),
}

_secrets = []


class HarnessError(Exception):
    pass


def redact(text: str) -> str:
    for secret in _secrets:
        text = text.replace(secret, "<redacted>")
    return TOKEN_PATTERN.sub("fintel_<redacted>", text)


def emit(line: str = "") -> None:
    print(redact(line), flush=True)


def bounded(text, limit=64) -> str:
    text = "".join(c if " " <= c < "\x7f" else "?" for c in str(text))
    return text if len(text) <= limit else text[:limit] + "..."


def is_number(value) -> bool:
    return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and math.isfinite(value))


def load_json(body):
    try:
        return json.loads(body)
    except (ValueError, UnicodeDecodeError, TypeError):
        return None


# --- plan -------------------------------------------------------------------

def probe_plan(now_s: int, history_days: int):
    """(route, local path, provider path) for the three probe requests.

    The spot and OHLC provider paths are exactly the native ones. The history
    control asks for the requested window; the native gap plan may align it to
    a neighbouring bucket.
    """
    to = now_s // 86400 * 86400
    start = to - history_days * 86400
    return (
        ("spot", "/v1/prices?ids=bitcoin",
         "/api/v3/simple/price?ids=bitcoin&vs_currencies=usd"
         "&include_24hr_change=true&include_24hr_vol=true"),
        ("history", f"/v1/prices/history?id=bitcoin&interval=1d&from={start}&to={to}",
         f"/api/v3/coins/bitcoin/market_chart/range?vs_currency=usd&from={start}&to={to}"),
        ("ohlc", "/v1/prices/ohlc?id=bitcoin&days=1",
         "/api/v3/coins/bitcoin/ohlc?vs_currency=usd&days=1"),
    )


def build_child_env(inherited, kv_dir, ca_bundle):
    env = {name: inherited[name] for name in ENV_ALLOWLIST if name in inherited}
    env.update({
        "FORTIS_KV_DIR": str(kv_dir),
        CANARY: "1",
        "FORTIS_CA_BUNDLE_PATH": str(ca_bundle),
        "ATTESTATION_MODE": "development",
    })
    scrubbed = sorted(name for name in inherited
                      if name not in ENV_ALLOWLIST and SCRUB_REPORT.search(name))
    return env, scrubbed


# --- native route classification ---------------------------------------------

def _spot_200(payload):
    if not isinstance(payload, dict):
        return "ambiguous", "200 with an unparseable body"
    prices = payload.get("prices")
    quote = prices.get("bitcoin") if isinstance(prices, dict) else None
    cache = quote.get("cache") if isinstance(quote, dict) else None
    if (isinstance(quote, dict) and is_number(quote.get("price")) and quote["price"] > 0
            and quote.get("source") == "coingecko" and quote.get("stale") is False
            and isinstance(cache, dict) and cache.get("hit") is False):
        return COMPATIBLE, "live coingecko quote (stale=false, cache.hit=false)"
    missing = payload.get("missing")
    if isinstance(missing, list) and "bitcoin" in missing:
        return "native_no_data", "bitcoin listed in missing: provider call failed or omitted it"
    return "ambiguous", "200 without a live coingecko quote for bitcoin"


def _history_200(payload):
    if not isinstance(payload, dict):
        return "ambiguous", "200 with an unparseable body"
    points = payload.get("points")
    cache = payload.get("cache")
    coverage = payload.get("coverage")
    valid = isinstance(points, list) and all(
        isinstance(p, list) and len(p) == 2 and is_number(p[0]) and is_number(p[1])
        for p in points)
    hit = cache.get("hit") if isinstance(cache, dict) else None
    fetched = coverage.get("fetchedPoints") if isinstance(coverage, dict) else None
    if valid and points and hit is False and is_number(fetched) and fetched > 0:
        return COMPATIBLE, f"{len(points)} daily points, fetchedPoints={fetched}, cache.hit=false"
    if valid and not points and hit is True:
        return "native_no_data", "empty window with cache.hit=true: backfill failed and was swallowed"
    count = len(points) if isinstance(points, list) else "?"
    return "ambiguous", f"points={count} cache.hit={hit} fetchedPoints={bounded(fetched, 12)}"


def _ohlc_200(payload):
    if not isinstance(payload, dict):
        return "ambiguous", "200 with an unparseable body"
    points = payload.get("points")
    cache = payload.get("cache")
    valid = isinstance(points, list) and all(
        isinstance(p, list) and len(p) == 5 and all(is_number(v) for v in p) for p in points)
    hit = cache.get("hit") if isinstance(cache, dict) else None
    if valid and points and hit is False:
        return COMPATIBLE, f"{len(points)} candles, cache.hit=false"
    count = len(points) if isinstance(points, list) else "?"
    return "ambiguous", f"candles={count} cache.hit={hit} on a fresh KV"


def classify_native(route, status, headers, body):
    """(category, detail) for one native route reply. Only COMPATIBLE claims
    compatibility; the detail never contains body text beyond the server's own
    short error code."""
    payload = load_json(body) if body else None
    error = payload.get("error") if isinstance(payload, dict) else None
    error = bounded(error, 32) if isinstance(error, str) else "non-JSON"
    if status == 404:
        return "route_absent", "404: canary routes not registered"
    if status in (401, 403):
        return "local_auth", f"{status} {error} from the native server's own auth/Origin check"
    if status == 429:
        return "local_quota", f"429 {error} from the native device quota, not the provider"
    if status == 400:
        return "local_validation", f"400 {error}: the probe request failed validation"
    if status == 200:
        return {"spot": _spot_200, "history": _history_200, "ohlc": _ohlc_200}[route](payload)
    if route == "spot" and status == 504:
        return "native_timeout", f"504 {error}: the spot owner deadline passed"
    if route == "ohlc" and status == 502:
        return "native_no_data", f"502 {error}: the OHLC owner saw a provider failure of some kind"
    return "route_error", f"{status} {error}: native route error, not provider evidence"


# --- control: native acceptance rules over a raw HTTP/1.x reply ---------------

def dechunk(data: bytes):
    """Decodes a chunked body as the native reader would; (body, error)."""
    out = bytearray()
    pos = 0
    while True:
        line_end = data.find(b"\r\n", pos)
        if line_end < 0:
            return None, "chunked_incomplete"
        line = data[pos:line_end]
        size_text = line.split(b";", 1)[0].strip()
        if not line or len(line) > 127 or not re.fullmatch(rb"[0-9A-Fa-f]+", size_text):
            return None, "chunk_size_line"
        size = int(size_text, 16)
        pos = line_end + 2
        if size == 0:
            while True:
                trailer_end = data.find(b"\r\n", pos)
                if trailer_end < 0:
                    return None, "chunked_incomplete"
                if trailer_end == pos:
                    return bytes(out), None
                name = data[pos:trailer_end].split(b":", 1)[0].strip().lower()
                if name in FORBIDDEN_TRAILERS:
                    return None, "forbidden_trailer"
                pos = trailer_end + 2
        if len(data) < pos + size + 2:
            return None, "chunked_incomplete"
        out += data[pos:pos + size]
        if data[pos + size:pos + size + 2] != b"\r\n":
            return None, "chunk_terminator"
        pos += size + 2


def parse_http_response(raw: bytes) -> dict:
    """Parses a complete raw HTTP/1.x response under the native client's rules.

    Returns status (None if the status line is unusable), lower-cased headers
    (first occurrence), the decoded body and `framing`: None when the native
    client would accept the framing, otherwise a short reason.
    """
    result = {"status": None, "headers": {}, "body": b"", "framing": None}

    def fail(reason):
        result["framing"] = reason
        return result

    head_end = raw.find(b"\r\n\r\n")
    if head_end < 0:
        return fail("header_too_large" if len(raw) > NATIVE_HEADER_CAP else "incomplete_headers")
    head = raw[:head_end + 4]
    first_end = head.find(b"\r\n")
    match = re.fullmatch(rb"HTTP/1\.([01]) ([0-9]{3})( [^\x00-\x1f\x7f]*)?", head[:first_end])
    if not match:
        return fail("status_line")
    http_10 = match.group(1) == b"0"
    status = int(match.group(2))
    result["status"] = status
    if status < 100:
        return fail("status_line")
    if status < 200:
        return fail("informational_response")
    headers = result["headers"]
    content_length = None
    chunked = False
    encoding_seen = False
    close = keep_alive = False
    for line in head[first_end + 2:head_end].split(b"\r\n"):
        if not line:
            continue
        name, sep, value = line.partition(b":")
        if not sep or not re.fullmatch(rb"[!#$%&'*+\-.^_`|~0-9A-Za-z]+", name):
            return fail("header_syntax")
        name = name.decode("latin1").lower()
        value = value.decode("latin1").strip(" \t")
        headers.setdefault(name, value)
        if name == "content-length":
            if not value.isdigit():
                return fail("header_syntax")
            if content_length is not None and int(value) != content_length:
                return fail("conflicting_content_length")
            content_length = int(value)
        elif name == "transfer-encoding":
            if chunked or value.lower() != "chunked":
                return fail("transfer_encoding=" + bounded(value, 20))
            chunked = True
        elif name == "content-encoding":
            if encoding_seen or value.lower() != "identity":
                return fail("content_encoding=" + bounded(value, 20))
            encoding_seen = True
        elif name == "connection":
            tokens = [token.strip().lower() for token in value.split(",")]
            close |= "close" in tokens
            keep_alive |= "keep-alive" in tokens
    if len(head) > NATIVE_HEADER_CAP:
        return fail("header_too_large")
    if re.search(rb"\r(?!\n)|(?<!\r)\n", head):
        return fail("bare_cr_or_lf")
    if content_length is not None and chunked:
        return fail("content_length_and_chunked")
    if status in (204, 205, 304):
        return result
    body = raw[head_end + 4:]
    if chunked:
        decoded, error = dechunk(body)
        if error:
            return fail(error)
        result["body"] = decoded
    elif content_length is not None:
        if len(body) < content_length:
            return fail("truncated_body")
        result["body"] = body[:content_length]
    elif close or (http_10 and not keep_alive):
        result["body"] = body
    else:
        return fail("no_length_and_not_close_delimited")
    return result


def _spot_shape(payload) -> bool:
    quote = payload.get("bitcoin") if isinstance(payload, dict) else None
    return isinstance(quote, dict) and is_number(quote.get("usd"))


def _history_shape(payload) -> bool:
    prices = payload.get("prices") if isinstance(payload, dict) else None
    return isinstance(prices, list) and bool(prices) and all(
        isinstance(p, list) and len(p) == 2 and is_number(p[0]) and is_number(p[1])
        for p in prices)


def _ohlc_shape(payload) -> bool:
    return isinstance(payload, list) and bool(payload) and all(
        isinstance(p, list) and len(p) == 5 and all(is_number(v) for v in p)
        for p in payload)


SHAPES = {"spot": _spot_shape, "history": _history_shape, "ohlc": _ohlc_shape}


def classify_control(route, outcome) -> tuple:
    """(category, detail, body bytes or None) for one control outcome.

    `outcome` holds `error` (None or resolve/private_address/connect/
    tls_verify/tls/timeout), `raw` bytes and `truncated`.
    """
    error = outcome.get("error")
    if error == "tls_verify":
        return ("ca_bundle_mismatch",
                "certificate chain did not verify against --ca-bundle", None)
    if error == "private_address":
        return "private_address", "only non-public IPv4 addresses resolved", None
    if error:
        return "control_network", f"control {error}: provider reachability unknown", None
    if outcome.get("truncated"):
        return ("body_cap", f"response over {LOCAL_READ_CAP} B "
                f"(native cap {NATIVE_BODY_CAPS[route]} B)", None)
    parsed = parse_http_response(outcome.get("raw", b""))
    status = parsed["status"]
    headers = parsed["headers"]
    note = ""
    if headers.get("server"):
        note += " server=" + bounded(headers["server"], 24)
    if "cf-mitigated" in headers:
        note += " cf-mitigated=" + bounded(headers["cf-mitigated"], 16)
    if status in (401, 403):
        return "provider_rejected", f"{status} from the provider{note}", None
    if status == 429:
        retry = bounded(headers.get("retry-after", "absent"), 16)
        return "provider_rate_limited", f"429 from the provider retry-after={retry}{note}", None
    if status is not None and 300 <= status < 400:
        target = urlsplit(headers.get("location", "")).hostname or "relative/absent"
        return ("provider_redirect",
                f"{status} to host {bounded(target, 48)}; native follows no redirects", None)
    if parsed["framing"]:
        return ("framing_unsupported",
                f"{parsed['framing']} (status {status}); native would reject it", None)
    if status != 200:
        return "provider_error", f"{status} from the provider{note}", None
    body = parsed["body"]
    cap = NATIVE_BODY_CAPS[route]
    if len(body) > cap:
        return "body_cap", f"{len(body)} B exceeds the native {cap} B cap", len(body)
    if not SHAPES[route](load_json(body)):
        return ("provider_payload_shape",
                "200 body lacks the shape the native parser expects", len(body))
    return "provider_ok", "200 with native-acceptable framing, size and shape", len(body)


def combine(native_category, native_elapsed, control_category):
    """The per-route verdict. COMPATIBLE needs live data from the native
    route itself; a control only attributes a native no-data outcome."""
    if native_category == COMPATIBLE:
        return "COMPATIBLE", "live provider data through the native route"
    if native_category in HARNESS_CATEGORIES:
        return "HARNESS_ERROR", "the probe request never reached provider work"
    if native_category == "skipped":
        return "UNKNOWN", "not sent: runtime budget exhausted"
    if native_category not in ATTRIBUTABLE_CATEGORIES:
        return "UNKNOWN", "native outcome is not attributable to the provider call"
    if control_category is None:
        return "UNKNOWN", "native route returned no live data and no control ran"
    if control_category == "provider_ok":
        if (native_category == "native_timeout" or
                (native_elapsed is not None and native_elapsed >= NATIVE_ATTEMPT_TIMEOUT_S - 0.5)):
            return ("NATIVE_TIMEOUT", "native call ran past a 10 s attempt while the later "
                    "control succeeded: native stall or transient provider latency")
        return ("NATIVE_TRANSPORT_OR_PARSER", "Python got an acceptable 200 with the same CA "
                "and request bytes; native TLS, framing or payload parsing failed "
                "(not isolatable from outside)")
    verdict, why = CONTROL_VERDICTS.get(control_category,
                                        ("UNKNOWN", "unrecognized control result"))
    if verdict != "UNKNOWN":
        why += " (control ran after the native call: likely cause, not proof)"
    return verdict, why


# --- network I/O (not exercised by the offline classification test) -----------

def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def local_exchange(port, method, path, headers=None, body=None, timeout=5.0):
    """One request to the native server; (status, headers, body, truncated)."""
    deadline = time.monotonic() + timeout
    lines = [f"{method} {path} HTTP/1.1", "Host: localhost", "Connection: close"]
    lines += [f"{name}: {value}" for name, value in (headers or {}).items()]
    if body is not None:
        lines.append(f"Content-Length: {len(body)}")
    received = bytearray()
    truncated = False
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as client:
        client.sendall(("\r\n".join(lines) + "\r\n\r\n").encode() + (body or b""))
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise socket.timeout("local response exceeded its bound")
            client.settimeout(left)
            chunk = client.recv(65536)
            if not chunk:
                break
            received.extend(chunk)
            if len(received) > LOCAL_READ_CAP:
                truncated = True
                break
    head, separator, payload = bytes(received).partition(b"\r\n\r\n")
    if not separator:
        raise ValueError("incomplete local response")
    first, *header_lines = head.decode("latin1").split("\r\n")
    response_headers = {}
    for line in header_lines:
        name, _, value = line.partition(":")
        response_headers[name.strip().lower()] = value.strip()
    return int(first.split()[1]), response_headers, payload, truncated


def resolve_ipv4(host, timeout):
    """getaddrinfo bounded by a helper thread; a list of IPv4 strings."""
    box = {}

    def run():
        try:
            box["infos"] = socket.getaddrinfo(host, 443, socket.AF_INET, socket.SOCK_STREAM)
        except OSError as error:
            box["error"] = error

    worker = threading.Thread(target=run, daemon=True)
    worker.start()
    worker.join(timeout)
    if worker.is_alive() or "error" in box:
        return None
    seen = []
    for info in box.get("infos", ()):
        if info[4][0] not in seen:
            seen.append(info[4][0])
    return seen[:8]


def control_request(path, ca_bundle, timeout):
    """One GET to the public host with the native client's request bytes."""
    started = time.monotonic()
    deadline = started + timeout
    outcome = {"error": None, "raw": b"", "truncated": False}

    def done(error=None):
        outcome["error"] = error
        outcome["elapsed"] = time.monotonic() - started
        return outcome

    addresses = resolve_ipv4(PUBLIC_HOST, timeout)
    if not addresses:
        return done("resolve")
    public = [a for a in addresses if ipaddress.ip_address(a).is_global]
    if not public:
        return done("private_address")
    context = ssl.create_default_context(cafile=str(ca_bundle))  # this bundle only
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    if hasattr(ssl, "OP_IGNORE_UNEXPECTED_EOF"):
        context.options |= ssl.OP_IGNORE_UNEXPECTED_EOF
    request = (f"GET {path} HTTP/1.1\r\nHost: {PUBLIC_HOST}\r\nConnection: close\r\n"
               "User-Agent: FortisNetWallet/1.0\r\nAccept-Encoding: identity\r\n\r\n").encode()
    raw_socket = None
    for address in public:
        left = deadline - time.monotonic()
        if left <= 0:
            return done("timeout")
        try:
            raw_socket = socket.create_connection((address, 443), timeout=left)
            break
        except socket.timeout:
            return done("timeout")
        except OSError:
            continue
    if raw_socket is None:
        return done("connect")
    received = bytearray()
    try:
        with context.wrap_socket(raw_socket, server_hostname=PUBLIC_HOST) as tls:
            tls.sendall(request)
            while True:
                left = deadline - time.monotonic()
                if left <= 0:
                    return done("timeout")
                tls.settimeout(left)
                try:
                    chunk = tls.recv(65536)
                except (ssl.SSLEOFError, ssl.SSLZeroReturnError):
                    break
                if not chunk:
                    break
                received.extend(chunk)
                if len(received) > LOCAL_READ_CAP:
                    outcome["truncated"] = True
                    break
    except ssl.SSLCertVerificationError:
        return done("tls_verify")
    except socket.timeout:
        return done("timeout")
    except ssl.SSLError:
        return done("tls")
    except OSError:
        return done("connect")
    finally:
        raw_socket.close()
    outcome["raw"] = bytes(received)
    return done()


def validate_ca_bundle(path: Path):
    """None if the native client and the control can both load the bundle."""
    text = str(path)
    if len(text) >= 1024 or any(ord(c) <= 0x20 or ord(c) == 0x7f for c in text):
        return "path is too long or contains whitespace/control bytes (native rejects it)"
    if not path.is_file():
        return "not a regular file"
    size = path.stat().st_size
    if size > NATIVE_CA_BUNDLE_MAX:
        return f"{size} B exceeds the native 1 MiB bundle cap"
    if b"-----BEGIN CERTIFICATE-----" not in path.read_bytes():
        return "no PEM certificate found"
    try:
        context = ssl.create_default_context(cafile=text)
    except (ssl.SSLError, OSError) as error:
        return f"unreadable by Python ssl ({type(error).__name__})"
    if not context.cert_store_stats().get("x509_ca"):
        return "no CA certificates loaded"
    return None


def stderr_tail(stderr_file) -> str:
    stderr_file.flush()
    stderr_file.seek(0, os.SEEK_END)
    size = stderr_file.tell()
    stderr_file.seek(max(0, size - STDERR_TAIL_BYTES))
    return bounded(stderr_file.read().decode("utf-8", "replace").replace("\n", " | "),
                   STDERR_TAIL_BYTES)


def start_server(binary, env, stderr_file):
    port = free_port()
    try:
        process = subprocess.Popen([str(binary), str(port)], env=env, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.DEVNULL, stderr=stderr_file)
    except OSError as error:
        raise HarnessError(f"could not start the server ({type(error).__name__})") from None
    try:
        deadline = time.monotonic() + START_TIMEOUT_S
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise HarnessError(f"server exited {process.returncode} before serving; "
                                   f"stderr tail: {stderr_tail(stderr_file)}")
            try:
                if local_exchange(port, "GET", "/healthz", timeout=0.5)[0] == 200:
                    return process, port
            except (OSError, ValueError, IndexError):
                pass
            time.sleep(0.05)
        return process, None
    except BaseException:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        raise


def enroll(port) -> str:
    status, _, body, _ = local_exchange(port, "POST", "/v1/devices/challenge", body=b"",
                                        timeout=10)
    challenge = load_json(body)
    if status != 200 or not isinstance(challenge, dict) or "challenge" not in challenge:
        raise HarnessError(f"device challenge answered {status}")
    payload = json.dumps({
        "platform": "android", "appVersion": "4.5.6",
        "attestation": {"provider": "development", "challenge": challenge["challenge"]},
    }, separators=(",", ":")).encode()
    status, _, body, _ = local_exchange(port, "POST", "/v1/devices",
                                        {"Content-Type": "application/json"}, payload,
                                        timeout=10)
    device = load_json(body)
    token = device.get("deviceToken") if isinstance(device, dict) else None
    if status != 201 or not isinstance(token, str) or not token:
        error = device.get("error") if isinstance(device, dict) else "non-JSON"
        raise HarnessError(f"device enrollment answered {status} {bounded(error, 32)}")
    _secrets.append(token)
    return token


def shutdown(process):
    """SIGTERM, then (clean, description)."""
    started = time.monotonic()
    process.send_signal(signal.SIGTERM)
    try:
        code = process.wait(timeout=SHUTDOWN_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)
        return False, f"UNCLEAN: no exit within {SHUTDOWN_TIMEOUT_S:.0f} s of SIGTERM; killed"
    elapsed = time.monotonic() - started
    if code == 0:
        return True, f"clean: exit 0 {elapsed:.2f} s after SIGTERM"
    return False, f"UNCLEAN: exit {code} {elapsed:.2f} s after SIGTERM"


def size_text(length, truncated=False) -> str:
    if length is None:
        return "-"
    return f">={LOCAL_READ_CAP} B" if truncated else f"{length} B"


def run_probe(args, binary, ca_bundle, kv_dir, stderr_file, state):
    """Fills state["results"]; once a server started, state["shutdown"] holds
    (clean, description) even when a harness error or interrupt escapes."""
    deadline = time.monotonic() + args.max_runtime
    env, scrubbed = build_child_env(os.environ, kv_dir, ca_bundle)
    shown = ", ".join(scrubbed[:20]) + (" ..." if len(scrubbed) > 20 else "")
    emit(f"environment: allowlist only; dropped {len(scrubbed)} inherited "
         f"provider/endpoint/proxy/test name(s){': ' + shown if scrubbed else ''}")
    process, port = start_server(binary, env, stderr_file)
    results = state["results"]
    try:
        if port is None:
            raise HarnessError(f"server did not answer /healthz within {START_TIMEOUT_S:.0f} s")
        emit(f"server: pid {process.pid} on 127.0.0.1:{port}, KV {kv_dir}")
        auth = {"Authorization": "Bearer " + enroll(port)}
        emit("device: development device enrolled (token withheld)")
        plan = probe_plan(int(time.time()), args.history_days)
        earlier_failure = False
        for index, (route, local_path, provider_path) in enumerate(plan):
            if index and args.pause:
                time.sleep(max(0.0, min(args.pause,
                                        deadline - time.monotonic() - SHUTDOWN_RESERVE_S)))
            budget = deadline - time.monotonic() - SHUTDOWN_RESERVE_S
            result = {"route": route}
            if budget < 5:
                result.update(category="skipped", detail="runtime budget exhausted",
                              elapsed=None, size=None)
                emit(f"[native ] {route:<7} GET {local_path} -> skipped (runtime budget)")
            else:
                started = time.monotonic()
                try:
                    status, headers, body, truncated = local_exchange(
                        port, "GET", local_path, auth, timeout=min(ROUTE_TIMEOUT_S, budget))
                    category, detail = classify_native(route, status, headers, body)
                    if truncated:
                        category, detail = "route_error", "local reply over the read cap"
                except socket.timeout:
                    status, body, truncated = None, None, False
                    category = "route_error"
                    detail = "no reply within the client bound (owner deadlines are 25-30 s)"
                except (OSError, ValueError, IndexError) as error:
                    status, body, truncated = None, None, False
                    category, detail = "route_error", f"local exchange failed ({type(error).__name__})"
                elapsed = time.monotonic() - started
                result.update(category=category, detail=detail, elapsed=elapsed,
                              size=len(body) if body is not None else None)
                emit(f"[native ] {route:<7} GET {local_path} -> {status or '-'} {category} "
                     f"{elapsed:.2f}s {size_text(result['size'], truncated)}: {detail}")
            control_category = None
            wants_control = result["category"] != "skipped" and (args.control == "always" or (
                args.control == "on-failure" and result["category"] in ATTRIBUTABLE_CATEGORIES))
            budget = deadline - time.monotonic() - SHUTDOWN_RESERVE_S
            if wants_control and budget >= 3:
                outcome = control_request(provider_path, ca_bundle,
                                          min(CONTROL_TIMEOUT_S, budget))
                control_category, control_detail, control_size = classify_control(route, outcome)
                parsed_status = parse_http_response(outcome["raw"])["status"] \
                    if outcome["raw"] else None
                emit(f"[control] {route:<7} GET {PUBLIC_HOST}{provider_path} -> "
                     f"{parsed_status or '-'} {control_category} {outcome['elapsed']:.2f}s "
                     f"{size_text(control_size, outcome['truncated'])}: {control_detail}")
            elif wants_control:
                emit(f"[control] {route:<7} skipped (runtime budget)")
            verdict, why = combine(result["category"], result["elapsed"], control_category)
            if verdict != "COMPATIBLE" and earlier_failure:
                why += ("; an earlier native failure may have opened the native coingecko "
                        "lane breaker (or a control used the per-IP provider budget), so "
                        "this route may have been shed without a provider call")
            earlier_failure |= result["category"] in ATTRIBUTABLE_CATEGORIES
            result["verdict"] = verdict
            results.append(result)
            emit(f"[verdict] {route:<7} {verdict}: {why}")
    finally:
        state["shutdown"] = shutdown(process)


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Opt-in live CoinGecko compatibility probe for the native price "
                    "canary. Makes real public requests; never run from make test.")
    parser.add_argument("binary", help="normal host binary, e.g. build/fortis_node")
    parser.add_argument("--ca-bundle", required=True,
                        help="public CA bundle (PEM, at most 1 MiB) for FORTIS_CA_BUNDLE_PATH")
    parser.add_argument("--control", choices=("on-failure", "always", "never"),
                        default="on-failure",
                        help="direct HTTPS control: only after a native no-data result "
                             "(default), after every route, or never")
    parser.add_argument("--history-days", type=int, default=7,
                        help="recent daily history window in days, 1-30 (default 7)")
    parser.add_argument("--pause", type=float, default=2.0,
                        help="seconds between provider-touching routes, 0-10 (default 2)")
    parser.add_argument("--max-runtime", type=float, default=240.0,
                        help="total runtime bound in seconds including shutdown, "
                             "90-600 (default 240)")
    args = parser.parse_args(argv)
    if not 1 <= args.history_days <= 30:
        parser.error("--history-days must be 1-30")
    if not 0 <= args.pause <= 10:
        parser.error("--pause must be 0-10")
    if not 90 <= args.max_runtime <= 600:
        parser.error("--max-runtime must be 90-600")
    return args


def main(argv=None) -> int:
    args = parse_args(argv)
    binary = Path(args.binary).resolve()
    ca_bundle = Path(args.ca_bundle).resolve()
    emit(f"live price canary: keyless {PUBLIC_HOST}, one spot, one 1d history "
         f"({args.history_days} days) and one OHLC GET for bitcoin; control={args.control}")
    if not binary.is_file() or not os.access(binary, os.X_OK):
        emit(f"SETUP ERROR: {binary} is not an executable file")
        return 2
    problem = validate_ca_bundle(ca_bundle)
    if problem:
        emit(f"SETUP ERROR: --ca-bundle {ca_bundle}: {problem}")
        return 2
    kv_dir = Path(tempfile.mkdtemp(prefix="fortis-live-price-canary-"))
    state = {"results": [], "shutdown": None}
    harness_failed = False
    with tempfile.TemporaryFile() as stderr_file:
        try:
            run_probe(args, binary, ca_bundle, kv_dir, stderr_file, state)
        except HarnessError as error:
            emit(f"HARNESS ERROR: {error}")
            harness_failed = True
        except KeyboardInterrupt:
            emit("HARNESS ERROR: interrupted")
            harness_failed = True
        if state["shutdown"] is None:
            emit("shutdown: no running server to stop")
        else:
            emit(f"shutdown: {state['shutdown'][1]}")
            if not state["shutdown"][0]:
                emit(f"server stderr tail: {stderr_tail(stderr_file)}")
    try:
        shutil.rmtree(kv_dir)
    except OSError as error:
        emit(f"cleanup: failed to remove {kv_dir} ({type(error).__name__})")
    kv_removed = not kv_dir.exists()
    if kv_removed:
        emit(f"cleanup: temporary KV {kv_dir} removed")

    results = state["results"]
    verdicts = [result["verdict"] for result in results]
    if (state["shutdown"] is not None and not state["shutdown"][0]) or not kv_removed:
        exit_code = 3
    elif harness_failed or "HARNESS_ERROR" in verdicts:
        exit_code = 2
    elif len(verdicts) != 3 or any(verdict != "COMPATIBLE" for verdict in verdicts):
        exit_code = 1
    else:
        exit_code = 0
    summary = ", ".join(f"{r['route']}={r['verdict']}" for r in results) or "no route verdicts"
    emit(f"RESULT: {summary}; exit {exit_code}")
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
