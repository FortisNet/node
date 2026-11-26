#!/usr/bin/env python3
"""Real-socket replay of the three Robinhood transaction POST routes (R43–R45)
against the pinned TypeScript socket-oracle fixtures that
the earlier TypeScript implementation's tests recorded from
the real Fastify app:

  R43 tests/corpus/robinhood_prepare_edges.json   POST …/sponsored-trades/prepare
  R44 tests/corpus/robinhood_submit_edges.json    POST …/sponsored-trades/submit
  R45 tests/corpus/robinhood_diagnose_edges.json  POST …/transactions/diagnose

Each fixture's SHA-256 is checked against its .sha256 sidecar (and a modified
copy must be refused). The test binary (build/fortis_node_robinhood_route_test)
runs with FORTIS_NATIVE_ROBINHOOD_CANARY=1; api.robinhood.com and both Alchemy
hosts go to one loopback HTTPS upstream (robinhood_route_test.Upstream) that
answers each case from TypeScript's recorded table (a JSON-RPC POST keyed by
its method) with the recorded bytes and delays, and records every request.
Per case the replay writes the case's frozen instant to the Robinhood clock
file (ticket expiry, expiresAt, simulatedAt and the catalog cache follow it)
and TypeScript's ticket counter to the ticket file (randomBytes(24) =
sha256("robinhood-ticket-n")[0:24]), then sends the recorded request bytes.

Checks per record: the status; every recorded header (content-type,
content-length, vary, cache-control, etag, x-content-type-options,
referrer-policy, permissions-policy, x-rate-limit-cost, retry-after,
access-control-allow-origin; x-rate-limit-remaining in the quota group), with
absent headers required absent; the exact JSON text or protobuf bytes; the
provider requests as a multiset of (method, URL, exact body) with their
content-type and accept; no client header, device token or id at a provider;
the gas policy id only in wallet_prepareCalls bodies; no key, policy id or
device credential in an answer or the process output; TypeScript's 10 s
timeout class. Concurrent records send every request at once and compare the
answers as a multiset, the ticket text replaced (simultaneous prepares draw
tickets n, n+1, … in scheduling order), plus the drawn tickets as a set.

The native quota, lanes and breakers run on real time where TypeScript's ran
on its frozen Date, so the replay paces devices inside their per-minute quota
and the Alchemy lanes inside their native budgets, and sleeps the frozen gap
(at most 32 s) between failure-group cases so a native breaker sees
TypeScript's elapsed time. Each fixture group is a fresh process with its own
KV directory; restart-b reuses restart-a's after a SIGKILL.

Asserted differences (named beside the native handlers):
  router_404                    other methods and spellings: 404 in both,
                                native body {"error":"NOT_FOUND"} with the
                                baseline headers, no provider request;
  quota_retry_after_wall_clock  TypeScript's frozen minute says retry-after 60,
                                native the wall clock's (1–60); all else exact;
  bigint_640_bit                (R45 valid-fee-640-bit) a fee product past the
                                native 640-bit integer: native 500
                                {"error":"INTERNAL"} (no Vary), TypeScript a
                                200 report; status, body and headers asserted,
                                provider requests exact.

Native-only checks: 12 simultaneous prepares against a slow wallet (every
answer exact-shaped 200 or the typed back-pressure 429); a SIGKILL while a
submit is at Alchemy (one send; after the restart the ticket is unknown);
the production composition (build/fortis_node): canary unset or 0 → 404
on all three routes; on → no bearer 401 (also with a text/xml body, auth-first), Origin 403 before any
provider work.

Environment: R43_R45_FIXTURES=prepare,submit,diagnose (default all),
R43_R45_GROUPS=main,failure,… (default all), R43_R45_FAIL_FAST=1,
R43_R45_SKIP_NATIVE_ONLY=1.
Usage: robinhood_writes_edges_test.py build/fortis_node_robinhood_route_test [build/fortis_node]
"""
import collections
import hashlib
import json
import os
import re
import shutil
import signal
import sys
import tempfile
import threading
import time
from pathlib import Path
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parent))
import robinhood_stock_catalog_edges_test as catalog  # noqa: E402  (sets the upstream's leak headers)
import discovery_post_edges as edges  # noqa: E402
import tls_ca  # noqa: E402

