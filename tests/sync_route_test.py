#!/usr/bin/env python3
"""Replay the frozen TypeScript sync contract
(tests/corpus/sync_route.json, written by
the earlier TypeScript implementation's tests) against
build/fortis_node_address_route_test.

Cases are POST /v1/sync requests (JSON and protobuf bodies, either Accept),
GET …/transfers requests that drive the real transfer writer over the
fixture's request-keyed provider table (address_routes_test.py's fake
upstream, including an Etherscan outage), and two seed kinds the
TypeScript test performed in-process:

  append  POST /probe/test-sync-append — one journal transaction for
          (network, address) through the store's append path; a rollback
          builds and drops it. The resulting journal head must match.
  watch   POST /probe/test-sync-watch — WatchStore.put with an explicit
          expiry, for the device whose bearer the probe carries.

Every case runs at its fixture clock. Status, the fixture header set and the
body (exact JSON bytes and protobuf wire hex for successes, parsed JSON for
errors) must match, as must the provider calls of each transfers case.

Each fixture group runs in a fresh process over one KV directory; the
previous process is killed with SIGKILL first, so the `restart` and `week`
groups read only what reached disk (crash/restart).

Usage: sync_route_test.py <binary>
"""
import json
import os
import signal
import sys
import tempfile
import time
from pathlib import Path

import address_routes_test as replay
import compat_expectations
import discovery_route_harness as harness

FIXTURE = "sync_route.json"

# Documented native bounds of the shared request JSON parser
# (bounded_json.cpp, as for the discovery routes): a number that
# overflows a double (or has 64+ characters) and a lone-surrogate \u escape
# are not valid JSON natively, while JSON.parse accepts them. These cases are
# asserted to give this server's invalid-JSON reply, not skipped.
NATIVE_INVALID_JSON = {"limitInfinite"}


def json_or_none(data):
    try:
        return json.loads(data)
    except (ValueError, UnicodeDecodeError):
        return None


def crash(server):
    """SIGKILL: no shutdown path runs, as on power loss of the process."""
    server.process.send_signal(signal.SIGKILL)
    server.process.wait()


def strict_compare(case, status, response_headers, payload, fail):
    """True when `case` recorded a removed behaviour; checks this
    server's answer instead (compat-decisions.md#form-bodies,
    #js-prototype-keys, #zod-issues)."""
    ce = compat_expectations
    if ce.is_prototype_poisoning(case.get("requestText", ""), case.get("bodyText") or case.get("body")):
        # An ordinary unknown key: the non-strict sync body schema ignores it.
        page = json_or_none(payload)
        if status != 200 or not isinstance(page, dict) or "cursor" not in page:
            fail(f"__proto__ key: {status} {payload[:200]!r} is not an ordinary page")
        return True
    recorded = case.get("bodyText") if "bodyText" in case else case.get("body")
    removed = ce.is_removed_body_media(case["requestHeaders"].get("content-type"))
    if removed or ce.fastify_code(recorded):
        # form-bodies + fastify-errors: the parser's or the body schema's
        # refusal after the device policy (charged), or the policy's first.
        want_status, _, want_body, charged = ce.fastify_expected(
            case["status"], {}, ce.UNSUPPORTED_MEDIA_TYPE_FASTIFY if removed else recorded, case["requestHeaders"])
        if status != want_status or payload != want_body or ("x-rate-limit-cost" in response_headers) != charged:
            fail(f"body framing: {status} {payload[:200]!r} is not {want_status} {want_body!r}")
        return True
    return False


def zod_mapped(case):
    """A recorded zod refusal as this server writes it: {"error":"BAD_REQUEST"}."""
    recorded = case.get("bodyText") if "bodyText" in case else case.get("body")
    status, headers, body = compat_expectations.zod_expected(case["status"], case["headers"], recorded)
    if headers is case["headers"]:
        return case
    key = "bodyText" if "bodyText" in case else "body"
    return dict(case, headers=headers, **{key: body})


def compare_response(case, status, response_headers, payload, fail):
    if strict_compare(case, status, response_headers, payload, fail):
        return
    case = zod_mapped(case)
    if status != case["status"]:
        fail(f"status {status} != {case['status']} ({payload[:400]!r})")
        return
    for header in replay.HEADERS:
        if response_headers.get(header) != case["headers"].get(header):
            fail(f"header {header}: {response_headers.get(header)!r} != {case['headers'].get(header)!r}")
    if "wireHex" in case:
        if payload.hex() != case["wireHex"]:
            expected, actual = case["wireHex"], payload.hex()
            index = next((i for i, (x, y) in enumerate(zip(expected, actual)) if x != y),
                         min(len(expected), len(actual)))
            fail(f"protobuf wire differs at hex {index}\n  expected …{expected[max(0, index - 200):index + 200]}\n"
                 f"  actual   …{actual[max(0, index - 200):index + 200]}")
    elif "bodyText" in case:
        if payload != case["bodyText"].encode("utf-8", "surrogatepass"):
            expected_text = case["bodyText"]
            actual_text = payload.decode("utf-8", "replace")
            index = next((i for i, (x, y) in enumerate(zip(expected_text, actual_text)) if x != y),
                         min(len(expected_text), len(actual_text)))
            fail(f"body differs at {index}\n  expected …{expected_text[max(0, index - 300):index + 300]}\n"
                 f"  actual   …{actual_text[max(0, index - 300):index + 300]}")
    elif json_or_none(payload) != case["body"]:
        fail(f"body {payload[:600]!r} != {json.dumps(case['body'])[:600]}")


