#!/usr/bin/env python3
"""Real-socket edges of GET /v1/networks/{network}/tx/{hash}/view (R27)
against tests/corpus/transaction_view_edges.json, which
the earlier TypeScript implementation's tests recorded from
the real Fastify app over real sockets.

The harness is R13's and R26's (tests/address_balances_edges_test.py,
tests/transaction_edges_test.py): the test binary
(build/fortis_node_address_route_test) sends every provider host to one
loopback HTTPS upstream that answers, per case, the exact bytes TypeScript was
served in the order TypeScript asked for them (a host the case marked down
answers 503). Each case's frozen instant is written to the shared clock file
first and the native provider lanes are paced by monotonic time. Each fixture
group is a fresh process with its own KV directory; restart-b reuses
restart-a's after a SIGKILL. The quota group drains device d with 240 socket
requests (TypeScript: app.inject) before its cases.

Provider URLs are compared as fetch() puts them on the wire
(address_transfers_edges_test.wire_url: the oracle records the string
TypeScript passed to fetch; Blockscout's txhash= takes the hash unencoded).

Checks, per case: the status, every recorded header (x-rate-limit-remaining
only in the quota group), the exact JSON text or protobuf bytes, and the
multiset of provider requests (exact URL and JSON-RPC body), after the
background revalidations went idle. `state` records compare, through POST
/probe/test-tx-state, each device's watches (topic as typed, account link,
tracked tokens, expiry), that no tx/detail record or tx/pending entry exists
(the view writes none), and, through POST /probe/test-address-swr, every
named SWR envelope ({asOf, freshUntil, staleUntil, d: the stored JSON text}).
Concurrent cases compare every response and the combined provider requests.
No provider request may carry a client header or credential, and no device
token or id may reach a response or process output.

Asserted differences (documented beside handle_view), each by case name:
  - router: other methods and spellings are 404 in both (status only, no
    provider request); the routing 414 and Node's "Client Error" 400 carry
    the native security headers (body and other headers exact);
  - a malformed path escape is the parser's {"error":"BAD_REQUEST"}
    (Fastify FST_ERR_BAD_URL); a raw '#' is refused by the request parser
    (TypeScript drops the fragment and serves the read), and so is a raw
    byte that leaves the decoded path invalid UTF-8 (Node: "Client Error");
  - a decoded NUL in {hash} of a network with a view: 400 {"error":
    "BAD_REQUEST"} with no provider request and no topic (TypeScript reads
    the chain with it, 404, and remembers the topic); parser-state therefore
    lacks exactly the NUL topics;
  - restart: the SWR envelope is in the durable KV natively, so the first
    process's final view is answered from it after the restart (cache hit,
    no provider request) where TypeScript's new process (an in-memory Kv)
    reads live; restart-state holds the first process's envelope;
  - a raw '#' (`fragment`): TypeScript served the read and remembered its
    topic, so parser-state lacks that topic natively too;
  - a cached protobuf read inside TypeScript's 30 s SwrCache L1 window
    (evm-large-numbers-again-protobuf) is the live object there, so the raw
    transaction's 1e400 is number_value Infinity and -0 keeps its sign;
    native always answers the stored JSON envelope (null_value, 0) -- the
    JSON answers are equal;
  - a provider body over the native provider execution bound (512 KiB:
    evm-over-bound-body's ~600 KiB transaction): TypeScript serves the view
    and stores it; native's first read is refused, so the view is 502
    UPSTREAM_UNAVAILABLE after that one request and evm-state has no
    envelope for it;
  - the lane breaker's Retry-After counts the cooldown on the monotonic clock
    (TypeScript: the lane clock): 1..30, and the replay waits out the native
    30 s cooldown before the watch cases (TypeScript's lane clock jumped a
    day).

Usage: transaction_view_edges_test.py build/fortis_node_address_route_test [build/fortis_node]
R27_DRAFT_FIXTURE=<path> replays a development draft without the pin and
exits 2 (never a verdict). R27_ONLY_GROUPS=a,b and R27_FAIL_FAST=1 narrow a
run.
"""
import collections
import hashlib
import json
import os
import signal
import sys
import tempfile
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
import address_transfers_edges_test as transfers  # noqa: E402
import transaction_edges_test as r26  # noqa: E402

FIXTURE = "transaction_view_edges.json"
PINNED = "5298d7a21922602f958bb9fc66634d7001c09599b37efe9ff3372033a20f4dc1"
check = edges.check
ROUTER_404 = {"router-post", "router-trailing-slash", "router-upper-view", "router-views", "router-upper-path",
              "router-extra-segment", "router-empty-with-slash"}