FIXTURES = Path(__file__).resolve().parents[1] / "tests/corpus"
FILES = {"prepare": ("R43", "robinhood_prepare_edges.json"),
         "submit": ("R44", "robinhood_submit_edges.json"),
         "diagnose": ("R45", "robinhood_diagnose_edges.json")}
ROUTES = {"prepare": "/v1/networks/robinhood/sponsored-trades/prepare",
          "submit": "/v1/networks/robinhood/sponsored-trades/submit",
          "diagnose": "/v1/networks/robinhood/transactions/diagnose"}
HEADERS = ["content-type", "content-length", "vary", "cache-control", "etag", "x-content-type-options",
           "referrer-policy", "permissions-policy", "x-rate-limit-cost", "x-rate-limit-remaining", "retry-after",
           "access-control-allow-origin"]
NATIVE_BASELINE = edges.NATIVE_BASELINE
NATIVE_NOT_FOUND = b'{"error":"NOT_FOUND"}'
ROUTER = {"router-get", "router-head", "router-put", "router-trailing-slash", "router-upper"}
QUOTA_RETRY = {"quota-over", "quota-over-protobuf", "quota-over-invalid"}
SECRETS = ["fixture-alchemy-key", "fixture-gas-policy"]
POLICY_ID = "fixture-gas-policy"
# Native lanes (provider_lane.cpp): rate per interval and burst.
LANES = {"api.g.alchemy.com": (20, 1000, 5), "robinhood-mainnet.g.alchemy.com": (10, 1000, 3),
         "api.robinhood.com": (60, 1000, 15)}
FAIL_FAST = os.environ.get("R43_R45_FAIL_FAST") == "1"
# A ticket in a JSON answer ("ticket":"…") or a Struct answer (the key
# "ticket", then a string Value of 32 bytes).
TICKET = re.compile(rb'("ticket":"|\x06ticket\x12"\x1a )([A-Za-z0-9_-]{32})')
FAILURES = []
COUNTS = collections.Counter()


def check(condition, message):
    if not condition:
        FAILURES.append(message)
        print("FAIL:", message, flush=True)
        if FAIL_FAST:
            raise SystemExit(1)
    return condition


def selected(variable, default):
    value = os.environ.get(variable)
    return set(value.split(",")) if value else set(default)


# ---- pacing ----------------------------------------------------------------------------------------------

class Lanes:
    """Token buckets with the native lane budgets, in real time."""

    def __init__(self):
        self.state = {}

    def reserve(self, calls):
        need = collections.Counter(urlsplit(call["url"]).hostname for call in calls)
        while True:
            now = time.monotonic()
            wait = 0.0
            for host, count in need.items():
                if host not in LANES:
                    continue
                rate, interval, burst = LANES[host]
                per_s = rate / interval * 1000
                tokens, at = self.state.get(host, (float(burst), now))
                tokens = min(float(burst), tokens + (now - at) * per_s)
                self.state[host] = (tokens, now)
                want = min(float(count), float(burst))
                if tokens < want:
                    wait = max(wait, (want - tokens) / per_s + 0.02)
            if wait <= 0:
                break
            time.sleep(wait)
        for host, count in need.items():
            if host in LANES:
                tokens, at = self.state[host]
                self.state[host] = (max(0.0, tokens - count), at)


