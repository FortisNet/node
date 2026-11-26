#!/usr/bin/env python3
"""Replay the frozen TypeScript Robinhood stock-token contract
(tests/corpus/token_robinhood.json, written by
the earlier TypeScript implementation's tests) against
build/fortis_node_token_route_test, then run native-only measurements.

The fake upstream serves the provider replies recorded in each case (the
case's own calls first, then the first recorded reply to the same request),
so the native token owner receives the TypeScript bytes. Token-owner requests
name their host in x-fortis-test-host; the price canary's spot owner names
none, so a `/rhj/...` target is the Robinhood API and anything else
CoinGecko.

Groups: `main` and `restart` run in two processes over one KV directory (the
restart group reads what `main` persisted, with an empty issuer client);
`alchemy` starts a third process over a fresh KV directory with
ALCHEMY_API_KEY, so the Robinhood RPC endpoint is Alchemy's (the key is in
that URL's path and must stay in provider requests only).

Native lanes refill by the monotonic clock where the TypeScript lanes used
the fixture clock, so the replay paces the CoinGecko (burst 4, 0.4/s),
robinhood-assets (burst 15, 60/s) and goplus (burst 30, 0.5/s) lanes before
each case, and waits out the 30 s breaker a 429 opens.

Each request compares status, the fixture header set, the exact JSON body
bytes or protobuf wire bytes, and the provider request multiset. Concurrent
cases compare both answers (the upstream then answers after 0.3 s so the
second reader joins the first one's in-flight reads).

Native-only checks (no TypeScript counterpart):
  - a cold and warm listed page: latency, provider calls, RSS;
  - four simultaneous cold readers of one contract send one catalog GET and
    one quote GET; three contracts at once send one catalog GET and three
    quote GETs;
  - the price route and the token page share one catalog: /v1/prices for a
    stock token after a page reads no catalog, and a page after /v1/prices
    (which fetched the catalog itself) reads none either;
  - 23 simultaneous cached reads against the 2-worker/16-slot token queue;
  - 23 distinct cold contracts at once behind a 0.3 s upstream: every answer
    is 200 or 429, one catalog GET and one quote GET per served page;
  - a catalog GET answering 429 (opening the robinhood-assets lane): every
    served page fails soft (errors on chainMeta, market and profile, never an
    invented value), and after the breaker cools down a failed contract reads
    again and answers (the failure was never cached);
  - SIGKILL, then the cached page from the durable KV with no provider call.
Usage: token_robinhood_test.py <binary>
"""
import json
import os
import ssl
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

import discovery_route_harness as harness
import tls_ca
import token_ledger_test as ledger
import token_routes_test as routes
import token_tron_test as tron

FIXTURE = "token_robinhood.json"
BREAKER_COOLDOWN_S = 31.0
ROBINHOOD = "api.robinhood.com"


def infer_host(target):
    return ROBINHOOD if target.startswith("/rhj/") else "api.coingecko.com"


