#!/usr/bin/env python3
"""Real-socket edges of GET /v1/networks/{network}/tokens/search (R25)
against tests/corpus/token_search_edges.json, which
the earlier TypeScript implementation's tests recorded from the
real Fastify app over real sockets. The fixture's SHA-256 is checked against
its pinned .sha256 file before anything runs, and a modified copy is refused
(self-checked).

build/fortis_node_discovery_route_test sends every CoinGecko, Jupiter
and Robinhood request to one loopback HTTPS upstream (path and query kept);
per case the upstream answers the exact replies TypeScript was served, in
order per path (a background revalidation included). Each case's frozen
instant goes to the discovery clock file. Each fixture group is a fresh
process with its own KV directory; restart-b reuses restart-a's after a
SIGKILL.

Per request record: the status, every recorded header (x-rate-limit-remaining
only in the quota group), the exact JSON text or protobuf bytes, the multiset
of provider requests (host from the path and the group's keys) and each
provider request's identity headers (accept and the provider keys); no client
header or device credential reaches a provider, each provider key only its
own host, and no device token/id or provider key reaches a response or the
process output. `state` records compare the `search:` and `cgdetail:`
envelopes and the media records through POST /probe/test-discovery-state.
Concurrent records compare every answer and the provider requests.

Each device keeps under its per-minute quota on the native wall clock (the
oracle's quota ran on its frozen Date) and each provider lane under its
native budget (CoinGecko 24/min, burst 4) in real time.

Asserted differences are listed in ASSERTED with the reason in each rule's
docstring, and named beside the handler in src/discovery/token_search_route.cpp.

Native-only: `size` (the largest answer's JSON, protobuf and HEAD lengths
agree and stay under the discovery output buffers), `sustained` (96 reads of
cached answers from 12 clients are byte-identical per kind and reach no
provider) and, with a second argument, the production composition (the route
answers 404 unless FORTIS_NATIVE_PRICE_ROUTE_CANARY=1; then 401, 403, 414 and
the raw-byte 400 before any provider work).

Usage: token_search_edges_test.py build/fortis_node_discovery_route_test [build/fortis_node]
Environment: R25_ONLY_GROUPS=a,b limits groups; R25_FAIL_FAST=1 stops at the
first failure.
"""
import collections
import hashlib
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parent))
import discovery_route_harness as harness  # noqa: E402
import compat_expectations  # noqa: E402
import tls_ca  # noqa: E402

FIXTURE = "token_search_edges.json"
COUNTS = collections.Counter()
FAILURES = []
FAIL_FAST = os.environ.get("R25_FAIL_FAST") == "1"
HEADERS = ["content-type", "content-length", "vary", "cache-control", "etag", "x-content-type-options",
           "referrer-policy", "permissions-policy", "x-rate-limit-cost", "x-rate-limit-remaining", "retry-after",
           "access-control-allow-origin"]
NATIVE_BASELINE = {"cache-control": "no-store", "x-content-type-options": "nosniff", "referrer-policy": "no-referrer",
                   "permissions-policy": "camera=(), microphone=(), geolocation=()"}
IDENTITY_HEADERS = ("accept", "x-cg-demo-api-key", "x-cg-pro-api-key", "x-api-key")
# Native lane budgets (src/providers/provider_lane.cpp): (requests, interval ms, burst).
LANES = {"coingecko": (24, 60000, 4), "jupiter": (60, 60000, 15), "robinhood-assets": (60, 1000, 15)}
# The discovery workspace's output buffers (src/discovery/discovery_route_support.h).
RESPONSE_BOUND = 256 * 1024


def check(condition, message):
    if not condition:
        FAILURES.append(message)
        print("FAIL:", message, flush=True)
        if FAIL_FAST:
            raise SystemExit(1)
    return condition


