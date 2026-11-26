#!/usr/bin/env python3
"""Real-socket edges of GET /v1/exchange/cross-chain/providers (R06) on the
native test binary (build/fortis_node_cross_chain_route_test), against
tests/corpus/cross_chain_providers_edges.json, which
the earlier TypeScript implementation's tests
recorded from the real Fastify app over real sockets.

  - configurations: one process per oracle configuration (every credential
    subset with MAYA execution off and on, credential and MAYA_NODE_URLS
    edges). Startup must fail exactly where loadConfig threw; otherwise the
    JSON and protobuf answers are TypeScript's bytes. Asserted difference:
    a credential of only \\v (TypeScript: unconfigured; native: refused);
  - request edges on the all-configured process: status, headers and bytes
    for Accept forms, HEAD, ignored queries, Origin/bearer/hook order and
    other methods/paths. Asserted router-wide differences: three Accept
    forms (R04), `%70roviders` (404) and 404 bodies;
  - 12 simultaneous reads (the native admission: 4 workers + 8 pending),
    the oracle's 24 simultaneous reads (asserted router-wide difference:
    excess connects are closed without a response, never answered wrongly),
    200 reads from 12 concurrent clients; no read reaches the upstream;
  - device D's premium minute drained (wall clock): the last 200, the 429s
    (JSON, protobuf Accept, HEAD) and another device;
  - SIGTERM and SIGKILL restarts serve the same bytes;
  - no credential in any response or in process output.
Usage: cross_chain_providers_route_test.py build/fortis_node_cross_chain_route_test
"""
import hashlib
import json
import os
import signal
import socket
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cross_chain_route_test import Server, Upstream  # noqa: E402

FIXTURES = Path(__file__).resolve().parents[1] / "tests/corpus"
FIXTURE = "cross_chain_providers_edges.json"
PATH = "/v1/exchange/cross-chain/providers"
HEADERS = ["content-type", "content-length", "vary", "cache-control", "x-content-type-options",
           "referrer-policy", "permissions-policy", "x-rate-limit-cost", "retry-after",
           "access-control-allow-origin"]
# Router-wide AcceptsProtobuf differences (R04, documented beside the handler).
ACCEPT_DIFFERENCES = {"accept-q-inf": "application/protobuf", "accept-q-binary": "application/json; charset=utf-8",
                      "accept-space-before-param": "application/protobuf"}
# Router 404s: the status matches, the body is the native router's.
ROUTER_404 = {"post", "put", "delete", "trailing-slash", "upper-path"}
CONFIG_DIFFERENCES = {"vertical-tab-key"}

FAILURES = []


def check(condition, message):
    if not condition:
        FAILURES.append(message)
        print("FAIL:", message)


def load_fixture():
    data = (FIXTURES / FIXTURE).read_bytes()
    pinned = (FIXTURES / f"{FIXTURE}.sha256").read_text().split()[0]
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        raise SystemExit(f"{FIXTURE}: sha256 {digest} does not match the pinned {pinned}")
    return json.loads(data)


def token(letter):
    return "fintel_dev_" + letter * 43


def raw(port, method, target, headers):
    """One request, exactly these header lines; returns (status, headers, payload)."""
    lines = [f"{method} {target} HTTP/1.1", "Host: localhost", "Connection: close"]
    lines += [f"{key}: {value}" for key, value in headers.items()]
    with socket.create_connection(("127.0.0.1", port), timeout=30) as sock:
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("latin1"))
        data = bytearray()
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data.extend(chunk)
    head, _, payload = bytes(data).partition(b"\r\n\r\n")
    first, *header_lines = head.decode("latin1").split("\r\n")
    response_headers = {}
    for line in header_lines:
        if ":" in line:
            key, value = line.split(":", 1)
            response_headers[key.lower()] = value.strip()
    return int(first.split()[1]), response_headers, payload


def get(port, letter="A", accept=None, method="GET"):
    headers = {"authorization": "Bearer " + token(letter)}
    if accept:
        headers["accept"] = accept
    return raw(port, method, PATH, headers)


def body_of(case):
    return bytes.fromhex(case["wireHex"]) if "wireHex" in case else case["responseText"].encode("utf-8")


def compare(case, status, headers, payload, remaining=False, label=None):
    label = label or case["name"]
    problems = []
    if status != case["status"]:
        problems.append(f"status {status} != {case['status']}")
    for name in HEADERS + (["x-rate-limit-remaining"] if remaining else []):
        if headers.get(name) != case["headers"].get(name):
            problems.append(f"header {name}: {headers.get(name)!r} != {case['headers'].get(name)!r}")
    if payload != body_of(case):
        problems.append(f"body {payload[:80]!r} != {body_of(case)[:80]!r}")
    check(not problems, f"{label}: " + "; ".join(problems))
    return not problems