NUL_HASH = {"escaped-nul-hash", "escaped-nul-hash-bitcoin"}
OVER_BOUND = "evm-over-bound-body"
OVER_BOUND_HASH = "0x" + "a7" * 32
RESTART_CACHED = {"restart-final-after"}
BREAKER_RECOVERY = "watch-first"
BREAKER_LAST_FAILURE = "breaker-failure-5"
# The Struct paths where TypeScript's L1 protobuf keeps the live number.
L1_PROTOBUF = {"evm-large-numbers-again-protobuf": [(("detail", "rawTx", "v"), "inf"),
                                                    (("detail", "rawTx", "w"), "-0")]}


def load_fixture():
    draft = os.environ.get("R27_DRAFT_FIXTURE")
    if draft:
        print(f"DRAFT replay of {draft}: the pin is not checked and this run is not a verdict", flush=True)
        fixture = json.loads(Path(draft).read_bytes())
    else:
        data = (harness.FIXTURES / FIXTURE).read_bytes()
        pinned = (harness.FIXTURES / f"{FIXTURE}.sha256").read_text().split()[0]
        digest = hashlib.sha256(data).hexdigest()
        if digest != pinned or digest != PINNED:
            raise SystemExit(f"{FIXTURE}: sha256 {digest} does not match the pinned {PINNED}")
        fixture = json.loads(data)
    for case in fixture["cases"]:
        for call in case.get("upstream", []):
            call["url"] = transfers.wire_url(call["url"])
    return fixture


# ---- asserted differences ------------------------------------------------------------------------

def nul_hash(case, actual, observed):
    """A decoded NUL in {hash} of a network with a view: TypeScript reads the
    chain with it (404 NOT_FOUND after one provider request); native refuses
    it after the network and family checks: 400 {"error":"BAD_REQUEST"},
    quota counted, no provider request."""
    return check(case["status"] == 404 and len(case["upstream"]) == 1 and actual[0] == 400 and
                 actual[2] == b'{"error":"BAD_REQUEST"}' and actual[1].get("x-rate-limit-cost") == "1" and
                 not observed, f"{case['name']}: native {actual[0]} {actual[2][:120]!r} ({observed})")


def over_bound(case, actual, observed):
    """A ~600 KiB eth_getTransactionByHash body: TypeScript serves the view
    (200 after five requests); native's provider execution refuses a body
    over 512 KiB, so the read fails after that one request: 502
    UPSTREAM_UNAVAILABLE."""
    methods = [json.loads(b).get("method") for _, _, _, b in observed if b]
    return check(case["status"] == 200 and actual[0] == 502 and actual[2] == b'{"error":"UPSTREAM_UNAVAILABLE"}'
                 and len(observed) == 1 and methods == ["eth_getTransactionByHash"],
                 f"{case['name']}: native {actual[0]} {actual[2][:120]!r} ({observed})")


def restart_cached(h, case, actual, observed):
    """After the restart the first process's envelope (durable KV natively)
    answers the final view: the TypeScript live document with cache
    {hit: true, timestamp: the first read's instant}, no provider request
    (TypeScript's in-memory Kv started empty, so it read live)."""
    before = next(c for c in h.fixture["cases"] if c["name"] == "restart-final-before")
    try:
        native, typescript = json.loads(actual[2]), json.loads(case["responseText"])
    except ValueError:
        return check(False, f"{case['name']}: not JSON {actual[2][:200]!r}")
    stamp = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(before["at"] / 1000)) + \
        f".{before['at'] % 1000:03d}Z"
    expected = dict(typescript, cache={"hit": True, "timestamp": stamp})
    headers = dict(case["headers"], **{"content-length": str(len(actual[2]))})
    ok = check(actual[0] == 200 and native == expected and typescript["cache"] == {"hit": False, "timestamp": None}
               and len(case["upstream"]) > 0 and not observed,
               f"{case['name']}: native {actual[2][:300]!r} (requests {observed})")
    for name in edges.HEADERS:
        if name != "x-rate-limit-remaining":
            ok = check(actual[1].get(name) == headers.get(name),
                       f"{case['name']}: header {name} {actual[1].get(name)!r} != {headers.get(name)!r}") and ok
    return ok


