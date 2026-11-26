#!/usr/bin/env python3
"""R28–R30: replay the frozen TypeScript news read edges
(tests/corpus/news_reads_edges.json, written by
the earlier TypeScript implementation's tests) against the
native handlers in build/fortis_node_news_route_test:

  R28 GET /v1/news                 handle_feed       (src/news/news_routes.cpp)
  R29 GET /v1/news/{id}            handle_item       (src/news/news_routes.cpp)
  R30 GET /v1/news/{id}/reactions  handle_reactions  (src/news/news_reactions.cpp)

Every fixture step runs in order against one native process on one KV
directory, at the fixture's instant (FORTIS_TEST_NEWS_NOW_FILE):
  seed / spots / corrupt / drop / prune  the same store operation through the
      news test probes (NewsStore Persist/Prune, PriceSeriesStore, raw KV);
  request    one raw request on a socket; status, every recorded header
      (x-rate-limit-remaining in the quota group only), the response bytes
      (SHA-256) and the multiset of CoinGecko URLs it caused;
  parallel   the same requests at once (each compared exactly);
  race       a persist issued together with reads: each read must equal its
      TypeScript post-persist answer or the recorded pre-persist answer;
  state      unexpired stories newest first, lastIngestAt, item presence and
      the named SWR envelopes (asOf, freshUntil, staleUntil, d);
  restart    SIGKILL and a new process on the same KV directory;
  drain      device D's quota minute drained (on the native wall clock).
CoinGecko replies are served per URL in recorded order with identical bytes;
a CoinGecko case first waits for the native lane's wall-clock refill.
`attestation` runs on a second process with ATTESTATION_MODE=required.

Differences are not hidden: each one is a named rule in DIFFERENCES that
asserts the native answer narrowly (see the handler contracts in
src/news/news_routes.cpp and src/news/news_reactions.cpp). The production composition (build/fortis_node)
is checked last: the routes exist only behind FORTIS_NATIVE_NEWS_CANARY=1.

Usage: news_reads_edges_test.py <news route test binary> [<production binary>]
Environment: NEWS_READS_DRAFT_FIXTURE=<path> replays a draft (no pin, exit 2
on success); R28_FAIL_FAST=1 stops at the first failing step;
R28_STOP_AFTER=<group> stops after that group (mutation runs).
"""
import collections
import hashlib
import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import compat_expectations
import news_route_harness as harness
import tls_ca

PINNED = "c69d8ca64f907a6154f2f5ea47c919d8334ba4debc616407f9e2e55c6643638b"
FIXTURE = harness.FIXTURES / "news_reads_edges.json"
HEADERS = ["content-type", "content-length", "vary", "cache-control", "etag", "x-content-type-options",
           "referrer-policy", "permissions-policy", "x-rate-limit-cost", "x-rate-limit-remaining", "retry-after",
           "access-control-allow-origin", "content-security-policy"]
FAILURES = []
COUNTS = collections.Counter()
FAIL_FAST = os.environ.get("R28_FAIL_FAST") == "1"
STOP_AFTER = os.environ.get("R28_STOP_AFTER")


def token(letter):
    return "fintel_dev_" + letter * 43


def fail(message):
    FAILURES.append(message)
    print("FAIL " + message, flush=True)
    if FAIL_FAST:
        raise SystemExit(1)


def raw(port, method, target_bytes, headers, timeout=60):
    lines = [f"{method} ".encode() + target_bytes + b" HTTP/1.1", b"Host: localhost", b"Connection: close"]
    lines += [f"{k}: {v}".encode("latin1") for k, v in headers]
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.sendall(b"\r\n".join(lines) + b"\r\n\r\n")
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


def post(server, path, document):
    body = json.dumps(document).encode()
    head = (f"POST {path} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
            f"Content-Type: application/json\r\nContent-Length: {len(body)}\r\n\r\n").encode()
    with socket.create_connection(("127.0.0.1", server.port), timeout=120) as sock:
        sock.sendall(head + body)
        data = bytearray()
        while True:
            chunk = sock.recv(1 << 20)
            if not chunk:
                break
            data.extend(chunk)
    head_bytes, _, payload = bytes(data).partition(b"\r\n\r\n")
    return int(head_bytes.split(b" ")[1]), json.loads(payload) if payload else None


