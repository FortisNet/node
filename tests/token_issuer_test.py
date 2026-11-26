#!/usr/bin/env python3
"""Replay the frozen TypeScript Algorand ASA / XRPL issued-token contract
(tests/corpus/token_issuer.json, written by
the earlier TypeScript implementation's tests) against
build/fortis_node_token_route_test, then run native-only measurements.

The fake upstream serves the provider replies recorded in each case (the
case's own calls first, then the first recorded reply to the same request),
so the native token owner receives the TypeScript bytes. algod GETs and the
XRPL JSON-RPC POSTs name their endpoint host (and a non-default port) in
x-fortis-test-host; POSTs are matched on their JSON body. A recorded XRPL
URL has an empty path (the endpoint itself), which the server sees as "/".

Groups: `main` and `restart` run in two processes over one KV directory (the
restart group reads what `main` persisted); `failover` starts a third process
over a fresh KV directory with FORTIS_TEST_TOKEN_NETWORK_ENDPOINTS listing a
failing fixture algod endpoint first, as the TypeScript fixture builds a
second app whose Algorand seed lists it first; `warm` starts a fourth process
over a fresh KV directory for the stats flush, demand projection and holder
warmer (which selects neither family). XRPL failover needs no override: the
catalog lists three endpoints.

Native lanes refill by the monotonic clock where the TypeScript lanes used
the fixture clock, so the replay paces the CoinGecko (burst 4, 0.4/s), algod
(burst 15, 1/s) and each rpc:{host} (burst 5, 20/s) lane before each case,
and waits out the 30 s breaker a 429 opens.

Each request compares status, the fixture header set, the exact JSON body
bytes or protobuf wire bytes, and the provider request multiset. Concurrent
cases compare both answers (the upstream then answers after 0.3 s so the
second reader joins the first one's in-flight request). Job cases compare the
job summary and its provider requests.

Native-only checks (no TypeScript counterpart):
  - a cold and warm XRPL and Algorand page: latency, provider calls, RSS;
  - the network is part of the in-flight key: with the testnet endpoint
    pointed at the mainnet endpoint (test-only override), one issued token on
    `xrpl` and `xrpl-testnet` at once behind a 0.3 s upstream sends four
    POSTs, never two shared ones;
  - two currencies of one issuer at once share both calls (two POSTs; the
    TypeScript SWR keys would send four: a recorded deliberate difference);
  - 23 simultaneous cached reads against the 2-worker/16-slot token queue;
  - 23 distinct cold issued tokens at once behind a 0.3 s upstream: every
    answer is 200 or 429 and every served page sends exactly one
    gateway_balances and one account_info;
  - 23 distinct cold ASAs (insight only) at once behind a 0.3 s upstream:
    one algod GET per served page (their CoinGecko contract lookups are
    paced and shed by the coingecko lane; a page whose lookup is shed still
    answers);
  - 23 issued tokens whose first gateway_balances answers 429 on xrplcluster
    (opening its lane): every served page fails over or fails soft
    (`errors.insight`, never invented sections), and holders stay null with
    no error;
  - SIGKILL, then the cached pages from the durable KV with no provider call.
Usage: token_issuer_test.py <binary>
"""
import json
import os
import sys
import tempfile
import threading
import time
from pathlib import Path
from urllib.parse import urlsplit

import discovery_route_harness as harness
import token_ledger_test as ledger
import token_routes_test as routes
import token_tron_test as tron

FIXTURE = "token_issuer.json"
BREAKER_COOLDOWN_S = 31.0
ALGOD_HOSTS = {"mainnet-api.algonode.cloud", "algod-a.fixture.test", "testnet-api.algonode.cloud"}
RPC_HOSTS = ["xrplcluster.com", "s1.ripple.com:51234", "s2.ripple.com:51234", "s.altnet.rippletest.net:51234"]
MAINNET_RPC = "https://xrplcluster.com"
TESTNET_RPC = "https://s.altnet.rippletest.net:51234"


