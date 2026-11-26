#!/usr/bin/env python3
"""Real-socket edges of GET /v1/networks/robinhood/stock-tokens/{address}
(R12) against tests/corpus/robinhood_instrument_edges.json, which
the earlier TypeScript implementation's tests recorded
from the real Fastify app over real sockets.

The test binary (build/fortis_node_robinhood_route_test) runs with
FORTIS_NATIVE_ROBINHOOD_CANARY=1 and sends api.robinhood.com to one loopback
HTTPS upstream (robinhood_route_test.Upstream) that answers each case's
provider table with the bytes and delays the TypeScript run served, and
records every request with its arrival time. Per case the replay writes the
case's frozen instant to the Robinhood clock file, so the 15-minute catalog,
15-second quote and one-hour corporate-action caches expire exactly where
TypeScript's did. Each fixture group is a fresh process with its own KV
directory; `restart-b` reuses `restart-a`'s directory after a SIGKILL.

Checks, per case: the status, every recorded header except
x-rate-limit-remaining (compared in the quota group only), the exact JSON
text or protobuf bytes (sha256 and length beyond 64 KiB) and the provider
requests (count, method, URL, accept) including those that continue after the
answer (the case's `settleMs`). No provider request may carry a client
header, and no device token or id may reach a response or process output.

Promise.all (the `parallel` and `quote-429` groups and the cold reads):
wherever TypeScript started the quote and the corporate-action requests
within 50 ms of each other, native must start them within 100 ms of each
other, and native must answer no later than TypeScript (+25 % + 300 ms;
native test transports skip HttpClient's retry sleeps, so a retried failure
answers sooner).

Asserted differences (documented beside the native handler):
  - router: other methods and spellings are 404 in both (status only); the
    routing 414 carries the native security headers (body and other
    headers exact);
  - bounds: a corporate-actions body over 512 KiB is a transport failure
    (502 after one request; TypeScript parses it);
  - concurrency: eight simultaneous cold reads of two instruments run on
    four workers, so one instrument's quote may start a provider round late
    (bodies and provider requests exact; at most two rounds).

Then: 200 sustained warm reads of one instrument from 12 clients (every
answer the exact document, or the typed back-pressure 429 when the job pool
is full) and the production composition (build/fortis_node).
Usage: robinhood_instrument_edges_test.py build/fortis_node_robinhood_route_test [build/fortis_node]
"""
import collections
import hashlib
import json
import signal
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compat_expectations  # noqa: E402
import robinhood_stock_catalog_edges_test as catalog  # noqa: E402
import tls_ca  # noqa: E402

FIXTURES = catalog.FIXTURES
FIXTURE = "robinhood_instrument_edges.json"
BASE = "/v1/networks/robinhood/stock-tokens"
ROUTER_404 = {"router-post", "router-trailing-slash", "router-extra-segment", "router-upper-path"}
UNAVAILABLE = b'{"error":"UPSTREAM_UNAVAILABLE"}'
PRICE = "api.robinhood.com/rhj/prices/"
ACTIONS = "api.robinhood.com/rhj/corporate-actions"
TIMED_GROUPS = {"parallel", "quote-429"}
NATIVE_BASELINE = {"cache-control": "no-store", "x-content-type-options": "nosniff", "referrer-policy": "no-referrer",
                   "permissions-policy": "camera=(), microphone=(), geolocation=()"}
FAILURES = catalog.FAILURES
TIMINGS = {}
check = catalog.check


