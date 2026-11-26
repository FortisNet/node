#!/usr/bin/env python3
"""Real-socket edges of GET /v1/networks/{network}/address/{address}/balances
(R13) against tests/corpus/address_balances_edges.json, which
the earlier TypeScript implementation's tests recorded from
the real Fastify app over real sockets.

The test binary (build/fortis_node_address_route_test) sends every
provider host to one loopback HTTPS upstream (address_routes_test.Upstream).
Per case the upstream answers the exact bytes TypeScript was served, in the
order TypeScript asked for them (a request this case did not make falls back
to the latest earlier answer for the same request, and a host the case marked
down answers 503). Each case's frozen instant is written to the shared
address/price/history clock file first, and the native provider lanes are
paced by monotonic time (address_routes_test.Lanes). Each fixture group is a
fresh process with its own KV directory; restart-b reuses restart-a's after a
SIGKILL.

Checks, per case: the status, every recorded header (x-rate-limit-remaining
only in the quota group), the exact JSON text or protobuf bytes, and the
multiset of provider requests (including background revalidations, collected
after /probe/test-address-idle). Concurrent cases compare every response and
the combined provider requests. `state` records compare the device's watches
(topic, account link, tracked tokens, expiry) and per account the read times
and balance snapshot through POST /probe/test-address-state. No provider
request may carry a client header or credential, and no device token or id
may reach a response or process output.

Asserted differences (documented beside the native handler):
  - router: other methods and spellings are 404 in both (status only); the
    routing 414 and the 431 header limit carry the native security headers
    (body and other headers exact);
  - restart: the SWR envelope lives in the durable KV natively, so a read
    5 s after a restart is a cache hit where TypeScript's process cache was
    empty (the durable floor and its later read match exactly).

Usage: address_balances_edges_test.py build/fortis_node_address_route_test [build/fortis_node]
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
from urllib.parse import unquote, urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parent))
import address_routes_test as routes  # noqa: E402
import discovery_route_harness as harness  # noqa: E402
import robinhood_stock_catalog_edges_test as catalog  # noqa: E402
import compat_expectations  # noqa: E402
import tls_ca  # noqa: E402

FIXTURE = "address_balances_edges.json"
FAILURES = []
# The header set the oracle records.
HEADERS = ["content-type", "content-length", "vary", "cache-control", "x-content-type-options",
           "referrer-policy", "permissions-policy", "x-rate-limit-cost", "x-rate-limit-remaining", "retry-after",
           "access-control-allow-origin", "etag"]
ROUTER_404 = {"router-post", "router-trailing-slash", "router-upper-path"}
RESTART_DIFFERENCE = {"restart-after-stale"}
NATIVE_BASELINE = {"cache-control": "no-store", "x-content-type-options": "nosniff", "referrer-policy": "no-referrer",
                   "permissions-policy": "camera=(), microphone=(), geolocation=()"}
# The rpc lane breaker opens on the case that makes its fifth consecutive
# failure; TypeScript's 30 s cooldown ran on the frozen Date, native's runs
# on the monotonic clock, so the replay waits it out before the recovery.
BREAKER_OPENED_BY = "floor-subset-checksummed"
BREAKER_RECOVERY = "floor-recovered"


def check(condition, message):
    if not condition:
        FAILURES.append(message)
        print("FAIL:", message, flush=True)
    return condition


def load_fixture():
    data = (harness.FIXTURES / FIXTURE).read_bytes()
    pinned = (harness.FIXTURES / f"{FIXTURE}.sha256").read_text().split()[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{FIXTURE}: sha256 {digest} does not match the pinned {pinned}")
    from quote_contract_expectations import retarget
    return retarget(json.loads(data))


class CaseTable:
    """address_routes_test.Upstream's table interface over per-case replies."""

    def __init__(self, cases):
        self.lock = threading.Lock()
        self.hosts = {}
        for case in cases:
            for call in case.get("upstream", []):
                split = urlsplit(routes.normalize_url(call["url"]))
                target = split.path + (f"?{split.query}" if split.query else "")
                self.hosts.setdefault((call["method"], target, call.get("requestBody") or ""), split.netloc)
        self.latest = {}
        self.queue = {}
        self.mode = {}
        self.extra = []
        self.synthesized = []

    def start_case(self, case):
        with self.lock:
            for key, replies in self.queue.items():
                if replies:
                    self.latest[key] = replies[-1]
            self.queue = {}
            for call in case.get("upstream", []):
                key = routes.request_key(call["method"], call["url"], call.get("requestBody") or "")
                self.queue.setdefault(key, []).append((call["status"], call["body"]))
            self.mode = case.get("mode") or {}
            self.served = collections.Counter()

    def subset(self, host, target):
        """A CoinGecko price request this case did not make in that shape:
        native splits a price join over 99 ids into owner requests of at most
        100 ids, so its token_price batches partition TypeScript's one batch.
        Answer the subset of the quotes TypeScript's batch was served."""
        split = urlsplit(target)
        if host != "api.coingecko.com" or not split.path.startswith("/api/v3/simple/"):
            return None
        params = dict(p.split("=", 1) for p in split.query.split("&") if "=" in p)
        field = "contract_addresses" if "token_price" in split.path else "ids"
        wanted = [unquote(v) for v in unquote(params.get(field, "")).split(",") if v]
        for key, replies in list(self.queue.items()) + [(k, [v]) for k, v in self.latest.items()]:
            if not key.startswith(f"GET https://{host}{split.path}?"):
                continue
            status, text = replies[-1]
            if status != 200:
                continue
            document = json.loads(text)
            return json.dumps({k: v for k, v in document.items() if k in wanted}, separators=(",", ":"))
        return None

    def host_for(self, method, target, body):
        return self.hosts.get((method, target, body), "api.coingecko.com")

    def answer(self, method, host, target, body):
        delay = self.mode.get("delayMs", 5)
        down = self.mode.get("down")
        if down == "all" or (isinstance(down, list) and host in down):
            return 503, b"unavailable", delay, False
        key = routes.request_key(method, f"https://{host}{target}", body)
        replies = self.queue.get(key)
        index = self.served[key]
        self.served[key] += 1
        if replies:
            status, text = replies[min(index, len(replies) - 1)]
        elif key in self.latest:
            status, text = self.latest[key]
        elif self.subset(host, target) is not None:
            status, text = 200, self.subset(host, target)
            self.synthesized.append(key)
        else:
            self.extra.append(key)
            return 404, b'{"error":"unexpected request"}', delay, True
        is_json = status < 300 and text != "unavailable"
        try:
            json.loads(text)
        except ValueError:
            is_json = False
        return status, text.encode("utf-8", "surrogatepass"), delay, is_json