def normalize_fixture(fixture):
    """An XRPL endpoint URL has no path; the server sees the request at "/"."""
    # The loopback fixture emits the provider's raw WTF-8 bytes. Unicode
    # maximal-subpart decoding replaces each malformed byte, independently
    # of the one-U+FFFD policy for an escaped JSON surrogate.
    import hashlib
    import re
    from strict_contract_expectations import encode_structured
    for case in fixture["cases"]:
        if case['name'] not in ('stringSurrogate','stringSurrogateProtobuf'): continue
        body=case['body'].get('payload',case['body'])
        section=body['insight'][0]
        decoded=bytes.fromhex('eda0bd').decode('utf-8','replace')
        section['summary']=decoded+' 0 outstanding'
        section['rows'][0]['value']=decoded
        if case['name']=='stringSurrogate':
            case['bodyText']=re.sub(r'\\ud83d',decoded,case['bodyText'])
            encoded=case['bodyText'].encode()
            prefix='token-page'
        else:
            encoded=encode_structured(body)
            case['wireHex']=encoded.hex()
            prefix='token-page-protobuf'
        case['headers']['content-length']=str(len(encoded))
        case['headers']['etag']='W/"'+prefix+'-'+hashlib.sha256(encoded).hexdigest()[:24]+'"'
    for case in fixture["cases"]:
        for call in case.get("upstream", []):
            url = urlsplit(call["url"])
            if not url.path:
                call["url"] = f"{url.scheme}://{url.netloc}/" + (f"?{url.query}" if url.query else "")


def replay(binary, fixture, root, failures):
    cases = fixture["cases"]
    rules = tron.Recorded(cases)
    upstream = tron.Upstream(root, rules)
    shared_kv = Path(root) / "kv"
    shared_kv.mkdir()
    clock_file = Path(root) / "clock"
    tokens = harness.TokenRotation(per_token=40)
    groups = fixture["source"]["groups"]
    ran = 0
    try:
        for group in ("main", "restart", "failover", "warm"):
            selected = [c for c in cases if c["group"] == group]
            if not selected:
                continue
            spec = groups[group]
            kv_dir = shared_kv
            if spec["kv"] == "fresh":
                kv_dir = Path(root) / f"kv-{group}"
                kv_dir.mkdir()
            extra = {"FORTIS_TEST_PRICE_NOW_FILE": str(clock_file)}
            if spec.get("endpoints"):
                extra["FORTIS_TEST_TOKEN_NETWORK_ENDPOINTS"] = ";".join(
                    f"{primary}={','.join(urls)}" for primary, urls in spec["endpoints"].items())
            harness.set_clock(clock_file, selected[0]["at"])
            server = routes.Server(binary, kv_dir, clock_file, upstream, selected[0]["at"], extra_env=extra)
            lanes = [routes.CoinGeckoLane(), ledger.Lane(ALGOD_HOSTS, 15, 1.0)]
            lanes += [ledger.Lane({host}, 5, 20.0) for host in RPC_HOSTS]
            try:
                for case in selected:
                    if "job" in case:
                        harness.set_clock(clock_file, case["at"])
                        for lane in lanes:
                            lane.reserve(case)
                        rules.use(case)
                        ledger.run_job(server, upstream, case, failures)
                    else:
                        tron.run_case(server, upstream, rules, clock_file, tokens, case, failures, lanes, [])
                    ran += 1
                    if any(call["status"] == 429 for call in case.get("upstream", [])):
                        # A 429 opens the lane breaker for 30 s on the
                        # monotonic clock (the TypeScript one cools down on
                        # the fixture clock the next case advances).
                        time.sleep(BREAKER_COOLDOWN_S)
            finally:
                server.stop()
    finally:
        upstream.close()
    return ran


# ---- native-only checks ------------------------------------------------------

BASE58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def issuer(index):
    digits = []
    n = index
    for _ in range(8):
        digits.append(BASE58[n % 58])
        n //= 58
    return ("rSyn" + "".join(digits)).ljust(30, "p")


def iou(code, index):
    return f"{code}.{issuer(index)}"