class Replies:
    """CoinGecko replies per URL, in the order TypeScript received them."""

    def __init__(self, fixture):
        self.queues = collections.defaultdict(collections.deque)
        self.bodies = fixture["replies"]
        self.lock = threading.Lock()
        for step in fixture["steps"]:
            for call in step.get("upstream", []):
                self.queues[call["url"]].append(call)

    def __call__(self, url, _headers):
        with self.lock:
            queue = self.queues.get(url)
            reply = queue.popleft() if queue else None
        if reply is None:
            return 599, {}, b"unexpected request"
        return reply["status"], {"content-type": "application/json"}, self.bodies[reply["body"]].encode()

    def unused(self):
        with self.lock:
            return {url: len(queue) for url, queue in self.queues.items() if queue}


# ---- differences -------------------------------------------------------------------
# name -> (rule, check(actual, record, fixture) -> problem string or None).
# Each rule is also stated beside the handler it concerns (src/news/news_routes.cpp).
NATIVE_BASELINE = {"cache-control": "no-store", "x-content-type-options": "nosniff",
                   "referrer-policy": "no-referrer",
                   "permissions-policy": "camera=(), microphone=(), geolocation=()"}
NATIVE_404 = b'{"error":"NOT_FOUND"}'
NATIVE_400 = b'{"error":"BAD_REQUEST"}'
STEPS = {}


def native_plain_error_headers(headers, payload):
    # The router's Server-Timing: well-formed, then excluded from the exact set.
    headers = dict(headers)
    timing = headers.pop("server-timing", "")
    if not re.fullmatch(r"app;dur=\d+\.\d{3}", timing):
        return f"native plain-error Server-Timing {timing!r}"
    expected = {"content-type": "application/json", "content-length": str(len(payload)),
                "connection": "close", **NATIVE_BASELINE}
    if headers != expected:
        return f"native plain-error headers {headers!r} != {expected!r}"
    return None


def same_headers_except(headers, record, names):
    for name in HEADERS:
        if name in names or name == "x-rate-limit-remaining":
            continue
        if headers.get(name) != record["headers"].get(name):
            return f"header {name}: {headers.get(name)!r} != {record['headers'].get(name)!r}"
    return None


def router_404(actual, record, _fixture):
    """find-my-way's not-found answer is Fastify's default 404 document (no
    /v1 hooks: no security headers, no cost); the native router answers its
    own {"error":"NOT_FOUND"} with the server baseline headers. Both 404,
    before any hook, no quota, no provider."""
    status, headers, payload = actual
    if record["status"] != 404 or status != 404 or payload != NATIVE_404:
        return f"{status} {payload!r}"
    return native_plain_error_headers(headers, payload)


def non_ascii_id(actual, record, _fixture):
    """An id spelt with U+017F, U+FB00 or U+00DF: TypeScript's toUpperCase
    mapped them into the ULID alphabet (S, FF, SS) and served the story;
    native upper-cases ASCII only, so the id is malformed (after the device
    policy, charged)."""
    status, headers, payload = actual
    if record["status"] != 200 or status != 400 or \
            payload != b'{"error":"BAD_REQUEST","detail":"malformed news id"}' or "x-rate-limit-cost" not in headers:
        return f"{status} {payload[:120]!r}"
    return None


def opaque_404(actual, record, _fixture):
    """An empty, control-character or over-long {id}: the native router's
    404 before any hook (opaque-params; TypeScript ran the hooks)."""
    status, headers, payload = actual
    if status != 404 or payload != NATIVE_404:
        return f"{status} {payload!r}"
    return native_plain_error_headers(headers, payload)


