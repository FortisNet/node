#!/usr/bin/env python3
"""Real-socket edges of GET /v1/networks/{network}/address/{address}/token-holdings
(R16) against tests/corpus/tron_token_holdings_edges.json, which
the earlier TypeScript implementation's tests recorded
from the real Fastify app over real sockets.

The harness is R13's (tests/address_balances_edges_test.py): the test binary
(build/fortis_node_address_route_test) sends every provider host to one
loopback HTTPS upstream that answers, per case, the exact bytes TypeScript was
served in the order TypeScript asked for them, after the recorded per-reply
delay (a host the case marked down answers 503). Each case's frozen instant
is written to the shared clock file first and the native provider lanes are
paced by monotonic time. Each fixture group is a fresh process with its own
KV directory; restart-b reuses restart-a's after a SIGKILL.

Checks, per case: the status, every recorded header (x-rate-limit-remaining
only in the quota group), the exact JSON text or protobuf bytes, the multiset
of provider requests (background revalidations included, collected after
/probe/test-address-idle) and their key placement: every TronScan request
carries TRON-PRO-API-KEY with the configured key and accept:
application/json, as TypeScript's did, and the key never appears in a URL,
request body, response or process output. Concurrent cases compare every
response and the combined provider requests. `state` records prove through
POST /probe/test-address-state that a read wrote no watch/topic, account row
or balance snapshot; `envelopes` records compare the stored SWR envelope
(asOf, freshUntil, staleUntil and the stored JSON text) through POST
/probe/test-address-swr. No provider request may carry a client header or
device credential, and no device token or id may reach a response or output.

Native-only bounded rules (timing-dependent, so not frozen by the oracle):
  - `bounded-workers`: ten distinct cold reads at once against a 400 ms
    TronScan all answer 200 with their own holdings (three foreground
    workers, the rest queued), ten provider requests, never more than two
    in flight (the provider execution adapter's two HTTPS workers; the
    TronScan lane itself allows three);
  - the production composition: the route answers 404 unless
    FORTIS_NATIVE_ADDRESS_CANARY=1, then 401/403 before any facet work.

Asserted differences (documented beside handle_holdings):
  - router: other methods and spellings are 404 in both (status only); the
    routing 414 and Node's "Client Error" 400 carry the native security
    headers; a malformed path escape is the parser's {"error":"BAD_REQUEST"}
    (Fastify FST_ERR_BAD_URL); a raw '#' is refused by the request parser
    (TypeScript drops the fragment and serves the read), and so is a raw
    byte that leaves the decoded path invalid UTF-8 (Node: "Client Error");
  - restart: the SWR envelope lives in the durable KV natively, so a read
    5 s after a restart is served from it (same bytes, no provider request)
    where TypeScript's new process reads live;
  - a cached protobuf answer within 20 s of a live read: TypeScript answers
    its in-process object, so a -0 or +-Infinity TronScan sent is that
    double; native answers the stored JSON (0, null_value), as TypeScript
    itself does once it reads the entry back from its KV;
  - a protobuf answer for a row that lacks one of the eight display fields:
    TypeScript's Struct carries an entry with an empty Value for it (its
    object literal holds undefined; the Dart decoder throws on such a
    Value), native omits the member as JSON does; every other byte equal;
  - the lane breaker's Retry-After counts the cooldown on the monotonic
    clock (TypeScript: the frozen case Date): 1..30;
  - a TronScan body over 512 KiB (the provider execution bound): native
    fails the read (502, then the 10 s hold) where TypeScript serves it.

Usage: tron_token_holdings_edges_test.py build/fortis_node_address_route_test [build/fortis_node]
"""
import collections
import hashlib
import json
import math
import os
import signal
import struct
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compat_expectations  # noqa: E402
import address_balances_edges_test as edges  # noqa: E402
import address_insight_edges_test as insight  # noqa: E402
import address_routes_test as routes  # noqa: E402
import discovery_route_harness as harness  # noqa: E402
import robinhood_stock_catalog_edges_test as catalog  # noqa: E402
import tls_ca  # noqa: E402

