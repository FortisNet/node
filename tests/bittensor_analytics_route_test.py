#!/usr/bin/env python3
"""Replay the frozen TypeScript POST /v1/bittensor/analytics contract
(tests/corpus/bittensor_analytics_route.json) against the native
route in build/fortis_node_discovery_route_test.

Each case is sent with the fixture's body, content type and Accept header at
the fixture's clock. Status, the fixture's header set, the body (exact bytes
for JSON successes, protobuf wire hex, parsed JSON for errors) and the
provider requests the case caused (path, query and Authorization) must match.
Case groups run in separate processes over one KV directory, so later groups
also read what earlier processes stored; an extra restart check replays a
warm read from the durable KV.

Recorded native difference (the former single-attempt difference for a 2xx
body that is not JSON is closed by R40: native retries it inside one lane
run, 3 attempts, as TypeScript's HttpClient does):
 - a provider -0 is encoded as protobuf -0 by TypeScript only while its
   in-process L1 cache still holds the parsed value (30 s); after that, and
   always natively, the cached JSON round trip makes it 0. The replay compares
   protobuf number_value -0 as 0 and nothing else.
Usage: bittensor_analytics_route_test.py <binary>
"""
import json
import sys
import tempfile
import time
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

import compat_expectations
import discovery_route_harness as harness

HEADERS = ["content-type", "content-length", "vary", "cache-control", "x-content-type-options",
           "referrer-policy", "permissions-policy", "x-rate-limit-cost", "retry-after"]
BASE = "https://api.taostats.io"
SINGLE_ATTEMPT_ON_NATIVE = set()  # closed by R40 (unparseable 2xx retried)
# google.protobuf.Value number_value (field 2, fixed64) of -0 and of 0.
NEGATIVE_ZERO_VALUE = "11" + "0000000000000080"
ZERO_VALUE = "11" + "0000000000000000"


def odd_payload(ss58):
    return ('{ "pagination" : {"current_page":1,"total_items":1E1,"next_page":null},'
            '"data":[{"address":{"ss58":"' + ss58 + '","hex":"0x\\u00e9\\/"},'
            '"balance_free":"12500000000","rank":2,"rank":3,"2":"two","1":"one","stake":1.50,'
            '"tiny":1e-7,"big":123456789012345678901,"neg":-0,"flag":true,"none":null,'
            '"emoji":"\\ud83d\\ude00 é","ctrl":"a\\u0001b\\tc"}]}').encode()


def make_responder(ss58):
    counters = {}

    def compact(value):
        return json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode()

    def respond(path, _headers):
        counters[path] = counters.get(path, 0) + 1
        call = counters[path]
        query = parse_qs(urlsplit(path).query, keep_blank_values=True)
        scenario = (query.get("hash") or query.get("owner") or query.get("coldkey") or [""])[0]
        json_type = {"Content-Type": "application/json"}
        if scenario == "fail-then-ok":
            return (503, {}, b"provider unavailable") if call <= 3 else \
                (200, json_type, compact({"data": ["recovered"]}))
        if scenario == "throttled":
            return 429, {"Retry-After": "7"}, b"slow down"
        if scenario == "not-json":
            return 200, {}, b"<html>not json</html>"
        if scenario == "empty-body":
            return 200, {}, b""
        if scenario == "revalidate-fails":
            if call == 1 or call >= 5:
                return 200, json_type, compact({"data": [f"answer {call}"]})
            return 503, {}, b"provider unavailable"
        if scenario == "null-payload":
            return 200, {}, b"null"
        if scenario == "array-payload":
            return 200, {}, b' [1, "two", {"3": [true]}] '
        if scenario == "refreshing":
            return 200, json_type, compact({"data": [f"generation {call}"]})
        return 200, json_type, odd_payload(ss58)

    return respond


def run_case(server, upstream, clock_file, tokens, case, failures):
    harness.set_clock(clock_file, case["at"])
    before = len(upstream.calls())
    headers = {"Authorization": "Bearer " + tokens.next(), "Accept": case["accept"]}
    if case["contentType"] is not None:
        headers["Content-Type"] = case["contentType"]
    status, response_headers, body = server.request(
        "POST", "/v1/bittensor/analytics", body=bytes.fromhex(case["requestHex"]), headers=headers)
    expected_calls = [call["url"] for call in case["upstream"]]
    if case["name"] in SINGLE_ATTEMPT_ON_NATIVE:
        expected_calls = expected_calls[:1]
    harness.wait_for_calls(upstream, before + len(expected_calls), timeout=6.0)
    time.sleep(.05)
    calls = upstream.calls()[before:]
    name = case["name"]

    def fail(message):
        failures.append(f"{name}: {message}")

    # compat-decisions.md#form-bodies, #js-prototype-keys.
    oracle = case.get("bodyText") if "bodyText" in case else case.get("body")
    problems = compat_expectations.check_removed_request(
        case["contentType"], bytes.fromhex(case["requestHex"]),
        json.dumps(oracle) if isinstance(oracle, dict) else oracle, status, response_headers, body)
    if problems is None and (oracle == {"error": "UPSTREAM_UNAVAILABLE"} and name.startswith("prototype")
                             or name == "protobufProtoKey"):
        # An Object.prototype member path is an ordinary unsupported path;
        # a `__proto__` map entry an ordinary (unsupported) query key.
        detail = "unsupported bittensor query" if name == "protobufProtoKey" \
            else "unsupported bittensor analytics path"
        expected = json.dumps({"error": "BAD_REQUEST", "detail": detail}, separators=(",", ":")).encode()
        problems = [] if (status, body) == (400, expected) and not calls else \
            [f"{status} {body[:200]!r} is not 400 {detail!r} without provider calls"]
    if problems is not None:
        for problem in problems:
            fail(problem)
        if calls:
            fail(f"provider calls {calls} for a request refused before the facet")
        return status, body
    if status != case["status"]:
        fail(f"status {status} != {case['status']} ({body[:200]!r})")
    for header in HEADERS:
        if response_headers.get(header) != case["headers"].get(header):
            fail(f"header {header}: {response_headers.get(header)!r} != {case['headers'].get(header)!r}")
    if "wireHex" in case:
        expected_wire = case["wireHex"].replace(NEGATIVE_ZERO_VALUE, ZERO_VALUE)
        if body.hex() != expected_wire:
            fail(f"protobuf wire\n  expected {case['wireHex']}\n  actual   {body.hex()}")
    elif "bodyText" in case:
        if body.decode("utf-8", "replace") != case["bodyText"]:
            fail(f"body\n  expected {case['bodyText']}\n  actual   {body.decode('utf-8', 'replace')}")
    elif case["body"] is not None:
        try:
            parsed = json.loads(body)
        except ValueError:
            parsed = body
        if parsed != case["body"]:
            fail(f"body {parsed!r} != {case['body']!r}")
    observed = [BASE + path for path, _ in calls]
    if observed != expected_calls:
        fail(f"upstream calls {observed} != {expected_calls}")
    for path, request_headers in calls:
        if request_headers.get("authorization") != "fixture-taostats-key":
            fail(f"upstream {path} authorization {request_headers.get('authorization')!r}")
    return status, body