def start(binary, upstream, root, name, env, clock):
    kv = root / f"kv-{name}"
    kv.mkdir(exist_ok=True)
    return Server(binary, upstream, kv, clock, env)


def no_secret(fixture, data, where):
    for secret in fixture["secrets"].values():
        check(secret.encode() not in data, f"{where}: credential {secret[:12]}… leaked")


def configurations(binary, upstream, root, clock, fixture):
    matched = refused = differences = 0
    for config in fixture["configs"]:
        name = config["name"]
        try:
            server = start(binary, upstream, root, name, config["env"], clock)
        except RuntimeError as error:
            if name in CONFIG_DIFFERENCES:
                check("error" not in config and "contains invalid characters" in str(error),
                      f"{name}: expected the documented refusal, got {str(error)[-160:]!r}")
                differences += 1
            elif "error" in config:
                refused += 1
            else:
                check(False, f"{name}: TypeScript served this configuration; native refused: {str(error)[-200:]!r}")
            continue
        try:
            check("error" not in config and name not in CONFIG_DIFFERENCES,
                  f"{name}: TypeScript refused ({config.get('error', 'difference')}); native served")
            if "error" in config or name in CONFIG_DIFFERENCES:
                continue
            json_status, _, json_body = get(server.port)
            proto_status, _, proto_body = get(server.port, accept="application/protobuf")
            ok = json_status == 200 and json_body == config["responseText"].encode("utf-8")
            ok = ok and proto_status == 200 and proto_body.hex() == config["wireHex"]
            check(ok, f"{name}: directory differs: {json_status} {json_body[:120]!r}")
            matched += ok
            no_secret(fixture, json_body + proto_body, name)
        finally:
            output = server.stop()
            no_secret(fixture, output, f"{name} output")
    print(f"configurations: {matched} byte-identical, {refused} refusals matched, {differences} asserted difference "
          f"({len(fixture['configs'])} total)")