def node_headers(actual, record, fixture):
    """A refusal Node/Fastify wrote outside the /v1 plugin (414
    FST_ERR_MAX_PARAM_LENGTH, the clientError 400 for a raw non-ASCII
    byte): the same status before any hook; since fastify-errors
    the body is this server's {"error":"URI_TOO_LONG"} / {"error":
    "BAD_REQUEST"} as application/json; charset=utf-8 with the server
    baseline headers."""
    status, headers, payload = actual
    want = b'{"error":"URI_TOO_LONG"}' if record["status"] == 414 else b'{"error":"BAD_REQUEST"}'
    if status != record["status"] or payload != want:
        return f"{status} {payload[:200]!r}"
    headers = dict(headers)
    timing = headers.pop("server-timing", "")
    if not re.fullmatch(r"app;dur=\d+\.\d{3}", timing):
        return f"Server-Timing {timing!r}"
    expected = {"content-type": "application/json; charset=utf-8", "content-length": str(len(payload)),
                "connection": "close", **NATIVE_BASELINE}
    if headers != expected:
        return f"headers {headers!r} != {expected!r}"
    return None


def parser_400(actual, record, _fixture):
    """Requests the native HTTP parser refuses before routing (a raw byte
    that is not UTF-8, a malformed or non-UTF-8 escape, a fragment, a
    repeated or over-256-byte If-None-Match): 400 {"error":"BAD_REQUEST"},
    no hook, no quota. TypeScript answers each differently (recorded)."""
    status, headers, payload = actual
    if status != 400 or payload != NATIVE_400:
        return f"{status} {payload[:200]!r}"
    return native_plain_error_headers(headers, payload)


def head_304_without_length(actual, record, _fixture):
    """A HEAD 304: Fastify sends content-length: 0, the native writer none
    (R07's accepted rule). Everything else is exact."""
    status, headers, payload = actual
    if status != 304 or record["status"] != 304 or payload or "content-length" in headers:
        return f"{status} {headers.get('content-length')!r}"
    return same_headers_except(headers, record, {"content-length"})


def accept_space_before_parameter(actual, record, _fixture):
    """Accept "application/protobuf ; q=0.5": acceptsProtobuf splits at ';'
    without trimming the media type, so TypeScript answers JSON; the shared
    native AcceptsProtobuf trims it and answers the recorded protobuf
    representation (feed-protobuf) instead."""
    status, headers, payload = actual
    protobuf = STEPS["feed-protobuf"]
    if status != 200 or hashlib.sha256(payload).hexdigest() != protobuf["bodySha256"]:
        return f"{status} {payload[:120]!r}"
    return same_headers_except(headers, protobuf, set())


def durable_envelope(actual, record, _fixture):
    """Hard restart: native SWR envelopes live in the durable KV, so the
    first reads answer the envelope the pre-restart revalidation stored
    (state-before-restart news:|40, fresh), without the story persisted
    just before the kill; production TypeScript's in-memory Kv starts empty
    and recomputes (the story is there)."""
    status, headers, payload = actual
    stored = STEPS["state-before-restart"]["swr"]["news:|40"]["d"]
    try:
        native = json.loads(payload)
    except ValueError:
        return f"{status} {payload[:120]!r}"
    if status != 200 or native != stored or native == json.loads(BODIES[record["bodySha256"]][5:]):
        return f"{status} body is not the stored envelope"
    digest = hashlib.sha256(payload).hexdigest()[:24]
    if headers.get("etag") != f'W/"news-{digest}"':
        return f"etag {headers.get('etag')!r}"
    if headers.get("content-length") != str(len(payload)):
        return f"content-length {headers.get('content-length')!r} != {len(payload)}"
    return same_headers_except(headers, record, {"etag", "content-length"})


