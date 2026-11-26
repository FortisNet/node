#!/usr/bin/env python3
"""Real-socket edges of GET /v1/networks/{network}/token/{tokenId} (R19)
against tests/corpus/token360_edges.json, which
the earlier TypeScript implementation's tests recorded from the
real Fastify app over real sockets. The fixture's SHA-256 is checked against
its pinned .sha256 file before anything runs.

build/fortis_node_token_route_test sends every token provider request
to one loopback HTTPS upstream naming its real host in x-fortis-test-host.
Per case the upstream answers the exact bytes TypeScript was served, in the
order TypeScript asked for them (a request this case did not make falls back
to the latest earlier answer for the same request; a /coins/markets request
for one coin id that TypeScript batched with others is answered with that
coin's rows of the batch; a host the case marked down answers 503). Each
case's frozen instant is written to the token clock file first; the native
provider lanes are paced by monotonic time and an open lane breaker (a 429)
is waited out in real time. Each fixture group is a fresh process with its
own KV directory; restart-b reuses restart-a's after a SIGKILL.

Checks per request case: the status, every recorded header
(x-rate-limit-remaining only in the quota group), the exact JSON text or
protobuf bytes, the multiset of provider requests (background revalidations
included, collected after /probe/test-token-idle; /coins/markets compared per
coin id), each provider request's identity headers as TypeScript sent them
(accept, content-type, x-cg-demo-api-key / x-cg-pro-api-key; Ethplorer's key
is in its URL), that no client header or device credential reaches a provider
and each provider key only its own host; no device token/id or provider key
reaches a response or the process output. `state` records compare the SWR
envelopes (asOf, freshUntil, staleUntil, the stored JSON text), the
token_facts document and the registry identity fields through POST
/probe/test-token-state. Concurrent cases compare every response and the
combined provider requests.

Native-only rules (timing-dependent, not frozen by the oracle):
  - `sustained`: 120 reads from 12 clients of cached tokens (JSON, protobuf,
    HEAD) are byte-identical and reach no provider;
  - the production composition: the route answers 404 unless
    FORTIS_NATIVE_PRICE_ROUTE_CANARY=1, then 401/403 before any facet work.

Asserted differences are listed in ASSERTED and documented beside
handle_token360 in src/tokens/token_task_routes.cpp.

Usage: token360_edges_test.py build/fortis_node_token_route_test [build/fortis_node]
Environment: R19_ONLY_GROUPS=a,b limits groups; R19_FAIL_FAST=1 stops at the
first failure.
"""
import collections
import hashlib
import json
import os
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parent))
import discovery_route_harness as harness  # noqa: E402
import compat_expectations  # noqa: E402
import tls_ca  # noqa: E402

FIXTURE = "token360_edges.json"
FAILURES = []
COUNTS = collections.Counter()
HEADERS = ["content-type", "content-length", "vary", "cache-control", "etag", "x-content-type-options",
           "referrer-policy", "permissions-policy", "x-rate-limit-cost", "x-rate-limit-remaining", "retry-after",
           "access-control-allow-origin"]
NATIVE_BASELINE = {"cache-control": "no-store", "x-content-type-options": "nosniff", "referrer-policy": "no-referrer",
                   "permissions-policy": "camera=(), microphone=(), geolocation=()"}
# Native lane budgets (src/providers/provider_lane.cpp): (requests, interval ms, burst).
LANES = {"coingecko": (24, 60000, 4), "dexscreener": (60, 60000, 15), "goplus": (30, 60000, 30),
         "ethplorer": (30, 60000, 30)}
RPC_LANE = (20, 1000, 5)
IDENTITY_HEADERS = ("accept", "content-type", "x-cg-demo-api-key", "x-cg-pro-api-key")
FAIL_FAST = os.environ.get("R19_FAIL_FAST") == "1"


def check(condition, message):
    if not condition:
        FAILURES.append(message)
        print("FAIL:", message, flush=True)
        if FAIL_FAST:
            raise SystemExit(1)
    return condition