class Synthetic:
    """XRPL answers every issuer with an obligation per asked code and no
    flags; algod answers every asset with its id as total supply; CoinGecko
    lists nothing."""

    def __init__(self):
        self.delay = 0.0
        self.reject_next_gateway = False
        self.lock = threading.Lock()

    def __call__(self, method, host, target, body):
        if self.delay:
            time.sleep(self.delay)
        split = urlsplit(target)
        if method == "POST" and host in RPC_HOSTS:
            request = json.loads(body or b"{}")
            account = ((request.get("params") or [{}])[0] or {}).get("account", "")
            if request.get("method") == "gateway_balances":
                with self.lock:
                    reject = self.reject_next_gateway and host == "xrplcluster.com"
                    if reject:
                        self.reject_next_gateway = False
                if reject:
                    return 429, b'{"error":"slowDown"}'
                obligations = {code: str(len(account) * 10 + i) for i, code in enumerate(("USD", "EUR", "BTC"))}
                return 200, json.dumps({"result": {"account": account, "obligations": obligations,
                                                   "status": "success", "validated": True}}).encode()
            return 200, json.dumps({"result": {"account_data": {"Account": account, "Flags": 0},
                                               "status": "success", "validated": True}}).encode()
        if host in ALGOD_HOSTS and split.path.startswith("/v2/assets/"):
            asset = split.path.rsplit("/", 1)[1]
            return 200, json.dumps({"index": int(asset), "params": {"total": int(asset), "decimals": 0,
                                                                     "manager": "", "freeze": "",
                                                                     "clawback": ""}}).encode()
        if host == "api.coingecko.com" and split.path.startswith("/api/v3/coins/markets"):
            return 200, b"[]"
        return 404, b'{"error":"coin not found"}'


