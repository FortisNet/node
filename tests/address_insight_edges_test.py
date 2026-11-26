#!/usr/bin/env python3
"""Real-socket edges of GET /v1/networks/{network}/address/{address}/insight
(R14) against tests/corpus/address_insight_edges.json, which
the earlier TypeScript implementation's tests recorded from
the real Fastify app over real sockets.

The harness is R13's (tests/address_balances_edges_test.py): the test binary
(build/fortis_node_address_route_test) sends every provider host to one
loopback HTTPS upstream that answers, per case, the exact bytes TypeScript was
served in the order TypeScript asked for them (a host the case marked down
answers 503). Each case's frozen instant is written to the shared clock file
first and the native provider lanes are paced by monotonic time. Each fixture
group is a fresh process with its own KV directory; restart-b reuses
restart-a's after a SIGKILL.

Checks, per case: the status, every recorded header (x-rate-limit-remaining
only in the quota group), the exact JSON text or protobuf bytes, and the
multiset of provider requests (background revalidations included, collected
after /probe/test-address-idle). Concurrent cases compare every response and
the combined provider requests. `state` records prove through POST
/probe/test-address-state that an insight read wrote no watch/topic for the
device and no account row or balance snapshot. No provider request may carry
a client header or credential, and no device token or id may reach a
response or process output.

Native-only bounded rules (timing-dependent, so not frozen by the oracle):
  - `bounded-workers`: ten distinct cold reads at once against a slow
    upstream all answer 200 with their own document (three foreground
    workers, the rest queued), and at most two provider requests are ever in
    flight: the provider execution adapter's two HTTPS workers bound them
    below the 3 workers x 2 parallel calls (TypeScript: the rpc lane's 8);
  - the production composition: the route answers 404 unless
    FORTIS_NATIVE_ADDRESS_CANARY=1, then 401/403 before any facet work.

Asserted differences (documented beside handle_insight):
  - router: other methods and spellings are 404 in both (status only); the
    routing 414 and Node's "Client Error" 400 carry the native security
    headers; a malformed path escape is the parser's {"error":"BAD_REQUEST"}
    (Fastify FST_ERR_BAD_URL); a raw '#' is refused by the request parser
    (TypeScript drops the fragment), and so is a raw byte that leaves the
    decoded path invalid UTF-8 (Node: "Client Error");
  - a decoded NUL in a path value: 400 without the RangeError detail;
  - restart: the SWR envelope lives in the durable KV natively, so a read
    5 s after a restart is a cache hit where TypeScript's new process reads
    live (the expired read after it matches exactly);
  - a protobuf answer whose txCount is past 2^1024 from TypeScript's
    in-process L1 object carries number_value +Infinity; native answers the
    JSON round trip of the cached entry (null_value), as TypeScript itself
    does once the entry is read back from its KV;
  - the lane breaker's Retry-After counts the cooldown on the monotonic clock
    (TypeScript: the frozen case Date): 1..30.

Usage: address_insight_edges_test.py build/fortis_node_address_route_test [build/fortis_node]
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

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compat_expectations  # noqa: E402
import address_balances_edges_test as edges  # noqa: E402
import address_routes_test as routes  # noqa: E402
import discovery_route_harness as harness  # noqa: E402
import robinhood_stock_catalog_edges_test as catalog  # noqa: E402
import tls_ca  # noqa: E402

FIXTURE = "address_insight_edges.json"
check = edges.check
ROUTER_404 = {"router-post", "router-trailing-slash", "router-upper-path"}
RESTART_DIFFERENCE = {"restart-after-fresh"}
INFINITE_PROTOBUF = {"nonce-over-2pow1024-protobuf"}
BREAKER_RECOVERY = "breaker-recovered"
BREAKER_CASES = ("breaker-failure-1", "breaker-failure-2", "breaker-failure-3")


def load_fixture():
    data = (harness.FIXTURES / FIXTURE).read_bytes()
    pinned = (harness.FIXTURES / f"{FIXTURE}.sha256").read_text().split()[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{FIXTURE}: sha256 {digest} does not match the pinned {pinned}")
    return json.loads(data)


def fragment_refusal(case, actual, observed):
    """A raw '#' in the target: Node drops the fragment and serves the read;
    the native request parser refuses the target (400) before routing."""
    return check(case["status"] == 200 and actual[0] == 400 and actual[2] == b'{"error":"BAD_REQUEST"}' and
                 not observed, f"{case['name']}: native {actual[0]} {actual[2][:120]!r}")


def infinite_protobuf(case, actual, observed):
    """TypeScript answered its L1 object (number_value +Infinity); native the
    cached JSON (txCount null): the same bytes with that one Value changed."""
    expected = bytes.fromhex(case["wireHex"])
    infinity = bytes.fromhex("0a140a077478436f756e74120911000000000000f07f")
    null = bytes.fromhex("0a0d0a077478436f756e7412020800")
    rewritten = expected.replace(infinity, null)
    # The outer Struct's length prefix shrinks by the same 7 bytes.
    ok = infinity in expected and len(rewritten) == len(expected) - 7 and expected[0] == 0x0a
    if ok and expected[1] & 0x80:
        length = (expected[1] & 0x7f) | (expected[2] << 7)
        shorter = length - 7
        prefix = bytes([0x0a, (shorter & 0x7f) | 0x80, shorter >> 7]) if shorter >= 0x80 else bytes([0x0a, shorter])
        rewritten = prefix + rewritten[3:]
    record = dict(case, wireHex=rewritten.hex(), headers=dict(case["headers"], **{"content-length": str(len(rewritten))}))
    return edges.compare(case["name"], record, actual) and edges.compare_upstream(case["name"], case, observed, [])


def invalid_utf8_path(case, actual, observed):
    """A raw byte that leaves the decoded path invalid UTF-8 (one latin1
    0xE9): Node's parser answers "Client Error"; the native request parser
    refuses the request line before routing with {"error":"BAD_REQUEST"}.
    Both 400, no quota, no provider request."""
    return check(case["status"] == 400 and b"Client Error" in edges.expected_body(case) and actual[0] == 400 and
                 actual[2] == b'{"error":"BAD_REQUEST"}' and not observed,
                 f"{case['name']}: native {actual[0]} {actual[2][:120]!r}")


ASSERTED = {
    "address-malformed-escape": edges.bad_url, "address-escaped-nul": edges.nul_param,
    "fragment": fragment_refusal, "raw-latin1-address": invalid_utf8_path,
}


def replay_case(h, server, case, lanes):
    name = case["name"]
    if "state" in case:
        return "matched" if edges.replay_state(h, server, case) else "failed"
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

        threads = [threading.Thread(target=one, args=(i, entry)) for i, entry in enumerate(kinds)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        observed = edges.settle(server, h.upstream, before, len(case["upstream"]))
        edges.repay(lanes, case, observed)
        ok = True
        late = 0
        for index, (record, actual) in enumerate(zip(case["responses"], results)):
            h.no_secret(actual[2], name)
            label = f"{name}[{index}:{kinds[index]['kind']}]"
            if actual[0] == 200 and (actual[2] != edges.expected_body(record) or actual[1].get("content-length") !=
                                     record["headers"].get("content-length")) and edges.late_follower(record, actual):
                late += 1
                continue
            ok = edges.compare(label, record, actual) and ok
        if late:
            # Three foreground workers: a reader queued behind them starts
            # after the shared fetch finished and answers the entry it wrote
            # (cache.hit true, the fetch's timestamp); every other byte equal.
            print(f"  {name}: {late} reader(s) answered from the fresh entry", flush=True)
        ok = edges.compare_upstream(name, case, observed, h.secrets) and ok
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
    if name in INFINITE_PROTOBUF:
        return "asserted" if infinite_protobuf(case, actual, observed) else "failed"
    if case["status"] == 429 and case["headers"].get("retry-after") and actual[0] == 429 and \
            b"temporarily busy" in actual[2]:
        native_after = int(actual[1].get("retry-after", "0"))
        check(1 <= native_after <= 30, f"{name}: retry-after {native_after} outside [1, 30]")
        record = dict(case, headers=dict(case["headers"], **{"retry-after": str(native_after)}))
        ok = edges.compare(name, record, actual) and edges.compare_upstream(name, case, observed, h.secrets)
        return "asserted" if ok else "failed"
    if name in RESTART_DIFFERENCE:
        # The SWR envelope is in the durable KV natively: 5 s after the
        # restart the entry is still fresh, a cache hit without a request.
        document = json.loads(actual[2]) if actual[0] == 200 else {}
        expected = json.loads(case["responseText"])
        strip = lambda d: {k: v for k, v in d.items() if k not in ("cache", "asOf", "stale")}  # noqa: E731
        ok = check(actual[0] == 200 and strip(document) == strip(expected) and document.get("stale") is False and
                   document.get("cache", {}).get("hit") is True and not observed,
                   f"{name}: native {actual[0]} {actual[2][:200]!r}, expected the durable fresh hit")
        return "asserted" if ok else "failed"
    ok = edges.compare(name, case, actual, remaining=case["group"] == "quota")
    ok = edges.compare_upstream(name, case, observed, h.secrets) and ok
    return "matched" if ok else "failed"


class TimedTable(edges.CaseTable):
    """Also records each answered request's [start, end] on the monotonic
    clock, so the worker bound can be measured."""

    def __init__(self, cases):
        super().__init__(cases)
        self.spans = []
        self.override = None

    def answer(self, method, host, target, body):
        if self.override is not None:
            reply = self.override(method, host, target, body)
            self.spans.append((time.monotonic(), time.monotonic() + reply[2] / 1000))
            return reply
        status, payload, delay, is_json = super().answer(method, host, target, body)
        # mode.hostDelayMs: the per-host reply delay that orders a race
        # between two endpoints the same way in both runtimes.
        delay = (self.mode.get("hostDelayMs") or {}).get(host, delay)
        return status, payload, delay, is_json


def bounded_workers(h, server, fixture):
    """Ten distinct cold addresses at once, the upstream answering after
    400 ms: every read answers 200 with its own document, and no more than
    two provider requests overlap (the execution adapter's HTTPS workers)."""
    addresses = [f"0x{index:040x}" for index in range(0x9000, 0x900a)]

    def answer(method, host, target, body):
        request = json.loads(body)
        holder = request["params"][0]
        value = "0x" if request["method"] == "eth_getCode" else hex(int(holder, 16) - 0x9000 + 1)
        return 200, json.dumps({"jsonrpc": "2.0", "id": 1, "result": value}).encode(), 400, True

    harness.set_clock(h.clock, fixture["now"] + 7 * 86400000)
    time.sleep(1.2)  # the rpc lane's burst refilled
    h.table.override = answer
    h.table.spans = []
    results = [None] * len(addresses)
    barrier = threading.Barrier(len(addresses))

    def one(index):
        barrier.wait()
        results[index] = catalog.raw(server.port, "GET", f"/v1/networks/eth-mainnet/address/{addresses[index]}/insight",
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
        ok = check(document.get("txCount") == index + 1 and document.get("kind") == "eoa",
                   f"bounded-workers[{index}]: {result[0] if result else None} {result[2][:120] if result else b''!r}") \
            and ok
    events = sorted([(start, 1) for start, _ in h.table.spans] + [(end, -1) for _, end in h.table.spans])
    peak = current = 0
    for _, delta in events:
        current += delta
        peak = max(peak, current)
    ok = check(len(h.table.spans) == 20 and peak <= 2,
               f"bounded-workers: {len(h.table.spans)} provider requests, peak in flight {peak} (bound 2)") and ok
    print(f"bounded-workers: 10 cold reads, {len(h.table.spans)} provider requests, peak in flight {peak}", flush=True)
    return ok


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = load_fixture()
    only = set(filter(None, os.environ.get("R14_ONLY_GROUPS", "").split(",")))
    groups = {g["name"]: g for g in fixture["groups"]}
    by_group = collections.OrderedDict()
    for case in fixture["cases"]:
        by_group.setdefault(case["group"], []).append(case)
    totals = collections.Counter()
    with tempfile.TemporaryDirectory(prefix="address-insight-") as root:
        root = Path(root)
        h = edges.Harness(binary, root, fixture)
        h.table = TimedTable(fixture["cases"])
        h.upstream.table = h.table
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
                            status, _, _ = catalog.raw(server.port, "GET", "/v1/networks/nowhere/address/"
                                                       + fixture["holders"]["EOA"] + "/insight",
                                                       {"authorization": "Bearer " + catalog.token("d")})
                            if not check(status == 400, f"quota drain answered {status}"):
                                break
                    for case in cases:
                        if case["name"] == BREAKER_RECOVERY and breaker_opened is not None:
                            wait = breaker_opened + 31 - time.monotonic()
                            if wait > 0:
                                time.sleep(wait)
                        if case["name"] in BREAKER_CASES:
                            breaker_opened = time.monotonic()
                        results[replay_case(h, server, case, lanes)] += 1
                        if os.environ.get("R14_FAIL_FAST") and edges.FAILURES:
                            raise SystemExit(1)
                    if name == "main" and not only:
                        results["matched" if bounded_workers(h, server, fixture) else "failed"] += 1
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
            print(f"requests TypeScript never made: {sorted(set(h.table.extra))[:10]}")
        if len(sys.argv) == 3:
            production(sys.argv[2], root, fixture)
    print(f"address insight: {totals['matched']} matched, {totals['asserted']} asserted differences, "
          f"{totals['failed']} failed")
    if edges.FAILURES:
        print(f"{len(edges.FAILURES)} failure(s)")
        raise SystemExit(1)
    print("address insight edges: all checks passed")


def production(binary, root, fixture):
    """The production composition: the route exists only behind the canary."""
    (root / "prod-tls").mkdir()
    ca, _, _, _ = tls_ca.create_certificates(root / "prod-tls", "DNS:localhost")
    path = "/v1/networks/eth-mainnet/address/" + fixture["holders"]["EOA"] + "/insight"
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
        status, _, body = catalog.raw(server.port, "GET", "/v1/networks/eth-mainnet/address//insight", {})
        # opaque-params: the router's 404 before any hook.
        check(status == 404, f"production empty address without a bearer: {status} {body!r}")
    finally:
        server.stop()
    print("production: canary unset/0 answers 404; on, no bearer 401 and Origin 403")


if __name__ == "__main__":
    main()