def load_fixture():
    data = (harness.FIXTURES / FIXTURE).read_bytes()
    pinned = (harness.FIXTURES / f"{FIXTURE}.sha256").read_text().split()[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{FIXTURE}: sha256 {digest} does not match the pinned {pinned}")
    print(f"{FIXTURE}: sha256 {digest} matches the pin", flush=True)
    return json.loads(data)


def lane_for(host):
    if host.endswith("coingecko.com"):
        return "coingecko"
    return {"api.dexscreener.com": "dexscreener", "api.gopluslabs.io": "goplus",
            "api.ethplorer.io": "ethplorer"}.get(host, "rpc:" + host)


def request_key(method, url, body):
    split = urlsplit(url)
    url = f"https://{split.netloc}{split.path or '/'}" + (f"?{split.query}" if split.query else "")
    return f"{method} {url} {body}" if body else f"{method} {url}"


def markets_ids(url):
    split = urlsplit(url)
    if not split.netloc.endswith("coingecko.com") or split.path != "/api/v3/coins/markets":
        return None
    query = parse_qs(split.query, keep_blank_values=True)
    return (query.get("ids") or [""])[0].split(","), sorted((k, v[0]) for k, v in query.items() if k != "ids")


def comparable(method, url, body):
    """A provider request as comparable keys: /coins/markets per coin id
    (TypeScript's WindowBatcher merges concurrent ids into one request)."""
    batched = markets_ids(url)
    if batched:
        ids, rest = batched
        host = urlsplit(url).netloc
        return [f"GET https://{host}/api/v3/coins/markets {json.dumps(rest)} {coin}" for coin in ids]
    return [request_key(method, url, body)]


class CaseTable:
    def __init__(self):
        self.lock = threading.Lock()
        self.latest = {}
        self.queue = {}
        self.served = collections.Counter()
        self.mode = {}
        self.extra = []

    def start_case(self, upstream_calls, mode):
        with self.lock:
            for key, replies in self.queue.items():
                if replies:
                    self.latest[key] = replies[-1]
            self.queue = {}
            for call in upstream_calls:
                key = request_key(call["method"], call["url"], call.get("requestBody") or "")
                self.queue.setdefault(key, []).append((call["status"], call["body"], call.get("delayMs", 25)))
            self.mode = mode or {}
            self.served = collections.Counter()

    def markets_rows(self, coin):
        """TypeScript's latest /coins/markets answer that covered `coin`."""
        for key, replies in list(self.queue.items()) + [(k, [v]) for k, v in self.latest.items()]:
            if not key.startswith("GET https://") or "/api/v3/coins/markets?" not in key:
                continue
            other = markets_ids(key.split(" ", 1)[1].split(" ")[0])
            if not other or coin not in other[0]:
                continue
            status, text, delay = replies[-1]
            if status != 200:
                return status, text, delay
            rows = json.loads(text)
            return 200, [row for row in rows if isinstance(row, dict) and row.get("id") == coin], delay
        return None

    def markets_subset(self, url):
        """A /coins/markets request whose id set TypeScript asked in another
        grouping (its WindowBatcher merges concurrent ids; the native
        revalidation batch merges queued ones): each coin's rows of the
        answer that covered it, in the requested order."""
        batched = markets_ids(url)
        if not batched:
            return None
        rows, delay = [], 25
        for coin in batched[0]:
            found = self.markets_rows(coin)
            if found is None:
                return None
            if found[0] != 200:
                return found
            rows += found[1]
            delay = found[2]
        return 200, json.dumps(rows, separators=(",", ":"), ensure_ascii=False), delay

    def answer(self, method, host, target, body):
        down = self.mode.get("down")
        if down == "all" or (isinstance(down, list) and host in down):
            return 503, b"unavailable", 25, False
        url = f"https://{host}{target}"
        key = request_key(method, url, body)
        replies = self.queue.get(key)
        index = self.served[key]
        self.served[key] += 1
        if replies:
            status, text, delay = replies[min(index, len(replies) - 1)]
        elif key in self.latest:
            status, text, delay = self.latest[key]
        elif self.markets_subset(url) is not None:
            status, text, delay = self.markets_subset(url)
        else:
            self.extra.append(key)
            return 404, b'{"error":"unexpected request"}', 25, True
        is_json = status < 300 and text not in ("unavailable",)
        try:
            json.loads(text)
        except ValueError:
            is_json = False
        return status, text.encode("utf-8", "surrogatepass"), delay, is_json


class Upstream:
    def __init__(self, root, table):
        self.table = table
        self.observed = []
        self.lock = threading.Lock()
        self.in_flight = 0
        self.peak = 0
        ca, cert, key, _ = tls_ca.create_certificates(Path(root), "DNS:localhost")
        self.ca = ca
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def handle_any(self, method):
                length = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(length).decode("utf-8", "surrogateescape") if length else ""
                host = self.headers.get("x-fortis-test-host") or "api.coingecko.com"
                with upstream.table.lock:
                    status, payload, delay, is_json = upstream.table.answer(method, host, self.path, body)
                with upstream.lock:
                    upstream.observed.append((method, host, self.path, body,
                                              {k.lower(): v for k, v in self.headers.items()}))
                    upstream.in_flight += 1
                    upstream.peak = max(upstream.peak, upstream.in_flight)
                time.sleep(delay / 1000)
                with upstream.lock:
                    upstream.in_flight -= 1
                self.send_response(status)
                if status < 300 and is_json:
                    self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.send_header("Connection", "close")
                self.end_headers()
                try:
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
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        self.server.socket = tls.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_address[1]

    def calls(self):
        with self.lock:
            return list(self.observed)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


class Lanes:
    """Per-lane token buckets with the native budgets, in real time."""

    def __init__(self):
        self.state = {}
        self.open_until = 0.0

    def reserve(self, calls):
        need = collections.Counter(lane_for(urlsplit(call["url"]).netloc) for call in calls)
        now = time.monotonic()
        wait = max(0.0, self.open_until - now)
        for lane, count in need.items():
            rate, interval, burst = LANES.get(lane, RPC_LANE)
            per_ms = rate / interval
            tokens, at = self.state.get(lane, (float(burst), now))
            tokens = min(float(burst), tokens + (now - at) * 1000 * per_ms)
            want = min(float(count), float(burst))
            if tokens < want:
                wait = max(wait, (want - tokens) / per_ms / 1000 + 0.05)
            self.state[lane] = (tokens, now)
        if wait:
            time.sleep(wait)
        now = time.monotonic()
        for lane, count in need.items():
            rate, interval, burst = LANES.get(lane, RPC_LANE)
            per_ms = rate / interval
            tokens, at = self.state[lane]
            tokens = min(float(burst), tokens + (now - at) * 1000 * per_ms)
            self.state[lane] = (max(0.0, tokens - count), now)

    def breaker(self, calls):
        """A 429 (or five consecutive faults) opened a lane breaker for 30 s on
        the native monotonic clock; TypeScript's ran on the frozen Date."""
        if any(call["status"] == 429 for call in calls):
            self.open_until = time.monotonic() + 31.0


class Server:
    def __init__(self, binary, kv_dir, clock_file, upstream, now_ms, extra_env=None):
        self.port = harness.free_port()
        env = dict(os.environ)
        for name in ("COINGECKO_API_KEY", "COINGECKO_TIER", "JUPITER_API_KEY", "ETHERSCAN_API_KEY",
                     "ETHPLORER_API_KEY", "TAOSTATS_API_KEY", "TRONSCAN_API_KEY", "TRONGRID_API_KEY",
                     "SAUCERSWAP_API_KEY", "FORTIS_NATIVE_PRICE_ROUTE_CANARY", "FORTIS_NATIVE_NEWS_CANARY",
                     "ATTESTATION_MODE", "ALCHEMY_API_KEY"):
            env.pop(name, None)
        env.update({
            "FORTIS_ENABLE_TEST_TOKEN_ROUTES": "1",
            "FORTIS_TEST_TOKEN_UPSTREAM_PORT": str(upstream.port),
            "FORTIS_TEST_TOKEN_UPSTREAM_CA": str(upstream.ca),
            "FORTIS_TEST_TOKEN_NOW_FILE": str(clock_file),
            "FORTIS_ENABLE_TEST_PRICE_ROUTE": "1",
            "FORTIS_TEST_PRICE_UPSTREAM_PORT": str(upstream.port),
            "FORTIS_TEST_PRICE_UPSTREAM_CA": str(upstream.ca),
            "FORTIS_TEST_PRICE_NOW_MS": str(now_ms),
            "FORTIS_KV_DIR": str(kv_dir),
            "PORT": str(self.port),
        })
        env.update(extra_env or {})
        self.output = tempfile.TemporaryFile()
        self.process = subprocess.Popen([binary, str(self.port)], env=env, stdout=self.output, stderr=self.output)
        for _ in range(300):
            if self.process.poll() is not None:
                self.output.seek(0)
                raise RuntimeError("test server exited: " + self.output.read().decode(errors="replace"))
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=.1):
                    return
            except OSError:
                time.sleep(.03)
        raise RuntimeError("test server did not start")

    def probe(self, method, target, body=None):
        status, _, payload = raw(self.port, method, target,
                                 [("content-type", "application/json"),
                                  ("content-length", str(len(body or b"")))] if body is not None else [],
                                 body=body)
        return status, payload

    def text(self):
        self.output.seek(0)
        return self.output.read().decode("utf-8", "replace")

    def stop(self, kill=False):
        if kill:
            self.process.send_signal(signal.SIGKILL)
        else:
            self.process.terminate()
        try:
            self.process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()


