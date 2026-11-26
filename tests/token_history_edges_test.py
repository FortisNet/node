#!/usr/bin/env python3
"""Real-socket edges of the four token history/liquidity reads (R20–R23)
against tests/corpus/token_history_edges.json, which
the earlier TypeScript implementation's tests recorded from
the real Fastify app over real sockets:

  R20 GET /v1/networks/{network}/token/{tokenId}/holders/history
  R21 GET /v1/networks/{network}/token/{tokenId}/liquidity/history
  R22 GET /v1/networks/{network}/token/{tokenId}/liquidity/pools
  R23 GET /v1/networks/{network}/token/{tokenId}/liquidity/summary

The fixture's SHA-256 is checked against its pinned .sha256 file before
anything runs, and a mismatched pin is refused (self-checked on a copy).

Per record, in fixture order, one process per group (restart-b reuses
restart-a's KV directory after a SIGKILL):
  - `seed`: the token clock is set to the record's instant and its operations
    go through POST /probe/test-token-history-seed (the production writers),
    in chunks under the 512 KiB request bound;
  - a request: the frozen instant on the token clock, then the exact status,
    every recorded header (x-rate-limit-remaining only in the quota group),
    the exact JSON text or protobuf bytes, and the provider requests (R22's
    DexScreener reads, served the bytes TypeScript was served, collected after
    /probe/test-token-idle) with their identity headers;
  - concurrent records: every simultaneous answer; with `racing` operations
    the seed POST runs while the reads are in flight and each answer must be
    TypeScript's answer of the same kind before or after the write (the
    following `-before-*` / `-after-*` records);
  - `state`: holder rows, raw observations, candles and facts through POST
    /probe/test-token-history-state (observation_projection below).

Each device keeps under its per-minute quota on the native wall clock (the
oracle's quota ran on its frozen Date): a request that would be the device's
41st in the current minute waits for the next minute. No other pacing.

Asserted differences are listed in ASSERTED with the reason in each rule's
docstring, and beside the handlers in src/tokens/token_history_routes.cpp
and src/tokens/token_task_routes.cpp.

Native-only: `rw_stress` (fourteen readers against 200 single-row writes,
each answer one snapshot) after the concurrent group, and with a second argument
the production composition (canary gate, 401/403/414/raw byte on all four).

Usage: token_history_edges_test.py build/fortis_node_token_route_test [build/fortis_node]
Environment: R20_ONLY_GROUPS=a,b limits groups; R20_FAIL_FAST=1 stops at the
first failure; R20_ONLY_ROUTES=holders,history,pools,summary limits the
request records (seeds and states always run).
"""
import collections
import hashlib
import json
import os
import shutil
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

FIXTURE = "token_history_edges.json"
HEADERS = base.HEADERS
COUNTS = collections.Counter()
ROUTE_COUNTS = collections.Counter()
FAIL_FAST = os.environ.get("R20_FAIL_FAST") == "1"
base.FAIL_FAST = FAIL_FAST
check = base.check
ROUTES = ("holders", "history", "pools", "summary")
SUFFIXES = {"/holders/history": "holders", "/liquidity/history": "history", "/liquidity/pools": "pools",
            "/liquidity/summary": "summary"}