def sample_ceiling(actual, record, _fixture):
    """More than 4,096 in-window samples for one asset (10 s data over a
    20.5 h window): native treats the asset like an unreadable day (no
    points, windows unavailable) to bound reaction memory; TypeScript keeps
    every sample. The benchmark and every other field are exact."""
    status, headers, payload = actual
    try:
        native = json.loads(payload)
    except ValueError:
        return f"{status} {payload[:120]!r}"
    typescript = json.loads(BODIES[record["bodySha256"]][5:])
    asset = typescript["assets"][0]
    if status != 200 or len(asset["points"]) <= 4096 or native["assets"][0]["points"]:
        return f"{status} native points {len(native['assets'][0]['points'])}"
    if any(window["status"] == "measured" for window in native["assets"][0]["windows"]):
        return "native measured a window"
    native["assets"][0] = asset
    if native != typescript:
        return "differs beyond the dense asset"
    digest = hashlib.sha256(payload).hexdigest()[:24]
    if headers.get("etag") != f'W/"news-reactions-{digest}"':
        return f"etag {headers.get('etag')!r}"
    if headers.get("content-length") != str(len(payload)):
        return f"content-length {headers.get('content-length')!r} != {len(payload)}"
    return same_headers_except(headers, record, {"etag", "content-length"})


DIFFERENCES = {}
for _name in ("item-trailing-slash", "reactions-trailing-slash", "reactions-upper", "reactions-extra", "feed-upper",
              "feed-double-slash", "feed-post", "item-post", "reactions-delete", "feed-options"):
    DIFFERENCES[_name] = ("router 404", router_404)
for _name in ("item-raw-utf8", "item-101-units", "item-101-units-no-auth", "reactions-101-units", "item-101-escaped",
              "item-51-emoji", "quota-over-101", "attestation-item-101"):
    DIFFERENCES[_name] = ("Node refusal headers", node_headers)
for _name in ("item-raw-latin1", "item-escape-invalid-utf8", "item-escape-malformed", "item-fragment", "inm-two-lines",
              "inm-long"):
    DIFFERENCES[_name] = ("native parser 400", parser_400)
for _name in ("inm-head", "inm-head-protobuf", "item-inm-head"):
    DIFFERENCES[_name] = ("HEAD 304 without content-length", head_304_without_length)
for _name in ("item-long-s", "item-ligature-ff", "item-sharp-s"):
    DIFFERENCES[_name] = ("ASCII-only id upper-casing (task 17 js-case)", non_ascii_id)
DIFFERENCES["accept-q-space"] = ("Accept media type trimmed", accept_space_before_parameter)
DIFFERENCES["after-restart-feed"] = ("durable SWR envelope", durable_envelope)
DIFFERENCES["after-restart-inm"] = ("durable SWR envelope", durable_envelope)
DIFFERENCES["reactions-dense"] = ("reaction sample ceiling", sample_ceiling)


def race_envelope(actual, expected, _fixture):
    """The variant a race read stored: TypeScript's read always follows its
    synchronous persist; natively the read may precede it. The envelope's
    timing fields are exact; its value is the pre or the post answer."""
    race = STEPS["race-persist-feed"]
    pre = json.loads(BODIES[race["pre"]["71"]][5:])
    if actual is None:
        return "no envelope"
    if set(actual) != set(expected):
        return f"envelope fields {set(actual)!r} != {set(expected)!r}"
    for field in ("asOf", "freshUntil", "staleUntil"):
        if actual.get(field) != expected.get(field):
            return f"{field} {actual.get(field)!r} != {expected.get(field)!r}"
    if actual["d"] not in (expected["d"], pre):
        return "value is neither the pre nor the post answer"
    return None


DIFFERENCES["state-concurrent:news:|71"] = ("race outcome", race_envelope)
BODIES = {}


def headers_of(step):
    out = []
    if step["device"]:
        out.append(("authorization", "Bearer " + token(step["device"])))
    out += [tuple(pair) for pair in step["requestHeaders"]]
    return out