def raw(port, method, target, headers, body=None, timeout=90):
    lines = [f"{method} {target} HTTP/1.1", "Host: localhost", "Connection: close"]
    lines += [f"{key}: {value}" for key, value in headers]
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("latin1") + (body or b""))
        data = bytearray()
        while True:
            chunk = sock.recv(1 << 20)
            if not chunk:
                break
            data.extend(chunk)
    head, _, payload = bytes(data).partition(b"\r\n\r\n")
    first, *header_lines = head.decode("latin1").split("\r\n")
    response_headers = {}
    for line in header_lines:
        if ":" in line:
            key, value = line.split(":", 1)
            response_headers[key.lower()] = value.strip()
    return int(first.split()[1]), response_headers, payload


def token(letter):
    return "fintel_dev_" + letter * 43


def device_id(letter):
    crockford = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
    return "01ARZ3NDEKTSV4RRFFQ69G5FT" + crockford[harness.TOKEN_LETTERS.index(letter)]


def request_headers(case, kind=None):
    headers = [("authorization", "Bearer " + token(case["device"]))] if case["device"] else []
    if kind is not None:
        headers += [("accept", "application/protobuf")] if kind == "protobuf" else []
    else:
        headers += list(case["requestHeaders"].items())
    return headers


def expected_body(record):
    if "wireHex" in record:
        return bytes.fromhex(record["wireHex"])
    return record.get("responseText", "").encode("utf-8", "surrogatepass")


# ---- asserted differences ----------------------------------------------------------------------

ROUTER_404 = {"router-post", "router-put", "router-trailing-slash", "router-upper-path", "router-plural",
              "router-extra-segment", "router-no-token"}


def first_difference(expected, actual):
    index = next((i for i, (x, y) in enumerate(zip(expected, actual)) if x != y), min(len(expected), len(actual)))
    return f"at {index}: expected …{expected[max(0, index - 120):index + 120]!r}\n   actual   …" \
           f"{actual[max(0, index - 120):index + 120]!r}"


def compare(label, record, status, headers, payload, remaining=False):
    if not check(status == record["status"], f"{label}: status {status} != {record['status']} {payload[:300]!r}"):
        return False
    ok = True
    for name in HEADERS:
        if name == "x-rate-limit-remaining" and not remaining:
            continue
        ok &= check(headers.get(name) == record["headers"].get(name),
                    f"{label}: header {name} {headers.get(name)!r} != {record['headers'].get(name)!r}")
    expected = expected_body(record)
    ok &= check(payload == expected, f"{label}: body {first_difference(expected, payload)}")
    return ok


def parser_baseline(h, case, actual, observed):
    """The routing 414 (FST_ERR_MAX_PARAM_LENGTH) and Node's raw-byte "Client
    Error": both answer before any hook with the same status; the body is
    this server's {"error":...} (fastify-errors);
    TypeScript's come from the router / clientError handler without the
    onSend security headers, the shared native writer adds its baseline."""
    status, headers, payload = actual
    # fastify-errors: both are {"error":...} as application/json;
    # charset=utf-8 now (URI_TOO_LONG, BAD_REQUEST), still before any hook.
    want = compat_expectations.fastify_case(dict(case, headers=dict(case["headers"], **NATIVE_BASELINE)))
    return compare(case["name"], want, status, headers, payload, remaining=case["group"] == "quota") and \
        check(not observed, f"{case['name']}: provider requests {observed}")


