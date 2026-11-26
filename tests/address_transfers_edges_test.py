#!/usr/bin/env python3
"""Real-socket edges of GET /v1/networks/{network}/address/{address}/transfers
(R17) against tests/corpus/address_transfers_edges.json, which
the earlier TypeScript implementation's tests recorded
from the real Fastify app over real sockets.

The harness is R15's (tests/address_nfts_edges_test.py): the test binary
(build/fortis_node_address_route_test) sends every provider host to one
loopback HTTPS upstream that answers, per case, the exact bytes TypeScript
was served in the order TypeScript asked for them (a host the case marked
down answers 503, `mode.delayMs` delays every reply). Each case's frozen
instant is written to the shared clock file first and the native provider
lanes are paced by monotonic time. Each fixture group is a fresh process with
its own KV directory; restart-b reuses restart-a's after a SIGKILL.

Checks, per case: the status, every recorded header (x-rate-limit-remaining
only in the quota group), the exact JSON text or protobuf bytes, and the
multiset of provider requests (collected after /probe/test-address-idle, so
an ingest a delta poll stopped waiting for is counted too). Concurrent cases
compare every response and the combined provider requests. `state` records
compare, through POST /probe/test-address-state, each device's watches
(topic, account link, tracked tokens, expiry) and, through POST
/probe/test-transfer-state, per account the read times, the transfer
sync-state record and every transfer row, page index (cursor, time,
token-time, unpriced, age, retention) and sync-journal entry keyed by the
account, plus the journal head, exactly; then every journal expiry
TypeScript recorded is probed one millisecond before and at the instant.
Each provider key may only reach its own provider; no device token, id,
client header or provider key may reach a response or the process output.

Native-only bounded rules (not frozen by the oracle):
  - `bounded-workers`: eight distinct cold accounts at once against a slow
    upstream all answer 200 with their own row, one ingest (two Etherscan
    requests) each, never more than two provider requests in flight;
  - the production composition: the route answers 404 unless
    FORTIS_NATIVE_ADDRESS_CANARY=1, then 401/403 before any facet work.

Asserted differences (each with its own rule below and documented beside
handle_transfers): see ASSERTED and the rule docstrings.

Usage: address_transfers_edges_test.py build/fortis_node_address_route_test [build/fortis_node]
"""
import collections
import hashlib
import json
import os
import signal
import sys
import tempfile
import threading
import time
from pathlib import Path
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compat_expectations  # noqa: E402
import address_balances_edges_test as edges  # noqa: E402
import address_insight_edges_test as insight  # noqa: E402
import address_routes_test as routes  # noqa: E402
import discovery_route_harness as harness  # noqa: E402
import robinhood_stock_catalog_edges_test as catalog  # noqa: E402
import tls_ca  # noqa: E402

FIXTURE = "address_transfers_edges.json"
check = edges.check
ROUTER_404 = {"router-post", "router-trailing-slash", "router-upper-path"}
COST = 10


def wire_url(url):
    """The request fetch() puts on the wire for a URL string: WHATWG parsing
    drops a fragment and percent-encodes the special-query set (C0 controls,
    space, '"', "'", '<', '>', DEL and non-ASCII, as UTF-8 in uppercase hex)
    in the query. The oracle records the string TypeScript passed to fetch;
    the identity for every URL without such a byte."""
    url = url.split("#", 1)[0]
    if "?" not in url:
        return url
    base, query = url.split("?", 1)
    out = []
    for byte in query.encode("utf-8", "surrogatepass"):
        if byte <= 0x20 or byte in b"\"'<>" or byte >= 0x7F:
            out.append(f"%{byte:02X}")
        else:
            out.append(chr(byte))
    return f"{base}?{''.join(out)}"