def l1_protobuf(case, actual, observed):
    """TypeScript answered the live object its L1 still held (Infinity and
    -0 at the named paths); native the stored JSON (null and 0). Everything
    else in the decoded StructuredReadResponse, and every header but
    Content-Length, is equal; no provider request."""
    expected = bytes.fromhex(case["wireHex"])
    try:
        ts = [v for n, _, v in r26.fields(expected)]
        native = [v for n, _, v in r26.fields(actual[2])]
        ts_tree, native_tree = r26.decode_struct(ts[0]), r26.decode_struct(native[0])
    except (ValueError, IndexError, UnicodeDecodeError) as error:
        return check(False, f"{case['name']}: undecodable protobuf ({error!r})")
    import math
    for path, kind in L1_PROTOBUF[case["name"]]:
        def live_to_stored(value, kind=kind):
            if kind == "inf" and value[0] == "number" and math.isinf(value[1]) and value[1] > 0:
                return ("null",)
            if kind == "-0" and value[0] == "number" and value[1] == 0 and math.copysign(1, value[1]) < 0:
                return ("number", 0.0)
            return None
        ts_tree = r26.replace_path(ts_tree, path, live_to_stored)
        if ts_tree is None:
            return check(False, f"{case['name']}: TypeScript has no live {kind} at {path}")
    ok = check(len(ts) == 1 and len(native) == 1 and ts_tree == native_tree and
               len(expected) - 11 <= len(actual[2]) <= len(expected) - 7,
               f"{case['name']}: native protobuf differs beyond the L1 numbers ({len(actual[2])} vs {len(expected)})")
    headers = dict(case["headers"], **{"content-length": str(len(actual[2]))})
    for name in edges.HEADERS:
        if name != "x-rate-limit-remaining":
            ok = check(actual[1].get(name) == headers.get(name),
                       f"{case['name']}: header {name} {actual[1].get(name)!r} != {headers.get(name)!r}") and ok
    return check(actual[0] == 200 and not observed, f"{case['name']}: {actual[0]} {observed}") and ok


ASSERTED = {
    "hash-malformed-escape": edges.bad_url, "hash-invalid-utf8-escape": edges.bad_url,
    "fragment": insight.fragment_refusal, "raw-latin1-hash": insight.invalid_utf8_path,
    **{name: nul_hash for name in NUL_HASH}, **{name: l1_protobuf for name in L1_PROTOBUF},
    OVER_BOUND: over_bound,
}


# ---- durable state ----------------------------------------------------------------

def restart_envelopes(h, case, envelopes):
    """restart-state: the envelope the first process wrote (restart-final-
    before), not TypeScript's live re-read after the restart."""
    before = next(c for c in h.fixture["cases"] if c["name"] == "restart-final-before")
    after = next(c for c in h.fixture["cases"] if c["name"] == "restart-final-after")
    out, shifted = [], False
    for e in envelopes:
        if e["envelope"] and e["envelope"]["asOf"] == after["at"]:
            shift = after["at"] - before["at"]
            e = dict(e, envelope=dict(e["envelope"], asOf=before["at"], freshUntil=e["envelope"]["freshUntil"] - shift,
                                      staleUntil=e["envelope"]["staleUntil"] - shift))
            shifted = True
        out.append(e)
    return out, shifted