def compare(label, step, actual, fixture, remaining):
    status, headers, payload = actual
    rule = DIFFERENCES.get(step["name"])
    if not rule and compat_expectations.malformed_path_target(step):
        rule = ("router 404 for a malformed path value (task 17 opaque-params)", opaque_404)
    if rule:
        problem = rule[1](actual, step, fixture)
        if problem:
            fail(f"{label}: difference '{rule[0]}' not as asserted: {problem}")
            return False
        COUNTS["asserted"] += 1
        return True
    problems = []
    # compat-decisions.md#zod-issues: a recorded zod refusal is
    # {"error":"BAD_REQUEST"} here.
    recorded = fixture["bodies"].get(step["bodySha256"], "")
    if recorded.startswith("text:") and compat_expectations.is_zod_refusal(recorded[5:]):
        step = dict(step, headers=compat_expectations.rewrite_headers(
                        step["headers"], len(compat_expectations.BAD_REQUEST)),
                    bodySha256=hashlib.sha256(compat_expectations.BAD_REQUEST).hexdigest())
        fixture = dict(fixture, bodies=dict(fixture["bodies"], **{
            step["bodySha256"]: "text:" + compat_expectations.BAD_REQUEST.decode()}))
    if status != step["status"]:
        problems.append(f"status {status} != {step['status']}")
    for name in HEADERS:
        if name == "x-rate-limit-remaining" and not remaining:
            continue
        if headers.get(name) != step["headers"].get(name):
            problems.append(f"header {name}: {headers.get(name)!r} != {step['headers'].get(name)!r}")
    if hashlib.sha256(payload).hexdigest() != step["bodySha256"]:
        expected = fixture["bodies"][step["bodySha256"]]
        problems.append(f"body differs\n    expected {expected[:600]!r}\n    actual   "
                        f"{payload[:600].decode('utf-8', 'replace')!r}")
    if problems:
        dump = os.environ.get("R28_DUMP")
        if dump:
            Path(dump).mkdir(parents=True, exist_ok=True)
            (Path(dump) / (label.replace("/", "__") + ".native")).write_bytes(payload)
        fail(f"{label}: " + "; ".join(problems))
        return False
    COUNTS["matched"] += 1
    return True


# ---- the run -------------------------------------------------------------------------

def lane_wait(calls):
    """The native CoinGecko lane refills one token per 2.5 s of wall time
    (TypeScript's frozen clock refilled by fixture time)."""
    if calls:
        time.sleep(2.6 * min(len(calls), 4))


# Rules whose native provider requests differ: name -> recorded calls -> the
# native calls (each also skipped in the never-requested check).
UPSTREAM = {
    # The dense asset fails on its first day natively, so its second day is
    # never requested (rule 'reaction sample ceiling').
    "reactions-dense": lambda calls: [c for i, c in enumerate(calls) if not (
        "/coins/coin-dense/" in c["url"] and any("/coins/coin-dense/" in p["url"] for p in calls[:i]))],
}
SKIPPED = collections.Counter()


def run_request(server, upstream, step, fixture):
    calls = step["upstream"]
    if step["name"] in UPSTREAM:
        native = UPSTREAM[step["name"]](calls)
        for call in calls:
            if call not in native:
                SKIPPED[call["url"]] += 1
        calls = native
    expected = sorted(call["url"] for call in calls)
    lane_wait(expected)
    before = len(upstream.calls())
    actual = raw(server.port, step["method"], bytes.fromhex(step["targetHex"]), headers_of(step))
    harness.wait_for_calls(upstream, before + len(expected), timeout=20.0)
    time.sleep(.2 if not expected else .4)
    observed = sorted(upstream.calls()[before:])
    compare(f"{step['group']}/{step['name']}", step, actual, fixture, step["group"] == "quota")
    if observed != expected:
        fail(f"{step['group']}/{step['name']}: upstream {observed} != {expected}")


def run_parallel(server, upstream, step, fixture):
    expected = sorted(call["url"] for call in step.get("upstream", []))
    lane_wait(expected)
    before = len(upstream.calls())
    results = [None] * len(step["requests"])

    def one(index, request):
        results[index] = raw(server.port, request["method"], bytes.fromhex(request["targetHex"]), headers_of(request))

    threads = [threading.Thread(target=one, args=(i, r)) for i, r in enumerate(step["requests"])]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    harness.wait_for_calls(upstream, before + len(expected), timeout=20.0)
    time.sleep(.4)
    for request, actual in zip(step["requests"], results):
        compare(f"{step['group']}/{step['name']}/{request['name']}", request, actual, fixture, False)
    observed = sorted(upstream.calls()[before:])
    if observed != expected:
        fail(f"{step['group']}/{step['name']}: upstream {observed} != {expected}")


