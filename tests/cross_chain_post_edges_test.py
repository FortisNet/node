#!/usr/bin/env python3
"""R41/R42: replays the pinned TypeScript socket oracles of
POST /v1/exchange/cross-chain/quotes (cross_chain_quotes_edges.json) and
POST /v1/exchange/cross-chain/status (cross_chain_status_edges.json),
written by the earlier TypeScript implementation's tests,
against build/fortis_node_cross_chain_route_test (main.cpp built with
-DFORTIS_CROSS_CHAIN_ROUTE_TEST_ONLY: every provider host goes to one
loopback HTTPS upstream named by x-fortis-test-host, the route clock follows
a file, devices A–D are seeded premium).

Each fixture's SHA-256 must equal its .sha256 sidecar (a modified copy is
refused). Each fixture group is a fresh process, as each TypeScript group is
a fresh app; restart-b reuses restart-a's KV directory after a SIGKILL (the
persistent volume). Per record: the status, every recorded response header
(x-rate-limit-remaining in the quota group, drained on the wall clock), the
exact JSON text or protobuf bytes, the coarse latency class (retries ≥ 1 s,
timeouts ≥ 10 s), and the provider requests as a multiset of (method, URL,
body) with their identity headers (and no client header or device
credential). Provider replies are TypeScript's recorded bytes, served after
the recorded delay. TypeScript's lanes and breakers ran on its frozen Date
(61 s between cases); the native ones run on real time, so the replay paces
each provider lane inside its native budget and waits out a 429's breaker.

Named differences are asserted narrowly (the rule names in RULES). Native-only
edges: saturation (more simultaneous quotes than native jobs), production
(the canary flag on the production binary).

Usage: cross_chain_post_edges_test.py build/fortis_node_cross_chain_route_test [build/fortis_node]
Environment: R41_R42_ONLY=quotes|status, R41_R42_GROUPS=main,failure,…,
R41_R42_NATIVE_ONLY=saturation|production|none, R41_R42_FAIL_FAST=1.
"""
import collections
import json
import os
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compat_expectations  # noqa: E402
import cross_chain_route_test as base  # noqa: E402
import discovery_post_edges as edges  # noqa: E402
import tls_ca  # noqa: E402

FIXTURES = {"quotes": "cross_chain_quotes_edges.json", "status": "cross_chain_status_edges.json"}
edges.FAIL_FAST = os.environ.get("R41_R42_FAIL_FAST") == "1"
TRACE = os.environ.get("R41_R42_TRACE") == "1"
check = edges.check
COUNTS = edges.COUNTS
UPSTREAM_HEADERS = ["content-type", "accept", "x-lifi-api-key", "x-integrator-id", "x-api-key", "authorization"]
# Native lane budgets (src/providers/provider_lane.cpp) by provider host: (requests, interval ms, burst).
edges.LANES.clear()
edges.LANES.update({
    "li.quest": (100, 60000, 25), "v2.api.squidrouter.com": (30, 60000, 30),
    "api.axelarscan.io": (30, 60000, 30), "1click.chaindefuser.com": (30, 60000, 30),
    "api.simpleswap.io": (30, 60000, 30), "bridge.vet": (20, 60000, 20),
})
ROTATION = "ABC"
QUOTA_GROUP = "quota"

# Named differences: rule → records, each asserted narrowly below.
RULES = {
    # The native router answers 404 {"error":"NOT_FOUND"} (router-wide);
    # Fastify names the route. Both 404 before any hook.
    "router_404": {"router-get", "router-head", "router-put", "router-trailing-slash", "router-upper"},
    # The redirected test transport (StartRedirected) skips the 250 ms / 1 s sleeps between
    # attempts at an unparseable 2xx (production waits them, as HttpClient):
    # TypeScript's latency class is "retry", the replay's "fast"; requests,
    # replies and answers are compared exactly. Applied to every record
    # whose retried provider replies were all 2xx.
    "test_transport_parse_retry_sleep": set(),
    # Native provider work has a 25 s deadline (the 30 s response handoff);
    # TypeScript has none. A provider hanging past HttpClient's 12 s abort on
    # every attempt gets three attempts in TypeScript (~37 s) and two natively
    # (~24 s): the same 502 and headers; the native requests are the first
    # two of TypeScript's identical three.
    "provider_deadline_25s": {"lifi-timeout-all"},
    # TypeScript's frozen-minute quota answers retry-after 60; the native
    # minute is the wall clock's, so it is the seconds left in it.
    "quota_retry_after_wall_clock": {"quota-over", "quota-over-protobuf", "quota-over-invalid-body",
                                     "quota-over-form", "quota-over-unknown-provider"},
}


