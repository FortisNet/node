#!/usr/bin/env python3
"""Real-socket edges of POST /v1/bittensor/analytics (R40) against
tests/corpus/bittensor_analytics_edges.json, which
the earlier TypeScript implementation's tests recorded
from the real Fastify app over real sockets. The fixture's SHA-256 is
checked against its pinned .sha256 file before anything runs, and a modified
copy is refused (self-checked).

build/fortis_node_discovery_route_test sends every Taostats request to
one loopback HTTPS upstream (path and query kept); per case the upstream
answers the exact replies TypeScript was served, in order per path (retries
and background revalidations included). Each case's frozen instant goes to
the discovery clock file. Each fixture group is a fresh process with its own
KV directory; restart-b reuses restart-a's after a SIGKILL.

Per request record: the status, every recorded header
(x-rate-limit-remaining only in the quota group), the exact JSON text or
protobuf bytes, the exact sequence of provider paths and their identity
headers (accept, and the key as Authorization; no client header or device
credential), and the durable state: every `taostats:` key TypeScript wrote
in the case holds TypeScript's final envelope (asOf, freshUntil, staleUntil,
JSON value) through POST /probe/test-discovery-state. Concurrent records
compare every answer and the provider paths. No device token/id or the
Taostats key reaches a response or the process output.

Asserted differences are listed in ASSERTED with the reason in each rule's
docstring, and named beside the handler in src/discovery/bittensor_route.cpp.

Native-only: `restart` (a SIGKILL keeps the durable entry and its window;
the miss hold does not survive), `saturation` (more simultaneous cold reads
than the 16 handoff slots: the excess answers 503 without provider work, the
rest complete, the server stays healthy), `sustained` (96 cached reads from
12 clients are byte-identical per representation and reach no provider) and,
with a second argument, the production composition.

Usage: bittensor_analytics_edges_test.py build/fortis_node_discovery_route_test [build/fortis_node]
Environment: R40_ONLY_GROUPS=a,b limits groups; R40_NATIVE_ONLY=<edge> runs one
native-only edge; R40_R47_FAIL_FAST=1 stops at
the first failure.
"""
import collections
import json
import os
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compat_expectations  # noqa: E402
import discovery_route_harness as harness  # noqa: E402
import discovery_post_edges as edges  # noqa: E402
from discovery_post_edges import check, COUNTS  # noqa: E402

FIXTURE = "bittensor_analytics_edges.json"
ROUTE = "/v1/bittensor/analytics"
COST = 10


def request_headers(case, item=None):
    source = item if item is not None else case
    headers = [("authorization", "Bearer " + edges.token(case["device"]))] if case["device"] else []
    return headers + [tuple(pair) for pair in source["requestHeaders"]]


def body_of(source):
    return None if source["bodyHex"] is None else bytes.fromhex(source["bodyHex"])


class Harness:
    def __init__(self, binary, root, fixture):
        self.binary = binary
        self.root = Path(root)
        self.fixture = fixture
        self.clock = self.root / "clock"
        self.replies = edges.Replies()
        self.upstream = harness.Upstream(self.root, self.replies)
        self.lanes = edges.Lanes()
        self.quota = edges.Quota()
        self.key = fixture["key"]
        self.client_values = list(fixture["clientHeaders"].values())
        self.by_name = {(c["group"], c["name"]): c for c in fixture["cases"]}

    def server(self, kv_name, env, start):
        kv_dir = self.root / f"kv-{kv_name}"
        kv_dir.mkdir(exist_ok=True)
        harness.set_clock(self.clock, start)
        return edges.Server(self.binary, kv_dir, self.clock, self.upstream, env)

    def no_secret(self, text, label):
        if isinstance(text, bytes):
            text = text.decode("utf-8", "replace")
        for secret in (self.key, "fintel_dev_"):
            check(secret not in text, f"{label}: {secret[:16]}… reached the response or output")