def late_follower(record, actual):
    """A JSON/HEAD/protobuf answer equal to the TypeScript one except that it
    reports the entry the shared fetch had just written."""
    if "responseText" not in record or not record["responseText"]:
        return actual[0] == 200 and int(actual[1].get("content-length", "0")) > int(record["headers"]["content-length"])
    try:
        expected = json.loads(record["responseText"])
        document = json.loads(actual[2])
    except ValueError:
        return False
    cache = document.get("cache", {})
    return (cache.get("hit") is True and cache.get("timestamp") == expected.get("asOf") and
            {k: v for k, v in document.items() if k != "cache"} == {k: v for k, v in expected.items() if k != "cache"})


def parser_refusal(case, actual, observed):
    """The native request parser refuses the target before routing: a raw
    '#', or a target over 12,500 bytes (TypeScript serves up to Node's
    16 KiB request head)."""
    return check(actual[0] == 400 and actual[2] == b'{"error":"BAD_REQUEST"}' and not observed,
                 f"{case['name']}: native {actual[0]} {actual[2][:120]!r}, expected the parser's 400")


def head_limit(case, actual, observed):
    """Over the 16 KiB request head: TypeScript 431; native answers 431 too,
    or the socket resets while the client is still sending."""
    return check(actual[0] in (431, 0) and not observed, f"{case['name']}: native {actual[0]}")