class Upstream:
    """HTTPS fake upstream recording (method, host, target, body)."""

    def __init__(self, root, rules):
        self.rules = rules
        self.observed = []
        self.lock = threading.Lock()
        ca, cert, key, _ = tls_ca.create_certificates(Path(root), "DNS:localhost")
        self.ca = ca
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def handle_any(self, method):
                length = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(length) if length else b""
                host = self.headers.get("x-fortis-test-host") or infer_host(self.path)
                with upstream.lock:
                    upstream.observed.append((method, host, self.path, body))
                status, payload = upstream.rules(method, host, self.path, body)
                try:
                    self.send_response(status)
                    if status < 300:
                        self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(payload)))
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.wfile.write(payload)
                    self.wfile.flush()
                except OSError:
                    pass
                self.close_connection = True

            def do_GET(self):
                self.handle_any("GET")

            def do_POST(self):
                self.handle_any("POST")

            def log_message(self, *_args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", harness.free_port()), Handler)
        self.server.daemon_threads = True
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        self.server.socket = tls.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_address[1]

    def calls(self):
        with self.lock:
            return list(self.observed)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


def run_case(server, upstream, rules, clock_file, tokens, case, failures, lanes, secrets):
    name = case["name"]
    harness.set_clock(clock_file, case["at"])
    for lane in lanes:
        lane.reserve(case)
    rules.use(case)
    before = len(upstream.calls())
    expected = routes.expected_calls(case)

    def send(entry, out, index):
        headers = {"Authorization": "Bearer " + tokens.next(), "Accept": entry["accept"]}
        if entry.get("ifNoneMatch"):
            headers["If-None-Match"] = entry["ifNoneMatch"]
        out[index] = server.request("GET", entry["url"], headers=headers)

    if "concurrent" in case:
        entries = case["concurrent"]
        results = [None] * len(entries)
        # Keep each provider read in flight long enough for the second reader
        # to join it (the TypeScript mock answers after 25 ms; the client's
        # single flights share one read).
        rules.delay = 0.3
        threads = [threading.Thread(target=send, args=(entry, results, i)) for i, entry in enumerate(entries)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        rules.delay = 0.0
    else:
        entries = [case]
        results = [None]
        send(case, results, 0)
    harness.wait_for_calls(upstream, before + len(case["upstream"]), timeout=10.0)
    server.request("GET", "/probe/test-token-idle")
    time.sleep(.05)
    observed = routes.observed_calls(upstream.calls()[before:])
    for index, (entry, (status, response_headers, payload)) in enumerate(zip(entries, results)):
        label = f"{name}[{index}]" if len(entries) > 1 else name
        tron.compare_response(label, entry, status, response_headers, payload, failures, secrets)
        for header, value in response_headers.items():
            if any(secret in f"{header}: {value}" for secret in secrets):
                failures.append(f"{label}: the key reached a response header")
    if observed != expected:
        failures.append(f"{name}: upstream\n  expected {expected}\n  actual   {observed}")


def key_leaks(root, secret, server, failures, label):
    """The Alchemy key may live only in provider request URLs: not in a KV
    file, not in the server's output."""
    needle = secret.encode()
    for path in Path(root).rglob("*"):
        if path.is_file() and path.suffix not in (".pem", ".key", ".crt") and needle in path.read_bytes():
            failures.append(f"{label}: the key is stored in {path.name}")
    output = b""
    for stream in (server.process.stdout, server.process.stderr):
        if stream is not None:
            output += stream.read() or b""
    if needle in output:
        failures.append(f"{label}: the key reached the server output")


def lanes():
    return [routes.CoinGeckoLane(), ledger.Lane({ROBINHOOD}, 15, 60.0), ledger.Lane({"api.gopluslabs.io"}, 30, 0.5)]


def normalized_cases(cases):
    """The recorded JSON-RPC endpoint URLs have an empty path; the native
    POST (and the fake upstream) see "/"."""
    for case in cases:
        for call in case.get("upstream", []):
            url = urlsplit(call["url"])
            if not url.path:
                call["url"] = url._replace(path="/").geturl()
    return cases


def replay(binary, fixture, root, failures):
    cases = normalized_cases(fixture["cases"])
    rules = tron.Recorded(cases)
    upstream = Upstream(root, rules)
    shared_kv = Path(root) / "kv"
    shared_kv.mkdir()
    clock_file = Path(root) / "clock"
    tokens = harness.TokenRotation(per_token=40)
    groups = fixture["source"]["groups"]
    secrets = [fixture["source"]["alchemyKey"]]
    ran = 0
    try:
        for group in ("main", "restart", "alchemy"):
            selected = [c for c in cases if c["group"] == group]
            if not selected:
                continue
            spec = groups[group]
            kv_dir = shared_kv
            if spec["kv"] == "fresh":
                kv_dir = Path(root) / f"kv-{group}"
                kv_dir.mkdir()
            extra = {"FORTIS_TEST_PRICE_NOW_FILE": str(clock_file)}
            if spec.get("ALCHEMY_API_KEY"):
                extra["ALCHEMY_API_KEY"] = spec["ALCHEMY_API_KEY"]
            harness.set_clock(clock_file, selected[0]["at"])
            server = routes.Server(binary, kv_dir, clock_file, upstream, selected[0]["at"], extra_env=extra)
            group_lanes = lanes()
            try:
                for case in selected:
                    run_case(server, upstream, rules, clock_file, tokens, case, failures, group_lanes, secrets)
                    ran += 1
                    if any(call["status"] == 429 for call in case.get("upstream", [])):
                        # A 429 opens the lane breaker for 30 s on the
                        # monotonic clock (the TypeScript one cools down on
                        # the fixture clock the next case advances).
                        time.sleep(BREAKER_COOLDOWN_S)
            finally:
                server.stop()
            key_leaks(kv_dir, secrets[0], server, failures, f"group {group}")
    finally:
        upstream.close()
    return ran


# ---- native-only checks ------------------------------------------------------

def contract(index):
    return "0x" + f"{index:040x}"


def symbol(index):
    return f"SYN{index}"


class Synthetic:
    """A 64-asset catalog (contracts 1..64, multiplier 1) whose quotes are
    bid 10 / ask 12; CoinGecko lists nothing and GoPlus knows nothing.
    `reject_next` makes the next catalog GET answer 429."""

    def __init__(self):
        self.delay = 0.0
        self.reject_next = False
        self.lock = threading.Lock()
        assets = []
        for i in range(1, 65):
            assets.append({
                "id": "0x" + f"{i:064x}", "tokenSymbol": symbol(i), "tokenName": f"Synthetic {i} Stock Token",
                "deployments": [{"chainId": 4663, "contractAddress": contract(i)}], "currentMultiplier": "1",
                "pendingMultiplier": "", "logoUrl": f"https://cdn.example/{i}.png",
                "status": "ASSET_STATUS_ACTIVE", "tradingCapabilities": {}})
        self.catalog = json.dumps({"assets": assets}).encode()

    def __call__(self, method, host, target, body):
        if self.delay:
            time.sleep(self.delay)
        split = urlsplit(target)
        if host == ROBINHOOD and split.path == "/rhj/assets":
            with self.lock:
                reject = self.reject_next
                self.reject_next = False
            if reject:
                return 429, b'{"message":"rate limited"}'
            return 200, self.catalog
        if host == ROBINHOOD and split.path.startswith("/rhj/prices/"):
            name = split.path[len("/rhj/prices/"):]
            index = int(name[3:])
            return 200, json.dumps({"quotes": [{
                "tokenSymbol": name, "deployments": [{"chainId": 4663, "contractAddress": contract(index)}],
                "currency": "USD", "bid": "10", "ask": "12", "dailyTradingVolume": "5",
                "isTradingHalt": False, "generatedAt": "2026-01-12T00:00:00Z"}]}).encode()
        if host == "api.coingecko.com" and split.path.startswith("/api/v3/coins/markets"):
            return 200, b"[]"
        if host == "api.gopluslabs.io":
            return 200, b'{"code":1,"message":"OK","result":{}}'
        return 404, b'{"error":"not found"}'


def native_checks(binary, root, failures):
    rules = Synthetic()
    upstream = Upstream(root, rules)
    clock_file = Path(root) / "native-clock"
    kv_dir = Path(root) / "kv-native"
    kv_dir.mkdir()
    now = 1768176000000  # 2026-01-12T00:00:00Z
    tokens = harness.TokenRotation(per_token=40)
    report = {}
    issuer = "identity,market,profile"

    def page(index, include=issuer):
        return f"/v1/networks/robinhood/token/{contract(index)}/page?include={include}"

    def get(path, token=None):
        return server.request("GET", path, headers={"Authorization": "Bearer " + (token or tokens.next()),
                                                    "Accept": "application/json"})

    def robinhood_since(before):
        out = []
        for _, host, target, _ in upstream.calls()[before:]:
            if host == ROBINHOOD:
                out.append(urlsplit(target).path)
        return sorted(out)

    harness.set_clock(clock_file, now)
    extra = {"FORTIS_TEST_PRICE_NOW_FILE": str(clock_file)}
    server = routes.Server(binary, kv_dir, clock_file, upstream, now, extra_env=extra)
    clock = now

    def advance(seconds):
        nonlocal clock
        clock += int(seconds * 1000)
        harness.set_clock(clock_file, clock)

    try:
        rss_boot = tron.rss_kib(server.process.pid)
        before = len(upstream.calls())
        t0 = time.monotonic()
        status, _, payload = get(page(1))
        cold_ms = (time.monotonic() - t0) * 1000
        calls = robinhood_since(before)
        body = routes.json_or_none(payload) or {}
        identity = body.get("identity") or {}
        market = body.get("market") or {}
        if status != 200 or identity.get("symbol") != "SYN1" or identity.get("verified") is not True or \
                identity.get("decimals") != 18 or market.get("priceUsd") != 11 or body.get("errors"):
            failures.append(f"measure: cold page {status} {payload[:300]!r}")
        if calls != ["/rhj/assets", "/rhj/prices/SYN1"] or len(upstream.calls()) - before != 2:
            failures.append(f"measure: cold provider calls {calls}")
        warm = []
        warm_before = len(upstream.calls())
        for _ in range(20):
            t0 = time.monotonic()
            status, _, _ = get(page(1))
            warm.append((time.monotonic() - t0) * 1000)
            if status != 200:
                failures.append(f"measure: warm status {status}")
                break
        warm_calls = len(upstream.calls()) - warm_before
        if warm_calls:
            failures.append(f"measure: {warm_calls} provider calls on warm reads")
        report["cold_and_warm"] = {"cold_ms": round(cold_ms, 1), "robinhood_calls": calls,
                                   "warm_ms_median": round(sorted(warm)[len(warm) // 2], 1),
                                   "warm_ms_max": round(max(warm), 1), "warm_provider_calls": warm_calls}
        rss_warm = tron.rss_kib(server.process.pid)

        letters = "CDEFGIJKLMNOPQRSTUVWXYZ"

        def one(index, path, out):
            t = time.monotonic()
            try:
                status, _, data = server.request("GET", path, headers={
                    "Authorization": "Bearer fintel_dev_" + letters[index % len(letters)] * 43,
                    "Accept": "application/json"})
            except OSError:
                status, data = "reset", b""
            out[index] = (status, (time.monotonic() - t) * 1000, data)

        def burst(paths):
            out = [None] * len(paths)
            threads = [threading.Thread(target=one, args=(i, paths[i], out)) for i in range(len(paths))]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            return out

        # The catalog expires after 15 minutes: four readers of one contract
        # at once share one catalog GET and one quote GET.
        advance(16 * 60)
        rules.delay = 0.3
        before = len(upstream.calls())
        shared = burst([page(2)] * 4)
        rules.delay = 0.0
        report["four_readers_one_contract"] = {"statuses": [s[0] for s in shared],
                                               "robinhood": robinhood_since(before)}
        if [s[0] for s in shared] != [200] * 4 or robinhood_since(before) != ["/rhj/assets", "/rhj/prices/SYN2"]:
            failures.append(f"in-flight sharing: {report['four_readers_one_contract']}")

        # Three contracts at once after expiry: one catalog GET, three quotes.
        advance(16 * 60)
        rules.delay = 0.3
        before = len(upstream.calls())
        trio = burst([page(3), page(4), page(5)])
        rules.delay = 0.0
        report["three_contracts"] = {"statuses": [s[0] for s in trio], "robinhood": robinhood_since(before)}
        if [s[0] for s in trio] != [200] * 3 or robinhood_since(before) != [
                "/rhj/assets", "/rhj/prices/SYN3", "/rhj/prices/SYN4", "/rhj/prices/SYN5"]:
            failures.append(f"three contracts: {report['three_contracts']}")

        # One catalog for the token page and the price route.
        advance(20)
        before = len(upstream.calls())
        status, _, data = get(f"/v1/prices?ids=robinhood:erc20:{contract(6)}&vs=usd")
        price_after_page = {"status": status, "robinhood": robinhood_since(before),
                            "body": routes.json_or_none(data)}
        advance(16 * 60)
        before = len(upstream.calls())
        status, _, data = get(f"/v1/prices?ids=robinhood:erc20:{contract(7)}&vs=usd")
        price_first = {"status": status, "robinhood": robinhood_since(before)}
        advance(1)
        before = len(upstream.calls())
        status, _, data = get(page(8))
        page_after_price = {"status": status, "robinhood": robinhood_since(before)}
        report["shared_catalog"] = {"price_after_page": price_after_page, "price_first": price_first,
                                    "page_after_price": page_after_price}
        if price_after_page["status"] != 200 or price_after_page["robinhood"] != ["/rhj/prices/SYN6"] or \
                price_first["robinhood"] != ["/rhj/assets", "/rhj/prices/SYN7"] or \
                page_after_price["robinhood"] != ["/rhj/prices/SYN8"]:
            failures.append(f"shared catalog: {report['shared_catalog']}")

        # 23 simultaneous cached reads, one seeded device each.
        cached_results = burst([page(1)] * len(letters))
        cached = {}
        for status, _, _ in cached_results:
            cached[status] = cached.get(status, 0) + 1
        if cached.get(200, 0) < 16 or set(cached) - {200, 429}:
            failures.append(f"measure: cached burst answered {cached}")

        def tally(results, calls, label):
            statuses, served, failed_soft = {}, 0, 0
            for status, _, data in results:
                statuses[status] = statuses.get(status, 0) + 1
                if status != 200:
                    continue
                body = routes.json_or_none(data) or {}
                errors = body.get("errors") or {}
                identity = body.get("identity") or {}
                if (identity.get("symbol") or "").startswith("SYN") and not errors and \
                        (body.get("market") or {}).get("priceUsd") == 11:
                    served += 1
                elif identity.get("symbol") is None and body.get("market") is None and \
                        body.get("profile") is None and \
                        errors == {k: "UPSTREAM_UNAVAILABLE" for k in ("chainMeta", "market", "profile")}:
                    failed_soft += 1
                else:
                    failures.append(f"{label}: unexpected page {data[:300]!r}")
            paths = [urlsplit(c[2]).path for c in calls if c[1] == ROBINHOOD]
            quotes = [p for p in paths if p.startswith("/rhj/prices/")]
            if set(statuses) - {200, 429}:
                failures.append(f"{label}: answered {statuses}")
            return {"statuses": statuses, "served_pages": served, "failed_soft_pages": failed_soft,
                    "catalog_gets": paths.count("/rhj/assets"), "quote_gets": len(quotes),
                    "distinct_quote_gets": len(set(quotes))}

        # 23 distinct cold contracts at once behind a slow upstream (expired
        # catalog: one shared catalog GET).
        advance(16 * 60)
        time.sleep(1.0)
        rules.delay = 0.3
        before = len(upstream.calls())
        rss_before_cold = tron.rss_kib(server.process.pid)
        t0 = time.monotonic()
        results = burst([page(10 + i) for i in range(len(letters))])
        seconds = time.monotonic() - t0
        server.request("GET", "/probe/test-token-idle")
        rules.delay = 0.0
        first = tally(results, upstream.calls()[before:], "cold pressure")
        first["seconds"] = round(seconds, 2)
        if first["catalog_gets"] != 1 or first["quote_gets"] != first["served_pages"] or \
                first["distinct_quote_gets"] != first["quote_gets"] or not first["served_pages"] or \
                first["failed_soft_pages"]:
            failures.append(f"cold pressure: {first}")

        # The catalog GET answers 429 (opening the lane): every served page
        # fails soft; nothing is cached.
        advance(16 * 60)
        time.sleep(1.0)
        rules.delay = 0.3
        rules.reject_next = True
        before = len(upstream.calls())
        t0 = time.monotonic()
        throttled = [40 + i for i in range(len(letters))]
        results = burst([page(n) for n in throttled])
        seconds = time.monotonic() - t0
        server.request("GET", "/probe/test-token-idle")
        rules.delay = 0.0
        second = tally(results, upstream.calls()[before:], "throttled")
        second["seconds"] = round(seconds, 2)
        if not second["failed_soft_pages"] or second["catalog_gets"] > 2 or second["served_pages"]:
            failures.append(f"throttled: the opened lane did not shed: {second}")
        failed_index = None
        for (status, _, data), index in zip(results, throttled):
            body = routes.json_or_none(data) or {} if status == 200 else {}
            if (body.get("errors") or {}).get("chainMeta"):
                failed_index = index
                break
        recovered = None
        if failed_index:
            time.sleep(BREAKER_COOLDOWN_S)
            advance(120)
            before = len(upstream.calls())
            status, _, data = get(page(failed_index))
            body = routes.json_or_none(data) or {}
            recovered = {"status": status, "symbol": (body.get("identity") or {}).get("symbol"),
                         "errors": body.get("errors"), "robinhood": robinhood_since(before)}
            if status != 200 or recovered["symbol"] != symbol(failed_index) or recovered["errors"] or \
                    recovered["robinhood"] != ["/rhj/assets", f"/rhj/prices/{symbol(failed_index)}"]:
                failures.append(f"throttled: failure was cached or did not recover: {recovered}")
        second["recovered_after_cooldown"] = recovered
        rss_after = tron.rss_kib(server.process.pid)
        report.update({
            "cached_burst_statuses": cached,
            "cached_burst_ms_max": round(max(ms for s, ms, _ in cached_results if s == 200), 1),
            "cold_contracts": first,
            "throttled_lane": second,
            "rss_kib": {"boot": rss_boot, "after_warm": rss_warm, "before_cold_bursts": rss_before_cold,
                        "after_bursts": rss_after},
        })

        # A page at the final clock, SIGKILL, then the same page 5 s later
        # from the durable KV.
        advance(1)
        status, _, _ = get(page(1))
        if status != 200:
            failures.append(f"restart: seed page {status}")
        server.process.kill()
        server.process.wait()
        advance(5)
        server = routes.Server(binary, kv_dir, clock_file, upstream, clock, extra_env=extra)
        before = len(upstream.calls())
        status, _, payload = get(page(1))
        body = routes.json_or_none(payload) or {}
        after = {"status": status, "provider_calls": len(upstream.calls()) - before,
                 "identity_hit": (body.get("cache") or {}).get("identity", {}).get("hit"),
                 "market_hit": (body.get("cache") or {}).get("market", {}).get("hit"),
                 "symbol": (body.get("identity") or {}).get("symbol")}
        if status != 200 or after["symbol"] != "SYN1" or not after["identity_hit"] or not after["market_hit"] or \
                after["provider_calls"]:
            failures.append(f"restart: {after}")
        report["after_sigkill"] = after
    finally:
        server.stop()
        upstream.close()
    return report


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    os.environ.pop("ALCHEMY_API_KEY", None)
    binary = sys.argv[1]
    fixture = harness.load_fixture(FIXTURE)
    failures = []
    with tempfile.TemporaryDirectory(prefix="fortis-token-robinhood-") as root:
        ran = 0 if os.environ.get("TOKEN_ROBINHOOD_NATIVE_ONLY") else replay(binary, fixture, root, failures)
        report = {} if os.environ.get("TOKEN_ROBINHOOD_REPLAY_ONLY") else native_checks(binary, root, failures)
    if failures:
        print(f"token robinhood replay: {len(failures)} failure(s) over {ran} cases")
        for failure in failures:
            print(" -", failure)
        raise SystemExit(1)
    print(f"token robinhood replay: {ran} cases match the TypeScript contract")
    if report:
        print("token robinhood native checks:", json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