def load_fixture():
    data = (FIXTURES / FIXTURE).read_bytes()
    pinned = (FIXTURES / f"{FIXTURE}.sha256").read_text().split()[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{FIXTURE}: sha256 {digest} does not match the pinned {pinned}")
    import strict_contract_expectations
    return strict_contract_expectations.apply("robinhood_instrument_edges.json", json.loads(data))


def headers_of(case, kind=None):
    headers = {}
    if case["device"]:
        headers["authorization"] = "Bearer " + catalog.token(case["device"])
    headers.update(case["requestHeaders"])
    if kind == "protobuf":
        headers["accept"] = "application/protobuf"
    return headers


def starts(calls, origin):
    """(quote start, actions start) in ms after `origin`, or None."""
    quote = next((c["at"] for c in calls if PRICE in c["url"]), None)
    action = next((c["at"] for c in calls if ACTIONS in c["url"]), None)
    if quote is None or action is None:
        return None
    return (quote - origin) * 1000, (action - origin) * 1000


def oracle_starts(case):
    quote = next((c["startMs"] for c in case["upstream"] if PRICE in c["url"]), None)
    action = next((c["startMs"] for c in case["upstream"] if ACTIONS in c["url"]), None)
    return None if quote is None or action is None else (quote, action)


def check_timing(case, observed, origin, elapsed):
    name = case["name"]
    oracle = oracle_starts(case)
    mine = starts(observed, origin)
    TIMINGS[name] = (elapsed, case["elapsedMs"] / 1000)
    if oracle and abs(oracle[0] - oracle[1]) < 50:
        ok = check(mine is not None and abs(mine[0] - mine[1]) < 100,
                   f"{name}: quote/actions started at {mine} ms natively (TypeScript {oracle} ms: together)")
        if not ok:
            return False
    budget = case["elapsedMs"] / 1000 * 1.25 + 0.3
    return check(elapsed <= budget, f"{name}: native answered in {elapsed:.2f} s, TypeScript {case['elapsedMs'] / 1000:.2f} s")


def run_concurrent(harness, port, case, target):
    kinds = case["concurrent"]
    results = [None] * len(kinds)
    barrier = threading.Barrier(len(kinds))

    def one(index, entry):
        path = f"{BASE}/{entry['address']}" if entry.get("address") else target
        barrier.wait()
        try:
            results[index] = catalog.raw(port, "HEAD" if entry["kind"] == "head" else "GET", path,
                                         headers_of(case, entry["kind"]))
        except (OSError, IndexError, ValueError) as error:
            results[index] = (0, {}, repr(error).encode())

    threads = [threading.Thread(target=one, args=(i, entry)) for i, entry in enumerate(kinds)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    matched = 0
    for index, (record, actual) in enumerate(zip(case["responses"], results)):
        harness.no_secret(actual[2], case["name"])
        matched += catalog.compare(f"{case['name']}[{index}:{kinds[index]['kind']}]", record, actual)
    return matched == len(kinds)


def replay_case(harness, port, case):
    """Returns 'matched', 'asserted' or 'failed'."""
    name = case["name"]
    harness.clock.write_text(str(case["at"]))
    harness.upstream.start_case(catalog.resolve_table(case["providers"], harness.fixture["bodies"]))
    target = bytes.fromhex(case["targetHex"]).decode("latin1")
    origin = time.monotonic()
    if "concurrent" in case:
        ok = run_concurrent(harness, port, case, target)
        elapsed = time.monotonic() - origin
    else:
        actual = catalog.raw(port, case["method"], target, headers_of(case))
        elapsed = time.monotonic() - origin
        harness.no_secret(actual[2], name)
    if case.get("settleMs"):
        time.sleep(case["settleMs"] / 1000)
    elif "concurrent" not in case and actual[0] in (429, 502):
        # A failure answers at once while the other read of Promise.all may
        # still be connecting: let its request reach the upstream log.
        time.sleep(0.25)
    observed = harness.upstream.calls()
    timed = case["group"] in TIMED_GROUPS or name.startswith("cold-json")
    if "concurrent" in case:
        ok = catalog.compare_upstream(name, case, observed, harness.secrets) and ok
        if name == "concurrent-two-instruments":
            # Eight reads over four workers: when the first four are all one
            # instrument, the other instrument's quote starts when a worker
            # frees (one more provider round). Bodies and requests are exact.
            TIMINGS[name] = (elapsed, case["elapsedMs"] / 1000)
            check(elapsed <= 2 * case["elapsedMs"] / 1000 * 1.25 + 0.3,
                  f"{name}: native answered in {elapsed:.2f} s, beyond two TypeScript rounds")
            return "asserted" if ok else "failed"
        if timed:
            ok = check_timing(case, observed, origin, elapsed) and ok
        return "matched" if ok else "failed"
    status, headers, payload = actual
    if compat_expectations.malformed_path_target(case):
        # opaque-params: an empty, control-character or over-long
        # path value is the router's 404 before any handler.
        ok = check(compat_expectations.router_refused(status, headers, payload, observed),
                   f"{name}: malformed path value is not the router's 404")
        return "asserted" if ok else "failed"
    if name in ROUTER_404:
        check(case["status"] == 404 and status == 404 and not observed, f"{name}: status {status} != 404")
        return "asserted"
    if name == "actions-over-cap":
        check(case["status"] == 200 and status == 502 and payload == UNAVAILABLE and headers.get("vary") is None,
              f"{name}: native {status} {payload[:60]!r}, expected 502 UPSTREAM_UNAVAILABLE")
        actions = [c for c in observed if ACTIONS in c["url"]]
        check(len(actions) == 1, f"{name}: {len(actions)} corporate-action requests, expected 1")
        return "asserted"
    if case["status"] == 414:
        # Fastify answers its routing 414 outside the /v1 plugin, without the
        # onSend security headers; native always sends them. Everything else,
        # the decoded message included, must match.
        # fastify-errors: the body is {"error":"URI_TOO_LONG"}.
        native = compat_expectations.fastify_case(dict(case, headers=dict(case["headers"], **NATIVE_BASELINE)))
        ok = catalog.compare(name, native, actual) and catalog.compare_upstream(name, case, observed, harness.secrets)
        return "asserted" if ok else "failed"
    ok = catalog.compare(name, case, actual, remaining=case["group"] == "quota")
    ok = catalog.compare_upstream(name, case, observed, harness.secrets) and ok
    if timed:
        ok = check_timing(case, observed, origin, elapsed) and ok
    return "matched" if ok else "failed"


def sustained(port, address, document):
    kinds = [("json", "protobuf", "head")[i % 3] for i in range(200)]
    # The protobuf answers must all be the same bytes (no oracle at this
    # instant); the JSON is the recorded document.
    expected = {"json": document, "protobuf": None, "head": b""}
    lock = threading.Lock()
    outcomes = []

    def client(offset):
        for kind in kinds[offset::12]:
            headers = {"authorization": "Bearer " + catalog.token("b")}
            if kind == "protobuf":
                headers["accept"] = "application/protobuf"
            try:
                result = catalog.raw(port, "HEAD" if kind == "head" else "GET", f"{BASE}/{address}", headers)
            except (OSError, IndexError, ValueError) as error:
                result = (0, {}, repr(error).encode())
            with lock:
                outcomes.append((kind, result))

    threads = [threading.Thread(target=client, args=(i,)) for i in range(12)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    protobuf = collections.Counter(r[2] for kind, r in outcomes if kind == "protobuf" and r[0] == 200)
    expected["protobuf"] = protobuf.most_common(1)[0][0] if protobuf else b"-"
    check(len(protobuf) <= 1 and expected["protobuf"][:1] == b"\x0a",
          f"sustained: {len(protobuf)} distinct protobuf answers")
    exact = sum(1 for kind, r in outcomes if r[0] == 200 and r[2] == expected[kind])
    pressure = sum(1 for kind, r in outcomes if r[0] == 429 and r[1].get("retry-after") == "2" and
                   (kind == "head" or b'"RATE_LIMITED"' in r[2]))
    refused = sum(1 for _, r in outcomes if r[0] == 0)
    wrong = len(outcomes) - exact - pressure - refused
    check(wrong == 0 and len(outcomes) == 200 and exact >= 150,
          f"sustained: {exact} exact, {pressure} back-pressure 429, {refused} refused, {wrong} wrong")
    print(f"sustained: {exact}/200 exact documents from 12 clients, {pressure} typed back-pressure 429, "
          f"{refused} refused at the socket, {wrong} wrong", flush=True)


def production(binary, root, address):
    (root / "prod-tls").mkdir()
    ca, _, _, _ = tls_ca.create_certificates(root / "prod-tls", "DNS:localhost")
    path = f"{BASE}/{address}"
    for value in (None, "0"):
        env = {"FORTIS_CA_BUNDLE_PATH": str(ca)}
        if value is not None:
            env["FORTIS_NATIVE_ROBINHOOD_CANARY"] = value
        server = catalog.Server(binary, root / f"kv-prod-off-{value}", env)
        try:
            status, _, _ = catalog.raw(server.port, "GET", path, {"authorization": "Bearer " + catalog.token("a")})
            check(status == 404, f"production canary {value!r}: {status}, not 404")
        finally:
            server.stop()
    server = catalog.Server(binary, root / "kv-prod-on", {"FORTIS_NATIVE_ROBINHOOD_CANARY": "1",
                                                           "FORTIS_CA_BUNDLE_PATH": str(ca)})
    try:
        status, headers, body = catalog.raw(server.port, "GET", path, {})
        check(status == 401 and body == b'{"error":"UNAUTHORIZED"}' and "vary" not in headers,
              f"production no-auth: {status} {body!r}")
        status, _, body = catalog.raw(server.port, "GET", f"{BASE}/", {})
        # opaque-params: an empty address is the router's 404.
        check(status == 404, f"production empty address without auth: {status} {body!r}")
        status, _, body = catalog.raw(server.port, "GET", path, {"origin": "https://evil.example"})
        check(status == 403 and body == b'{"error":"BROWSER_ORIGIN_FORBIDDEN"}', f"production origin: {status}")
    finally:
        server.stop()
    print("production: canary unset/0 answers 404; on, no bearer 401 (an empty address: the router's 404) and Origin 403")


def graceful_inflight_stop(harness, root, fixture, address):
    """Stop while both companion reads are active; they must be joined before teardown."""
    case = next(c for c in fixture["cases"] if c["name"] == "both-slow-ok")
    server = harness.server("stop-with-instrument", {}, case["at"])
    stopped = False
    reader = None
    result = []
    try:
        harness.clock.write_text(str(case["at"]))
        harness.upstream.start_case(catalog.resolve_table(case["providers"], fixture["bodies"]))

        def read():
            try:
                result.append(catalog.raw(server.port, "GET", f"{BASE}/{address}", headers_of(case)))
            except (OSError, ValueError) as error:
                result.append(error)

        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            calls = harness.upstream.calls()
            if any(PRICE in call["url"] for call in calls) and any(ACTIONS in call["url"] for call in calls):
                break
            time.sleep(.01)
        else:
            check(False, "graceful stop: quote and action reads never started")
        began = time.monotonic()
        output = server.stop(signal.SIGTERM)
        stopped = True
        elapsed = time.monotonic() - began
        if reader is not None:
            reader.join(timeout=5)
        check(server.process.returncode == 0 and elapsed < 5,
              f"graceful stop: exit {server.process.returncode} after {elapsed:.2f} s")
        check(reader is not None and not reader.is_alive() and result and not isinstance(result[0], Exception) and
              result[0][0] == case["status"] and result[0][2] == case["responseText"].encode(),
              "graceful stop: the in-flight response is complete and exact")
        harness.no_secret(output, "graceful stop output")
        print(f"graceful stop: companion reads joined and process exited in {elapsed:.2f} s")
    finally:
        if not stopped:
            server.stop(signal.SIGKILL)
        if reader is not None:
            reader.join(timeout=5)


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = load_fixture()
    groups = {g["name"]: g for g in fixture["groups"]}
    by_group = collections.OrderedDict()
    for case in fixture["cases"]:
        by_group.setdefault(case["group"], []).append(case)
    totals = collections.Counter()
    tsla = fixture["addresses"]["TSLA"].lower()
    with tempfile.TemporaryDirectory(prefix="robinhood-instrument-") as root:
        root = Path(root)
        harness = catalog.Harness(binary, root, fixture)
        try:
            for name, cases in by_group.items():
                group = groups[name]
                kv_name = "restart" if name.startswith("restart-") else name
                server = harness.server(kv_name, group["env"], group["start"])
                try:
                    if name == "quota":
                        catalog.wait_for_fresh_minute(20)
                        harness.upstream.start_case({})
                        for _ in range(240):
                            status, _, _ = catalog.raw(server.port, "GET",
                                                       "/v1/networks/robinhood/sponsored-trades/capabilities",
                                                       {"authorization": "Bearer " + catalog.token("d")})
                            if not check(status == 200, f"quota drain answered {status}"):
                                break
                    results = collections.Counter(replay_case(harness, server.port, case) for case in cases)
                    totals.update(results)
                    print(f"{name}: {results['matched']} matched, {results['asserted']} asserted differences, "
                          f"{results['failed']} failed", flush=True)
                    if name == "main":
                        by_name = {c["name"]: c for c in cases}
                        refetch = by_name["quote-refetch"]
                        harness.clock.write_text(str(refetch["at"]))
                        harness.upstream.start_case({})
                        sustained(server.port, tsla, refetch["responseText"].encode())
                        check(not harness.upstream.calls(), "sustained warm reads reached the provider")
                finally:
                    output = server.stop(signal.SIGKILL if name == "restart-a" else signal.SIGTERM)
                    harness.no_secret(output, f"{name} output")
            graceful_inflight_stop(harness, root, fixture, tsla)
        finally:
            harness.upstream.close()
        if len(sys.argv) == 3:
            production(sys.argv[2], root, tsla)
    for name, (native, oracle) in sorted(TIMINGS.items()):
        if native > 0.2 or oracle > 0.2:
            print(f"  timing {name}: native {native:.2f} s, TypeScript {oracle:.2f} s")
    print(f"robinhood instrument: {totals['matched']} matched, {totals['asserted']} asserted differences, "
          f"{totals['failed']} failed")
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        raise SystemExit(1)
    print("robinhood instrument edges: all checks passed")


if __name__ == "__main__":
    main()