def bad_url(case, actual, observed):
    """A malformed path escape: both 400 before any hook; Fastify sends its
    FST_ERR_BAD_URL object, the native parser {"error":"BAD_REQUEST"}."""
    return check(actual[0] == 400 and actual[2] == b'{"error":"BAD_REQUEST"}' and not observed,
                 f"{case['name']}: native {actual[0]} {actual[2][:120]!r}")


def nul_param(case, actual, observed):
    """A decoded NUL in a path parameter: TypeScript's RangeError detail
    echoes it; native answers 400 without the detail (no topic either)."""
    return check(actual[0] == 400 and actual[2] == b'{"error":"BAD_REQUEST"}' and not observed and
                 json.loads(case["responseText"])["error"] == "BAD_REQUEST",
                 f"{case['name']}: native {actual[0]} {actual[2][:120]!r}")


def chunked_prices(case, actual, observed):
    """Over 99 price ids: the same document, but native's price join is
    owner requests of at most 100 ids, so TypeScript's one token_price batch
    arrives as up to three that partition the same addresses."""
    ok = compare(case["name"], case, actual)

    def addresses(urls):
        out = []
        for url in urls:
            query = urlsplit(url).query
            params = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
            out += unquote(params.get("contract_addresses", "")).split(",")
        return sorted(a for a in out if a)
    ts = [c["url"] for c in case["upstream"] if "/simple/token_price/" in c["url"]]
    native = [f"https://{host}{target}" for _, host, target, _ in observed if "/simple/token_price/" in target]
    ok = check(addresses(ts) == addresses(native) and len(native) <= 3,
               f"{case['name']}: token_price batches {len(native)} cover a different address set") and ok
    rest = lambda calls: sorted(c for c in calls if "/simple/token_price/" not in c)  # noqa: E731
    ts_rest = rest(routes.request_key(c["method"], c["url"], c.get("requestBody") or "") for c in case["upstream"])
    native_rest = rest(routes.request_key(m, f"https://{h}{t}", b) for m, h, t, b in observed)
    return check(ts_rest == native_rest, f"{case['name']}: other provider requests differ") and ok


TRON_EXTRA_TOKEN = {"tokenId": "trc10:1000001", "balanceRaw": "77", "decimals": 2,
                    "symbol": "VS", "price": None, "fiat": None, "source": "trongrid"}


def tron_tokens_match(expected, actual):
    if actual == expected:
        return True
    return any(token == TRON_EXTRA_TOKEN and actual[:index] + actual[index + 1:] == expected
               for index, token in enumerate(actual))


def tron_promise_all(case, actual, observed):
    """Tron's background revalidation: TypeScript's Promise.all rejects at the
    first TRC-20 view the busy lane sheds while the admitted view is still in
    flight, so the TRC-10 fallback read that follows is usually shed too and
    that token drops out; native waits for the whole TRC-20 phase, so its
    fallback (getassetissuebyid) is admitted and the token kept. Both are
    races on the lane, so native may also match exactly. Everything else is
    equal: native's requests and tokens are TypeScript's plus at most the
    `getassetissuebyid(1000001)` read and its exact `trc10:1000001` entry."""
    if actual[0] != case["status"]:
        return check(False, f"{case['name']}: status {actual[0]}")
    expected = sorted(routes.request_key(c["method"], c["url"], c.get("requestBody") or "") for c in case["upstream"])
    native = [routes.request_key(m, f"https://{h}{t}", b) for m, h, t, b in observed]
    extra = [k for k in native if "getassetissuebyid" in k and '"value":"1000001"' in k
             and k not in expected]
    ok = check(sorted(k for k in native if k not in extra) == expected and len(extra) <= 1,
               f"{case['name']}: requests {[k[:90] for k in native]} vs {[k[:90] for k in expected]}")
    if case["name"].endswith("-stale"):
        return compare(case["name"], case, actual) and ok
    for name in HEADERS:
        if name == "content-length" or name == "x-rate-limit-remaining":
            continue
        ok = check(actual[1].get(name) == case["headers"].get(name),
                   f"{case['name']}: header {name} {actual[1].get(name)!r} != {case['headers'].get(name)!r}") and ok
    ok = check(actual[1].get("content-length") == str(len(actual[2])),
               f"{case['name']}: content length does not match the response") and ok
    want = json.loads(case["responseText"])
    have = json.loads(actual[2])
    tokens_ts = want.pop("tokens")
    tokens_native = have.pop("tokens")
    return check(want == have and tron_tokens_match(tokens_ts, tokens_native),
                 f"{case['name']}: native tokens {[t['tokenId'] for t in tokens_native]} vs "
                 f"{[t['tokenId'] for t in tokens_ts]}; rest equal: {want == have}") and ok


