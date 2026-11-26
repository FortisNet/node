#!/usr/bin/env python3
"""Native-only checks of GET /v1/dashpay/names/resolve that the TypeScript
contract fixture cannot express (tests/dashpay_route_test.py replays that).

Production binary (build/fortis_node):
  - without FORTIS_NATIVE_DASHPAY_CANARY the route is absent (404); with it
    and a CA bundle it is registered (401 without a bearer);
  - the canary without FORTIS_CA_BUNDLE_PATH, or with a value other than
    0/1, refuses to start.
Test binary (build/fortis_node_dashpay_route_test) against the captured
mainnet upstream (tests/corpus/dashpay_upstream.json):
  - the default 15 s lookup bound: a stalled evonode answers 503 after ~15 s;
  - an old failure never clears a newer healthy connection (the TypeScript
    resolver's promise-identity rule, here a connection generation);
  - freshness: a reply signed more than 31 minutes from the local clock, and
    a reply more than one block below the highest seen, are 503;
  - bounds: a 70 KiB reply (64 KiB cap) and a non-zero grpc-status are 503;
  - evonode choice: ineligible list entries are never contacted, the first
    live node is used, a failed node is banned and the next one tried;
  - a quorum missing from the trusted lists is refetched once and verified;
  - admission: with every job slot stalled, further requests answer 503 at
    once, and every stalled one answers 503 when its lookup bound expires;
  - shutdown during a stalled lookup is prompt and still answers 503;
  - no DashPay name or identity reaches process output.
Usage: dashpay_native_test.py build/fortis_node build/fortis_node_dashpay_route_test
"""
import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time

import dashpay_route_test as replay

AUTH = {"authorization": "Bearer fintel_dev_" + "A" * 43}
PATH = "/v1/dashpay/names/resolve?name="
GRPC = "application/grpc-web+proto"
IDENTITIES = (b"Bt5vvxCx1bHJuKfP9gX2uMx6JZhuQBjgLYR8hXG2FjF", b"XX5U5QtMwqnpdvwZCLuT6iGZzFFHo6sfygiZE2wjmAP")


def check(condition, message, failures):
    if not condition:
        failures.append(message)


def probe(server):
    status, _, body = server.request("GET", "/probe/test-dashpay-generation", {})
    assert status == 200, (status, body)
    return json.loads(body)