def load_fixture():
    data = (harness.FIXTURES / FIXTURE).read_bytes()
    pinned = (harness.FIXTURES / f"{FIXTURE}.sha256").read_text().split()[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{FIXTURE}: sha256 {digest} does not match the pinned {pinned}")
    fixture = json.loads(data)
    for case in fixture["cases"]:
        for call in case.get("upstream", []):
            call["url"] = wire_url(call["url"])
    return fixture


# ---- asserted differences ---------------------------------------------------------------------------


def nul_address(case, actual, observed):
    """A decoded NUL in the address: TypeScript touches an account for it,
    remembers its topics and asks Etherscan (refused: invalid address) and
    Alchemy, answering the empty stored page (200); native answers 400
    {"error":"BAD_REQUEST"} with no account, topic or provider request."""
    document = json.loads(case["responseText"])
    return check(case["status"] == 200 and document["items"] == [] and "\u0000" in document["address"] and
                 actual[0] == 400 and actual[2] == b'{"error":"BAD_REQUEST"}' and not observed,
                 f"{case['name']}: native {actual[0]} {actual[2][:120]!r}")


def nul_token(case, actual, observed):
    """A decoded NUL in `token`: TypeScript reads the token index of that id
    (no stored token id holds a NUL: an empty page) and tracks the token in
    the portfolio watch; native answers 400 {"error":"BAD_REQUEST"} without
    the facet. Neither asks a provider."""
    document = json.loads(case["responseText"])
    return check(case["status"] == 200 and document["items"] == [] and not case["upstream"] and
                 actual[0] == 400 and actual[2] == b'{"error":"BAD_REQUEST"}' and not observed,
                 f"{case['name']}: native {actual[0]} {actual[2][:120]!r}")


def fragment_query(case, actual, observed):
    """A raw '#' inside the query: TypeScript keeps it in the value
    (limit "2#frag" fails zod: 400, charged 10); the native request parser
    refuses the target before routing (400, the same body, uncharged)."""
    return check(case["status"] == 400 and edges.expected_body(case) == b'{"error":"BAD_REQUEST"}' and
                 actual[0] == 400 and actual[2] == b'{"error":"BAD_REQUEST"}' and not observed,
                 f"{case['name']}: native {actual[0]} {actual[2][:120]!r}")


ASSERTED = {
    "address-malformed-escape": edges.bad_url,
    "fragment-in-query": fragment_query,
    "address-escaped-nul": nul_address,
    "token-nul": nul_token,
    "fragment": insight.fragment_refusal,
    "raw-latin1-address": insight.invalid_utf8_path,
}
# TypeScript's String(err) for a failed ingest is compared exactly, except for
# these error classes, where native stores its own transport description
# (e.g. "Error: upstream 503" for "UpstreamError: alchemy 503"): there the
# record must hold a native Error with a nonempty description, at the same
# lastErrorAt, without a provider key.
TRANSPORT_ERROR_CLASSES = ("UpstreamError: ", "UpstreamAnswerError: ", "TypeError: ", "LaneBusyError: ")
CONSTRAINED = collections.Counter()


def compare_case(h, case, actual, observed):
    name = case["name"]
    if compat_expectations.malformed_path_target(case):
        # opaque-params: an empty, control-character or over-long
        # path value is the router's 404 before any handler.
        ok = check(compat_expectations.router_refused(actual[0], actual[1], actual[2], observed),
                   f"{name}: malformed path value is not the router's 404")
        return "asserted" if ok else "failed"
    if name in ROUTER_404:
        return "asserted" if check(case["status"] == 404 and actual[0] == 404 and not observed,
                                   f"{name}: status {actual[0]} != 404") else "failed"
    rule = ASSERTED.get(name)
    if rule:
        return "asserted" if rule(case, actual, observed) else "failed"
    if case["status"] in (414, 431) or name.startswith("raw-utf8"):
        ok = edges.compare(name, case, actual, baseline=True) and \
            edges.compare_upstream(name, case, observed, h.secrets)
        return "asserted" if ok else "failed"
    ok = edges.compare(name, case, actual, remaining=case["group"] == "quota")
    ok = edges.compare_upstream(name, case, observed, h.secrets) and ok
    return "matched" if ok else "failed"


# ---- durable state ----------------------------------------------------------------------------------


def transfer_probe(server, case, now=None):
    body = {"accounts": case["state"]["accounts"], "entries": case["state"]["entries"]}
    if now is not None:
        body["now"] = now
    status, _, payload = server.request("POST", "/probe/test-transfer-state", body=json.dumps(body).encode(),
                                        headers={"Content-Type": "application/json"})
    if not check(status == 200, f"{case['name']}: transfer probe {status} {payload[:200]!r}"):
        return None
    return json.loads(payload)


def compare_watches(h, server, case):
    """Every watch exactly, except the topics of a path value holding a NUL
    or (opaque-params) any control character or DEL: only TypeScript
    writes them, the native router refuses such a path."""
    ok = True
    for letter in case["state"]["devices"]:
        body = json.dumps({"device": h.fixture["deviceIds"][letter], "accounts": []}).encode()
        status, _, payload = server.request("POST", "/probe/test-address-state", body=body,
                                            headers={"Content-Type": "application/json"})
        if not check(status == 200, f"{case['name']}: state probe {status} {payload[:200]!r}"):
            return False
        got = json.loads(payload)["watches"]
        want = [w for w in case["watches"][letter]
                if not any(ord(c) < 0x20 or c == "\x7f" for c in w["topic"])]
        if len(want) != len(case["watches"][letter]):
            CONSTRAINED["nul-address-topics"] += 1
        ok = check(got == want, f"{case['name']}: watches of {letter}\n  expected {want}\n  actual   {got}") and ok
    return ok


def sync_matches(label, want, have):
    if want == have or not isinstance(want, dict) or not isinstance(have, dict):
        return check(want == have, f"{label}: sync\n  expected {json.dumps(want)[:1500]}\n"
                                   f"  actual   {json.dumps(have)[:1500]}")
    ts_error, native_error = want.get("lastError"), have.get("lastError")
    if isinstance(ts_error, str) and ts_error.startswith(TRANSPORT_ERROR_CLASSES) and isinstance(native_error, str) \
            and native_error.startswith("Error: ") and len(native_error) > len("Error: "):
        CONSTRAINED["lastError-text"] += 1
        have = dict(have, lastError=ts_error)
    return check(want == have, f"{label}: sync\n  expected {json.dumps(want)[:1500]}\n  actual   {json.dumps(have)[:1500]}")


def compare_transfer(h, case, got):
    name = case["name"]
    ok = check(len(case["transfer"]) == len(got["transfer"]), f"{name}: account count")
    for want, have in zip(case["transfer"], got["transfer"]):
        label = f"{name}: {want['network']}/{want['address'][:24]}"
        if any(ord(c) < 0x20 or c == "\x7f" for c in want["network"] + want["address"]):
            # opaque-params: the router refused that path, so no
            # account, sync record or index entry exists for it.
            want = {k: (None if k in ("sync", "account") else [] if k == "entries" else v)
                    for k, v in want.items()}
        ok = sync_matches(label, want.get("sync"), have.get("sync")) and ok
        for key in ("account", "counts"):
            if key in want or key in have:
                ok = check(have.get(key) == want.get(key),
                           f"{label}: {key}\n  expected {json.dumps(want.get(key))[:1500]}\n"
                           f"  actual   {json.dumps(have.get(key))[:1500]}") and ok
        if "entries" in want:
            want_entries = [{"key": e["key"], "value": e["value"]} for e in want["entries"]]
            have_entries = have.get("entries") or []
            if have_entries != want_entries:
                index = next((i for i, (x, y) in enumerate(zip(want_entries, have_entries)) if x != y),
                             min(len(want_entries), len(have_entries)))
                ok = check(False, f"{label}: entries ({len(want_entries)} expected, {len(have_entries)} stored), "
                                  f"first difference at {index}\n"
                                  f"  expected {json.dumps(want_entries[index:index + 1])[:1500]}\n"
                                  f"  actual   {json.dumps(have_entries[index:index + 1])[:1500]}")
        sync = have.get("sync") or {}
        for secret in h.provider_keys:
            ok = check(secret not in json.dumps(sync), f"{label}: the sync record holds a provider key") and ok
    ok = check(got.get("head") == case["head"], f"{name}: journal head {got.get('head')} != {case['head']}") and ok
    return ok


def visible(case, at):
    return [sorted(e["key"] for e in row.get("entries", []) if e["expiresAt"] is None or e["expiresAt"] > at)
            for row in case["transfer"]]


def replay_state(h, server, case):
    ok = compare_watches(h, server, case)
    got = transfer_probe(server, case)
    if got is None or not compare_transfer(h, case, got):
        return False
    instants = sorted({e["expiresAt"] for row in case["transfer"] for e in row.get("entries", [])
                       if e["expiresAt"] is not None})
    for instant in instants:
        for at in (instant - 1, instant):
            probed = transfer_probe(server, case, at)
            if probed is None:
                return False
            have = [sorted(e["key"] for e in row.get("entries", [])) for row in probed["transfer"]]
            ok = check(have == visible(case, at), f"{case['name']}: entries visible at {at} (expiry {instant})") and ok
    return ok


# ---- requests -----------------------------------------------------------------------------------------


class QuotaPacer:
    """The native device quota counts wall-clock minutes (TypeScript's the
    frozen case Date): before each charged request, wait for the next minute
    when the device's budget (premium 1,200, development 240; 10 per read)
    would run out."""
    LIMITS = {"A": 1200, "B": 1200}

    def __init__(self):
        self.used = collections.Counter()

    def charge(self, case, count=1):
        device = case.get("device")
        if not device or case["group"] == "quota":
            return
        limit = self.LIMITS.get(device, 240)
        minute = int(time.time() // 60)
        if self.used[(device, minute)] + COST * count > limit:
            time.sleep(60 - time.time() % 60 + 0.2)
            minute = int(time.time() // 60)
        self.used[(device, minute)] += COST * count


PACER = QuotaPacer()


def provider_keys_contained(h, name, observed, payloads):
    etherscan, alchemy = h.provider_keys
    ok = True
    for method, host, target, body in observed:
        text = f"{target} {body}"
        if etherscan in text:
            ok = check(host == "api.etherscan.io", f"{name}: the Etherscan key went to {host}") and ok
        if alchemy in text:
            ok = check(host.endswith(".g.alchemy.com"), f"{name}: the Alchemy key went to {host}") and ok
    for payload in payloads:
        for key in h.provider_keys + h.family_keys:
            ok = check(key.encode() not in payload, f"{name}: a provider key reached a response") and ok
    return ok


def replay_case(h, server, case, lanes):
    name = case["name"]
    if "state" in case:
        return "matched" if replay_state(h, server, case) else "failed"
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
            try:
                results[index] = catalog.raw(server.port, "HEAD" if entry["kind"] == "head" else "GET",
                                             entry.get("target") or target, edges.headers_of(case, entry["kind"]))
            except (OSError, IndexError, ValueError) as error:
                results[index] = (0, {}, repr(error).encode())

        PACER.charge(case, len(kinds))
        threads = [threading.Thread(target=one, args=(i, entry)) for i, entry in enumerate(kinds)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        observed = edges.settle(server, h.upstream, before, len(case["upstream"]))
        edges.repay(lanes, case, observed)
        ok = provider_keys_contained(h, name, observed, [r[2] for r in results])
        for index, (record, actual) in enumerate(zip(case["responses"], results)):
            h.no_secret(actual[2], name)
            ok = edges.compare(f"{name}[{index}:{kinds[index]['kind']}]", record, actual) and ok
        ok = edges.compare_upstream(name, case, observed, h.secrets) and ok
        return "matched" if ok else "failed"
    PACER.charge(case)
    try:
        actual = catalog.raw(server.port, case["method"], target, edges.headers_of(case))
    except OSError as error:
        check(False, f"{name}: {error!r} (TypeScript {case['status']})")
        edges.settle(server, h.upstream, before, len(case["upstream"]))
        return "failed"
    h.no_secret(actual[2], name)
    observed = edges.settle(server, h.upstream, before, len(case["upstream"]))
    edges.repay(lanes, case, observed)
    provider_keys_contained(h, name, observed, [actual[2]])
    return compare_case(h, case, actual, observed)


# ---- native-only bounded rules ------------------------------------------------------------------------


def bounded_workers(h, server, fixture):
    """Eight cold accounts at once, the upstream answering after 400 ms: each
    read answers its own row after one Etherscan pair, never more than two
    provider requests in flight (the execution adapter's HTTPS workers)."""
    holders = [f"0x{index:040x}" for index in range(0x7100, 0x7108)]

    def answer(method, host, target, body):
        query = dict(p.split("=", 1) for p in urlsplit(target).query.split("&") if "=" in p)
        holder = query.get("address", "")
        if host != "api.etherscan.io" or query.get("action") != "txlist":
            return 200, b'{"status":"0","message":"No transactions found","result":[]}', 400, True
        row = {"blockNumber": "", "timeStamp": "1790000000", "hash": "0x" + holder[-8:].rjust(64, "0"),
               "from": "0x" + "99" * 20, "to": holder, "value": "1", "isError": "0", "txreceipt_status": "1",
               "input": "0x"}
        return 200, json.dumps({"status": "1", "message": "OK", "result": [row]}).encode(), 400, True

    harness.set_clock(h.clock, fixture["now"] + 20 * 86400000)
    PACER.charge({"device": "A", "group": "bounded"}, len(holders))
    time.sleep(4)  # the etherscan lane's burst refilled
    h.table.override = answer
    h.table.spans = []
    results = [None] * len(holders)
    barrier = threading.Barrier(len(holders))

    def one(index):
        barrier.wait()
        results[index] = catalog.raw(server.port, "GET", f"/v1/networks/eth-mainnet/address/{holders[index]}/transfers",
                                     {"authorization": "Bearer " + catalog.token("A")}, timeout=120)

    threads = [threading.Thread(target=one, args=(i,)) for i in range(len(holders))]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    server.request("GET", "/probe/test-address-idle")
    h.table.override = None
    ok = True
    for index, result in enumerate(results):
        document = json.loads(result[2]) if result and result[0] == 200 else {}
        ok = check([i.get("hash") for i in document.get("items", [])] == ["0x" + holders[index][-8:].rjust(64, "0")],
                   f"bounded-workers[{index}]: {result[0] if result else None} {result[2][:160] if result else b''!r}") \
            and ok
    events = sorted([(start, 1) for start, _ in h.table.spans] + [(end, -1) for _, end in h.table.spans])
    peak = current = 0
    for _, delta in events:
        current += delta
        peak = max(peak, current)
    ok = check(len(h.table.spans) == 2 * len(holders) and peak <= 2,
               f"bounded-workers: {len(h.table.spans)} provider requests, peak in flight {peak} (bound 2)") and ok
    print(f"bounded-workers: {len(holders)} cold accounts, {len(h.table.spans)} provider requests, peak in flight "
          f"{peak}", flush=True)
    return ok


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = load_fixture()
    only = set(filter(None, os.environ.get("R17_ONLY_GROUPS", "").split(",")))
    groups = {g["name"]: g for g in fixture["groups"]}
    by_group = collections.OrderedDict()
    for case in fixture["cases"]:
        by_group.setdefault(case["group"], []).append(case)
    totals = collections.Counter()
    with tempfile.TemporaryDirectory(prefix="address-transfers-") as root:
        root = Path(root)
        h = edges.Harness(binary, root, fixture)
        h.provider_keys = list(fixture["providerKeys"])
        h.family_keys = sorted({v for g in fixture["groups"] for k, v in g["env"].items()
                                if k.endswith(("_KEY", "_TOKEN")) and v not in h.provider_keys})
        h.table = insight.TimedTable(fixture["cases"])
        h.upstream.table = h.table
        try:
            for name, cases in by_group.items():
                bounded_only = name == "main" and "bounded" in only
                if only and name not in only and not bounded_only and \
                        not (name.startswith("restart") and "restart" in only) and \
                        not (name.startswith("family") and "family" in only):
                    continue
                group = groups[name]
                server = h.server(group["kv"], group["env"], group["start"])
                lanes = routes.Lanes()
                results = collections.Counter()
                try:
                    if name == "quota":
                        catalog.wait_for_fresh_minute(25)
                        for _ in range(24):
                            status, _, _ = catalog.raw(server.port, "GET", "/v1/networks/nowhere/address/"
                                                       + fixture["holders"]["QUERY"] + "/transfers",
                                                       {"authorization": "Bearer " + catalog.token("d")})
                            if not check(status == 400, f"quota drain answered {status}"):
                                break
                    for case in ([] if bounded_only and name not in only else cases):
                        results[replay_case(h, server, case, lanes)] += 1
                        if os.environ.get("R17_FAIL_FAST") and edges.FAILURES:
                            raise SystemExit(1)
                    if name == "main" and (not only or bounded_only):
                        results["matched" if bounded_workers(h, server, fixture) else "failed"] += 1
                finally:
                    if name == "restart-a":
                        server.process.send_signal(signal.SIGKILL)
                        server.process.wait()
                    else:
                        server.stop()
                    output = server.process.stdout.read() + server.process.stderr.read()
                    h.no_secret(output, f"{name} output")
                    for key in h.provider_keys + h.family_keys:
                        check(key.encode() not in output, f"{name} output: a provider key was logged")
                totals.update(results)
                print(f"{name}: {results['matched']} matched, {results['asserted']} asserted differences, "
                      f"{results['failed']} failed", flush=True)
        finally:
            h.upstream.close()
        if h.table.extra:
            print(f"requests TypeScript never made: {sorted(set(h.table.extra))[:10]}")
        if len(sys.argv) == 3 and not only:
            production(sys.argv[2], root, fixture)
    print(f"address transfers: {totals['matched']} matched, {totals['asserted']} asserted differences, "
          f"{totals['failed']} failed; constrained comparisons {dict(CONSTRAINED)}")
    if edges.FAILURES:
        print(f"{len(edges.FAILURES)} failure(s)")
        raise SystemExit(1)
    print("address transfers edges: all checks passed")


def production(binary, root, fixture):
    """The production composition: the route exists only behind the canary."""
    (root / "prod-tls").mkdir()
    ca, _, _, _ = tls_ca.create_certificates(root / "prod-tls", "DNS:localhost")
    path = "/v1/networks/eth-mainnet/address/" + fixture["holders"]["QUERY"] + "/transfers"
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
        status, _, body = catalog.raw(server.port, "GET", "/v1/networks/eth-mainnet/address//transfers", {})
        # opaque-params: the router's 404 before any hook.
        check(status == 404, f"production empty address without a bearer: {status} {body!r}")
    finally:
        server.stop()
    print("production: canary unset/0 answers 404; on, no bearer 401 and Origin 403")


if __name__ == "__main__":
    main()