ASSERTED = {
    "tokens-fragment": parser_refusal, "tokens-fragment-after-token": parser_refusal,
    "tokens-8000-units-mixed-astral": parser_refusal, "tokens-8001-units-mixed-astral": parser_refusal,
    "tokens-12500-byte-target": parser_refusal, "tokens-8001-units-astral-431": head_limit,
    "address-malformed-escape": bad_url, "address-escaped-nul": nul_param,
    "tokens-160-erc20": chunked_prices,
    "family-tronBalances-stale": tron_promise_all, "family-tronBalances-revalidated": tron_promise_all,
    "family-tronBalances-floor": tron_promise_all, "family-tronBalances-floor-held": tron_promise_all,
}
# The Tron state record after that revalidation (the snapshot keeps the token).
STATE_DIFFERENCE = {"family-tronBalances-state"}


def repay(lanes, case, observed):
    """Lanes.reserve paced the case by TypeScript's requests; wait out the
    refill of any request native made beyond them (a chunked price join), so
    a later background refresh is not shed by a lane TypeScript had full."""
    expected = collections.Counter(routes.lane_for(urlsplit(c["url"]).netloc) for c in case.get("upstream", []))
    actual = collections.Counter(routes.lane_for(host) for _, host, _, _ in observed)
    wait = 0.0
    for lane, count in actual.items():
        extra = count - expected.get(lane, 0)
        if extra > 0:
            rate, interval, _ = routes.LANES.get(lane, routes.RPC_DEFAULT)
            wait = max(wait, extra * interval / rate / 1000 + 0.05)
    if wait:
        time.sleep(wait)


def headers_of(case, kind=None):
    headers = {}
    if case["device"]:
        headers["authorization"] = "Bearer " + catalog.token(case["device"])
    headers.update(case.get("requestHeaders") or {})
    if kind == "protobuf":
        headers["accept"] = "application/protobuf"
    return headers


def expected_body(record):
    if "wireHex" in record:
        return bytes.fromhex(record["wireHex"])
    return record.get("responseText", "").encode("utf-8", "surrogatepass")


def compare(label, record, actual, remaining=False, baseline=False):
    status, headers, payload = actual
    if baseline:
        # fastify-errors: the routing 414 and Node's raw-byte 400
        # are this server's {"error":...} now, still before any hook.
        record = compat_expectations.fastify_case(record)
    ok = check(status == record["status"], f"{label}: status {status} != {record['status']} ({payload[:300]!r})")
    if not ok:
        return False
    want = dict(record["headers"])
    if baseline:
        want.update(NATIVE_BASELINE)
    for name in HEADERS:
        if name == "x-rate-limit-remaining" and not remaining:
            continue
        ok = check(headers.get(name) == want.get(name),
                   f"{label}: header {name} {headers.get(name)!r} != {want.get(name)!r}") and ok
    expected = expected_body(record)
    if payload != expected:
        index = next((i for i, (x, y) in enumerate(zip(payload, expected)) if x != y), min(len(payload), len(expected)))
        ok = check(False, f"{label}: body differs at byte {index}\n  expected …{expected[max(0, index - 200):index + 200]!r}"
                          f"\n  actual   …{payload[max(0, index - 200):index + 200]!r}")
    return ok


