#!/usr/bin/env python3
"""Real-socket edges of GET /v1/networks/{network}/token/{tokenId}/page (R24)
against tests/corpus/token_page_edges.json, which
the earlier TypeScript implementation's tests recorded from the
real Fastify app over real sockets. The fixture's SHA-256 is checked against
its pinned .sha256 file before anything runs, and a modified copy is refused
(self-checked).

build/fortis_node_token_route_test sends every token, spot and price
history provider request to one loopback HTTPS upstream naming its real host
in x-fortis-test-host; per case the upstream answers the exact bytes
TypeScript was served (tests/token360_edges_test.py's CaseTable). Each case's
frozen instant goes to the token clock file, which the price history
backfill owner also reads (FORTIS_TEST_HISTORY_NOW_FILE). Each fixture group
is a fresh process with its own KV directory; restart-b reuses restart-a's
after a SIGKILL.

Per request record: the status, every recorded header (x-rate-limit-remaining
only in the quota group), the exact JSON text or protobuf bytes, the multiset
of provider requests (background revalidations included, collected after
/probe/test-token-idle; /coins/markets compared per coin id) and each
provider request's identity headers; no client header or device credential
reaches a provider, each provider key only its own host, and no device
token/id reaches a response or the process output. `state` records compare
the SWR envelopes, token_facts rows and registry identity fields through POST
/probe/test-token-state. Concurrent records compare every answer.

Each device keeps under its per-minute quota on the native wall clock (the
oracle's quota ran on its frozen Date): a request that would be the device's
41st in the current minute waits for the next minute.

Asserted differences are listed in ASSERTED with the reason in each rule's
docstring, and named beside handle_page in src/tokens/token_task_routes.cpp.

Native-only: `size` (the largest page's JSON, protobuf and HEAD lengths agree
and stay under the 2 MiB response bound), `sustained` (96 reads of cached
pages from 12 clients are byte-identical per kind and reach no provider) and,
with a second argument, the production composition (the route answers 404
unless FORTIS_NATIVE_PRICE_ROUTE_CANARY=1; then 401, 403, 414 and the
raw-byte 400 before any facet work).

Usage: token_page_edges_test.py build/fortis_node_token_route_test [build/fortis_node]
Environment: R24_ONLY_GROUPS=a,b limits groups; R24_FAIL_FAST=1 stops at the
first failure.
"""
import collections
import hashlib
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from urllib.parse import urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parent))
import discovery_route_harness as harness  # noqa: E402
import compat_expectations  # noqa: E402
import tls_ca  # noqa: E402
import token360_edges_test as base  # noqa: E402
import token_history_edges_test as history  # noqa: E402

FIXTURE = "token_page_edges.json"
COUNTS = collections.Counter()
FAIL_FAST = os.environ.get("R24_FAIL_FAST") == "1"
base.FAIL_FAST = FAIL_FAST
check = base.check
RESPONSE_BOUND = 2 * 1024 * 1024


