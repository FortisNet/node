#!/usr/bin/env python3
"""Real-socket edges of POST /v1/prices/markets (R47) against
tests/corpus/price_markets_edges.json, which
the earlier TypeScript implementation's tests recorded from
the real Fastify app over real sockets. The fixture's SHA-256 is checked
against its pinned .sha256 file before anything runs, and a modified copy is
refused (self-checked).

build/fortis_node_discovery_route_test sends every CoinGecko request to
one loopback HTTPS upstream that answers exactly as the oracle's provider
fake did, from the fixture's `provider` table: the first scenario id found in
a batch decides the whole reply, else each listed id's row text for the n-th
time that id is listed in a reply of this process (`rows[id][n - 1]`), joined
into a JSON array. So a batch TypeScript never sent (the native 50-id split,
no 20 ms window) is answered with the same rows. Each case's frozen instant
goes to the discovery clock file; each fixture group is a fresh process with
its own KV directory; restart-b reuses restart-a's after a SIGKILL.

Per request record: the status, every recorded header
(x-rate-limit-remaining only in the quota group), the exact JSON text or
protobuf bytes, the multiset of provider paths (and their identity headers:
accept, no key, no client header or device credential), and the durable
state: every `mkt:` key TypeScript wrote in the case holds TypeScript's final
envelope (asOf, freshUntil, staleUntil, JSON value). `state` records compare
the envelopes and the logo media records. Concurrent records compare every
answer and the provider requests.

Asserted differences are listed in ASSERTED with the reason in each rule's
docstring, and named beside the handler in src/discovery/price_markets_route.cpp.

Native-only: `restart` (SIGKILL durability of entries and media records),
`deferred-bound` (nine simultaneous cold requests: eight wait on the
provider, the ninth is answered at once with its coins pending, then all
complete), `sustained` (96 cached reads from 12 clients, byte-identical per
representation, no provider request) and, with a second argument, the
production composition.

Usage: price_markets_edges_test.py build/fortis_node_discovery_route_test [build/fortis_node]
Environment: R47_ONLY_GROUPS=a,b limits groups; R47_NATIVE_ONLY=<edge> runs one
native-only edge; R40_R47_FAIL_FAST=1 stops at
the first failure.
"""
import collections
import copy
import json
import os
import sys
import tempfile
import threading
import time
import price_markets_case_expectations as case_expectations
from pathlib import Path
from urllib.parse import parse_qs, quote, urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parent))
import discovery_route_harness as harness  # noqa: E402
import discovery_post_edges as edges  # noqa: E402
from discovery_post_edges import check, COUNTS  # noqa: E402

FIXTURE = "price_markets_edges.json"
ROUTE = "/v1/prices/markets"
MARKETS_PATH = "/api/v3/coins/markets"


def request_headers(case, item=None):
    source = item if item is not None else case
    headers = [("authorization", "Bearer " + edges.token(case["device"]))] if case["device"] else []
    return headers + [tuple(pair) for pair in source["requestHeaders"]]


def body_of(source):
    return None if source["bodyHex"] is None else bytes.fromhex(source["bodyHex"])


def ids_of(path):
    split = urlsplit(path)
    return parse_qs(split.query, keep_blank_values=True).get("ids", [""])[0].split(",")