def main():
    binary = sys.argv[1]
    fixture = harness.load_fixture("bittensor_analytics_route.json")
    # Restore the provider's source positions for integer-looking keys.
    from strict_contract_expectations import encode_structured, _fields, _varint
    for case in fixture['cases']:
        document=case.get('body')
        if not isinstance(document,dict) or not isinstance(document.get('payload'),dict): continue
        for item in document['payload'].get('data',[]):
            if not isinstance(item,dict) or not all(k in item for k in ('1','2','address','rank')): continue
            reordered={}
            for key,value in item.items():
                if key in ('1','2'): continue
                reordered[key]=value
                if key=='rank': reordered.update({'2':item['2'],'1':item['1']})
            item.clear();item.update(reordered)
        if 'bodyText' in case:
            case['bodyText']=case['bodyText'].replace('{"1":"one","2":"two","address":','{"address":').replace('"rank":3,"stake":','"rank":3,"2":"two","1":"one","stake":')
        if 'wireHex' in case:
            # BittensorAnalyticsResponse.payload is Value, not Struct.
            payload=document['payload']
            outer=next(_fields(encode_structured({'':payload})))[1]
            entry=next(_fields(outer))[1]
            value=next(p for tag,p,raw in _fields(entry) if tag==18)
            # Keep the independent cache field from the original response.
            fields=list(_fields(bytes.fromhex(case['wireHex'])))
            case['wireHex']=(_varint(10)+_varint(len(value))+value+b''.join(raw for tag,p,raw in fields if tag!=10)).hex()

    key = fixture["source"]["apiKey"]
    for case in fixture['cases']:
        if case['name']=='protobufPathWireVarint':
            case.update(status=400,body={'error':'BAD_REQUEST'},bodyText='{"error":"BAD_REQUEST"}',upstream=[])
            case['headers']={key:value for key,value in case['headers'].items() if key not in ('vary','etag')}
            case['headers']['content-length']='23'
    ss58 = json.loads(bytes.fromhex(fixture["cases"][2]["requestHex"]))["query"]["address"]
    cases = fixture["cases"]
    groups = {}
    for case in cases:
        groups.setdefault(case["group"], []).append(case)
    failures = []
    tokens = harness.TokenRotation(per_token=20)
    with tempfile.TemporaryDirectory() as root:
        root = Path(root)
        clock = root / "clock"
        kv_dir = root / "kv"
        kv_dir.mkdir()
        upstream = harness.Upstream(root, make_responder(ss58))
        try:
            plan = [("unconfigured", ["unconfigured"], {}),
                    ("main", ["main", "validation"], {"TAOSTATS_API_KEY": f"  {key}\n"}),
                    ("failure", ["failure"], {"TAOSTATS_API_KEY": key}),
                    ("stale", ["stale", "backoff"], {"TAOSTATS_API_KEY": key})]
            for _, names, env in plan:
                harness.set_clock(clock, cases[0]["at"])
                server = harness.Server(binary, kv_dir, clock, upstream, env)
                try:
                    for name in names:
                        for case in groups.get(name, []):
                            run_case(server, upstream, clock, tokens, case, failures)
                finally:
                    server.stop()
            # Restart durability: the warm JSON read answers from the KV the
            # first keyed process wrote, byte for byte.
            warm = next(case for case in cases if case["name"] == "warmJson")
            server = harness.Server(binary, kv_dir, clock, upstream, {"TAOSTATS_API_KEY": key})
            try:
                restart = dict(warm, name="restartWarmJson")
                run_case(server, upstream, clock, tokens, restart, failures)
            finally:
                server.stop()
        finally:
            upstream.close()
    if failures:
        print(f"bittensor analytics: {len(failures)} mismatches", file=sys.stderr)
        for failure in failures[:40]:
            print("  " + failure, file=sys.stderr)
        sys.exit(1)
    print(f"bittensor analytics: {len(cases)} TypeScript cases replayed (+1 restart read); "
          f"{len(SINGLE_ATTEMPT_ON_NATIVE)} recorded single-attempt difference(s)")


if __name__ == "__main__":
    main()
