#!/usr/bin/env python3
"""Replay the frozen TypeScript POST /v1/prices/markets contract
(tests/corpus/price_markets_route.json) against the native route
in build/fortis_node_discovery_route_test.

The fixture records every provider reply (URL, status, body) in the case that
caused it; the upstream here serves exactly those replies per URL, in order.
Each case is sent at the fixture's clock with its body, content type and
Accept header; status, the fixture's header set, the body (exact bytes for
JSON successes, protobuf wire hex, parsed JSON for errors) and the provider
URLs requested must match. Case groups run in separate processes over one KV
directory (the CoinGecko lane allows a burst of four per process).

Native-only checks follow: a restart serves stored coins from the durable KV
without a provider call, and two concurrent requests for one cold coin share
one provider call.

The former recorded difference (a 2xx body that is not JSON was one native
provider attempt) is closed by R47: native retries it inside one lane run as
TypeScript's HttpClient does (3 attempts); both report the coin pending.
Usage: price_markets_route_test.py <binary>
"""
import json
import copy
from urllib.parse import quote

import price_markets_case_expectations as case_expectations
import sys
import tempfile
import threading
import time
from pathlib import Path

import compat_expectations
import discovery_route_harness as harness

HEADERS = ["content-type", "content-length", "vary", "cache-control", "x-content-type-options",
           "referrer-policy", "permissions-policy", "x-rate-limit-cost", "retry-after"]
BASE = "https://api.coingecko.com"
SINGLE_ATTEMPT_ON_NATIVE = set()  # closed by R47 (unparseable 2xx retried)


class Replies:
    """Recorded provider replies, served per URL in recording order."""

    def __init__(self, cases):
        self.queues = {}
        self.lock = threading.Lock()
        self.delay = {}
        for case in cases:
            for call in case["upstream"]:
                self.queues.setdefault(call["url"][len(BASE):], []).append(call)

    def add(self, path, status, body, delay=0.0):
        with self.lock:
            self.queues.setdefault(path, []).append({"status": status, "body": body})
            if delay:
                self.delay[path] = delay

    def drop(self, path, count):
        with self.lock:
            del self.queues[path][:count]

    def __call__(self, path, _headers):
        with self.lock:
            queue = self.queues.get(path) or []
            reply = queue.pop(0) if queue else None
            delay = self.delay.get(path, 0.0)
        if delay:
            time.sleep(delay)
        if reply is None:
            return 599, {}, b"unexpected request"
        headers = {"Content-Type": "application/json"} if reply["status"] == 200 else {}
        return reply["status"], headers, reply["body"].encode()


def run_case(server, upstream, replies, clock_file, tokens, case, failures):
    harness.set_clock(clock_file, case["at"])
    before = len(upstream.calls())
    headers = {"Authorization": "Bearer " + tokens.next(), "Accept": case["accept"]}
    if case["contentType"] is not None:
        headers["Content-Type"] = case["contentType"]
    status, response_headers, body = server.request(
        "POST", "/v1/prices/markets", body=bytes.fromhex(case["requestHex"]), headers=headers)
    expected = [call["url"] for call in case["upstream"]]
    if case["name"] in SINGLE_ATTEMPT_ON_NATIVE:
        replies.drop(expected[0][len(BASE):], len(expected) - 1)
        expected = expected[:1]
    harness.wait_for_calls(upstream, before + len(expected), timeout=6.0)
    time.sleep(.08)
    observed = [BASE + path for path, _ in upstream.calls()[before:]]
    name = case["name"]

    def fail(message):
        failures.append(f"{name}: {message}")

    # compat-decisions.md#form-bodies, #js-prototype-keys.
    oracle = case.get("bodyText") if "bodyText" in case else case.get("body")
    problems = compat_expectations.check_removed_request(
        case["contentType"], bytes.fromhex(case["requestHex"]),
        json.dumps(oracle) if isinstance(oracle, dict) else oracle, status, response_headers, body)
    if problems is not None:
        for problem in problems:
            fail(problem)
        if observed:
            fail(f"provider calls {observed} for a request refused before the facet")
        return status, body
    if status != case["status"]:
        fail(f"status {status} != {case['status']} ({body[:300]!r})")
    for header in HEADERS:
        if response_headers.get(header) != case["headers"].get(header):
            fail(f"header {header}: {response_headers.get(header)!r} != {case['headers'].get(header)!r}")
    if "wireHex" in case:
        if body.hex() != case["wireHex"]:
            fail(f"protobuf wire\n  expected {case['wireHex']}\n  actual   {body.hex()}")
    elif "bodyText" in case:
        text = body.decode("utf-8", "replace")
        if text != case["bodyText"]:
            fail(f"body\n  expected {case['bodyText']}\n  actual   {text}")
    elif case["body"] is not None:
        try:
            parsed = json.loads(body)
        except ValueError:
            parsed = body
        if parsed != case["body"]:
            fail(f"body {parsed!r} != {case['body']!r}")
    if observed != expected:
        fail(f"upstream {observed} != {expected}")
    return status, body