def run_seed(server, fixture, case, fail):
    if case["seed"] == "append":
        body = json.dumps(case["input"]).encode()
        status, _, payload = server.request("POST", "/probe/test-sync-append", body=body,
                                            headers={"Content-Type": "application/json"})
        result = json_or_none(payload)
        if status != 200 or result != case["result"]:
            fail(f"append {status} {payload[:200]!r} != {case['result']}")
        return
    seed = case["input"]
    token = fixture["tokens"][seed["device"]]
    body = json.dumps({"topic": seed["topic"], "account": seed["account"],
                       "expiresAt": seed["expiresAt"]}).encode()
    status, _, payload = server.request("POST", "/probe/test-sync-watch", body=body,
                                        headers={"Content-Type": "application/json",
                                                 "Authorization": f"Bearer {token}"})
    if status != 200:
        fail(f"watch {status} {payload[:200]!r}")


def run_case(server, upstream, table, clock_file, fixture, case, failures, lanes):
    name = case["name"]

    def fail(message):
        failures.append(f"{name}: {message}")

    harness.set_clock(clock_file, case["at"])
    if "seed" in case:
        run_seed(server, fixture, case, fail)
        return
    lanes.reserve(case)
    table.mode = case.get("mode") or {}
    before = len(upstream.calls())
    headers = {key: value for key, value in case["requestHeaders"].items()}
    if "requestHex" in case:
        body = bytes.fromhex(case["requestHex"])
    elif "requestText" in case:
        body = case["requestText"].encode("utf-8", "surrogatepass")
    else:
        body = b""
    status, response_headers, payload = server.request(case["method"], case["url"], body=body,
                                                       headers=headers, timeout=90)
    expected = replay.expected_calls(case)
    if expected:
        harness.wait_for_calls(upstream, before + len(expected), timeout=15.0)
        server.request("GET", "/probe/test-address-idle")
        time.sleep(.05)
    observed = replay.observed_calls(upstream.calls()[before:])
    table.mode = {}
    if name in NATIVE_INVALID_JSON:
        # fastify-errors: this server's 400 after the device policy.
        if status != 400 or payload != b'{"error":"BAD_REQUEST"}' or "x-rate-limit-cost" not in response_headers:
            fail(f"native parser bound: {status} {payload[:300]!r}")
        return
    compare_response(case, status, response_headers, payload, fail)
    if observed != expected:
        fail(f"upstream\n  expected {expected}\n  actual   {observed}")


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = replay.load_fixture(FIXTURE)
    cases = fixture["cases"]
    only = os.environ.get("SYNC_ROUTE_ONLY_CASES")
    groups = []
    for case in cases:
        if case["group"] not in groups:
            groups.append(case["group"])
    failures = []
    ran = 0
    with tempfile.TemporaryDirectory(prefix="fortis-sync-route-") as root:
        table = replay.Table(fixture["providers"], [c for c in cases if "seed" not in c])
        upstream = replay.Upstream(root, table)
        kv_dir = Path(root) / "kv"
        kv_dir.mkdir()
        clock_file = Path(root) / "clock"
        try:
            server = None
            for group in groups:
                selected = [c for c in cases if c["group"] == group]
                harness.set_clock(clock_file, selected[0]["at"])
                if server is not None:
                    crash(server)
                server = replay.Server(binary, kv_dir, clock_file, upstream, fixture.get("env") or {})
                lanes = replay.Lanes()
                for case in selected:
                    if only and case["name"] not in only.split(","):
                        continue
                    run_case(server, upstream, table, clock_file, fixture, case, failures, lanes)
                    ran += 1
            if server is not None:
                server.stop()
        finally:
            upstream.close()
        if upstream.misses:
            failures.append(f"unmatched provider requests {sorted(set(upstream.misses))[:20]}")
    if failures:
        print(f"{FIXTURE}: {len(failures)} failure(s) over {ran} cases")
        for failure in failures:
            print(" -", failure)
        raise SystemExit(1)
    print(f"{FIXTURE}: {ran} cases match the TypeScript contract "
          f"({len(NATIVE_INVALID_JSON)} of them as the documented native JSON parser bound)")


if __name__ == "__main__":
    main()