def replay_state(h, server, case):
    name = case["name"]
    devices = case["state"]["devices"]
    body = json.dumps({"devices": [h.fixture["deviceIds"][letter] for letter in devices],
                       "txs": case["state"]["txs"]}, ensure_ascii=False).encode("utf-8")
    harness.set_clock(h.clock, case["at"])
    status, _, payload = server.request("POST", "/probe/test-tx-state", body=body,
                                        headers={"Content-Type": "application/json"})
    if not check(status == 200, f"{name}: state probe {status} {payload[:200]!r}"):
        return "failed"
    got = json.loads(payload)
    actual = {"watches": {letter: got["watches"][h.fixture["deviceIds"][letter]] for letter in devices},
              "records": got["records"], "pending": got["pending"]}
    expected = {"watches": case["watches"], "records": case["records"], "pending": case["pending"]}
    trimmed, dropped = r26.without_nul(expected)
    outcome = "matched"
    if dropped:
        expected = trimmed
        outcome = "asserted"
        print(f"  {name}: {dropped} NUL-path topic(s) TypeScript remembered, native refuses", flush=True)
    if name == "parser-state":
        # The `fragment` read TypeScript served (native refuses a raw '#').
        topic = "tx:eth-mainnet:" + h.fixture["holders"]["FINAL"]
        kept = {letter: [w for w in rows if w["topic"] != topic] for letter, rows in expected["watches"].items()}
        if kept != expected["watches"]:
            expected = dict(expected, watches=kept)
            outcome = "asserted"
    ok = True
    for part in ("watches", "records", "pending"):
        if expected[part] != actual[part]:
            ok = check(False, f"{name}: {part}\n  expected {json.dumps(expected[part])[:1500]}\n"
                              f"  actual   {json.dumps(actual[part])[:1500]}") and ok
    keys = [e["key"] for e in case["envelopes"]]
    status, _, payload = server.request("POST", "/probe/test-address-swr", body=json.dumps({"keys": keys}).encode(),
                                        headers={"Content-Type": "application/json"})
    if not check(status == 200, f"{name}: swr probe {status} {payload[:200]!r}"):
        return "failed"
    want = case["envelopes"]
    if name == "evm-state":
        # The over-bound read failed natively: no envelope (TypeScript stored one).
        over = [e for e in want if e["key"].endswith(":" + OVER_BOUND_HASH) and e["envelope"]]
        if over:
            want = [dict(e, envelope=None) if e in over else e for e in want]
            outcome = "asserted"
    if name == "restart-state":
        want, shifted = restart_envelopes(h, case, want)
        if shifted:
            outcome = "asserted"
    for expected_envelope, have in zip(want, json.loads(payload)["envelopes"]):
        ok = check(have == expected_envelope, f"{name}: {expected_envelope['key'][:80]}\n"
                                              f"  expected {json.dumps(expected_envelope)[:900]}\n"
                                              f"  actual   {json.dumps(have)[:900]}") and ok
    return outcome if ok else "failed"


def replay_case(h, server, case, lanes):
    name = case["name"]
    if "state" in case:
        return replay_state(h, server, case)
    lanes.reserve(case)
    harness.set_clock(h.clock, case["at"])
    h.table.start_case(case)
    target = bytes.fromhex(case["targetHex"]).decode("latin1")
    if "concurrent" in case:
        return r26.replay_concurrent(h, server, case, lanes, target)
    before = len(h.upstream.calls())
    try:
        actual = catalog.raw(server.port, case["method"], target, edges.headers_of(case))
    except OSError as error:
        check(False, f"{name}: {error!r} (TypeScript {case['status']})")
        edges.settle(server, h.upstream, before, len(case["upstream"]))
        return "failed"
    h.no_secret(actual[2], name)
    observed = edges.settle(server, h.upstream, before, len(case["upstream"]))
    edges.repay(lanes, case, observed)
    if compat_expectations.malformed_path_target(case):
        # opaque-params: an empty, control-character or over-long
        # path value is the router's 404 before any handler.
        ok = check(compat_expectations.router_refused(actual[0], actual[1], actual[2], observed),
                   f"{name}: malformed path value is not the router's 404")
        return "asserted" if ok else "failed"
    if name in ROUTER_404:
        check(case["status"] == 404 and actual[0] == 404 and not observed, f"{name}: status {actual[0]} != 404")
        return "asserted"
    if name in RESTART_CACHED:
        return "asserted" if restart_cached(h, case, actual, observed) else "failed"
    rule = ASSERTED.get(name)
    if rule:
        return "asserted" if rule(case, actual, observed) else "failed"
    if case["status"] in (414, 431) or name.startswith("raw-utf8"):
        ok = edges.compare(name, case, actual, baseline=True) and \
            edges.compare_upstream(name, case, observed, h.secrets)
        return "asserted" if ok else "failed"
    if case["status"] == 429 and case["headers"].get("retry-after") and actual[0] == 429 and \
            b"temporarily busy" in actual[2]:
        native_after = int(actual[1].get("retry-after", "0"))
        check(1 <= native_after <= 30, f"{name}: retry-after {native_after} outside [1, 30]")
        record = dict(case, headers=dict(case["headers"], **{"retry-after": str(native_after)}))
        ok = edges.compare(name, record, actual) and edges.compare_upstream(name, case, observed, h.secrets)
        return "asserted" if ok else "failed"
    ok = edges.compare(name, case, actual, remaining=case["group"] == "quota")
    ok = edges.compare_upstream(name, case, observed, h.secrets) and ok
    return "matched" if ok else "failed"


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = load_fixture()
    only = set(filter(None, os.environ.get("R27_ONLY_GROUPS", "").split(",")))
    groups = {g["name"]: g for g in fixture["groups"]}
    by_group = collections.OrderedDict()
    for case in fixture["cases"]:
        by_group.setdefault(case["group"], []).append(case)
    totals = collections.Counter()
    replayed = 0
    with tempfile.TemporaryDirectory(prefix="transaction-view-edges-") as root:
        root = Path(root)
        h = edges.Harness(binary, root, fixture)
        breaker_opened = None
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
                        for _ in range(240):
                            status, _, _ = catalog.raw(server.port, "GET", "/v1/networks/" + "n" * 65 + "/tx/" +
                                                       fixture["holders"]["FINAL"] + "/view",
                                                       {"authorization": "Bearer " + catalog.token("d")})
                            if not check(status == 400, f"quota drain answered {status}"):
                                break
                    for case in cases:
                        if case["name"] == BREAKER_RECOVERY and breaker_opened is not None:
                            wait = breaker_opened + 31 - time.monotonic()
                            if wait > 0:
                                time.sleep(wait)
                        outcome = replay_case(h, server, case, lanes)
                        if case["name"] == BREAKER_LAST_FAILURE:
                            breaker_opened = time.monotonic()
                        if os.environ.get("R27_TRACE"):
                            print(f"    {case['name']}: {outcome}", flush=True)
                        results[outcome] += 1
                        replayed += 1
                        if os.environ.get("R27_FAIL_FAST") and edges.FAILURES:
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
        finally:
            h.upstream.close()
        if h.table.extra:
            check(False, f"requests TypeScript never made: {sorted(set(h.table.extra))[:10]}")
        if len(sys.argv) == 3 and not only:
            production(sys.argv[2], root, fixture)
    print(f"transaction view: {replayed} of {len(fixture['cases'])} cases replayed: {totals['matched']} matched, "
          f"{totals['asserted']} asserted differences, {totals['failed']} failed")
    if not only:
        check(replayed == len(fixture["cases"]), f"replayed {replayed} of {len(fixture['cases'])} cases")
    if edges.FAILURES:
        print(f"{len(edges.FAILURES)} failure(s)")
        raise SystemExit(1)
    if os.environ.get("R27_DRAFT_FIXTURE"):
        print("transaction view edges: draft replay passed (not a verdict)")
        raise SystemExit(2)
    print("transaction view edges: all checks passed")