def compare_upstream(label, case, observed, secrets):
    expected = sorted(routes.request_key(c["method"], c["url"], c.get("requestBody") or "") for c in case["upstream"])
    actual = sorted(routes.request_key(m, f"https://{h}{t}", b) for m, h, t, b in observed)
    ok = check(actual == expected, f"{label}: provider requests\n  expected {[k[:160] for k in expected]}\n"
                                   f"  actual   {[k[:160] for k in actual]}")
    for method, host, target, body in observed:
        text = f"{host}{target} {body}"
        for secret in secrets:
            ok = check(secret not in text, f"{label}: a provider request carries {secret[:12]}…") and ok
    return ok


class Harness:
    def __init__(self, binary, root, fixture):
        self.binary = binary
        self.root = root
        self.fixture = fixture
        self.table = CaseTable(fixture["cases"])
        self.upstream = routes.Upstream(str(root), self.table)
        self.clock = root / "clock"
        self.secrets = list(fixture["deviceIds"].values()) + [catalog.token(k) for k in fixture["deviceIds"]] + \
            list(fixture["clientHeaders"].values())

    def server(self, kv_name, env, start):
        kv_dir = self.root / f"kv-{kv_name}"
        kv_dir.mkdir(exist_ok=True)
        harness.set_clock(self.clock, start)
        return routes.Server(self.binary, kv_dir, self.clock, self.upstream, env)

    def no_secret(self, payload, label):
        for secret in self.secrets:
            check(secret.encode() not in payload, f"{label}: {secret[:12]}… reached the output")


def settle(server, upstream, before, expected):
    began = time.monotonic()
    harness.wait_for_calls(upstream, before + expected, timeout=15.0)
    if os.environ.get("R13_TRACE"):
        print(f"    settled {len(upstream.calls()) - before}/{expected} calls in {time.monotonic() - began:.2f} s", flush=True)
    if os.environ.get("R13_SETTLE_EXTRA"):
        time.sleep(float(os.environ["R13_SETTLE_EXTRA"]))
    server.request("GET", "/probe/test-address-idle")
    time.sleep(.05)
    return upstream.calls()[before:]


def replay_state(h, server, case):
    devices = case["state"]["devices"]
    ok = True
    for index, letter in enumerate(devices):
        body = json.dumps({"device": h.fixture["deviceIds"][letter],
                           "accounts": case["state"]["accounts"] if index == 0 else []}).encode()
        status, _, payload = server.request("POST", "/probe/test-address-state", body=body,
                                            headers={"Content-Type": "application/json"})
        if not check(status == 200, f"{case['name']}: state probe {status} {payload[:200]!r}"):
            return False
        got = json.loads(payload)
        # opaque-params: no topic for a path the router refused.
        watches = [w for w in case["watches"][letter] if not any(ord(c) < 0x20 or c == "\x7f" for c in w["topic"])]
        ok = check(got["watches"] == watches,
                   f"{case['name']}: watches of {letter}\n  expected {watches}\n"
                   f"  actual   {got['watches']}") and ok
        if index == 0:
            for want, have in zip(case["rows"], got["rows"]):
                if any(ord(c) < 0x20 or c == "\x7f" for c in want["network"] + want["address"]):
                    # opaque-params: the router refused that path,
                    # so no account was touched for it.
                    want = dict(want, account=None)
                ok = check(have == want, f"{case['name']}: {want['network']}/{want['address']}\n"
                                         f"  expected {json.dumps(want)[:1500]}\n  actual   {json.dumps(have)[:1500]}") \
                    and ok
            ok = check(len(case["rows"]) == len(got["rows"]), f"{case['name']}: row count") and ok
    return ok


def replay_tron_state(h, server, case):
    body = json.dumps({"device": h.fixture["deviceIds"][case["state"]["devices"][0]],
                       "accounts": case["state"]["accounts"]}).encode()
    status, _, payload = server.request("POST", "/probe/test-address-state", body=body,
                                        headers={"Content-Type": "application/json"})
    got = json.loads(payload) if status == 200 else {}
    want = case["rows"][0]
    have = (got.get("rows") or [{}])[0]
    ok = check(got.get("watches") == case["watches"][case["state"]["devices"][0]], f"{case['name']}: watches")
    tokens_ts = want["balance"]["payload"]["tokens"]
    tokens_native = have.get("balance", {}).get("payload", {}).get("tokens", [])
    strip = lambda row: dict(row, balance=dict(row["balance"], payload={k: v for k, v in row["balance"]["payload"].items()
                                                                        if k != "tokens"}))  # noqa: E731
    return check(have.get("balance") is not None and strip(have) == strip(want) and
                 tron_tokens_match(tokens_ts, tokens_native),
                 f"{case['name']}: snapshot differs beyond the kept TRC-10 token") and ok