def compare_upstream(h, label, case, observed, ordered=True):
    expected = [edges.path_of(c["url"]) for c in case["upstream"]]
    actual = [path for path, _ in observed]
    if case["name"] == "concurrent-spellings":
        # Four equal cache keys race to become the one provider-flight leader.
        # Its query insertion order may be either JSON spelling; both yield
        # the same value and durable key. Still require exactly one call.
        alternate = "/api/subnet/owner/v1?limit=5&owner=slow-b"
        if actual == [alternate]:
            actual = expected[:]
    if not ordered:
        expected, actual = sorted(expected), sorted(actual)
    ok = check(actual == expected, f"{label}: upstream\n   expected {expected}\n   actual   {actual}")
    for path, headers in observed:
        sent = f"{path}{json.dumps(headers)}"
        for secret in h.client_values + ["fintel_dev_"]:
            ok &= check(secret not in sent, f"{label}: {secret[:20]} reached the provider")
        ok &= check(h.key not in path, f"{label}: the key reached the provider path")
        ok &= check(headers.get("authorization") == h.key and headers.get("accept") == "application/json",
                    f"{label}: provider identity headers {headers}")
    return ok


# ---- asserted differences --------------------------------------------------------------------------

def iso(ms):
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(ms // 1000)) + ".%03dZ" % (ms % 1000)


def restart_durable_swr(h, server, case, actual, observed):
    """After the restart the `taostats:` entry is in the durable discovery KV
    natively, so the answer is served from the first process's write ({hit:
    true, timestamp: that write}) with no provider request, where
    TypeScript's new process (its production Kv is in memory) read cold and
    asked Taostats again ({hit: false, timestamp: null}). The payload is
    TypeScript's own answer of this case; only `cache` differs, by exactly
    that; the entry keeps the first process's window."""
    status, headers, payload = actual
    before = h.by_name[("restart-a", "restart-before")]
    document = json.loads(case["responseText"], object_pairs_hook=collections.OrderedDict)
    document["cache"] = collections.OrderedDict([("hit", True), ("timestamp", iso(before["at"]))])
    want = json.dumps(document, separators=(",", ":"), ensure_ascii=False).encode()
    ok = check(status == 200 == case["status"] and payload == want,
               f"{case['name']}: not TypeScript's answer read from the durable entry "
               f"{edges.first_difference(want, payload)}")
    ok &= edges.compare(case["name"], case, status, headers, payload, skip=("content-length", "body"))
    ok &= check(headers.get("content-length") == str(len(want)), f"{case['name']}: content-length")
    ok &= check(not observed, f"{case['name']}: provider requests {observed}")
    return ok and edges.compare_state(case["name"], server, before["kvWrites"])


def json_reader_bounds(h, server, case, actual, observed):
    """A lone surrogate escape ("\\ud800") or a number that overflows a
    double (1e400) in a JSON body: TypeScript's JSON.parse reads them (the
    surrogate reaches the URL as U+FFFD, Infinity fails zod after the quota);
    the shared native bounded JSON reader refuses both as invalid JSON: 400
    {"error":"BAD_REQUEST"} after the device policy (fastify-errors),
    with no provider request (router-wide, as R46; the app's JSON encoder
    never writes either)."""
    ok = edges.native_invalid_json(case["name"], actual, COST)
    return check(not observed, f"{case['name']}: provider requests {observed}") and ok


def router_404(h, server, case, actual, observed):
    """Other methods (GET, HEAD, PUT), a trailing slash or another case: both
    the router's 404 before any hook; Fastify's body names the route, the
    native router answers its own {"error":"NOT_FOUND"} with the API baseline
    headers (router-wide, as R19–R25)."""
    ok = edges.native_plain_error(case["name"], actual, 404, edges.NATIVE_NOT_FOUND,
                                  head=case["method"] == "HEAD")
    return check(case["status"] == 404 and not observed,
                 f"{case['name']}: router fixture/provider mismatch {observed}") and ok


def strict_refused(detail):
    """compat-decisions.md#js-prototype-keys: an Object.prototype
    member path is an ordinary unsupported path (TypeScript: 502), and a
    `__proto__` Struct entry an ordinary query key, which no path allows
    (TypeScript dropped the entry and answered 200)."""
    want = json.dumps({"error": "BAD_REQUEST", "detail": detail}, separators=(",", ":")).encode()

    def assertion(h, server, case, actual, observed):
        status, headers, payload = actual
        return check(status == 400 and payload == want and not observed and "x-rate-limit-cost" in headers,
                     f"{case['name']}: {status} {payload[:160]!r} is not 400 {detail!r} after the hook")
    return assertion


def strict_cold_read(h, server, case, actual, observed):
    """compat-decisions.md#form-bodies: the form record that warmed
    this entry in TypeScript (media-form-proto-key) is now a 415, so this read
    is the cold one: TypeScript's payload with {hit: false, timestamp: null}."""
    status, headers, payload = actual
    recorded = case["responseText"]
    at = recorded.rindex(',"cache":{')
    want = (recorded[:at] + ',"cache":{"hit":false,"timestamp":null}}').encode("utf-8", "surrogatepass")
    return check(status == 200 == case["status"] and payload == want,
                 f"{case['name']}: not the cold read {edges.first_difference(want, payload)}")


ASSERTED = {
    **{name: strict_refused("unsupported bittensor analytics path")
       for name in ("path-proto-constructor", "path-proto-to-string", "path-proto-has-own", "path-proto-value-of",
                    "path-proto-dunder", "path-proto-locale", "path-proto-lookup", "path-proto-protobuf")},
    "pb-query-proto-key": strict_refused("unsupported bittensor query"),
    "pb-unknown-group": strict_cold_read,
    **{name: router_404 for name in ("router-get", "router-put", "router-head", "router-trailing-slash",
                                     "router-upper")},
    "restart-after": restart_durable_swr,
    "zod-number-overflow": json_reader_bounds,
}
# google.protobuf.Value number_value (field 2, fixed64) of -0 and of 0.
NEGATIVE_ZERO = "11" + "0000000000000080"
ZERO = "11" + "0000000000000000"


def negative_zero_l1(case, status, headers, payload):
    """negative_zero_l1: a provider -0 is protobuf number_value -0 in
    TypeScript only while its in-process L1 holds the parsed value (≤ 30 s);
    natively, and in TypeScript after the L1, the stored JSON text makes it
    0. The protobuf answer equals TypeScript's with every -0 number_value
    read as 0, and nothing else differs."""
    if "wireHex" not in case or NEGATIVE_ZERO not in case["wireHex"]:
        return False
    want = dict(case, wireHex=case["wireHex"].replace(NEGATIVE_ZERO, ZERO))
    return edges.expected_body(want) == payload and status == case["status"]


# ---- the replay ----------------------------------------------------------------------------------

def replay_case(h, server, case):
    name = case["name"]
    harness.set_clock(h.clock, case["at"])
    h.replies.start(case["upstream"])
    h.lanes.reserve(case["upstream"])
    h.quota.reserve(case["device"], COST)
    before = len(h.upstream.calls())
    status, headers, payload = edges.raw(server.port, case["method"], case["target"], request_headers(case),
                                         body_of(case))
    observed = edges.settle(h.upstream, before, len(case["upstream"]), grace=edges.grace_for(case["upstream"]))
    h.lanes.breaker(case["upstream"])
    h.no_secret(payload, name)
    h.no_secret(json.dumps(headers), name)
    if name in ASSERTED:
        return ASSERTED[name](h, server, case, (status, headers, payload), observed)
    if negative_zero_l1(case, status, headers, payload):
        ok = edges.compare(name, case, status, headers, payload, remaining=case["group"] == "quota", skip=("body",))
        ok &= compare_upstream(h, name, case, observed)
        ok &= edges.compare_state(name, server, case["kvWrites"])
        return "asserted" if ok else False
    ok = edges.compare(name, case, status, headers, payload, remaining=case["group"] == "quota")
    if compat_expectations.is_removed_body_media(edges.request_header(case, "content-type")):
        # compat-decisions.md#form-bodies: refused before the facet.
        return ok and check(not observed, f"{name}: provider requests {observed} for a 415")
    ok &= compare_upstream(h, name, case, observed)
    ok &= edges.compare_state(name, server, case["kvWrites"])
    return ok


def replay_concurrent(h, server, case):
    harness.set_clock(h.clock, case["at"])
    h.replies.start(case["upstream"])
    if case["name"] == "concurrent-spellings":
        leader_reply = case["upstream"][0]
        alternate = "/api/subnet/owner/v1?limit=5&owner=slow-b"

        def alternate_leader(path, _headers):
            if path != alternate:
                return None
            time.sleep(leader_reply.get("delayMs", 25) / 1000)
            return (leader_reply["status"], dict(leader_reply.get("replyHeaders", {})),
                    leader_reply["body"].encode("utf-8", "surrogatepass"))

        h.replies.fallback = alternate_leader
    h.lanes.reserve(case["upstream"])
    h.quota.reserve(case["device"], COST * len(case["concurrent"]))
    before = len(h.upstream.calls())
    results = [None] * len(case["concurrent"])
    barrier = threading.Barrier(len(results))

    def run(index, item):
        barrier.wait()
        # A staggered record sends its requests `staggerMs` apart, in order.
        time.sleep(index * case.get("staggerMs", 0) / 1000)
        results[index] = edges.raw(server.port, "POST", ROUTE, request_headers(case, item), body_of(item))

    threads = [threading.Thread(target=run, args=(i, item)) for i, item in enumerate(case["concurrent"])]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    observed = edges.settle(h.upstream, before, len(case["upstream"]), grace=edges.grace_for(case["upstream"], 0.5))
    h.replies.fallback = None
    for result in results:
        h.no_secret(result[2], case["name"])
    ok = True
    asserted = False
    for index, (record, result) in enumerate(zip(case["responses"], results)):
        if negative_zero_l1(record, result[0], result[1], result[2]):
            asserted = True
            ok &= edges.compare(f"{case['name']}[{index}]", record, *result, skip=("body",))
        else:
            ok &= edges.compare(f"{case['name']}[{index}]", record, *result)
    ok &= compare_upstream(h, case["name"], case, observed, ordered=False)
    ok &= edges.compare_state(case["name"], server, case["kvWrites"])
    return "asserted" if ok and asserted else ok


# ---- native-only edges ------------------------------------------------------------------------------

def post(server, letter, body, accept="application/json", content_type="application/json"):
    headers = [("authorization", "Bearer " + edges.token(letter)), ("content-type", content_type),
               ("accept", accept)]
    return edges.raw(server.port, "POST", ROUTE, headers, body)


def restart_check(h, at):
    """SIGKILL durability: a cold read's entry and window survive a hard
    kill; the next process serves it (hit, the first write's timestamp)
    without provider work; a held cold failure does not survive (the new
    process asks the provider again, three attempts)."""
    h.replies.start([])
    good = json.dumps({"path": "/api/subnet/owner/v1", "query": {"owner": "restart-native"}}).encode()
    down = json.dumps({"path": "/api/subnet/owner/v1", "query": {"owner": "down"}}).encode()
    page = b'{"data":["restart native"]}'
    h.replies.fallback = lambda path, _headers: (200, {"Content-Type": "application/json"}, page) \
        if "restart-native" in path else (503, {}, b"provider unavailable") if "owner=down" in path else None
    try:
        server = h.server("native-restart", {"TAOSTATS_API_KEY": h.key}, at)
        h.quota.reserve("N", 4 * COST)
        before = len(h.upstream.calls())
        first = post(server, "N", good)
        held = post(server, "N", down)
        calls = len(h.upstream.calls()) - before
        server.stop(kill=True)
        server = h.server("native-restart", {"TAOSTATS_API_KEY": h.key}, at + 1000)
        before = len(h.upstream.calls())
        again = post(server, "N", good)
        down_again = post(server, "N", down)
        after = h.upstream.calls()[before:]
        server.stop()
        want = b'{"payload":{"data":["restart native"]},"cache":{"hit":true,"timestamp":"' + iso(at).encode() + b'"}}'
        ok = check(first[0] == 200 and held[0] == 502 and calls == 4 and again[0] == 200 and again[2] == want and
                   down_again[0] == 502 and len(after) == 3 and all("owner=down" in p for p, _ in after),
                   f"restart: first {first[0]} held {held[0]} calls {calls}; after kill {again[0]} {again[2][:120]!r} "
                   f"{down_again[0]} {[p for p, _ in after]}")
        print(f"  restart: SIGKILL kept the entry ({len(again[2])} B answer from KV, 0 provider calls); "
              f"the held failure retried ({len(after)} attempts)", flush=True)
        return ok
    finally:
        h.replies.fallback = None


def saturation(h, at):
    """More simultaneous cold reads than the 16 handoff slots: every request
    is answered; the excess is 503 UPSTREAM_UNAVAILABLE (cost charged, no
    Vary, no provider work), the rest the provider's page or, past the
    lane's 2 s queue timeout, 429 RATE_LIMITED with retry-after; at most four
    provider requests run at once (the taostats lane); then a cached read is
    served."""
    page = b'{"data":["saturated"]}'
    in_flight = {"now": 0, "peak": 0}
    lock = threading.Lock()

    def answer(path, _headers):
        if "saturate-" not in path:
            return None
        with lock:
            in_flight["now"] += 1
            in_flight["peak"] = max(in_flight["peak"], in_flight["now"])
        time.sleep(0.3)
        with lock:
            in_flight["now"] -= 1
        return 200, {"Content-Type": "application/json"}, page

    h.replies.start([])
    h.replies.fallback = answer
    try:
        server = h.server("native-saturation", {"TAOSTATS_API_KEY": h.key}, at)
        count = 20
        letters = "EFGIJKLMNOPQRSTVWXYZ"
        for letter in letters[:count]:
            h.quota.reserve(letter, COST)
        results = [None] * count
        barrier = threading.Barrier(count)

        def run(index):
            body = json.dumps({"path": "/api/subnet/owner/v1", "query": {"owner": f"saturate-{index}"}}).encode()
            barrier.wait()
            results[index] = post(server, letters[index], body)

        threads = [threading.Thread(target=run, args=(i,)) for i in range(count)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        statuses = collections.Counter(status for status, _, _ in results)
        refused = [r for r in results if r[0] == 503]
        served = [r for r in results if r[0] == 200]
        throttled = [r for r in results if r[0] == 429]
        cached_index = next((i for i, r in enumerate(results) if r[0] == 200), 0)
        h.quota.reserve("U", COST)
        cached = post(server, "U", json.dumps({"path": "/api/subnet/owner/v1",
                                               "query": {"owner": f"saturate-{cached_index}"}}).encode())
        server.stop()
        ok = check(sum(statuses.values()) == count and len(refused) == count - 16 and
                   len(served) + len(throttled) == 16 and served and
                   all(r[2] == b'{"error":"UPSTREAM_UNAVAILABLE"}' and "vary" not in r[1] and
                       r[1].get("x-rate-limit-cost") == "10" for r in refused) and
                   all(r[1].get("retry-after") and b"RATE_LIMITED" in r[2] for r in throttled) and
                   1 <= in_flight["peak"] <= 4 and cached[0] == 200 and b'"hit":true' in cached[2],
                   f"saturation: {dict(statuses)}, peak {in_flight['peak']}, cached {cached[0]} {cached[2][:80]!r}")
        print(f"  saturation: {count} simultaneous cold reads → {dict(statuses)}; provider peak "
              f"{in_flight['peak']} in flight; a cached read then served", flush=True)
        return ok
    finally:
        h.replies.fallback = None


def sustained(h, server, at):
    """One cold read of a fresh key, then 96 reads of it from 12 clients
    (JSON and protobuf) at the same instant give one byte string per
    representation and reach no provider."""
    harness.set_clock(h.clock, at)
    body = json.dumps({"path": "/api/subnet/owner/v1", "query": {"owner": "sustained"}}).encode()
    page = b'{"data":["sustained"]}'
    h.replies.start([])
    h.replies.fallback = lambda path, _headers: (200, {"Content-Type": "application/json"}, page) \
        if "owner=sustained" in path else None
    letters = "EFGIJKLMNOPR"
    try:
        h.quota.reserve("Q", COST)
        first = post(server, "Q", body)
        before = len(h.upstream.calls())
        results = collections.defaultdict(set)
        statuses = collections.Counter()
        lock = threading.Lock()
        for letter in letters:
            h.quota.reserve(letter, 8 * COST)

        def client(index):
            for n in range(8):
                accept = "application/protobuf" if (index + n) % 2 else "application/json"
                status, _, payload = post(server, letters[index], body, accept=accept)
                with lock:
                    statuses[status] += 1
                    results[accept].add(payload)

        threads = [threading.Thread(target=client, args=(i,)) for i in range(12)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        calls = h.upstream.calls()[before:]
    finally:
        h.replies.fallback = None
    ok = check(first[0] == 200 and statuses == collections.Counter({200: 96}) and
               all(len(v) == 1 for v in results.values()) and len(results) == 2 and not calls,
               f"sustained: first {first[0]}, statuses {dict(statuses)}, variants "
               f"{[len(v) for v in results.values()]}, calls {calls[:3]}")
    print(f"  sustained: 96 reads from 12 clients, {len(results)} distinct answers, {len(calls)} provider calls",
          flush=True)
    return ok


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = edges.load_fixture(FIXTURE)
    if not edges.pin_self_check(FIXTURE):
        raise SystemExit("the pin self-check failed")
    only = [g for g in os.environ.get("R40_ONLY_GROUPS", "").split(",") if g]
    # R40_NATIVE_ONLY=<edge> runs one native-only edge and no fixture group.
    native_only = os.environ.get("R40_NATIVE_ONLY", "")
    if native_only:
        only = ["(none)"]
    with tempfile.TemporaryDirectory(prefix="fortis-r40-") as root:
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
                    first = next(c for c in selected if c["name"] == "quota-second-last")
                    edges.drain_quota(server, ROUTE, "D", int(first["headers"]["x-rate-limit-remaining"]) + COST,
                                      body=b'{"path":"/x"}')
                for case in selected:
                    ok = replay_concurrent(h, server, case) if "concurrent" in case else replay_case(h, server, case)
                    kind = "asserted" if case["name"] in ASSERTED or ok == "asserted" else "matched"
                    COUNTS[(name, kind if ok else "failed")] += 1
                if name == "main":
                    COUNTS[("sustained", "matched" if sustained(h, server, selected[-1]["at"] + 60_000) else "failed")] += 1
                if group["kv"] != "restart" or name == "restart-b":
                    server.stop()
                    h.no_secret(server.text(), f"{name} process output")
                    servers.pop(group["kv"], None)
            if not only or native_only:
                at = fixture["cases"][0]["at"] + 20 * 3_600_000
                if native_only in ("", "restart"):
                    COUNTS[("restart", "matched" if restart_check(h, at) else "failed")] += 1
                if native_only in ("", "saturation"):
                    COUNTS[("saturation", "matched" if saturation(h, at + 60_000) else "failed")] += 1
        finally:
            for server in servers.values():
                server.stop()
            h.upstream.close()
        if len(sys.argv) == 3 and not only:
            json_type = ("content-type", "application/json")
            body = b'{"path":"/api/account/latest/v1"}'
            ok = edges.production(sys.argv[2], root, [
                ROUTE,
                ("no bearer", [json_type], body, 401, b'{"error":"UNAUTHORIZED"}'),
                ("origin", [json_type, ("origin", "https://evil.example")], body, 403,
                 b'{"error":"BROWSER_ORIGIN_FORBIDDEN"}'),
                # fastify-errors: auth-first, so no bearer is 401 whatever the body.
                ("unsupported media, no bearer", [("content-type", "text/xml")], b"<x/>", 401,
                 b'{"error":"UNAUTHORIZED"}'),
            ])
            COUNTS[("production", "matched" if ok else "failed")] += 1
        if h.replies.extra:
            print("requests beyond TypeScript's:", sorted(set(h.replies.extra))[:20])
    edges.summary("bittensor analytics edges")


if __name__ == "__main__":
    main()