def native_checks(binary, root, failures):
    rules = Synthetic()
    upstream = tron.Upstream(root, rules)
    clock_file = Path(root) / "native-clock"
    kv_dir = Path(root) / "kv-native"
    kv_dir.mkdir()
    now = 1768176000000  # 2026-01-12T00:00:00Z
    tokens = harness.TokenRotation(per_token=40)
    report = {}
    xrpl_url = f"/v1/networks/xrpl/token/{iou('USD', 0)}/page?include=identity,holders,insight"
    algo_url = "/v1/networks/algorand/token/7000001/page?include=insight"
    # The testnet endpoint is pointed at the mainnet one, so only the network
    # part of the in-flight key keeps the two apart.
    extra = {"FORTIS_TEST_TOKEN_NETWORK_ENDPOINTS": f"{TESTNET_RPC}={MAINNET_RPC}"}

    def get(path, token=None):
        return server.request("GET", path, headers={"Authorization": "Bearer " + (token or tokens.next()),
                                                    "Accept": "application/json"})

    def calls_since(before):
        return sorted((c[0], c[1], json.loads(c[3] or b"{}").get("method", urlsplit(c[2]).path))
                      for c in upstream.calls()[before:])

    harness.set_clock(clock_file, now)
    server = routes.Server(binary, kv_dir, clock_file, upstream, now, extra_env=extra)
    try:
        rss_boot = tron.rss_kib(server.process.pid)
        cold = {}
        for label, url, expected in (
                ("xrpl", xrpl_url, [("POST", "xrplcluster.com", "account_info"),
                                    ("POST", "xrplcluster.com", "gateway_balances")]),
                # An ASA ref also resolves its CoinGecko contract (algorand
                # platform) on its first read, as TypeScript does.
                ("algorand", algo_url, [("GET", "api.coingecko.com", "/api/v3/coins/algorand/contract/7000001"),
                                        ("GET", "mainnet-api.algonode.cloud", "/v2/assets/7000001")])):
            before = len(upstream.calls())
            t0 = time.monotonic()
            status, _, payload = get(url)
            cold_ms = (time.monotonic() - t0) * 1000
            observed = calls_since(before)
            body = routes.json_or_none(payload) or {}
            if status != 200 or not body.get("insight") or body.get("holders") is not None or body.get("errors"):
                failures.append(f"measure: cold {label} page {status} {payload[:300]!r}")
            if observed != expected:
                failures.append(f"measure: cold {label} provider calls {observed}")
            warm = []
            warm_before = len(upstream.calls())
            for _ in range(20):
                t0 = time.monotonic()
                status, _, _ = get(url)
                warm.append((time.monotonic() - t0) * 1000)
                if status != 200:
                    failures.append(f"measure: warm {label} status {status}")
                    break
            warm_calls = len(upstream.calls()) - warm_before
            if warm_calls:
                failures.append(f"measure: {warm_calls} provider calls on warm {label} reads")
            cold[label] = {"cold_ms": round(cold_ms, 1), "provider_calls": observed,
                           "warm_ms_median": round(sorted(warm)[len(warm) // 2], 1),
                           "warm_ms_max": round(max(warm), 1), "warm_provider_calls": warm_calls}
        report["cold_and_warm"] = cold
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

        # One issued token on two networks whose endpoint is the same: four
        # POSTs (the network is in the in-flight key).
        rules.delay = 0.3
        before = len(upstream.calls())
        pair = burst([f"/v1/networks/{network}/token/{iou('USD', 500)}/page?include=insight"
                      for network in ("xrpl", "xrpl-testnet")])
        rules.delay = 0.0
        pair_calls = calls_since(before)
        report["same_request_two_networks"] = {"statuses": [p[0] for p in pair], "calls": pair_calls}
        if [p[0] for p in pair] != [200, 200] or pair_calls != sorted(
                [("POST", "xrplcluster.com", "account_info")] * 2 + [("POST", "xrplcluster.com", "gateway_balances")] * 2):
            failures.append(f"in-flight key (network): {report['same_request_two_networks']}")

        # Two currencies of one issuer at once: identical requests share.
        rules.delay = 0.3
        before = len(upstream.calls())
        shared = burst([f"/v1/networks/xrpl/token/{iou(code, 501)}/page?include=insight" for code in ("USD", "EUR")])
        rules.delay = 0.0
        shared_calls = calls_since(before)
        supplies = [(((routes.json_or_none(d) or {}).get("insight") or [{}])[0]).get("summary") for _, _, d in shared]
        report["two_currencies_one_issuer"] = {"statuses": [s[0] for s in shared], "calls": shared_calls,
                                               "summaries": supplies}
        if shared_calls != [("POST", "xrplcluster.com", "account_info"),
                            ("POST", "xrplcluster.com", "gateway_balances")] or \
                supplies != ["300 USD outstanding", "301 EUR outstanding"]:
            failures.append(f"shared issuer calls: {report['two_currencies_one_issuer']}")

        # 23 simultaneous cached reads, one seeded device each.
        cached_results = burst([xrpl_url] * len(letters))
        cached = {}
        for status, _, _ in cached_results:
            cached[status] = cached.get(status, 0) + 1
        if cached.get(200, 0) < 16 or set(cached) - {200, 429}:
            failures.append(f"measure: cached burst answered {cached}")

        def tally(results, calls, label, per_page):
            statuses, served, failed_soft = {}, 0, 0
            for status, _, data in results:
                statuses[status] = statuses.get(status, 0) + 1
                if status != 200:
                    continue
                page = routes.json_or_none(data) or {}
                errors = page.get("errors") or {}
                if page.get("holders") is not None or "holders" in errors:
                    failures.append(f"{label}: a holder answer or error {data[:300]!r}")
                if page.get("insight") and not errors:
                    served += 1
                elif page.get("insight") == [] and errors.get("insight") == "UPSTREAM_UNAVAILABLE":
                    failed_soft += 1
                else:
                    failures.append(f"{label}: unexpected page {data[:300]!r}")
            reads = {}
            for method, host, target, body, _ in calls:
                if host in RPC_HOSTS or host in ALGOD_HOSTS:
                    key = (json.loads(body or b"{}").get("method"),
                           ((json.loads(body or b"{}").get("params") or [{}])[0] or {}).get("account")) \
                        if method == "POST" else ("GET", target)
                    reads[key] = reads.get(key, 0) + 1
            if set(statuses) - {200, 429}:
                failures.append(f"{label}: answered {statuses}")
            return {"statuses": statuses, "served_pages": served, "failed_soft_pages": failed_soft,
                    "provider_requests": sum(reads.values()), "distinct_requests": len(reads),
                    "per_served_page": per_page}

        # 23 distinct cold issued tokens at once behind a slow upstream.
        harness.set_clock(clock_file, now + 120_000)
        time.sleep(1.0)
        rules.delay = 0.3
        before = len(upstream.calls())
        rss_before_cold = tron.rss_kib(server.process.pid)
        t0 = time.monotonic()
        results = burst([f"/v1/networks/xrpl/token/{iou('USD', i + 10)}/page?include=identity,holders,insight"
                         for i in range(len(letters))])
        seconds = time.monotonic() - t0
        server.request("GET", "/probe/test-token-idle")
        rules.delay = 0.0
        first = tally(results, upstream.calls()[before:], "xrpl pressure", 2)
        first["seconds"] = round(seconds, 2)
        if first["provider_requests"] != 2 * first["served_pages"] or \
                first["distinct_requests"] != first["provider_requests"]:
            failures.append(f"xrpl pressure: requests per served page: {first}")

        # 23 distinct cold ASAs (insight only).
        time.sleep(1.0)
        rules.delay = 0.3
        before = len(upstream.calls())
        t0 = time.monotonic()
        results = burst([f"/v1/networks/algorand/token/{7100000 + i}/page?include=insight"
                         for i in range(len(letters))])
        seconds = time.monotonic() - t0
        server.request("GET", "/probe/test-token-idle")
        rules.delay = 0.0
        second = tally(results, upstream.calls()[before:], "algod pressure", 1)
        second["seconds"] = round(seconds, 2)
        if second["provider_requests"] != second["served_pages"] + second["failed_soft_pages"] or \
                second["distinct_requests"] != second["provider_requests"]:
            failures.append(f"algod pressure: requests per served page: {second}")

        # 23 issued tokens whose first gateway_balances on xrplcluster
        # answers 429: the lane opens and later reads fail over to s1/s2.
        time.sleep(1.0)
        rules.delay = 0.3
        rules.reject_next_gateway = True
        before = len(upstream.calls())
        t0 = time.monotonic()
        results = burst([f"/v1/networks/xrpl/token/{iou('EUR', i + 100)}/page?include=holders,insight"
                         for i in range(len(letters))])
        seconds = time.monotonic() - t0
        server.request("GET", "/probe/test-token-idle")
        rules.delay = 0.0
        calls = upstream.calls()[before:]
        third = tally(results, calls, "xrpl throttled", 2)
        third["seconds"] = round(seconds, 2)
        third["hosts"] = {}
        for _, host, _, _, _ in calls:
            third["hosts"][host] = third["hosts"].get(host, 0) + 1
        if not any(h != "xrplcluster.com" for h in third["hosts"]):
            failures.append(f"xrpl throttled: no failover after the lane opened: {third}")
        rss_after = tron.rss_kib(server.process.pid)
        report.update({
            "cached_burst_statuses": cached,
            "cached_burst_ms_max": round(max(ms for s, ms, _ in cached_results if s == 200), 1),
            "cold_issued_tokens": first,
            "cold_asas_insight": second,
            "issued_tokens_throttled_primary": third,
            "rss_kib": {"boot": rss_boot, "after_warm": rss_warm, "before_cold_bursts": rss_before_cold,
                        "after_bursts": rss_after},
        })

        # SIGKILL, then the same pages from the durable KV.
        server.process.kill()
        server.process.wait()
        harness.set_clock(clock_file, now + 180_000)
        server = routes.Server(binary, kv_dir, clock_file, upstream, now, extra_env=extra)
        after = {}
        for label, url in (("xrpl", xrpl_url), ("algorand", algo_url)):
            before = len(upstream.calls())
            status, _, payload = get(url)
            body = routes.json_or_none(payload) or {}
            after[label] = {"status": status, "provider_calls": len(upstream.calls()) - before,
                            "insight_hit": (body.get("cache") or {}).get("insight", {}).get("hit"),
                            "sections": [s.get("title") for s in body.get("insight") or []]}
            if status != 200 or after[label]["provider_calls"] or not after[label]["insight_hit"] or \
                    not body.get("insight"):
                failures.append(f"restart {label}: {after[label]}")
        report["after_sigkill"] = after
    finally:
        server.stop()
        upstream.close()
    return report


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = harness.load_fixture(FIXTURE)
    normalize_fixture(fixture)
    failures = []
    with tempfile.TemporaryDirectory(prefix="fortis-token-issuer-") as root:
        ran = 0 if os.environ.get("TOKEN_ISSUER_NATIVE_ONLY") else replay(binary, fixture, root, failures)
        report = {} if os.environ.get("TOKEN_ISSUER_REPLAY_ONLY") else native_checks(binary, root, failures)
    if failures:
        print(f"token issuer replay: {len(failures)} failure(s) over {ran} cases")
        for failure in failures:
            print(" -", failure)
        raise SystemExit(1)
    print(f"token issuer replay: {ran} cases match the TypeScript contract")
    if report:
        print("token issuer native checks:", json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