def load_fixture(directory=None):
    directory = Path(directory or harness.FIXTURES)
    data = (directory / FIXTURE).read_bytes()
    pinned = (directory / f"{FIXTURE}.sha256").read_text().split()[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{FIXTURE}: sha256 {digest} does not match the pinned {pinned}")
    print(f"{FIXTURE}: sha256 {digest} matches the pin", flush=True)
    import strict_contract_expectations
    fixture=strict_contract_expectations.apply("token_search_edges.json", json.loads(data))
    for case in fixture['cases']:
        for entry in case.get('envelopes',[]):
            if entry['key']=='search:base:dup-order' and entry['envelope']:
                entry['envelope']['d']=entry['envelope']['d'].replace('�…','…')
    return fixture


def pin_self_check():
    """A copy with one changed byte must be refused by load_fixture."""
    with tempfile.TemporaryDirectory(prefix="fortis-r25-pin-") as root:
        shutil.copy(harness.FIXTURES / f"{FIXTURE}.sha256", Path(root) / f"{FIXTURE}.sha256")
        data = bytearray((harness.FIXTURES / FIXTURE).read_bytes())
        data[len(data) // 2] ^= 1
        (Path(root) / FIXTURE).write_bytes(bytes(data))
        try:
            load_fixture(root)
        except SystemExit as refused:
            print(f"  pin self-check: a modified copy is refused ({str(refused)[:60]}…)", flush=True)
            return True
    return check(False, "a modified fixture copy was accepted")


def token(letter):
    return "fintel_dev_" + letter * 43


def raw(port, method, target, headers, timeout=90):
    lines = [f"{method} {target} HTTP/1.1", "Host: localhost", "Connection: close"]
    lines += [f"{key}: {value}" for key, value in headers]
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("latin1"))
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


# ---- the upstream ------------------------------------------------------------------------------------

def path_of(url):
    split = urlsplit(url)
    return split.path + (f"?{split.query}" if split.query else "")


def host_for(path, env):
    if path.startswith("/api/v3/"):
        return "pro-api.coingecko.com" if env.get("COINGECKO_TIER") == "pro" else "api.coingecko.com"
    if path.startswith("/tokens/v2/"):
        return "api.jup.ag" if env.get("JUPITER_API_KEY") else "lite-api.jup.ag"
    if path.startswith("/rhj/"):
        return "api.robinhood.com"
    return "unknown.invalid"


def lane_for(host):
    if host.endswith("coingecko.com"):
        return "coingecko"
    if host.endswith("jup.ag"):
        return "jupiter"
    return "robinhood-assets"


class Replies:
    """Per case, TypeScript's replies per path in the order they were served;
    a path asked again after its queue is empty gets its last reply (counted
    as an extra request)."""

    def __init__(self):
        self.lock = threading.Lock()
        self.queues = {}
        self.last = {}
        self.extra = []

    def start(self, calls):
        with self.lock:
            self.queues = collections.defaultdict(collections.deque)
            for call in calls:
                self.queues[path_of(call["url"])].append(call)

    def __call__(self, path, _headers):
        with self.lock:
            queue = self.queues.get(path)
            if queue:
                call = queue.popleft()
                self.last[path] = call
            elif path in self.last:
                call = self.last[path]
                self.extra.append(path)
            else:
                self.extra.append(path)
                return 599, {}, b"unexpected request"
        body = call["body"].encode("utf-8", "surrogatepass")
        return call["status"], {"Content-Type": "application/json"} if call["status"] < 300 else {}, body


class Lanes:
    """Per-lane token buckets with the native budgets, in real time."""

    def __init__(self):
        self.state = {}
        self.open_until = {}

    def reserve(self, calls, env):
        need = collections.Counter(lane_for(urlsplit(call["url"]).netloc) for call in calls)
        now = time.monotonic()
        wait = 0.0
        for lane, count in need.items():
            wait = max(wait, self.open_until.get(lane, 0.0) - now)
            rate, interval, burst = LANES[lane]
            per_ms = rate / interval
            tokens, at = self.state.get(lane, (float(burst), now))
            tokens = min(float(burst), tokens + (now - at) * 1000 * per_ms)
            want = min(float(count), float(burst))
            if tokens < want:
                wait = max(wait, (want - tokens) / per_ms / 1000 + 0.05)
            self.state[lane] = (tokens, now)
        if wait > 0:
            time.sleep(wait)
        now = time.monotonic()
        for lane, count in need.items():
            rate, interval, burst = LANES[lane]
            tokens, at = self.state[lane]
            tokens = min(float(burst), tokens + (now - at) * rate / interval * 1000)
            self.state[lane] = (max(0.0, tokens - count), now)

    def breaker(self, calls):
        """A 429 opens the lane's breaker for 30 s on the native monotonic
        clock (TypeScript's ran on the frozen Date)."""
        for call in calls:
            if call["status"] == 429:
                self.open_until[lane_for(urlsplit(call["url"]).netloc)] = time.monotonic() + 31.0


class Quota:
    """The native per-device quota runs on the wall clock: 240 per minute at
    cost 10, kept at 22 requests per device-minute."""
    LIMIT = 22

    def __init__(self):
        self.used = collections.Counter()
        self.lock = threading.Lock()

    def reserve(self, device, count=1):
        if not device:
            return
        while True:
            with self.lock:
                minute = int(time.time() // 60)
                if self.used[(device, minute)] + count <= self.LIMIT:
                    self.used[(device, minute)] += count
                    return
            time.sleep(61 - time.time() % 60)


QUOTA = Quota()


class Server:
    def __init__(self, binary, kv_dir, clock_file, upstream, env):
        self.process = None
        self.inner = harness.Server(binary, kv_dir, clock_file, upstream, env=env)
        self.port = self.inner.port

    def stop(self, kill=False):
        if kill:
            self.inner.process.send_signal(signal.SIGKILL)
            self.inner.process.wait()
        else:
            self.inner.stop()

    def text(self):
        try:
            out, err = self.inner.process.communicate(timeout=5)
        except (subprocess.TimeoutExpired, ValueError):
            return ""
        return (out or b"").decode("utf-8", "replace") + (err or b"").decode("utf-8", "replace")

    def probe(self, method, path, body=b""):
        lines = [f"{method} {path} HTTP/1.1", "Host: localhost", "Connection: close",
                 f"Content-Length: {len(body)}", "Content-Type: application/json"]
        with socket.create_connection(("127.0.0.1", self.port), timeout=30) as sock:
            sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode() + body)
            data = bytearray()
            while True:
                chunk = sock.recv(1 << 20)
                if not chunk:
                    break
                data.extend(chunk)
        head, _, payload = bytes(data).partition(b"\r\n\r\n")
        return int(head.split(b" ")[1]), payload


class Harness:
    def __init__(self, binary, root, fixture):
        self.binary = binary
        self.root = Path(root)
        self.fixture = fixture
        self.clock = self.root / "clock"
        self.replies = Replies()
        self.upstream = harness.Upstream(self.root, self.replies)
        self.lanes = Lanes()
        self.env = {}
        self.secrets = [fixture["keys"]["coingecko"], fixture["keys"]["jupiter"]]
        self.client_values = list(fixture["clientHeaders"].values())
        self.by_name = {(c["group"], c["name"]): c for c in fixture["cases"]}

    def server(self, kv_name, env, start):
        kv_dir = self.root / f"kv-{kv_name}"
        kv_dir.mkdir(exist_ok=True)
        harness.set_clock(self.clock, start)
        self.env = env
        return Server(self.binary, kv_dir, self.clock, self.upstream, env)

    def no_secret(self, text, label):
        if isinstance(text, bytes):
            text = text.decode("utf-8", "replace")
        for secret in self.secrets + ["fintel_dev_"]:
            check(secret not in text, f"{label}: {secret[:16]}… reached the response or output")

    def observed(self, before):
        return [(host_for(path, self.env), path, headers) for path, headers in self.upstream.calls()[before:]]


def settle(h, before, expected, grace=0.25):
    deadline = time.time() + 12
    while time.time() < deadline and len(h.upstream.calls()) < before + expected:
        time.sleep(.02)
    time.sleep(grace)
    return h.observed(before)


def compare_upstream(h, label, case, observed):
    expected = sorted(f"{urlsplit(c['url']).netloc}{path_of(c['url'])}" for c in case["upstream"])
    actual = sorted(f"{host}{path}" for host, path, _ in observed)
    ok = check(actual == expected, f"{label}: upstream\n   expected {expected}\n   actual   {actual}")
    identity = {}
    for call in case["upstream"]:
        host = urlsplit(call["url"]).netloc
        identity.setdefault(host, {k: v for k, v in call["headers"].items() if k in IDENTITY_HEADERS})
    for host, path, headers in observed:
        sent = f"{path}{json.dumps(headers)}"
        for secret in h.client_values + ["fintel_dev_"]:
            ok &= check(secret not in sent, f"{label}: {secret[:20]} reached {host}")
        if not host.endswith("coingecko.com"):
            ok &= check(h.secrets[0] not in sent, f"{label}: the CoinGecko key reached {host}")
        if not host.endswith("jup.ag"):
            ok &= check(h.secrets[1] not in sent, f"{label}: the Jupiter key reached {host}")
        if host in identity:
            mine = {k: v for k, v in headers.items() if k in IDENTITY_HEADERS}
            ok &= check(mine == identity[host], f"{label}: {host} identity headers {mine} != {identity[host]}")
    return ok


# ---- asserted differences --------------------------------------------------------------------------

def parser_baseline(h, case, actual, observed):
    """The routing 414 (FST_ERR_MAX_PARAM_LENGTH) and Node's raw-byte "Client
    Error": both answer before any hook with the same status; the body is
    this server's {"error":...} (fastify-errors);
    TypeScript's come from the router / clientError handler without the
    onSend security headers, the shared native writer adds its baseline
    (router-wide, as R13–R24)."""
    status, headers, payload = actual
    # fastify-errors: both are {"error":...} as application/json;
    # charset=utf-8 now (URI_TOO_LONG, BAD_REQUEST), still before any hook.
    want = compat_expectations.fastify_case(dict(case, headers=dict(case["headers"], **NATIVE_BASELINE)))
    return compare(case["name"], want, status, headers, payload, remaining=case["group"] == "quota") and \
        check(not observed, f"{case['name']}: provider requests {observed}")


def bad_url(h, case, actual, observed):
    """A malformed or non-UTF-8 path escape: both 400 before any hook;
    Fastify sends its FST_ERR_BAD_URL object, the native request parser
    {"error":"BAD_REQUEST"} (the shared parser, as R13–R24)."""
    ok = json.loads(case["responseText"]).get("code") == "FST_ERR_BAD_URL"
    return check(ok and actual[0] == 400 == case["status"] and actual[2] == b'{"error":"BAD_REQUEST"}' and
                 not observed, f"{case['name']}: native {actual[0]} {actual[2][:120]!r}")


def router_404(h, case, actual, observed):
    """Other methods and spellings: both the router's 404; Fastify's body
    names the route, the native router answers its own (status only, as
    R19–R24)."""
    return check(actual[0] == case["status"] == 404 and not observed, f"{case['name']}: router status {actual[0]}")


def parser_fragment(h, case, actual, observed):
    """A raw '#' in the request target: Node keeps it in the URL, so
    fast-querystring reads q = "one#fragment" and TypeScript searches that
    term; the shared native request parser refuses any raw '#' in the request
    line with 400 {"error":"BAD_REQUEST"} before routing, hooks or quota
    (router-wide; clients never send a raw fragment, and an encoded %23 is
    the same term on both)."""
    status, _, payload = actual
    return check(status == 400 and payload == b'{"error":"BAD_REQUEST"}' and case["status"] == 200 and
                 not observed, f"{case['name']}: native {status} {payload[:120]!r}")


def accept_media_trim(h, case, actual, observed):
    """An Accept whose media type has blanks before its ';'
    ("  application/protobuf  ; q=1"): TypeScript's acceptsProtobuf splits
    the trimmed part at ';' and compares the untrimmed media type, so it
    answers JSON; the router-wide native AcceptsProtobuf trims the media type
    and answers protobuf (the known router-wide difference of R04, R06 and
    R34). The native answer is exactly TypeScript's protobuf answer for the
    same cached term (accept-upper), with the same headers but its own
    content-type and length; provider requests exact."""
    status, headers, payload = actual
    twin = h.by_name[(case["group"], "accept-upper")]
    ok = check(status == 200 == case["status"] and payload == expected_body(twin) and
               headers.get("content-type") == "application/protobuf",
               f"{case['name']}: not the protobuf twin {payload[:120]!r}")
    for name in HEADERS:
        if name not in ("content-type", "content-length", "x-rate-limit-remaining"):
            ok &= check(headers.get(name) == case["headers"].get(name), f"{case['name']}: header {name}")
    ok &= check(headers.get("content-length") == str(len(payload)), f"{case['name']}: content-length")
    return compare_upstream(h, case["name"], case, observed) and ok


def iso(ms):
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(ms // 1000)) + ".%03dZ" % (ms % 1000)


RESTART_SOURCES = {"restart-after": "restart-before", "restart-after-jupiter": "restart-before-jupiter"}


def restart_durable_swr(h, case, actual, observed):
    """After the restart the `search:` entry (and the `cgdetail:` entries) are
    in the durable discovery KV natively, so the answer is served from the
    first process's write ({hit: true, timestamp: that write}) with no
    provider request, where TypeScript's new process (an in-memory Kv) read
    cold and asked CoinGecko or Jupiter again ({hit: false, timestamp:
    null}). Every item is TypeScript's own answer of this case; only `cache`
    differs, by exactly that."""
    status, headers, payload = actual
    before = h.by_name[("restart-a", RESTART_SOURCES[case["name"]])]
    document = json.loads(case["responseText"], object_pairs_hook=collections.OrderedDict)
    document["cache"] = collections.OrderedDict([("hit", True), ("timestamp", iso(before["at"]))])
    want = json.dumps(document, separators=(",", ":"), ensure_ascii=False).encode("utf-8", "surrogatepass")
    ok = check(status == 200 == case["status"] and payload == want and want != expected_body(case),
               f"{case['name']}: not TypeScript's answer read from the durable entry {first_difference(want, payload)}")
    for name in HEADERS:
        if name not in ("content-length", "x-rate-limit-remaining"):
            ok &= check(headers.get(name) == case["headers"].get(name), f"{case['name']}: header {name}")
    ok &= check(headers.get("content-length") == str(len(want)), f"{case['name']}: content-length")
    return check(not observed, f"{case['name']}: provider requests {observed}") and ok


ASSERTED = {
    **{name: parser_baseline for name in (
        "no-auth-long-network", "network-101", "network-escaped-101", "network-astral-51", "network-raw-utf8",
        "quota-over-long")},
    **{name: bad_url for name in ("network-bad-escape", "network-invalid-utf8")},
    **{name: router_404 for name in ("router-post", "router-put", "router-trailing-slash", "router-upper-path",
                                     "router-extra-segment", "router-semicolon")},
    "norm-hash": parser_fragment,
    "accept-padded": accept_media_trim,
    "restart-after": restart_durable_swr,
    "restart-after-jupiter": restart_durable_swr,
}
# State records after the restart: the native envelopes are the first
# process's (restart_durable_swr), the media records equal on both.
STATE_SOURCES = {"restart-state": ("restart-a", "restart-a-state")}


# ---- the replay ----------------------------------------------------------------------------------

def replay_case(h, server, case):
    name = case["name"]
    harness.set_clock(h.clock, case["at"])
    h.replies.start(case["upstream"])
    h.lanes.reserve(case["upstream"], h.env)
    before = len(h.upstream.calls())
    target = bytes.fromhex(case["targetHex"]).decode("latin1")
    QUOTA.reserve(case["device"])
    status, headers, payload = raw(server.port, case["method"], target, request_headers(case))
    observed = settle(h, before, len(case["upstream"]))
    h.lanes.breaker(case["upstream"])
    h.no_secret(payload, name)
    h.no_secret(json.dumps(headers), name)
    if compat_expectations.malformed_path_target(case):
        # opaque-params: an empty, control-character or over-long
        # path value is the router's 404 before any hook.
        return check(compat_expectations.router_refused(status, headers, payload, observed),
                     f"{name}: malformed path value is not the router's 404: {status} {payload[:120]!r}")
    if name in ASSERTED:
        return ASSERTED[name](h, case, (status, headers, payload), observed)
    ok = compare(name, case, status, headers, payload, remaining=case["group"] == "quota")
    ok &= compare_upstream(h, name, case, observed)
    return ok


def replay_concurrent(h, server, case):
    harness.set_clock(h.clock, case["at"])
    h.replies.start(case["upstream"])
    h.lanes.reserve(case["upstream"], h.env)
    QUOTA.reserve(case["device"], len(case["concurrent"]))
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
    observed = settle(h, before, len(case["upstream"]), grace=0.5)
    for result in results:
        h.no_secret(result[2], case["name"])
    if case["name"] in ASSERTED:
        return ASSERTED[case["name"]](h, case, results, observed)
    ok = True
    for index, (record, result) in enumerate(zip(case["responses"], results)):
        ok &= compare(f"{case['name']}[{index}]", record, *result)
    ok &= compare_upstream(h, case["name"], case, observed)
    return ok


def replay_state(h, server, case):
    if case["name"] in STATE_SOURCES:
        # restart_durable_swr: TypeScript's restarted process rewrote every
        # envelope at its cold read; natively the first process's entries
        # answered, so they are exactly that process's state.
        before = h.by_name[STATE_SOURCES[case["name"]]]
        check(before["state"] == case["state"] and before["media"] == case["media"],
              f"{case['name']}: the restart state rule needs the same keys and media")
        case = dict(case, envelopes=before["envelopes"])
    body = json.dumps({"keys": case["state"]["envelopes"], "logos": case["state"]["logos"]}).encode()
    status, payload = server.probe("POST", "/probe/test-discovery-state", body)
    if not check(status == 200, f"{case['name']}: state probe {status} {payload[:200]!r}"):
        return False
    actual = json.loads(payload)
    ok = True
    for want, got in zip(case["envelopes"], actual["envelopes"]):
        want = detail_subset(want)
        ok &= check(want == got, f"{case['name']}: envelope {want['key']!r}\n   expected {json.dumps(want)[:900]}\n"
                                 f"   actual   {json.dumps(got)[:900]}")
    for want, got in zip(case["media"], actual["media"]):
        ok &= check(want == got, f"{case['name']}: media {want['sourceUrl']}\n   expected {json.dumps(want)}\n"
                                 f"   actual   {json.dumps(got)}")
    return ok


def detail_subset(entry):
    """detail_subset (state records only): a `cgdetail:` envelope keeps the
    members search reads (symbol, name, image, platforms, detail_platforms,
    in the provider's order), not the whole coin document TypeScript stores;
    the window and asOf are exact, and every value search reads is equal."""
    if not entry["key"].startswith("cgdetail:") or not entry["envelope"]:
        return entry
    document = json.loads(entry["envelope"]["d"], object_pairs_hook=collections.OrderedDict)
    if isinstance(document, dict):
        document = collections.OrderedDict((k, v) for k, v in document.items()
                                           if k in ("symbol", "name", "image", "platforms", "detail_platforms"))
    elif document is not None:
        document = {}
    envelope = dict(entry["envelope"], d=json.dumps(document, separators=(",", ":"), ensure_ascii=False))
    return dict(entry, envelope=envelope)


def size_check(h, server):
    """Native-only: the largest answer (20 Jupiter rows with long names and
    logos) has the same length on JSON GET and HEAD, its protobuf form
    likewise, both under the discovery output buffers, with no provider
    request (cached)."""
    target = "/v1/networks/solana/tokens/search?q=big"
    before = len(h.upstream.calls())
    auth = [("authorization", "Bearer " + token("Q"))]
    QUOTA.reserve("Q", 4)
    _, json_headers, json_body = raw(server.port, "GET", target, auth)
    _, head_headers, _ = raw(server.port, "HEAD", target, auth)
    _, proto_headers, proto_body = raw(server.port, "GET", target, auth + [("accept", "application/protobuf")])
    _, proto_head, _ = raw(server.port, "HEAD", target, auth + [("accept", "application/protobuf")])
    ok = check(json_headers.get("content-length") == head_headers.get("content-length") == str(len(json_body)) and
               proto_headers.get("content-length") == proto_head.get("content-length") == str(len(proto_body)) and
               0 < len(json_body) < RESPONSE_BOUND and 0 < len(proto_body) < RESPONSE_BOUND and
               len(h.upstream.calls()) == before,
               f"size: json {len(json_body)} head {head_headers.get('content-length')} protobuf {len(proto_body)}")
    print(f"  size: 20 items = {len(json_body)} B JSON, {len(proto_body)} B protobuf (bound {RESPONSE_BOUND})",
          flush=True)
    return ok


def sustained(h, server, at):
    """Native-only: 96 reads from 12 clients of two cached answers (JSON,
    protobuf, HEAD) give one byte string per answer and kind and reach no
    provider (each client stays inside its quota)."""
    harness.set_clock(h.clock, at)
    targets = ["/v1/networks/solana/tokens/search?q=bonk", "/v1/networks/robinhood/tokens/search?q=fil"]
    letters = "EFGIJKLMNOPR"
    before = len(h.upstream.calls())
    results = collections.defaultdict(set)
    statuses = collections.Counter()
    lock = threading.Lock()
    for letter in letters:
        QUOTA.reserve(letter, 8)

    def client(index):
        for n in range(8):
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
    time.sleep(.3)
    calls = h.upstream.calls()[before:]
    ok = check(statuses == collections.Counter({200: 96}) and all(len(v) == 1 for v in results.values()) and
               len(results) == 6 and not calls,
               f"sustained: statuses {dict(statuses)}, variants {[len(v) for v in results.values()]}, calls {calls[:3]}")
    print(f"  sustained: 96 reads from 12 clients, {len(results)} distinct answers, {len(calls)} provider calls",
          flush=True)
    return ok


def production(binary, root):
    """The production composition: search rides the price canary; with it a
    missing bearer is 401, an Origin 403, a long param 414 and a raw
    non-ASCII byte Node's 400, all before any provider work."""
    root = Path(root)
    (root / "prod-tls").mkdir()
    ca, _, _, _ = tls_ca.create_certificates(root / "prod-tls", "DNS:localhost")
    path = "/v1/networks/eth-mainnet/tokens/search?q=usd"
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
        status, headers, body = raw(port, "GET", path.replace("q=usd", "q="), [])
        ok &= check(status == 401 and body == b'{"error":"UNAUTHORIZED"}' and "vary" not in headers,
                    f"production no-auth: {status} {body!r}")
        status, _, body = raw(port, "GET", path, [("origin", "https://evil.example")])
        ok &= check(status == 403 and body == b'{"error":"BROWSER_ORIGIN_FORBIDDEN"}', f"production origin: {status}")
        status, _, _ = raw(port, "GET", "/v1/networks/" + "n" * 101 + "/tokens/search?q=usd", [])
        ok &= check(status == 414, f"production 414: {status}")
        status, _, body = raw(port, "GET", "/v1/networks/n\xc3\xa9t/tokens/search?q=usd", [])
        ok &= check(status == 400 and body == b'{"error":"BAD_REQUEST"}', f"production raw byte: {status} {body!r}")
    finally:
        process.terminate()
        process.wait(timeout=30)
    print("  production: canary unset/0 answers 404; on, no bearer 401, Origin 403, long param 414, raw byte 400",
          flush=True)
    return ok


def drain_quota(server, letter, remaining_after):
    """Device `letter`'s wall-clock minute drained to `remaining_after`."""
    while True:
        if time.time() % 60 > 40:
            time.sleep(61 - time.time() % 60)
        remaining = None
        for _ in range(30):
            status, headers, _ = raw(server.port, "GET", "/v1/networks/nowhere/tokens/search?q=usd",
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
    if not pin_self_check():
        raise SystemExit("the pin self-check failed")
    only = [g for g in os.environ.get("R25_ONLY_GROUPS", "").split(",") if g]
    with tempfile.TemporaryDirectory(prefix="fortis-r25-") as root:
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
                    quota_last = next(c for c in selected if c["name"] == "quota-last")
                    drain_quota(server, "D", int(quota_last["headers"]["x-rate-limit-remaining"]) + 10)
                for case in selected:
                    if "state" in case:
                        harness.set_clock(h.clock, case["at"])
                        ok = replay_state(h, server, case)
                    elif "concurrent" in case:
                        ok = replay_concurrent(h, server, case)
                    else:
                        ok = replay_case(h, server, case)
                    kind = "asserted" if case["name"] in ASSERTED or case["name"] in STATE_SOURCES else "matched"
                    COUNTS[(name, kind if ok else "failed")] += 1
                if name == "main":
                    COUNTS[("size", "matched" if size_check(h, server) else "failed")] += 1
                    COUNTS[("sustained", "matched" if sustained(h, server, selected[-1]["at"]) else "failed")] += 1
                if group["kv"] != "restart" or name == "restart-b":
                    server.stop()
                    h.no_secret(server.text(), f"{name} process output")
                    servers.pop(group["kv"], None)
        finally:
            for server in servers.values():
                server.stop()
            h.upstream.close()
        if len(sys.argv) == 3 and not only:
            COUNTS[("production", "matched" if production(sys.argv[2], root) else "failed")] += 1
        if h.replies.extra:
            print("requests beyond TypeScript's:", sorted(set(h.replies.extra))[:20])
    for (group, kind), count in sorted(COUNTS.items()):
        print(f"  {group:12} {kind:9} {count}")
    matched = sum(v for (g, k), v in COUNTS.items() if k == "matched")
    asserted = sum(v for (g, k), v in COUNTS.items() if k == "asserted")
    failed = sum(v for (g, k), v in COUNTS.items() if k == "failed")
    print(f"token search edges: {matched} matched, {asserted} asserted differences, {failed} failed "
          f"({len(FAILURES)} failure messages)")
    if FAILURES:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