def wait_for_fresh_minute(budget_seconds):
    """Quota windows are wall-clock minutes: leave room for the whole drain."""
    second = time.time() % 60
    if second > 60 - budget_seconds:
        time.sleep(60 - second + 0.2)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    binary = sys.argv[1]
    fixture = load_fixture()
    check(fixture["upstreamFetches"] == 0, "the oracle reached an upstream")
    cases = {case["name"]: case for case in fixture["cases"]}
    with tempfile.TemporaryDirectory(prefix="cross-chain-providers-") as root:
        root = Path(root)
        (root / "tls").mkdir()
        upstream = Upstream(root / "tls")
        clock = root / "clock"
        clock.write_text("0")
        upstream.start_case({})
        try:
            configurations(binary, upstream, root, clock, fixture)
            server = start(binary, upstream, root, "main", fixture["mainEnv"], clock)
            payloads = bytearray()
            try:
                # ---- request edges ----
                matched = asserted = 0
                for case in fixture["cases"]:
                    if case["group"] != "request":
                        continue
                    name = case["name"]
                    headers = {}
                    if case["device"]:
                        headers["authorization"] = "Bearer " + token(case["device"])
                    headers.update(case["requestHeaders"])
                    target = bytes.fromhex(case["targetHex"]).decode("latin1")
                    status, response_headers, payload = raw(server.port, case["method"], target, headers)
                    payloads += payload
                    if name in ACCEPT_DIFFERENCES:
                        expected_type = ACCEPT_DIFFERENCES[name]
                        check(status == 200 and response_headers.get("content-type") == expected_type
                              and response_headers.get("content-type") != case["headers"]["content-type"],
                              f"{name}: documented Accept difference changed: {response_headers.get('content-type')}")
                        asserted += 1
                    elif name == "escaped-path":
                        check(case["status"] == 200 and status == 404, f"{name}: expected native 404, got {status}")
                        asserted += 1
                    elif name in ROUTER_404:
                        check(case["status"] == 404 and status == 404, f"{name}: status {status} != 404")
                        asserted += 1
                    else:
                        matched += compare(case, status, response_headers, payload)
                print(f"request edges: {matched} matched, {asserted} asserted differences")

                # ---- concurrency ----
                # Native HTTP admission (router-wide, main.cpp): 4 workers and
                # 8 pending connections; a further simultaneous connect is
                # closed without a response. So: 12 simultaneous reads must
                # all match; the oracle's 24 simultaneous reads may lose some
                # to a socket-level refusal, never to a wrong answer; then 200
                # reads from 12 concurrent clients.
                concurrent = fixture["concurrent"]
                expected = {kind: bytes.fromhex(hex_) for kind, hex_ in zip(concurrent["kinds"], concurrent["payloadHex"])}
                check(concurrent["statuses"] == [200] * len(concurrent["kinds"]), "oracle concurrent statuses")

                def read(kind):
                    try:
                        return get(server.port, "B", "application/protobuf" if kind == "protobuf" else None,
                                   "HEAD" if kind == "head" else "GET")
                    except (OSError, IndexError, ValueError) as error:
                        return (0, {}, repr(error).encode())

                def burst(kinds):
                    results = [None] * len(kinds)
                    barrier = threading.Barrier(len(kinds))

                    def one(index, kind):
                        barrier.wait()
                        results[index] = read(kind)

                    threads = [threading.Thread(target=one, args=(i, kind)) for i, kind in enumerate(kinds)]
                    for thread in threads:
                        thread.start()
                    for thread in threads:
                        thread.join()
                    return results

                admitted = concurrent["kinds"][:12]
                results = burst(admitted)
                bad = [(admitted[i], r[0]) for i, r in enumerate(results) if r[0] != 200 or r[2] != expected[admitted[i]]]
                check(not bad, f"12 simultaneous reads differ: {bad[:5]}")
                results = burst(concurrent["kinds"])
                answered = [(kind, r) for kind, r in zip(concurrent["kinds"], results) if r[0] != 0]
                wrong = [(kind, r[0]) for kind, r in answered if r[0] != 200 or r[2] != expected[kind]]
                check(not wrong and len(answered) >= 12, f"24 simultaneous: {len(answered)} answered, wrong {wrong[:5]}")
                more = [("json", "protobuf", "head")[i % 3] for i in range(200)]
                lock = threading.Lock()
                outcomes = []

                def client(offset):
                    for kind in more[offset::12]:
                        result = read(kind)
                        with lock:
                            outcomes.append((kind, result))

                threads = [threading.Thread(target=client, args=(i,)) for i in range(12)]
                for thread in threads:
                    thread.start()
                for thread in threads:
                    thread.join()
                bad_more = [(kind, r[0]) for kind, r in outcomes if r[0] != 200 or r[2] != expected[kind]]
                check(len(outcomes) == 200 and not bad_more, f"concurrent clients differ: {bad_more[:5]}")
                print(f"concurrent: 12/12 simultaneous identical; oracle burst of 24: {len(answered)} answered, all "
                      f"identical, {24 - len(answered)} refused at the socket; "
                      f"{len(outcomes) - len(bad_more)}/200 from 12 clients identical")
                check(not upstream.calls(), f"a provider read reached the upstream: {upstream.calls()[:2]}")

                # ---- quota (device D, wall-clock minute) ----
                wait_for_fresh_minute(25)
                for _ in range(1199):
                    status, _, _ = get(server.port, "D")
                    if status != 200:
                        check(False, f"quota drain answered {status}")
                        break
                quota_matched = 0
                for case in fixture["cases"]:
                    if case["group"] != "quota":
                        continue
                    headers = {"authorization": "Bearer " + token(case["device"]), **case["requestHeaders"]}
                    status, response_headers, payload = raw(server.port, case["method"], PATH, headers)
                    quota_matched += compare(case, status, response_headers, payload, remaining=True)
                print(f"quota: {quota_matched} matched")
            finally:
                no_secret(fixture, bytes(payloads), "responses")

            # ---- restarts (same KV directory) ----
            reference = fixture["restart"]["before"].encode("utf-8")
            status, _, before = get(server.port, "C")
            check(status == 200 and before == reference, "before restart: directory differs")
            output = server.stop()
            no_secret(fixture, output, "main output")
            server = start(binary, upstream, root, "main", fixture["mainEnv"], clock)
            status, headers, after = get(server.port, "A")
            check(status == 200 and after == reference, f"after SIGTERM restart: {status} {after[:80]!r}")
            status, headers, after = get(server.port, "D")
            # Quota counters are process memory in both: a drained device starts
            # a fresh minute after any restart.
            check(status == fixture["restart"]["afterStatus"] and after == reference
                  and headers.get("x-rate-limit-remaining") == fixture["restart"]["afterRemaining"],
                  f"device D after restart: {status} remaining {headers.get('x-rate-limit-remaining')}")
            print(f"restart: same bytes; device D answers {status} with {headers.get('x-rate-limit-remaining')} "
                  f"remaining (TypeScript {fixture['restart']['afterStatus']}/{fixture['restart']['afterRemaining']})")
            server.process.send_signal(signal.SIGKILL)
            server.process.wait()
            output = server.process.stdout.read() + server.process.stderr.read()
            no_secret(fixture, output, "killed output")
            server = start(binary, upstream, root, "main", fixture["mainEnv"], clock)
            status, _, after = get(server.port, "B", "application/protobuf")
            check(status == 200 and after == bytes.fromhex(cases["protobuf"]["wireHex"]),
                  f"after SIGKILL restart: {status} {after[:40].hex()}")
            no_secret(fixture, server.stop(), "restarted output")
            check(not upstream.calls(), "a provider read reached the upstream")
        finally:
            upstream.close()
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        raise SystemExit(1)
    print("cross-chain providers route: all checks passed")


if __name__ == "__main__":
    main()