def replay_case(h, server, case, lanes):
    name = case["name"]
    if "state" in case:
        if case["name"] in STATE_DIFFERENCE:
            return "asserted" if replay_tron_state(h, server, case) else "failed"
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
                                             entry.get("target") or target, headers_of(case, entry["kind"]))
            except (OSError, IndexError, ValueError) as error:
                results[index] = (0, {}, repr(error).encode())

        threads = [threading.Thread(target=one, args=(i, entry)) for i, entry in enumerate(kinds)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        observed = settle(server, h.upstream, before, len(case["upstream"]))
        repay(lanes, case, observed)
        ok = True
        late = 0
        for index, (record, actual) in enumerate(zip(case["responses"], results)):
            h.no_secret(actual[2], name)
            label = f"{name}[{index}:{kinds[index]['kind']}]"
            if actual[0] == 200 and (actual[2] != expected_body(record) or actual[1].get("content-length") !=
                                     record["headers"].get("content-length")) and late_follower(record, actual):
                late += 1
                continue
            ok = compare(label, record, actual) and ok
        if late:
            # Three foreground workers: a reader queued behind them starts
            # after the shared fetch finished and answers the fresh entry
            # (cache.hit true, the fetch's timestamp); every other byte equal.
            print(f"  {name}: {late} reader(s) answered from the fresh entry", flush=True)
        ok = compare_upstream(name, case, observed, h.secrets) and ok
        return "matched" if ok else "failed"
    try:
        actual = catalog.raw(server.port, case["method"], target, headers_of(case))
    except OSError as error:
        if ASSERTED.get(name) is head_limit:
            settle(server, h.upstream, before, len(case["upstream"]))
            return "asserted"
        check(False, f"{name}: {error!r} (TypeScript {case['status']})")
        settle(server, h.upstream, before, len(case["upstream"]))
        return "failed"
    h.no_secret(actual[2], name)
    observed = settle(server, h.upstream, before, len(case["upstream"]))
    late_price = False
    if len(observed) < len(case["upstream"]) and any("api.coingecko.com" in c["url"] for c in case["upstream"]):
        # Native runs one background price batch at a time: a second stale
        # price refresh (the contract batch after the coin batch) can start
        # well after TypeScript's, which ran both at once.
        harness.wait_for_calls(h.upstream, before + len(case["upstream"]), timeout=30.0)
        observed = h.upstream.calls()[before:]
        late_price = len(observed) == len(case["upstream"])
    repay(lanes, case, observed)
    if os.environ.get("R13_TRACE"):
        print(f"  {name}: {[t[:60] for _, _, t, _ in observed if 'coingecko' in t or 'simple' in t]}", flush=True)
    if compat_expectations.malformed_path_target(case):
        # opaque-params: an empty, control-character or over-long
        # path value is the router's 404 before any handler.
        ok = check(compat_expectations.router_refused(actual[0], actual[1], actual[2], observed),
                   f"{name}: malformed path value is not the router's 404")
        return "asserted" if ok else "failed"
    if name in ROUTER_404:
        check(case["status"] == 404 and actual[0] == 404 and not observed, f"{name}: status {actual[0]} != 404")
        return "asserted"
    if case["status"] in (414, 431) or name.startswith("tokens-raw-utf8"):
        ok = compare(name, case, actual, baseline=True) and compare_upstream(name, case, observed, h.secrets)
        return "asserted" if ok else "failed"
    rule = ASSERTED.get(name)
    if rule:
        ok = rule(case, actual, observed)
        return "asserted" if ok else "failed"
    if case["status"] == 429 and case["headers"].get("retry-after") and actual[0] == 429 and \
            b"temporarily busy" in actual[2]:
        # The lane breaker's remaining cooldown: TypeScript's ran on the frozen
        # case Date (seconds passed between cases), native's on the monotonic
        # clock (milliseconds passed).
        native_after = int(actual[1].get("retry-after", "0"))
        check(1 <= native_after <= 30, f"{name}: retry-after {native_after} outside [1, 30]")
        record = dict(case, headers=dict(case["headers"], **{"retry-after": str(native_after)}))
        ok = compare(name, record, actual) and compare_upstream(name, case, observed, h.secrets)
        return "asserted" if ok else "failed"
    if name in RESTART_DIFFERENCE:
        # The SWR envelope is in the durable KV natively: after the restart
        # the read is a stale hit that revalidates in the background (the
        # same provider requests TypeScript's cold process made live).
        document = json.loads(actual[2]) if actual[0] == 200 else {}
        expected = json.loads(case["responseText"])
        strip = lambda d: {k: v for k, v in d.items() if k not in ("cache", "asOf", "stale")}  # noqa: E731
        ok = check(actual[0] == 200 and strip(document) == strip(expected) and document.get("stale") is True and
                   document.get("cache", {}).get("hit") is True,
                   f"{name}: native {actual[0]} {actual[2][:200]!r}, expected the durable stale hit")
        # The spot cache is durable too: prices TypeScript's new process had
        # to fetch again can still be fresh natively.
        expected_calls = sorted(routes.request_key(c["method"], c["url"], c.get("requestBody") or "")
                                for c in case["upstream"] if "api.coingecko.com" not in c["url"])
        native_calls = sorted(routes.request_key(m, f"https://{host}{t}", b) for m, host, t, b in observed
                              if host != "api.coingecko.com")
        ok = check(native_calls == expected_calls, f"{name}: chain requests differ") and ok
        return "asserted" if ok else "failed"
    ok = compare(name, case, actual, remaining=case["group"] == "quota")
    ok = compare_upstream(name, case, observed, h.secrets) and ok
    if late_price and ok:
        print(f"  {name}: a background price refresh started late (sequential background batches)", flush=True)
        return "asserted"
    return "matched" if ok else "failed"


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = load_fixture()
    only = set(filter(None, os.environ.get("R13_ONLY_GROUPS", "").split(",")))
    groups = {g["name"]: g for g in fixture["groups"]}
    by_group = collections.OrderedDict()
    for case in fixture["cases"]:
        by_group.setdefault(case["group"], []).append(case)
    totals = collections.Counter()
    with tempfile.TemporaryDirectory(prefix="address-balances-") as root:
        root = Path(root)
        h = Harness(binary, root, fixture)
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
                                                       + fixture["holders"]["H1"] + "/balances",
                                                       {"authorization": "Bearer " + catalog.token("d")})
                            if not check(status == 400, f"quota drain answered {status}"):
                                break
                    for case in cases:
                        if case["name"] == BREAKER_RECOVERY and breaker_opened is not None:
                            wait = breaker_opened + 31 - time.monotonic()
                            if wait > 0:
                                time.sleep(wait)
                        if case["name"] == BREAKER_OPENED_BY:
                            breaker_opened = time.monotonic()
                        results[replay_case(h, server, case, lanes)] += 1
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
    print(f"address balances: {totals['matched']} matched, {totals['asserted']} asserted differences, "
          f"{totals['failed']} failed")
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        raise SystemExit(1)
    print("address balances edges: all checks passed")


def production(binary, root, fixture):
    """The production composition: the route exists only behind the canary."""
    (root / "prod-tls").mkdir()
    ca, _, _, _ = tls_ca.create_certificates(root / "prod-tls", "DNS:localhost")
    path = "/v1/networks/eth-mainnet/address/" + fixture["holders"]["H1"] + "/balances"
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
    finally:
        server.stop()
    print("production: canary unset/0 answers 404; on, no bearer 401 and Origin 403")


if __name__ == "__main__":
    main()