def parser_stress(server, step, fixture, rounds=6):
    """Native only: the recorded parallel-feed-variants requests again, all
    at once, `rounds` times. Every answer must equal its TypeScript record
    (invalid queries re-parse each time; valid ones now read their cached
    variant), so per-request parse scratch shared between workers would show
    as a wrong detail or a wrong variant."""
    requests = step["requests"] * rounds
    results = [None] * len(requests)

    def one(index, request):
        results[index] = raw(server.port, request["method"], bytes.fromhex(request["targetHex"]), headers_of(request))

    threads = [threading.Thread(target=one, args=(i, r)) for i, r in enumerate(requests)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    bad = 0
    for request, (status, _, payload) in zip(requests, results):
        want = request["bodySha256"]
        # compat-decisions.md#zod-issues: a recorded zod refusal is
        # {"error":"BAD_REQUEST"} here.
        recorded = fixture["bodies"].get(want, "")
        if recorded.startswith("text:") and compat_expectations.is_zod_refusal(recorded[5:]):
            want = hashlib.sha256(compat_expectations.BAD_REQUEST).hexdigest()
        if status != request["status"] or hashlib.sha256(payload).hexdigest() != want:
            bad += 1
            fail(f"stress/{request['name']}: {status} {payload[:160]!r}")
    COUNTS["stress"] += len(requests) - bad


def run_race(server, step, fixture):
    """Reads issued together with a persist: native may serialize either way."""
    results = [None] * len(step["requests"])
    persisted = []

    def read(index, request):
        results[index] = raw(server.port, request["method"], bytes.fromhex(request["targetHex"]), headers_of(request))

    threads = [threading.Thread(target=lambda: persisted.append(post(server, "/probe/test-news-persist",
                                                                     {"stories": step["stories"]})))]
    threads += [threading.Thread(target=read, args=(i, r)) for i, r in enumerate(step["requests"])]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    if not persisted or persisted[0][0] != 200:
        fail(f"race/{step['name']}: persist {persisted}")
    for request, (status, _, payload) in zip(step["requests"], results):
        limit = request["name"].split("-")[1]
        digest = hashlib.sha256(payload).hexdigest()
        if status != 200 or digest not in (request["bodySha256"], step["pre"][limit]):
            fail(f"race/{request['name']}: {status} {payload[:200]!r} is neither the pre nor the post answer")
        else:
            COUNTS["race-" + ("post" if digest == request["bodySha256"] else "pre")] += 1


def run_state(server, step):
    status, actual = post(server, "/probe/test-news-state", {"ids": list(step["items"]), "keys": list(step["swr"])})
    if status != 200:
        fail(f"state/{step['name']}: probe {status}")
        return
    for field in ("recent", "lastIngestAt", "items"):
        if actual[field] != step[field]:
            fail(f"state/{step['name']}: {field} {actual[field]} != {step[field]}")
    for key, envelope in step["swr"].items():
        rule = DIFFERENCES.get(f"{step['name']}:{key}")
        if rule:
            problem = rule[1](actual["swr"][key], envelope, None)
            if problem:
                fail(f"state/{step['name']}/{key}: difference '{rule[0]}' not as asserted: {problem}")
            else:
                COUNTS["asserted-state"] += 1
            continue
        if actual["swr"][key] != envelope:
            fail(f"state/{step['name']}: swr {key}\n    expected {json.dumps(envelope)[:400]}\n"
                 f"    actual   {json.dumps(actual['swr'][key])[:400]}")
    COUNTS["state"] += 1


def drain(server, step):
    """Device D's quota minute: the native window is the wall-clock minute."""
    second = time.time() % 60
    if second > 30:
        time.sleep(60 - second + .3)
    for i in range(step["count"]):
        status, _, _ = raw(server.port, "GET", step["target"].encode(), [("authorization", "Bearer " + token(step["device"]))])
        if status != 200:
            fail(f"drain {i}: {status}")
            return


def start(binary, kv_dir, clock, upstream, env=None):
    return harness.Server(binary, kv_dir, clock, upstream, env)


def attestation(binary, root, fixture, upstream):
    kv_dir = root / "kv-attest"
    kv_dir.mkdir()
    clock = root / "clock-attest"
    harness.set_clock(clock, fixture["steps"][0]["at"])
    server = start(binary, kv_dir, clock, upstream, {"ATTESTATION_MODE": "required"})
    try:
        for case in fixture["attestation"]:
            status, headers, payload = raw(server.port, "GET", case["target"].encode(), [("authorization", "Bearer " + token("A"))])
            step = {"name": "attestation-" + case["name"], "status": case["status"], "headers": case["headers"],
                    "bodySha256": hashlib.sha256(case["body"].encode()).hexdigest()}
            fixture["bodies"].setdefault(step["bodySha256"], "text:" + case["body"])
            compare(f"attestation/{case['name']}", step, (status, headers, payload), fixture, False)
    finally:
        server.stop()


def production(binary, root):
    """The production composition: the news reads exist only behind the canary
    and, with it on, refuse a missing bearer and any Origin before the read."""
    (root / "prod-tls").mkdir()
    ca, _, _, _ = tls_ca.create_certificates(root / "prod-tls", "DNS:localhost")
    targets = ["/v1/news", "/v1/news/01KFFSSZ3NDEKTSV4RRQ69G5FA", "/v1/news/01KFFSSZ3NDEKTSV4RRQ69G5FA/reactions"]

    def server_with(env, name):
        port = harness.free_port()
        environment = {k: v for k, v in os.environ.items()
                       if not k.startswith("FORTIS_") and k not in ("ATTESTATION_MODE", "NODE_ENV", "COINGECKO_API_KEY")}
        kv_dir = root / name
        kv_dir.mkdir()
        environment.update({"FORTIS_KV_DIR": str(kv_dir), "PORT": str(port), "FORTIS_CA_BUNDLE_PATH": str(ca)})
        environment.update(env)
        process = subprocess.Popen([binary, str(port)], env=environment, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
        for _ in range(400):
            if process.poll() is not None:
                raise RuntimeError(f"production server exited ({env})")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.1):
                    return process, port
            except OSError:
                time.sleep(.03)
        raise RuntimeError("production server did not start")

    for value in (None, "0"):
        process, port = server_with({} if value is None else {"FORTIS_NATIVE_NEWS_CANARY": value}, f"prod-{value}")
        try:
            for target in targets:
                status, _, _ = raw(port, "GET", target.encode(), [("authorization", "Bearer " + token("A"))])
                if status != 404:
                    fail(f"production canary {value!r} {target}: {status}, not 404")
        finally:
            process.terminate()
            process.wait()
    process, port = server_with({"FORTIS_NATIVE_NEWS_CANARY": "1"}, "prod-on")
    try:
        for target in targets + ["/v1/news?limit=0", "/v1/news/nope", "/v1/news/nope/reactions"]:
            status, _, body = raw(port, "GET", target.encode(), [])
            if (status, body) != (401, b'{"error":"UNAUTHORIZED"}'):
                fail(f"production no-auth {target}: {status} {body!r}")
            status, _, body = raw(port, "GET", target.encode(), [("origin", "https://evil.example")])
            if (status, body) != (403, b'{"error":"BROWSER_ORIGIN_FORBIDDEN"}'):
                fail(f"production origin {target}: {status} {body!r}")
    finally:
        process.terminate()
        process.wait()
    print("production: canary unset/0 answers 404; on, no bearer 401 and Origin 403 before validation", flush=True)


def main():
    binary = sys.argv[1]
    draft = os.environ.get("NEWS_READS_DRAFT_FIXTURE")
    path = Path(draft) if draft else FIXTURE
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if not draft:
        pinned = (FIXTURE.parent / (FIXTURE.name + ".sha256")).read_text().strip()
        if digest != PINNED or pinned != PINNED:
            raise SystemExit(f"fixture sha256 {digest} (pin file {pinned}) is not the pinned {PINNED}")
    fixture = json.loads(data)
    BODIES.update(fixture["bodies"])
    for step in fixture["steps"]:
        if "name" in step:
            STEPS.setdefault(step["name"], step)
    replies = Replies(fixture)
    by_handler = collections.Counter()
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        clock = root / "clock"
        kv_dir = root / "kv"
        kv_dir.mkdir()
        upstream = harness.Upstream(root, replies)
        harness.set_clock(clock, fixture["steps"][0]["at"])
        server = start(binary, kv_dir, clock, upstream)
        group = None
        try:
            for step in fixture["steps"]:
                if STOP_AFTER and group == STOP_AFTER and step["group"] != STOP_AFTER:
                    break
                group = step["group"]
                if "at" in step:
                    harness.set_clock(clock, step["at"])
                op = step["op"]
                if op == "request":
                    run_request(server, upstream, step, fixture)
                    by_handler[step["handler"]] += 1
                elif op == "parallel":
                    run_parallel(server, upstream, step, fixture)
                    if step["name"] == "parallel-feed-variants":
                        parser_stress(server, step, fixture)
                    for request in step["requests"]:
                        by_handler[request["handler"]] += 1
                elif op == "race":
                    run_race(server, step, fixture)
                    by_handler["R28"] += len(step["requests"])
                elif op == "seed":
                    for offset in range(0, len(step["stories"]), 10):
                        status, _ = post(server, "/probe/test-news-persist", {"stories": step["stories"][offset:offset + 10]})
                        if status != 200:
                            fail(f"seed/{step['name']}: {status}")
                elif op == "spots":
                    status, _ = post(server, "/probe/test-news-spots", {"spots": step["spots"]})
                    if status != 200:
                        fail(f"spots/{step['name']}: {status}")
                elif op in ("corrupt", "drop"):
                    document = {"key": f"news/item/{step['id']}", "value": None}
                    if op == "corrupt":
                        document.update(value='"garbage"', expiresAt=step["expiresAt"])
                    status, _ = post(server, "/probe/test-news-raw", document)
                    if status != 200:
                        fail(f"{op}/{step['name']}: {status}")
                elif op == "prune":
                    status, _, payload = raw(server.port, "GET", f"/probe/test-news-prune-stories?before={step['before']}".encode(), [])
                    removed = json.loads(payload).get("removed")
                    # Rule 'expired stories swept': SID 13 was persisted
                    # already expired, and EXP expires six minutes before
                    # this prune. The native KV sweeper can remove either or
                    # both before this request; buildApp does not run the
                    # embedded-store sweep job main() schedules. The next
                    # state and item/feed assertions check the exact result.
                    allowed = {step["removed"] - 1, step["removed"] - 2}
                    if status != 200 or removed not in allowed:
                        fail(f"prune/{step['name']}: {status} removed {removed}, expected {allowed} (swept)")
                    else:
                        COUNTS["asserted"] += 1
                elif op == "state":
                    run_state(server, step)
                elif op == "restart":
                    server.kill()
                    server = start(binary, kv_dir, clock, upstream)
                elif op == "drain":
                    drain(server, step)
                else:
                    fail(f"unknown op {op}")
        finally:
            server.stop()
            if FAILURES:
                sys.stderr.write(server.log()[-3000:])
        if not STOP_AFTER:
            attestation(binary, root, fixture, upstream)
            if len(sys.argv) > 2:
                production(sys.argv[2], root)
        upstream.close()
    unused = {url: n - SKIPPED[url] for url, n in replies.unused().items() if n - SKIPPED[url]}
    if unused and not STOP_AFTER:
        fail(f"recorded CoinGecko replies never requested: {unused}")
    print(f"news reads: {dict(by_handler)} requests by handler; {COUNTS['matched']} matched, "
          f"{COUNTS['asserted']} asserted, {COUNTS['state']} states ({COUNTS['asserted-state']} asserted envelopes), "
          f"race {COUNTS['race-pre']} pre / {COUNTS['race-post']} post; parser stress {COUNTS['stress']} exact; "
          f"{len(FAILURES)} failed", flush=True)
    if FAILURES:
        raise SystemExit(1)
    if draft:
        print("news reads: draft replay passed (not a verdict)")
        raise SystemExit(2)
    print("news reads: all checks passed")


if __name__ == "__main__":
    main()