def load_fixture(directory=None):
    directory = Path(directory or harness.FIXTURES)
    data = (directory / FIXTURE).read_bytes()
    pinned = (directory / f"{FIXTURE}.sha256").read_text().split()[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{FIXTURE}: sha256 {digest} does not match the pinned {pinned}")
    print(f"{FIXTURE}: sha256 {digest} matches the pin", flush=True)
    import strict_contract_expectations
    return strict_contract_expectations.apply(FIXTURE, json.loads(data))


def pin_self_check():
    """A copy with one byte changed must be refused before any replay."""
    with tempfile.TemporaryDirectory(prefix="fortis-r20-pin-") as root:
        shutil.copy(harness.FIXTURES / FIXTURE, Path(root) / FIXTURE)
        shutil.copy(harness.FIXTURES / f"{FIXTURE}.sha256", Path(root) / f"{FIXTURE}.sha256")
        with open(Path(root) / FIXTURE, "r+b") as handle:
            handle.seek(10)
            byte = handle.read(1)
            handle.seek(10)
            handle.write(b"x" if byte != b"x" else b"y")
        try:
            load_fixture(root)
        except SystemExit as refused:
            print(f"  pin self-check: a modified copy is refused ({str(refused)[:60]}…)", flush=True)
            return True
    return check(False, "a modified fixture copy was accepted")


def route_of(case):
    target = bytes.fromhex(case.get("targetHex", "")).decode("latin1").split("?")[0]
    for suffix, route in SUFFIXES.items():
        if suffix in target:
            return route
    name = case["name"]
    return next((route for route in ROUTES if name.startswith(route + "-") or f"-{route}" in name), "other")


# ---- quota pacing (wall clock) ------------------------------------------------------------------

class Quota:
    LIMIT = 40

    def __init__(self):
        self.used = collections.Counter()
        self.lock = threading.Lock()

    def reserve(self, device, count=1):
        if not device:
            return
        while True:
            with self.lock:
                minute = int(time.time() // 60)
                if self.used[(device, minute)] + count <= self.LIMIT:
                    self.used[(device, minute)] += count
                    return
            time.sleep(61 - time.time() % 60)


QUOTA = Quota()


# ---- asserted differences --------------------------------------------------------------------------

def parser_baseline(h, case, actual, observed):
    """The routing 414 (FST_ERR_MAX_PARAM_LENGTH) and Node's raw-byte "Client
    Error": both answer before any hook with the same status; the body is
    this server's {"error":...} (fastify-errors);
    TypeScript's come from the router / clientError handler without the
    onSend security headers, the shared native writer adds its baseline (as
    R13–R19)."""
    status, headers, payload = actual
    # fastify-errors: both are {"error":...} as application/json;
    # charset=utf-8 now (URI_TOO_LONG, BAD_REQUEST), still before any hook.
    want = compat_expectations.fastify_case(dict(case, headers=dict(case["headers"], **base.NATIVE_BASELINE)))
    return base.compare(case["name"], want, status, headers, payload, remaining=case["group"] == "quota") and \
        check(not observed, f"{case['name']}: provider requests {observed}")


def bad_url(h, case, actual, observed):
    """A malformed or non-UTF-8 path escape: both 400 before any hook;
    Fastify sends its FST_ERR_BAD_URL object, the native request parser
    {"error":"BAD_REQUEST"} (the shared parser, as R13–R19)."""
    ok = json.loads(case["responseText"]).get("code") == "FST_ERR_BAD_URL"
    return check(ok and actual[0] == 400 == case["status"] and actual[2] == b'{"error":"BAD_REQUEST"}' and
                 not observed, f"{case['name']}: native {actual[0]} {actual[2][:120]!r}")


def router_404(h, case, actual, observed):
    """Other methods and spellings: both the router's 404; Fastify's body
    names the route, the native router answers its own (status only, as
    R19)."""
    return check(actual[0] == case["status"] == 404 and not observed, f"{case['name']}: router status {actual[0]}")


def weak_etag(kind, payload):
    digest = hashlib.sha256(payload).hexdigest()[:24]
    return f'W/"token-liquidity-pools{"-protobuf" if kind == "protobuf" else ""}-{digest}"'


def late_follower(h, case, actual, observed):
    """R22, simultaneous cold reads of one token: TypeScript's SWR single
    flight answers every reader from the one live DexScreener read (hit:
    false); the native service's two foreground workers compose it and the
    queued readers read the entry just written (hit: true, the same
    timestamp). Each answer is TypeScript's bytes, or those bytes with the
    cache block hit: true and the weak ETag of exactly those bytes (a HEAD:
    the length and ETag of the JSON follower); every other header equal.
    DexScreener is asked exactly once (the token owner coalesces the
    identical in-flight GET)."""
    ok = True
    late = 0
    json_twin = next(r for r in case["responses"] if r.get("responseText"))
    flipped_json = base.flip_hits(json_twin)
    for index, (item, record, (status, headers, payload)) in enumerate(
            zip(case["concurrent"], case["responses"], actual)):
        label = f"{case['name']}[{index}]"
        kind = item["kind"]
        if kind == "head":
            exact = headers.get("content-length") == record["headers"].get("content-length") and \
                headers.get("etag") == record["headers"].get("etag")
            follower = headers.get("content-length") == str(len(flipped_json)) and \
                headers.get("etag") == weak_etag("json", flipped_json)
        else:
            exact = payload == base.expected_body(record) and headers.get("etag") == record["headers"].get("etag")
            variant = base.flip_hits(record)
            follower = payload == variant and variant != base.expected_body(record) and \
                headers.get("etag") == weak_etag(kind, variant) and headers.get("content-length") == str(len(variant))
        late += bool(follower and not exact)
        ok &= check(status == record["status"] == 200 and (exact or follower),
                    f"{label}: neither TypeScript's answer nor its late-follower form {payload[:160]!r}")
        for name in HEADERS:
            if name in ("x-rate-limit-remaining", "content-length", "etag"):
                continue
            ok &= check(headers.get(name) == record["headers"].get(name), f"{label}: header {name}")
    print(f"  {case['name']}: {late} late followers of {len(actual)}", flush=True)
    return base.compare_upstream(h, case["name"], case, observed, case["group"]) and ok


def restart_durable_swr(h, case, actual, observed):
    """R22, 20 s after a restart: the 60 s liquidity-detail entry lives in the
    durable KV natively, so the read is served from it (hit: true, the first
    process's timestamp) without a DexScreener request, where TypeScript's
    new process (an in-memory Kv) reads live and writes a second observation.
    Every other byte is equal. At 2 min both expired and both read live."""
    status, headers, payload = actual
    before = next(c for c in h.fixture["cases"] if c.get("name") == "restart-pools-before")
    typescript = json.loads(case["responseText"])
    native = json.loads(payload)
    stored = json.loads(before["responseText"])["cache"]
    ok = check(status == 200 and native == dict(typescript, cache=dict(stored, hit=True)) and native != typescript,
               f"{case['name']}: native {payload[:300]!r}")
    for name in HEADERS:
        if name not in ("x-rate-limit-remaining", "etag", "content-length"):
            ok &= check(headers.get(name) == case["headers"].get(name), f"{case['name']}: header {name}")
    return check(not observed, f"{case['name']}: provider requests {observed}") and ok


def head_304(h, case, actual, observed):
    """HEAD with a matching If-None-Match: both 304 with the same ETag,
    cache-control and rate-limit headers and no body; Node adds
    content-length: 0 to that HEAD 304, the shared native writer sends none
    (router-wide, as asserted for R07 and R18)."""
    status, headers, payload = actual
    want = dict(case, headers={k: v for k, v in case["headers"].items() if k != "content-length"})
    return check(case["headers"].get("content-length") == "0" and "content-length" not in headers,
                 f"{case['name']}: content-length {headers.get('content-length')!r}") and \
        base.compare(case["name"], want, status, headers, payload) and \
        check(not observed, f"{case['name']}: provider requests {observed}")


ASSERTED = {
    "pools-concurrent-cold": late_follower,
    "restart-pools-fresh": restart_durable_swr,
}
for _route in ROUTES:
    for _suffix in ("no-auth-long-network", "network-101", "network-escaped-101", "network-astral-51", "ref-bang-101",
                    "ref-astral-51", "network-raw-utf8", "ref-raw-utf8"):
        ASSERTED[f"{_route}-{_suffix}"] = parser_baseline
    for _suffix in ("network-bad-escape", "network-invalid-utf8", "ref-bad-escape", "ref-invalid-utf8"):
        ASSERTED[f"{_route}-{_suffix}"] = bad_url
    for _suffix in ("router-post", "router-trailing-slash", "router-upper", "router-extra-segment"):
        ASSERTED[f"{_route}-{_suffix}"] = router_404
ASSERTED["quota-over-long"] = parser_baseline
for _name in ("holders-inm-match-head", "history-inm-head", "pools-inm-cached-head", "summary-inm-same-instant-head"):
    ASSERTED[_name] = head_304


# ---- durable state ---------------------------------------------------------------------------------

def finite(value):
    if isinstance(value, str):
        try:
            value = float(value.strip()) if value.strip() else 0.0
        except ValueError:
            return None
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return value if value == value and value not in (float("inf"), float("-inf")) else None


def observation_projection(row):
    """TypeScript's stored LiquidityObservation as the native block keeps it:
    numbers instead of decimal strings and only the h24 volume and trade
    gauges of the volumeUsd/txns windows (the only parts any read uses). The
    native row drops network/tokenId (its key holds them) and priceChangePct
    (never read). A named, narrow projection of the durable form only."""
    window = row.get("txns") if isinstance(row.get("txns"), dict) else None
    trades = window.get("h24") if window is not None else None
    trades = trades if isinstance(trades, (dict, list)) else None
    volume = row.get("volumeUsd") if isinstance(row.get("volumeUsd"), dict) else None
    return {
        "ts": row["ts"], "status": row["status"], "source": row["source"],
        "samplingIntervalSeconds": row.get("samplingIntervalSeconds"),
        "totalLiquidityUsd": finite(row.get("totalLiquidityUsd")),
        "weightedPriceUsd": finite(row.get("weightedPriceUsd")),
        "priceSpreadPct": finite(row.get("priceSpreadPct")),
        "priceSpreadPools": finite(row.get("priceSpreadPools")),
        "poolCount": row.get("poolCount"),
        "topPoolShare": finite(row.get("topPoolShare")),
        "fdvUsd": finite(row.get("fdvUsd")),
        "marketCapUsd": finite(row.get("marketCapUsd")),
        "volumeH24": finite(volume.get("h24")) if volume is not None else None,
        "buysH24": finite(trades.get("buys")) if isinstance(trades, dict) else None,
        "sellsH24": finite(trades.get("sells")) if isinstance(trades, dict) else None,
    }


def numbers_equal(a, b):
    """JSON equality with ints and floats compared as numbers."""
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(numbers_equal(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(numbers_equal(x, y) for x, y in zip(a, b))
    if isinstance(a, (int, float)) and isinstance(b, (int, float)) and not isinstance(a, bool) and \
            not isinstance(b, bool):
        return float(a) == float(b)
    return a == b


def replay_state(h, server, case):
    body = json.dumps({"tokens": case["state"]["tokens"]}).encode()
    status, payload = server.probe("POST", "/probe/test-token-history-state", body)
    if not check(status == 200, f"{case['name']}: state probe {status} {payload[:200]!r}"):
        return False
    actual = json.loads(payload)["rows"]
    ok = True
    for want, got in zip(case["rows"], actual):
        label = f"{case['name']} {want['network']}/{want['tokenId']}"
        ok &= check(numbers_equal(want["holders"], got["holders"]),
                    f"{label}: holder rows\n   expected {json.dumps(want['holders'])[:1500]}\n"
                    f"   actual   {json.dumps(got['holders'])[:1500]}")
        projected = [observation_projection(row) for row in want["observations"]]
        ok &= check(numbers_equal(projected, got["observations"]),
                    f"{label}: observations ({len(projected)} vs {len(got['observations'])})\n"
                    f"   expected {json.dumps(projected)[:1200]}\n   actual   {json.dumps(got['observations'])[:1200]}")
        for interval in ("1h", "1d", "1w"):
            ok &= check(numbers_equal(want["candles"][interval], got["candles"][interval]),
                        f"{label}: {interval} candles ({len(want['candles'][interval])} vs "
                        f"{len(got['candles'][interval])})\n   expected {json.dumps(want['candles'][interval])[:1200]}\n"
                        f"   actual   {json.dumps(got['candles'][interval])[:1200]}")
        ok &= check(numbers_equal(want["facts"], got["facts"]),
                    f"{label}: facts\n   expected {json.dumps(want['facts'])[:1200]}\n"
                    f"   actual   {json.dumps(got['facts'])[:1200]}")
    return ok


# ---- the replay -----------------------------------------------------------------------------------------

class Harness:
    def __init__(self, binary, root, fixture):
        self.binary = binary
        self.root = Path(root)
        self.fixture = fixture
        self.table = base.CaseTable()
        self.upstream = base.Upstream(root, self.table)
        self.clock = self.root / "clock"
        self.lanes = base.Lanes()
        self.secrets = ["<no-coingecko-key-in-r20>", "<no-ethplorer-key-in-r20>"]
        self.client_values = []
        self.by_name = {(c["group"], c["name"]): c for c in fixture["cases"]}

    def server(self, kv_name, env, start):
        kv_dir = self.root / f"kv-{kv_name}"
        kv_dir.mkdir(exist_ok=True)
        harness.set_clock(self.clock, start)
        return base.Server(self.binary, kv_dir, self.clock, self.upstream, start, env)

    def no_secret(self, payload, label):
        text = payload.decode("utf-8", "replace") if isinstance(payload, bytes) else payload
        for secret in ["fintel_dev_"] + [base.device_id(letter) for letter in harness.TOKEN_LETTERS]:
            check(secret not in text, f"{label}: secret {secret[:16]}… in a response")


def seed(h, server, case):
    harness.set_clock(h.clock, case["at"])
    chunk, size, ok = [], 0, True
    chunks = []
    for op in case["seed"]:
        text = json.dumps(op)
        if chunk and size + len(text) > 380_000:
            chunks.append(chunk)
            chunk, size = [], 0
        chunk.append(op)
        size += len(text)
    if chunk:
        chunks.append(chunk)
    applied = 0
    for part in chunks:
        status, payload = server.probe("POST", "/probe/test-token-history-seed", json.dumps({"ops": part}).encode())
        ok &= check(status == 200, f"{case['name']}: seed probe {status} {payload[:200]!r}")
        if status == 200:
            applied += sum(json.loads(payload)["applied"])
    print(f"  seed {case['name']}: {len(case['seed'])} ops, {applied} written", flush=True)
    return ok


def run_request(h, server, case, target, method, headers):
    QUOTA.reserve(case["device"])
    return base.raw(server.port, method, target, headers)


def replay_case(h, server, case):
    name = case["name"]
    harness.set_clock(h.clock, case["at"])
    h.table.start_case(case["upstream"], case.get("mode"))
    h.lanes.reserve(case["upstream"])
    before = len(h.upstream.calls())
    target = bytes.fromhex(case["targetHex"]).decode("latin1")
    status, headers, payload = run_request(h, server, case, target, case["method"], base.request_headers(case))
    observed = base.settle(server, h.upstream, before, len(case["upstream"])) if case["upstream"] or \
        route_of(case) == "pools" else h.upstream.calls()[before:]
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


def answer_key(kind, response):
    status, headers, payload = response
    if kind == "head":
        return (status, headers.get("content-length"), headers.get("etag"))
    return (status, payload, headers.get("etag"))


def recorded_key(kind, record):
    if kind == "head":
        return (record["status"], record["headers"].get("content-length"), record["headers"].get("etag"))
    return (record["status"], base.expected_body(record), record["headers"].get("etag"))


def replay_concurrent(h, server, case):
    harness.set_clock(h.clock, case["at"])
    h.table.start_case(case["upstream"], case.get("mode"))
    h.lanes.reserve(case["upstream"])
    QUOTA.reserve(case["device"], len(case["concurrent"]))
    before = len(h.upstream.calls())
    results = [None] * len(case["concurrent"])
    racing = case.get("racing")
    barrier = threading.Barrier(len(results) + (1 if racing else 0))
    seeded = {}

    written = threading.Event()

    def run(index, item):
        target = item.get("target") or bytes.fromhex(case["targetHex"]).decode("latin1")
        barrier.wait()
        # With a racing write, the later readers start once it returned, so
        # both states are exercised; the first ones race it.
        if racing and index >= len(results) // 2:
            written.wait(10)
        results[index] = base.raw(server.port, "HEAD" if item["kind"] == "head" else "GET", target,
                                  base.request_headers(case, item["kind"]))

    def write():
        barrier.wait()
        seeded["status"] = server.probe("POST", "/probe/test-token-history-seed", json.dumps({"ops": racing}).encode())
        written.set()

    threads = [threading.Thread(target=run, args=(i, item)) for i, item in enumerate(case["concurrent"])]
    if racing:
        threads.append(threading.Thread(target=write))
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
    if racing:
        ok &= check(seeded.get("status", (0,))[0] == 200, f"{case['name']}: racing seed {seeded}")
        states = collections.defaultdict(dict)
        for when in ("before", "after"):
            for kind in ("json", "protobuf", "head"):
                record = h.by_name[(case["group"], f"{case['name']}-{when}-{kind}")]
                states[kind][recorded_key(kind, record)] = when
        tally = collections.Counter()
        for index, (item, result) in enumerate(zip(case["concurrent"], results)):
            when = states[item["kind"]].get(answer_key(item["kind"], result))
            tally[when] += 1
            ok &= check(when is not None, f"{case['name']}[{index}]: neither the state before nor after "
                                          f"the write {result[0]} {result[2][:200]!r}")
        print(f"  {case['name']}: {len(results) - tally[None]} of {len(results)} answers are a TypeScript state "
              f"({tally['before']} before, {tally['after']} after the write)", flush=True)
        return ok
    for index, (record, result) in enumerate(zip(case["responses"], results)):
        ok &= base.compare(f"{case['name']}[{index}]", record, *result)
    ok &= base.compare_upstream(h, case["name"], case, observed, case["group"])
    return ok


TODAY = 1770681600000  # the oracle's TODAY (2026-02-10)


def rw_stress(h, server, at):
    """Native-only: fourteen readers of one token's holder history while one
    writer replaces today's count 200 times (one probe request and one KV
    frame each). Every answer is 200 and internally consistent: its newest
    observation and its trends come from one snapshot of the rows (latestAt
    is that observation's time, change24hPct is computed from its count
    against the fixed baseline), and the readers see many distinct states."""
    harness.set_clock(h.clock, at)
    token_id = "erc20:0x" + "ab" * 20
    path = "/v1/networks/eth-mainnet/token/0x" + "ab" * 20 + "/holders/history?days=3"
    day = lambda ago, second: TODAY - ago * 86400000 + second * 1000  # noqa: E731
    base_op = {"op": "count", "network": "eth-mainnet", "tokenId": token_id, "count": 1000, "atLeast": False,
               "source": "ethplorer", "observedAt": day(1, 1200)}
    status, _ = server.probe("POST", "/probe/test-token-history-seed", json.dumps({"ops": [base_op]}).encode())
    ok = check(status == 200, f"rw_stress: baseline seed {status}")
    done = threading.Event()
    answers = []
    lock = threading.Lock()

    def reader(letter):
        for _ in range(38):
            if done.is_set():
                break
            QUOTA.reserve(letter)
            result = base.raw(server.port, "GET", path, [("authorization", "Bearer " + base.token(letter))])
            with lock:
                answers.append(result)

    def writer():
        for k in range(1, 201):
            op = dict(base_op, count=1000 + 7 * k, observedAt=day(0, k))
            server.probe("POST", "/probe/test-token-history-seed", json.dumps({"ops": [op]}).encode())
        done.set()

    threads = [threading.Thread(target=reader, args=(letter,)) for letter in "JKLMNOPQRSTUVW"]
    threads.append(threading.Thread(target=writer))
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    states = set()
    for status, _, payload in answers:
        if not check(status == 200, f"rw_stress: status {status} {payload[:160]!r}"):
            ok = False
            continue
        document = json.loads(payload)
        newest = document["observations"][0]
        trends = document["trends"]
        states.add(newest["count"])
        if newest["count"] == 1000:
            continue
        expected = (newest["count"] - 1000) / 1000 * 100
        ok &= check(trends is not None and trends["latestAt"] == newest["at"] and
                    trends["change24hPct"] == expected,
                    f"rw_stress: trends {trends} do not describe the newest observation {newest}")
    ok &= check(len(states) >= 10, f"rw_stress: readers saw only {len(states)} states")
    print(f"  rw_stress: {len(answers)} reads during 200 writes, {len(states)} distinct states, "
          f"every answer one snapshot", flush=True)
    return ok


def production(binary, root):
    """Native-only, the production composition (build/fortis_node): the
    four routes ride the price canary — 404 unless
    FORTIS_NATIVE_PRICE_ROUTE_CANARY=1; with it, no bearer is 401 and an
    Origin 403 before any read, a 101-unit network 414 before the hooks, and a
    raw non-ASCII byte Node's 400."""
    root = Path(root)
    (root / "prod-tls").mkdir()
    ca, _, _, _ = tls_ca.create_certificates(root / "prod-tls", "DNS:localhost")
    ok = True

    def start(name, extra):
        kv_dir = root / name
        kv_dir.mkdir()
        port = harness.free_port()
        env = {k: v for k, v in os.environ.items()
               if not k.startswith("FORTIS_") and k not in ("ALCHEMY_API_KEY", "ATTESTATION_MODE", "NODE_ENV")}
        env.update({"FORTIS_KV_DIR": str(kv_dir), "PORT": str(port), "FORTIS_CA_BUNDLE_PATH": str(ca)}, **extra)
        process = base.subprocess.Popen([binary, str(port)], env=env, stdout=base.subprocess.DEVNULL,
                                        stderr=base.subprocess.DEVNULL)
        for _ in range(400):
            if process.poll() is not None:
                raise RuntimeError(f"production server {name} exited")
            try:
                with base.socket.create_connection(("127.0.0.1", port), timeout=.1):
                    return process, port
            except OSError:
                time.sleep(.03)
        raise RuntimeError("production server did not start")

    suffixes = list(SUFFIXES)
    process, port = start("kv-prod-off", {})
    try:
        for suffix in suffixes:
            status, _, _ = base.raw(port, "GET", f"/v1/networks/bitcoin/token/native{suffix}",
                                    [("authorization", "Bearer " + base.token("A"))])
            ok &= check(status == 404, f"production canary unset {suffix}: {status}, not 404")
    finally:
        process.terminate()
        process.wait(timeout=30)
    process, port = start("kv-prod-on", {"FORTIS_NATIVE_PRICE_ROUTE_CANARY": "1"})
    try:
        for suffix in suffixes:
            path = f"/v1/networks/bitcoin/token/native{suffix}"
            status, headers, body = base.raw(port, "GET", path, [])
            ok &= check(status == 401 and body == b'{"error":"UNAUTHORIZED"}', f"production no-auth {suffix}: {status}")
            status, _, body = base.raw(port, "GET", path, [("origin", "https://evil.example")])
            ok &= check(status == 403 and body == b'{"error":"BROWSER_ORIGIN_FORBIDDEN"}',
                        f"production origin {suffix}: {status}")
            status, _, _ = base.raw(port, "GET", "/v1/networks/" + "n" * 101 + "/token/native" + suffix, [])
            ok &= check(status == 414, f"production 414 {suffix}: {status}")
            status, _, body = base.raw(port, "GET", "/v1/networks/n\u00c3\u00a9t/token/native" + suffix, [])
            ok &= check(status == 400 and body == b'{"error":"BAD_REQUEST"}', f"production raw byte {suffix}: {status}")
    finally:
        process.terminate()
        process.wait(timeout=30)
    print("  production: canary unset answers 404 on all four routes; on, no bearer 401, Origin 403, long "
          "param 414, raw byte 400", flush=True)
    return ok


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = load_fixture()
    pin_ok = pin_self_check()
    only = [g for g in os.environ.get("R20_ONLY_GROUPS", "").split(",") if g]
    only_routes = [r for r in os.environ.get("R20_ONLY_ROUTES", "").split(",") if r]
    with tempfile.TemporaryDirectory(prefix="fortis-r20-") as root:
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
                    base.drain_quota(server, "D", 5)
                started = time.time()
                for case in selected:
                    if "seed" in case:
                        ok = seed(h, server, case)
                        COUNTS[(name, "seeded" if ok else "failed")] += 1
                        continue
                    if "state" in case:
                        ok = replay_state(h, server, case)
                        COUNTS[(name, "state-matched" if ok else "failed")] += 1
                        continue
                    route = route_of(case)
                    if only_routes and route not in only_routes:
                        continue
                    ok = replay_concurrent(h, server, case) if "concurrent" in case else replay_case(h, server, case)
                    kind = "asserted" if case["name"] in ASSERTED else "matched"
                    COUNTS[(name, kind if ok else "failed")] += 1
                    ROUTE_COUNTS[(route, kind if ok else "failed")] += 1
                if name == "concurrent":
                    stress_at = max(c["at"] for c in selected) + 1000
                    COUNTS[("rw_stress", "matched" if rw_stress(h, server, stress_at) else "failed")] += 1
                print(f"  group {name}: {len(selected)} records in {time.time() - started:.1f} s", flush=True)
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
        print(f"  {group:12} {kind:15} {count}")
    for (route, kind), count in sorted(ROUTE_COUNTS.items()):
        print(f"  route {route:8} {kind:9} {count}")
    matched = sum(v for (g, k), v in COUNTS.items() if k in ("matched", "state-matched"))
    asserted = sum(v for (g, k), v in COUNTS.items() if k in ("asserted", "state-asserted"))
    seeded = sum(v for (g, k), v in COUNTS.items() if k == "seeded")
    failed = sum(v for (g, k), v in COUNTS.items() if k == "failed")
    print(f"token history edges: {matched} matched, {asserted} asserted differences, {seeded} seeds, {failed} failed "
          f"({len(base.FAILURES)} failure messages)")
    if base.FAILURES or not pin_ok:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