FIXTURE = "tron_token_holdings_edges.json"
check = edges.check
ROUTER_404 = {"router-post", "router-trailing-slash", "router-upper-path", "router-singular"}
RESTART_DIFFERENCE = {"restart-after-fresh"}
L1_PROTOBUF = {"odd-numbers-cached-protobuf"}
OVER_BODY_BOUND = {"huge-over-512kib", "huge-over-512kib-again"}
# The case that opens the TronScan lane breaker -> the case after its cooldown.
BREAKER = {"breaker-429": "breaker-recovered", "breaker-fault-5": "breaker-faults-recovered"}
TRONSCAN = "apilist.tronscanapi.com"


def load_fixture():
    data = (harness.FIXTURES / FIXTURE).read_bytes()
    pinned = (harness.FIXTURES / f"{FIXTURE}.sha256").read_text().split()[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{FIXTURE}: sha256 {digest} does not match the pinned {pinned}")
    return json.loads(data)


# ---- google.protobuf.Struct decoding (for the -0/Infinity rule) ----------------------------------

def _varint(data, at):
    shift = value = 0
    while True:
        byte = data[at]
        at += 1
        value |= (byte & 0x7f) << shift
        if not byte & 0x80:
            return value, at
        shift += 7


def _fields(data):
    at = 0
    while at < len(data):
        tag, at = _varint(data, at)
        number, wire = tag >> 3, tag & 7
        if wire == 0:
            value, at = _varint(data, at)
        elif wire == 1:
            value, at = data[at:at + 8], at + 8
        elif wire == 2:
            length, at = _varint(data, at)
            value, at = data[at:at + length], at + length
        else:
            raise ValueError(f"wire type {wire}")
        yield number, value


def _value(data):
    for number, value in _fields(data):
        if number == 1:
            return ("null",)
        if number == 2:
            return ("number", struct.unpack("<d", value)[0])
        if number == 3:
            return ("string", value.decode("utf-8"))
        if number == 4:
            return ("bool", bool(value))
        if number == 5:
            return _struct(value)
        if number == 6:
            return ("list", [_value(item) for n, item in _fields(value) if n == 1])
    return ("empty",)


def _struct(data):
    out = []
    for number, entry in _fields(data):
        if number != 1:
            continue
        key, value = None, ("empty",)
        for n, v in _fields(entry):
            if n == 1:
                key = v.decode("utf-8")
            elif n == 2:
                value = _value(v)
        out.append((key, value))
    return ("struct", out)


def decode_structured(payload):
    for number, value in _fields(payload):
        if number == 1:
            return _struct(value)
    return None


def exact(node):
    """A comparable form that tells -0 from 0."""
    kind = node[0]
    if kind == "number":
        return ("number", repr(node[1]))
    if kind == "struct":
        return ("struct", [(k, exact(v)) for k, v in node[1]])
    if kind == "list":
        return ("list", [exact(v) for v in node[1]])
    return node


def json_round_trip(node):
    """What JSON.stringify + JSON.parse leaves of a decoded Struct value:
    -0 becomes 0 and a non-finite number null."""
    kind = node[0]
    if kind == "number":
        number = node[1]
        if not math.isfinite(number):
            return ("null",)
        return ("number", 0.0 if number == 0 else number)
    if kind == "struct":
        return ("struct", [(k, json_round_trip(v)) for k, v in node[1]])
    if kind == "list":
        return ("list", [json_round_trip(v) for v in node[1]])
    return node


def drop_undefined(node):
    """TypeScript builds the Struct from its object literal, so a member
    the TronScan row lacked (undefined) becomes an entry whose Value has no
    kind; JSON.stringify, its own KV copy and native omit the member."""
    kind = node[0]
    if kind == "struct":
        return ("struct", [(k, drop_undefined(v)) for k, v in node[1] if v != ("empty",)])
    if kind == "list":
        return ("list", [drop_undefined(v) for v in node[1]])
    return node


def has_undefined(node):
    kind = node[0]
    if kind == "struct":
        return any(v == ("empty",) or has_undefined(v) for _, v in node[1])
    if kind == "list":
        return any(has_undefined(v) for v in node[1])
    return False


def protobuf_headers(case, headers, payload):
    ok = True
    for name in edges.HEADERS:
        if name in ("content-length", "x-rate-limit-remaining"):
            continue
        ok = check(headers.get(name) == case["headers"].get(name),
                   f"{case['name']}: header {name} {headers.get(name)!r} != {case['headers'].get(name)!r}") and ok
    return check(headers.get("content-length") == str(len(payload)), f"{case['name']}: content-length") and ok


def undefined_members(case, actual, observed, h):
    """A protobuf answer for rows missing display fields: TypeScript's
    carries an empty google.protobuf.Value for each missing member (which
    the Dart decoder refuses); native omits the member. Everything else,
    including every double's bits, is equal."""
    status, headers, payload = actual
    try:
        native = exact(decode_structured(payload))
        typescript = decode_structured(bytes.fromhex(case["wireHex"]))
    except (ValueError, IndexError, UnicodeDecodeError, struct.error) as error:
        return check(False, f"{case['name']}: protobuf decode {error!r}")
    expected = exact(drop_undefined(typescript))
    ok = check(status == case["status"] and has_undefined(typescript) and native == expected,
               f"{case['name']}: native {str(native)[:600]}\n  expected {str(expected)[:600]}")
    ok = protobuf_headers(case, headers, payload) and ok
    return edges.compare_upstream(case["name"], case, observed, h.secrets) and ok


def l1_protobuf(case, actual, observed):
    """TypeScript answered its in-process object (-0 and +-Infinity kept as
    doubles); native the stored JSON (0 and null_value). Same document after
    that JSON round trip, same headers but content-length, no request."""
    status, headers, payload = actual
    try:
        native = exact(decode_structured(payload))
        typescript = drop_undefined(decode_structured(bytes.fromhex(case["wireHex"])))
        expected = exact(json_round_trip(typescript))
        changed = expected != exact(typescript)
    except (ValueError, IndexError, UnicodeDecodeError, struct.error) as error:
        return check(False, f"{case['name']}: protobuf decode {error!r}")
    ok = check(status == 200 and native == expected and changed,
               f"{case['name']}: native {native!r}\n  expected {expected!r} (TypeScript's differs: {changed})")
    ok = protobuf_headers(case, headers, payload) and ok
    return check(not observed, f"{case['name']}: provider requests {observed}") and ok


def over_body_bound(case, actual, observed):
    """A TronScan answer over 512 KiB: TypeScript serves (then caches) it;
    native's provider execution refuses the body, so the read fails (502)
    and the failure is held 10 s (no second request)."""
    ok = check(actual[0] == 502 and actual[2] == b'{"error":"UPSTREAM_UNAVAILABLE"}',
               f"{case['name']}: native {actual[0]} {actual[2][:120]!r}")
    calls = [t for _, h, t, _ in observed if h == TRONSCAN]
    if case["name"].endswith("-again"):
        return check(not observed, f"{case['name']}: held read asked TronScan {calls}") and ok
    return check(1 <= len(calls) <= 3 and all("/api/account/wallet?address=" in t for t in calls),
                 f"{case['name']}: provider requests {observed}") and ok


ASSERTED = {
    "address-malformed-escape": edges.bad_url, "fragment": insight.fragment_refusal,
    "raw-latin1-address": insight.invalid_utf8_path,
}


class DelayedTable(insight.TimedTable):
    """Answers each request after the delay its TypeScript reply had."""

    def start_case(self, case):
        super().start_case(case)
        with self.lock:
            self.delays = {}
            for call in case.get("upstream", []):
                if "delayMs" in call:
                    key = routes.request_key(call["method"], call["url"], call.get("requestBody") or "")
                    self.delays[key] = call["delayMs"]

    def answer(self, method, host, target, body):
        status, payload, delay, is_json = super().answer(method, host, target, body)
        if self.override is None:
            delay = getattr(self, "delays", {}).get(routes.request_key(method, f"https://{host}{target}", body), delay)
        return status, payload, delay, is_json


def key_placement(h, case, before, count):
    """Every native TronScan request carries the key header and accept that
    TypeScript's request with the same URL carried."""
    expected = {}
    for call in case.get("upstream", []):
        expected.setdefault(routes.request_key(call["method"], call["url"], call.get("requestBody") or ""),
                            call.get("headers") or {})
    ok = True
    calls = h.upstream.calls()[before:before + count]
    headers = h.upstream.call_headers()[before:before + count]
    for (method, host, target, body), sent in zip(calls, headers):
        want = expected.get(routes.request_key(method, f"https://{host}{target}", body))
        if want is None:
            continue  # compare_upstream reports the unexpected request
        for name in ("tron-pro-api-key", "accept"):
            ok = check(sent.get(name) == want.get(name),
                       f"{case['name']}: {name} {sent.get(name)!r} != {want.get(name)!r}") and ok
        for leaked in ("authorization", "cookie", "x-forwarded-for", "origin"):
            ok = check(leaked not in sent, f"{case['name']}: request carries {leaked}") and ok
    return ok


def replay_envelopes(h, server, case):
    harness.set_clock(h.clock, case["at"])
    body = json.dumps({"keys": [entry["key"] for entry in case["envelopes"]]}).encode()
    status, _, payload = server.request("POST", "/probe/test-address-swr", body=body,
                                        headers={"Content-Type": "application/json"})
    if not check(status == 200, f"{case['name']}: swr probe {status} {payload[:200]!r}"):
        return False
    got = json.loads(payload)["envelopes"]
    ok = check(len(got) == len(case["envelopes"]), f"{case['name']}: envelope count")
    for want, have in zip(case["envelopes"], got):
        ok = check(have == want, f"{case['name']}: {want['key']}\n  expected {json.dumps(want)[:800]}\n"
                                 f"  actual   {json.dumps(have)[:800]}") and ok
    return ok


def replay_case(h, server, case, lanes):
    name = case["name"]
    if "state" in case:
        return "matched" if edges.replay_state(h, server, case) else "failed"
    if "envelopes" in case:
        return "matched" if replay_envelopes(h, server, case) else "failed"
    lanes.reserve(case)
    harness.set_clock(h.clock, case["at"])
    h.table.start_case(case)
    before = len(h.upstream.calls())
    target = bytes.fromhex(case["targetHex"]).decode("latin1")
    if "concurrent" in case:
        kinds = case["concurrent"]
        results = [None] * len(kinds)
        barrier = threading.Barrier(len(kinds))

        def one(index, entry):
            barrier.wait()
            if entry.get("startDelayMs"):
                time.sleep(entry["startDelayMs"] / 1000)
            try:
                results[index] = catalog.raw(server.port, "HEAD" if entry["kind"] == "head" else "GET",
                                             entry.get("target") or target, edges.headers_of(case, entry["kind"]))
            except (OSError, IndexError, ValueError) as error:
                results[index] = (0, {}, repr(error).encode())

        threads = [threading.Thread(target=one, args=(i, entry)) for i, entry in enumerate(kinds)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        observed = edges.settle(server, h.upstream, before, len(case["upstream"]))
        edges.repay(lanes, case, observed)
        ok = True
        for index, (record, actual) in enumerate(zip(case["responses"], results)):
            h.no_secret(actual[2], name)
            ok = edges.compare(f"{name}[{index}:{kinds[index]['kind']}]", record, actual) and ok
        ok = edges.compare_upstream(name, case, observed, h.secrets) and ok
        ok = key_placement(h, case, before, len(observed)) and ok
        return "matched" if ok else "failed"
    try:
        actual = catalog.raw(server.port, case["method"], target, edges.headers_of(case))
    except OSError as error:
        check(False, f"{name}: {error!r} (TypeScript {case['status']})")
        edges.settle(server, h.upstream, before, len(case["upstream"]))
        return "failed"
    h.no_secret(actual[2], name)
    observed = edges.settle(server, h.upstream, before, len(case["upstream"]))
    edges.repay(lanes, case, observed)
    placed = key_placement(h, case, before, len(observed))
    if not placed:
        return "failed"
    if compat_expectations.malformed_path_target(case):
        # opaque-params: an empty, control-character or over-long
        # path value is the router's 404 before any handler.
        ok = check(compat_expectations.router_refused(actual[0], actual[1], actual[2], observed),
                   f"{name}: malformed path value is not the router's 404")
        return "asserted" if ok else "failed"
    if name in ROUTER_404:
        check(case["status"] == 404 and actual[0] == 404 and not observed, f"{name}: status {actual[0]} != 404")
        return "asserted"
    rule = ASSERTED.get(name)
    if rule:
        return "asserted" if rule(case, actual, observed) else "failed"
    if case["status"] in (414, 431) or name.startswith("raw-utf8"):
        ok = edges.compare(name, case, actual, baseline=True) and \
            edges.compare_upstream(name, case, observed, h.secrets)
        return "asserted" if ok else "failed"
    if name in L1_PROTOBUF:
        return "asserted" if l1_protobuf(case, actual, observed) else "failed"
    if name in OVER_BODY_BOUND:
        return "asserted" if over_body_bound(case, actual, observed) else "failed"
    if "wireHex" in case and actual[2] != bytes.fromhex(case["wireHex"]) and \
            has_undefined(decode_structured(bytes.fromhex(case["wireHex"]))):
        return "asserted" if undefined_members(case, actual, observed, h) and placed else "failed"
    if case["status"] == 429 and case["headers"].get("retry-after") and actual[0] == 429 and \
            b"temporarily busy" in actual[2]:
        native_after = int(actual[1].get("retry-after", "0"))
        check(1 <= native_after <= 30, f"{name}: retry-after {native_after} outside [1, 30]")
        record = dict(case, headers=dict(case["headers"], **{"retry-after": str(native_after)}))
        ok = edges.compare(name, record, actual) and edges.compare_upstream(name, case, observed, h.secrets)
        return "asserted" if ok else "failed"
    if name in RESTART_DIFFERENCE:
        # The SWR envelope is in the durable KV natively: 5 s after the
        # restart the entry is still fresh, served without a request.
        ok = edges.compare(name, case, actual) and check(not observed, f"{name}: asked TronScan {observed}")
        return "asserted" if ok else "failed"
    ok = edges.compare(name, case, actual, remaining=case["group"] == "quota")
    ok = edges.compare_upstream(name, case, observed, h.secrets) and ok
    return "matched" if ok and placed else "failed"


def bounded_workers(h, root, fixture):
    """Ten distinct cold addresses at once, TronScan answering after 400 ms:
    every read answers 200 with its own holdings, and no more than two
    provider requests overlap (the execution adapter's HTTPS workers)."""
    addresses = [f"TBound{index}" + "1" * (33 - len(f"Bound{index}")) for index in range(1, 10)] + \
        ["TBoundz" + "1" * 27]

    def answer(method, host, target, body):
        address = target.split("address=", 1)[1]
        document = {"data": [{"token_type": 20, "token_id": address, "balance": "1"}]}
        return 200, json.dumps(document).encode(), 400, True

    group = next(g for g in fixture["groups"] if g["name"] == "main")
    server = h.server("bounded", group["env"], fixture["now"] + 20 * 86400000)
    try:
        h.table.override = answer
        h.table.spans = []
        results = [None] * len(addresses)
        barrier = threading.Barrier(len(addresses))

        def one(index):
            barrier.wait()
            results[index] = catalog.raw(server.port, "GET", f"/v1/networks/tron/address/{addresses[index]}/token-holdings",
                                         {"authorization": "Bearer " + catalog.token("A")})

        threads = [threading.Thread(target=one, args=(i,)) for i in range(len(addresses))]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        h.table.override = None
        ok = True
        for index, result in enumerate(results):
            document = json.loads(result[2]) if result and result[0] == 200 else {}
            ok = check(document == {"data": [{"token_type": 20, "token_id": addresses[index], "balance": "1"}]},
                       f"bounded-workers[{index}]: {result[0] if result else None} "
                       f"{result[2][:120] if result else b''!r}") and ok
        events = sorted([(start, 1) for start, _ in h.table.spans] + [(end, -1) for _, end in h.table.spans])
        peak = current = 0
        for _, delta in events:
            current += delta
            peak = max(peak, current)
        ok = check(len(h.table.spans) == 10 and peak <= 2,
                   f"bounded-workers: {len(h.table.spans)} provider requests, peak in flight {peak} (bound 2)") and ok
        print(f"bounded-workers: 10 cold reads, {len(h.table.spans)} provider requests, peak in flight {peak}",
              flush=True)
    finally:
        h.table.override = None
        server.stop()
        h.no_secret(server.process.stdout.read() + server.process.stderr.read(), "bounded output")
    return ok


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = load_fixture()
    only = set(filter(None, os.environ.get("R16_ONLY_GROUPS", "").split(",")))
    groups = {g["name"]: g for g in fixture["groups"]}
    by_group = collections.OrderedDict()
    for case in fixture["cases"]:
        by_group.setdefault(case["group"], []).append(case)
    totals = collections.Counter()
    with tempfile.TemporaryDirectory(prefix="tron-holdings-") as root:
        root = Path(root)
        h = edges.Harness(binary, root, fixture)
        h.table = DelayedTable(fixture["cases"])
        h.upstream.table = h.table
        # The TronScan key may reach TronScan only as its header.
        h.secrets += [fixture["key"], "test-tronscan-key"]
        opened = {}
        try:
            for name, cases in by_group.items():
                if only and name not in only and not (name.startswith("restart") and "restart" in only):
                    continue
                group = groups[name]
                server = h.server(group["kv"], group["env"], group["start"])
                lanes = routes.Lanes()
                results = collections.Counter()
                try:
                    if name == "quota":
                        catalog.wait_for_fresh_minute(25)
                        for _ in range(48):
                            status, _, _ = catalog.raw(server.port, "GET", "/v1/networks/solana/address/"
                                                       + fixture["holders"]["PLAIN"] + "/token-holdings",
                                                       {"authorization": "Bearer " + catalog.token("d")})
                            if not check(status == 400, f"quota drain answered {status}"):
                                break
                    for case in cases:
                        waiting_for = [opener for opener, after in BREAKER.items() if after == case["name"]]
                        for opener in waiting_for:
                            if opener in opened:
                                wait = opened[opener] + 31 - time.monotonic()
                                if wait > 0:
                                    time.sleep(wait)
                        results[replay_case(h, server, case, lanes)] += 1
                        if case["name"] in BREAKER:
                            opened[case["name"]] = time.monotonic()
                        if os.environ.get("R16_FAIL_FAST") and edges.FAILURES:
                            raise SystemExit(1)
                finally:
                    if name == "restart-a":
                        server.process.send_signal(signal.SIGKILL)
                        server.process.wait()
                    else:
                        server.stop()
                    h.no_secret(server.process.stdout.read() + server.process.stderr.read(), f"{name} output")
                totals.update(results)
                print(f"{name}: {results['matched']} matched, {results['asserted']} asserted differences, "
                      f"{results['failed']} failed", flush=True)
            if not only or "bounded" in only:
                totals["matched" if bounded_workers(h, root, fixture) else "failed"] += 1
        finally:
            h.upstream.close()
        if h.table.extra:
            print(f"requests TypeScript never made: {sorted(set(h.table.extra))[:10]}")
        if len(sys.argv) == 3 and not only:
            production(sys.argv[2], root, fixture)
    print(f"tron token holdings: {totals['matched']} matched, {totals['asserted']} asserted differences, "
          f"{totals['failed']} failed")
    if edges.FAILURES:
        print(f"{len(edges.FAILURES)} failure(s)")
        raise SystemExit(1)
    print("tron token holdings edges: all checks passed")


def production(binary, root, fixture):
    """The production composition: the route exists only behind the canary."""
    (root / "prod-tls").mkdir()
    ca, _, _, _ = tls_ca.create_certificates(root / "prod-tls", "DNS:localhost")
    path = "/v1/networks/tron/address/" + fixture["holders"]["PLAIN"] + "/token-holdings"
    for value in (None, "0"):
        env = {"FORTIS_CA_BUNDLE_PATH": str(ca), "TRONSCAN_API_KEY": fixture["key"]}
        if value is not None:
            env["FORTIS_NATIVE_ADDRESS_CANARY"] = value
        server = catalog.Server(binary, root / f"kv-prod-off-{value}", env)
        try:
            status, _, _ = catalog.raw(server.port, "GET", path, {"authorization": "Bearer " + catalog.token("a")})
            check(status == 404, f"production canary {value!r}: {status}, not 404")
        finally:
            server.stop()
    server = catalog.Server(binary, root / "kv-prod-on", {"FORTIS_NATIVE_ADDRESS_CANARY": "1",
                                                           "FORTIS_CA_BUNDLE_PATH": str(ca),
                                                           "TRONSCAN_API_KEY": fixture["key"]})
    try:
        status, headers, body = catalog.raw(server.port, "GET", path, {})
        check(status == 401 and body == b'{"error":"UNAUTHORIZED"}' and "vary" not in headers,
              f"production no-auth: {status} {body!r}")
        status, _, body = catalog.raw(server.port, "GET", path, {"origin": "https://evil.example"})
        check(status == 403 and body == b'{"error":"BROWSER_ORIGIN_FORBIDDEN"}', f"production origin: {status}")
        status, _, body = catalog.raw(server.port, "GET", "/v1/networks/tron/address//token-holdings", {})
        # opaque-params: the router's 404 before any hook.
        check(status == 404, f"production empty address without a bearer: {status} {body!r}")
    finally:
        server.stop()
        output = server.process.stdout.read() + server.process.stderr.read()
        check(fixture["key"].encode() not in output, "production output carries the TronScan key")
    print("production: canary unset/0 answers 404; on, no bearer 401 and Origin 403")


if __name__ == "__main__":
    main()