def production(binary, root, fixture):
    """The production composition: the route exists only behind the canary,
    and with it on refuses a missing bearer and any Origin before the read."""
    (root / "prod-tls").mkdir()
    ca, _, _, _ = tls_ca.create_certificates(root / "prod-tls", "DNS:localhost")
    path = "/v1/networks/eth-mainnet/tx/" + fixture["holders"]["FINAL"] + "/view"
    for value in (None, "0"):
        env = {"FORTIS_CA_BUNDLE_PATH": str(ca)}
        if value is not None:
            env["FORTIS_NATIVE_ADDRESS_CANARY"] = value
        server = catalog.Server(binary, root / f"kv-prod-off-{value}", env)
        try:
            status, _, _ = catalog.raw(server.port, "GET", path, {"authorization": "Bearer " + catalog.token("a")})
            check(status == 404, f"production canary {value!r}: {status}, not 404")
        finally:
            server.stop()
    server = catalog.Server(binary, root / "kv-prod-on", {"FORTIS_NATIVE_ADDRESS_CANARY": "1",
                                                           "FORTIS_CA_BUNDLE_PATH": str(ca)})
    try:
        status, headers, body = catalog.raw(server.port, "GET", path, {})
        check(status == 401 and body == b'{"error":"UNAUTHORIZED"}' and "vary" not in headers,
              f"production no-auth: {status} {body!r}")
        status, _, body = catalog.raw(server.port, "GET", path, {"origin": "https://evil.example"})
        check(status == 403 and body == b'{"error":"BROWSER_ORIGIN_FORBIDDEN"}', f"production origin: {status}")
        status, _, body = catalog.raw(server.port, "GET", "/v1/networks/tron/tx/x/view", {})
        check(status == 401, f"production unsupported family without a bearer: {status} {body!r}")
        status, _, body = catalog.raw(server.port, "GET", "/v1/networks/eth-mainnet/tx//view", {})
        # opaque-params: the router's 404 before any hook.
        check(status == 404, f"production empty hash without a bearer: {status} {body!r}")
    finally:
        server.stop()
    print("production: canary unset/0 answers 404; on, no bearer 401 and Origin 403")


if __name__ == "__main__":
    main()
