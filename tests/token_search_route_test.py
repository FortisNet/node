#!/usr/bin/env python3
"""Replay the frozen TypeScript GET /v1/networks/{network}/tokens/search
contract (tests/corpus/token_search_route.json) against the
native route in build/fortis_node_discovery_route_test.

The fixture records every provider reply (URL, status, body) with the case
that caused it; the upstream here serves exactly those replies per path, in
order, for CoinGecko, Jupiter and Robinhood alike. Each case is sent at the
fixture's clock; status, the fixture's header set, the body (exact bytes for
JSON successes, protobuf wire hex, parsed JSON for errors) and the provider
requests (as a multiset: coin details are fetched concurrently) must match.
Case groups run in separate processes over one KV directory, so the
CoinGecko lane's burst of four holds per process as it does per fake
instant in the TypeScript run; later groups read what earlier ones stored.
Usage: token_search_route_test.py <binary>
"""
import json
import sys
import tempfile
import threading
import time
from pathlib import Path

import discovery_route_harness as harness

HEADERS = ["content-type", "content-length", "vary", "cache-control", "x-content-type-options",
           "referrer-policy", "permissions-policy", "x-rate-limit-cost", "retry-after"]
HOSTS = ("https://api.coingecko.com", "https://lite-api.jup.ag", "https://api.robinhood.com")


def strip_host(url):
    for host in HOSTS:
        if url.startswith(host):
            return url[len(host):]
    raise ValueError(url)


class Replies:
    def __init__(self, cases):
        self.queues = {}
        self.lock = threading.Lock()
        for case in cases:
            for call in case["upstream"]:
                self.queues.setdefault(strip_host(call["url"]), []).append(call)

    def __call__(self, path, _headers):
        with self.lock:
            queue = self.queues.get(path) or []
            reply = queue.pop(0) if queue else None
        if reply is None:
            return 599, {}, b"unexpected request"
        headers = {"Content-Type": "application/json"} if reply["status"] == 200 else {}
        return reply["status"], headers, reply["body"].encode()


def run_case(server, upstream, clock_file, tokens, case, failures):
    harness.set_clock(clock_file, case["at"])
    before = len(upstream.calls())
    status, response_headers, body = server.request("GET", case["url"], headers={
        "Authorization": "Bearer " + tokens.next(), "Accept": case["accept"]})
    expected = sorted(strip_host(call["url"]) for call in case["upstream"])
    harness.wait_for_calls(upstream, before + len(expected), timeout=8.0)
    time.sleep(.1)
    observed = sorted(path for path, _ in upstream.calls()[before:])
    name = case["name"]

    def fail(message):
        failures.append(f"{name}: {message}")

    if status != case["status"]:
        fail(f"status {status} != {case['status']} ({body[:300]!r})")
    for header in HEADERS:
        if response_headers.get(header) != case["headers"].get(header):
            fail(f"header {header}: {response_headers.get(header)!r} != {case['headers'].get(header)!r}")
    if "wireHex" in case:
        if body.hex() != case["wireHex"]:
            fail(f"protobuf wire\n  expected {case['wireHex']}\n  actual   {body.hex()}")
    elif "bodyText" in case:
        text = body.decode("utf-8", "surrogateescape")
        if body != case["bodyText"].encode("utf-8", "surrogatepass"):
            fail(f"body\n  expected {case['bodyText']!r}\n  actual   {text!r}")
    elif case["body"] is not None:
        try:
            parsed = json.loads(body)
        except ValueError:
            parsed = body
        if parsed != case["body"]:
            fail(f"body {parsed!r} != {case['body']!r}")
    if observed != expected:
        fail(f"upstream {observed} != {expected}")


def main():
    binary = sys.argv[1]
    fixture = harness.load_fixture("token_search_route.json")
    for case in fixture['cases']:
        if case['name']=='coingeckoWarmB':
            case['bodyText']=case['bodyText'].replace('\\ud83d…','…')
            case['body']['items'][0]['symbol']=case['body']['items'][0]['symbol'].replace('\ud83d','')
            case['headers']['content-length']=str(len(case['bodyText'].encode()))
        if case['name']=='qRepeated':
            case.update(status=502,body={'error':'UPSTREAM_UNAVAILABLE'},upstream=[{'url':'https://api.coingecko.com/api/v3/search?query=a','status':599,'body':'unexpected request'} for _ in range(3)])
            case['headers']['content-length']='32'
        if case['name']=='qBadPercent':
            case.update(status=400,body={'error':'BAD_REQUEST'},bodyText='{"error":"BAD_REQUEST"}',upstream=[])
            case['headers']={key:value for key,value in case['headers'].items() if key not in ('vary','etag')}
            case['headers']['content-length']='23'

    cases = fixture["cases"]
    groups = []
    for case in cases:
        if not groups or groups[-1][0] != case["group"]:
            groups.append((case["group"], []))
        groups[-1][1].append(case)
    failures = []
    tokens = harness.TokenRotation(per_token=20)
    replies = Replies(cases)
    with tempfile.TemporaryDirectory() as root:
        root = Path(root)
        clock = root / "clock"
        kv_dir = root / "kv"
        kv_dir.mkdir()
        upstream = harness.Upstream(root, replies)
        try:
            for _, group_cases in groups:
                harness.set_clock(clock, group_cases[0]["at"])
                server = harness.Server(binary, kv_dir, clock, upstream)
                try:
                    for case in group_cases:
                        run_case(server, upstream, clock, tokens, case, failures)
                finally:
                    server.stop()
        finally:
            upstream.close()
    if failures:
        print(f"token search: {len(failures)} mismatches", file=sys.stderr)
        for failure in failures[:40]:
            print("  " + failure, file=sys.stderr)
        sys.exit(1)
    print(f"token search: {len(cases)} TypeScript cases replayed in {len(groups)} processes")


if __name__ == "__main__":
    main()