def bad_url(h, case, actual, observed):
    """A malformed or non-UTF-8 path escape: both 400 before any hook;
    Fastify sends its FST_ERR_BAD_URL object, the native request parser
    {"error":"BAD_REQUEST"} (the shared parser, as in R13-R18)."""
    ok = json.loads(case["responseText"]).get("code") == "FST_ERR_BAD_URL"
    return check(ok and actual[0] == 400 == case["status"] and actual[2] == b'{"error":"BAD_REQUEST"}' and
                 not observed, f"{case['name']}: native {actual[0]} {actual[2][:120]!r}")


def reorder_errors(native_text, typescript_text):
    """The native JSON with its `errors` members in TypeScript's order."""
    native = json.loads(native_text, object_pairs_hook=collections.OrderedDict)
    typescript = json.loads(typescript_text, object_pairs_hook=collections.OrderedDict)
    if set(native.get("errors", {})) != set(typescript.get("errors", {})):
        return None
    native["errors"] = collections.OrderedDict((k, native["errors"][k]) for k in typescript["errors"])
    return json.dumps(native, separators=(",", ":"), ensure_ascii=False).encode("utf-8", "surrogatepass")


def errors_order(h, case, actual, observed):
    """Several sections failed on a cold read: TypeScript's `errors` members
    follow promise settlement (a reverted eth_call settles before a CoinGecko
    request retried 250 ms + 1 s later), native's follow facet order (market,
    profile, liquidity, holders, chainMeta) — the known token_routes
    difference. Equal bytes once the members are in TypeScript's order, with
    the same content-length; provider requests exact."""
    status, headers, payload = actual
    reordered = reorder_errors(payload, case["responseText"])
    ok = check(status == case["status"] == 200 and reordered == expected_body(case) and payload != reordered,
               f"{case['name']}: body differs beyond the errors order {payload[:200]!r}")
    for name in HEADERS:
        if name != "x-rate-limit-remaining":
            ok &= check(headers.get(name) == case["headers"].get(name), f"{case['name']}: header {name}")
    return compare_upstream(h, case["name"], case, observed, case["group"]) and ok


def flip_hits(record):
    """TypeScript's answer with every cache block reporting hit: true."""
    if "wireHex" in record:
        return bytes.fromhex(record["wireHex"]).replace(b"\n\x03hit\x12\x02 \x00", b"\n\x03hit\x12\x02 \x01")
    return record.get("responseText", "").replace('"hit":false', '"hit":true').encode("utf-8", "surrogatepass")


def late_follower(h, case, actual, observed):
    """Simultaneous cold reads of one token: TypeScript's SWR single flight
    hands every reader the one live read (hit: false); the native service's
    two foreground workers compose it live and queued readers can read any
    subset of the four entries just written (hit: true, same timestamps).
    Each answer is TypeScript's bytes or one of those cache-provenance forms.
    The
    workers' GETs coalesce only while an owner flight is active. A queued
    worker can miss after that flight ends and before its SWR write, repeating
    one GET; EVM multicall POSTs can repeat too (TypeScript itself sends
    identical eth_calls of two facets within one read, token_robinhood.json).
    Each distinct recorded provider request may repeat at most once. The
    cross-worker cold-read gap remains an open performance follow-up."""
    ok = True
    late = 0
    def forms(record):
        if record.get("wireHex"):
            document = decode_wire(bytes.fromhex(record["wireHex"]))
            encode = struct_wire
        elif record.get("responseText"):
            document = json.loads(record["responseText"], object_pairs_hook=collections.OrderedDict)
            encode = lambda value: json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        else:
            return []
        names = [name for name, value in document.get("cache", {}).items() if value.get("hit") is False]
        variants = []
        for bits in range(1, 1 << len(names)):
            candidate = json.loads(json.dumps(document), object_pairs_hook=collections.OrderedDict)
            for index, name in enumerate(names):
                if bits & (1 << index):
                    candidate["cache"][name]["hit"] = True
            variants.append(encode(candidate))
        return variants

    for index, (record, (status, headers, payload)) in enumerate(zip(case["responses"], actual)):
        label = f"{case['name']}[{index}]"
        variants = forms(record)
        exact = payload == expected_body(record)
        follower = payload in variants and payload != expected_body(record)
        if record["headers"].get("content-type", "").startswith("application/protobuf") or "wireHex" in record:
            pass
        head = record["status"] == 200 and not payload and not expected_body(record)
        if head:
            length = headers.get("content-length")
            exact = length == record["headers"].get("content-length")
            json_twin = next((r for r in case["responses"] if r.get("responseText")), None)
            follower = json_twin is not None and any(length == str(len(form)) for form in forms(json_twin))
        late += follower and not exact
        ok &= check(status == record["status"] == 200 and (exact or follower), f"{label}: neither TypeScript's "
                    f"answer nor its late-follower form {payload[:160]!r}")
        for name in HEADERS:
            if name in ("x-rate-limit-remaining", "content-length"):
                continue
            ok &= check(headers.get(name) == record["headers"].get(name), f"{label}: header {name}")
        if payload:
            ok &= check(headers.get("content-length") == str(len(payload)), f"{label}: content-length")
    print(f"  {case['name']}: {late} late followers of {len(actual)}", flush=True)
    expected = collections.Counter(request_key(c["method"], c["url"], c.get("requestBody") or "")
                                   for c in case["upstream"])
    actual_keys = collections.Counter(request_key(m, f"https://{host}{target}", b)
                                      for m, host, target, b, _ in observed)
    extra = actual_keys - expected
    ok &= check(all(expected[key] and count == 1 for key, count in extra.items()),
                f"{case['name']}: provider requests beyond one repeat {list(extra)}")
    repeats = [dict(call) for call in case["upstream"]
               if request_key(call["method"], call["url"], call.get("requestBody") or "") in extra]
    return compare_upstream(h, case["name"], dict(case, upstream=case["upstream"] + repeats), observed,
                            case["group"]) and ok