class Provider:
    """The oracle's CoinGecko fake: ordered scenarios, then rows by id and
    generation (reset per process)."""

    def __init__(self, table):
        self.scenarios = table["scenarios"]
        self.rows = table["rows"]
        self.lock = threading.Lock()
        self.generations = collections.Counter()
        self.extra = []
        # The oracle's reply delay of the case being replayed (seconds).
        self.delay = 0.025

    def reset(self):
        with self.lock:
            self.generations.clear()

    def __call__(self, path, _headers):
        if urlsplit(path).path != MARKETS_PATH:
            return 404, {}, b'{"error":"unexpected"}'
        # As the oracle's fake: the reply (and each id's generation) is
        # decided on arrival, then delivered after the round trip.
        delay = self.delay
        ids = ids_of(path)
        for scenario_id, reply in self.scenarios:
            if scenario_id in ids:
                time.sleep(delay)
                return reply["status"], dict(reply.get("headers", {})), reply["text"].encode("utf-8", "surrogatepass")
        texts = []
        with self.lock:
            for coin in ids:
                self.generations[coin] += 1
                known = self.rows.get(coin, [])
                generation = self.generations[coin]
                if generation > len(known):
                    self.extra.append(f"{coin}#{generation}")
                text = known[min(generation, len(known)) - 1] if known else ""
                if text:
                    texts.append(text)
        body = ("[" + ",".join(texts) + "]").encode("utf-8", "surrogatepass")
        time.sleep(delay)
        return 200, {"content-type": "application/json"}, body


class Harness:
    def __init__(self, binary, root, fixture):
        self.binary = binary
        self.root = Path(root)
        self.fixture = fixture
        self.clock = self.root / "clock"
        self.provider = Provider(fixture["provider"])
        self.upstream = harness.Upstream(self.root, self.provider)
        self.lanes = edges.Lanes()
        self.quota = edges.Quota()
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
        check("fintel_dev_" not in text, f"{label}: a device token reached the response or output")


def identity_ok(h, label, observed):
    ok = True
    for path, headers in observed:
        sent = f"{path}{json.dumps(headers)}"
        for secret in h.client_values + ["fintel_dev_"]:
            ok &= check(secret not in sent, f"{label}: {secret[:20]} reached the provider")
        ok &= check(headers.get("accept") == "application/json" and
                    not any(k.startswith("x-cg-") for k in headers),
                    f"{label}: provider identity headers {headers}")
    return ok


def compare_upstream(h, label, case, observed):
    expected = sorted(edges.path_of(c["url"]) for c in case["upstream"])
    actual = sorted(path for path, _ in observed)
    ok = check(actual == expected, f"{label}: upstream\n   expected {expected[:6]}\n   actual   {actual[:6]}")
    return identity_ok(h, label, observed) and ok


def compare_ids_once(h, label, case, observed):
    """Every coin TypeScript fetched is fetched natively too, as often (the
    batches may group them differently)."""
    expected = collections.Counter(coin for c in case["upstream"] for coin in ids_of(edges.path_of(c["url"])))
    actual = collections.Counter(coin for path, _ in observed for coin in ids_of(path))
    ok = check(actual == expected, f"{label}: provider ids {sorted(actual.items())[:8]} != "
                                   f"{sorted(expected.items())[:8]}")
    return identity_ok(h, label, observed) and ok