def load_fixture(directory=None):
    directory = Path(directory or harness.FIXTURES)
    data = (directory / FIXTURE).read_bytes()
    pinned = (directory / f"{FIXTURE}.sha256").read_text().split()[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{FIXTURE}: sha256 {digest} does not match the pinned {pinned}")
    print(f"{FIXTURE}: sha256 {digest} matches the pin", flush=True)
    return json.loads(data)


def pin_self_check():
    """A copy with one changed byte must be refused by load_fixture."""
    with tempfile.TemporaryDirectory(prefix="fortis-r24-pin-") as root:
        shutil.copy(harness.FIXTURES / f"{FIXTURE}.sha256", Path(root) / f"{FIXTURE}.sha256")
        data = bytearray((harness.FIXTURES / FIXTURE).read_bytes())
        data[len(data) // 2] ^= 1
        (Path(root) / FIXTURE).write_bytes(bytes(data))
        try:
            load_fixture(root)
        except SystemExit as refused:
            print(f"  pin self-check: a modified copy is refused ({str(refused)[:60]}…)", flush=True)
            return True
    return check(False, "a modified fixture copy was accepted")


# ---- asserted differences --------------------------------------------------------------------------

def parser_baseline(h, case, actual, observed):
    """The routing 414 (FST_ERR_MAX_PARAM_LENGTH) and Node's raw-byte "Client
    Error": both answer before any hook with the same status; the body is
    this server's {"error":...} (fastify-errors);
    TypeScript's come from the router / clientError handler without the
    onSend security headers, the shared native writer adds its baseline
    (router-wide, as R13–R23)."""
    status, headers, payload = actual
    # fastify-errors: both are {"error":...} as application/json;
    # charset=utf-8 now (URI_TOO_LONG, BAD_REQUEST), still before any hook.
    want = compat_expectations.fastify_case(dict(case, headers=dict(case["headers"], **base.NATIVE_BASELINE)))
    return base.compare(case["name"], want, status, headers, payload, remaining=case["group"] == "quota") and \
        check(not observed, f"{case['name']}: provider requests {observed}")


def bad_url(h, case, actual, observed):
    """A malformed or non-UTF-8 path escape: both 400 before any hook;
    Fastify sends its FST_ERR_BAD_URL object, the native request parser
    {"error":"BAD_REQUEST"} (the shared parser, as R13–R23)."""
    ok = json.loads(case["responseText"]).get("code") == "FST_ERR_BAD_URL"
    return check(ok and actual[0] == 400 == case["status"] and actual[2] == b'{"error":"BAD_REQUEST"}' and
                 not observed, f"{case['name']}: native {actual[0]} {actual[2][:120]!r}")


def router_404(h, case, actual, observed):
    """Other methods and spellings: both the router's 404; Fastify's body
    names the route, the native router answers its own (status only, as
    R19–R23)."""
    return check(actual[0] == case["status"] == 404 and not observed, f"{case['name']}: router status {actual[0]}")


def head_304(h, case, actual, observed):
    """HEAD with a matching If-None-Match: both 304 with the same ETag,
    cache-control and rate-limit headers and no body; Node adds
    content-length: 0 to that HEAD 304, the shared native writer sends none
    (router-wide, as R07, R18 and R20–R23)."""
    status, headers, payload = actual
    want = dict(case, headers={k: v for k, v in case["headers"].items() if k != "content-length"})
    return check(case["headers"].get("content-length") == "0" and "content-length" not in headers,
                 f"{case['name']}: content-length {headers.get('content-length')!r}") and \
        base.compare(case["name"], want, status, headers, payload) and \
        check(not observed, f"{case['name']}: provider requests {observed}")


def iso(ms):
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(ms // 1000)) + ".%03dZ" % (ms % 1000)


def parse_iso(text):
    import calendar
    return calendar.timegm(time.strptime(text[:19], "%Y-%m-%dT%H:%M:%S")) * 1000 + int(text[20:23])


def decoded(record):
    """TypeScript's answer as an ordered document (JSON text or Struct wire)."""
    if "wireHex" in record:
        return base.decode_wire(bytes.fromhex(record["wireHex"])), True
    return json.loads(record["responseText"], object_pairs_hook=collections.OrderedDict), False


def encoded(document, protobuf):
    if protobuf:
        return base.struct_wire(document)
    return json.dumps(document, separators=(",", ":"), ensure_ascii=False).encode("utf-8", "surrogatepass")


def as_cached(document, written_ms, now_ms):
    """The page as read back from entries written at `written_ms`: every
    section TypeScript composed live (hit false) is a hit with that
    timestamp; `asOf` is again the oldest section (oldestSectionAsOf)."""
    out = json.loads(json.dumps(document), object_pairs_hook=collections.OrderedDict)
    for name, block in out["cache"].items():
        if not block.get("hit"):
            out["cache"][name] = collections.OrderedDict([("hit", True), ("timestamp", iso(written_ms))])
    oldest = now_ms
    for block in out["cache"].values():
        at = parse_iso(block["timestamp"]) if block.get("timestamp") else now_ms
        oldest = min(oldest, at)
    out["asOf"] = iso(oldest)
    return out


def same_headers(label, record, headers, payload, skip=("x-rate-limit-remaining", "content-length", "etag")):
    ok = True
    for name in base.HEADERS:
        if name in skip:
            continue
        ok &= check(headers.get(name) == record["headers"].get(name), f"{label}: header {name}")
    if payload:
        ok &= check(headers.get("content-length") == str(len(payload)), f"{label}: content-length")
    return ok


def weak_etag(payload, protobuf):
    return f'W/"token-page{"-protobuf" if protobuf else ""}-{hashlib.sha256(payload).hexdigest()[:24]}"'


# The facts sub-facets TokenPageFacet.get reads for a section set (its
# `only`), and the TypeScript combineCacheProvenance.
def page_facets(included):
    sections = set(included) | {"identity"}
    facets = {"chainMeta"}
    if sections & {"market", "quote", "safety", "comparison"}:
        facets.add("market")
    if "profile" in sections:
        facets.add("profile")
    if sections & {"liquidity", "quote", "safety"}:
        facets.add("liquidity")
    if sections & {"holders", "safety"}:
        facets.add("holders")
    for name in ("insight", "contract"):
        if name in sections:
            facets.add(name)
    return sorted(facets)


def combine(entries):
    cached = [entry for entry in entries if entry["hit"]]
    stamps = [entry["timestamp"] for entry in cached if entry.get("timestamp")]
    out = collections.OrderedDict([("hit", len(cached) == len(entries)), ("timestamp", max(stamps) if stamps else None)])
    if 0 < len(cached) < len(entries):
        out["partial"] = True
    return out


def follower_forms(document, at_ms):
    """TypeScript's page with every subset of its sub-facets read from the
    entry a concurrent sibling just wrote ({hit: true, timestamp: the write})
    instead of live, the composed blocks (identity, safety, a market-sourced
    quote) recombined as TokenPageFacet.get does and asOf recomputed."""
    import itertools
    stamp = iso(at_ms)
    included = document["included"]
    sections = set(included)
    facets = page_facets(included)
    live = {name: collections.OrderedDict([("hit", False), ("timestamp", stamp)]) for name in facets}
    for name in facets:
        if name in document["cache"]:
            live[name] = document["cache"][name]
    forms = []
    for size in range(1, len(facets) + 1):
        for followed in itertools.combinations(facets, size):
            prov = {name: collections.OrderedDict([("hit", True), ("timestamp", stamp)]) if name in followed
                    else live[name] for name in facets}
            out = json.loads(json.dumps(document), object_pairs_hook=collections.OrderedDict)
            cache = out["cache"]
            cache["identity"] = combine([prov["chainMeta"]] + ([prov["market"]] if sections & {"market", "quote"}
                                                               else []))
            quote = out.get("quote")
            if "quote" in cache and isinstance(quote, dict) and quote.get("source") == "market":
                cache["quote"] = prov["market"]
            for name in ("market", "profile", "liquidity", "holders", "contract", "insight"):
                if name in cache:
                    cache[name] = prov[name]
            if "safety" in cache:
                cache["safety"] = combine([prov["market"], prov["liquidity"], prov["holders"]])
            oldest = at_ms
            for block in cache.values():
                if block.get("timestamp"):
                    oldest = min(oldest, parse_iso(block["timestamp"]))
            out["asOf"] = iso(oldest)
            forms.append(out)
    return forms


def late_follower(h, case, actual, observed):
    """Simultaneous cold reads: TypeScript's SWR single flight hands every
    reader the one live read (every composed section hit: false). The native
    service composes on two foreground workers; a request queued behind them
    reads, per sub-facet, either live or the entry a sibling has just written
    (hit: true at the write, the case instant) — R19's late_follower seen
    through the page's provenance, where one page can mix both (its identity
    and safety blocks then combine a hit and a miss: partial). Each answer is
    TypeScript's bytes or one such form, with its own weak ETag. Provider
    requests: TypeScript's plus at most one repeat of each — the token owner
    coalesces a GET only while it is in flight, so a queued worker whose read
    misses just after a sibling's flight completed, but before that sibling's
    SWR write, fetches again (the multicall eth_call POST is never shared).
    This is a known open follow-up (concurrent cold
    reads compose in two workers), not a page defect."""
    ok = True
    late = 0
    for index, (item, record, (status, headers, payload)) in enumerate(zip(case["concurrent"], case["responses"],
                                                                         actual)):
        label = f"{case['name']}[{index}]"
        head = item["kind"] == "head"
        source = record if not head else next(r for i, r in zip(case["concurrent"], case["responses"])
                                              if i.get("target") == item.get("target") and i["kind"] == "json")
        document, protobuf = decoded(source)
        followers = [encoded(form, protobuf) for form in follower_forms(document, case["at"])]
        exact_body = base.expected_body(source)
        if head:
            length = headers.get("content-length")
            exact = length == record["headers"].get("content-length") and \
                headers.get("etag") == record["headers"].get("etag")
            follows = any(length == str(len(form)) and headers.get("etag") == weak_etag(form, protobuf)
                          for form in followers)
        else:
            exact = payload == exact_body and headers.get("etag") == record["headers"].get("etag")
            follows = any(payload == form and form != exact_body and headers.get("etag") == weak_etag(form, protobuf)
                          for form in followers)
        late += follows and not exact
        ok &= check(status == record["status"] == 200 and (exact or follows),
                    f"{label}: neither TypeScript's answer nor a follower form; against TypeScript "
                    f"{base.first_difference(exact_body, payload)}")
        ok &= same_headers(label, record, headers, payload)
    print(f"  {case['name']}: {late} late followers of {len(actual)}", flush=True)
    expected = collections.Counter(base.request_key(c["method"], c["url"], c.get("requestBody") or "")
                                   for c in case["upstream"])
    actual_keys = collections.Counter(base.request_key(m, f"https://{host}{target}", b)
                                      for m, host, target, b, _ in observed)
    extra = actual_keys - expected
    ok &= check(all(expected[key] and count == 1 for key, count in extra.items()),
                f"{case['name']}: provider requests beyond one repeat of TypeScript's {list(extra)}")
    if extra:
        print(f"  {case['name']}: repeated provider requests {sorted(k[:90] for k in extra)}", flush=True)
    repeats = [dict(call) for call in case["upstream"]
               if base.request_key(call["method"], call["url"], call.get("requestBody") or "") in extra]
    return base.compare_upstream(h, case["name"], dict(case, upstream=case["upstream"] + repeats), observed,
                                 case["group"]) and ok


def restart_durable_swr(h, case, actual, observed):
    """After the restart the SWR entries are in the durable KV natively, so
    the page is served from them (each section a hit at the first process's
    write, `asOf` that write) with no provider request, where TypeScript's
    new process (an in-memory Kv) composed every section live (or, for the
    series, read the durable candles) and asked its providers again. Every
    value is TypeScript's own answer of this case; only provenance, asOf and
    the identity's coingeckoIdSource (the durable `tref:` resolution from
    before the restart) differ, by exactly that."""
    status, headers, payload = actual
    before = h.by_name[(case["group"].replace("restart-b", "restart-a"), RESTART_SOURCES[case["name"]])]
    document, protobuf = decoded(case)
    cached = as_cached(document, before["at"], case["at"])
    # The token resolution (`tref:`) is one of those durable entries: the
    # identity keeps the coin id's source from before the restart, where
    # TypeScript's new process re-read its durable registry row ("registry").
    earlier, _ = decoded(before)
    if isinstance(cached.get("identity"), dict) and isinstance(earlier.get("identity"), dict):
        cached["identity"]["coingeckoIdSource"] = earlier["identity"].get("coingeckoIdSource")
    want = encoded(cached, protobuf)
    ok = check(status == 200 and payload == want and want != base.expected_body(case),
               f"{case['name']}: native is not TypeScript's page read from the durable entries "
               f"{base.first_difference(want, payload)}")
    ok &= check(headers.get("etag") == weak_etag(want, protobuf), f"{case['name']}: etag")
    ok &= same_headers(case["name"], case, headers, payload)
    return check(not observed, f"{case['name']}: provider requests {observed}") and ok


def errors_order_page(h, case, actual, observed):
    """A cold read with every provider down: TypeScript's `errors` members
    follow promise settlement, native's facet order (R19's errors_order, the
    known token_routes difference). Equal bytes once the members are in
    TypeScript's order, every header equal except the weak ETag, which hashes
    the page body and so is the native body's own; provider requests exact."""
    status, headers, payload = actual
    reordered = base.reorder_errors(payload, case["responseText"])
    ok = check(status == case["status"] == 200 and reordered == base.expected_body(case) and payload != reordered,
               f"{case['name']}: body differs beyond the errors order {payload[:200]!r}")
    ok &= check(headers.get("etag") == weak_etag(payload, False), f"{case['name']}: etag {headers.get('etag')}")
    ok &= same_headers(case["name"], case, headers, payload)
    return base.compare_upstream(h, case["name"], case, observed, case["group"]) and ok


RESTART_SOURCES = {"restart-after-fresh": "restart-before", "restart-after-series": "restart-before-series"}


def restart_floor_asof(h, case, actual, observed):
    """Eight days on with every provider down both answer the durable
    token_facts floor. TypeScript's floor was rewritten by its live re-read
    after the restart (restart-after-fresh); natively that read was served
    from the durable SWR (restart_durable_swr), so the floor still carries
    the first process's write. Equal bytes once that one instant is mapped
    back; provider requests exact."""
    status, headers, payload = actual
    before = h.by_name[("restart-a", "restart-before")]
    after = h.by_name[("restart-b", "restart-after-fresh")]
    want = base.expected_body(case).replace(iso(after["at"]).encode(), iso(before["at"]).encode())
    ok = check(status == case["status"] == 200 and payload == want and want != base.expected_body(case),
               f"{case['name']}: {base.first_difference(want, payload)}")
    ok &= check(headers.get("etag") == weak_etag(want, "wireHex" in case), f"{case['name']}: etag")
    ok &= same_headers(case["name"], case, headers, payload)
    return base.compare_upstream(h, case["name"], case, observed, case["group"]) and ok


ASSERTED = {
    **{name: parser_baseline for name in (
        "no-auth-long-network", "network-101", "network-escaped-101", "network-astral-51", "ref-bang-101",
        "ref-astral-51", "network-raw-utf8", "ref-raw-utf8", "quota-over-long")},
    **{name: bad_url for name in ("network-bad-escape", "network-invalid-utf8", "ref-bad-escape", "ref-invalid-utf8")},
    **{name: router_404 for name in ("router-post", "router-put", "router-trailing-slash", "router-upper-path",
                                     "router-extra-segment", "router-semicolon")},
    "inm-match-head": head_304,
    "swr-cold-down": errors_order_page,
    "concurrent-cold-one": late_follower,
    "concurrent-sections": late_follower,
    "restart-after-fresh": restart_durable_swr,
    "restart-after-series": restart_durable_swr,
    "restart-late-floor": restart_floor_asof,
}


# ---- the replay ----------------------------------------------------------------------------------

def reserve_lanes(h, calls):
    """The native lanes' budgets, in real time (tests/token360_edges_test.py
    Lanes). A background revalidation (a call TypeScript made after its
    answer) is admitted natively only when its lane can dispatch at once with
    nothing queued or in flight (ProviderLane background admission), where
    TypeScript's frozen-Date lane always had room: before such a case the lane
    is refilled to its whole burst."""
    padded = list(calls)
    for call in calls:
        if not call.get("afterResponse"):
            continue
        lane = base.lane_for(urlsplit(call["url"]).netloc)
        _, _, burst = base.LANES.get(lane, base.RPC_LANE)
        same = sum(1 for other in calls if base.lane_for(urlsplit(other["url"]).netloc) == lane)
        padded += [call] * max(0, burst - same)
    h.lanes.reserve(padded)


class Harness(base.Harness):
    def __init__(self, binary, root, fixture):
        super().__init__(binary, root, fixture)
        self.by_name = {(c["group"], c["name"]): c for c in fixture["cases"]}

    def server(self, kv_name, env, start):
        kv_dir = self.root / f"kv-{kv_name}"
        kv_dir.mkdir(exist_ok=True)
        harness.set_clock(self.clock, start)
        return base.Server(self.binary, kv_dir, self.clock, self.upstream, start,
                           dict(env, FORTIS_TEST_HISTORY_NOW_FILE=str(self.clock),
                                FORTIS_TEST_PRICE_NOW_FILE=str(self.clock)))


def replay_case(h, server, case):
    name = case["name"]
    harness.set_clock(h.clock, case["at"])
    h.table.start_case(case["upstream"], case.get("mode"))
    reserve_lanes(h, case["upstream"])
    before = len(h.upstream.calls())
    target = bytes.fromhex(case["targetHex"]).decode("latin1")
    history.QUOTA.reserve(case["device"])
    status, headers, payload = base.raw(server.port, case["method"], target, base.request_headers(case))
    observed = base.settle(server, h.upstream, before, len(case["upstream"]))
    h.lanes.breaker(case["upstream"])
    h.no_secret(payload, name)
    h.no_secret(json.dumps(headers), name)
    if compat_expectations.malformed_path_target(case):
        # opaque-params: an empty, control-character or over-long
        # path value is the router's 404 before any hook.
        return check(compat_expectations.router_refused(status, headers, payload, observed),
                     f"{name}: malformed path value is not the router's 404: {status} {payload[:120]!r}")
    if name in ASSERTED:
        return ASSERTED[name](h, case, (status, headers, payload), observed)
    ok = base.compare(name, case, status, headers, payload, remaining=case["group"] == "quota")
    ok &= base.compare_upstream(h, name, case, observed, case["group"])
    return ok


def replay_concurrent(h, server, case):
    harness.set_clock(h.clock, case["at"])
    h.table.start_case(case["upstream"], case.get("mode"))
    reserve_lanes(h, case["upstream"])
    history.QUOTA.reserve(case["device"], len(case["concurrent"]))
    before = len(h.upstream.calls())
    results = [None] * len(case["concurrent"])
    barrier = threading.Barrier(len(results))

    def run(index, item):
        target = item.get("target") or bytes.fromhex(case["targetHex"]).decode("latin1")
        barrier.wait()
        results[index] = base.raw(server.port, "HEAD" if item["kind"] == "head" else "GET", target,
                                  base.request_headers(case, item["kind"]))

    threads = [threading.Thread(target=run, args=(i, item)) for i, item in enumerate(case["concurrent"])]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    observed = base.settle(server, h.upstream, before, len(case["upstream"]))
    for result in results:
        h.no_secret(result[2], case["name"])
    if case["name"] in ASSERTED:
        return ASSERTED[case["name"]](h, case, results, observed)
    ok = True
    for index, (record, result) in enumerate(zip(case["responses"], results)):
        ok &= base.compare(f"{case['name']}[{index}]", record, *result)
    ok &= base.compare_upstream(h, case["name"], case, observed, case["group"])
    return ok


def size_check(h, server):
    """Native-only: the largest page (include=all, 365 hourly days) answers
    the same length on JSON GET and HEAD, its protobuf form likewise, both
    under the 2 MiB response bound, with no provider request (cached)."""
    target = "/v1/networks/eth-mainnet/token/0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48/page" \
             "?include=all&days=365&interval=1h"
    before = len(h.upstream.calls())
    auth = [("authorization", "Bearer " + base.token("Q"))]
    history.QUOTA.reserve("Q", 4)
    _, json_headers, json_body = base.raw(server.port, "GET", target, auth)
    _, head_headers, _ = base.raw(server.port, "HEAD", target, auth)
    _, proto_headers, proto_body = base.raw(server.port, "GET", target, auth + [("accept", "application/protobuf")])
    _, proto_head, _ = base.raw(server.port, "HEAD", target, auth + [("accept", "application/protobuf")])
    ok = check(json_headers.get("content-length") == head_headers.get("content-length") == str(len(json_body)) and
               proto_headers.get("content-length") == proto_head.get("content-length") == str(len(proto_body)) and
               0 < len(json_body) < RESPONSE_BOUND and 0 < len(proto_body) < RESPONSE_BOUND and
               len(h.upstream.calls()) == before,
               f"size: json {len(json_body)} head {head_headers.get('content-length')} protobuf {len(proto_body)}")
    print(f"  size: include=all 365 hourly days = {len(json_body)} B JSON, {len(proto_body)} B protobuf "
          f"(bound {RESPONSE_BOUND})", flush=True)
    return ok


def sustained(h, server):
    """Native-only: 96 reads from 12 clients of two cached pages (JSON,
    protobuf, HEAD) give one byte string per page and kind and reach no
    provider (each client stays inside its quota)."""
    targets = ["/v1/networks/eth-mainnet/token/0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48/page",
               "/v1/networks/eth-mainnet/token/0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48/page?include=identity,market"]
    letters = "EFGIJKLMNOPR"
    before = len(h.upstream.calls())
    results = collections.defaultdict(set)
    statuses = collections.Counter()
    lock = threading.Lock()
    for letter in letters:
        history.QUOTA.reserve(letter, 8)

    def client(index):
        for n in range(8):
            kind = ("json", "protobuf", "head")[(index + n) % 3]
            target = targets[(index + n) % 2]
            headers = [("authorization", "Bearer " + base.token(letters[index]))]
            if kind == "protobuf":
                headers.append(("accept", "application/protobuf"))
            status, response_headers, payload = base.raw(server.port, "HEAD" if kind == "head" else "GET", target,
                                                         headers)
            with lock:
                statuses[status] += 1
                results[(target, kind)].add((payload if kind != "head" else response_headers.get("content-length"),
                                             response_headers.get("etag")))

    threads = [threading.Thread(target=client, args=(i,)) for i in range(12)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    server.probe("GET", "/probe/test-token-idle")
    calls = h.upstream.calls()[before:]
    ok = check(statuses == collections.Counter({200: 96}) and all(len(v) == 1 for v in results.values()) and
               len(results) == 6 and not calls,
               f"sustained: statuses {dict(statuses)}, variants {[len(v) for v in results.values()]}, calls {calls[:3]}")
    print(f"  sustained: 96 reads from 12 clients, {len(results)} distinct answers, {len(calls)} provider calls",
          flush=True)
    return ok


def production(binary, root):
    """The production composition: the page rides the price canary; with it
    a missing bearer is 401, an Origin 403, a long param 414 and a raw
    non-ASCII byte Node's 400, all before any facet work."""
    root = Path(root)
    (root / "prod-tls").mkdir()
    ca, _, _, _ = tls_ca.create_certificates(root / "prod-tls", "DNS:localhost")
    path = "/v1/networks/eth-mainnet/token/native/page"
    ok = True

    def start(name, extra):
        kv_dir = root / name
        kv_dir.mkdir()
        port = harness.free_port()
        env = {k: v for k, v in os.environ.items()
               if not k.startswith("FORTIS_") and k not in ("ALCHEMY_API_KEY", "ATTESTATION_MODE", "NODE_ENV")}
        env.update({"FORTIS_KV_DIR": str(kv_dir), "PORT": str(port), "FORTIS_CA_BUNDLE_PATH": str(ca)}, **extra)
        process = subprocess.Popen([binary, str(port)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(400):
            if process.poll() is not None:
                raise RuntimeError(f"production server {name} exited")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.1):
                    return process, port
            except OSError:
                time.sleep(.03)
        raise RuntimeError("production server did not start")

    for value in (None, "0"):
        process, port = start(f"kv-prod-{value}", {} if value is None else {"FORTIS_NATIVE_PRICE_ROUTE_CANARY": value})
        try:
            status, _, _ = base.raw(port, "GET", path, [("authorization", "Bearer " + base.token("A"))])
            ok &= check(status == 404, f"production canary {value!r}: {status}, not 404")
        finally:
            process.terminate()
            process.wait(timeout=30)
    process, port = start("kv-prod-on", {"FORTIS_NATIVE_PRICE_ROUTE_CANARY": "1"})
    try:
        status, headers, body = base.raw(port, "GET", path + "?include=bogus", [])
        ok &= check(status == 401 and body == b'{"error":"UNAUTHORIZED"}' and "vary" not in headers,
                    f"production no-auth: {status} {body!r}")
        status, _, body = base.raw(port, "GET", path, [("origin", "https://evil.example")])
        ok &= check(status == 403 and body == b'{"error":"BROWSER_ORIGIN_FORBIDDEN"}', f"production origin: {status}")
        status, _, _ = base.raw(port, "GET", "/v1/networks/" + "n" * 101 + "/token/native/page", [])
        ok &= check(status == 414, f"production 414: {status}")
        status, _, body = base.raw(port, "GET", "/v1/networks/n\xc3\xa9t/token/native/page", [])
        ok &= check(status == 400 and body == b'{"error":"BAD_REQUEST"}', f"production raw byte: {status} {body!r}")
    finally:
        process.terminate()
        process.wait(timeout=30)
    print("  production: canary unset/0 answers 404; on, no bearer 401, Origin 403, long param 414, raw byte 400",
          flush=True)
    return ok


def drain_quota(server, letter, remaining_after):
    """Device `letter`'s wall-clock minute drained to `remaining_after`."""
    while True:
        if time.time() % 60 > 40:
            time.sleep(61 - time.time() % 60)
        remaining = None
        for _ in range(60):
            status, headers, _ = base.raw(server.port, "GET", "/v1/networks/nowhere/token/native/page",
                                          [("authorization", "Bearer " + base.token(letter))])
            remaining = int(headers.get("x-rate-limit-remaining", "-1"))
            if remaining <= remaining_after:
                break
        if remaining == remaining_after:
            return
        time.sleep(61 - time.time() % 60)


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = load_fixture()
    if not pin_self_check() or not base.self_check_codec(fixture):
        raise SystemExit("the pin or the Struct codec self-check failed")
    only = [g for g in os.environ.get("R24_ONLY_GROUPS", "").split(",") if g]
    with tempfile.TemporaryDirectory(prefix="fortis-r24-") as root:
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
                    drain_quota(server, "D", 5)
                for case in selected:
                    if "state" in case:
                        ok = base.replay_state(h, server, case)
                    elif "concurrent" in case:
                        ok = replay_concurrent(h, server, case)
                    else:
                        ok = replay_case(h, server, case)
                    kind = "asserted" if case["name"] in ASSERTED else "matched"
                    COUNTS[(name, kind if ok else "failed")] += 1
                if name == "main":
                    COUNTS[("size", "matched" if size_check(h, server) else "failed")] += 1
                    COUNTS[("sustained", "matched" if sustained(h, server) else "failed")] += 1
                h.no_secret(server.text(), f"{name} process output")
                if group["kv"] != "restart" or name == "restart-b":
                    server.stop()
                    servers.pop(group["kv"], None)
        finally:
            for server in servers.values():
                server.stop()
            h.upstream.close()
        if len(sys.argv) == 3 and not only:
            COUNTS[("production", "matched" if production(sys.argv[2], root) else "failed")] += 1
        if h.table.extra:
            print("requests no TypeScript case made:", sorted(set(h.table.extra))[:20])
    for (group, kind), count in sorted(COUNTS.items()):
        print(f"  {group:12} {kind:9} {count}")
    matched = sum(v for (g, k), v in COUNTS.items() if k == "matched")
    asserted = sum(v for (g, k), v in COUNTS.items() if k == "asserted")
    failed = sum(v for (g, k), v in COUNTS.items() if k == "failed")
    print(f"token page edges: {matched} matched, {asserted} asserted differences, {failed} failed "
          f"({len(base.FAILURES)} failure messages)")
    if base.FAILURES:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