def ascii_cases(cases):
    """Retarget the two Unicode oracle cases to the ASCII identifier mapping.

    Provider row bytes stay frozen, but the requested path changes and a
    formerly aliased lowercase ID requires a separate provider response.
    """
    cases = copy.deepcopy(cases)
    first = next(case for case in cases if case['name'] == 'unicode')
    later = next(case for case in cases if case['name'] == 'unicodeProtobuf')
    source = copy.deepcopy(first['upstream'][0])
    def provider(ids):
        call = copy.deepcopy(source)
        prefix, rest = call['url'].split('&ids=', 1)
        _, suffix = rest.split('&', 1)
        call['url'] = prefix + '&ids=' + quote(','.join(ids), safe="-_.!~*'()") + '&' + suffix
        return call
    first['bodyText'] = case_expectations.json_without_markets(first['bodyText'], {'i\u0307stanbul', 'ä-coin'})
    first['body']['markets'] = [row for row in first['body']['markets'] if row['id'] not in {'i\u0307stanbul', 'ä-coin'}]
    ids = json.loads(bytes.fromhex(first['requestHex']))['ids']
    ids = {value.strip().translate(str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ', 'abcdefghijklmnopqrstuvwxyz')) for value in ids}
    first['upstream'] = [provider(sorted(ids, key=lambda value: value.encode('utf-16-be')))]
    first['headers']['content-length'] = str(len(first['bodyText'].encode()))
    later['wireHex'] = case_expectations.proto_without_markets(
        bytes.fromhex(later['wireHex']), later['body']['markets'], {'ä-coin'}).hex()
    later['body']['markets'] = [row for row in later['body']['markets'] if row['id'] != 'ä-coin']
    later['body']['cache'].update(hit=False, partial=True)
    call = provider(['i\u0307stanbul'])
    call['body'] = '[' + ','.join(raw for row, raw in case_expectations.array_rows(source['body'])
                                if row['id'] == 'i\u0307stanbul') + ']'
    later['upstream'] = [call]
    later['headers']['content-length'] = str(len(bytes.fromhex(later['wireHex'])))
    return cases


def main():
    binary = sys.argv[1]
    fixture = harness.load_fixture("price_markets_route.json")
    for case in fixture['cases']:
        if case['name']=='protobufIdWireVarint':
            case.update(status=400,body={'error':'BAD_REQUEST'},bodyText='{"error":"BAD_REQUEST"}',upstream=[])
            case['headers']={key:value for key,value in case['headers'].items() if key not in ('vary','etag')}
            case['headers']['content-length']='23'

    cases = ascii_cases(fixture["cases"])
    groups = {}
    for case in cases:
        groups.setdefault(case["group"], []).append(case)
    failures = []
    tokens = harness.TokenRotation(per_token=40)
    replies = Replies(cases)
    with tempfile.TemporaryDirectory() as root:
        root = Path(root)
        clock = root / "clock"
        kv_dir = root / "kv"
        kv_dir.mkdir()
        upstream = harness.Upstream(root, replies)
        try:
            for group in ["basic", "failure", "stale", "shapes", "unicode", "validation"]:
                harness.set_clock(clock, groups[group][0]["at"])
                server = harness.Server(binary, kv_dir, clock, upstream)
                try:
                    for case in groups[group]:
                        run_case(server, upstream, replies, clock, tokens, case, failures)
                finally:
                    server.stop()
            # Restart: bitcoin and tether, written only by the first process,
            # answer from the durable KV with the cold read's entries.
            cold = next(case for case in cases if case["name"] == "coldJson")
            server = harness.Server(binary, kv_dir, clock, upstream)
            try:
                harness.set_clock(clock, cold["at"] + 30_000)
                before = len(upstream.calls())
                status, _, body = server.request("POST", "/v1/prices/markets",
                    body=json.dumps({"ids": ["tether", "bitcoin"]}).encode(), headers={
                        "Authorization": "Bearer " + tokens.next(),
                        "Content-Type": "application/json", "Accept": "application/json"})
                expected = {"markets": [m for m in cold["body"]["markets"]
                                        if m["id"] in ("bitcoin", "tether")],
                            "pending": [],
                            "cache": {"hit": True, "timestamp": "2026-01-02T03:04:05.000Z"}}
                if status != 200 or json.loads(body) != expected or upstream.calls()[before:]:
                    failures.append(f"restartRead: {status} {body[:300]!r}")
                # Single flight: two concurrent cold reads of one coin share a
                # provider call; both report the coin live.
                at = cases[-1]["at"] + 100_000
                harness.set_clock(clock, at)
                path = ("/api/v3/coins/markets?vs_currency=usd&ids=shared-coin"
                        "&price_change_percentage=1h,24h,7d,14d,30d,200d,1y&sparkline=true")
                replies.add(path, 200, json.dumps([{"id": "shared-coin", "symbol": "sc"}]),
                            delay=0.6)
                before = len(upstream.calls())
                results = []

                def read():
                    body = json.dumps({"ids": ["shared-coin"]}).encode()
                    results.append(server.request("POST", "/v1/prices/markets", body=body, headers={
                        "Authorization": "Bearer " + tokens.next(),
                        "Content-Type": "application/json", "Accept": "application/json"}))

                threads = [threading.Thread(target=read) for _ in range(2)]
                for thread in threads:
                    thread.start()
                    time.sleep(.1)
                for thread in threads:
                    thread.join()
                calls = upstream.calls()[before:]
                bodies = [json.loads(result[2]) for result in results]
                if len(calls) != 1 or any(result[0] != 200 for result in results) or any(
                        [m["id"] for m in b["markets"]] != ["shared-coin"] or b["pending"] or
                        b["cache"] != {"hit": False, "timestamp": None} for b in bodies):
                    failures.append(f"singleFlight: calls={len(calls)} bodies={bodies}")
            finally:
                server.stop()
        finally:
            upstream.close()
    if failures:
        print(f"price markets: {len(failures)} mismatches", file=sys.stderr)
        for failure in failures[:40]:
            print("  " + failure, file=sys.stderr)
        sys.exit(1)
    print(f"price markets: {len(cases)} TypeScript cases replayed (+ restart read, single flight); "
          f"{len(SINGLE_ATTEMPT_ON_NATIVE)} recorded single-attempt difference")


if __name__ == "__main__":
    main()
