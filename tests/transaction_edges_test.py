#!/usr/bin/env python3
"""Real-socket edges of GET /v1/networks/{network}/tx/{hash} (R26) against
tests/corpus/transaction_edges.json, which
the earlier TypeScript implementation's tests recorded from the
real Fastify app over real sockets.

The harness is R13's (tests/address_balances_edges_test.py): the test binary
(build/fortis_node_address_route_test) sends every provider host to one
loopback HTTPS upstream that answers, per case, the exact bytes TypeScript was
served in the order TypeScript asked for them (a host the case marked down
answers 503). Each case's frozen instant is written to the shared clock file
first and the native provider lanes are paced by monotonic time. Each fixture
group is a fresh process with its own KV directory; restart-b reuses
restart-a's after a SIGKILL. The quota group drains device d with 240
socket requests (TypeScript: app.inject) before its cases.

Checks, per case: the status, every recorded header (x-rate-limit-remaining
only in the quota group), the exact JSON text or protobuf bytes, and the
multiset of provider requests (exact URL and JSON-RPC body). `job` records
run TxDetailFacet.pollPendingOnce through GET /probe/test-address-job and
compare {transitions} and the provider requests. `state` records compare,
through POST /probe/test-tx-state, each device's watches (topic as typed,
account link, tracked tokens, expiry), each tx/detail record as stored with
its physical expiry, and every live tx/pending entry (key, value, expiry).
Concurrent cases compare every response and the combined provider requests.
No provider request may carry a client header or credential, and no device
token or id may reach a response or process output.

Asserted differences (documented beside handle_tx), each by case name:
  - router: other methods and spellings are 404 in both (status only, no
    provider request); the routing 414 and Node's "Client Error" 400 carry
    the native security headers (body and other headers exact);
  - a malformed path escape is the parser's {"error":"BAD_REQUEST"}
    (Fastify FST_ERR_BAD_URL); a raw '#' is refused by the request parser
    (TypeScript drops the fragment and serves the read), and so is a raw
    byte that leaves the decoded path invalid UTF-8 (Node: "Client Error");
  - a decoded NUL in {hash} on a servable network: 400 {"error":
    "BAD_REQUEST"} with no provider request, record, pending entry or topic
    (TypeScript reads the chain and stores them); a NUL in {network} answers
    TypeScript's exact "unknown network" 400 but writes no topic. The state
    records therefore differ by exactly the items that hold a NUL;
  - a cached protobuf read of a record holding Infinity (a tip past 2^1024,
    a provider's 1e400): TypeScript's msgpack record keeps the double, so its
    Struct carries number_value Infinity; the native JSON record stores null
    (null_value); every other Value and byte equal (JSON answers are equal);
  - the lane breaker's Retry-After counts the cooldown on the monotonic clock
    (TypeScript: the lane clock): 1..30.

Usage: transaction_edges_test.py build/fortis_node_address_route_test [build/fortis_node]
"""
import collections
import hashlib
import json
import re
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

FIXTURE = "transaction_edges.json"
PINNED = "d952641c6af196607b0a9436addbd7896c83ec5ef8b8790eb00c26a522f7ac6e"
check = edges.check
ROUTER_404 = {"router-post", "router-trailing-slash", "router-upper-path", "router-extra-segment",
              "router-view-upper", "hash-empty-with-slash"}
# The Struct paths where TypeScript's cached protobuf carries Infinity.
INFINITE_PROTOBUF = {
    "tip-over-2pow1024-again-protobuf": [("detail", "confirmations")],
    "transaction-large-integer-again-protobuf": [("raw", "transaction", "v")],
}
NUL_HASH = {"hash-escaped-nul"}
BREAKER_RECOVERY = "breaker-recovered"
BREAKER_LAST_FAILURE = "breaker-failure-5"