# ---- google.protobuf.Struct in StructuredReadResponse {payload = 1} -------------------------

def _varint(n):
    out = bytearray()
    while True:
        byte = n & 0x7F
        n >>= 7
        out.append(byte | (0x80 if n else 0))
        if not n:
            return bytes(out)


def _field(number, payload):
    return _varint(number << 3 | 2) + _varint(len(payload)) + payload


def _value(v):
    if v is None:
        return b"\x08\x00"
    if v is True or v is False:
        return b"\x20" + (b"\x01" if v else b"\x00")
    if isinstance(v, (int, float)):
        return b"\x11" + struct.pack("<d", float(v))
    if isinstance(v, str):
        return _field(3, v.encode("utf-8", "surrogatepass"))
    if isinstance(v, list):
        return _field(6, b"".join(_field(1, _value(item)) for item in v))
    return _field(5, _struct(v))


def _struct(obj):
    return b"".join(_field(1, _field(1, k.encode("utf-8", "surrogatepass")) + _field(2, _value(v)))
                    for k, v in obj.items())


def struct_wire(document):
    return _field(1, _struct(document))


def _read(data, i):
    shift = n = 0
    while True:
        byte = data[i]
        i += 1
        n |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return n, i


def _fields(data):
    i = 0
    while i < len(data):
        key, i = _read(data, i)
        if key & 7 == 2:
            length, i = _read(data, i)
            yield key >> 3, 2, data[i:i + length]
            i += length
        elif key & 7 == 1:
            yield key >> 3, 1, data[i:i + 8]
            i += 8
        else:
            value, i = _read(data, i)
            yield key >> 3, 0, value


def _decode_value(data):
    for number, _, payload in _fields(data):
        if number == 1:
            return None
        if number == 2:
            return struct.unpack("<d", payload)[0]
        if number == 3:
            return payload.decode("utf-8", "surrogatepass")
        if number == 4:
            return bool(payload)
        if number == 5:
            return _decode_struct(payload)
        if number == 6:
            return [_decode_value(item) for n, _, item in _fields(payload) if n == 1]
    return None


def _decode_struct(data):
    out = collections.OrderedDict()
    for _, _, entry in _fields(data):
        parts = {n: p for n, _, p in _fields(entry)}
        out[parts[1].decode("utf-8", "surrogatepass")] = _decode_value(parts.get(2, b""))
    return out


def decode_wire(data):
    return _decode_struct(next(p for n, _, p in _fields(data) if n == 1))


def self_check_codec(fixture):
    """struct_wire reproduces TypeScript's StructuredReadResponse bytes."""
    for unit in fixture["units"]:
        document = json.loads(unit["responseText"], object_pairs_hook=collections.OrderedDict, parse_int=float)
        if not check(struct_wire(document).hex() == unit["jsonWireHex"] and
                     decode_wire(bytes.fromhex(unit["jsonWireHex"])) == document, f"codec: unit {unit['name']}"):
            return False
    return True