def production_gates(binary, upstream, root, failures):
    clock = root / "unused-clock"
    identity = root / "unused-identity"
    for canary, expected in ((False, 404), (True, 401)):
        kv = root / f"prod-kv-{canary}"
        kv.mkdir()
        env = {"FORTIS_NATIVE_DASHPAY_CANARY": "1"} if canary else {}
        server = replay.Server(binary, upstream, kv, clock, identity, env, canary=False)
        try:
            status, _, body = server.request("GET", PATH + "dash", {})
            check(status == expected, f"production canary={canary}: {status} != {expected} {body[:80]!r}", failures)
        finally:
            server.stop()
    for value, message in (("1", "requires FORTIS_CA_BUNDLE_PATH"), ("2", "must be 0 or 1")):
        environment = {k: v for k, v in os.environ.items() if not k.startswith("FORTIS_")}
        environment.update({"FORTIS_NATIVE_DASHPAY_CANARY": value, "FORTIS_KV_DIR": str(root)})
        if value == "2":
            environment["FORTIS_CA_BUNDLE_PATH"] = str(upstream.ca)
        completed = subprocess.run([binary, str(replay.free_port())], env=environment, capture_output=True,
                                   timeout=30)
        output = (completed.stdout + completed.stderr).decode(errors="replace")
        check(completed.returncode != 0 and message in output,
              f"canary={value}: exit {completed.returncode}, output {output[-200:]!r}", failures)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    production, binary = sys.argv[1:3]
    fixture = replay.load("dashpay_upstream.json")
    failures = []
    outputs = []
    with tempfile.TemporaryDirectory(prefix="dashpay-native-") as root:
        root = Path(root)
        (root / "tls").mkdir()
        upstream = replay.Upstream(root / "tls", fixture)
        clock = root / "clock"
        identity = root / "identity"
        identity.write_text("")
        clock.write_text(str(fixture["clockMs"]))
        count = [0]

        def server(env=None):
            count[0] += 1
            kv = root / f"kv-{count[0]}"
            kv.mkdir()
            upstream.reset()
            clock.write_text(str(fixture["clockMs"]))
            return replay.Server(binary, upstream, kv, clock, identity, env)

        def finish(instance):
            outputs.append(instance.stop())

        try:
            production_gates(production, upstream, root, failures)

            # The default lookup bound (15 s): connect succeeds, the evonodes stall.
            instance = server()
            try:
                upstream.override["dash"] = "stall"
                started = time.monotonic()
                status, _, body = instance.request("GET", PATH + "dash", AUTH)
                elapsed = time.monotonic() - started
                check(status == 503 and json.loads(body) == {"error": "DASHPAY_UNAVAILABLE"},
                      f"stalled lookup: {status} {body!r}", failures)
                check(14.0 <= elapsed <= 20.0, f"stalled lookup answered after {elapsed:.1f}s, not ~15s", failures)
                print(f"default lookup bound: 503 after {elapsed:.1f}s")
            finally:
                finish(instance)

            # An old failure never clears a newer healthy connection.
            instance = server({"FORTIS_TEST_DASHPAY_LOOKUP_MS": "6000", "FORTIS_TEST_DASHPAY_REQUEST_MS": "6000"})
            try:
                upstream.override["dash"] = "stall"
                upstream.override["alice"] = (200, GRPC, replay.corrupt(upstream.replies["alice"]))
                slow = {}
                thread = threading.Thread(target=lambda: slow.update(r=instance.request("GET", PATH + "dash", AUTH)))
                thread.start()
                time.sleep(1.0)
                status, _, _ = instance.request("GET", PATH + "alice", AUTH)
                check(status == 503, f"generation: failing lookup {status}", failures)
                check(probe(instance)["generation"] == 0, "generation: the failure did not drop connection 1", failures)
                status, _, body = instance.request("GET", PATH + "zzzznotarealname1234", AUTH)
                check(status == 200 and json.loads(body)["identityId"] is None, f"generation: reconnect {status}",
                      failures)
                check(probe(instance)["generation"] == 2, f"generation: {probe(instance)} != 2", failures)
                thread.join(30)
                check(slow.get("r", (0,))[0] == 503, f"generation: stalled lookup {slow.get('r')}", failures)
                check(probe(instance)["generation"] == 2, "generation: the old failure dropped connection 2", failures)
                status, _, _ = instance.request("GET", PATH + "x-y", AUTH)
                with upstream.lock:
                    connects = upstream.masternodes
                check(status == 200 and connects == 2, f"generation: kept connection {status} connects={connects}",
                      failures)
                print(f"generation rule: connects={connects}, generation={probe(instance)['generation']}")
            finally:
                finish(instance)

            # Freshness: time.
            instance = server()
            try:
                clock.write_text(str(fixture["clockMs"] + 40 * 60 * 1000))
                status, _, _ = instance.request("GET", PATH + "dash", AUTH)
                last = probe(instance)["lastFailure"]
                check(status == 503 and last == "stale node metadata", f"time freshness: {status} {last!r}", failures)
                clock.write_text(str(fixture["clockMs"]))
                status, _, _ = instance.request("GET", PATH + "dash", AUTH)
                check(status == 200, f"time freshness recovery: {status}", failures)
            finally:
                finish(instance)

            # Freshness: height (newer block first, then an older reply).
            instance = server()
            try:
                upstream.override["dash"] = (200, GRPC, base64.b64decode(fixture["newer"]["dash"]))
                status, _, body = instance.request("GET", PATH + "dash", AUTH)
                check(status == 200 and json.loads(body)["identityId"] == IDENTITIES[0].decode(),
                      f"height: newer reply {status} {body!r}", failures)
                status, _, _ = instance.request("GET", PATH + "alice", AUTH)
                last = probe(instance)["lastFailure"]
                check(status == 503 and last == "stale node metadata", f"height freshness: {status} {last!r}",
                      failures)
                print("freshness: time and height windows enforced")
            finally:
                finish(instance)

            # Bounds: reply size and gRPC status.
            instance = server()
            try:
                body = upstream.replies["dash"]
                upstream.override["dash"] = (200, GRPC, body + b"\x80" + (70 * 1024).to_bytes(4, "big") + bytes(70 * 1024))
                status, _, _ = instance.request("GET", PATH + "dash", AUTH)
                check(status == 503, f"70 KiB reply: {status}", failures)
                frames, at, data = [], 0, upstream.replies["alice"]
                while at < len(data):
                    length = int.from_bytes(data[at + 1:at + 5], "big")
                    frames.append((data[at], data[at + 5:at + 5 + length]))
                    at += 5 + length
                status14 = b"".join(bytes([f]) + len(p).to_bytes(4, "big") + p for f, p in frames if not f & 0x80)
                trailer = b"grpc-status:14\r\ngrpc-message:unavailable\r\n"
                status14 += b"\x80" + len(trailer).to_bytes(4, "big") + trailer
                upstream.override["alice"] = (200, GRPC, status14)
                status, _, _ = instance.request("GET", PATH + "alice", AUTH)
                check(status == 503, f"grpc-status 14: {status}", failures)
                last = probe(instance)["lastFailure"]
                check(last == "proof: response_framing", f"grpc-status 14 diagnostic {last!r}", failures)
            finally:
                finish(instance)

            # Evonode choice and bans; quorum refetch.
            instance = server()
            try:
                upstream.override["dash"] = (200, GRPC, replay.corrupt(upstream.replies["dash"]))
                status, _, _ = instance.request("GET", PATH + "dash", AUTH)
                with upstream.lock:
                    hosts = [host for method, host, _, _ in upstream.observed if method == "POST"]
                expected = [address.rsplit(":", 1)[0] for address in fixture["eligible"]]
                check(status == 503 and hosts == expected, f"node order: {hosts} != {expected}", failures)
                print(f"evonode choice: {hosts}")
            finally:
                finish(instance)
            instance = server()
            try:
                # The quorum that signed the captured reply, removed from the
                # first fetch of both lists and present in the second.
                data = upstream.replies["dash"]
                message = data[5:5 + int.from_bytes(data[1:5], "big")]
                version = replay.fields(message)[2][0]
                signer = replay.fields(replay.fields(version)[2][0])[2][0].hex()
                stripped = {}
                for path in ("/quorums", "/previous"):
                    document = json.loads(fixture["trusted"][path])
                    rows = document["data"] if isinstance(document["data"], list) else document["data"]["quorums"]
                    rows[:] = [row for row in rows if row["quorum_hash"] != signer]
                    stripped[path] = [json.dumps(document), fixture["trusted"][path]]
                upstream.trusted_sequence.update(stripped)
                status, _, body = instance.request("GET", PATH + "dash", AUTH)
                with upstream.lock:
                    fetched = upstream.trusted_served["/quorums"]
                check(status == 200 and fetched == 2, f"quorum refetch: {status} /quorums fetched {fetched}x",
                      failures)
                print(f"quorum refetch: status {status}, /quorums fetched {fetched}x")
            finally:
                finish(instance)

            # Admission: eight job slots.
            instance = server({"FORTIS_TEST_DASHPAY_LOOKUP_MS": "4000", "FORTIS_TEST_DASHPAY_REQUEST_MS": "4000"})
            try:
                instance.request("GET", PATH + "x-y", AUTH)  # connect first
                upstream.override["dash"] = "stall"
                results = []
                lock = threading.Lock()

                def one():
                    started = time.monotonic()
                    try:
                        status, _, _ = instance.request("GET", PATH + "dash", AUTH)
                    except OSError as error:
                        status = str(error)
                    with lock:
                        results.append((status, time.monotonic() - started))

                threads = [threading.Thread(target=one) for _ in range(12)]
                for thread in threads:
                    thread.start()
                for thread in threads:
                    thread.join(60)
                immediate = [r for r in results if r[0] == 503 and r[1] < 2.0]
                bounded = [r for r in results if r[0] == 503 and r[1] >= 2.0]
                check(len(results) == 12 and all(r[0] == 503 for r in results), f"admission: {results}", failures)
                check(len(immediate) >= 4 and len(bounded) <= 8, f"admission split {len(immediate)}/{len(bounded)}",
                      failures)
                print(f"admission: {len(immediate)} immediate 503, {len(bounded)} after the lookup bound")
            finally:
                finish(instance)

            # Shutdown during a stalled lookup: the transport is cancelled, so
            # the process exits promptly and the request is still answered.
            instance = server()
            try:
                instance.request("GET", PATH + "x-y", AUTH)  # connect first
                upstream.override["dash"] = "stall"
                pending = {}

                def stalled():
                    try:
                        pending["r"] = instance.request("GET", PATH + "dash", AUTH)
                    except OSError as error:
                        pending["r"] = (str(error),)

                thread = threading.Thread(target=stalled)
                thread.start()
                time.sleep(1.0)
                started = time.monotonic()
            finally:
                finish(instance)
            elapsed = time.monotonic() - started
            thread.join(30)
            check(elapsed < 8.0, f"shutdown took {elapsed:.1f}s with a stalled lookup", failures)
            check(pending.get("r", ("none",))[0] == 503, f"stalled lookup at shutdown: {pending.get('r')}", failures)
            print(f"shutdown with a stalled lookup: {elapsed:.1f}s, answered {pending.get('r', ('none',))[0]}")
        finally:
            upstream.close()
    output = b"".join(outputs)
    for secret in IDENTITIES + (b"dash.dash", b"alice.dash"):
        check(secret not in output, f"process output contains {secret!r}", failures)
    for failure in failures:
        print("FAIL", failure)
    if failures:
        raise SystemExit(1)
    print("dashpay native checks: all passed")


if __name__ == "__main__":
    main()