def ascii_identifier_case(h, original):
    """js-case: exact rewrites of the frozen Unicode examples.

    Keep every unaffected JSON/wire byte. The capital dotted I and capital
    A-diaeresis no longer alias their Unicode lowercase rows. The former
    is fetched by its distinct lowercase spelling three fixture seconds later.
    """
    name = original['name']
    if original['group'] != 'main' or name not in ('unicode', 'unicode-protobuf', 'main-state-1'):
        return original
    case = copy.deepcopy(original)
    first = h.by_name[('main', 'unicode')]
    later = h.by_name[('main', 'unicode-protobuf')]
    source = first['upstream'][0]
    def upstream(ids):
        call = copy.deepcopy(source)
        prefix, rest = call['url'].split('&ids=', 1)
        _, suffix = rest.split('&', 1)
        call['url'] = prefix + '&ids=' + quote(','.join(ids), safe="-_.!~*'()") + '&' + suffix
        return [call]
    if name == 'unicode':
        case['responseText'] = case_expectations.json_without_markets(
            case['responseText'], {'i\u0307stanbul', 'ä-coin'})
        ids = json.loads(bytes.fromhex(case['bodyHex']))['ids']
        ids = {value.strip().translate(str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz'))
               for value in ids}
        case['upstream'] = upstream(sorted(ids, key=lambda value: value.encode('utf-16-be')))
        rows = {row['key']: row for row in case['kvWrites']}
        for old, new in [('i\u0307stanbul', 'İstanbul'), ('ä-coin', 'Ä-coin')]:
            row = rows.pop('mkt:usd:' + old)
            row.update(key='mkt:usd:' + new, d='{"market":null}')
            rows[row['key']] = row
        case['kvWrites'] = list(rows.values())
        length = len(case['responseText'].encode())
    elif name == 'unicode-protobuf':
        case['wireHex'] = case_expectations.proto_without_markets(
            bytes.fromhex(case['wireHex']), case['decoded']['markets'], {'ä-coin'}).hex()
        case['decoded']['markets'] = [row for row in case['decoded']['markets'] if row['id'] != 'ä-coin']
        case['decoded']['cache'].update(hit=False, partial=True)
        case['upstream'] = upstream(['i\u0307stanbul'])
        row = copy.deepcopy(next(row for row in first['kvWrites'] if row['key'] == 'mkt:usd:i\u0307stanbul'))
        for field in ('asOf', 'freshUntil', 'staleUntil'):
            row[field] += later['at'] - first['at']
        case['kvWrites'] = [row]
        length = len(bytes.fromhex(case['wireHex']))
    else:
        for row in case['envelopes']:
            if row['key'] == 'mkt:usd:i\u0307stanbul' and row['envelope']:
                for field in ('asOf', 'freshUntil', 'staleUntil'):
                    row['envelope'][field] += later['at'] - first['at']
        return case
    case['headers']['content-length'] = str(length)
    return case


def cost_of(case):
    try:
        return int(case["headers"].get("x-rate-limit-cost", "1"))
    except ValueError:
        return 1


# ---- asserted differences --------------------------------------------------------------------------

def iso(ms):
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(ms // 1000)) + ".%03dZ" % (ms % 1000)


def router_404(h, server, case, actual, observed):
    """Other methods (GET, HEAD, PUT), a trailing slash or another case: both
    the router's 404 before any hook; Fastify's body names the route, the
    native router answers its own {"error":"NOT_FOUND"} (router-wide, as
    R19–R25)."""
    ok = edges.native_plain_error(case["name"], actual, 404, edges.NATIVE_NOT_FOUND,
                                  head=case["method"] == "HEAD")
    return check(case["status"] == 404 and not observed,
                 f"{case['name']}: router fixture/provider mismatch {observed}") and ok


def json_reader_bounds(h, server, case, actual, observed):
    """A lone surrogate escape or a number overflowing a double in a JSON
    body: TypeScript reads them (the id fails encodeURIComponent and is
    answered pending; Infinity costs 1 and fails zod); the shared native
    bounded JSON reader refuses both as invalid JSON, 400
    {"error":"BAD_REQUEST"} after the quota (cost 1), no provider request
    (router-wide, as R46; the app's encoder never writes either)."""
    ok = edges.native_invalid_json(case["name"], actual, 1)
    return check(not observed, f"{case['name']}: provider requests {observed}") and ok


def batch_split(h, server, case, actual, observed):
    """batch_split: more than 50 cold coins in one request. TypeScript's
    window batcher sends them in one call (≤ 250 ids); natively they go in
    calls of 50 in id order (the 512 KiB provider body bound). Every coin is
    fetched exactly once, the rows are the same, and the answer, headers and
    durable entries are exactly TypeScript's."""
    status, headers, payload = actual
    ok = edges.compare(case["name"], case, status, headers, payload)
    ok &= compare_ids_once(h, case["name"], case, observed)
    sizes = [len(ids_of(path)) for path, _ in observed]
    total = sum(len(ids_of(edges.path_of(c["url"]))) for c in case["upstream"])
    want = [50] * (total // 50) + ([total % 50] if total % 50 else [])
    ok &= check(sorted(sizes) == sorted(want), f"{case['name']}: native batch sizes {sizes} != {want}")
    return ok and edges.compare_state(case["name"], server, case["kvWrites"])


def restart_durable_swr(h, server, case, actual, observed):
    """After the restart the `mkt:usd:` entries are in the durable discovery
    KV natively, so the answer is served from the first process's writes
    ({hit: true, timestamp: that write}) with no provider request, where
    TypeScript's new process (its production Kv is in memory) read cold and
    asked CoinGecko again ({hit: false, timestamp: null}). The markets are
    TypeScript's own; only `cache` differs, by exactly that."""
    status, headers, payload = actual
    before = h.by_name[("restart-a", "restart-before")]
    document = json.loads(case["responseText"], object_pairs_hook=collections.OrderedDict)
    document["cache"] = collections.OrderedDict([("hit", True), ("timestamp", iso(before["at"]))])
    want = json.dumps(document, separators=(",", ":"), ensure_ascii=False).encode()
    ok = check(status == 200 == case["status"] and payload == want,
               f"{case['name']}: not TypeScript's answer read from the durable entries "
               f"{edges.first_difference(want, payload)}")
    ok &= edges.compare(case["name"], case, status, headers, payload, skip=("content-length", "body"))
    ok &= check(headers.get("content-length") == str(len(want)), f"{case['name']}: content-length")
    ok &= check(not observed, f"{case['name']}: provider requests {observed}")
    return ok and edges.compare_state(case["name"], server, before["kvWrites"])


def window_batching(h, server, case, results, observed):
    """window_batching: cold coins of simultaneous requests. TypeScript's
    20 ms window batcher merges them into one /coins/markets call; natively
    each request batches its own cold coins and joins coins already in
    flight. Every coin is fetched exactly as often, and every answer,
    header and durable entry is exactly TypeScript's."""
    ok = True
    for index, (record, result) in enumerate(zip(case["responses"], results)):
        ok &= edges.compare(f"{case['name']}[{index}]", record, *result)
    ok &= compare_ids_once(h, case["name"], case, observed)
    return ok and edges.compare_state(case["name"], server, case["kvWrites"])


def ascii_case_ids(h, server, case, actual, observed):
    """js-case: ids are ASCII lower-cased, so 128 x U+0130 stays
    as written (TypeScript lowered it to i + U+0307, past the 100-unit id
    bound of a provider row it then served). Native answers 200 and never
    the Unicode-lowered id (the provider has no coin under the id as sent)."""
    status, headers, payload = actual
    try:
        document = json.loads(payload)
    except ValueError:
        return check(False, f"{case['name']}: not JSON {payload[:120]!r}")
    ids = [m["id"] for m in document.get("markets", [])] + list(document.get("pending", []))
    return check(status == 200 and case["status"] == 200 and
                 all(not any("A" <= c <= "Z" for c in i) and "i\u0307" not in i for i in ids),
                 f"{case['name']}: native {status} {payload[:160]!r}")


ASSERTED = {
    **{name: router_404 for name in ("router-get", "router-put", "router-head", "router-trailing-slash",
                                     "router-upper")},
    "id-lower-grows": ascii_case_ids,
    "number-overflow-cost": json_reader_bounds,
    "ids-100-cold": batch_split,
    "restart-after": restart_durable_swr,
}
CONCURRENT_ASSERTED = {"concurrent-overlap": window_batching, "concurrent-disjoint": window_batching}
STATE_SOURCES = {"restart-state": ("restart-a", "restart-a-state")}
# js-case: "İstanbul" in `unicode` is no longer lowered to the
# "i̇stanbul" that `unicode-protobuf` asks for, so that id is not cached and
# native asks the provider once there: the lane budget must cover the call
# and the replay waits for it to settle before the next case.
NATIVE_CALLS = {"ids-100-cold": 2, "concurrent-overlap": 3, "concurrent-disjoint": 4, "unicode-protobuf": 1}
NATIVE_ONLY_CALLS = {"unicode-protobuf": [{"url": "https://api.coingecko.com/api/v3/coins/markets?js-case"}]}


# ---- the replay ----------------------------------------------------------------------------------

def delay_of(h, case):
    delays = [call.get("delayMs", 25) for call in case.get("upstream", [])]
    h.provider.delay = (max(delays) if delays else 300 if case["group"] == "concurrent" else 25) / 1000


def trace(case, observed):
    if os.environ.get("R47_TRACE") == "1":
        print(f"  [{case['group']}] {case['name']}: {[path.split('ids=')[1].split('&')[0][:60] for path, _ in observed]}",
              flush=True)


def replay_case(h, server, case):
    case = ascii_identifier_case(h, case)
    name = case["name"]
    delay_of(h, case)
    harness.set_clock(h.clock, case["at"])
    h.lanes.reserve(case["upstream"] or NATIVE_ONLY_CALLS.get(name, []), NATIVE_CALLS.get(name, 0))
    h.quota.reserve(case["device"], cost_of(case))
    before = len(h.upstream.calls())
    expected_calls = NATIVE_CALLS.get(name, len(case["upstream"]))
    status, headers, payload = edges.raw(server.port, case["method"], case["target"], request_headers(case),
                                         body_of(case))
    observed = edges.settle(h.upstream, before, expected_calls, grace=edges.grace_for(case["upstream"]))
    trace(case, observed)
    h.lanes.breaker(case["upstream"])
    h.no_secret(payload, name)
    if name in ASSERTED:
        return ASSERTED[name](h, server, case, (status, headers, payload), observed)
    remaining = case["group"] == "quota"
    if edges.negative_zero_l1(case, status, payload):
        ok = edges.compare(name, case, status, headers, payload, remaining=remaining, skip=("body",))
        ok &= compare_upstream(h, name, case, observed)
        ok &= edges.compare_state(name, server, case["kvWrites"])
        return "asserted" if ok else False
    ok = edges.compare(name, case, status, headers, payload, remaining=remaining)
    ok &= compare_upstream(h, name, case, observed)
    ok &= edges.compare_state(name, server, case["kvWrites"])
    return "asserted" if ok and name in ("unicode", "unicode-protobuf") else ok


def replay_concurrent(h, server, case):
    name = case["name"]
    delay_of(h, case)
    harness.set_clock(h.clock, case["at"])
    h.lanes.reserve(case["upstream"], NATIVE_CALLS.get(name, 0))
    h.quota.reserve(case["device"], len(case["concurrent"]) * 10)
    before = len(h.upstream.calls())
    results = [None] * len(case["concurrent"])
    barrier = threading.Barrier(len(results))

    def run(index, item):
        barrier.wait()
        time.sleep(index * case.get("staggerMs", 0) / 1000)
        results[index] = edges.raw(server.port, "POST", ROUTE, request_headers(case, item), body_of(item))

    threads = [threading.Thread(target=run, args=(i, item)) for i, item in enumerate(case["concurrent"])]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    observed = edges.settle(h.upstream, before, NATIVE_CALLS.get(name, len(case["upstream"])),
                           grace=edges.grace_for(case["upstream"], 0.5))
    trace(case, observed)
    h.lanes.breaker(case["upstream"])
    for result in results:
        h.no_secret(result[2], name)
    if name in CONCURRENT_ASSERTED:
        return CONCURRENT_ASSERTED[name](h, server, case, results, observed)
    ok = True
    for index, (record, result) in enumerate(zip(case["responses"], results)):
        ok &= edges.compare(f"{name}[{index}]", record, *result)
    ok &= compare_upstream(h, name, case, observed)
    ok &= edges.compare_state(name, server, case["kvWrites"])
    return ok


def replay_state(h, server, case):
    case = ascii_identifier_case(h, case)
    if case["name"] in STATE_SOURCES:
        before = h.by_name[STATE_SOURCES[case["name"]]]
        check(before["state"] == case["state"] and before["media"] == case["media"],
              f"{case['name']}: the restart state rule needs the same keys and media")
        case = dict(case, envelopes=before["envelopes"])
    harness.set_clock(h.clock, case["at"])
    actual = server.state(case["state"]["keys"], case["state"]["logos"])
    if not check(actual is not None, f"{case['name']}: state probe failed"):
        return False
    ok = check(len(case["envelopes"]) == len(actual["envelopes"]) and len(case["media"]) == len(actual["media"]),
               f"{case['name']}: state response lost envelope/media rows")
    for want, got in zip(case["envelopes"], actual["envelopes"]):
        ok &= check(want == got, f"{case['name']}: envelope {want['key']!r}\n   expected {json.dumps(want)[:700]}\n"
                                 f"   actual   {json.dumps(got)[:700]}")
    for want, got in zip(case["media"], actual["media"]):
        ok &= check(want == got, f"{case['name']}: media {want['sourceUrl']}\n   expected {json.dumps(want)}\n"
                                 f"   actual   {json.dumps(got)}")
    return ok


# ---- native-only edges ------------------------------------------------------------------------------

def post(server, letter, body, accept="application/json", content_type="application/json"):
    headers = [("authorization", "Bearer " + edges.token(letter)), ("content-type", content_type),
               ("accept", accept)]
    return edges.raw(server.port, "POST", ROUTE, headers, body)


def restart_check(h, at):
    """SIGKILL durability: cold coins' entries (and their logo media records)
    survive a hard kill; the next process answers them from the KV (hit, the
    first write's timestamp) without provider work."""
    h.provider.reset()
    server = h.server("native-restart", {}, at)
    h.quota.reserve("N", 2)
    h.lanes.reserve([{"url": "https://api.coingecko.com/x"}])
    first = post(server, "N", b'{"ids":["alpha","beta"]}')
    server.stop(kill=True)
    server = h.server("native-restart", {}, at + 1000)
    before = len(h.upstream.calls())
    again = post(server, "N", b'{"ids":["beta","alpha"]}')
    logo = json.loads(h.provider.rows["alpha"][0])["image"]
    media = server.state([], [logo])
    after = h.upstream.calls()[before:]
    server.stop()
    want = first[2].replace(b'"cache":{"hit":false,"timestamp":null}',
                            b'"cache":{"hit":true,"timestamp":"' + iso(at).encode() + b'"}')
    ok = check(first[0] == 200 and again[0] == 200 and again[2] == want and not after and
               media and media["media"][0]["record"] is not None,
               f"restart: {first[0]} {again[0]} {again[2][:160]!r} calls {after} media {media}")
    print(f"  restart: SIGKILL kept 2 entries and the logo record; {len(again[2])} B answer, 0 provider calls",
          flush=True)
    return ok


def deferred_bound(h, at):
    """Nine simultaneous requests with cold coins while the provider is
    slow: eight wait on their batches; the ninth (the synchronous slot) is
    answered at once with its cold coin pending; every request is answered;
    the server then serves the stored coins from cache."""
    slow = threading.Event()

    def provider(path, headers):
        time.sleep(0.8)
        return h.provider(path, headers)

    h.provider.reset()
    original = h.upstream.respond
    h.upstream.respond = provider
    try:
        server = h.server("native-deferred", {}, at)
        letters = "EFGIJKLMN"
        for letter in letters:
            h.quota.reserve(letter, 1)
        time.sleep(11)  # a full CoinGecko burst
        results = [None] * 9
        started = [0.0] * 9
        barrier = threading.Barrier(9)

        def run(index):
            barrier.wait()
            time.sleep(index * 0.05)
            started[index] = time.monotonic()
            results[index] = (post(server, letters[index], json.dumps({"ids": [f"bound-{index}"]}).encode()),
                              time.monotonic() - started[index])

        threads = [threading.Thread(target=run, args=(i,)) for i in range(9)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        server.stop()
    finally:
        h.upstream.respond = original
        slow.set()
    answers = [json.loads(r[0][2]) for r in results]
    immediate = [i for i, a in enumerate(answers) if a.get("pending") == [f"bound-{i}"] and results[i][1] < 0.3]
    # The bound-* coins are unlisted: a request that waited for its batch
    # answers with neither a market nor a pending id.
    served = [i for i, a in enumerate(answers) if a.get("pending") == [] and results[i][1] >= 0.5]
    ok = check(all(r[0][0] == 200 for r in results) and len(immediate) == 1 and len(served) >= 2,
               f"deferred-bound: statuses {[r[0][0] for r in results]}, immediate {immediate}, served {served}, "
               f"times {[round(r[1], 2) for r in results]}")
    print(f"  deferred-bound: 9 simultaneous cold requests → request {immediate} answered at once with its coin "
          f"pending (the synchronous slot); {len(served)} answered after their batch; the rest pending after the "
          f"lane's 3 s queue timeout (times {[round(r[1], 2) for r in results]})", flush=True)
    return ok


def sustained(h, server, at):
    """96 reads of cached coins from 12 clients (JSON and protobuf) give one
    byte string per representation and reach no provider."""
    harness.set_clock(h.clock, at)
    body = json.dumps({"ids": ["sustained-a", "sustained-b"]}).encode()
    letters = "EFGIJKLMNOPR"
    h.quota.reserve("Q", 1)
    h.lanes.reserve([{"url": "https://api.coingecko.com/x"}])
    first = post(server, "Q", body)
    time.sleep(0.5)
    before = len(h.upstream.calls())
    results = collections.defaultdict(set)
    statuses = collections.Counter()
    lock = threading.Lock()
    for letter in letters:
        h.quota.reserve(letter, 8)

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
    only = [g for g in os.environ.get("R47_ONLY_GROUPS", "").split(",") if g]
    # R47_NATIVE_ONLY=<edge> runs one native-only edge and no fixture group.
    native_only = os.environ.get("R47_NATIVE_ONLY", "")
    if native_only:
        only = ["(none)"]
    with tempfile.TemporaryDirectory(prefix="fortis-r47-") as root:
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
                h.provider.reset()
                server = h.server(group["kv"], group["env"], group["start"])
                servers[group["kv"]] = server
                if name == "quota":
                    first = next(c for c in selected if c["name"] == "quota-cost-10")
                    edges.drain_quota(server, ROUTE, "D", int(first["headers"]["x-rate-limit-remaining"]) + 10,
                                      body=json.dumps({"ids": [7] * 100}).encode())
                for case in selected:
                    if "state" in case:
                        ok = replay_state(h, server, case)
                        kind = "asserted" if case["name"] in STATE_SOURCES or case["name"] == "main-state-1" else "matched"
                    elif "concurrent" in case:
                        ok = replay_concurrent(h, server, case)
                        kind = "asserted" if case["name"] in CONCURRENT_ASSERTED else "matched"
                    else:
                        ok = replay_case(h, server, case)
                        kind = "asserted" if case["name"] in ASSERTED or ok == "asserted" else "matched"
                    COUNTS[(name, kind if ok else "failed")] += 1
                if name == "main":
                    COUNTS[("sustained", "matched" if sustained(h, server, selected[-1]["at"] + 1000)
                            else "failed")] += 1
                if group["kv"] != "restart" or name == "restart-b":
                    server.stop()
                    h.no_secret(server.text(), f"{name} process output")
                    servers.pop(group["kv"], None)
            if not only or native_only:
                at = fixture["cases"][0]["at"] + 20 * 3_600_000
                if native_only in ("", "restart"):
                    COUNTS[("restart", "matched" if restart_check(h, at) else "failed")] += 1
                if native_only in ("", "deferred-bound"):
                    COUNTS[("deferred-bound", "matched" if deferred_bound(h, at + 60_000) else "failed")] += 1
        finally:
            for server in servers.values():
                server.stop()
            h.upstream.close()
        if len(sys.argv) == 3 and not only:
            json_type = ("content-type", "application/json")
            body = b'{"ids":["bitcoin"]}'
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
        if h.provider.extra:
            print("rows beyond TypeScript's generations:", sorted(set(h.provider.extra))[:20])
    edges.summary("price markets edges")


if __name__ == "__main__":
    main()