def unlisted_coin_id(h, case, actual, observed):
    """CoinGecko's contract lookup answers an id TypeScript carries into its
    URLs, the registry and `meta` but native refuses: a non-string id
    (`{id: 5}` → /coins/5, ids=5, coingeckoId 5) or one of 1,100 bytes or
    more (the native Resolution bound; real ids are under 100). Native treats
    the token as unlisted. Expected: TypeScript's own answer with meta,
    market and profile null, safety the unlisted score (TypeScript's for the
    unlisted `contract-no-id` token), no `errors.profile`, and profile
    provenance equal to market's (both read and stored null); the provider
    requests are TypeScript's without the /coins/markets and /coins/{id}
    reads."""
    status, headers, payload = actual
    twin = json.loads(next(c for c in h.fixture["cases"] if c.get("name") == "contract-no-id")["responseText"])
    protobuf = "wireHex" in case
    expected = decode_wire(bytes.fromhex(case["wireHex"])) if protobuf else \
        json.loads(case["responseText"], object_pairs_hook=collections.OrderedDict)
    expected["meta"] = None
    expected["market"] = None
    expected["profile"] = None
    expected["safety"] = twin["safety"]
    expected["errors"] = collections.OrderedDict((k, v) for k, v in expected["errors"].items() if k != "profile")
    expected["cache"]["profile"] = dict(expected["cache"]["market"])
    want = struct_wire(expected) if protobuf else \
        json.dumps(expected, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    ok = check(status == 200 and payload == want and want != expected_body(case),
               f"{case['name']}: native is not the unlisted answer {first_difference(want, payload)}")
    for name in HEADERS:
        if name not in ("x-rate-limit-remaining", "content-length"):
            ok &= check(headers.get(name) == case["headers"].get(name), f"{case['name']}: header {name}")
    ok &= check(headers.get("content-length") == str(len(payload)), f"{case['name']}: content-length")
    reduced = dict(case, upstream=[c for c in case["upstream"] if not (
        "coingecko.com" in c["url"] and "/contract/" not in c["url"])])
    return compare_upstream(h, case["name"], reduced, observed, case["group"]) and ok


def restart_durable_swr(h, case, actual, observed):
    """20 s after a restart: the SWR envelopes are in the durable KV natively,
    so the read is served from them (hit: true, the first process's
    timestamps) without a provider request, where TypeScript's new process
    (an in-memory Kv) reads every section live. Every other byte is equal:
    the providers answer the same."""
    status, headers, payload = actual
    before = next(c for c in h.fixture["cases"] if c.get("name") == "restart-before")
    typescript = json.loads(case["responseText"])
    native = json.loads(payload)
    stored = json.loads(before["responseText"])["cache"]
    as_of = {k: {"hit": True, "timestamp": v["timestamp"]} for k, v in stored.items()}
    ok = check(status == 200 and native == dict(typescript, cache=as_of) and native != typescript,
               f"{case['name']}: native {payload[:300]!r}")
    return check(not observed, f"{case['name']}: provider requests {observed}") and ok


def restart_state(h, case, server):
    """After the restart TypeScript's live re-read (above) rewrote the USDT
    token_facts row; native's is still the first process's, exactly as
    TypeScript recorded it before the restart (restart-a-state). The BNB rows
    and every envelope (all expired after eight days) are equal."""
    before = next(c for c in h.fixture["cases"] if c.get("name") == "restart-a-state")
    usdt = "erc20:0xdac17f958d2ee523a2206206994597c13d831ec7"
    rows = [next(r for r in before["rows"] if r["tokenId"] == usdt) if row["tokenId"] == usdt else row
            for row in case["rows"]]
    changed = [r for r in case["rows"] if r["tokenId"] == usdt] != [r for r in rows if r["tokenId"] == usdt]
    return check(changed, f"{case['name']}: TypeScript's row did not move") and \
        replay_state(h, server, dict(case, rows=rows))


ASSERTED = {
    **{name: errors_order for name in (
        "markets-503", "markets-500", "markets-html", "markets-truncated", "markets-429", "profile-503",
        "profile-404", "profile-html", "profile-truncated", "profile-odd-fields", "contract-odd-names",
        "dex-503", "dex-html", "dex-truncated", "ethplorer-500", "ethplorer-html", "all-down", "swr-cold-down")},
    **{name: unlisted_coin_id for name in (
        "contract-number-id", "contract-number-id-protobuf", "contract-long-id", "contract-long-id-protobuf")},
    "concurrent-cold-one": late_follower,
    "concurrent-cold-spellings": late_follower,
    "restart-after-fresh": restart_durable_swr,
    **{name: parser_baseline for name in (
        "no-auth-long-network", "network-101", "network-escaped-101", "network-astral-51", "ref-bang-101",
        "ref-astral-51", "network-raw-utf8", "ref-raw-utf8", "quota-over-long")},
    **{name: bad_url for name in (
        "network-bad-escape", "network-truncated-escape", "network-invalid-utf8", "ref-bad-escape",
        "ref-invalid-utf8")},
}


# ---- the replay ----------------------------------------------------------------------------------

class Harness:
    def __init__(self, binary, root, fixture):
        self.binary = binary
        self.root = Path(root)
        self.fixture = fixture
        self.table = CaseTable()
        self.upstream = Upstream(root, self.table)
        self.clock = self.root / "clock"
        self.lanes = Lanes()
        self.secrets = [fixture["keys"]["coingecko"], fixture["keys"]["ethplorer"]]
        self.client_values = list(fixture["clientHeaders"].values())

    def server(self, kv_name, env, start):
        kv_dir = self.root / f"kv-{kv_name}"
        kv_dir.mkdir(exist_ok=True)
        harness.set_clock(self.clock, start)
        return Server(self.binary, kv_dir, self.clock, self.upstream, start, env)

    def no_secret(self, payload, label):
        text = payload.decode("utf-8", "replace") if isinstance(payload, bytes) else payload
        for secret in self.secrets + ["fintel_dev_"] + [device_id(letter) for letter in harness.TOKEN_LETTERS]:
            check(secret not in text, f"{label}: secret {secret[:16]}… in a response")


def settle(server, upstream, before, expected):
    deadline = time.time() + 12
    while time.time() < deadline and len(upstream.calls()) < before + expected:
        time.sleep(.02)
    server.probe("GET", "/probe/test-token-idle")
    time.sleep(.08)
    server.probe("GET", "/probe/test-token-idle")
    return upstream.calls()[before:]


def compare_upstream(h, label, case, observed, group):
    expected = sorted(key for call in case["upstream"]
                      for key in comparable(call["method"], call["url"], call.get("requestBody") or ""))
    actual = sorted(key for method, host, target, body, _ in observed
                    for key in comparable(method, f"https://{host}{target}", body))
    ok = check(actual == expected, f"{label}: upstream\n   expected {expected}\n   actual   {actual}")
    identity = {}
    for call in case["upstream"]:
        host = urlsplit(call["url"]).netloc
        identity.setdefault(host, {k: v for k, v in call["headers"].items() if k in IDENTITY_HEADERS})
    for method, host, target, body, headers in observed:
        sent = f"{target}{json.dumps(headers)}{body}"
        for secret in h.client_values + ["fintel_dev_"] + [device_id(letter) for letter in harness.TOKEN_LETTERS]:
            ok &= check(secret not in sent, f"{label}: client value {secret[:20]} reached {host}")
        if not host.endswith("coingecko.com"):
            ok &= check(h.secrets[0] not in sent, f"{label}: the CoinGecko key reached {host}")
        if host != "api.ethplorer.io":
            ok &= check(h.secrets[1] not in sent, f"{label}: the Ethplorer key reached {host}")
        if host in identity:
            mine = {k: v for k, v in headers.items() if k in IDENTITY_HEADERS}
            ok &= check(mine == identity[host], f"{label}: {host} identity headers {mine} != {identity[host]}")
    return ok


def replay_state(h, server, case):
    body = json.dumps({"keys": case["state"]["envelopes"], "tokens": case["state"]["tokens"]}).encode()
    status, payload = server.probe("POST", "/probe/test-token-state", body)
    if not check(status == 200, f"{case['name']}: state probe {status} {payload[:200]!r}"):
        return False
    actual = json.loads(payload)
    ok = True
    for want, got in zip(case["envelopes"], actual["envelopes"]):
        ok &= check(want == got, f"{case['name']}: envelope {want['key']}\n   expected {json.dumps(want)[:900]}\n"
                                 f"   actual   {json.dumps(got)[:900]}")
    for want, got in zip(case["rows"], actual["rows"]):
        facts = normalized_facts(want["facts"])
        ok &= check(facts == got["facts"], f"{case['name']}: facts {want['network']}/{want['tokenId']}\n"
                                           f"   expected {json.dumps(facts)[:1500]}\n   actual   {json.dumps(got['facts'])[:1500]}")
        registry = normalized_registry(want["registry"])
        ok &= check(registry == got["registry"], f"{case['name']}: registry {want['network']}/{want['tokenId']} "
                                                 f"{registry} != {got['registry']}")
    return ok


def normalized_facts(row):
    """TypeScript's row with Dates as epoch ms (the native document's form)."""
    if row is None:
        return None
    out = {}
    for key, value in row.items():
        if isinstance(value, dict) and set(value) == {"date"}:
            out[key] = value["date"]
        elif isinstance(value, dict) and set(value) == {"undefined"}:
            continue
        else:
            out[key] = value
    return out


def normalized_registry(row):
    """The registry fields the native identity record keeps."""
    if row is None:
        return None
    return {key: row[key] for key in ("symbol", "name", "logoMediaKey", "coingeckoId")
            if key in row and row[key] is not None}


def replay_concurrent(h, server, case):
    harness.set_clock(h.clock, case["at"])
    h.table.start_case(case["upstream"], case.get("mode"))
    h.lanes.reserve(case["upstream"])
    before = len(h.upstream.calls())
    results = [None] * len(case["concurrent"])
    barrier = threading.Barrier(len(results))

    def run(index, item):
        target = item.get("target") or bytes.fromhex(case["targetHex"]).decode("latin1")
        barrier.wait()
        results[index] = raw(server.port, "HEAD" if item["kind"] == "head" else "GET", target,
                             request_headers(case, item["kind"]))

    threads = [threading.Thread(target=run, args=(i, item)) for i, item in enumerate(case["concurrent"])]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    observed = settle(server, h.upstream, before, len(case["upstream"]))
    for result in results:
        h.no_secret(result[2], case["name"])
    if case["name"] in ASSERTED:
        return ASSERTED[case["name"]](h, case, results, observed)
    ok = True
    for index, (record, result) in enumerate(zip(case["responses"], results)):
        ok &= compare(f"{case['name']}[{index}]", record, *result)
    ok &= compare_upstream(h, case["name"], case, observed, case["group"])
    return ok


def replay_case(h, server, case):
    name = case["name"]
    harness.set_clock(h.clock, case["at"])
    h.table.start_case(case["upstream"], case.get("mode"))
    h.lanes.reserve(case["upstream"])
    before = len(h.upstream.calls())
    target = bytes.fromhex(case["targetHex"]).decode("latin1")
    status, headers, payload = raw(server.port, case["method"], target, request_headers(case))
    observed = settle(server, h.upstream, before, len(case["upstream"]))
    h.lanes.breaker(case["upstream"])
    h.no_secret(payload, name)
    h.no_secret(json.dumps(headers), name)
    if name in ASSERTED:
        return ASSERTED[name](h, case, (status, headers, payload), observed)
    if compat_expectations.malformed_path_target(case):
        # opaque-params: an empty, control-character or over-long
        # path value is the router's 404 before any handler.
        ok = check(compat_expectations.router_refused(status, headers, payload, observed),
                   f"{name}: malformed path value is not the router's 404")
        return ok
    if name in ROUTER_404:
        return check(status == case["status"] == 404, f"{name}: router status {status}")
    ok = compare(name, case, status, headers, payload, remaining=case["group"] == "quota")
    ok &= compare_upstream(h, name, case, observed, case["group"])
    return ok


def sustained(h, server):
    """Native-only: 120 reads from 12 clients of two cached tokens (JSON,
    protobuf, HEAD) give one byte string per token and kind and reach no
    provider (device rotation keeps each client inside its quota)."""
    targets = ["/v1/networks/eth-mainnet/token/0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48",
               "/v1/networks/bnb/token/0x0e09fabb73bd3ade0a17ecc321fd13a19e81ce82"]
    letters = "EFGIJKLMNOPQ"
    before = len(h.upstream.calls())
    results = collections.defaultdict(set)
    statuses = collections.Counter()
    lock = threading.Lock()

    def client(index):
        for n in range(10):
            kind = ("json", "protobuf", "head")[(index + n) % 3]
            target = targets[(index + n) % 2]
            headers = [("authorization", "Bearer " + token(letters[index]))]
            if kind == "protobuf":
                headers.append(("accept", "application/protobuf"))
            status, response_headers, payload = raw(server.port, "HEAD" if kind == "head" else "GET", target, headers)
            with lock:
                statuses[status] += 1
                results[(target, kind)].add(payload if kind != "head" else response_headers.get("content-length"))

    threads = [threading.Thread(target=client, args=(i,)) for i in range(12)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    server.probe("GET", "/probe/test-token-idle")
    calls = h.upstream.calls()[before:]
    ok = check(statuses == collections.Counter({200: 120}) and all(len(v) == 1 for v in results.values()) and
               len(results) == 6 and not calls,
               f"sustained: statuses {dict(statuses)}, variants {[len(v) for v in results.values()]}, calls {calls[:3]}")
    print(f"  sustained: 120 reads from 12 clients, {len(results)} distinct answers, {len(calls)} provider calls",
          flush=True)
    return ok


def production(binary, root):
    """The production composition: the token routes ride the price canary."""
    root = Path(root)
    (root / "prod-tls").mkdir()
    ca, _, _, _ = tls_ca.create_certificates(root / "prod-tls", "DNS:localhost")
    path = "/v1/networks/eth-mainnet/token/native"
    ok = True

    def start(name, extra):
        kv_dir = root / name
        kv_dir.mkdir()
        port = harness.free_port()
        env = {k: v for k, v in os.environ.items()
               if not k.startswith("FORTIS_") and k not in ("ALCHEMY_API_KEY", "ATTESTATION_MODE", "NODE_ENV")}
        env.update({"FORTIS_KV_DIR": str(kv_dir), "PORT": str(port), "FORTIS_CA_BUNDLE_PATH": str(ca)}, **extra)
        process = subprocess.Popen([binary, str(port)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(400):
            if process.poll() is not None:
                raise RuntimeError(f"production server {name} exited")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.1):
                    return process, port
            except OSError:
                time.sleep(.03)
        raise RuntimeError("production server did not start")

    for value in (None, "0"):
        process, port = start(f"kv-prod-{value}", {} if value is None else {"FORTIS_NATIVE_PRICE_ROUTE_CANARY": value})
        try:
            status, _, _ = raw(port, "GET", path, [("authorization", "Bearer " + token("A"))])
            ok &= check(status == 404, f"production canary {value!r}: {status}, not 404")
        finally:
            process.terminate()
            process.wait(timeout=30)
    process, port = start("kv-prod-on", {"FORTIS_NATIVE_PRICE_ROUTE_CANARY": "1"})
    try:
        status, headers, body = raw(port, "GET", path, [])
        ok &= check(status == 401 and body == b'{"error":"UNAUTHORIZED"}' and "vary" not in headers,
                    f"production no-auth: {status} {body!r}")
        status, _, body = raw(port, "GET", path, [("origin", "https://evil.example")])
        ok &= check(status == 403 and body == b'{"error":"BROWSER_ORIGIN_FORBIDDEN"}', f"production origin: {status}")
        status, _, body = raw(port, "GET", "/v1/networks/" + "n" * 101 + "/token/native", [])
        ok &= check(status == 414, f"production 414: {status}")
    finally:
        process.terminate()
        process.wait(timeout=30)
    print("  production: canary unset/0 answers 404; on, no bearer 401, Origin 403, long param 414", flush=True)
    return ok


def drain_quota(server, letter, remaining_after):
    """Device `letter`'s wall-clock minute drained to `remaining_after`."""
    while True:
        if time.time() % 60 > 40:
            time.sleep(61 - time.time() % 60)
        remaining = None
        for _ in range(60):
            status, headers, _ = raw(server.port, "GET", "/v1/networks/nowhere/token/native",
                                     [("authorization", "Bearer " + token(letter))])
            remaining = int(headers.get("x-rate-limit-remaining", "-1"))
            if remaining <= remaining_after:
                break
        if remaining == remaining_after:
            return
        time.sleep(61 - time.time() % 60)


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = load_fixture()
    if not self_check_codec(fixture):
        raise SystemExit("the Struct codec does not reproduce TypeScript's wire")
    only = [g for g in os.environ.get("R19_ONLY_GROUPS", "").split(",") if g]
    with tempfile.TemporaryDirectory(prefix="fortis-r19-") as root:
        h = Harness(binary, root, fixture)
        servers = {}
        try:
            for group in fixture["groups"]:
                name = group["name"]
                if only and name not in only and not (name == "restart-a" and "restart-b" in only):
                    continue
                selected = [c for c in fixture["cases"] if c["group"] == name]
                previous = servers.pop(group["kv"], None)
                if previous:
                    previous.stop(kill=True)
                server = h.server(group["kv"], group["env"], group["start"])
                servers[group["kv"]] = server
                if name == "quota":
                    drain_quota(server, "D", 5)
                for case in selected:
                    if "state" in case and case["name"] == "restart-state":
                        ok = restart_state(h, case, server)
                    elif "state" in case:
                        ok = replay_state(h, server, case)
                    elif "concurrent" in case:
                        ok = replay_concurrent(h, server, case)
                    else:
                        ok = replay_case(h, server, case)
                    kind = "asserted" if case["name"] in ASSERTED or case["name"] in ROUTER_404 or \
                        case["name"] == "restart-state" else "matched"
                    COUNTS[(name, kind if ok else "failed")] += 1
                if name == "main":
                    COUNTS[("sustained", "matched" if sustained(h, server) else "failed")] += 1
                h.no_secret(server.text(), f"{name} process output")
                if group["kv"] != "restart" or name == "restart-b":
                    server.stop()
                    servers.pop(group["kv"], None)
        finally:
            for server in servers.values():
                server.stop()
            h.upstream.close()
        if len(sys.argv) == 3 and not only:
            COUNTS[("production", "matched" if production(sys.argv[2], root) else "failed")] += 1
        if h.table.extra:
            print("requests no TypeScript case made:", sorted(set(h.table.extra))[:20])
    for (group, kind), count in sorted(COUNTS.items()):
        print(f"  {group:12} {kind:9} {count}")
    matched = sum(v for (g, k), v in COUNTS.items() if k == "matched")
    asserted = sum(v for (g, k), v in COUNTS.items() if k == "asserted")
    failed = sum(v for (g, k), v in COUNTS.items() if k == "failed")
    print(f"token360 edges: {matched} matched, {asserted} asserted differences, {failed} failed "
          f"({len(FAILURES)} failure messages)")
    if FAILURES:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