class Quota:
    """Native device quotas run on the wall clock: premium A–T 1,200 a minute,
    development a–d 240; every request here costs 20."""

    def __init__(self):
        self.used = collections.Counter()

    def reserve(self, device, cost=20):
        if not device or device not in "ABCDEFGHIJKLMNOPQRSTabcd":
            return
        limit = (1200 if device.isupper() else 240) - 40
        while True:
            minute = int(time.time() // 60)
            if self.used[(device, minute)] + cost <= limit:
                self.used[(device, minute)] += cost
                return
            time.sleep(61 - time.time() % 60)


# ---- the harness -------------------------------------------------------------------------------------------

class Harness:
    def __init__(self, binary, root):
        self.binary, self.root = binary, root
        (root / "tls").mkdir()
        self.upstream = catalog.Upstream(root / "tls")
        self.clock = root / "clock"
        self.tickets = root / "tickets"
        self.tickets.write_text("0")
        self.lanes = Lanes()
        self.quota = Quota()
        self.secrets = []

    def server(self, kv_name, env, at):
        self.clock.write_text(str(at))
        return catalog.Server(self.binary, self.root / f"kv-{kv_name}", {
            "FORTIS_NATIVE_ROBINHOOD_CANARY": "1",
            "FORTIS_CA_BUNDLE_PATH": str(self.upstream.ca),
            "FORTIS_ENABLE_TEST_ROBINHOOD_ROUTES": "1",
            "FORTIS_TEST_ROBINHOOD_UPSTREAM_PORT": str(self.upstream.port),
            "FORTIS_TEST_ROBINHOOD_UPSTREAM_CA": str(self.upstream.ca),
            "FORTIS_TEST_ROBINHOOD_NOW_FILE": str(self.clock),
            "FORTIS_TEST_ROBINHOOD_TICKET_FILE": str(self.tickets),
            **env,
        })

    def no_secret(self, data, where, allow_policy=False):
        for secret in self.secrets + SECRETS:
            if allow_policy and secret == POLICY_ID:
                continue
            check(secret.encode() not in data, f"{where}: {secret[:18]}… reached a response, provider or the output")


def request_headers(device, recorded):
    headers = [("authorization", "Bearer " + edges.token(device))] if device else []
    return headers + [tuple(pair) for pair in recorded]


def bearer_fix(case, headers):
    # The oracle's lower-case "bearer" case carries its own authorization header.
    return headers


def compare_response(label, record, actual, group):
    status, headers, payload = actual
    if edges.strict_compare(label, record, status, headers, payload):
        return True
    problems = []
    if status != record["status"]:
        problems.append(f"status {status} != {record['status']}")
    for name in HEADERS:
        if name == "x-rate-limit-remaining" and group != "quota":
            continue
        if headers.get(name) != record["headers"].get(name):
            problems.append(f"header {name}: {headers.get(name)!r} != {record['headers'].get(name)!r}")
    want = edges.expected_body(record)
    if payload != want:
        problems.append(f"body {edges.first_difference(want, payload)}")
    return check(not problems, f"{label}: " + "; ".join(problems))


def compare_upstream(harness, label, expected, observed):
    want = collections.Counter((c["method"], c["url"], c.get("body")) for c in expected)
    got = collections.Counter((c["method"], c["url"], c.get("body")) for c in observed)
    ok = True
    if want != got:
        missing = list((want - got).elements())
        extra = list((got - want).elements())
        ok = check(False, f"{label}: provider requests differ; TypeScript only {[(m, u, (b or '')[:300]) for m, u, b in missing]}; "
                          f"native only {[(m, u, (b or '')[:300]) for m, u, b in extra]}")
    for call in observed:
        headers = call["headers"]
        expected_type = "application/json" if call["method"] == "POST" else None
        ok = check((headers.get("accept") or "").lower() == "application/json",
                   f"{label}: provider accept {headers.get('accept')!r}") and ok
        if expected_type:
            ok = check((headers.get("content-type") or "").lower().startswith(expected_type),
                       f"{label}: provider content-type {headers.get('content-type')!r}") and ok
        for name in ("authorization", "cookie", "x-forwarded-for", "forwarded", "x-real-ip", "origin"):
            ok = check(name not in headers, f"{label}: provider request carries {name}") and ok
        text = json.dumps(call).encode()
        harness.no_secret(text.replace(b"/v2/fixture-alchemy-key", b"/v2/[key]"), f"{label} provider request",
                          allow_policy='"wallet_prepareCalls"' in (call.get("body") or ""))
    return ok


def elapsed_class(seconds):
    return "timeout" if seconds >= 9 else "slow" if seconds >= 1 else "fast"


def send(harness, port, case, target, device, recorded_headers, body_hex, method="POST"):
    body = bytes.fromhex(body_hex) if body_hex is not None else None
    started = time.monotonic()
    result = edges.raw(port, method, target, request_headers(device, recorded_headers), body)
    return result, time.monotonic() - started


def replay_single(harness, port, kind, case):
    name, group = case["name"], case["group"]
    label = f"{kind}/{group}/{name}"
    harness.quota.reserve(case["device"])
    harness.lanes.reserve(case["upstream"])
    harness.clock.write_text(str(case["at"]))
    harness.tickets.write_text(str(case["ticketCounter"]))
    harness.upstream.start_case(case["providers"])
    actual, seconds = send(harness, port, case, case["target"], case["device"], case["requestHeaders"],
                           case["bodyHex"], case["method"])
    time.sleep(0.03 if actual[0] < 500 and seconds < 1 else 0.25)
    observed = harness.upstream.calls()
    harness.no_secret(actual[2], f"{label} response")
    status, headers, payload = actual
    if name in ROUTER:
        ok = edges.native_plain_error(label, actual, 404, NATIVE_NOT_FOUND, head=name == "router-head")
        ok = check(case["status"] == 404 and not observed, f"{label}: TypeScript {case['status']}, native requests "
                                                          f"{len(observed)}") and ok
        return "asserted" if ok else "failed"
    if group == "quota" and name in QUOTA_RETRY:
        retry = headers.get("retry-after", "")
        ok = check(retry.isdigit() and 1 <= int(retry) <= 60 and case["headers"].get("retry-after") == "60",
                   f"{label}: retry-after {retry!r} (TypeScript {case['headers'].get('retry-after')!r})")
        expected = dict(case, headers={k: v for k, v in case["headers"].items() if k != "retry-after"})
        headers = {k: v for k, v in headers.items() if k != "retry-after"}
        ok = compare_response(label, expected, (status, headers, payload), group) and ok
        ok = compare_upstream(harness, label, case["upstream"], observed) and ok
        return "asserted" if ok else "failed"
    if kind == "diagnose" and name == "valid-fee-640-bit":
        expected = {"status": 500, "responseText": '{"error":"INTERNAL"}',
                    "headers": {k: v for k, v in case["headers"].items() if k != "vary"}}
        expected["headers"]["content-length"] = "20"
        ok = check(case["status"] == 200, f"{label}: TypeScript {case['status']}")
        ok = compare_response(label, expected, actual, group) and ok
        ok = compare_upstream(harness, label, case["upstream"], observed) and ok
        return "asserted" if ok else "failed"
    ok = compare_response(label, case, actual, group)
    ok = compare_upstream(harness, label, case["upstream"], observed) and ok
    ok = check(elapsed_class(seconds) == case["elapsedClass"] or
               (case["elapsedClass"] != "timeout" and elapsed_class(seconds) != "timeout"),
               f"{label}: native {seconds:.2f} s ({elapsed_class(seconds)}), TypeScript {case['elapsedClass']}") and ok
    return "matched" if ok else "failed"


def replay_concurrent(harness, port, kind, case):
    label = f"{kind}/{case['group']}/{case['name']}"
    for entry in case["concurrent"]:
        harness.quota.reserve(entry["device"])
    harness.lanes.reserve(case["upstream"])
    harness.clock.write_text(str(case["at"]))
    harness.tickets.write_text(str(case["ticketCounter"]))
    harness.upstream.start_case(case["providers"])
    results = [None] * len(case["concurrent"])
    barrier = threading.Barrier(len(case["concurrent"]))

    def one(index, entry):
        barrier.wait()
        try:
            results[index] = send(harness, port, case, entry["target"], entry["device"], entry["requestHeaders"],
                                  entry["bodyHex"])[0]
        except (OSError, ValueError) as error:
            results[index] = (0, {}, repr(error).encode())

    threads = [threading.Thread(target=one, args=(i, e)) for i, e in enumerate(case["concurrent"])]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    time.sleep(0.3)
    observed = harness.upstream.calls()

    def key(status, headers, payload):
        return (status, tuple((h, headers.get(h)) for h in HEADERS if h != "x-rate-limit-remaining"),
                TICKET.sub(lambda m: m.group(1) + b"T" * 32, payload))

    # Simultaneous prepares draw tickets n, n+1, … in the order their jobs
    # reach the store, which differs between runs and servers when the
    # answers differ in encoding: answers are compared with the ticket text
    # replaced, and the drawn tickets as a set.
    def tickets(payloads):
        return collections.Counter(m.group(2) for p in payloads for m in TICKET.finditer(p))

    want = collections.Counter(key(r["status"], r["headers"], edges.expected_body(r)) for r in case["responses"])
    got = collections.Counter(key(*r) for r in results)
    ok = check(want == got, f"{label}: answers differ; TypeScript only "
                            f"{[(s, p[:160]) for s, _, p in (want - got).elements()]}; native only "
                            f"{[(s, p[:160]) for s, _, p in (got - want).elements()]}")
    drawn = tickets(r[2] for r in results)
    ok = check(drawn == tickets(edges.expected_body(r) for r in case["responses"]),
               f"{label}: drawn tickets {sorted(drawn)} differ from TypeScript's") and ok
    for result in results:
        harness.no_secret(result[2], f"{label} response")
    ok = compare_upstream(harness, label, case["upstream"], observed) and ok
    return "matched" if ok else "failed"


def fill(harness, port, spec):
    """The cap group's 1,000 prepares (TypeScript filled them through inject)."""
    harness.upstream.start_case(spec["providers"])
    harness.tickets.write_text(str(spec["firstCounter"]))
    body = bytes.fromhex(spec["bodyHex"])
    devices = spec["devices"]
    wallet = [{"url": "https://api.g.alchemy.com/v2/x"}]
    statuses = collections.Counter()
    started = time.monotonic()
    for i in range(spec["count"]):
        device = devices[i % len(devices)]
        harness.quota.reserve(device)
        harness.lanes.reserve(wallet)
        harness.clock.write_text(str(spec["start"] + i * spec["stepMs"]))
        status, _, payload = edges.raw(port, "POST", ROUTES["prepare"],
                                       [("authorization", "Bearer " + edges.token(device)),
                                        ("content-type", "application/json")], body)
        statuses[status] += 1
        if status != 200:
            check(False, f"cap fill {i}: {status} {payload[:120]!r}")
            break
    print(f"  cap fill: {dict(statuses)} in {time.monotonic() - started:.1f} s", flush=True)


def wait_breaker_gap(group, previous_at, case):
    """Native breakers cool down on the monotonic clock: sleep the frozen gap."""
    if not group.startswith("failure") or previous_at is None:
        return
    gap = (case["at"] - previous_at) / 1000
    if 0.5 <= gap <= 32:
        time.sleep(gap)


def replay_fixture(harness, kind, fixture, groups_wanted):
    row, _ = FILES[kind]
    harness.secrets = [edges.token(letter) for letter in fixture["devices"]] + \
        [edges.token(letter) for letter in fixture["edgeDevices"]] + list(fixture["devices"].values()) + \
        list(fixture["edgeDevices"].values()) + [value for _, value in fixture["clientHeaders"]]
    groups = {g["name"]: g for g in fixture["groups"]}
    by_group = collections.OrderedDict()
    for case in fixture["cases"]:
        by_group.setdefault(case["group"], []).append(case)
    for name in [g["name"] for g in fixture["groups"]]:
        if name not in groups_wanted or name not in by_group and "fill" not in groups[name]:
            continue
        group = groups[name]
        kv_name = f"{kind}-restart" if name.startswith("restart-") else f"{kind}-{name}"
        if name == "restart-a":
            shutil.rmtree(harness.root / f"kv-{kv_name}", ignore_errors=True)
        server = harness.server(kv_name, group["env"], group["start"])
        results = collections.Counter()
        try:
            cases = by_group.get(name, [])
            if name == "quota":
                catalog.wait_for_fresh_minute(40)
            if "fill" in group:
                fill(harness, server.port, dict(group["fill"], start=group["start"]))
            previous_at = None
            for case in cases:
                if name == "quota" and case["name"] == "quota-last":
                    harness.upstream.start_case({})
                    edges.drain_quota(server, ROUTES[kind], "d", 20, b"{}", max_requests=12, expected_cost=20)
                wait_breaker_gap(name, previous_at, case)
                previous_at = case["at"]
                outcome = (replay_concurrent if "concurrent" in case else replay_single)(harness, server.port, kind,
                                                                                          case)
                results[outcome] += 1
                COUNTS[(row, outcome)] += 1
        finally:
            output = server.stop(signal.SIGKILL if name == "restart-a" else signal.SIGTERM)
            harness.no_secret(output, f"{kind}/{name} process output")
        print(f"{row} {kind}/{name}: {results['matched']} matched, {results['asserted']} asserted, "
              f"{results['failed']} failed", flush=True)


# ---- native-only ------------------------------------------------------------------------------------------

def saturation(harness, fixture):
    case = next(c for c in fixture["cases"] if c["name"] == "json-valid")
    server = harness.server("prepare-saturation", {"ALCHEMY_API_KEY": "fixture-alchemy-key",
                                                    "ALCHEMY_GAS_POLICY_ID": "fixture-gas-policy"}, case["at"])
    try:
        harness.clock.write_text(str(case["at"]))
        harness.tickets.write_text("5000")
        table = dict(case["providers"])
        key = next(k for k in table if k.endswith("wallet_prepareCalls"))
        table[key] = [dict(entry, delayMs=1500) for entry in table[key]]
        harness.upstream.start_case(table)
        letters = "ABCDEFGHIJKL"
        results = [None] * 12
        barrier = threading.Barrier(12)

        def one(i):
            barrier.wait()
            results[i] = edges.raw(server.port, "POST", ROUTES["prepare"],
                                   request_headers(letters[i], case["requestHeaders"]), bytes.fromhex(case["bodyHex"]))

        threads = [threading.Thread(target=one, args=(i,)) for i in range(12)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        expected = json.loads(case["responseText"])
        exact = pressure = 0
        for status, headers, payload in results:
            if status == 200:
                document = json.loads(payload)
                exact += document["prepared"] == expected["prepared"] and len(document["ticket"]) == 32
            elif status == 429 and headers.get("retry-after") == "2" and b'"RATE_LIMITED"' in payload:
                pressure += 1
        sends = [c for c in harness.upstream.calls() if c["url"].startswith("https://api.g.alchemy.com/")]
        check(exact + pressure == 12 and exact >= 8 and len(sends) == exact,
              f"saturation: {exact} exact 200, {pressure} back-pressure 429, {len(sends)} wallet calls")
        print(f"native-only saturation: 12 simultaneous prepares → {exact} × 200, {pressure} × typed 429; "
              f"{len(sends)} wallet_prepareCalls", flush=True)
    finally:
        harness.no_secret(server.stop(), "saturation output")


def hard_restart_submit(harness, prepare_fixture):
    case = next(c for c in prepare_fixture["cases"] if c["name"] == "json-valid")
    env = {"ALCHEMY_API_KEY": "fixture-alchemy-key", "ALCHEMY_GAS_POLICY_ID": "fixture-gas-policy"}
    shutil.rmtree(harness.root / "kv-submit-hard-restart", ignore_errors=True)
    server = harness.server("submit-hard-restart", env, case["at"])
    harness.clock.write_text(str(case["at"]))
    harness.tickets.write_text("7000")
    harness.upstream.start_case(case["providers"])
    status, _, payload = edges.raw(server.port, "POST", ROUTES["prepare"],
                                   request_headers("A", case["requestHeaders"]), bytes.fromhex(case["bodyHex"]))
    check(status == 200, f"hard restart: prepare {status}")
    ticket = json.loads(payload)["ticket"]
    body = json.dumps({"ticket": ticket, "signatures": ["0x" + "33" * 65]}).encode()
    send_key = "POST api.g.alchemy.com/v2/fixture-alchemy-key wallet_sendPreparedCalls"
    harness.upstream.start_case({send_key: [{"status": 200, "delayMs": 3000,
                                             "body": '{"jsonrpc":"2.0","id":1,"result":{"id":"0x01"}}'}]})
    outcome = []
    thread = threading.Thread(target=lambda: outcome.append(_try_raw(server.port, ticket, body)), daemon=True)
    thread.start()
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and not harness.upstream.calls():
        time.sleep(0.01)
    check(len(harness.upstream.calls()) == 1, "hard restart: the send never reached Alchemy")
    harness.no_secret(server.stop(signal.SIGKILL), "hard restart output")
    thread.join(timeout=5)
    server = harness.server("submit-hard-restart", env, case["at"] + 1000)
    try:
        harness.clock.write_text(str(case["at"] + 1000))
        status, _, payload = edges.raw(server.port, "POST", ROUTES["submit"],
                                       [("authorization", "Bearer " + edges.token("A")),
                                        ("content-type", "application/json")], body)
        sends = [c for c in harness.upstream.calls() if "wallet_sendPreparedCalls" in (c.get("body") or "")]
        check(status == 404 and payload == b'{"error":"SPONSORED_TRADE_NOT_FOUND"}' and len(sends) == 1,
              f"hard restart: after SIGKILL mid-send the submit answers {status} {payload!r}, {len(sends)} sends")
        print(f"native-only hard restart: SIGKILL while the send was at Alchemy (client saw "
              f"{outcome[0] if outcome else 'nothing'}); after restart 404, {len(sends)} send in total", flush=True)
    finally:
        harness.no_secret(server.stop(), "hard restart output")


def _try_raw(port, ticket, body):
    try:
        return edges.raw(port, "POST", ROUTES["submit"], [("authorization", "Bearer " + edges.token("A")),
                                                           ("content-type", "application/json")], body, timeout=10)[0]
    except (OSError, ValueError, IndexError) as error:
        return type(error).__name__


def production(binary, root):
    (root / "prod-tls").mkdir()
    ca, _, _, _ = tls_ca.create_certificates(root / "prod-tls", "DNS:localhost")
    auth = [("authorization", "Bearer " + edges.token("A")), ("content-type", "application/json")]
    for value in (None, "0"):
        env = {"FORTIS_CA_BUNDLE_PATH": str(ca)}
        if value is not None:
            env["FORTIS_NATIVE_ROBINHOOD_CANARY"] = value
        server = catalog.Server(binary, root / f"kv-prod-off-{value}", env)
        try:
            for route in ROUTES.values():
                status, _, _ = edges.raw(server.port, "POST", route, auth, b"{}")
                check(status == 404, f"production canary {value!r} {route}: {status}, not 404")
        finally:
            server.stop()
    server = catalog.Server(binary, root / "kv-prod-on", {"FORTIS_NATIVE_ROBINHOOD_CANARY": "1",
                                                           "FORTIS_CA_BUNDLE_PATH": str(ca),
                                                           "ALCHEMY_API_KEY": "prod-alchemy-key",
                                                           "ALCHEMY_GAS_POLICY_ID": "prod-gas-policy"})
    try:
        for route in ROUTES.values():
            status, _, body = edges.raw(server.port, "POST", route, [("content-type", "application/json")], b"{}")
            check(status == 401 and body == b'{"error":"UNAUTHORIZED"}', f"production no-auth {route}: {status} {body!r}")
            status, _, body = edges.raw(server.port, "POST", route, auth + [("origin", "https://evil.example")], b"{}")
            check(status == 403 and body == b'{"error":"BROWSER_ORIGIN_FORBIDDEN"}', f"production origin {route}: {status}")
            status, _, body = edges.raw(server.port, "POST", route, [("content-type", "text/xml")], b"<x/>")
            # fastify-errors: auth-first, so XML without a bearer is 401.
            check(status == 401 and body == b'{"error":"UNAUTHORIZED"}', f"production xml {route}: {status} {body!r}")
    finally:
        output = server.stop()
        check(b"prod-alchemy-key" not in output and b"prod-gas-policy" not in output, "production output leaks a key")
    print("production: canary unset/0 → 404 on all three routes; on → no bearer 401 (text/xml too), Origin 403", flush=True)


def load(kind):
    _, name = FILES[kind]
    fixture = edges.load_fixture(name, FIXTURES)
    edges.pin_self_check(name)
    return fixture


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    edges.FAIL_FAST = FAIL_FAST
    edges.FAILURES = FAILURES
    binary = sys.argv[1]
    kinds = [k for k in ("prepare", "submit", "diagnose") if k in selected("R43_R45_FIXTURES", FILES)]
    fixtures = {kind: load(kind) for kind in kinds}
    with tempfile.TemporaryDirectory(prefix="robinhood-writes-") as root:
        root = Path(root)
        harness = Harness(binary, root)
        try:
            for kind in kinds:
                wanted = selected("R43_R45_GROUPS", [g["name"] for g in fixtures[kind]["groups"]])
                replay_fixture(harness, kind, fixtures[kind], wanted)
            if os.environ.get("R43_R45_SKIP_NATIVE_ONLY") != "1" and "prepare" in fixtures:
                harness.secrets = [edges.token(letter) for letter in fixtures["prepare"]["devices"]]
                saturation(harness, fixtures["prepare"])
                hard_restart_submit(harness, fixtures["prepare"])
        finally:
            harness.upstream.close()
        if len(sys.argv) == 3 and os.environ.get("R43_R45_SKIP_NATIVE_ONLY") != "1":
            production(sys.argv[2], root)
    for row in ("R43", "R44", "R45"):
        if any(r == row for r, _ in COUNTS):
            print(f"{row}: {COUNTS[(row, 'matched')]} matched, {COUNTS[(row, 'asserted')]} asserted differences, "
                  f"{COUNTS[(row, 'failed')]} failed")
    total = collections.Counter()
    for (_, outcome), count in COUNTS.items():
        total[outcome] += count
    print(f"robinhood writes: {total['matched']} matched, {total['asserted']} asserted differences, "
          f"{total['failed']} failed ({len(FAILURES)} failure messages)")
    if FAILURES:
        raise SystemExit(1)
    print("robinhood writes edges: all checks passed")


if __name__ == "__main__":
    main()