def load_fixture():
    data = (harness.FIXTURES / FIXTURE).read_bytes()
    pinned = (harness.FIXTURES / f"{FIXTURE}.sha256").read_text().split()[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned or digest != PINNED:
        raise SystemExit(f"{FIXTURE}: sha256 {digest} does not match the pinned {PINNED}")
    return json.loads(data)


# ---- protobuf Struct decoding (google.protobuf.Value), for the Infinity rule ----

def varint(data, at):
    shift = value = 0
    while True:
        byte = data[at]
        at += 1
        value |= (byte & 0x7f) << shift
        shift += 7
        if not byte & 0x80:
            return value, at


def fields(data):
    at = 0
    while at < len(data):
        key, at = varint(data, at)
        number, wire = key >> 3, key & 7
        if wire == 0:
            value, at = varint(data, at)
        elif wire == 1:
            value, at = data[at:at + 8], at + 8
        elif wire == 2:
            length, at = varint(data, at)
            value, at = data[at:at + length], at + length
        else:
            raise ValueError(f"wire type {wire}")
        yield number, wire, value


def decode_value(data):
    for number, _, value in fields(data):
        if number == 1:
            return ("null",)
        if number == 2:
            return ("number", struct.unpack("<d", value)[0])
        if number == 3:
            return ("string", value.decode("utf-8"))
        if number == 4:
            return ("bool", bool(value))
        if number == 5:
            return ("struct", decode_struct(value))
        if number == 6:
            return ("list", [decode_value(v) for n, _, v in fields(value) if n == 1])
    return ("absent",)


def decode_struct(data):
    out = []
    for number, _, entry in fields(data):
        if number != 1:
            continue
        key, value = "", b""
        for n, _, v in fields(entry):
            if n == 1:
                key = v.decode("utf-8")
            elif n == 2:
                value = v
        out.append((key, decode_value(value)))
    return out


def replace_path(tree, path, replacement):
    """The Struct `tree` with the Value at `path` replaced; None when the
    path does not exist."""
    head, rest = path[0], path[1:]
    out, found = [], False
    for key, value in tree:
        if key == head and not found:
            found = True
            if rest:
                if value[0] != "struct":
                    return None
                inner = replace_path(value[1], rest, replacement)
                if inner is None:
                    return None
                value = ("struct", inner)
            else:
                value = replacement(value)
                if value is None:
                    return None
        out.append((key, value))
    return out if found else None


def infinite_protobuf(case, actual, observed):
    """TypeScript answered its msgpack record (number_value +Infinity at the
    named paths); native the JSON record (null_value there). The decoded
    StructuredReadResponse is otherwise identical, the native bytes are 7
    shorter per Value, and every header but Content-Length matches."""
    expected = bytes.fromhex(case["wireHex"])
    try:
        ts = [(n, v) for n, _, v in fields(expected)]
        native = [(n, v) for n, _, v in fields(actual[2])]
        ts_tree, native_tree = decode_struct(ts[0][1]), decode_struct(native[0][1])
    except (ValueError, IndexError, UnicodeDecodeError, struct.error) as error:
        return check(False, f"{case['name']}: undecodable protobuf ({error!r})")
    paths = INFINITE_PROTOBUF[case["name"]]
    for path in paths:
        def infinity_to_null(value):
            return ("null",) if value[0] == "number" and math.isinf(value[1]) and value[1] > 0 else None
        ts_tree = replace_path(ts_tree, path, infinity_to_null)
        if ts_tree is None:
            return check(False, f"{case['name']}: TypeScript has no Infinity at {path}")
    differing = [(a, b) for a, b in zip(ts_tree, native_tree) if a != b][:2]
    # Each Value is 7 bytes shorter; an enclosing length prefix may shrink too.
    ok = check(len(ts) == 1 and len(native) == 1 and ts_tree == native_tree and
               len(expected) - 7 * len(paths) - 4 <= len(actual[2]) <= len(expected) - 7 * len(paths),
               f"{case['name']}: native protobuf differs beyond Infinity -> null at {paths}: {differing} "
               f"({len(actual[2])} vs {len(expected)} bytes)")
    headers = dict(case["headers"], **{"content-length": str(len(actual[2]))})
    for name in edges.HEADERS:
        if name == "x-rate-limit-remaining":
            continue
        ok = check(actual[1].get(name) == headers.get(name),
                   f"{case['name']}: header {name} {actual[1].get(name)!r} != {headers.get(name)!r}") and ok
    return check(actual[0] == 200, f"{case['name']}: status {actual[0]}") and \
        edges.compare_upstream(case["name"], case, observed, []) and ok


def nul_hash(case, actual, observed):
    """A decoded NUL in {hash} of a servable network: TypeScript reads the
    chain with it (200 unknown); native refuses it before the facet: 400
    {"error":"BAD_REQUEST"}, quota counted, no provider request."""
    return check(case["status"] == 200 and actual[0] == 400 and actual[2] == b'{"error":"BAD_REQUEST"}' and
                 actual[1].get("x-rate-limit-cost") == "1" and not observed,
                 f"{case['name']}: native {actual[0]} {actual[2][:120]!r}")


# lmdb-msgpack: a cached read answers the record as written. The
# oracle's records are its msgpack copies of the provider's `memo: "\ud800x"`
# and key `"k\udc00"` (three U+FFFD each in these short strings) and of a
# `__proto__` key (renamed `__proto_`); these are their inverses.
STORED_AS_WRITTEN_JSON = (("\ufffd\ufffd\ufffdx", "\\ud800x"), ("k\ufffd\ufffd\ufffd", "k\\udc00"),
                          ('"__proto_"', '"__proto__"'))
STORED_AS_WRITTEN_TEXT = (("\ufffd\ufffd\ufffdx", "\ud800x"), ("k\ufffd\ufffd\ufffd", "k\udc00"),
                          ("__proto_", "__proto__"))
# In protobuf a lone surrogate is written as one U+FFFD.
STORED_AS_WRITTEN_PROTOBUF = (("\ufffd\ufffd\ufffdx", "\ufffdx"), ("k\ufffd\ufffd\ufffd", "k\ufffd"),
                              ("__proto_", "__proto__"))


def rewrite_tree(tree, pairs):
    def text(value):
        for old, new in pairs:
            if value == old:
                return new
        return value

    def value_of(value):
        if value[0] == "string":
            return ("string", text(value[1]))
        if value[0] == "struct":
            return ("struct", rewrite_tree(value[1], pairs))
        if value[0] == "list":
            return ("list", [value_of(v) for v in value[1]])
        return value
    return [(text(key), value_of(value)) for key, value in tree]


def stored_as_written(case, actual, observed):
    """A cached read of a record holding a lone surrogate or a `__proto__`
    key answers it as written (lmdb-msgpack); everything else, and
    every header but Content-Length, is the oracle's."""
    if "wireHex" in case:
        try:
            ts = decode_struct([v for n, _, v in fields(bytes.fromhex(case["wireHex"]))][0])
            native = decode_struct([v for n, _, v in fields(actual[2])][0])
        except (ValueError, IndexError, UnicodeDecodeError, struct.error) as error:
            return check(False, f"{case['name']}: undecodable protobuf ({error!r})")
        ok = check(rewrite_tree(ts, STORED_AS_WRITTEN_PROTOBUF) == native,
                   f"{case['name']}: protobuf differs beyond the stored-as-written strings")
    else:
        want = case["responseText"]
        for old, new in STORED_AS_WRITTEN_JSON:
            want = want.replace(old, new)
        ok = check(actual[2] == want.encode("utf-8"), f"{case['name']}: body {actual[2][:300]!r}")
    headers = dict(case["headers"], **{"content-length": str(len(actual[2]))})
    for name in edges.HEADERS:
        if name == "x-rate-limit-remaining":
            continue
        ok = check(actual[1].get(name) == headers.get(name),
                   f"{case['name']}: header {name} {actual[1].get(name)!r} != {headers.get(name)!r}") and ok
    return check(actual[0] == 200, f"{case['name']}: status {actual[0]}") and \
        edges.compare_upstream(case["name"], case, observed, []) and ok


ASSERTED = {
    **{name: stored_as_written for name in ("transaction-lone-surrogate-again", "transaction-lone-surrogate-again-protobuf",
                                            "transaction-proto-key-again", "transaction-proto-key-again-protobuf")},
    "hash-malformed-escape": edges.bad_url, "hash-invalid-utf8-escape": edges.bad_url,
    "fragment": insight.fragment_refusal, "raw-latin1-hash": insight.invalid_utf8_path,
    **{name: infinite_protobuf for name in INFINITE_PROTOBUF}, **{name: nul_hash for name in NUL_HASH},
}


# ---- durable state ----------------------------------------------------------------

def without_nul(expected):
    """The TypeScript state as native leaves it: no topic, record or pending
    entry for a path value holding a NUL (or, since opaque-params,
    any control character or DEL: the router refuses it) that TypeScript
    wrote. The probe still lists the asked-for transaction, with no record.
    Stored records hold what was written (lmdb-msgpack)."""
    has_nul = lambda text: any(ord(c) < 0x20 or c == "\x7f" for c in text) or \
        re.search(r"%(?:[01][0-9A-Fa-f]|7[Ff])", text) is not None  # noqa: E731
    watches = {letter: [w for w in rows if not has_nul(w["topic"])] for letter, rows in expected["watches"].items()}
    records = [dict(r, record=None, expiresAt=None) if has_nul(r["hash"]) else r for r in expected["records"]]
    written = json.loads(stored_text(json.dumps(records, ensure_ascii=False)))
    records = written
    pending = [p for p in expected["pending"] if not has_nul(p["key"])]
    dropped = sum(len(rows) for rows in expected["watches"].values()) - sum(len(rows) for rows in watches.values()) + \
        sum(1 for a, b in zip(expected["records"], records) if a != b) + len(expected["pending"]) - len(pending)
    return {"watches": watches, "records": records, "pending": pending}, dropped


def stored_text(text):
    for old, new in STORED_AS_WRITTEN_TEXT:
        text = text.replace(old, new) if old != "__proto_" else text.replace('"__proto_"', '"__proto__"')
    return text


def replay_state(h, server, case):
    devices = case["state"]["devices"]
    body = json.dumps({"devices": [h.fixture["deviceIds"][letter] for letter in devices],
                       "txs": case["state"]["txs"]}, ensure_ascii=False).encode("utf-8")
    harness.set_clock(h.clock, case["at"])
    status, _, payload = server.request("POST", "/probe/test-tx-state", body=body,
                                        headers={"Content-Type": "application/json"})
    if not check(status == 200, f"{case['name']}: state probe {status} {payload[:200]!r}"):
        return "failed"
    got = json.loads(payload)
    actual = {"watches": {letter: got["watches"][h.fixture["deviceIds"][letter]] for letter in devices},
              "records": got["records"], "pending": got["pending"]}
    expected = {"watches": case["watches"], "records": case["records"], "pending": case["pending"]}
    trimmed, dropped = without_nul(expected)
    outcome = "matched"
    if dropped:
        expected = trimmed
        outcome = "asserted"
        print(f"  {case['name']}: {dropped} NUL-path item(s) TypeScript stored, native refuses", flush=True)
    ok = True
    for part in ("watches", "records", "pending"):
        want, have = expected[part], actual[part]
        if want == have:
            continue
        if isinstance(want, dict):
            pairs = [(k, want.get(k), have.get(k)) for k in sorted(set(want) | set(have))]
        else:
            pairs = [(i, want[i] if i < len(want) else None, have[i] if i < len(have) else None)
                     for i in range(max(len(want), len(have)))]
        for index, a, b in pairs:
            if a != b:
                ok = check(False, f"{case['name']}: {part}[{index}]\n  expected {json.dumps(a)[:1500]}\n"
                                  f"  actual   {json.dumps(b)[:1500]}") and ok
    return outcome if ok else "failed"


def replay_job(h, server, case, lanes):
    lanes.reserve(case)
    harness.set_clock(h.clock, case["at"])
    h.table.start_case(case)
    before = len(h.upstream.calls())
    status, _, payload = server.request("GET", "/probe/test-address-job?job=pending")
    observed = edges.settle(server, h.upstream, before, len(case["upstream"]))
    edges.repay(lanes, case, observed)
    ok = check(status == 200 and json.loads(payload) == case["result"],
               f"{case['name']}: job {status} {payload[:200]!r} != {case['result']}")
    ok = edges.compare_upstream(case["name"], case, observed, h.secrets) and ok
    return "matched" if ok else "failed"


def replay_concurrent(h, server, case, lanes, target):
    name = case["name"]
    kinds = case["concurrent"]
    before = len(h.upstream.calls())
    results = [None] * len(kinds)
    barrier = threading.Barrier(len(kinds))

    def one(index, entry):
        barrier.wait()
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
    return "matched" if ok else "failed"


def replay_case(h, server, case, lanes):
    name = case["name"]
    if "state" in case:
        return replay_state(h, server, case)
    if "job" in case:
        return replay_job(h, server, case, lanes)
    lanes.reserve(case)
    harness.set_clock(h.clock, case["at"])
    h.table.start_case(case)
    target = bytes.fromhex(case["targetHex"]).decode("latin1")
    if "concurrent" in case:
        return replay_concurrent(h, server, case, lanes, target)
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
    only = set(filter(None, os.environ.get("R26_ONLY_GROUPS", "").split(",")))
    groups = {g["name"]: g for g in fixture["groups"]}
    by_group = collections.OrderedDict()
    for case in fixture["cases"]:
        by_group.setdefault(case["group"], []).append(case)
    totals = collections.Counter()
    replayed = 0
    with tempfile.TemporaryDirectory(prefix="transaction-edges-") as root:
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
                                                       fixture["holders"]["CONFIRMED"],
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
                        results[outcome] += 1
                        replayed += 1
                        if os.environ.get("R26_FAIL_FAST") and edges.FAILURES:
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
        if len(sys.argv) == 3:
            production(sys.argv[2], root, fixture)
    print(f"transaction status: {replayed} of {len(fixture['cases'])} cases replayed: {totals['matched']} matched, "
          f"{totals['asserted']} asserted differences, {totals['failed']} failed")
    if not only:
        check(replayed == len(fixture["cases"]), f"replayed {replayed} of {len(fixture['cases'])} cases")
    if edges.FAILURES:
        print(f"{len(edges.FAILURES)} failure(s)")
        raise SystemExit(1)
    print("transaction edges: all checks passed")


def production(binary, root, fixture):
    """The production composition: the route exists only behind the canary."""
    (root / "prod-tls").mkdir()
    ca, _, _, _ = tls_ca.create_certificates(root / "prod-tls", "DNS:localhost")
    path = "/v1/networks/eth-mainnet/tx/" + fixture["holders"]["CONFIRMED"]
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
        status, _, body = catalog.raw(server.port, "GET", "/v1/networks/eth-mainnet/tx/", {})
        # opaque-params: the router's 404 before any hook.
        check(status == 404, f"production empty hash without a bearer: {status} {body!r}")
    finally:
        server.stop()
    print("production: canary unset/0 answers 404; on, no bearer 401 and Origin 403")


if __name__ == "__main__":
    main()