def letter_for(record, index):
    device = record.get("device", "A")
    if device == "":
        return ""
    if record["group"] in (QUOTA_GROUP, "attestation") or device not in ROTATION:
        return device
    return ROTATION[index % len(ROTATION)]


def parse_retries_only(record):
    """A provider asked more than once in one record, every reply a 2xx (an
    unparseable body retried), and no delayed reply."""
    per_url = collections.Counter(call["url"] for call in record["upstream"])
    retried = [call for call in record["upstream"] if per_url[call["url"]] > 1]
    return bool(retried) and all(200 <= call["status"] < 300 and not call.get("delayMs") for call in retried)


def latency_class(seconds):
    return "timeout" if seconds >= 10 else "retry" if seconds >= 1 else "fast"


class Upstream:
    """One HTTPS upstream for every provider host: each request is answered
    from the record's table (`METHOD host/path`, a list consumed in order,
    its last entry repeating; a missing key → 404) after the entry's delay."""

    def __init__(self, root):
        self.lock = threading.Lock()
        self.table = {}
        self.consumed = collections.Counter()
        self.observed = []
        self.in_flight = 0
        self.peak = 0
        ca, cert, key, _ = tls_ca.create_certificates(Path(root), "DNS:localhost")
        self.ca = ca
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def handle_any(self):
                length = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(length) if length else b""
                headers = {k.lower(): v for k, v in self.headers.items()}
                host = headers.get("x-fortis-test-host", "")
                key = f"{self.command} {host}{urlsplit(self.path).path}"
                with upstream.lock:
                    upstream.observed.append({
                        "method": self.command, "url": f"https://{host}{self.path}", "key": key,
                        "body": body.decode("utf-8", "surrogateescape") if body else None,
                        "headers": headers,
                    })
                    entries = upstream.table.get(key)
                    if entries is None:
                        reply = {"status": 404, "body": json.dumps({"error": "unexpected request", "key": key},
                                                                   separators=(",", ":")),
                                 "headers": {"content-type": "application/json"}}
                    else:
                        index = upstream.consumed[key]
                        upstream.consumed[key] += 1
                        reply = entries[min(index, len(entries) - 1)]
                    upstream.in_flight += 1
                    upstream.peak = max(upstream.peak, upstream.in_flight)
                try:
                    time.sleep(reply.get("delayMs", 5) / 1000)
                    payload = reply["body"].encode("utf-8", "surrogatepass")
                    self.send_response(reply["status"])
                    for name, value in (reply.get("headers") or {}).items():
                        self.send_header(name, value)
                    self.send_header("Content-Length", str(len(payload)))
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.wfile.write(payload)
                    self.wfile.flush()
                except OSError:
                    pass  # the native transport gave up (its per-attempt timeout)
                finally:
                    with upstream.lock:
                        upstream.in_flight -= 1
                self.close_connection = True

            do_GET = handle_any
            do_POST = handle_any

            def log_message(self, *_args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", base.free_port()), Handler)
        self.server.daemon_threads = True
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.port = self.server.server_address[1]

    def start_case(self, table):
        with self.lock:
            self.table = table
            self.consumed = collections.Counter()
            self.observed = []
            self.peak = 0

    def calls(self):
        with self.lock:
            return list(self.observed)

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def removed_media(record):
    """compat-decisions.md#form-bodies: text/plain and form bodies
    are refused before any provider work."""
    return compat_expectations.is_removed_body_media(edges.request_header(record, "content-type"))


def compare_upstream(label, record, observed, secrets):
    key = lambda call: (call["method"], call["url"], call.get("body"))  # noqa: E731
    want = collections.Counter(key(c) for c in record["upstream"])
    got = collections.Counter(key(c) for c in observed)
    if removed_media(record):
        want = collections.Counter()
    # A concurrent record's text/plain or form members no longer reach a
    # provider: their recorded calls are missing, nothing else may differ.
    dropped = sum(1 for request, response in zip(record.get("concurrent", []), record.get("responses", []))
                  if removed_media(request) and response.get("status") == 200)
    if dropped and not (got - want) and sum((want - got).values()) == dropped:
        want = got
    ok = check(want == got, f"{label}: provider requests differ; missing={list((want - got).elements())[:3]} "
                            f"extra={list((got - want).elements())[:3]}")
    if not ok:
        return False
    by_key = collections.defaultdict(list)
    for call in observed:
        by_key[key(call)].append(call["headers"])
    for call in [] if removed_media(record) or dropped else record["upstream"]:
        headers = by_key[key(call)].pop(0)
        for name in UPSTREAM_HEADERS:
            mine, theirs = headers.get(name), call["headers"].get(name)
            same = (mine or "").lower() == (theirs or "").lower() if name in ("content-type", "accept") \
                else mine == theirs
            ok &= check(same, f"{label}: {call['url'][:70]} header {name}: {mine!r} != {theirs!r}")
    for call in observed:
        sent = call["url"] + (call.get("body") or "") + json.dumps(call["headers"])
        for secret in secrets:
            ok &= check(secret not in sent, f"{label}: {secret[:24]!r} reached a provider")
    return ok


def request_headers(record, letter):
    headers = [] if not letter else [("authorization", "Bearer " + edges.token(letter))]
    return headers + [tuple(pair) for pair in record["requestHeaders"]]


def table_of(record):
    return {key: entries for key, entries in record["providers"].items()}


def no_secret_in_answer(label, fixture, payload, headers):
    text = payload.decode("utf-8", "replace") + json.dumps(headers)
    for secret in fixture["secrets"] + ["fintel_dev_"]:
        check(secret not in text, f"{label}: {secret[:24]!r} in the answer")


class Replay:
    def __init__(self, binary, fixture, kind, root):
        self.binary = binary
        self.fixture = fixture
        self.kind = kind
        self.root = Path(root)
        self.upstream = Upstream(self.root / "tls")
        self.clock = self.root / "clock"
        self.lanes = edges.Lanes()
        self.quota = edges.Quota(limit=1200, margin=60)
        self.envs = {group["name"]: group["env"] for group in fixture["groups"]}
        # What must never reach a provider: client headers and device
        # credentials. (Provider keys reach their own provider: compared per
        # call with TypeScript's identity headers.)
        self.secrets = [value for _, value in fixture["clientHeaders"]] + ["fintel_dev_"] + \
            list(fixture["devices"].values())
        self.index = 0
        self.previous_at = None

    def set_clock(self, at):
        temporary = self.clock.with_suffix(".tmp")
        temporary.write_text(str(at))
        os.replace(temporary, self.clock)

    def start(self, group, kv_name=None):
        kv = self.root / f"kv-{self.kind}-{kv_name or group}"
        kv.mkdir(exist_ok=True)
        return base.Server(self.binary, self.upstream, kv, self.clock, self.envs[group])

    def one(self, server, record):
        name = record["name"]
        label = f"{self.kind}/{record['group']}/{name}"
        letter = letter_for(record, self.index)
        self.index += 1
        # TypeScript's breaker ran on its frozen Date: a record 0–5 s after the
        # previous one saw that much of the 30 s open window go by. The native
        # breaker runs on the monotonic clock, so the replay waits the gap.
        if self.previous_at is not None and 0 < record["at"] - self.previous_at <= 5000:
            time.sleep((record["at"] - self.previous_at) / 1000)
        self.previous_at = record["at"]
        self.set_clock(record["at"])
        if TRACE:
            print(f"  … {label} ({time.strftime('%H:%M:%S')})", flush=True)
        self.lanes.reserve(record["upstream"])
        if letter and letter != "Z" and record["group"] != QUOTA_GROUP:
            self.quota.reserve(letter, 10)
        if TRACE:
            print(f"    paced ({time.strftime('%H:%M:%S')})", flush=True)
        self.upstream.start_case(table_of(record))
        body = None if record["bodyHex"] is None else bytes.fromhex(record["bodyHex"])
        started = time.monotonic()
        status, headers, payload = edges.raw(server.port, record["method"], record["target"],
                                             request_headers(record, letter), body)
        elapsed = time.monotonic() - started
        time.sleep(0.05)
        self.lanes.breaker(record["upstream"])
        observed = self.upstream.calls()
        no_secret_in_answer(label, self.fixture, payload, headers)
        if name in RULES["router_404"]:
            ok = edges.native_plain_error(label, (status, headers, payload), 404, edges.NATIVE_NOT_FOUND,
                                          head=record["method"] == "HEAD")
            ok &= check(record["status"] == 404 and not observed, f"{label}: router record")
            COUNTS[(f"{self.kind}/{record['group']}", "asserted" if ok else "failed")] += 1
            return
        if name in RULES["provider_deadline_25s"]:
            want = [call["url"] for call in record["upstream"]]
            got = [call["url"] for call in observed]
            ok = edges.compare(label, record, status, headers, payload)
            ok &= check(len(want) == 3 and len(set(want)) == 1 and got == want[:2] and 20 <= elapsed < 30,
                        f"{label}: deadline {elapsed:.1f}s, {len(got)} of {len(want)} attempts")
            # The shortened deadline may omit only attempt three. The first
            # two must still have TypeScript's method, URL, body and identity
            # headers, with no client credential sent upstream.
            ok &= compare_upstream(label, {**record, "upstream": record["upstream"][:2]},
                                   observed, self.secrets)
            COUNTS[(f"{self.kind}/{record['group']}", "asserted" if ok else "failed")] += 1
            return
        if name in RULES["quota_retry_after_wall_clock"] and status == 429:
            retry = headers.get("retry-after", "")
            ok = check(retry.isdigit() and 1 <= int(retry) <= 60 and record["headers"].get("retry-after") == "60",
                       f"{label}: retry-after {retry!r}")
            ok &= edges.compare(label, record, status, headers, payload, remaining=True, skip=("retry-after",))
            ok &= compare_upstream(label, record, observed, self.secrets)
            COUNTS[(f"{self.kind}/{record['group']}", "asserted" if ok else "failed")] += 1
            return
        ok = edges.compare(label, record, status, headers, payload, remaining=record["group"] == QUOTA_GROUP)
        ok &= compare_upstream(label, record, observed, self.secrets)
        native_class = latency_class(elapsed)
        if parse_retries_only(record) and native_class != record["elapsedClass"]:
            ok &= check(native_class == "fast" and record["elapsedClass"] == "retry",
                        f"{label}: latency {elapsed:.2f}s ({native_class}) vs TypeScript {record['elapsedClass']}")
            RULES["test_transport_parse_retry_sleep"].add(name)
            COUNTS[(f"{self.kind}/{record['group']}", "asserted" if ok else "failed")] += 1
            return
        ok &= check(native_class == record["elapsedClass"],
                    f"{label}: latency {elapsed:.2f}s ({native_class}) vs TypeScript {record['elapsedClass']}")
        COUNTS[(f"{self.kind}/{record['group']}", "matched" if ok else "failed")] += 1

    def concurrent(self, server, record):
        label = f"{self.kind}/{record['group']}/{record['name']}"
        if TRACE:
            print(f"  … {label}: lane reserve ({time.strftime('%H:%M:%S')})", flush=True)
        self.set_clock(record["at"])
        self.lanes.reserve(record["upstream"])
        if TRACE:
            print(f"  … {label}: lane ready, quota reserve ({time.strftime('%H:%M:%S')})", flush=True)
        self.upstream.start_case(table_of(record))
        letter = record["device"]
        self.quota.reserve(letter, 10 * len(record["concurrent"]))
        if TRACE:
            print(f"  … {label}: quota ready, sending ({time.strftime('%H:%M:%S')})", flush=True)
        results = [None] * len(record["concurrent"])
        barrier = threading.Barrier(len(record["concurrent"]))

        def send(index, request):
            body = None if request["bodyHex"] is None else bytes.fromhex(request["bodyHex"])
            barrier.wait()
            try:
                results[index] = edges.raw(server.port, "POST", request["target"],
                                           [("authorization", "Bearer " + edges.token(letter))] +
                                           [tuple(pair) for pair in request["requestHeaders"]], body)
            except (OSError, IndexError, ValueError) as error:
                results[index] = (0, {}, repr(error).encode())

        threads = [threading.Thread(target=send, args=(i, r)) for i, r in enumerate(record["concurrent"])]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        if TRACE:
            print(f"  … {label}: replies joined ({time.strftime('%H:%M:%S')})", flush=True)
        time.sleep(0.1)
        ok = True
        for index, (expected, actual) in enumerate(zip(record["responses"], results)):
            request = record["concurrent"][index]
            ok &= edges.compare(f"{label}[{index}]", {**expected, "status": expected["status"],
                                                      "requestHeaders": request.get("requestHeaders", [])}, *actual)
        ok &= compare_upstream(label, record, self.upstream.calls(), self.secrets)
        COUNTS[(f"{self.kind}/{record['group']}", "matched" if ok else "failed")] += 1

    def run(self, groups_filter):
        records = self.fixture["cases"]
        by_group = collections.OrderedDict()
        for record in records:
            by_group.setdefault(record["group"], []).append(record)
        restart_kv = None
        for group, members in by_group.items():
            if groups_filter and group not in groups_filter and not (group == "restart-b" and "restart-a" in groups_filter):
                continue
            self.set_clock(members[0]["at"])
            self.lanes = edges.Lanes()  # a fresh process: fresh native lanes and breakers
            self.previous_at = None
            kv_name = "restart" if group.startswith("restart-") else None
            server = self.start(group, kv_name)
            try:
                if group == QUOTA_GROUP:
                    # Premium cross-chain quota is 1,200 units/minute at 10
                    # units/request: reaching 20 needs 118 calls in one
                    # minute, more than the shared helper's default 60.
                    edges.drain_quota(server, self.fixture["route"], "D", 20, b"{}", max_requests=140,
                                      expected_cost=10)
                for record in members:
                    if "concurrent" in record:
                        self.concurrent(server, record)
                    else:
                        self.one(server, record)
            finally:
                if group == "restart-a":
                    server.process.send_signal(signal.SIGKILL)
                    server.process.wait()
                    output = server.process.stdout.read() + server.process.stderr.read()
                    restart_kv = True
                else:
                    output = server.stop()
                for secret in self.fixture["secrets"]:
                    check(secret.encode() not in output, f"{self.kind}/{group}: a credential reached process output")
        return restart_kv


# ---- native-only edges ---------------------------------------------------------------------------------------

def saturation(binary, root, fixture):
    """16 simultaneous slow quotes (more than the service's 12 jobs and the
    router's 4 + 8 admission): every answer is the oracle's 200 or the typed
    local back-pressure 429, or the connection is refused at admission
    (status 0: closed without an answer) — never another answer."""
    replay = Replay(binary, fixture, "saturation", root)
    record = next(r for r in fixture["cases"] if r["name"] == "json-valid")
    replay.set_clock(record["at"])
    server = replay.start("main")
    table = {key: [{**entry, "delayMs": 1500} for entry in entries] for key, entries in record["providers"].items()}
    replay.upstream.start_case(table)
    results = []
    lock = threading.Lock()
    barrier = threading.Barrier(16)

    def send(letter):
        barrier.wait()
        try:
            answer = edges.raw(server.port, "POST", fixture["route"], request_headers(record, letter),
                               bytes.fromhex(record["bodyHex"]))
        except (OSError, IndexError, ValueError) as error:
            answer = (0, {}, repr(error).encode())
        with lock:
            results.append(answer)

    try:
        threads = [threading.Thread(target=send, args=("ABC"[i % 3],)) for i in range(16)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
    finally:
        server.stop()
        replay.upstream.close()
    expected = edges.expected_body(record)
    statuses = collections.Counter(status for status, _, _ in results)
    ok = all(status == 200 and payload == expected or status == 0 or
             status == 429 and b'"RATE_LIMITED"' in payload for status, _, payload in results)
    ok = check(ok and statuses[200] >= 1, f"saturation: {statuses} {[p[:80] for s, _, p in results if s not in (200, 429)]}")
    print(f"  saturation: 16 simultaneous slow quotes → {dict(statuses)}; provider peak {replay.upstream.peak}", flush=True)
    return ok


def production(binary, root):
    """The production binary: the routes ride FORTIS_NATIVE_CROSS_CHAIN_CANARY."""
    (Path(root) / "tls-prod").mkdir()
    ca, _, _, _ = tls_ca.create_certificates(Path(root) / "tls-prod", "DNS:localhost")
    ok = True
    for flag in (None, "0", "1"):
        kv = Path(root) / f"kv-prod-{flag}"
        kv.mkdir()
        port = base.free_port()
        env = {k: v for k, v in os.environ.items() if not k.startswith("FORTIS_")}
        env.update({"FORTIS_KV_DIR": str(kv), "PORT": str(port), "FORTIS_CA_BUNDLE_PATH": str(ca)})
        if flag is not None:
            env["FORTIS_NATIVE_CROSS_CHAIN_CANARY"] = flag
        process = subprocess.Popen([binary, str(port)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            for _ in range(400):
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=.1):
                        break
                except OSError:
                    time.sleep(.03)
            for route in ("/v1/exchange/cross-chain/quotes", "/v1/exchange/cross-chain/status"):
                no_auth = edges.raw(port, "POST", route, [("content-type", "application/json")], b"{}")
                origin = edges.raw(port, "POST", route, [("origin", "https://evil.example"),
                                                         ("content-type", "application/json")], b"{}")
                xml = edges.raw(port, "POST", route, [("content-type", "text/xml")], b"<x/>")
                if flag == "1":
                    # fastify-errors: auth-first, so the XML body
                    # without a bearer is 401 too.
                    ok &= check(no_auth[0] == 401 and origin[0] == 403 and xml[0] == 401,
                                f"production on {route}: {no_auth[0]} {origin[0]} {xml[0]}")
                else:
                    ok &= check(no_auth[0] == 404, f"production {flag!r} {route}: {no_auth[0]}, not 404")
        finally:
            process.terminate()
            process.wait(timeout=30)
    print("  production: canary unset/0 → 404; on → 401 / 403 / 415 before any provider work", flush=True)
    return ok


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    production_binary = sys.argv[2] if len(sys.argv) == 3 else None
    only = os.environ.get("R41_R42_ONLY")
    groups_filter = {g for g in os.environ.get("R41_R42_GROUPS", "").split(",") if g}
    native_only = os.environ.get("R41_R42_NATIVE_ONLY", "all")
    with tempfile.TemporaryDirectory(prefix="cross-chain-post-") as root:
        root = Path(root)
        fixtures = {}
        for kind, name in FIXTURES.items():
            if only and kind != only:
                continue
            fixtures[kind] = edges.load_fixture(name)
            edges.pin_self_check(name)
        if native_only in ("all", "none"):
            for kind, fixture in fixtures.items():
                (root / kind).mkdir()
                (root / kind / "tls").mkdir()
                replay = Replay(binary, fixture, kind, root / kind)
                try:
                    replay.run(groups_filter)
                finally:
                    replay.upstream.close()
        if native_only in ("all", "saturation") and "quotes" in fixtures and not groups_filter:
            (root / "sat").mkdir()
            (root / "sat" / "tls").mkdir()
            saturation(binary, root / "sat", fixtures["quotes"])
        if native_only in ("all", "production") and production_binary and not groups_filter:
            (root / "prod").mkdir()
            production(production_binary, root / "prod")
    edges.summary("cross-chain POST replay")


if __name__ == "__main__":
    main()
